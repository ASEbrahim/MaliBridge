/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * syncmirror_check: fence and event status as the app must see it, with the client's sync mirror
 * (client/syncmirror.c) on or off (VKBRIDGE_SYNC_MIRROR=0): every answer is checked against what
 * the app did. A submit held by a timeline semaphore the host signals later keeps its fence not
 * ready for as long as the test wants. Then many polls of a done fence and of a set event, which
 * the mirror answers without the server (run_tests.sh counts the round trips). Timeline semaphore
 * values likewise: never ahead of the GPU, caught up once it signals, also past more signals in
 * flight than the mirror follows at once.
 * One line per check, "OK: ..." / "FAILED: ..." last.
 */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static PFN_vkGetInstanceProcAddr gipa;
static VkInstance inst;
static VkDevice dev;
static VkQueue queue;
static VkCommandPool cpool;
static int failures, checks;

#define I(name) static PFN_##name name
I(vkCreateInstance); I(vkEnumeratePhysicalDevices); I(vkGetPhysicalDeviceProperties); I(vkGetPhysicalDeviceQueueFamilyProperties);
I(vkCreateDevice); I(vkGetDeviceProcAddr); I(vkDestroyInstance); I(vkGetDeviceQueue); I(vkCreateCommandPool);
I(vkAllocateCommandBuffers); I(vkBeginCommandBuffer); I(vkEndCommandBuffer); I(vkQueueSubmit); I(vkQueueSubmit2); I(vkCreateFence);
I(vkDestroyFence); I(vkWaitForFences); I(vkGetFenceStatus); I(vkResetFences); I(vkCreateSemaphore); I(vkDestroySemaphore);
I(vkSignalSemaphore); I(vkQueueWaitIdle); I(vkDestroyDevice); I(vkCreateEvent); I(vkDestroyEvent); I(vkSetEvent); I(vkResetEvent);
I(vkGetEventStatus); I(vkCmdSetEvent); I(vkCmdResetEvent); I(vkCmdSetEvent2); I(vkCmdExecuteCommands); I(vkResetCommandBuffer); I(vkGetSemaphoreCounterValue); I(vkWaitSemaphores);
#undef I

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static void check(const char *what, VkResult got, VkResult want)
{
    checks++;
    if (got != want) failures++;
    printf("%s %s: %d (want %d)\n", got == want ? "PASS" : "FAIL", what, got, want);
    fflush(stdout);
}

static VkFence fence(int signaled)
{
    VkFenceCreateInfo ci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, NULL, signaled ? VK_FENCE_CREATE_SIGNALED_BIT : 0};
    VkFence f = VK_NULL_HANDLE;
    vkCreateFence(dev, &ci, NULL, &f);
    return f;
}

static VkSemaphore timeline(void)
{
    VkSemaphoreTypeCreateInfo ti = {VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, NULL, VK_SEMAPHORE_TYPE_TIMELINE, 0};
    VkSemaphoreCreateInfo ci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &ti};
    VkSemaphore s = VK_NULL_HANDLE;
    vkCreateSemaphore(dev, &ci, NULL, &s);
    return s;
}

/* A submit of cb (may be null) signalling f, held until the timeline gate reaches 1 (gate may be null). */
static VkResult submit(VkCommandBuffer cb, VkFence f, VkSemaphore gate)
{
    uint64_t one = 1;
    VkPipelineStageFlags st = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkTimelineSemaphoreSubmitInfo ts = {VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO, NULL, 1, &one, 0, NULL};
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, gate ? &ts : NULL, gate ? 1 : 0, gate ? &gate : NULL, gate ? &st : NULL,
                       cb ? 1 : 0, cb ? &cb : NULL, 0, NULL};
    return vkQueueSubmit(queue, 1, &si, f);
}

static void open_gate(VkSemaphore gate)
{
    VkSemaphoreSignalInfo si = {VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, NULL, gate, 1};
    vkSignalSemaphore(dev, &si);
}

/* Polls until want or 2 s. */
static VkResult poll_fence(VkFence f, VkResult want)
{
    double t = now_ms();
    VkResult r;
    do r = vkGetFenceStatus(dev, f);
    while (r != want && r >= 0 && now_ms() - t < 2000);
    return r;
}

static VkCommandBuffer cmdbuf(VkCommandBufferLevel level)
{
    VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, cpool, level, 1};
    VkCommandBuffer cb = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(dev, &ai, &cb);
    return cb;
}

static void begin(VkCommandBuffer cb, int secondary)
{
    VkCommandBufferInheritanceInfo inh = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO};
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, 0, secondary ? &inh : NULL};
    vkResetCommandBuffer(cb, 0);
    vkBeginCommandBuffer(cb, &bi);
}

/* Runs cb and waits for it. */
static void run(VkCommandBuffer cb)
{
    VkFence f = fence(0);
    submit(cb, f, VK_NULL_HANDLE);
    vkWaitForFences(dev, 1, &f, VK_TRUE, UINT64_MAX);
    vkDestroyFence(dev, f, NULL);
}

int main(int argc, char **argv)
{
    int polls = argc > 1 ? atoi(argv[1]) : 2000;
    const char *mode = argc > 2 ? argv[2] : NULL;
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib) {
        printf("FAILED: loader: %s\n", dlerror());
        return 2;
    }
    gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
#define G(n) n = (PFN_##n)gipa(inst, #n)
    G(vkCreateInstance);
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "vkbridge-syncmirror-check", 1, "vkbridge", 1, VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 0, NULL};
    if (vkCreateInstance(&ici, NULL, &inst)) {
        printf("FAILED: vkCreateInstance\n");
        return 1;
    }
    G(vkEnumeratePhysicalDevices); G(vkGetPhysicalDeviceProperties); G(vkGetPhysicalDeviceQueueFamilyProperties);
    G(vkCreateDevice); G(vkGetDeviceProcAddr); G(vkDestroyInstance);
#undef G
    uint32_t n = 1;
    VkPhysicalDevice pd;
    if (vkEnumeratePhysicalDevices(inst, &n, &pd) < 0 || !n) {
        printf("FAILED: no physical device\n");
        return 1;
    }
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(pd, &props);
    uint32_t nq = 0, qfam;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, NULL);
    VkQueueFamilyProperties qp[16];
    if (nq > 16) nq = 16;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qp);
    for (qfam = 0; qfam < nq; qfam++)
        if (qp[qfam].queueFlags & VK_QUEUE_GRAPHICS_BIT) break;
    if (qfam == nq || props.apiVersion < VK_API_VERSION_1_2) {
        printf("FAILED: %s: no graphics queue or not Vulkan 1.2\n", props.deviceName);
        return 1;
    }
    int v13 = props.apiVersion >= VK_API_VERSION_1_3;
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, qfam, 1, &prio};
    VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    f13.synchronization2 = VK_TRUE;
    VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, v13 ? &f13 : NULL};
    f12.timelineSemaphore = VK_TRUE;
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f12, 0, 1, &qci, 0, NULL, 0, NULL, NULL};
    if (vkCreateDevice(pd, &dci, NULL, &dev)) {
        printf("FAILED: vkCreateDevice\n");
        return 1;
    }
#define G(n) n = (PFN_##n)vkGetDeviceProcAddr(dev, #n)
    G(vkGetDeviceQueue); G(vkCreateCommandPool); G(vkAllocateCommandBuffers); G(vkBeginCommandBuffer); G(vkEndCommandBuffer);
    G(vkQueueSubmit); G(vkQueueSubmit2); G(vkCreateFence); G(vkDestroyFence); G(vkWaitForFences); G(vkGetFenceStatus); G(vkResetFences);
    G(vkCreateSemaphore); G(vkDestroySemaphore); G(vkSignalSemaphore); G(vkQueueWaitIdle); G(vkDestroyDevice); G(vkCreateEvent);
    G(vkDestroyEvent); G(vkSetEvent); G(vkResetEvent); G(vkGetEventStatus); G(vkCmdSetEvent); G(vkCmdResetEvent); G(vkCmdSetEvent2);
    G(vkCmdExecuteCommands); G(vkResetCommandBuffer); G(vkGetSemaphoreCounterValue); G(vkWaitSemaphores);
#undef G
    vkGetDeviceQueue(dev, qfam, 0, &queue);
    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, qfam};
    vkCreateCommandPool(dev, &pci, NULL, &cpool);
    const char *mv = getenv("VKBRIDGE_SYNC_MIRROR");
    printf("device: %s, sync mirror %s\n", props.deviceName, mv && !strcmp(mv, "0") ? "off" : "on unless the server says off");

    if (mode) {
        /* Every submit signals a fence and a timeline semaphore (DXVK's pattern).
         * "unmet": each is asked about while its submit is still held (a wait with a short time that runs out, as
         *   DXVK's queue thread waits right after submitting): the mirror never has the answer, so after a short
         *   try it must stop adding batches (only rare re-tries), where v0.5.5 mirrored every submit.
         * "met": each is asked about after the work is done (Balatro's semaphore, vkcube's fences): the mirror answers,
         *   so the submits stay mirrored and the answers come from shared memory.
         * run_tests.sh reads the client's "sync mirror: N submits, M of them with mirror batches" line. */
        int unmet = !strcmp(mode, "unmet");
        VkFence bf = fence(0);
        VkSemaphore bt = timeline(), bg = timeline();
        VkResult bad = VK_SUCCESS;
        for (int i = 1; i <= polls && bad == VK_SUCCESS; i++) {
            uint64_t v = (uint64_t)i;
            VkPipelineStageFlags st = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
            VkTimelineSemaphoreSubmitInfo ts = {VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO, NULL, unmet ? 1u : 0u, &v, 1, &v};
            VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, &ts, unmet ? 1u : 0u, &bg, &st, 0, NULL, 1, &bt};
            if (vkQueueSubmit(queue, 1, &si, bf) != VK_SUCCESS) bad = VK_ERROR_UNKNOWN;
            VkSemaphoreWaitInfo wi = {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, NULL, 0, 1, &bt, &v};
            if (unmet) {
                if (vkWaitSemaphores(dev, &wi, 100000ull) != VK_TIMEOUT) bad = VK_ERROR_UNKNOWN;
                if (vkWaitForFences(dev, 1, &bf, VK_TRUE, 100000ull) != VK_TIMEOUT) bad = VK_ERROR_UNKNOWN;
                VkSemaphoreSignalInfo sg = {VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, NULL, bg, v};
                vkSignalSemaphore(dev, &sg);
            }
            vkQueueWaitIdle(queue);
            if (!unmet) {
                if (vkWaitSemaphores(dev, &wi, 2000000000ull) != VK_SUCCESS) bad = VK_TIMEOUT;
                if (vkWaitForFences(dev, 1, &bf, VK_TRUE, 2000000000ull) != VK_SUCCESS) bad = VK_TIMEOUT;
            }
            vkResetFences(dev, 1, &bf);
        }
        check(unmet ? "submits asked about while held" : "submits waited for once done", bad, VK_SUCCESS);
        vkDestroyFence(dev, bf, NULL);
        vkDestroySemaphore(dev, bt, NULL);
        vkDestroySemaphore(dev, bg, NULL);
        vkDestroyDevice(dev, NULL);
        vkDestroyInstance(inst, NULL);
        if (failures) printf("FAILED: %d of %d checks\n", failures, checks);
        else printf("OK: %d checks\n", checks);
        return failures ? 1 : 0;
    }

    /* Fences */
    VkFence fs = fence(1);
    check("fence created signalled: status", vkGetFenceStatus(dev, fs), VK_SUCCESS);
    vkResetFences(dev, 1, &fs);
    check("... reset: status", vkGetFenceStatus(dev, fs), VK_NOT_READY);
    check("... reset: wait with no time", vkWaitForFences(dev, 1, &fs, VK_TRUE, 0), VK_TIMEOUT);

    VkSemaphore gate = timeline();
    VkFence f = fence(0);
    check("held submit", submit(VK_NULL_HANDLE, f, gate), VK_SUCCESS);
    check("held submit: status", vkGetFenceStatus(dev, f), VK_NOT_READY);
    check("held submit: wait with no time", vkWaitForFences(dev, 1, &f, VK_TRUE, 0), VK_TIMEOUT);
    check("held submit: 50 ms wait", vkWaitForFences(dev, 1, &f, VK_TRUE, 50000000ull), VK_TIMEOUT);
    VkFence pair[2] = {f, fs};
    vkResetFences(dev, 1, &fs);
    check("held + unsignalled: wait any with no time", vkWaitForFences(dev, 2, pair, VK_FALSE, 0), VK_TIMEOUT);
    open_gate(gate);
    check("gate opened: polled to done", poll_fence(f, VK_SUCCESS), VK_SUCCESS);
    check("... status again", vkGetFenceStatus(dev, f), VK_SUCCESS);
    check("... wait all of it", vkWaitForFences(dev, 1, &f, VK_TRUE, 0), VK_SUCCESS);
    check("done + unsignalled: wait any with no time", vkWaitForFences(dev, 2, pair, VK_FALSE, 0), VK_SUCCESS);
    check("done + unsignalled: wait all with no time", vkWaitForFences(dev, 2, pair, VK_TRUE, 0), VK_TIMEOUT);
    /* Reset at once and resubmit, many times: the server holds the reset until the driver's signal. */
    VkResult bad = VK_SUCCESS;
    for (int i = 0; i < 200 && bad == VK_SUCCESS; i++) {
        vkResetFences(dev, 1, &f);
        if (vkGetFenceStatus(dev, f) != VK_NOT_READY) bad = VK_ERROR_UNKNOWN;
        submit(VK_NULL_HANDLE, f, VK_NULL_HANDLE);
        VkResult r = poll_fence(f, VK_SUCCESS);
        if (r != VK_SUCCESS) bad = r;
    }
    check("reset at once and resubmitted 200 times", bad, VK_SUCCESS);
    vkDestroySemaphore(dev, gate, NULL);
    gate = timeline();
    vkResetFences(dev, 1, &f);
    if (v13) {
        uint64_t one = 1;
        VkSemaphoreSubmitInfo w = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, NULL, gate, one, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0};
        VkSubmitInfo2 s2 = {VK_STRUCTURE_TYPE_SUBMIT_INFO_2, NULL, 0, 1, &w, 0, NULL, 0, NULL};
        check("vkQueueSubmit2, held", vkQueueSubmit2(queue, 1, &s2, f), VK_SUCCESS);
        check("vkQueueSubmit2, held: status", vkGetFenceStatus(dev, f), VK_NOT_READY);
        open_gate(gate);
        check("vkQueueSubmit2, gate opened: polled to done", poll_fence(f, VK_SUCCESS), VK_SUCCESS);
    }
    double t = now_ms();
    bad = VK_SUCCESS;
    for (int i = 0; i < polls; i++) {
        VkResult r = vkGetFenceStatus(dev, f);
        if (r != VK_SUCCESS) bad = r;
    }
    printf("  %d polls of a done fence in %.1f ms\n", polls, now_ms() - t);
    check("polls of a done fence", bad, VK_SUCCESS);
    vkDestroyFence(dev, f, NULL);
    vkDestroyFence(dev, fs, NULL);
    vkDestroySemaphore(dev, gate, NULL);

    /* Timeline semaphores */
    {
        VkSemaphoreTypeCreateInfo ti = {VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, NULL, VK_SEMAPHORE_TYPE_TIMELINE, 5};
        VkSemaphoreCreateInfo ci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &ti};
        VkSemaphore tl = VK_NULL_HANDLE;
        vkCreateSemaphore(dev, &ci, NULL, &tl);
        uint64_t v = 0;
        vkGetSemaphoreCounterValue(dev, tl, &v);
        check("timeline created at 5: value 5", v == 5 ? VK_SUCCESS : VK_ERROR_UNKNOWN, VK_SUCCESS);
        VkSemaphoreSignalInfo hs = {VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, NULL, tl, 7};
        vkSignalSemaphore(dev, &hs);
        vkGetSemaphoreCounterValue(dev, tl, &v);
        check("host signal 7: value 7", v == 7 ? VK_SUCCESS : VK_ERROR_UNKNOWN, VK_SUCCESS);
        gate = timeline();
        uint64_t one = 1, ten = 10;
        VkPipelineStageFlags st = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkTimelineSemaphoreSubmitInfo tsi = {VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO, NULL, 1, &one, 1, &ten};
        VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, &tsi, 1, &gate, &st, 0, NULL, 1, &tl};
        check("held submit signalling 10", vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE), VK_SUCCESS);
        vkGetSemaphoreCounterValue(dev, tl, &v);
        check("held: value still 7", v == 7 ? VK_SUCCESS : VK_ERROR_UNKNOWN, VK_SUCCESS);
        VkSemaphoreWaitInfo wi = {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, NULL, 0, 1, &tl, &ten};
        check("held: wait for 10 with no time", vkWaitSemaphores(dev, &wi, 0), VK_TIMEOUT);
        open_gate(gate);
        double t0 = now_ms();
        do vkGetSemaphoreCounterValue(dev, tl, &v);
        while (v < 10 && now_ms() - t0 < 2000);
        check("gate opened: polled to 10", v == 10 ? VK_SUCCESS : VK_ERROR_UNKNOWN, VK_SUCCESS);
        check("wait for 10 with no time", vkWaitSemaphores(dev, &wi, 0), VK_SUCCESS);
        vkDestroySemaphore(dev, gate, NULL);
        /* More signals in flight than the mirror follows at once: the last is still reached. */
        uint64_t last = 10;
        for (int i = 0; i < 50; i++) {
            uint64_t nv = ++last;
            VkTimelineSemaphoreSubmitInfo t2 = {VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO, NULL, 0, NULL, 1, &nv};
            VkSubmitInfo s2 = {VK_STRUCTURE_TYPE_SUBMIT_INFO, &t2, 0, NULL, NULL, 0, NULL, 1, &tl};
            vkQueueSubmit(queue, 1, &s2, VK_NULL_HANDLE);
        }
        VkResult back = VK_SUCCESS;
        uint64_t prev = 0;
        t0 = now_ms();
        do {
            vkGetSemaphoreCounterValue(dev, tl, &v);
            if (v < prev) back = VK_ERROR_UNKNOWN;
            prev = v;
        } while (v < last && now_ms() - t0 < 2000);
        check("50 signals in a row: polled to the last, never backwards", v == last ? back : VK_ERROR_UNKNOWN, VK_SUCCESS);
        if (v13) {
            uint64_t nv = ++last;
            VkSemaphoreSubmitInfo sg = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, NULL, tl, nv, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0};
            VkSubmitInfo2 s2 = {VK_STRUCTURE_TYPE_SUBMIT_INFO_2, NULL, 0, 0, NULL, 0, NULL, 1, &sg};
            vkQueueSubmit2(queue, 1, &s2, VK_NULL_HANDLE);
            t0 = now_ms();
            do vkGetSemaphoreCounterValue(dev, tl, &v);
            while (v < nv && now_ms() - t0 < 2000);
            check("vkQueueSubmit2 signal: polled to it", v == nv ? VK_SUCCESS : VK_ERROR_UNKNOWN, VK_SUCCESS);
        }
        t0 = now_ms();
        VkResult badv = VK_SUCCESS;
        for (int i = 0; i < polls; i++) {
            vkGetSemaphoreCounterValue(dev, tl, &v);
            if (v != last) badv = VK_ERROR_UNKNOWN;
        }
        printf("  %d polls of a timeline value in %.1f ms\n", polls, now_ms() - t0);
        check("polls of a timeline value", badv, VK_SUCCESS);
        vkQueueWaitIdle(queue);
        vkDestroySemaphore(dev, tl, NULL);
    }

    /* Events */
    VkEventCreateInfo eci = {VK_STRUCTURE_TYPE_EVENT_CREATE_INFO};
    VkEvent ev = VK_NULL_HANDLE;
    vkCreateEvent(dev, &eci, NULL, &ev);
    check("event created: status", vkGetEventStatus(dev, ev), VK_EVENT_RESET);
    vkSetEvent(dev, ev);
    check("host set: status", vkGetEventStatus(dev, ev), VK_EVENT_SET);
    vkResetEvent(dev, ev);
    check("host reset: status", vkGetEventStatus(dev, ev), VK_EVENT_RESET);
    VkCommandBuffer cb = cmdbuf(VK_COMMAND_BUFFER_LEVEL_PRIMARY), sec = cmdbuf(VK_COMMAND_BUFFER_LEVEL_SECONDARY);
    begin(cb, 0);
    vkCmdSetEvent(cb, ev, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    vkEndCommandBuffer(cb);
    run(cb);
    check("vkCmdSetEvent, done: status", vkGetEventStatus(dev, ev), VK_EVENT_SET);
    begin(cb, 0);
    vkCmdResetEvent(cb, ev, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    vkEndCommandBuffer(cb);
    run(cb);
    check("vkCmdResetEvent, done: status", vkGetEventStatus(dev, ev), VK_EVENT_RESET);
    begin(cb, 0);
    vkCmdSetEvent(cb, ev, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    vkCmdResetEvent(cb, ev, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    vkEndCommandBuffer(cb);
    run(cb);
    check("set then reset in one command buffer: status", vkGetEventStatus(dev, ev), VK_EVENT_RESET);
    begin(sec, 1);
    vkCmdSetEvent(sec, ev, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    vkEndCommandBuffer(sec);
    begin(cb, 0);
    vkCmdExecuteCommands(cb, 1, &sec);
    vkEndCommandBuffer(cb);
    run(cb);
    check("set in a secondary: status", vkGetEventStatus(dev, ev), VK_EVENT_SET);
    if (v13) {
        vkResetEvent(dev, ev);
        VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        begin(cb, 0);
        vkCmdSetEvent2(cb, ev, &di);
        vkEndCommandBuffer(cb);
        run(cb);
        check("vkCmdSetEvent2, done: status", vkGetEventStatus(dev, ev), VK_EVENT_SET);
    }
    /* Set on the GPU while a held submit keeps it from running: reset until the gate opens. */
    vkResetEvent(dev, ev);
    gate = timeline();
    begin(cb, 0);
    vkCmdSetEvent(cb, ev, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    vkEndCommandBuffer(cb);
    f = fence(0);
    submit(cb, f, gate);
    check("vkCmdSetEvent, held: status", vkGetEventStatus(dev, ev), VK_EVENT_RESET);
    open_gate(gate);
    vkWaitForFences(dev, 1, &f, VK_TRUE, UINT64_MAX);
    check("vkCmdSetEvent, gate opened and done: status", vkGetEventStatus(dev, ev), VK_EVENT_SET);
    t = now_ms();
    bad = VK_EVENT_SET;
    for (int i = 0; i < polls; i++) {
        VkResult r = vkGetEventStatus(dev, ev);
        if (r != VK_EVENT_SET) bad = r;
    }
    printf("  %d polls of a set event in %.1f ms\n", polls, now_ms() - t);
    check("polls of a set event", bad, VK_EVENT_SET);
    vkQueueWaitIdle(queue);
    vkDestroyFence(dev, f, NULL);
    vkDestroySemaphore(dev, gate, NULL);
    vkDestroyEvent(dev, ev, NULL);

    /* A timeline semaphore first polled while a signal made before that poll is still held (that signal had no
     * mirror copy): its value is never ahead, reaches the held value once the gate opens, and the next signal (now
     * mirrored) is reached too. A fence first polled while held: the same for its status. */
    {
        VkSemaphore g2 = timeline(), t2 = timeline();
        VkFence f2 = fence(0);
        uint64_t three = 3, one = 1, v = 99;
        VkPipelineStageFlags st = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkTimelineSemaphoreSubmitInfo ts = {VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO, NULL, 1, &one, 1, &three};
        VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, &ts, 1, &g2, &st, 0, NULL, 1, &t2};
        check("held submit signalling 3 (semaphore not polled before)", vkQueueSubmit(queue, 1, &si, f2), VK_SUCCESS);
        vkGetSemaphoreCounterValue(dev, t2, &v);
        check("first poll while held: value 0", v == 0 ? VK_SUCCESS : VK_ERROR_UNKNOWN, VK_SUCCESS);
        check("first fence poll while held: not ready", vkGetFenceStatus(dev, f2), VK_NOT_READY);
        for (int i = 0; i < 50; i++) {
            vkGetSemaphoreCounterValue(dev, t2, &v);
            if (v != 0) break;
        }
        check("polled while held: still 0", v == 0 ? VK_SUCCESS : VK_ERROR_UNKNOWN, VK_SUCCESS);
        open_gate(g2);
        double t0 = now_ms();
        do vkGetSemaphoreCounterValue(dev, t2, &v);
        while (v < 3 && now_ms() - t0 < 2000);
        check("gate opened: polled to 3", v == 3 ? VK_SUCCESS : VK_ERROR_UNKNOWN, VK_SUCCESS);
        check("gate opened: fence polled to done", poll_fence(f2, VK_SUCCESS), VK_SUCCESS);
        VkResult back = VK_SUCCESS;
        for (uint64_t n = 4; n < 24; n++) {
            VkTimelineSemaphoreSubmitInfo ts2 = {VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO, NULL, 0, NULL, 1, &n};
            VkSubmitInfo s2 = {VK_STRUCTURE_TYPE_SUBMIT_INFO, &ts2, 0, NULL, NULL, 0, NULL, 1, &t2};
            vkResetFences(dev, 1, &f2);
            vkQueueSubmit(queue, 1, &s2, f2);
            uint64_t prev = v;
            t0 = now_ms();
            do {
                vkGetSemaphoreCounterValue(dev, t2, &v);
                if (v < prev || v > n) back = VK_ERROR_UNKNOWN;
                prev = v;
            } while (v < n && now_ms() - t0 < 2000);
            if (v != n || poll_fence(f2, VK_SUCCESS) != VK_SUCCESS) back = VK_TIMEOUT;
        }
        check("then 20 mirrored signals and fences: each reached, never ahead or backwards", back, VK_SUCCESS);
        vkQueueWaitIdle(queue);
        vkDestroyFence(dev, f2, NULL);
        vkDestroySemaphore(dev, t2, NULL);
        vkDestroySemaphore(dev, g2, NULL);
    }

    vkDestroyDevice(dev, NULL);
    vkDestroyInstance(inst, NULL);
    if (failures) printf("FAILED: %d of %d checks\n", failures, checks);
    else printf("OK: %d checks\n", checks);
    return failures ? 1 : 0;
}
