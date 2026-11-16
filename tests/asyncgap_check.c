/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * asyncgap_check: the server's order wait when a connection of the program has ended (a player log, Dying Light on a
 * Mali-G1-Ultra: "async request 823 never arrived; continuing", then a SIGSEGV at 0x32 in Mali's vkQueueSubmit2).
 * A thread makes one call and exits first, so its connection ends (any program does this; it armed the old gap rule).
 *
 *   asyncgap_check held|drop
 *     held: a command buffer is ended (asynchronous through the bridge; the server holds its replay with
 *           VKBRIDGE_TEST_DELAY_CMD=driver-end, as a player log's End waited for its BC transcoders), then another thread
 *           submits it with a fence and waits for the fence. Prints the fence's result and how long after the End
 *           it came: a submit run only after the End is done comes after the hold;
 *     drop: the same with the End never sent (the client's VKBRIDGE_TEST_DROP_ASYNC=vkEndCommandBuffer: a request
 *           whose number a thread took with it). Prints the fence wait's result.
 * The shell test judges (host/asyncgap_test.sh); "DONE: <mode> wait=<VkResult> ms=<ms>" last.
 */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static PFN_vkGetInstanceProcAddr gipa;
static VkInstance inst;
static VkDevice dev;
static VkQueue queue;
static VkCommandBuffer cb;
static VkFence fence;

#define I(name) static PFN_##name name
I(vkCreateInstance); I(vkEnumeratePhysicalDevices); I(vkGetPhysicalDeviceProperties); I(vkGetPhysicalDeviceQueueFamilyProperties);
I(vkCreateDevice); I(vkGetDeviceProcAddr); I(vkGetDeviceQueue); I(vkQueueSubmit); I(vkDeviceWaitIdle); I(vkDestroyDevice);
I(vkCreateCommandPool); I(vkAllocateCommandBuffers); I(vkBeginCommandBuffer); I(vkEndCommandBuffer); I(vkCmdPipelineBarrier);
I(vkCmdSetViewport); I(vkDestroyCommandPool); I(vkCreateFence); I(vkWaitForFences); I(vkDestroyFence);
#undef I

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static void sleep_ms(long ms)
{
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

/* one call on a connection of its own, then the thread ends and its connection with it */
static void *short_thread(void *arg)
{
    *(VkResult *)arg = vkDeviceWaitIdle(dev);
    return NULL;
}

static VkResult wait_r = VK_RESULT_MAX_ENUM;
static void *submit_thread(void *arg)
{
    (void)arg;
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cb, 0, NULL};
    VkResult r = vkQueueSubmit(queue, 1, &si, fence);
    wait_r = r == VK_SUCCESS ? vkWaitForFences(dev, 1, &fence, VK_TRUE, 20000000000ull) : r;
    return NULL;
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "held";
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib) {
        printf("FAILED: loader: %s\n", dlerror());
        return 2;
    }
    gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
#define G(n) n = (PFN_##n)gipa(inst, #n)
    G(vkCreateInstance);
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "vkbridge-asyncgap-check", 1, "vkbridge", 1, VK_API_VERSION_1_1};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 0, NULL};
    if (vkCreateInstance(&ici, NULL, &inst)) {
        printf("FAILED: vkCreateInstance\n");
        return 1;
    }
    G(vkEnumeratePhysicalDevices); G(vkGetPhysicalDeviceProperties); G(vkGetPhysicalDeviceQueueFamilyProperties);
    G(vkCreateDevice); G(vkGetDeviceProcAddr);
#undef G
    uint32_t n = 1, nq = 0, qfam;
    VkPhysicalDevice pd;
    if (vkEnumeratePhysicalDevices(inst, &n, &pd) < 0 || !n) {
        printf("FAILED: no physical device\n");
        return 1;
    }
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(pd, &props);
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, NULL);
    VkQueueFamilyProperties qp[16];
    if (nq > 16) nq = 16;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qp);
    for (qfam = 0; qfam < nq; qfam++)
        if (qp[qfam].queueFlags & VK_QUEUE_GRAPHICS_BIT) break;
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, qfam, 1, &prio};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &qci, 0, NULL, 0, NULL, NULL};
    if (qfam == nq || vkCreateDevice(pd, &dci, NULL, &dev)) {
        printf("FAILED: vkCreateDevice\n");
        return 1;
    }
#define G(n) n = (PFN_##n)vkGetDeviceProcAddr(dev, #n)
    G(vkGetDeviceQueue); G(vkQueueSubmit); G(vkDeviceWaitIdle); G(vkDestroyDevice); G(vkCreateCommandPool); G(vkAllocateCommandBuffers);
    G(vkBeginCommandBuffer); G(vkEndCommandBuffer); G(vkCmdPipelineBarrier); G(vkCmdSetViewport); G(vkDestroyCommandPool);
    G(vkCreateFence); G(vkWaitForFences); G(vkDestroyFence);
#undef G
    vkGetDeviceQueue(dev, qfam, 0, &queue);
    printf("device: %s (%s)\n", props.deviceName, mode);

    /* 1: a thread's connection ends */
    VkResult r = VK_RESULT_MAX_ENUM;
    pthread_t t;
    pthread_create(&t, NULL, short_thread, &r);
    pthread_join(t, NULL);
    sleep_ms(200); /* the server sees the connection go */
    printf("  a thread made one call (%d) and ended\n", r);

    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, 0, qfam};
    VkCommandPool pool = VK_NULL_HANDLE;
    r = vkCreateCommandPool(dev, &pci, NULL, &pool);
    VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    if (r == VK_SUCCESS) r = vkAllocateCommandBuffers(dev, &ai, &cb);
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (r == VK_SUCCESS) r = vkCreateFence(dev, &fci, NULL, &fence);
    if (r != VK_SUCCESS) {
        printf("FAILED: setup %d\n", r);
        return 1;
    }

    /* 2: the End (held by the server, or never sent) */
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT};
    VkViewport vp = {0, 0, 64, 64, 0, 1};
    vkBeginCommandBuffer(cb, &bi);
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    double t0 = now_ms();
    r = vkEndCommandBuffer(cb);
    printf("  vkEndCommandBuffer: %d\n", r);
    /* the client's flusher sends the End on its own connection (~100 us); let it go out before the submit's thread */
    sleep_ms(200);

    /* 3: the submit and its fence wait on another thread (another connection, as in a player log) */
    pthread_create(&t, NULL, submit_thread, NULL);
    pthread_join(t, NULL);
    double ms = now_ms() - t0;
    printf("  the submit's fence wait on another thread: %d, %.0f ms after the End\n", wait_r, ms);
    vkDeviceWaitIdle(dev);
    vkDestroyFence(dev, fence, NULL);
    vkDestroyCommandPool(dev, pool, NULL);
    vkDestroyDevice(dev, NULL);
    printf("DONE: %s wait=%d ms=%.0f\n", mode, wait_r, ms);
    return 0;
}
