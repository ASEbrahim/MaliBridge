/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * wait_check: every kind of wait the bridge forwards, each answered as the app should see it. For
 * the server's wait_retry erratum (server_wait.c) and its imitation of the Mali driver failing
 * waits with VK_ERROR_INITIALIZATION_FAILED (-3) after a signal; run against a server with the
 * imitation on, the fix on ("ok") or off ("bug").
 *
 *   wait_check ok|bug|any [case...]   cases: fence-finite fence-infinite sem-finite sem-infinite
 *                                     queue-idle device-idle query-wait (the default: all of them)
 *                                     fence-status, leak (exits without destroying the device,
 *                                     after a submit: the server's own vkDeviceWaitIdle)
 *
 * ok:  a finite wait on what is never signalled ends in VK_TIMEOUT, not before its timeout
 *      (300 ms); one on what is signalled, and the idle and query waits, in VK_SUCCESS; a polled
 *      fence status reaches VK_SUCCESS and is never -3;
 * bug: each ends in -3 (the imitation, the fix off);
 * any: the results are printed, not judged.
 * One line per case, "OK: ..." / "FAILED: ..." last. Set VKBRIDGE_QUERY_MIRROR=0 so the query wait
 * reaches the server.
 */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
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
static const char *expect = "ok";

#define I(name) static PFN_##name name
I(vkCreateInstance); I(vkEnumeratePhysicalDevices); I(vkGetPhysicalDeviceProperties); I(vkGetPhysicalDeviceQueueFamilyProperties);
I(vkCreateDevice); I(vkGetDeviceProcAddr); I(vkDestroyInstance); I(vkGetDeviceQueue); I(vkCreateCommandPool);
I(vkAllocateCommandBuffers); I(vkBeginCommandBuffer); I(vkEndCommandBuffer); I(vkQueueSubmit); I(vkCreateFence);
I(vkDestroyFence); I(vkWaitForFences); I(vkGetFenceStatus); I(vkCreateSemaphore); I(vkDestroySemaphore); I(vkWaitSemaphores);
I(vkQueueWaitIdle); I(vkDeviceWaitIdle); I(vkCreateQueryPool); I(vkDestroyQueryPool); I(vkCmdResetQueryPool);
I(vkCmdWriteTimestamp); I(vkGetQueryPoolResults); I(vkDestroyDevice);
#undef I

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

#define FINITE_NS 300000000ull

/* want: the result the fix gives; min_ms: not before (finite timeouts) */
static void judge(const char *what, VkResult r, double ms, VkResult want, double min_ms, const char *extra)
{
    int ok;
    if (!strcmp(expect, "bug"))
        ok = r == VK_ERROR_INITIALIZATION_FAILED;
    else if (!strcmp(expect, "ok"))
        ok = r == want && ms >= min_ms;
    else
        ok = 1;
    checks++;
    if (!ok) failures++;
    printf("%s %s: %d after %.0f ms%s%s\n", ok ? "PASS" : "FAIL", what, r, ms, extra ? ", " : "", extra ? extra : "");
    fflush(stdout);
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

static int wanted(int argc, char **argv, const char *name, int by_default)
{
    if (argc <= 2) return by_default;
    for (int i = 2; i < argc; i++)
        if (!strcmp(argv[i], name)) return 1;
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1) expect = argv[1];
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib) {
        printf("FAILED: loader: %s\n", dlerror());
        return 2;
    }
    gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
#define G(n) n = (PFN_##n)gipa(inst, #n)
    G(vkCreateInstance);
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "vkbridge-wait-check", 1, "vkbridge", 1, VK_API_VERSION_1_2};
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
    G(vkGetDeviceQueue); G(vkCreateCommandPool); G(vkAllocateCommandBuffers); G(vkBeginCommandBuffer); G(vkEndCommandBuffer);
    G(vkQueueSubmit); G(vkCreateFence); G(vkDestroyFence); G(vkWaitForFences); G(vkGetFenceStatus); G(vkCreateSemaphore);
    G(vkDestroySemaphore); G(vkWaitSemaphores); G(vkQueueWaitIdle); G(vkDeviceWaitIdle); G(vkCreateQueryPool);
    G(vkDestroyQueryPool); G(vkCmdResetQueryPool); G(vkCmdWriteTimestamp); G(vkGetQueryPoolResults); G(vkDestroyDevice);
#undef G
    vkGetDeviceQueue(dev, qfam, 0, &queue);
    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, 0, qfam};
    vkCreateCommandPool(dev, &pci, NULL, &cpool);
    printf("device: %s (expecting %s)\n", props.deviceName, expect);

    double t;
    VkResult r;
    if (wanted(argc, argv, "fence-finite", 1)) {
        VkFence f = fence();
        t = now_ms();
        r = vkWaitForFences(dev, 1, &f, VK_TRUE, FINITE_NS);
        judge("fence-finite (never signalled, 300 ms)", r, now_ms() - t, VK_TIMEOUT, 295, NULL);
        vkDestroyFence(dev, f, NULL);
    }
    if (wanted(argc, argv, "fence-infinite", 1)) {
        VkFence f = fence();
        submit(VK_NULL_HANDLE, f, VK_NULL_HANDLE);
        t = now_ms();
        r = vkWaitForFences(dev, 1, &f, VK_TRUE, UINT64_MAX);
        judge("fence-infinite (signalled, no timeout)", r, now_ms() - t, VK_SUCCESS, 0, NULL);
        vkQueueWaitIdle(queue);
        vkDestroyFence(dev, f, NULL);
    }
    if (wanted(argc, argv, "sem-finite", 1)) {
        VkSemaphore s = timeline();
        uint64_t v = 1;
        VkSemaphoreWaitInfo wi = {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, NULL, 0, 1, &s, &v};
        t = now_ms();
        r = vkWaitSemaphores(dev, &wi, FINITE_NS);
        judge("sem-finite (never signalled, 300 ms)", r, now_ms() - t, VK_TIMEOUT, 295, NULL);
        vkDestroySemaphore(dev, s, NULL);
    }
    if (wanted(argc, argv, "sem-infinite", 1)) {
        VkSemaphore s = timeline();
        submit(VK_NULL_HANDLE, VK_NULL_HANDLE, s);
        uint64_t v = 1;
        VkSemaphoreWaitInfo wi = {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, NULL, 0, 1, &s, &v};
        t = now_ms();
        r = vkWaitSemaphores(dev, &wi, UINT64_MAX);
        judge("sem-infinite (signalled, no timeout)", r, now_ms() - t, VK_SUCCESS, 0, NULL);
        vkQueueWaitIdle(queue);
        vkDestroySemaphore(dev, s, NULL);
    }
    if (wanted(argc, argv, "queue-idle", 1)) {
        submit(VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE);
        t = now_ms();
        r = vkQueueWaitIdle(queue);
        judge("queue-idle", r, now_ms() - t, VK_SUCCESS, 0, NULL);
    }
    if (wanted(argc, argv, "device-idle", 1)) {
        submit(VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE);
        t = now_ms();
        r = vkDeviceWaitIdle(dev);
        judge("device-idle", r, now_ms() - t, VK_SUCCESS, 0, NULL);
    }
    if (wanted(argc, argv, "query-wait", 1)) {
        VkQueryPoolCreateInfo qpci = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, NULL, 0, VK_QUERY_TYPE_TIMESTAMP, 1, 0};
        VkQueryPool pool = VK_NULL_HANDLE;
        vkCreateQueryPool(dev, &qpci, NULL, &pool);
        VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, cpool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
        VkCommandBuffer cb;
        vkAllocateCommandBuffers(dev, &ai, &cb);
        VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        vkBeginCommandBuffer(cb, &bi);
        vkCmdResetQueryPool(cb, pool, 0, 1);
        vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, pool, 0);
        vkEndCommandBuffer(cb);
        submit(cb, VK_NULL_HANDLE, VK_NULL_HANDLE);
        uint64_t ts[2] = {0, 0}; /* the value, its availability */
        t = now_ms();
        r = vkGetQueryPoolResults(dev, pool, 0, 1, sizeof(ts), ts, sizeof(ts),
                                  VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
        char extra[64];
        snprintf(extra, sizeof(extra), "availability %llu", (unsigned long long)ts[1]);
        if (r == VK_SUCCESS && ts[1] != 1 && !strcmp(expect, "ok")) r = VK_ERROR_UNKNOWN; /* written as available */
        judge("query-wait (timestamp, WAIT_BIT)", r, now_ms() - t, VK_SUCCESS, 0, extra);
        vkQueueWaitIdle(queue);
        vkDestroyQueryPool(dev, pool, NULL);
    }
    if (wanted(argc, argv, "fence-status", 0)) {
        VkFence f = fence();
        submit(VK_NULL_HANDLE, f, VK_NULL_HANDLE);
        t = now_ms();
        VkResult bad = VK_SUCCESS;
        int polls = 0;
        do {
            r = vkGetFenceStatus(dev, f);
            polls++;
            if (r != VK_SUCCESS && r != VK_NOT_READY && bad == VK_SUCCESS) bad = r;
            if (!strcmp(expect, "bug") && bad != VK_SUCCESS) break;
        } while (r != VK_SUCCESS && now_ms() - t < 2000);
        char extra[64];
        snprintf(extra, sizeof(extra), "%d poll(s)", polls);
        judge("fence-status (polled until signalled)", bad != VK_SUCCESS ? bad : r, now_ms() - t, VK_SUCCESS, 0, extra);
        vkQueueWaitIdle(queue);
        vkDestroyFence(dev, f, NULL);
    }
    if (wanted(argc, argv, "leak", 0)) {
        submit(VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE);
        printf("OK: exiting with the device alive (%d case(s) before)\n", checks);
        fflush(stdout);
        _exit(0);
    }
    vkDestroyDevice(dev, NULL);
    vkDestroyInstance(inst, NULL);
    if (failures) printf("FAILED: %d of %d case(s)\n", failures, checks);
    else printf("OK: %d case(s), expecting %s\n", checks, expect);
    return failures ? 1 : 0;
}
