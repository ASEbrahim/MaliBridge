/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * presentwait_check: VK_KHR_present_id + VK_KHR_present_wait (and present_id2 + present_wait2,
 * --v2) on the bridge's swapchain (client/wsi.c, wsi_wayland.c).
 *
 *   presentwait_check [--wayland | --xcb] [--v2] [--off] [--engine NAME] [--hold-ms N] [--frames N]
 *
 * Surface: headless (copy presents in the app's call), --wayland (a role-less wl_surface on the
 * compositor in WAYLAND_DISPLAY: dma-buf images, the presenting thread) or --xcb (a mapped X11
 * window on DISPLAY; under gamescope its WSI layer turns it into a Wayland one, the device's path).
 *
 *  1 the extensions and features are offered (--off: none are, as with VKBRIDGE_PRESENT_WAIT=0 or
 *    --engine DXVK, and the check ends there); --engine: the VkApplicationInfo's engine name;
 *  2 ordering: a frame (id 1) whose GPU work is held --hold-ms (default 300) by a timeline
 *    semaphore another thread signals; a wait for it with timeout 0 is VK_TIMEOUT while held (the
 *    presenting thread's path; the copy paths already waited in vkQueuePresentKHR), and a wait
 *    without a timeout returns only after the GPU work was let go;
 *  3 steady: --frames (default 60) frames with ids 2.., each followed by a wait for the one before
 *    (vkd3d-proton's "latency 1"): all VK_SUCCESS, a wait for an id already shown returns at once;
 *  4 timeout: a wait for an id never presented, 100 ms timeout: VK_TIMEOUT after about 100 ms;
 *  5 retire: a thread waits (no timeout) for an id the swapchain never presents, then a new swapchain
 *    is created with it as oldSwapchain: the wait ends with VK_ERROR_OUT_OF_DATE_KHR;
 *  6 destroy: a thread waits (no timeout) for an id never presented on the new swapchain, which is then
 *    destroyed: the wait ends with VK_ERROR_OUT_OF_DATE_KHR and vkDestroySwapchainKHR returns.
 * One line per step, "OK: ..." / "FAILED: ..." / "SKIPPED: ..." last.
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
static VkPhysicalDevice pd;
static VkDevice dev;
static VkQueue queue;
static uint32_t family;
static VkSurfaceKHR surface;
static int v2, fails;

#define I(name) static PFN_##name name
I(vkCreateInstance); I(vkEnumeratePhysicalDevices); I(vkGetPhysicalDeviceQueueFamilyProperties); I(vkCreateDevice);
I(vkEnumerateDeviceExtensionProperties); I(vkGetPhysicalDeviceFeatures2); I(vkGetDeviceProcAddr);
I(vkGetDeviceQueue); I(vkCreateCommandPool); I(vkAllocateCommandBuffers); I(vkBeginCommandBuffer); I(vkEndCommandBuffer);
I(vkCmdPipelineBarrier); I(vkQueueSubmit); I(vkCreateSemaphore); I(vkSignalSemaphore); I(vkQueueWaitIdle); I(vkDeviceWaitIdle);
I(vkCreateHeadlessSurfaceEXT); I(vkGetPhysicalDeviceSurfaceCapabilitiesKHR); I(vkGetPhysicalDeviceSurfaceCapabilities2KHR);
I(vkGetPhysicalDeviceSurfacePresentModesKHR); I(vkGetPhysicalDeviceSurfaceSupportKHR); I(vkCreateSwapchainKHR);
I(vkGetSwapchainImagesKHR); I(vkAcquireNextImageKHR); I(vkQueuePresentKHR); I(vkCreateFence); I(vkWaitForFences);
I(vkResetFences); I(vkDestroySwapchainKHR); I(vkWaitForPresentKHR); I(vkWaitForPresent2KHR); I(vkResetCommandBuffer);
#undef I

typedef struct {
    VkStructureType sType;
    const void *pNext;
    VkFlags flags;
    void *display;
    void *surface;
} wl_surface_ci;
typedef VkResult (VKAPI_PTR *pfn_create_wl_surface)(VkInstance, const wl_surface_ci *, const VkAllocationCallbacks *, VkSurfaceKHR *);
typedef struct {
    VkStructureType sType;
    const void *pNext;
    VkFlags flags;
    void *connection;
    uint32_t window;
} xcb_surface_ci;
typedef VkResult (VKAPI_PTR *pfn_create_xcb_surface)(VkInstance, const xcb_surface_ci *, const VkAllocationCallbacks *, VkSurfaceKHR *);

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

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

/* ------------------------------------------------------------------ window systems (dlopen) */

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
    (void)data, (void)version;
    if (!strcmp(iface, "wl_compositor") && !wl.compositor)
        wl.compositor = wl.marshal_constructor_versioned(registry, 0, wl.compositor_iface, 1, name, "wl_compositor", 1u, NULL);
}
static void reg_remove(void *data, void *registry, uint32_t name) { (void)data, (void)registry, (void)name; }
static void (*reg_listener[2])(void) = {(void (*)(void))reg_global, (void (*)(void))reg_remove};

static int wayland_surface(void **display, void **surf)
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
    void *reg = wl.marshal_constructor(d, 1, wl.registry_iface, NULL);
    wl.add_listener(reg, reg_listener, NULL);
    wl.display_roundtrip(d);
    if (!wl.compositor) return 0;
    *surf = wl.marshal_constructor(wl.compositor, 0, wl.surface_iface, NULL);
    *display = d;
    return *surf != NULL;
}

/* a mapped 640x360 X11 window */
static int xcb_window(void **conn, uint32_t *win)
{
    void *l = dlopen("libxcb.so.1", RTLD_NOW);
    if (!l) return 0;
    void *(*connect)(const char *, int *) = dlsym(l, "xcb_connect");
    int (*has_error)(void *) = dlsym(l, "xcb_connection_has_error");
    const void *(*get_setup)(void *) = dlsym(l, "xcb_get_setup");
    struct it { void *data; int rem, index; } (*roots)(const void *) = dlsym(l, "xcb_setup_roots_iterator");
    uint32_t (*gen_id)(void *) = dlsym(l, "xcb_generate_id");
    struct ck { unsigned int seq; } (*create)(void *, uint8_t, uint32_t, uint32_t, int16_t, int16_t, uint16_t, uint16_t, uint16_t, uint16_t,
                                              uint32_t, uint32_t, const void *) = dlsym(l, "xcb_create_window");
    struct ck (*map)(void *, uint32_t) = dlsym(l, "xcb_map_window");
    int (*flush)(void *) = dlsym(l, "xcb_flush");
    if (!connect || !has_error || !get_setup || !roots || !gen_id || !create || !map || !flush) return 0;
    void *c = connect(NULL, NULL);
    if (!c || has_error(c)) return 0;
    struct it r = roots(get_setup(c));
    /* xcb_screen_t: root (u32), default_colormap, white_pixel, black_pixel, current_input_masks, ... root_visual at 32 */
    const uint8_t *scr = r.data;
    uint32_t root, visual;
    memcpy(&root, scr, 4);
    memcpy(&visual, scr + 32, 4);
    uint32_t w = gen_id(c);
    create(c, 0 /* copy from parent */, w, root, 0, 0, 640, 360, 0, 1 /* input/output */, visual, 0, NULL);
    map(c, w);
    flush(c);
    *conn = c;
    *win = w;
    return 1;
}

/* ------------------------------------------------------------------ the held GPU job */

static VkSemaphore gate;
static int hold_ms = 300;
static double t_gate;

static void *signal_later(void *arg)
{
    uint64_t v = (uint64_t)(uintptr_t)arg;
    usleep((useconds_t)hold_ms * 1000);
    t_gate = now_ms();
    VkSemaphoreSignalInfo si = {VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, NULL, gate, v};
    vkSignalSemaphore(dev, &si);
    return NULL;
}

/* ------------------------------------------------------------------ the swapchain */

typedef struct chainx {
    VkSwapchainKHR sc;
    uint32_t n;
    VkImage img[8];
    VkCommandBuffer cb[8];
    VkCommandPool pool;
} chainx;

static VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
static VkFence acq_fence;

static int make_chain(chainx *c, VkSwapchainKHR old)
{
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surface, &caps);
    uint32_t want = caps.minImageCount > 4 ? caps.minImageCount : 4;
    if (caps.maxImageCount && want > caps.maxImageCount) want = caps.maxImageCount;
    VkExtent2D ext = caps.currentExtent.width == 0xFFFFFFFFu ? (VkExtent2D){64, 64} : caps.currentExtent;
    VkSwapchainCreateInfoKHR sci = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, NULL, 0, surface, want, VK_FORMAT_B8G8R8A8_UNORM,
                                    VK_COLOR_SPACE_SRGB_NONLINEAR_KHR, ext, 1, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                    VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
                                    VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, mode, VK_TRUE, old};
    if (v2) sci.flags |= VK_SWAPCHAIN_CREATE_PRESENT_ID_2_BIT_KHR | VK_SWAPCHAIN_CREATE_PRESENT_WAIT_2_BIT_KHR;
    if (vkCreateSwapchainKHR(dev, &sci, NULL, &c->sc)) return 0;
    c->n = 8;
    vkGetSwapchainImagesKHR(dev, c->sc, &c->n, c->img);
    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, 0, family};
    vkCreateCommandPool(dev, &pci, NULL, &c->pool);
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, c->pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, c->n};
    vkAllocateCommandBuffers(dev, &cai, c->cb);
    for (uint32_t i = 0; i < c->n; i++) {
        VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        vkBeginCommandBuffer(c->cb[i], &bi);
        VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, 0, 0, VK_IMAGE_LAYOUT_UNDEFINED,
                                  VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, c->img[i],
                                  {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        vkCmdPipelineBarrier(c->cb[i], VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
        vkEndCommandBuffer(c->cb[i]);
    }
    return 1;
}

static VkResult wait_present(VkSwapchainKHR sc, uint64_t id, uint64_t timeout)
{
    if (v2) {
        VkPresentWait2InfoKHR wi = {VK_STRUCTURE_TYPE_PRESENT_WAIT_2_INFO_KHR, NULL, id, timeout};
        return vkWaitForPresent2KHR(dev, sc, &wi);
    }
    return vkWaitForPresentKHR(dev, sc, id, timeout);
}

/* one frame with present id `id`; gate_value: its GPU work waits for the gate to reach it (0: not held) */
static VkResult frame(chainx *c, uint64_t id, uint64_t gate_value)
{
    uint32_t idx = 0;
    VkResult r = vkAcquireNextImageKHR(dev, c->sc, 5000000000ull, VK_NULL_HANDLE, acq_fence, &idx);
    if (r < 0) return r;
    vkWaitForFences(dev, 1, &acq_fence, VK_TRUE, 5000000000ull);
    vkResetFences(dev, 1, &acq_fence);
    VkPipelineStageFlags st = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkTimelineSemaphoreSubmitInfo ts = {VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO, NULL, 1, &gate_value, 0, NULL};
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, gate_value ? &ts : NULL, gate_value ? 1 : 0, &gate, &st, 1, &c->cb[idx], 0, NULL};
    r = vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
    if (r) return r;
    /* no wait semaphore: the swapchain's own submit comes after this batch on the queue
     * (present_late_check.c says why a binary semaphore from a held batch deadlocks on RADV) */
    VkPresentIdKHR pid = {VK_STRUCTURE_TYPE_PRESENT_ID_KHR, NULL, 1, &id};
    VkPresentId2KHR pid2 = {VK_STRUCTURE_TYPE_PRESENT_ID_2_KHR, NULL, 1, &id};
    VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, v2 ? (const void *)&pid2 : (const void *)&pid, 0, NULL, 1, &c->sc, &idx, NULL};
    return vkQueuePresentKHR(queue, &pi);
}

typedef struct waiter {
    VkSwapchainKHR sc;
    uint64_t id;
    VkResult r;
    double ms;
    volatile int done;
} waiter;

static void *wait_thread(void *arg)
{
    waiter *w = arg;
    double t0 = now_ms();
    w->r = wait_present(w->sc, w->id, UINT64_MAX);
    w->ms = now_ms() - t0;
    w->done = 1;
    return NULL;
}

static int has_ext(const VkExtensionProperties *e, uint32_t n, const char *name)
{
    for (uint32_t i = 0; i < n; i++)
        if (!strcmp(e[i].extensionName, name)) return 1;
    return 0;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    int use_wl = 0, use_xcb = 0, off = 0, frames = 60;
    const char *engine = "vkbridge";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--wayland")) use_wl = 1;
        else if (!strcmp(argv[i], "--xcb")) use_xcb = 1;
        else if (!strcmp(argv[i], "--v2")) v2 = 1;
        else if (!strcmp(argv[i], "--off")) off = 1;
        else if (!strcmp(argv[i], "--engine") && i + 1 < argc) engine = argv[++i];
        else if (!strcmp(argv[i], "--hold-ms") && i + 1 < argc) hold_ms = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
    }
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib) {
        printf("FAILED: loader: %s\n", dlerror());
        return 2;
    }
    gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
#define G(n) n = (PFN_##n)gipa(inst, #n)
    G(vkCreateInstance);
    const char *iexts[3] = {"VK_KHR_surface", use_wl ? "VK_KHR_wayland_surface" : use_xcb ? "VK_KHR_xcb_surface" : "VK_EXT_headless_surface",
                            "VK_KHR_get_surface_capabilities2"};
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "vkbridge-presentwait-check", 1, engine, 1, VK_API_VERSION_1_2};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 3, iexts};
    if (vkCreateInstance(&ici, NULL, &inst)) {
        printf("FAILED: setup: vkCreateInstance\n");
        return 2;
    }
    G(vkEnumeratePhysicalDevices); G(vkGetPhysicalDeviceQueueFamilyProperties); G(vkCreateDevice); G(vkGetDeviceQueue);
    G(vkEnumerateDeviceExtensionProperties); G(vkGetPhysicalDeviceFeatures2); G(vkGetDeviceProcAddr);
    G(vkCreateCommandPool); G(vkAllocateCommandBuffers); G(vkBeginCommandBuffer); G(vkEndCommandBuffer); G(vkCmdPipelineBarrier);
    G(vkQueueSubmit); G(vkCreateSemaphore); G(vkSignalSemaphore); G(vkQueueWaitIdle); G(vkDeviceWaitIdle);
    G(vkCreateHeadlessSurfaceEXT); G(vkGetPhysicalDeviceSurfaceCapabilitiesKHR); G(vkGetPhysicalDeviceSurfaceCapabilities2KHR);
    G(vkGetPhysicalDeviceSurfacePresentModesKHR); G(vkGetPhysicalDeviceSurfaceSupportKHR); G(vkCreateSwapchainKHR);
    G(vkGetSwapchainImagesKHR); G(vkAcquireNextImageKHR); G(vkQueuePresentKHR); G(vkCreateFence); G(vkWaitForFences);
    G(vkResetFences); G(vkDestroySwapchainKHR); G(vkResetCommandBuffer);
    uint32_t n = 1;
    vkEnumeratePhysicalDevices(inst, &n, &pd);
    if (!n) {
        printf("FAILED: setup: no GPU\n");
        return 2;
    }

    /* 1 offered */
    uint32_t ne = 0;
    vkEnumerateDeviceExtensionProperties(pd, NULL, &ne, NULL);
    VkExtensionProperties *ep = calloc(ne + 1, sizeof(*ep));
    vkEnumerateDeviceExtensionProperties(pd, NULL, &ne, ep);
    int e_id = has_ext(ep, ne, "VK_KHR_present_id"), e_wait = has_ext(ep, ne, "VK_KHR_present_wait");
    int e_id2 = has_ext(ep, ne, "VK_KHR_present_id2"), e_wait2 = has_ext(ep, ne, "VK_KHR_present_wait2");
    VkPhysicalDevicePresentWait2FeaturesKHR fw2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_2_FEATURES_KHR};
    VkPhysicalDevicePresentId2FeaturesKHR fi2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_2_FEATURES_KHR, &fw2};
    VkPhysicalDevicePresentWaitFeaturesKHR fw = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR, &fi2};
    VkPhysicalDevicePresentIdFeaturesKHR fi = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR, &fw};
    VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &fi};
    fi.presentId = fw.presentWait = fi2.presentId2 = fw2.presentWait2 = 7; /* not 0/1: shows whether anyone wrote them */
    vkGetPhysicalDeviceFeatures2(pd, &f2);
    if (off) {
        int none = !e_id && !e_wait && !e_id2 && !e_wait2 && !fi.presentId && !fw.presentWait && !fi2.presentId2 && !fw2.presentWait2;
        step(none, "1 off: extensions id %d wait %d id2 %d wait2 %d, features %u %u %u %u", e_id, e_wait, e_id2, e_wait2, fi.presentId,
             fw.presentWait, fi2.presentId2, fw2.presentWait2);
        printf(fails ? "FAILED: present waits offered\n" : "OK: nothing offered\n");
        return fails ? 1 : 0;
    }
    int all = e_id && e_wait && e_id2 && e_wait2 && fi.presentId == 1 && fw.presentWait == 1 && fi2.presentId2 == 1 && fw2.presentWait2 == 1;
    step(all, "1 offered: extensions id %d wait %d id2 %d wait2 %d, features %u %u %u %u", e_id, e_wait, e_id2, e_wait2, fi.presentId,
         fw.presentWait, fi2.presentId2, fw2.presentWait2);
    if (!all) {
        printf("FAILED: present waits not offered\n");
        return 1;
    }

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
    } else if (use_xcb) {
        void *c = NULL;
        uint32_t w = 0;
        if (!xcb_window(&c, &w)) {
            printf("SKIPPED: no X server\n");
            return 3;
        }
        pfn_create_xcb_surface create = (pfn_create_xcb_surface)gipa(inst, "vkCreateXcbSurfaceKHR");
        xcb_surface_ci ci = {VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR, NULL, 0, c, w};
        if (!create || create(inst, &ci, NULL, &surface)) {
            printf("FAILED: setup: vkCreateXcbSurfaceKHR\n");
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
    family = UINT32_MAX;
    for (uint32_t i = 0; i < nq && family == UINT32_MAX; i++) {
        VkBool32 ok = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(pd, i, surface, &ok);
        if ((qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && ok) family = i;
    }
    if (family == UINT32_MAX) {
        printf("FAILED: setup: no queue can present\n");
        return 2;
    }
    if (v2) {
        VkSurfaceCapabilitiesPresentWait2KHR cw2 = {VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_PRESENT_WAIT_2_KHR};
        VkSurfaceCapabilitiesPresentId2KHR ci2 = {VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_PRESENT_ID_2_KHR, &cw2};
        VkSurfaceCapabilities2KHR c2 = {VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR, &ci2};
        VkPhysicalDeviceSurfaceInfo2KHR si = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR, NULL, surface};
        vkGetPhysicalDeviceSurfaceCapabilities2KHR(pd, &si, &c2);
        step(ci2.presentId2Supported && cw2.presentWait2Supported, "1 the surface supports present id2 %u, wait2 %u", ci2.presentId2Supported,
             cw2.presentWait2Supported);
    }

    float prio = 1;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, family, 1, &prio};
    VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    f12.timelineSemaphore = VK_TRUE;
    /* only the pair under test, the other pair's structs left out */
    VkPhysicalDevicePresentIdFeaturesKHR ei = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR, &f12, VK_TRUE};
    VkPhysicalDevicePresentWaitFeaturesKHR ew = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR, &ei, VK_TRUE};
    VkPhysicalDevicePresentId2FeaturesKHR ei2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_2_FEATURES_KHR, &f12, VK_TRUE};
    VkPhysicalDevicePresentWait2FeaturesKHR ew2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_2_FEATURES_KHR, &ei2, VK_TRUE};
    const char *dexts1[3] = {"VK_KHR_swapchain", "VK_KHR_present_id", "VK_KHR_present_wait"};
    const char *dexts2[3] = {"VK_KHR_swapchain", "VK_KHR_present_id2", "VK_KHR_present_wait2"};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, v2 ? (const void *)&ew2 : (const void *)&ew, 0, 1, &qci, 0, NULL, 3,
                              v2 ? dexts2 : dexts1, NULL};
    if (vkCreateDevice(pd, &dci, NULL, &dev)) {
        printf("FAILED: setup: vkCreateDevice with %s\n", v2 ? "present_id2/wait2" : "present_id/wait");
        return 2;
    }
    vkWaitForPresentKHR = (PFN_vkWaitForPresentKHR)vkGetDeviceProcAddr(dev, "vkWaitForPresentKHR");
    vkWaitForPresent2KHR = (PFN_vkWaitForPresent2KHR)vkGetDeviceProcAddr(dev, "vkWaitForPresent2KHR");
#undef G
    if (v2 ? !vkWaitForPresent2KHR || vkWaitForPresentKHR : !vkWaitForPresentKHR || vkWaitForPresent2KHR) {
        printf("FAILED: setup: vkGetDeviceProcAddr gave wait %p, wait2 %p\n", (void *)vkWaitForPresentKHR, (void *)vkWaitForPresent2KHR);
        return 2;
    }
    vkGetDeviceQueue(dev, family, 0, &queue);

    VkPresentModeKHR modes[8];
    uint32_t nm = 8;
    vkGetPhysicalDeviceSurfacePresentModesKHR(pd, surface, &nm, modes);
    /* A role-less Wayland surface gets no frame callbacks: not FIFO there */
    if (use_wl)
        for (uint32_t i = 0; i < nm; i++)
            if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR || (modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR && mode == VK_PRESENT_MODE_FIFO_KHR))
                mode = modes[i];
    VkSemaphoreTypeCreateInfo ti = {VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, NULL, VK_SEMAPHORE_TYPE_TIMELINE, 0};
    VkSemaphoreCreateInfo tci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &ti};
    vkCreateSemaphore(dev, &tci, NULL, &gate);
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    vkCreateFence(dev, &fci, NULL, &acq_fence);
    chainx c;
    if (!make_chain(&c, VK_NULL_HANDLE)) {
        printf("FAILED: setup: vkCreateSwapchainKHR\n");
        return 2;
    }
    int async = use_wl || use_xcb; /* xcb: under gamescope's layer a Wayland surface; on plain X11 a copy present */
    printf("%s surface, %u images, present mode %d, %s, frame 1 held %d ms\n", use_wl ? "Wayland" : use_xcb ? "X11" : "headless", c.n, mode,
           v2 ? "present_id2 + present_wait2" : "present_id + present_wait", hold_ms);

    /* 2 ordering */
    pthread_t th;
    pthread_create(&th, NULL, signal_later, (void *)(uintptr_t)1);
    double t0 = now_ms();
    VkResult r = frame(&c, 1, 1);
    double t_present = now_ms() - t0;
    VkResult poll = wait_present(c.sc, 1, 0);
    double tw0 = now_ms();
    VkResult rw = wait_present(c.sc, 1, UINT64_MAX);
    double t_ret = now_ms();
    pthread_join(th, NULL);
    int presented_late = t_present >= hold_ms - 20; /* the copy paths: vkQueuePresentKHR waited for the rendering */
    step(r >= 0 && rw == VK_SUCCESS && t_ret >= t_gate && (presented_late ? poll == VK_SUCCESS : poll == VK_TIMEOUT),
         "2 ordering: present %d after %.0f ms; while held, wait(1, 0) = %d; wait(1) = %d after %.0f ms, %.1f ms after the GPU work was let go",
         r, t_present, poll, rw, t_ret - tw0, t_ret - t_gate);
    if (async && presented_late) printf("     (the present waited for the rendering: a copy path, not the presenting thread)\n");

    /* 3 steady: id k presented, then the wait for k - 1 */
    int bad = 0;
    double sum = 0, mx = 0, show_sum = 0, show_max = 0;
    uint64_t id = 1;
    for (int i = 0; i < frames && !bad; i++) {
        id++;
        double a = now_ms();
        r = frame(&c, id, 0);
        double b = now_ms();
        rw = wait_present(c.sc, id - 1, 2000000000ull);
        double w = now_ms() - b;
        if (r < 0 || rw != VK_SUCCESS) {
            printf("     frame %llu: present %d, wait(%llu) %d\n", (unsigned long long)id, r, (unsigned long long)(id - 1), rw);
            bad = 1;
        }
        sum += w;
        if (w > mx) mx = w;
        (void)a;
    }
    /* the last one, until shown: present -> shown */
    double b = now_ms();
    rw = wait_present(c.sc, id, 2000000000ull);
    double last = now_ms() - b;
    VkResult again = wait_present(c.sc, id - 5, 0);
    show_sum = last, show_max = last;
    step(!bad && rw == VK_SUCCESS && again == VK_SUCCESS,
         "3 steady: %d frames, each then a wait for the one before: %.2f ms on average, longest %.1f ms; the last frame shown %.1f ms after "
         "its present; an earlier id: %d", frames, sum / (frames ? frames : 1), mx, show_max, again);
    (void)show_sum;

    /* 4 timeout */
    t0 = now_ms();
    rw = wait_present(c.sc, id + 100, 100000000ull);
    double tt = now_ms() - t0;
    step(rw == VK_TIMEOUT && tt >= 95 && tt < 1000, "4 timeout: wait(%llu, 100 ms) = %d after %.0f ms", (unsigned long long)(id + 100), rw, tt);

    /* 5 retire */
    waiter w1 = {c.sc, id + 100, 0, 0, 0};
    pthread_create(&th, NULL, wait_thread, &w1);
    usleep(150000);
    int early = w1.done;
    chainx c2;
    vkQueueWaitIdle(queue);
    if (!make_chain(&c2, c.sc)) {
        printf("FAILED: setup: the second vkCreateSwapchainKHR\n");
        return 2;
    }
    pthread_join(th, NULL);
    step(!early && w1.r == VK_ERROR_OUT_OF_DATE_KHR, "5 retire: a wait for an id never presented ended %d after %.0f ms (the new swapchain made at 150)",
         w1.r, w1.ms);
    vkDestroySwapchainKHR(dev, c.sc, NULL);

    /* 6 destroy while waiting */
    r = frame(&c2, 1, 0);
    rw = wait_present(c2.sc, 1, 2000000000ull);
    waiter w2 = {c2.sc, 50, 0, 0, 0};
    pthread_create(&th, NULL, wait_thread, &w2);
    usleep(150000);
    early = w2.done;
    vkQueueWaitIdle(queue);
    t0 = now_ms();
    vkDestroySwapchainKHR(dev, c2.sc, NULL);
    double td = now_ms() - t0;
    pthread_join(th, NULL);
    step(r >= 0 && rw == VK_SUCCESS && !early && w2.r == VK_ERROR_OUT_OF_DATE_KHR,
         "6 destroy: the new swapchain's frame 1 %d/%d; a wait for id 50 ended %d after %.0f ms; vkDestroySwapchainKHR took %.0f ms", r, rw, w2.r,
         w2.ms, td);

    vkDeviceWaitIdle(dev);
    if (fails) printf("FAILED: %d step%s\n", fails, fails == 1 ? "" : "s");
    else printf("OK: present ids and waits (%s) on a %s surface\n", v2 ? "v2" : "v1", use_wl ? "Wayland" : use_xcb ? "X11" : "headless");
    return fails ? 1 : 0;
}
