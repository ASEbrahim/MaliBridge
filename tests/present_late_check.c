/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * present_late_check: a frame whose GPU work takes longer than 10 s is presented late, not turned
 * into a lost device (Watch_Dogs on a Dimensity 8300 lost its device while loading, the
 * bridge client's own 10 s present-fence limit; client/wsi.c vkb_wsi_wait_frame).
 *
 *   present_late_check ok|lost [--wayland] [--delay-ms N]
 *
 * The first frame's rendering waits on a timeline semaphore that another thread signals N ms
 * later (default 15000): the GPU queue is held that long, as a queue busy converting textures is.
 * Then two ordinary frames. The surface is headless (the swapchain's synchronous path,
 * vkb_wsi_wait_rendering) or, with --wayland, a Wayland surface of the compositor in
 * WAYLAND_DISPLAY (dma-buf images: the presenting thread, wsi_wayland.c); the Wayland surface has
 * no role, nothing is shown.
 *
 * ok:   every acquire and present succeeds; headless: the slow present returns after N ms;
 * lost: some acquire or present returns VK_ERROR_DEVICE_LOST (a limit below N ms:
 *       VKBRIDGE_PRESENT_TIMEOUT_S, in the guest or from the server).
 * One line per step, "OK: ..." / "FAILED: ..." last.
 */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

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
static VkQueue queue;

#define I(name) static PFN_##name name
I(vkCreateInstance); I(vkEnumeratePhysicalDevices); I(vkGetPhysicalDeviceQueueFamilyProperties); I(vkCreateDevice);
I(vkGetDeviceQueue); I(vkCreateCommandPool); I(vkAllocateCommandBuffers); I(vkBeginCommandBuffer); I(vkEndCommandBuffer);
I(vkCmdPipelineBarrier); I(vkQueueSubmit); I(vkCreateSemaphore); I(vkSignalSemaphore); I(vkQueueWaitIdle); I(vkDeviceWaitIdle);
I(vkCreateHeadlessSurfaceEXT); I(vkGetPhysicalDeviceSurfaceCapabilitiesKHR); I(vkGetPhysicalDeviceSurfacePresentModesKHR);
I(vkGetPhysicalDeviceSurfaceSupportKHR); I(vkCreateSwapchainKHR); I(vkGetSwapchainImagesKHR); I(vkAcquireNextImageKHR);
I(vkQueuePresentKHR); I(vkCreateFence); I(vkWaitForFences); I(vkResetFences);
#undef I

/* VkWaylandSurfaceCreateInfoKHR without <wayland-client.h> (not installed on every host) */
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
    if (!strcmp(iface, "wl_compositor") && !wl.compositor)
        wl.compositor = wl.marshal_constructor_versioned(registry, 0 /* bind */, wl.compositor_iface, 1, name,
                                                         "wl_compositor", 1u, NULL);
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

/* ------------------------------------------------------------------ the slow GPU job */

static VkSemaphore gate;
static int delay_ms = 15000;
static volatile int signalled;

static void *signal_later(void *arg)
{
    (void)arg;
    usleep((useconds_t)delay_ms * 1000);
    VkSemaphoreSignalInfo si = {VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, NULL, gate, 1};
    VkResult r = vkSignalSemaphore(dev, &si);
    printf("gate signalled after %d ms: %d\n", delay_ms, r);
    fflush(stdout);
    signalled = 1;
    return NULL;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    const char *expect = argc > 1 ? argv[1] : "ok";
    int use_wl = 0;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--wayland")) use_wl = 1;
        else if (!strcmp(argv[i], "--delay-ms") && i + 1 < argc) delay_ms = atoi(argv[++i]);
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
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "vkbridge-present-late-check", 1, "vkbridge", 1, VK_API_VERSION_1_2};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 2, iexts};
    if (vkCreateInstance(&ici, NULL, &inst)) {
        printf("FAILED: setup: vkCreateInstance\n");
        return 2;
    }
    G(vkEnumeratePhysicalDevices); G(vkGetPhysicalDeviceQueueFamilyProperties); G(vkCreateDevice); G(vkGetDeviceQueue);
    G(vkCreateCommandPool); G(vkAllocateCommandBuffers); G(vkBeginCommandBuffer); G(vkEndCommandBuffer); G(vkCmdPipelineBarrier);
    G(vkQueueSubmit); G(vkCreateSemaphore); G(vkSignalSemaphore); G(vkQueueWaitIdle); G(vkDeviceWaitIdle);
    G(vkCreateHeadlessSurfaceEXT); G(vkGetPhysicalDeviceSurfaceCapabilitiesKHR); G(vkGetPhysicalDeviceSurfacePresentModesKHR);
    G(vkGetPhysicalDeviceSurfaceSupportKHR); G(vkCreateSwapchainKHR); G(vkGetSwapchainImagesKHR); G(vkAcquireNextImageKHR);
    G(vkQueuePresentKHR); G(vkCreateFence); G(vkWaitForFences); G(vkResetFences);
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
    uint32_t family = UINT32_MAX;
    for (uint32_t i = 0; i < nq && family == UINT32_MAX; i++) {
        VkBool32 ok = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(pd, i, surface, &ok);
        if ((qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && ok) family = i;
    }
    if (family == UINT32_MAX) {
        printf("FAILED: setup: no queue can present\n");
        return 2;
    }
    float prio = 1;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, family, 1, &prio};
    VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    f12.timelineSemaphore = VK_TRUE;
    const char *dexts[1] = {"VK_KHR_swapchain"};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f12, 0, 1, &qci, 0, NULL, 1, dexts, NULL};
    if (vkCreateDevice(pd, &dci, NULL, &dev)) {
        printf("FAILED: setup: vkCreateDevice\n");
        return 2;
    }
    vkGetDeviceQueue(dev, family, 0, &queue);

    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surface, &caps);
    VkPresentModeKHR modes[8], mode = VK_PRESENT_MODE_FIFO_KHR;
    uint32_t nm = 8;
    vkGetPhysicalDeviceSurfacePresentModesKHR(pd, surface, &nm, modes);
    /* A surface with no role gets no frame callbacks: not FIFO where there is a choice */
    for (uint32_t i = 0; i < nm; i++)
        if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR || (modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR && mode == VK_PRESENT_MODE_FIFO_KHR))
            mode = modes[i];
    uint32_t want = caps.minImageCount > 4 ? caps.minImageCount : 4;
    if (caps.maxImageCount && want > caps.maxImageCount) want = caps.maxImageCount;
    VkExtent2D ext = caps.currentExtent.width == 0xFFFFFFFFu ? (VkExtent2D){64, 64} : caps.currentExtent;
    VkSwapchainCreateInfoKHR sci = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, NULL, 0, surface, want, VK_FORMAT_B8G8R8A8_UNORM,
                                    VK_COLOR_SPACE_SRGB_NONLINEAR_KHR, ext, 1, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                    VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
                                    VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, mode, VK_TRUE, VK_NULL_HANDLE};
    VkSwapchainKHR sc;
    if (vkCreateSwapchainKHR(dev, &sci, NULL, &sc)) {
        printf("FAILED: setup: vkCreateSwapchainKHR\n");
        return 2;
    }
    uint32_t nimg = 8;
    VkImage imgs[8];
    vkGetSwapchainImagesKHR(dev, sc, &nimg, imgs);
    printf("%s surface, %u images, present mode %d, first frame held %d ms\n", use_wl ? "Wayland" : "headless", nimg, mode, delay_ms);

    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, 0, family};
    VkCommandPool pool;
    vkCreateCommandPool(dev, &pci, NULL, &pool);
    VkCommandBuffer cbs[8];
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
    VkSemaphoreCreateInfo bci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence acq_fence;
    vkCreateFence(dev, &fci, NULL, &acq_fence);

    int lost = 0, failed = 0;
    double slow_ms = 0;
    pthread_t th;
    int th_on = 0;
    for (int frame = 0; frame < 3; frame++) {
        VkSemaphore done;
        vkCreateSemaphore(dev, &bci, NULL, &done);
        uint32_t idx = 0;
        /* the acquire waited for on the CPU (a fence): only the gate holds the first frame's work */
        VkResult r = vkAcquireNextImageKHR(dev, sc, 5000000000ull, VK_NULL_HANDLE, acq_fence, &idx);
        if (r >= 0) {
            vkWaitForFences(dev, 1, &acq_fence, VK_TRUE, 5000000000ull);
            vkResetFences(dev, 1, &acq_fence);
        }
        printf("frame %d: acquire %d (image %u)\n", frame, r, idx);
        if (r == VK_ERROR_DEVICE_LOST) lost = 1;
        if (r < 0) {
            failed = 1;
            break;
        }
        uint64_t one = 1;
        VkPipelineStageFlags st = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkTimelineSemaphoreSubmitInfo ts = {VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO, NULL, 1, &one, 0, NULL};
        VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, frame == 0 ? &ts : NULL, frame == 0, &gate, &st, 1, &cbs[idx], frame ? 1 : 0, &done};
        r = vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
        if (r) {
            printf("frame %d: submit %d\n", frame, r);
            failed = 1;
            if (r == VK_ERROR_DEVICE_LOST) lost = 1;
            break;
        }
        if (frame == 0) th_on = !pthread_create(&th, NULL, signal_later, NULL);
        double t0 = now_ms();
        /* The held frame is presented without a wait semaphore: the swapchain's own submit (with its
         * fence) comes after the held batch on the queue, so its fence still signals only once the gate
         * opens. A binary semaphore from the held batch would make RADV block in that submit until the
         * gate opens (a wait-before-signal), and the gate's own vkSignalSemaphore waits in the server
         * for that submit (the process's request order): a deadlock of the test's making. */
        VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, NULL, frame ? 1 : 0, frame ? &done : NULL, 1, &sc, &idx, NULL};
        r = vkQueuePresentKHR(queue, &pi);
        double ms = now_ms() - t0;
        printf("frame %d: present %d after %.0f ms\n", frame, r, ms);
        fflush(stdout);
        if (frame == 0) {
            slow_ms = ms;
            /* the GPU done with the slow frame, and the presenting thread past it */
            if (th_on) pthread_join(th, NULL);
            th_on = 0;
            vkQueueWaitIdle(queue);
            usleep(500000);
        }
        if (r == VK_ERROR_DEVICE_LOST) lost = 1;
        if (r < 0) {
            failed = 1;
            break;
        }
    }
    if (th_on) pthread_join(th, NULL);
    if (!signalled) {
        /* the gate never opened (an error before the thread): open it so the device can idle */
        VkSemaphoreSignalInfo ssi = {VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, NULL, gate, 1};
        vkSignalSemaphore(dev, &ssi);
    }
    vkDeviceWaitIdle(dev);

    if (!strcmp(expect, "lost")) {
        if (lost) printf("OK: the device was reported lost (a limit below %d ms)\n", delay_ms);
        else printf("FAILED: the device was not reported lost\n");
        return lost ? 0 : 1;
    }
    if (failed) {
        printf("FAILED: a frame held %d ms on the GPU ended the swapchain (%s)\n", delay_ms, lost ? "device lost" : "an error");
        return 1;
    }
    if (!use_wl && slow_ms < delay_ms - 200) {
        printf("FAILED: the slow present returned after %.0f ms, before the GPU was done (%d ms)\n", slow_ms, delay_ms);
        return 1;
    }
    printf("OK: a frame held %d ms on the GPU was presented late (%.0f ms in vkQueuePresentKHR), the next frames too\n", delay_ms, slow_ms);
    return 0;
}
