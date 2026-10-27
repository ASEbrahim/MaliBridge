/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * eventstatus_check: vkGetEventStatus as the app must see it. The client answers a poll of an event
 * it was just told is reset from its 1 ms quiet window (client/icd.c); that may only make an answer
 * late, never wrong. Each "set" check below comes right after a "reset" answer for the same event,
 * inside that window: after a host vkSetEvent, and after the app waited (fence wait, fence status,
 * vkQueueWaitIdle, vkDeviceWaitIdle, vkWaitSemaphores) for GPU work that set it. Then many polls of a
 * set event. The event checks are the ones of syncmirror_check (feat/queue-waits).
 * One line per check ("PASS" / "FAIL" with the time since the reset answer), "OK: ..." / "FAILED: ..." last.
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
static double reset_seen = -1e9; /* when vkGetEventStatus last answered reset */
static double since_reset;        /* at the last vkGetEventStatus: ms since the reset answer before it */

#define I(name) static PFN_##name name
I(vkCreateInstance); I(vkEnumeratePhysicalDevices); I(vkGetPhysicalDeviceProperties); I(vkGetPhysicalDeviceQueueFamilyProperties);
I(vkCreateDevice); I(vkGetDeviceProcAddr); I(vkDestroyInstance); I(vkGetDeviceQueue); I(vkCreateCommandPool);
I(vkAllocateCommandBuffers); I(vkBeginCommandBuffer); I(vkEndCommandBuffer); I(vkQueueSubmit); I(vkCreateFence);
I(vkDestroyFence); I(vkWaitForFences); I(vkGetFenceStatus); I(vkCreateSemaphore); I(vkDestroySemaphore);
I(vkSignalSemaphore); I(vkQueueWaitIdle); I(vkDeviceWaitIdle); I(vkWaitSemaphores); I(vkDestroyDevice); I(vkCreateEvent);
I(vkDestroyEvent); I(vkSetEvent); I(vkResetEvent); I(vkGetEventStatus); I(vkCmdSetEvent); I(vkCmdResetEvent); I(vkCmdSetEvent2);
I(vkCmdExecuteCommands); I(vkResetCommandBuffer);
#undef I

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static VkResult status(VkEvent ev)
{
    double t = now_ms();
    VkResult r = vkGetEventStatus(dev, ev);
    since_reset = t - reset_seen;
    if (r == VK_EVENT_RESET) reset_seen = now_ms();
    return r;
}

static void check(const char *what, VkResult got, VkResult want)
{
    checks++;
    if (got != want) failures++;
    printf("%s %s: %d (want %d)", got == want ? "PASS" : "FAIL", what, got, want);
    if (want == VK_EVENT_SET) printf(", asked %.3f ms after a reset answer", since_reset);
    printf("\n");
    fflush(stdout);
}

/* A reset answer for ev now, so the next poll falls in its quiet window. */
static void prime(VkEvent ev, const char *what)
{
    char name[160];
    snprintf(name, sizeof name, "%s: status before", what);
    check(name, status(ev), VK_EVENT_RESET);
}

static VkFence fence(void)
{
    VkFenceCreateInfo ci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
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

/* A submit of cb signalling f (may be null), held until the timeline gate reaches 1 (gate may be
 * null), signalling the timeline done to 1 (may be null). */
static VkResult submit(VkCommandBuffer cb, VkFence f, VkSemaphore gate, VkSemaphore done)
{
    uint64_t one = 1;
    VkPipelineStageFlags st = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkTimelineSemaphoreSubmitInfo ts = {VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO, NULL, gate ? 1 : 0, &one, done ? 1 : 0, &one};
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, gate || done ? &ts : NULL, gate ? 1 : 0, gate ? &gate : NULL, gate ? &st : NULL,
                       1, &cb, done ? 1 : 0, done ? &done : NULL};
    return vkQueueSubmit(queue, 1, &si, f);
}

static void open_gate(VkSemaphore gate)
{
    VkSemaphoreSignalInfo si = {VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, NULL, gate, 1};
    vkSignalSemaphore(dev, &si);
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

static void record_set(VkCommandBuffer cb, VkEvent ev)
{
    begin(cb, 0);
    vkCmdSetEvent(cb, ev, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    vkEndCommandBuffer(cb);
}

/* Runs cb and waits for it with a fence. */
static void run(VkCommandBuffer cb)
{
    VkFence f = fence();
    submit(cb, f, VK_NULL_HANDLE, VK_NULL_HANDLE);
    vkWaitForFences(dev, 1, &f, VK_TRUE, UINT64_MAX);
    vkDestroyFence(dev, f, NULL);
}

/* How the app learns that a held vkCmdSetEvent ran. */
enum { BY_FENCE_WAIT, BY_FENCE_POLL, BY_QUEUE_IDLE, BY_DEVICE_IDLE, BY_SEMAPHORE_WAIT };
static const char *by_name[] = {"vkWaitForFences", "vkGetFenceStatus polled to done", "vkQueueWaitIdle", "vkDeviceWaitIdle",
                                "vkWaitSemaphores"};

/* vkCmdSetEvent held behind a gate: reset while held; the gate opens with a reset answer just
 * given; once the app has seen the work done, set. */
static void held_set(VkCommandBuffer cb, VkEvent ev, int by)
{
    char name[160];
    vkResetEvent(dev, ev);
    VkSemaphore gate = timeline(), done = timeline();
    VkFence f = fence();
    record_set(cb, ev);
    submit(cb, f, gate, done);
    snprintf(name, sizeof name, "vkCmdSetEvent held, learnt by %s: status while held", by_name[by]);
    check(name, status(ev), VK_EVENT_RESET);
    open_gate(gate);
    status(ev); /* most likely still reset: starts a window just before the wait */
    switch (by) {
    case BY_FENCE_WAIT: vkWaitForFences(dev, 1, &f, VK_TRUE, UINT64_MAX); break;
    case BY_FENCE_POLL: {
        double t = now_ms();
        while (vkGetFenceStatus(dev, f) == VK_NOT_READY && now_ms() - t < 2000) {}
        break;
    }
    case BY_QUEUE_IDLE: vkQueueWaitIdle(queue); break;
    case BY_DEVICE_IDLE: vkDeviceWaitIdle(dev); break;
    case BY_SEMAPHORE_WAIT: {
        uint64_t one = 1;
        VkSemaphoreWaitInfo wi = {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, NULL, 0, 1, &done, &one};
        vkWaitSemaphores(dev, &wi, UINT64_MAX);
        break;
    }
    }
    snprintf(name, sizeof name, "vkCmdSetEvent held, learnt by %s: status once done", by_name[by]);
    check(name, status(ev), VK_EVENT_SET);
    vkQueueWaitIdle(queue);
    vkDestroyFence(dev, f, NULL);
    vkDestroySemaphore(dev, gate, NULL);
    vkDestroySemaphore(dev, done, NULL);
}

int main(int argc, char **argv)
{
    int polls = argc > 1 ? atoi(argv[1]) : 2000;
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib) {
        printf("FAILED: loader: %s\n", dlerror());
        return 2;
    }
    gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
#define G(n) n = (PFN_##n)gipa(inst, #n)
    G(vkCreateInstance);
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "vkbridge-eventstatus-check", 1, "vkbridge", 1, VK_API_VERSION_1_3};
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
    G(vkQueueSubmit); G(vkCreateFence); G(vkDestroyFence); G(vkWaitForFences); G(vkGetFenceStatus); G(vkCreateSemaphore);
    G(vkDestroySemaphore); G(vkSignalSemaphore); G(vkQueueWaitIdle); G(vkDeviceWaitIdle); G(vkWaitSemaphores); G(vkDestroyDevice);
    G(vkCreateEvent); G(vkDestroyEvent); G(vkSetEvent); G(vkResetEvent); G(vkGetEventStatus); G(vkCmdSetEvent); G(vkCmdResetEvent);
    G(vkCmdSetEvent2); G(vkCmdExecuteCommands); G(vkResetCommandBuffer);
#undef G
    vkGetDeviceQueue(dev, qfam, 0, &queue);
    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, qfam};
    vkCreateCommandPool(dev, &pci, NULL, &cpool);
    printf("device: %s\n", props.deviceName);

    VkEventCreateInfo eci = {VK_STRUCTURE_TYPE_EVENT_CREATE_INFO};
    VkEvent ev = VK_NULL_HANDLE;
    vkCreateEvent(dev, &eci, NULL, &ev);
    check("event created: status", status(ev), VK_EVENT_RESET);
    vkSetEvent(dev, ev);
    check("host set: status", status(ev), VK_EVENT_SET);
    vkResetEvent(dev, ev);
    check("host reset: status", status(ev), VK_EVENT_RESET);
    vkSetEvent(dev, ev);
    check("host set again: status", status(ev), VK_EVENT_SET);
    vkResetEvent(dev, ev);

    VkCommandBuffer cb = cmdbuf(VK_COMMAND_BUFFER_LEVEL_PRIMARY), sec = cmdbuf(VK_COMMAND_BUFFER_LEVEL_SECONDARY);
    record_set(cb, ev);
    prime(ev, "vkCmdSetEvent");
    run(cb);
    check("vkCmdSetEvent, done: status", status(ev), VK_EVENT_SET);
    begin(cb, 0);
    vkCmdResetEvent(cb, ev, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    vkEndCommandBuffer(cb);
    run(cb);
    check("vkCmdResetEvent, done: status", status(ev), VK_EVENT_RESET);
    begin(cb, 0);
    vkCmdSetEvent(cb, ev, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    vkCmdResetEvent(cb, ev, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    vkEndCommandBuffer(cb);
    run(cb);
    check("set then reset in one command buffer: status", status(ev), VK_EVENT_RESET);
    begin(sec, 1);
    vkCmdSetEvent(sec, ev, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    vkEndCommandBuffer(sec);
    begin(cb, 0);
    vkCmdExecuteCommands(cb, 1, &sec);
    vkEndCommandBuffer(cb);
    prime(ev, "set in a secondary");
    run(cb);
    check("set in a secondary: status", status(ev), VK_EVENT_SET);
    if (v13) {
        vkResetEvent(dev, ev);
        VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        begin(cb, 0);
        vkCmdSetEvent2(cb, ev, &di);
        vkEndCommandBuffer(cb);
        prime(ev, "vkCmdSetEvent2");
        run(cb);
        check("vkCmdSetEvent2, done: status", status(ev), VK_EVENT_SET);
    }
    for (int by = BY_FENCE_WAIT; by <= BY_SEMAPHORE_WAIT; by++) held_set(cb, ev, by);

    double t = now_ms();
    VkResult bad = VK_EVENT_SET;
    for (int i = 0; i < polls; i++) {
        VkResult r = vkGetEventStatus(dev, ev);
        if (r != VK_EVENT_SET) bad = r;
    }
    printf("  %d polls of a set event in %.1f ms\n", polls, now_ms() - t);
    check("polls of a set event", bad, VK_EVENT_SET);
    vkDestroyEvent(dev, ev, NULL);

    vkDestroyDevice(dev, NULL);
    vkDestroyInstance(inst, NULL);
    if (failures) printf("FAILED: %d of %d checks\n", failures, checks);
    else printf("OK: %d checks\n", checks);
    return failures ? 1 : 0;
}
