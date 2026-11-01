/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * poll_check: status polls (vkGetEventStatus, vkGetFenceStatus, vkGetSemaphoreCounterValue) while
 * the host's own sets and resets of those objects are still queued as async requests. The client
 * sends polls without waiting for the queued work (VKBRIDGE_POLL_NOBARRIER) except where the
 * polled object has such a request pending; each poll here must still see the host's last set or
 * reset, and timeline values never go back or ahead of what was submitted.
 * One line per check, "OK: ..." / "FAILED: ..." last.
 */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static PFN_vkGetInstanceProcAddr gipa;
static VkInstance inst;
static VkDevice dev;
static VkQueue queue;
static VkCommandPool cpool;
static int failures, checks;

#define I(name) static PFN_##name name
I(vkCreateInstance); I(vkEnumeratePhysicalDevices); I(vkGetPhysicalDeviceProperties); I(vkGetPhysicalDeviceQueueFamilyProperties);
I(vkCreateDevice); I(vkGetDeviceProcAddr); I(vkDestroyInstance); I(vkGetDeviceQueue); I(vkCreateCommandPool); I(vkDestroyCommandPool);
I(vkAllocateCommandBuffers); I(vkBeginCommandBuffer); I(vkEndCommandBuffer); I(vkQueueSubmit); I(vkCreateFence);
I(vkDestroyFence); I(vkWaitForFences); I(vkGetFenceStatus); I(vkCreateSemaphore); I(vkDestroySemaphore); I(vkWaitSemaphores);
I(vkQueueWaitIdle); I(vkDeviceWaitIdle); I(vkCreateQueryPool); I(vkDestroyQueryPool); I(vkCmdResetQueryPool);
I(vkCmdWriteTimestamp); I(vkGetQueryPoolResults); I(vkDestroyDevice); I(vkCreateEvent); I(vkDestroyEvent);
I(vkSetEvent); I(vkResetEvent); I(vkGetEventStatus); I(vkResetFences); I(vkGetSemaphoreCounterValue);
#undef I

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
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

/* An empty submit signalling f and/or the timeline s to 1 */
static VkResult submit(VkCommandBuffer cb, VkFence f, VkSemaphore s)
{
    uint64_t one = 1;
    VkTimelineSemaphoreSubmitInfo ts = {VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO, NULL, 0, NULL, 1, &one};
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, s ? &ts : NULL, 0, NULL, NULL, cb ? 1 : 0, cb ? &cb : NULL, s ? 1 : 0, s ? &s : NULL};
    return vkQueueSubmit(queue, 1, &si, f);
}


static void judge(const char *what, int ok, const char *fmt, ...)
{
    char extra[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(extra, sizeof(extra), fmt, ap);
    va_end(ap);
    checks++;
    if (!ok) failures++;
    printf("%s %s: %s\n", ok ? "PASS" : "FAIL", what, extra);
    fflush(stdout);
}

#define NEVENTS 6000 /* more than the client's 4096 per-object slots: some share one */

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib) {
        printf("FAILED: loader: %s\n", dlerror());
        return 2;
    }
    gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
#define G(n) n = (PFN_##n)gipa(inst, #n)
    G(vkCreateInstance);
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "vkbridge-poll-check", 1, "vkbridge", 1, VK_API_VERSION_1_2};
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
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, qfam, 1, &prio};
    VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    f12.timelineSemaphore = VK_TRUE;
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f12, 0, 1, &qci, 0, NULL, 0, NULL, NULL};
    if (vkCreateDevice(pd, &dci, NULL, &dev)) {
        printf("FAILED: vkCreateDevice\n");
        return 1;
    }
#define G(n) n = (PFN_##n)vkGetDeviceProcAddr(dev, #n)
    G(vkGetDeviceQueue); G(vkCreateCommandPool); G(vkDestroyCommandPool); G(vkAllocateCommandBuffers); G(vkBeginCommandBuffer); G(vkEndCommandBuffer);
    G(vkQueueSubmit); G(vkCreateFence); G(vkDestroyFence); G(vkWaitForFences); G(vkGetFenceStatus); G(vkCreateSemaphore);
    G(vkDestroySemaphore); G(vkWaitSemaphores); G(vkQueueWaitIdle); G(vkDeviceWaitIdle); G(vkCreateQueryPool);
    G(vkDestroyQueryPool); G(vkCmdResetQueryPool); G(vkCmdWriteTimestamp); G(vkGetQueryPoolResults); G(vkDestroyDevice);
    G(vkCreateEvent); G(vkDestroyEvent); G(vkSetEvent); G(vkResetEvent); G(vkGetEventStatus); G(vkResetFences);
    G(vkGetSemaphoreCounterValue);
#undef G
    vkGetDeviceQueue(dev, qfam, 0, &queue);
    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, 0, qfam};
    vkCreateCommandPool(dev, &pci, NULL, &cpool);
    printf("device: %s\n", props.deviceName);

    /* Host set and reset of one event, each followed at once by a poll: the poll must see it,
     * also right after a "reset" answer opened the client's quiet window. */
    {
        VkEventCreateInfo eci = {VK_STRUCTURE_TYPE_EVENT_CREATE_INFO};
        VkEvent e = VK_NULL_HANDLE;
        vkCreateEvent(dev, &eci, NULL, &e);
        int wrong = 0;
        double t = now_ms();
        for (int i = 0; i < 2000; i++) {
            if (i & 1) vkResetEvent(dev, e);
            else vkSetEvent(dev, e);
            VkResult want = i & 1 ? VK_EVENT_RESET : VK_EVENT_SET;
            if (vkGetEventStatus(dev, e) != want) wrong++;
        }
        judge("event set/reset then poll", !wrong, "%d of 2000 wrong, %.1f us a pair", wrong, (now_ms() - t) * 1e3 / 2000);
        vkDestroyEvent(dev, e, NULL);
    }
    /* Many events set, then each polled: the per-object record of the pending set shares slots. */
    {
        static VkEvent ev[NEVENTS];
        VkEventCreateInfo eci = {VK_STRUCTURE_TYPE_EVENT_CREATE_INFO};
        for (int i = 0; i < NEVENTS; i++) vkCreateEvent(dev, &eci, NULL, &ev[i]);
        int wrong = 0;
        for (int round = 0; round < 2; round++) {
            for (int i = 0; i < NEVENTS; i++) {
                if (round) vkResetEvent(dev, ev[i]);
                else vkSetEvent(dev, ev[i]);
            }
            for (int i = 0; i < NEVENTS; i++)
                if (vkGetEventStatus(dev, ev[i]) != (round ? VK_EVENT_RESET : VK_EVENT_SET)) wrong++;
        }
        judge("6000 events set, polled; reset, polled", !wrong, "%d wrong", wrong);
        for (int i = 0; i < NEVENTS; i++) vkDestroyEvent(dev, ev[i], NULL);
    }
    /* A signalled fence reset on the host: the next poll says not ready; submitted again, polls
     * reach success. */
    {
        VkFence f = fence();
        int wrong = 0, stuck = 0;
        long polls = 0;
        for (int i = 0; i < 200; i++) {
            submit(VK_NULL_HANDLE, f, VK_NULL_HANDLE);
            double t = now_ms();
            VkResult r;
            while ((r = vkGetFenceStatus(dev, f)) == VK_NOT_READY && now_ms() - t < 2000) polls++;
            if (r != VK_SUCCESS) stuck++;
            vkResetFences(dev, 1, &f);
            if (vkGetFenceStatus(dev, f) != VK_NOT_READY) wrong++;
        }
        judge("fence submit, polled, reset, polled", !wrong && !stuck, "%d reset fences seen signalled, %d never signalled, %ld not-ready polls", wrong, stuck,
              polls);
        vkQueueWaitIdle(queue);
        vkDestroyFence(dev, f, NULL);
    }
    /* A timeline semaphore signalled by submits: polled values never go back and never pass
     * what was submitted, and reach it. */
    {
        VkSemaphore s = timeline();
        uint64_t last = 0;
        int wrong = 0, stuck = 0;
        for (uint64_t k = 1; k <= 200; k++) {
            VkTimelineSemaphoreSubmitInfo ts = {VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO, NULL, 0, NULL, 1, &k};
            VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, &ts, 0, NULL, NULL, 0, NULL, 1, &s};
            vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
            double t = now_ms();
            uint64_t v = 0;
            do {
                if (vkGetSemaphoreCounterValue(dev, s, &v) != VK_SUCCESS) wrong++;
                if (v < last || v > k) wrong++;
                last = v;
            } while (v < k && now_ms() - t < 2000);
            if (v < k) stuck++;
        }
        judge("timeline submits, counter polled", !wrong && !stuck, "%d wrong values, %d never reached", wrong, stuck);
        vkQueueWaitIdle(queue);
        vkDestroySemaphore(dev, s, NULL);
    }
    vkDestroyCommandPool(dev, cpool, NULL);
    vkDestroyDevice(dev, NULL);
    vkDestroyInstance(inst, NULL);
    if (failures) printf("FAILED: %d of %d check(s)\n", failures, checks);
    else printf("OK: %d of %d checks passed\n", checks, checks);
    return failures ? 1 : 0;
}
