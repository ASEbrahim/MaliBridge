/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * present_block_check: vkQueuePresentKHR must not hold the presenting thread on GPU work that waits
 * for something the same thread does after the present. D3D12 and Vulkan both allow a queue wait on
 * a timeline value that is signalled later (wait-before-signal), by the host or by a later submit.
 * A present that waits on the CPU for its frame then waits for itself: the thread cannot make the
 * signal while it is inside the present.
 *
 *   present_block_check [--wayland] [--queue2] [--watchdog-ms N] [--frames N]
 *
 * Thread A (named "vkd3d_queue", as vkd3d-proton's submission thread, which presents in D3D12 games):
 *   1 acquires an image, submits the frame's work waiting on timeline T >= 1, presents it (present
 *     id 1 where present_id is offered);
 *   2 checks the frame is not shown yet (vkWaitForPresentKHR(1, 0) = VK_TIMEOUT: the frame is
 *     handed to the window system only after its GPU work);
 *   3 signals T = 1: from the host (vkSignalSemaphore), or with --queue2 by a submit on a second
 *     queue;
 *   4 waits for the frame to be shown (vkWaitForPresentKHR(1, 5 s)), then presents --frames more
 *     ordinary frames.
 * A watchdog thread signals T itself after --watchdog-ms (default 8000) if thread A is still inside
 * the present, so a deadlock ends: the check then fails, with how long the present took.
 * The surface is headless (the swapchain's copy path) or, with --wayland, a role-less Wayland surface
 * of the compositor in WAYLAND_DISPLAY (dma-buf images, the presenting thread). VKBRIDGE_WSI_SHM=1
 * in the environment makes the Wayland swapchain a copy one (wl_shm).
 * Also printed: the threads named "vkd3d_queue" after the first present (a thread the bridge
 * starts from thread A inherits its name).
 * One line per step, "OK: ..." / "FAILED: ..." / "SKIPPED: ..." last (exit 0 / 1 / 3).
 */
#define _GNU_SOURCE
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dirent.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static PFN_vkGetInstanceProcAddr gipa;
static VkInstance inst;
static VkDevice dev;

#define I(name) static PFN_##name name
I(vkCreateInstance); I(vkEnumeratePhysicalDevices); I(vkGetPhysicalDeviceQueueFamilyProperties); I(vkCreateDevice);
I(vkGetDeviceQueue); I(vkCreateCommandPool); I(vkAllocateCommandBuffers); I(vkBeginCommandBuffer); I(vkEndCommandBuffer);
I(vkCmdPipelineBarrier); I(vkQueueSubmit); I(vkCreateSemaphore); I(vkSignalSemaphore); I(vkGetSemaphoreCounterValue);
I(vkDeviceWaitIdle); I(vkCreateHeadlessSurfaceEXT); I(vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
I(vkGetPhysicalDeviceSurfacePresentModesKHR); I(vkGetPhysicalDeviceSurfaceSupportKHR); I(vkCreateSwapchainKHR);
I(vkGetSwapchainImagesKHR); I(vkAcquireNextImageKHR); I(vkQueuePresentKHR); I(vkCreateFence); I(vkWaitForFences);
I(vkResetFences); I(vkDestroySwapchainKHR); I(vkEnumerateDeviceExtensionProperties); I(vkWaitForPresentKHR);
#undef I

typedef struct {
    VkStructureType sType;
    const void *pNext;
    VkFlags flags;
    void *display;
    void *surface;
} wl_surface_ci;
typedef VkResult (VKAPI_PTR *pfn_create_wl_surface)(VkInstance, const wl_surface_ci *, const VkAllocationCallbacks *, VkSurfaceKHR *);

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

/* ------------------------------------------------------------------ a Wayland surface (dlopen) */

static struct {
    void *(*display_connect)(const char *);
    int (*display_roundtrip)(void *);
    void *(*marshal_constructor)(void *, uint32_t, const void *, ...);
    void *(*marshal_constructor_versioned)(void *, uint32_t, const void *, uint32_t, ...);
    int (*add_listener)(void *, void (**)(void), void *);
    const void *registry_iface, *compositor_iface, *surface_iface;
    void *compositor;
} wl;

static void reg_global(void *data, void *registry, uint32_t name, const char *iface, uint32_t version)
{
    (void)data;
    (void)version;
    if (!strcmp(iface, "wl_compositor") && !wl.compositor)
        wl.compositor = wl.marshal_constructor_versioned(registry, 0 /* bind */, wl.compositor_iface, 1, name, "wl_compositor", 1u, NULL);
}
static void reg_remove(void *data, void *registry, uint32_t name) { (void)data, (void)registry, (void)name; }
static void (*reg_listener[2])(void) = {(void (*)(void))reg_global, (void (*)(void))reg_remove};

static int wayland_surface(void **display, void **surface)
{
    void *l = dlopen("libwayland-client.so.0", RTLD_NOW);
    if (!l) return 0;
    wl.display_connect = dlsym(l, "wl_display_connect");
    wl.display_roundtrip = dlsym(l, "wl_display_roundtrip");
    wl.marshal_constructor = dlsym(l, "wl_proxy_marshal_constructor");
    wl.marshal_constructor_versioned = dlsym(l, "wl_proxy_marshal_constructor_versioned");
    wl.add_listener = dlsym(l, "wl_proxy_add_listener");
    wl.registry_iface = dlsym(l, "wl_registry_interface");
    wl.compositor_iface = dlsym(l, "wl_compositor_interface");
    wl.surface_iface = dlsym(l, "wl_surface_interface");
    if (!wl.display_connect || !wl.display_roundtrip || !wl.marshal_constructor || !wl.marshal_constructor_versioned ||
        !wl.add_listener || !wl.registry_iface || !wl.compositor_iface || !wl.surface_iface)
        return 0;
    void *d = wl.display_connect(NULL);
    if (!d) return 0;
    void *reg = wl.marshal_constructor(d, 1 /* wl_display.get_registry */, wl.registry_iface, NULL);
    wl.add_listener(reg, reg_listener, NULL);
    wl.display_roundtrip(d);
    if (!wl.compositor) return 0;
    *surface = wl.marshal_constructor(wl.compositor, 0 /* create_surface */, wl.surface_iface, NULL);
    *display = d;
    return *surface != NULL;
}

/* ------------------------------------------------------------------ the check */

static VkSemaphore gate;
static int watchdog_ms = 8000;
static volatile int in_present, rescued, finished;

/* Ends a deadlock: signals the gate if thread A is still inside the present after watchdog_ms. */
static void *watchdog(void *arg)
{
    (void)arg;
    double t0 = now_ms();
    while (!finished && now_ms() - t0 < watchdog_ms) usleep(10000);
    if (!finished && in_present) {
        rescued = 1;
        printf("watchdog: vkQueuePresentKHR has not returned after %d ms; signalling the gate from here\n", watchdog_ms);
        VkSemaphoreSignalInfo si = {VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, NULL, gate, 1};
        vkSignalSemaphore(dev, &si);
    }
    return NULL;
}

/* threads of this process named `name`, their tids in buf */
static int threads_named(const char *name, char *buf, size_t len)
{
    DIR *d = opendir("/proc/self/task");
    int n = 0;
    size_t used = 0;
    buf[0] = 0;
    if (!d) return -1;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char path[300], comm[32] = "";
        snprintf(path, sizeof(path), "/proc/self/task/%s/comm", e->d_name);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        if (fgets(comm, sizeof(comm), f)) comm[strcspn(comm, "\n")] = 0;
        fclose(f);
        if (!strcmp(comm, name)) {
            n++;
            if (used < len) used += (size_t)snprintf(buf + used, len - used, "%s%s", n > 1 ? " " : "", e->d_name);
        }
    }
    closedir(d);
    return n;
}

static int use_wl, use_q2, extra = 3;
static VkQueue queue, queue2;
static VkSwapchainKHR sc;
static VkCommandBuffer cbs[8];
static VkFence acq_fence;
static int has_pw;
static int fails;
static double t_present_ms;

static void step(int ok, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
#include <stdarg.h>
static void step(int ok, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("%s ", ok ? "ok  " : "FAIL");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    if (!ok) fails++;
}

static VkResult acquire(uint32_t *idx)
{
    VkResult r = vkAcquireNextImageKHR(dev, sc, 5000000000ull, VK_NULL_HANDLE, acq_fence, idx);
    if (r >= 0) {
        vkWaitForFences(dev, 1, &acq_fence, VK_TRUE, 5000000000ull);
        vkResetFences(dev, 1, &acq_fence);
    }
    return r;
}

static VkResult present(uint32_t idx, uint64_t id)
{
    VkPresentIdKHR pid = {VK_STRUCTURE_TYPE_PRESENT_ID_KHR, NULL, 1, &id};
    /* No wait semaphore: the swapchain's own submit comes after the held batch on the queue
     * (present_late_check.c says why a binary semaphore from a held batch deadlocks on RADV). */
    VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, has_pw && id ? &pid : NULL, 0, NULL, 1, &sc, &idx, NULL};
    return vkQueuePresentKHR(queue, &pi);
}

/* thread A: vkd3d-proton's queue thread in a D3D12 game */
static void *thread_a(void *arg)
{
    (void)arg;
    pthread_setname_np(pthread_self(), "vkd3d_queue");
    char tids[256];
    int before = threads_named("vkd3d_queue", tids, sizeof(tids));
    uint32_t idx = 0;
    VkResult r = acquire(&idx);
    if (r < 0) {
        step(0, "1 acquire: %d", r);
        return NULL;
    }
    uint64_t one = 1;
    VkPipelineStageFlags st = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkTimelineSemaphoreSubmitInfo ts = {VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO, NULL, 1, &one, 0, NULL};
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, &ts, 1, &gate, &st, 1, &cbs[idx], 0, NULL};
    r = vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
    if (r) {
        step(0, "1 submit (waits on T >= 1): %d", r);
        return NULL;
    }
    in_present = 1;
    double t0 = now_ms();
    r = present(idx, 1);
    t_present_ms = now_ms() - t0;
    in_present = 0;
    int after = threads_named("vkd3d_queue", tids, sizeof(tids));
    step(r >= 0 && !rescued && t_present_ms < 2000,
         "1 present of a frame whose work waits on T >= 1 (signalled later by this thread): %d after %.0f ms%s", r, t_present_ms,
         rescued ? " - DEADLOCK, ended by the watchdog" : "");
    printf("     threads named vkd3d_queue: %d before the present, %d after (tids %s)\n", before, after, tids);
    if (has_pw && !rescued && r >= 0) {
        VkResult poll = vkWaitForPresentKHR(dev, sc, 1, 0);
        step(poll == VK_TIMEOUT, "2 frame 1 not shown while its work waits: vkWaitForPresentKHR(1, 0) = %d", poll);
    } else {
        printf("     2 not checked (%s)\n", has_pw ? "the present failed or was rescued" : "no VK_KHR_present_wait");
    }
    double tg = now_ms();
    if (!rescued) {
        if (use_q2) {
            VkTimelineSemaphoreSubmitInfo ts2 = {VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO, NULL, 0, NULL, 1, &one};
            VkSubmitInfo s2 = {VK_STRUCTURE_TYPE_SUBMIT_INFO, &ts2, 0, NULL, NULL, 0, NULL, 1, &gate};
            r = vkQueueSubmit(queue2, 1, &s2, VK_NULL_HANDLE);
            printf("     3 T = 1 by a submit on the second queue: %d\n", r);
        } else {
            VkSemaphoreSignalInfo ssi = {VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, NULL, gate, 1};
            r = vkSignalSemaphore(dev, &ssi);
            printf("     3 T = 1 from the host: %d\n", r);
        }
    }
    if (has_pw) {
        VkResult w = vkWaitForPresentKHR(dev, sc, 1, 5000000000ull);
        step(w >= 0, "4 frame 1 shown after the signal: vkWaitForPresentKHR(1, 5 s) = %d, %.0f ms after the signal", w, now_ms() - tg);
    }
    int bad = 0;
    double mx = 0;
    for (int f = 0; f < extra && !bad; f++) {
        r = acquire(&idx);
        if (r < 0) {
            bad = 1;
            break;
        }
        VkSubmitInfo s = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cbs[idx], 0, NULL};
        vkQueueSubmit(queue, 1, &s, VK_NULL_HANDLE);
        double a = now_ms();
        r = present(idx, (uint64_t)f + 2);
        double ms = now_ms() - a;
        if (ms > mx) mx = ms;
        if (r < 0) bad = 1;
    }
    step(!bad, "5 %d more frames: %s, longest present %.1f ms", extra, bad ? "an acquire or present failed" : "presented", mx);
    return NULL;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--wayland")) use_wl = 1;
        else if (!strcmp(argv[i], "--queue2")) use_q2 = 1;
        else if (!strcmp(argv[i], "--watchdog-ms") && i + 1 < argc) watchdog_ms = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) extra = atoi(argv[++i]);
    }
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib) {
        printf("FAILED: loader: %s\n", dlerror());
        return 2;
    }
    gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
#define G(n) n = (PFN_##n)gipa(inst, #n)
    G(vkCreateInstance);
    const char *iexts[2] = {"VK_KHR_surface", use_wl ? "VK_KHR_wayland_surface" : "VK_EXT_headless_surface"};
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "vkbridge-present-block-check", 1, "vkbridge", 1, VK_API_VERSION_1_2};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 2, iexts};
    if (vkCreateInstance(&ici, NULL, &inst)) {
        printf("FAILED: setup: vkCreateInstance\n");
        return 2;
    }
    G(vkEnumeratePhysicalDevices); G(vkGetPhysicalDeviceQueueFamilyProperties); G(vkCreateDevice); G(vkGetDeviceQueue);
    G(vkCreateCommandPool); G(vkAllocateCommandBuffers); G(vkBeginCommandBuffer); G(vkEndCommandBuffer); G(vkCmdPipelineBarrier);
    G(vkQueueSubmit); G(vkCreateSemaphore); G(vkSignalSemaphore); G(vkGetSemaphoreCounterValue); G(vkDeviceWaitIdle);
    G(vkCreateHeadlessSurfaceEXT); G(vkGetPhysicalDeviceSurfaceCapabilitiesKHR); G(vkGetPhysicalDeviceSurfacePresentModesKHR);
    G(vkGetPhysicalDeviceSurfaceSupportKHR); G(vkCreateSwapchainKHR); G(vkGetSwapchainImagesKHR); G(vkAcquireNextImageKHR);
    G(vkQueuePresentKHR); G(vkCreateFence); G(vkWaitForFences); G(vkResetFences); G(vkDestroySwapchainKHR);
    G(vkEnumerateDeviceExtensionProperties); G(vkWaitForPresentKHR);
#undef G
    uint32_t n = 1;
    VkPhysicalDevice pd;
    vkEnumeratePhysicalDevices(inst, &n, &pd);
    if (!n) {
        printf("FAILED: setup: no GPU\n");
        return 2;
    }
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (use_wl) {
        void *d = NULL, *s = NULL;
        if (!wayland_surface(&d, &s)) {
            printf("SKIPPED: no Wayland compositor\n");
            return 3;
        }
        pfn_create_wl_surface create = (pfn_create_wl_surface)gipa(inst, "vkCreateWaylandSurfaceKHR");
        wl_surface_ci ci = {VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR, NULL, 0, d, s};
        if (!create || create(inst, &ci, NULL, &surface)) {
            printf("FAILED: setup: vkCreateWaylandSurfaceKHR\n");
            return 2;
        }
    } else {
        VkHeadlessSurfaceCreateInfoEXT ci = {VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT};
        if (!vkCreateHeadlessSurfaceEXT || vkCreateHeadlessSurfaceEXT(inst, &ci, NULL, &surface)) {
            printf("FAILED: setup: vkCreateHeadlessSurfaceEXT\n");
            return 2;
        }
    }

    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, NULL);
    VkQueueFamilyProperties qp[16];
    if (nq > 16) nq = 16;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qp);
    uint32_t family = UINT32_MAX, family2 = UINT32_MAX, index2 = 0;
    for (uint32_t i = 0; i < nq && family == UINT32_MAX; i++) {
        VkBool32 ok = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(pd, i, surface, &ok);
        if ((qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && ok) family = i;
    }
    if (family == UINT32_MAX) {
        printf("FAILED: setup: no queue can present\n");
        return 2;
    }
    if (use_q2) {
        if (qp[family].queueCount > 1) family2 = family, index2 = 1;
        for (uint32_t i = 0; i < nq && family2 == UINT32_MAX; i++)
            if (i != family && qp[i].queueCount) family2 = i;
        if (family2 == UINT32_MAX) {
            printf("SKIPPED: one queue only\n");
            return 3;
        }
    }
    float prio[2] = {1, 1};
    VkDeviceQueueCreateInfo qci[2] = {{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, family, family2 == family ? 2 : 1, prio},
                                      {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, family2, 1, prio}};
    uint32_t nqci = use_q2 && family2 != family ? 2 : 1;
    uint32_t ne = 0;
    vkEnumerateDeviceExtensionProperties(pd, NULL, &ne, NULL);
    VkExtensionProperties *ep = calloc(ne ? ne : 1, sizeof(*ep));
    vkEnumerateDeviceExtensionProperties(pd, NULL, &ne, ep);
    int has_id = 0, has_wait = 0;
    for (uint32_t i = 0; i < ne; i++) {
        if (!strcmp(ep[i].extensionName, "VK_KHR_present_id")) has_id = 1;
        if (!strcmp(ep[i].extensionName, "VK_KHR_present_wait")) has_wait = 1;
    }
    free(ep);
    has_pw = has_id && has_wait && vkWaitForPresentKHR;
    VkPhysicalDevicePresentWaitFeaturesKHR fw = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR, NULL, VK_TRUE};
    VkPhysicalDevicePresentIdFeaturesKHR fi = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR, &fw, VK_TRUE};
    VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, has_pw ? &fi : NULL};
    f12.timelineSemaphore = VK_TRUE;
    const char *dexts[3] = {"VK_KHR_swapchain", "VK_KHR_present_id", "VK_KHR_present_wait"};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f12, 0, nqci, qci, 0, NULL, has_pw ? 3 : 1, dexts, NULL};
    if (vkCreateDevice(pd, &dci, NULL, &dev)) {
        printf("FAILED: setup: vkCreateDevice\n");
        return 2;
    }
    vkGetDeviceQueue(dev, family, 0, &queue);
    if (use_q2) vkGetDeviceQueue(dev, family2, index2, &queue2);

    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surface, &caps);
    VkPresentModeKHR modes[8], mode = VK_PRESENT_MODE_FIFO_KHR;
    uint32_t nm = 8;
    vkGetPhysicalDeviceSurfacePresentModesKHR(pd, surface, &nm, modes);
    for (uint32_t i = 0; i < nm; i++)
        if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR || (modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR && mode == VK_PRESENT_MODE_FIFO_KHR))
            mode = modes[i];
    uint32_t want = caps.minImageCount > 3 ? caps.minImageCount : 3;
    if (caps.maxImageCount && want > caps.maxImageCount) want = caps.maxImageCount;
    VkExtent2D ext = caps.currentExtent.width == 0xFFFFFFFFu ? (VkExtent2D){64, 64} : caps.currentExtent;
    VkSwapchainCreateInfoKHR sci = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, NULL, 0, surface, want, VK_FORMAT_B8G8R8A8_UNORM,
                                    VK_COLOR_SPACE_SRGB_NONLINEAR_KHR, ext, 1, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                    VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
                                    VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, mode, VK_TRUE, VK_NULL_HANDLE};
    if (vkCreateSwapchainKHR(dev, &sci, NULL, &sc)) {
        printf("FAILED: setup: vkCreateSwapchainKHR\n");
        return 2;
    }
    uint32_t nimg = 8;
    VkImage imgs[8];
    vkGetSwapchainImagesKHR(dev, sc, &nimg, imgs);
    printf("%s surface, %u images, present mode %d, present wait %s, T signalled %s\n", use_wl ? "Wayland" : "headless", nimg, mode,
           has_pw ? "on" : "off", use_q2 ? "by a submit on a second queue" : "from the host");

    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, 0, family};
    VkCommandPool pool;
    vkCreateCommandPool(dev, &pci, NULL, &pool);
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, nimg};
    vkAllocateCommandBuffers(dev, &cai, cbs);
    for (uint32_t i = 0; i < nimg; i++) {
        VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        vkBeginCommandBuffer(cbs[i], &bi);
        VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, 0, 0, VK_IMAGE_LAYOUT_UNDEFINED,
                                  VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, imgs[i],
                                  {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        vkCmdPipelineBarrier(cbs[i], VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
        vkEndCommandBuffer(cbs[i]);
    }
    VkSemaphoreTypeCreateInfo ti = {VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, NULL, VK_SEMAPHORE_TYPE_TIMELINE, 0};
    VkSemaphoreCreateInfo tci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &ti};
    vkCreateSemaphore(dev, &tci, NULL, &gate);
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    vkCreateFence(dev, &fci, NULL, &acq_fence);

    pthread_t a, w;
    pthread_create(&w, NULL, watchdog, NULL);
    pthread_create(&a, NULL, thread_a, NULL);
    pthread_join(a, NULL);
    finished = 1;
    pthread_join(w, NULL);
    uint64_t v = 0;
    vkGetSemaphoreCounterValue(dev, gate, &v);
    if (!v) {
        VkSemaphoreSignalInfo ssi = {VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, NULL, gate, 1};
        vkSignalSemaphore(dev, &ssi);
    }
    vkDeviceWaitIdle(dev);
    vkDestroySwapchainKHR(dev, sc, NULL);
    if (fails || rescued) {
        printf("FAILED: %d step(s)%s\n", fails, rescued ? "; the present held its own thread until the watchdog signalled T" : "");
        return 1;
    }
    printf("OK: the present returned in %.0f ms while its frame waited on a later signal from the same thread; shown after it\n", t_present_ms);
    return 0;
}
