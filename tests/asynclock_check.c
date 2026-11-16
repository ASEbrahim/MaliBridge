/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * asynclock_check: the client's async queue lock under a DXVK-like load (RE3, Tomb Raider, BL2: dxvk-cs waited 1.1-1.4 s of every
 * 10 s on this lock). Threads, for a few seconds:
 *   dxvk-cs      queues async requests nonstop (vkResetFences, and vkSetEvent / vkResetEvent of one event, ending set);
 *   dxvk-queue   a synchronous call with the order barrier every ~0.5 ms (vkQueueWaitIdle: it writes the queue out first);
 *   dxvk-submit  an empty vkQueueSubmit every ~2 ms (async).
 * The client's flusher sends what is queued ~100 us after it was queued. host/asynclock_test.sh stops the server for short
 * moments in some arms (a server thread behind: the socket fills, a write blocks) and judges the client log's "async sends"
 * lines (lock wait of dxvk-cs) and "async flusher" lines.
 * Checks here: the event polled after everything is set (the order kept), every call returned. Prints "DONE: calls=<n> ..."
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
static VkQueue queue;
static VkFence fence;
static VkEvent event;
static PFN_vkResetFences resetf;
static PFN_vkSetEvent setev;
static PFN_vkResetEvent resetev;
static PFN_vkGetEventStatus evstatus;
static PFN_vkQueueWaitIdle waitidle;
static PFN_vkQueueSubmit submitq;
static volatile int running = 1;
static unsigned long calls, syncs, submits;
static int run_ms = 4000;

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static void nap_us(long us)
{
    struct timespec ts = {0, us * 1000};
    nanosleep(&ts, NULL);
}

static void *cs(void *arg)
{
    (void)arg;
    prctl(PR_SET_NAME, "dxvk-cs", 0, 0, 0);
    double end = now_ms() + run_ms;
    while (now_ms() < end) {
        for (int i = 0; i < 62; i++, calls++) resetf(dev, 1, &fence);
        resetev(dev, event);
        setev(dev, event);
        calls += 2;
    }
    running = 0;
    return NULL;
}

static void *qthread(void *arg)
{
    (void)arg;
    prctl(PR_SET_NAME, "dxvk-queue", 0, 0, 0);
    while (running) {
        waitidle(queue);
        syncs++;
        nap_us(500);
    }
    return NULL;
}

static void *sthread(void *arg)
{
    (void)arg;
    prctl(PR_SET_NAME, "dxvk-submit", 0, 0, 0);
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    while (running) {
        submitq(queue, 1, &si, VK_NULL_HANDLE);
        submits++;
        nap_us(2000);
    }
    return NULL;
}

int main(int argc, char **argv)
{
    if (argc > 1) run_ms = atoi(argv[1]);
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib) return 2;
    gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "vkbridge-asynclock-check", 1, "vkbridge", 1, VK_API_VERSION_1_1};
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
    resetf = (PFN_vkResetFences)gdpa(dev, "vkResetFences");
    setev = (PFN_vkSetEvent)gdpa(dev, "vkSetEvent");
    resetev = (PFN_vkResetEvent)gdpa(dev, "vkResetEvent");
    evstatus = (PFN_vkGetEventStatus)gdpa(dev, "vkGetEventStatus");
    waitidle = (PFN_vkQueueWaitIdle)gdpa(dev, "vkQueueWaitIdle");
    submitq = (PFN_vkQueueSubmit)gdpa(dev, "vkQueueSubmit");
    ((PFN_vkGetDeviceQueue)gdpa(dev, "vkGetDeviceQueue"))(dev, 0, 0, &queue);
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (((PFN_vkCreateFence)gdpa(dev, "vkCreateFence"))(dev, &fci, NULL, &fence)) return 1;
    VkEventCreateInfo eci = {VK_STRUCTURE_TYPE_EVENT_CREATE_INFO};
    if (((PFN_vkCreateEvent)gdpa(dev, "vkCreateEvent"))(dev, &eci, NULL, &event)) return 1;
    pthread_t a, b, c;
    double t0 = now_ms();
    pthread_create(&a, NULL, cs, NULL);
    pthread_create(&b, NULL, qthread, NULL);
    pthread_create(&c, NULL, sthread, NULL);
    pthread_join(a, NULL);
    pthread_join(b, NULL);
    pthread_join(c, NULL);
    double secs = (now_ms() - t0) / 1e3;
    VkResult st = evstatus(dev, event);
    waitidle(queue);
    printf("%s the event set last by dxvk-cs reads set after the run (%d)\n", st == VK_EVENT_SET ? "PASS" : "FAIL", (int)st);
    printf("DONE: calls=%lu syncs=%lu submits=%lu secs=%.2f calls_per_s=%.0f\n", calls, syncs, submits, secs, calls / secs);
    return st == VK_EVENT_SET ? 0 : 1;
}
