/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * asynctimer_check: the client's async-send timer (client/conn.c, VKBRIDGE_ASYNC_TIMER): the time a thread spends queuing
 * asynchronous requests (no reply: vkResetFences here) was not measured, so a game's idle time could not be told from
 * time spent in the bridge's queue. A thread named "asyncer" sends vkResetFences as fast as it can for a few seconds
 * (the server stopped for the middle second by the script, so the queue's flush blocks); prints "DONE: calls=<n>".
 * host/asynctimer_test.sh judges the client log's "async sends" lines.
 */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/prctl.h>
#include <time.h>

static PFN_vkGetInstanceProcAddr gipa;
static VkDevice dev;
static VkFence fence;
static PFN_vkResetFences resetf;
static unsigned long calls;
static int run_ms = 3500;

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static void *asyncer(void *arg)
{
    (void)arg;
    prctl(PR_SET_NAME, "asyncer", 0, 0, 0);
    double end = now_ms() + run_ms;
    while (now_ms() < end)
        for (int i = 0; i < 64; i++, calls++) resetf(dev, 1, &fence);
    return NULL;
}

int main(int argc, char **argv)
{
    if (argc > 1) run_ms = atoi(argv[1]);
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib) return 2;
    gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "vkbridge-asynctimer-check", 1, "vkbridge", 1, VK_API_VERSION_1_1};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 0, NULL};
    VkInstance inst;
    if (((PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance"))(&ici, NULL, &inst)) return 1;
    uint32_t n = 1;
    VkPhysicalDevice pd;
    ((PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices"))(inst, &n, &pd);
    float prio = 1;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, 0, 1, &prio};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &qci, 0, NULL, 0, NULL, NULL};
    if (((PFN_vkCreateDevice)gipa(inst, "vkCreateDevice"))(pd, &dci, NULL, &dev)) return 1;
    PFN_vkGetDeviceProcAddr gdpa = (PFN_vkGetDeviceProcAddr)gipa(inst, "vkGetDeviceProcAddr");
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    ((PFN_vkCreateFence)gdpa(dev, "vkCreateFence"))(dev, &fci, NULL, &fence);
    resetf = (PFN_vkResetFences)gdpa(dev, "vkResetFences");
    pthread_t t;
    pthread_create(&t, NULL, asyncer, NULL);
    pthread_join(t, NULL);
    ((PFN_vkDeviceWaitIdle)gdpa(dev, "vkDeviceWaitIdle"))(dev);
    printf("DONE: calls=%lu\n", calls);
    return 0;
}
