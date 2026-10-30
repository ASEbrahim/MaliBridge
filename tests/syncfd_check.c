/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * syncfd_check direct|bounded|stall|any: a sync-fd export of a semaphore whose batch is held, with and without the server's deadline.
 */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define HOLD_MS 3000

static PFN_vkGetInstanceProcAddr gipa;
static VkInstance inst;
static VkDevice dev;
static VkQueue queue;
static int failures, checks;

#define I(name) static PFN_##name name
I(vkCreateInstance); I(vkEnumeratePhysicalDevices); I(vkGetPhysicalDeviceProperties); I(vkGetPhysicalDeviceQueueFamilyProperties);
I(vkGetPhysicalDeviceExternalSemaphoreProperties); I(vkCreateDevice); I(vkGetDeviceProcAddr); I(vkGetDeviceQueue);
I(vkQueueSubmit); I(vkCreateSemaphore); I(vkDestroySemaphore); I(vkQueueWaitIdle); I(vkDeviceWaitIdle); I(vkDestroyDevice);
I(vkSignalSemaphore); I(vkGetSemaphoreFdKHR);
#undef I

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static void check(int ok, const char *what)
{
    checks++;
    if (!ok) failures++;
    printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
}

static void *late_signal(void *arg)
{
    struct timespec ts = {HOLD_MS / 1000, (HOLD_MS % 1000) * 1000000L};
    nanosleep(&ts, NULL);
    VkSemaphoreSignalInfo si = {VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, NULL, *(VkSemaphore *)arg, 1};
    vkSignalSemaphore(dev, &si);
    return NULL;
}

static void *idle_thread(void *arg)
{
    *(VkResult *)arg = vkDeviceWaitIdle(dev);
    return NULL;
}

static VkSemaphore exportable(void)
{
    VkExportSemaphoreCreateInfo ex = {VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO, NULL, VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT};
    VkSemaphoreCreateInfo ci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &ex, 0};
    VkSemaphore s = VK_NULL_HANDLE;
    vkCreateSemaphore(dev, &ci, NULL, &s);
    return s;
}

/* A batch that signals `b`, waiting on `timeline` = 1 when it is given. */
static VkResult submit_signal(VkSemaphore b, VkSemaphore timeline)
{
    uint64_t one = 1, zero = 0;
    VkTimelineSemaphoreSubmitInfo ts = {VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO, NULL, timeline ? 1 : 0, &one, 1, &zero};
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, &ts, timeline ? 1 : 0, &timeline, &stage, 0, NULL, 1, &b};
    return vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
}

static int export_fd(VkSemaphore s, VkResult *r)
{
    VkSemaphoreGetFdInfoKHR gi = {VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR, NULL, s, VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT};
    int fd = -2;
    *r = vkGetSemaphoreFdKHR(dev, &gi, &fd);
    return fd;
}

int main(int argc, char **argv)
{
    const char *expect = argc > 1 ? argv[1] : "any";
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib) {
        printf("FAILED: loader: %s\n", dlerror());
        return 2;
    }
    gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
#define G(n) n = (PFN_##n)gipa(inst, #n)
    G(vkCreateInstance);
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "vkbridge-syncfd-check", 1, "vkbridge", 1, VK_API_VERSION_1_2};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 0, NULL};
    if (vkCreateInstance(&ici, NULL, &inst)) {
        printf("FAILED: vkCreateInstance\n");
        return 1;
    }
    G(vkEnumeratePhysicalDevices); G(vkGetPhysicalDeviceProperties); G(vkGetPhysicalDeviceQueueFamilyProperties);
    G(vkGetPhysicalDeviceExternalSemaphoreProperties); G(vkCreateDevice); G(vkGetDeviceProcAddr);
#undef G
    uint32_t n = 1;
    VkPhysicalDevice pd;
    if (vkEnumeratePhysicalDevices(inst, &n, &pd) < 0 || !n) {
        printf("FAILED: no physical device\n");
        return 1;
    }
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(pd, &props);
    VkPhysicalDeviceExternalSemaphoreInfo ei = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO, NULL,
                                                VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT};
    VkExternalSemaphoreProperties ep = {VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES};
    vkGetPhysicalDeviceExternalSemaphoreProperties(pd, &ei, &ep);
    if (!(ep.externalSemaphoreFeatures & VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT)) {
        printf("SKIP: %s exports no sync fds\n", props.deviceName);
        return 0;
    }
    uint32_t nq = 0, qfam;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, NULL);
    VkQueueFamilyProperties qp[16];
    if (nq > 16) nq = 16;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qp);
    for (qfam = 0; qfam < nq; qfam++)
        if (qp[qfam].queueFlags & VK_QUEUE_GRAPHICS_BIT) break;
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, qfam, 1, &prio};
    VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    f12.timelineSemaphore = VK_TRUE;
    const char *exts[] = {"VK_KHR_external_semaphore_fd"};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f12, 0, 1, &qci, 0, NULL, 1, exts, NULL};
    if (qfam == nq || vkCreateDevice(pd, &dci, NULL, &dev)) {
        printf("FAILED: vkCreateDevice\n");
        return 1;
    }
#define G(n) n = (PFN_##n)vkGetDeviceProcAddr(dev, #n)
    G(vkGetDeviceQueue); G(vkQueueSubmit); G(vkCreateSemaphore); G(vkDestroySemaphore); G(vkQueueWaitIdle); G(vkDeviceWaitIdle);
    G(vkDestroyDevice); G(vkSignalSemaphore); G(vkGetSemaphoreFdKHR);
#undef G
    vkGetDeviceQueue(dev, qfam, 0, &queue);
    printf("device: %s (expecting %s)\n", props.deviceName, expect);

    /* stall: an empty submit the server holds, then a synchronous call that has to wait for it. */
    if (!strcmp(expect, "stall")) {
        VkSemaphore b0 = exportable();
        double t0 = now_ms();
        VkResult r = submit_signal(b0, VK_NULL_HANDLE);
        /* from another thread: its own connection, ordered behind the submit by the server */
        pthread_t w;
        if (r == VK_SUCCESS) {
            pthread_create(&w, NULL, idle_thread, &r);
            pthread_join(w, NULL);
        }
        printf("  a held submit, then vkDeviceWaitIdle on another thread: %d after %.0f ms\n", r, now_ms() - t0);
        vkDestroySemaphore(dev, b0, NULL);
        vkDestroyDevice(dev, NULL);
        printf(r == VK_SUCCESS ? "OK: done\n" : "FAILED: %d\n", r);
        return r == VK_SUCCESS ? 0 : 1;
    }

    /* 1: the held batch */
    VkSemaphoreTypeCreateInfo tci = {VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, NULL, VK_SEMAPHORE_TYPE_TIMELINE, 0};
    VkSemaphoreCreateInfo sci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &tci, 0};
    VkSemaphore timeline = VK_NULL_HANDLE, b = exportable();
    vkCreateSemaphore(dev, &sci, NULL, &timeline);
    VkResult r = submit_signal(b, timeline);
    if (r != VK_SUCCESS) {
        printf("FAILED: the held submit: %d\n", r);
        return 1;
    }
    pthread_t th;
    double t0 = now_ms();
    pthread_create(&th, NULL, late_signal, &timeline);
    int fd = export_fd(b, &r);
    double took = now_ms() - t0;
    printf("  held batch: export %d, fd %d, %.0f ms (the batch held %d ms)\n", r, fd, took, HOLD_MS);
    if (!strcmp(expect, "direct")) {
        if (took < HOLD_MS / 3) {
            printf("SKIP: the driver answered the export before the batch was out (%.0f ms): nothing to bound here\n", took);
            if (fd >= 0) close(fd);
            pthread_join(th, NULL);
            vkDeviceWaitIdle(dev);
            return 0;
        }
        check(r == VK_SUCCESS && fd >= 0 && took >= HOLD_MS * 0.8, "the export waits for the batch and gives a sync fd");
    } else if (!strcmp(expect, "bounded")) {
        check(r == VK_SUCCESS && took < HOLD_MS * 0.7, "the export returns before the batch is out (the server's bound)");
        check(fd == -1 || fd >= 0, "the fd is -1 (already signalled) or a sync fd");
    }
    if (fd >= 0) close(fd);
    /* at once, while the driver's export may still be blocked on it */
    vkDestroySemaphore(dev, b, NULL);
    pthread_join(th, NULL);
    r = vkQueueWaitIdle(queue);
    check(r == VK_SUCCESS || !strcmp(expect, "any"), "the queue goes idle after the signal");

    /* 2: an ordinary export */
    b = exportable();
    submit_signal(b, VK_NULL_HANDLE);
    t0 = now_ms();
    fd = export_fd(b, &r);
    took = now_ms() - t0;
    printf("  ordinary batch: export %d, fd %d, %.1f ms\n", r, fd, took);
    vkQueueWaitIdle(queue);
    int signalled = 0;
    if (fd >= 0) {
        struct pollfd p = {fd, POLLIN, 0};
        signalled = poll(&p, 1, 1000) == 1;
        close(fd);
    }
    if (strcmp(expect, "any")) {
        check(r == VK_SUCCESS && took < 500, "an ordinary export is quick");
        check(fd >= 0 ? signalled : fd == -1, "its fd signals once the queue is idle (or is -1)");
    }
    vkDestroySemaphore(dev, b, NULL);
    vkDestroySemaphore(dev, timeline, NULL);
    vkDeviceWaitIdle(dev);
    vkDestroyDevice(dev, NULL);
    if (!strcmp(expect, "any")) {
        printf("OK: printed\n");
        return 0;
    }
    if (failures) printf("FAILED: %d of %d checks\n", failures, checks);
    else printf("OK: %d checks\n", checks);
    return failures ? 1 : 0;
}
