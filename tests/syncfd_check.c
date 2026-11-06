/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * syncfd_check: a sync-fd export (vkGetSemaphoreFdKHR, VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT)
 * of a binary semaphore whose signalling batch the driver has not handed to the kernel yet: the
 * batch waits on a timeline value another thread signals HOLD_MS later. A driver with a submit
 * thread (Mesa's RADV here; Samsung's Xclipse driver is suspected of the same: Xwayland
 * stuck in this call, the X server with it) answers the export only once the batch is out.
 *
 *   syncfd_check direct|bounded|stall|order|orderoff|any
 *     direct:  the export takes about HOLD_MS and gives a valid fd (the driver's own answer; the
 *              bridge's bound off, or no bridge);
 *     bounded: the export ends well before HOLD_MS (VKBRIDGE_SYNCFD_EXPORT_MS on the server, set
 *              below HOLD_MS), fd -1 ("already signalled"); the semaphore is destroyed at once,
 *              while the driver's call may still be blocked on it (the server holds that destroy
 *              back until the call returns);
 *     stall:   an empty submit and a vkDeviceWaitIdle on another thread after it, nothing else (the server holds the
 *              submit: VKBRIDGE_TEST_DELAY_CMD; its log then says what the wait waited for);
 *     order:   a submit the server holds (VKBRIDGE_TEST_DELAY_CMD) and, on the same thread right after it, a sync-fd
 *              export of what it signals (Xwayland's pattern): with VKBRIDGE_SYNCFD_ORDER_MS below the hold
 *              the export is answered at that deadline with fd -1; the queue goes idle once the submit is out;
 *     orderoff: the same with the order deadline off: the export waits for the whole hold;
 *     endstall: a command buffer of a few recorded commands is ended (asynchronous through the bridge, its stream replayed
 *              by the server) and a vkDeviceWaitIdle on another thread waits behind it. The server holds the replay of
 *              one command (VKBRIDGE_TEST_DELAY_CMD=vkCmdSetViewport) or the driver's own End (=driver-end): its log then
 *              names the command it is in (Xwayland's End never returned on Xclipse);
 *     any:     printed, not judged.
 *   Both then check an ordinary export (the batch out at once): quick, a valid fd, and that the fd
 *   signals once the queue is idle. A driver that does not block (the first export quick in
 *   "direct") prints "SKIP" and exits 0.
 * One line per step, "OK: ..." / "FAILED: ..." / "SKIP: ..." last.
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
I(vkCreateCommandPool); I(vkAllocateCommandBuffers); I(vkBeginCommandBuffer); I(vkEndCommandBuffer); I(vkCmdPipelineBarrier);
I(vkCmdSetViewport); I(vkDestroyCommandPool);
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

/* The client's flusher sends an asynchronous request on its own connection 100 us after it is queued; a
 * synchronous call made before that sends the queue first on ITS connection (client/conn.c), where the held
 * request is no earlier request of another connection and the server has no order wait to name it. On a busy
 * host the flusher can be that late (1 in 5 runs on 11-05 while the PC was gaming), so the held request is let
 * go out first: the waiting thread then starts about 0.2 s into the hold. */
static void let_the_flusher_send(void)
{
    struct timespec ts = {0, 200000000L};
    nanosleep(&ts, NULL);
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
    G(vkCreateCommandPool); G(vkAllocateCommandBuffers); G(vkBeginCommandBuffer); G(vkEndCommandBuffer); G(vkCmdPipelineBarrier);
    G(vkCmdSetViewport); G(vkDestroyCommandPool);
#undef G
    vkGetDeviceQueue(dev, qfam, 0, &queue);
    printf("device: %s (expecting %s)\n", props.deviceName, expect);

    /* stall: an empty submit (asynchronous through the bridge; the server holds it with
     * VKBRIDGE_TEST_DELAY_CMD), then a synchronous call that has to wait for it there. */
    if (!strcmp(expect, "stall")) {
        VkSemaphore b0 = exportable();
        double t0 = now_ms();
        VkResult r = submit_signal(b0, VK_NULL_HANDLE);
        /* from another thread: its own connection, ordered behind the submit by the server */
        pthread_t w;
        if (r == VK_SUCCESS) {
            let_the_flusher_send();
            pthread_create(&w, NULL, idle_thread, &r);
            pthread_join(w, NULL);
        }
        printf("  a held submit, then vkDeviceWaitIdle on another thread: %d after %.0f ms\n", r, now_ms() - t0);
        vkDestroySemaphore(dev, b0, NULL);
        vkDestroyDevice(dev, NULL);
        printf(r == VK_SUCCESS ? "OK: done\n" : "FAILED: %d\n", r);
        return r == VK_SUCCESS ? 0 : 1;
    }

    /* endstall: a recorded command buffer ended (the server replays its stream, holding one command), then a synchronous
     * call on another thread that has to wait behind that asynchronous End. */
    if (!strcmp(expect, "endstall")) {
        VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, 0, qfam};
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer cb = VK_NULL_HANDLE;
        VkResult r = vkCreateCommandPool(dev, &pci, NULL, &pool);
        VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
        if (r == VK_SUCCESS) r = vkAllocateCommandBuffers(dev, &ai, &cb);
        double t0 = now_ms();
        if (r == VK_SUCCESS) {
            VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT};
            VkViewport vp = {0, 0, 64, 64, 0, 1};
            r = vkBeginCommandBuffer(cb, &bi);
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
            vkCmdSetViewport(cb, 0, 1, &vp);
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
            VkResult re = vkEndCommandBuffer(cb);
            if (r == VK_SUCCESS) r = re;
        }
        pthread_t w;
        if (r == VK_SUCCESS) {
            let_the_flusher_send();
            pthread_create(&w, NULL, idle_thread, &r);
            pthread_join(w, NULL);
        }
        printf("  a command buffer ended, then vkDeviceWaitIdle on another thread: %d after %.0f ms\n", r, now_ms() - t0);
        if (pool) vkDestroyCommandPool(dev, pool, NULL);
        vkDestroyDevice(dev, NULL);
        printf(r == VK_SUCCESS ? "OK: done\n" : "FAILED: %d\n", r);
        return r == VK_SUCCESS ? 0 : 1;
    }

    /* order / orderoff: the held submit is the first; an async call before it starts the client's flusher */
    if (!strcmp(expect, "order") || !strcmp(expect, "orderoff")) {
        VkSemaphore warm = exportable();
        vkDestroySemaphore(dev, warm, NULL);
        struct timespec ts = {0, 200000000L};
        nanosleep(&ts, NULL);
        VkSemaphore b0 = exportable();
        VkResult r = submit_signal(b0, VK_NULL_HANDLE);
        double t0 = now_ms();
        int fd = r == VK_SUCCESS ? export_fd(b0, &r) : -2;
        double took = now_ms() - t0;
        printf("  a held submit, then its export on the same thread: export %d, fd %d, %.0f ms\n", r, fd, took);
        if (!strcmp(expect, "order"))
            check(r == VK_SUCCESS && fd == -1 && took < 2000, "the export is answered at the order deadline with fd -1");
        else
            check(r == VK_SUCCESS && fd >= 0 && took >= 3000, "the export waits for the held submit and gives a sync fd");
        if (fd >= 0) close(fd);
        t0 = now_ms();
        r = vkQueueWaitIdle(queue);
        printf("  vkQueueWaitIdle: %d after %.0f ms more\n", r, now_ms() - t0);
        check(r == VK_SUCCESS, "the queue goes idle once the submit is out");
        vkDestroySemaphore(dev, b0, NULL);
        b0 = exportable();
        submit_signal(b0, VK_NULL_HANDLE);
        t0 = now_ms();
        fd = export_fd(b0, &r);
        took = now_ms() - t0;
        printf("  ordinary batch: export %d, fd %d, %.1f ms\n", r, fd, took);
        check(r == VK_SUCCESS && fd >= 0 && took < 500, "an ordinary export is quick and real");
        if (fd >= 0) close(fd);
        vkDeviceWaitIdle(dev);
        vkDestroySemaphore(dev, b0, NULL);
        vkDestroyDevice(dev, NULL);
        if (failures) printf("FAILED: %d of %d checks\n", failures, checks);
        else printf("OK: %d checks\n", checks);
        return failures ? 1 : 0;
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
