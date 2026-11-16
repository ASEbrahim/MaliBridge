/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * querypoll_check: query reads that reach the server (a late or unmirrored query; run with VKBRIDGE_QUERY_MIRROR=0 so every
 * read does) while another thread keeps the process's async queue full, the shape of BL2's RenderingThread and Tekken's
 * RenderThread (DXVK's getData: vkGetQueryPoolResults, count 1, flags 0x1, no WAIT) beside dxvk-cs's stream of requests.
 * Before this, each such read first wrote the whole queue on its own socket and waited in the server behind every other
 * thread's queued work (0.8-1.5 ms a read on the phones). A non-WAIT read now goes like a fence poll, except while the host's
 * own vkResetQueryPool of a polled query has not run yet.
 * A thread named "dxvk-cs" queues vkResetFences nonstop; the main thread ("RenderThread"), per round:
 *   host reset of query q, then at once a non-WAIT read with availability: it must say unavailable (the reset is the app's,
 *   done when vkResetQueryPool returned: an "available, old value" answer is a stale result, the  class);
 *   a command buffer writing q's timestamp is submitted, then q is polled non-WAIT 64-bit until ready (each read timed);
 *   the ready value read again as 32-bit, with availability, with partial: the same value; every 8th round a WAIT read of q
 *   right after its submit (it must see the submitted work: VK_SUCCESS).
 * Then a pool reset on the GPU (vkCmdResetQueryPool): its reads keep the barrier (the client says so once in its log).
 * Prints "RESULT: polls=<n> median_us=<m> p90_us=<p> stale=<s> wait_bad=<w> width_bad=<b> gpu_bad=<g>" and PASS/FAIL lines
 * for the correctness checks; host/querypoll_test.sh compares the timings across its arms.
 */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <time.h>

static PFN_vkGetInstanceProcAddr gipa;
static VkInstance inst;
static VkDevice dev;
static VkQueue queue;

#define I(name) static PFN_##name name
I(vkCreateInstance); I(vkEnumeratePhysicalDevices); I(vkGetPhysicalDeviceProperties); I(vkGetPhysicalDeviceQueueFamilyProperties);
I(vkCreateDevice); I(vkGetDeviceProcAddr); I(vkGetDeviceQueue); I(vkCreateCommandPool); I(vkAllocateCommandBuffers);
I(vkBeginCommandBuffer); I(vkEndCommandBuffer); I(vkQueueSubmit); I(vkCreateFence); I(vkWaitForFences); I(vkResetFences);
I(vkQueueWaitIdle); I(vkCreateQueryPool); I(vkResetQueryPool); I(vkCmdResetQueryPool); I(vkCmdWriteTimestamp);
I(vkGetQueryPoolResults); I(vkResetCommandBuffer);
#undef I

static volatile int flooding = 1;
static unsigned long flood_calls;
static long flood_rate; /* requests a second, 0 = as fast as it can */
static volatile int hammering;
static VkQueryPool hammer_pool;
static volatile uint32_t hammer_q;
static unsigned long hammer_bad, hammer_reads;
static VkFence flood_fence;

static double now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
}

/* dxvk-cs: a steady stream of async requests (as DXVK's command thread: ~160k a second in RE3) */
static void *flooder(void *arg)
{
    (void)arg;
    prctl(PR_SET_NAME, "dxvk-cs", 0, 0, 0);
    double t0 = now_us();
    while (flooding) {
        for (int i = 0; i < 64; i++, flood_calls++) vkResetFences(dev, 1, &flood_fence);
        if (flood_rate > 0) {
            double due = t0 + flood_calls * 1e6 / flood_rate, now = now_us();
            if (due > now + 50) {
                struct timespec ts = {0, (long)((due - now) * 1000)};
                nanosleep(&ts, NULL);
            }
        }
    }
    return NULL;
}

/* hammer (argv[3] = 1): a second thread reads the current query with availability nonstop; "available" with the value 0 is
 * counted (a timestamp is never 0). Run natively (no bridge) it tells the driver's own answers from the bridge's. */
static void *hammer(void *arg)
{
    (void)arg;
    while (hammering) {
        uint64_t v[2] = {0, 0};
        VkResult r = vkGetQueryPoolResults(dev, hammer_pool, hammer_q, 1, 16, v, 16, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
        hammer_reads++;
        if ((r == VK_SUCCESS || v[1]) && v[0] == 0) hammer_bad++;
    }
    return NULL;
}

static int cmp_d(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

#define ROUNDS 600
#define MAXPOLLS 400000

int main(int argc, char **argv)
{
    int rounds = argc > 1 ? atoi(argv[1]) : ROUNDS;
    flood_rate = argc > 2 ? atol(argv[2]) : 0;
    int do_hammer = argc > 3 && atoi(argv[3]) == 1;
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib) {
        printf("FAILED: loader: %s\n", dlerror());
        return 2;
    }
    gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
#define G(n) n = (PFN_##n)gipa(inst, #n)
    G(vkCreateInstance);
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "vkbridge-querypoll-check", 1, "vkbridge", 1, VK_API_VERSION_1_2};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 0, NULL};
    if (vkCreateInstance(&ici, NULL, &inst)) {
        printf("FAILED: vkCreateInstance\n");
        return 1;
    }
    G(vkEnumeratePhysicalDevices); G(vkGetPhysicalDeviceProperties); G(vkGetPhysicalDeviceQueueFamilyProperties);
    G(vkCreateDevice); G(vkGetDeviceProcAddr);
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
        if ((qp[qfam].queueFlags & VK_QUEUE_GRAPHICS_BIT) && qp[qfam].timestampValidBits) break;
    if (qfam == nq || props.apiVersion < VK_API_VERSION_1_2) {
        printf("FAILED: %s: no graphics queue with timestamps or not Vulkan 1.2\n", props.deviceName);
        return 1;
    }
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, qfam, 1, &prio};
    VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    f12.hostQueryReset = VK_TRUE;
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f12, 0, 1, &qci, 0, NULL, 0, NULL, NULL};
    if (vkCreateDevice(pd, &dci, NULL, &dev)) {
        printf("FAILED: vkCreateDevice\n");
        return 1;
    }
#define G(n) n = (PFN_##n)vkGetDeviceProcAddr(dev, #n)
    G(vkGetDeviceQueue); G(vkCreateCommandPool); G(vkAllocateCommandBuffers); G(vkBeginCommandBuffer); G(vkEndCommandBuffer);
    G(vkQueueSubmit); G(vkCreateFence); G(vkWaitForFences); G(vkResetFences); G(vkQueueWaitIdle); G(vkCreateQueryPool);
    G(vkResetQueryPool); G(vkCmdResetQueryPool); G(vkCmdWriteTimestamp); G(vkGetQueryPoolResults); G(vkResetCommandBuffer);
#undef G
    vkGetDeviceQueue(dev, qfam, 0, &queue);
    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, qfam};
    VkCommandPool cpool;
    vkCreateCommandPool(dev, &pci, NULL, &cpool);
    VkCommandBuffer cb[2];
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, cpool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 2};
    vkAllocateCommandBuffers(dev, &cai, cb);
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, NULL, VK_FENCE_CREATE_SIGNALED_BIT};
    VkFence cf[2];
    vkCreateFence(dev, &fci, NULL, &cf[0]);
    vkCreateFence(dev, &fci, NULL, &cf[1]);
    VkFenceCreateInfo ffi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    vkCreateFence(dev, &ffi, NULL, &flood_fence);
    VkQueryPoolCreateInfo qpi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, NULL, 0, VK_QUERY_TYPE_TIMESTAMP, 64, 0};
    VkQueryPool pool, gpool;
    vkCreateQueryPool(dev, &qpi, NULL, &pool);
    vkCreateQueryPool(dev, &qpi, NULL, &gpool);
    vkResetQueryPool(dev, pool, 0, 64);
    vkResetQueryPool(dev, gpool, 0, 64);
    printf("device: %s\n", props.deviceName);
    fflush(stdout);
    prctl(PR_SET_NAME, "RenderThread", 0, 0, 0);

    pthread_t ft, ht;
    pthread_create(&ft, NULL, flooder, NULL);
    hammer_pool = pool;
    if (do_hammer) {
        hammering = 1;
        pthread_create(&ht, NULL, hammer, NULL);
    }
    double t_start = now_us();
    while (now_us() - t_start < 200000) {} /* the queue is busy before the first read */

    static double polls_us[MAXPOLLS];
    long npolls = 0;
    int stale = 0, wait_bad = 0, width_bad = 0, never = 0;
    for (int r = 0; r < rounds; r++) {
        uint32_t q = (uint32_t)(r % 64), k = (uint32_t)(r & 1);
        hammer_q = q;
        vkWaitForFences(dev, 1, &cf[k], VK_TRUE, UINT64_MAX); /* the command buffer is free again */
        vkResetFences(dev, 1, &cf[k]);
        vkResetQueryPool(dev, pool, q, 1);
        if (r >= 64) {
            /* used before (ready then): right after the host reset it is unavailable */
            uint64_t v[2] = {0, 0};
            VkResult a = vkGetQueryPoolResults(dev, pool, q, 1, sizeof(v), v, sizeof(v), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
            if (a == VK_SUCCESS || v[1]) stale++;
        }
        vkResetCommandBuffer(cb[k], 0);
        VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        vkBeginCommandBuffer(cb[k], &bi);
        vkCmdWriteTimestamp(cb[k], VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, pool, q);
        vkEndCommandBuffer(cb[k]);
        VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cb[k], 0, NULL};
        vkQueueSubmit(queue, 1, &si, cf[k]);
        uint64_t val = 0;
        if (r % 8 == 7) {
            VkResult w = vkGetQueryPoolResults(dev, pool, q, 1, 8, &val, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
            if (w != VK_SUCCESS || !val) wait_bad++;
        } else {
            double t0 = now_us();
            VkResult p;
            /* with availability: every read reaches the server (a plain not-ready poll opens icd.c's 1 ms quiet window, whose
             * local answers would fill the timings); the plain 64-bit read follows once it is ready */
            uint64_t av[2];
            do {
                av[0] = av[1] = 0;
                double a = now_us();
                p = vkGetQueryPoolResults(dev, pool, q, 1, 16, av, 16, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
                double b = now_us();
                if (npolls < MAXPOLLS) polls_us[npolls++] = b - a;
            } while (p == VK_NOT_READY && now_us() - t0 < 2e6);
            if (p == VK_SUCCESS) p = vkGetQueryPoolResults(dev, pool, q, 1, 8, &val, 8, VK_QUERY_RESULT_64_BIT);
            if (p == VK_SUCCESS && (av[1] != 1 || av[0] != val)) {
                if (width_bad < 8) printf("  round %d q %u: ready with availability %llu/%llu, plain read %llu\n", r, q, (unsigned long long)av[0],
                                          (unsigned long long)av[1], (unsigned long long)val);
                width_bad++;
            }
            if (p != VK_SUCCESS) {
                if (never < 8) printf("  round %d q %u: not ready after %.0f ms (last %d, availability %llu/%llu)\n", r, q, (now_us() - t0) / 1e3, (int)p,
                                      (unsigned long long)av[0], (unsigned long long)av[1]);
                never++;
                continue;
            }
        }
        /* the same result in the other widths and with availability / partial */
        uint32_t v32 = 0;
        uint64_t va[2] = {0, 0};
        uint32_t vp[2] = {0, 0};
        VkResult r32 = vkGetQueryPoolResults(dev, pool, q, 1, 4, &v32, 4, 0);
        VkResult ra = vkGetQueryPoolResults(dev, pool, q, 1, 16, va, 16, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
        VkResult rp = vkGetQueryPoolResults(dev, pool, q, 1, 8, vp, 8, VK_QUERY_RESULT_PARTIAL_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
        if (r32 != VK_SUCCESS || v32 != (uint32_t)val || ra != VK_SUCCESS || va[0] != val || va[1] != 1 || rp != VK_SUCCESS ||
            vp[0] != (uint32_t)val || vp[1] != 1) {
            if (width_bad < 8)
                printf("  round %d q %u: value %llu; 32-bit %d %u; avail %d %llu/%llu; partial %d %u/%u\n", r, q, (unsigned long long)val, (int)r32, v32,
                       (int)ra, (unsigned long long)va[0], (unsigned long long)va[1], (int)rp, vp[0], vp[1]);
            width_bad++;
        }
    }
    /* A pool reset on the GPU: reads after the submit, never the last use's value as "ready" */
    int gpu_bad = 0;
    uint64_t last = 0;
    for (int r = 0; r < 64; r++) {
        uint32_t k = (uint32_t)(r & 1);
        vkWaitForFences(dev, 1, &cf[k], VK_TRUE, UINT64_MAX);
        vkResetFences(dev, 1, &cf[k]);
        vkResetCommandBuffer(cb[k], 0);
        VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        vkBeginCommandBuffer(cb[k], &bi);
        vkCmdResetQueryPool(cb[k], gpool, 0, 1);
        vkCmdWriteTimestamp(cb[k], VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, gpool, 0);
        vkEndCommandBuffer(cb[k]);
        VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cb[k], 0, NULL};
        vkQueueSubmit(queue, 1, &si, cf[k]);
        vkWaitForFences(dev, 1, &cf[k], VK_TRUE, UINT64_MAX);
        uint64_t v = 0;
        VkResult p = vkGetQueryPoolResults(dev, gpool, 0, 1, 8, &v, 8, VK_QUERY_RESULT_64_BIT);
        if (p != VK_SUCCESS || (r && v == last)) gpu_bad++;
        last = v;
    }
    flooding = 0;
    pthread_join(ft, NULL);
    if (do_hammer) {
        hammering = 0;
        pthread_join(ht, NULL);
        printf("%s hammer: %lu concurrent reads, %lu said available with the value 0\n", hammer_bad ? "FAIL" : "PASS", hammer_reads, hammer_bad);
    }
    vkQueueWaitIdle(queue);
    double secs = (now_us() - t_start) / 1e6;
    qsort(polls_us, (size_t)npolls, sizeof(double), cmp_d);
    double med = npolls ? polls_us[npolls / 2] : 0, p90 = npolls ? polls_us[npolls * 9 / 10] : 0;
    printf("flood: %lu async requests in %.1f s (%.0f a second; asked %ld, 0 = unpaced)\n", flood_calls, secs, flood_calls / secs, flood_rate);
    printf("%s host reset then read: %d of %d reads said available (stale)\n", stale ? "FAIL" : "PASS", stale, rounds > 64 ? rounds - 64 : 0);
    printf("%s WAIT read right after the submit: %d bad\n", wait_bad ? "FAIL" : "PASS", wait_bad);
    printf("%s 32-bit / availability / partial reads of a ready query: %d differ\n", width_bad ? "FAIL" : "PASS", width_bad);
    printf("%s every query became ready: %d never did\n", never ? "FAIL" : "PASS", never);
    printf("%s GPU-reset pool: %d of 64 reads not ready after the fence or the last use's value\n", gpu_bad ? "FAIL" : "PASS", gpu_bad);
    printf("RESULT: polls=%ld median_us=%.1f p90_us=%.1f stale=%d wait_bad=%d width_bad=%d gpu_bad=%d never=%d\n", npolls, med, p90, stale,
           wait_bad, width_bad, gpu_bad, never);
    return 0;
}
