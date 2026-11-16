/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * present_path_check: the game's thread and the swapchain's presenting thread (client/wsi_wayland.c)
 * under a compositor that paces like nested gamescope (tests/fake_nested_wayland.py; gamescope's WSI
 * layer gives the driver MAILBOX and does FIFO in gamescope). Always a Wayland surface of
 * WAYLAND_DISPLAY, 3 images, MAILBOX. host/present_path_test.sh runs every mode.
 *
 *   block [--hold-ms N]      frames 0 and 1 render at once; frame 2's GPU work is held N ms (default 600)
 *                            by a timeline semaphore another thread signals, so the presenting thread
 *                            waits for it; the compositor (--release-delay-ms 50) releases image 0 ~50 ms
 *                            after frame 1's commit. OK when the 4th acquire returned in under N/2 ms (once
 *                            the release came, not once frame 2 rendered: VKBRIDGE_PRESENT_RELEASE_THREAD).
 *   import [--max-acquire-ms M]  the server holds every vkQueueSubmit (VKBRIDGE_TEST_DELAY_REPEAT); a loop of
 *                            frames, each one waiting for frame k-2's fence first: the acquire's own cost on
 *                            the game's thread. OK when its average is under M ms (VKBRIDGE_ASYNC_ACQUIRE).
 *   lat                      the micro-test: a loop like Godot's (wait for frame k-fq's fence, --cpu-ms of CPU
 *                            work, acquire, --cpu2-ms of CPU work, submit, present) with --gpu-ms of emulated GPU per frame (a timeline
 *                            value a thread signals that long after the GPU would start the frame). Prints the
 *                            game thread's ms per frame in vkAcquireNextImageKHR and vkQueuePresentKHR. Always OK.
 *   order [--acq-fence]      --frames frames, each image cleared to its frame number (the compositor reads it,
 *                            --pixels): the shell checks every frame was shown once, in order, and no image
 *                            changed while the compositor held it. Here: an acquire never returns an image the
 *                            app holds. --acq-fence: the acquire signals a fence (polled, then waited) instead
 *                            of a semaphore.
 *   recreate                 a new swapchain made from the old one (oldSwapchain) while releases are pending:
 *                            the old one answers VK_ERROR_OUT_OF_DATE_KHR, is destroyed, the new one presents.
 *   destroy [--hold-ms N]    a swapchain destroyed right after its presents, frame 2 held N ms on the GPU and
 *                            the releases late: it returns (no hang), within N + 2000 ms.
 *   idle                     5 frames, then 2 s with nothing presented: the process's CPU time in those 2 s
 *                            (no thread of the swapchain's spins). OK under 40 ms.
 *   lost                     the compositor drops the connection (--drop-after) or the device is lost
 *                            (VKBRIDGE_TEST_LOST): some call answers an error, nothing hangs, destroy returns.
 * --v11: the app asks for Vulkan 1.1 and no 1.2 features (Mali r18-r40 show games 1.1: no timeline semaphore).
 *   block and destroy then hold frame 2's GPU work through the server instead: frame 2's command buffer alone
 *   records a vkCmdSetViewport, which the server holds while it replays it (VKBRIDGE_TEST_DELAY_CMD=vkCmdSetViewport,
 *   VKBRIDGE_TEST_DELAY_MS=--hold-ms), so everything after it on the server waits; no frame-fence waits (--fq 4).
 *   lat needs the timeline semaphore (not with --v11).
 * One line per step, "OK: ..." / "FAILED: ..." / "SKIPPED: ..." last (exit 0 / 1 / 3; 2 = setup).
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
I(vkResetCommandBuffer); I(vkCmdPipelineBarrier); I(vkCmdClearColorImage); I(vkCmdSetViewport); I(vkQueueSubmit); I(vkCreateSemaphore);
I(vkSignalSemaphore); I(vkDeviceWaitIdle); I(vkGetPhysicalDeviceSurfaceSupportKHR); I(vkCreateSwapchainKHR);
I(vkGetSwapchainImagesKHR); I(vkAcquireNextImageKHR); I(vkQueuePresentKHR); I(vkCreateFence); I(vkWaitForFences);
I(vkGetFenceStatus); I(vkResetFences); I(vkDestroySwapchainKHR); I(vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
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

static void sleep_until_ms(double t)
{
    double d = t - now_ms();
    if (d <= 0) return;
    struct timespec ts = {(time_t)(d / 1e3), (long)((d - (time_t)(d / 1e3) * 1e3) * 1e6)};
    nanosleep(&ts, NULL);
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
    (void)data, (void)version;
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

/* ------------------------------------------------------------------ the emulated GPU */

/* Frame k's work waits for gate >= k + 1 (with the gate on). The thread signals it G ms after the later
 * of frame k's submit and frame k-1's end: one queue, frames back to back. */
static VkSemaphore gate;
static pthread_mutex_t gpu_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gpu_cond = PTHREAD_COND_INITIALIZER;
static double *submitted; /* frame -> ms, 0 = not yet */
static int gpu_frames, gpu_quit;
static double gpu_ms;

static void *gpu_thread(void *arg)
{
    (void)arg;
    double end = 0;
    for (int k = 0; k < gpu_frames; k++) {
        pthread_mutex_lock(&gpu_lock);
        while (!submitted[k] && !gpu_quit) pthread_cond_wait(&gpu_cond, &gpu_lock);
        double sub = submitted[k];
        int quit = gpu_quit;
        pthread_mutex_unlock(&gpu_lock);
        if (quit && !sub) break;
        double start = sub > end ? sub : end;
        end = start + gpu_ms;
        sleep_until_ms(end);
        VkSemaphoreSignalInfo si = {VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, NULL, gate, (uint64_t)k + 1};
        vkSignalSemaphore(dev, &si);
        double late = now_ms();
        if (late > end) end = late; /* the signal itself took time: the queue was busy until then */
    }
    return NULL;
}

static int hold_ms = 600;
static uint64_t open_value = 3;
static void *open_later(void *arg)
{
    (void)arg;
    usleep((useconds_t)hold_ms * 1000);
    VkSemaphoreSignalInfo si = {VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, NULL, gate, open_value};
    vkSignalSemaphore(dev, &si);
    printf("frame 2's GPU work let go after %d ms\n", hold_ms);
    return NULL;
}

/* ------------------------------------------------------------------ the swapchain */

static VkPhysicalDevice pd;
static VkSurfaceKHR surface;
static uint32_t family;
static VkCommandPool pool;
static VkExtent2D extent;

static VkSwapchainKHR make_chain(VkSwapchainKHR old, VkImage *imgs, uint32_t *nimg)
{
    VkSwapchainCreateInfoKHR sci = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, NULL, 0, surface, 3, VK_FORMAT_B8G8R8A8_UNORM,
                                    VK_COLOR_SPACE_SRGB_NONLINEAR_KHR, extent, 1, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                    VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
                                    VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_PRESENT_MODE_MAILBOX_KHR, VK_TRUE, old};
    VkSwapchainKHR sc = VK_NULL_HANDLE;
    VkResult r = vkCreateSwapchainKHR(dev, &sci, NULL, &sc);
    if (r) {
        printf("vkCreateSwapchainKHR: %d\n", r);
        return VK_NULL_HANDLE;
    }
    *nimg = 8;
    vkGetSwapchainImagesKHR(dev, sc, nimg, imgs);
    return sc;
}

/* A frame's commands: image cleared to the frame's number (B 0x5a, G k >> 8, R k & 0xff: the
 * compositor reads it), then ready to present. */
static void record(VkCommandBuffer cb, VkImage img, int k, int hold)
{
    vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(cb, &bi);
    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, img, range};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    VkClearColorValue c = {.float32 = {(float)(k & 0xff) / 255.0f, (float)((k >> 8) & 0xff) / 255.0f, (float)0x5a / 255.0f, 1.0f}};
    vkCmdClearColorImage(cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c, 1, &range);
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = 0;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    if (hold) {
        /* --v11: the server holds this command while it replays the buffer (VKBRIDGE_TEST_DELAY_CMD) */
        VkViewport vp = {0, 0, 64, 64, 0, 1};
        vkCmdSetViewport(cb, 0, 1, &vp);
    }
    vkEndCommandBuffer(cb);
}

/* ------------------------------------------------------------------ main */

static int v11;

static int setup(void)
{
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib) {
        printf("FAILED: loader: %s\n", dlerror());
        return 2;
    }
    gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
#define G(n) n = (PFN_##n)gipa(inst, #n)
    G(vkCreateInstance);
    const char *iexts[2] = {"VK_KHR_surface", "VK_KHR_wayland_surface"};
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "vkbridge-present-path-check", 1, "vkbridge", 1,
                             v11 ? VK_API_VERSION_1_1 : VK_API_VERSION_1_2};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 2, iexts};
    if (vkCreateInstance(&ici, NULL, &inst)) {
        printf("FAILED: setup: vkCreateInstance\n");
        return 2;
    }
    G(vkEnumeratePhysicalDevices); G(vkGetPhysicalDeviceQueueFamilyProperties); G(vkCreateDevice); G(vkGetDeviceQueue);
    G(vkCreateCommandPool); G(vkAllocateCommandBuffers); G(vkBeginCommandBuffer); G(vkEndCommandBuffer); G(vkResetCommandBuffer);
    G(vkCmdPipelineBarrier); G(vkCmdClearColorImage); G(vkCmdSetViewport); G(vkQueueSubmit); G(vkCreateSemaphore); G(vkSignalSemaphore);
    G(vkDeviceWaitIdle); G(vkGetPhysicalDeviceSurfaceSupportKHR); G(vkCreateSwapchainKHR); G(vkGetSwapchainImagesKHR);
    G(vkAcquireNextImageKHR); G(vkQueuePresentKHR); G(vkCreateFence); G(vkWaitForFences); G(vkGetFenceStatus); G(vkResetFences);
    G(vkDestroySwapchainKHR); G(vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
#undef G
    uint32_t n = 1;
    vkEnumeratePhysicalDevices(inst, &n, &pd);
    if (!n) {
        printf("FAILED: setup: no GPU\n");
        return 2;
    }
    void *d = NULL, *s = NULL;
    if (!wayland_surface(&d, &s)) {
        printf("SKIPPED: no Wayland compositor\n");
        return 3;
    }
    pfn_create_wl_surface create = (pfn_create_wl_surface)gipa(inst, "vkCreateWaylandSurfaceKHR");
    wl_surface_ci wci = {VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR, NULL, 0, d, s};
    if (!create || create(inst, &wci, NULL, &surface)) {
        printf("FAILED: setup: vkCreateWaylandSurfaceKHR\n");
        return 2;
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
    float prio = 1;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, family, 1, &prio};
    VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    f12.timelineSemaphore = VK_TRUE;
    const char *dexts[1] = {"VK_KHR_swapchain"};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, v11 ? NULL : &f12, 0, 1, &qci, 0, NULL, 1, dexts, NULL};
    if (vkCreateDevice(pd, &dci, NULL, &dev)) {
        printf("FAILED: setup: vkCreateDevice\n");
        return 2;
    }
    vkGetDeviceQueue(dev, family, 0, &queue);
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surface, &caps);
    extent = caps.currentExtent.width == 0xFFFFFFFFu ? (VkExtent2D){64, 64} : caps.currentExtent;
    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, family};
    vkCreateCommandPool(dev, &pci, NULL, &pool);
    if (!v11) {
        VkSemaphoreTypeCreateInfo ti = {VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, NULL, VK_SEMAPHORE_TYPE_TIMELINE, 0};
        VkSemaphoreCreateInfo tci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &ti};
        if (vkCreateSemaphore(dev, &tci, NULL, &gate)) {
            printf("FAILED: setup: no timeline semaphore (--v11 for a Vulkan 1.1 device)\n");
            return 2;
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    const char *mode = argc > 1 ? argv[1] : "block";
    int frames = 0, fq = 2, acq_fence = 0;
    double cpu_ms = 0, cpu2_ms = 0, max_acq = 1.0;
    gpu_ms = 0;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--hold-ms") && i + 1 < argc) hold_ms = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--cpu-ms") && i + 1 < argc) cpu_ms = atof(argv[++i]);
        else if (!strcmp(argv[i], "--cpu2-ms") && i + 1 < argc) cpu2_ms = atof(argv[++i]);
        else if (!strcmp(argv[i], "--gpu-ms") && i + 1 < argc) gpu_ms = atof(argv[++i]);
        else if (!strcmp(argv[i], "--fq") && i + 1 < argc) fq = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--max-acquire-ms") && i + 1 < argc) max_acq = atof(argv[++i]);
        else if (!strcmp(argv[i], "--acq-fence")) acq_fence = 1;
        else if (!strcmp(argv[i], "--v11")) v11 = 1;
    }
    int block = !strcmp(mode, "block"), import = !strcmp(mode, "import"), lat = !strcmp(mode, "lat"), order = !strcmp(mode, "order");
    int recreate = !strcmp(mode, "recreate"), destroy = !strcmp(mode, "destroy"), lost = !strcmp(mode, "lost");
    int idle = !strcmp(mode, "idle");
    if (!block && !import && !lat && !order && !recreate && !destroy && !lost && !idle) {
        printf("FAILED: unknown mode %s\n", mode);
        return 2;
    }
    if (!frames) frames = block ? 4 : destroy ? 3 : recreate ? 16 : import ? 200 : lost ? 400 : order ? 120 : idle ? 5 : 600;
    if (v11 && lat) {
        printf("FAILED: lat needs a timeline semaphore (no --v11)\n");
        return 2;
    }
    if (v11 && (block || destroy)) fq = 4; /* no frame-fence waits: on 1.1 each one drains the server's queue */
    int gated = !v11 && (lat || block || destroy); /* frames wait for the emulated GPU's gate */
    int s = setup();
    if (s) return s;
    VkImage imgs[8];
    uint32_t nimg = 0;
    VkSwapchainKHR sc = make_chain(VK_NULL_HANDLE, imgs, &nimg);
    if (!sc) {
        printf("FAILED: setup: vkCreateSwapchainKHR\n");
        return 2;
    }
    printf("%s: Wayland surface %ux%u, %u images, MAILBOX\n", mode, extent.width, extent.height, nimg);
    if (nimg != 3) {
        printf("FAILED: setup: %u images, 3 asked\n", nimg);
        return 2;
    }
    if (fq < 1) fq = 1;
    if (fq > 8) fq = 8;
    /* one command buffer per frame slot (fq of them, each waited for by its fence before reuse) */
    VkCommandBuffer cbs[8];
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 8};
    vkAllocateCommandBuffers(dev, &cai, cbs);
    VkSemaphoreCreateInfo bci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, NULL, VK_FENCE_CREATE_SIGNALED_BIT};
    VkFence ffence[8], afence;
    for (int i = 0; i < 8; i++) vkCreateFence(dev, &fci, NULL, &ffence[i]);
    VkFenceCreateInfo fci0 = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    vkCreateFence(dev, &fci0, NULL, &afence);
    VkSemaphore acq_sems[16];
    for (int i = 0; i < 16; i++) vkCreateSemaphore(dev, &bci, NULL, &acq_sems[i]);

    submitted = calloc((size_t)frames + 1, sizeof(double));
    gpu_frames = frames;
    pthread_t gth, oth;
    int oth_on = 0, gth_on = 0;
    if (lat) gth_on = !pthread_create(&gth, NULL, gpu_thread, NULL);
    if (destroy) open_value = 3;

    int failed = 0, held_twice = 0, errors = 0, frames_done = 0;
    VkResult first_err = VK_SUCCESS;
    const char *err_call = "";
    double acq_total = 0, acq_max = 0, pres_total = 0, pres_max = 0, acq3 = 0, t_start = 0, t_end = 0;
    int warm = lat ? (frames > 60 ? 60 : 0) : import ? 10 : 0, counted = 0;
    int holding[8] = {0}; /* images the app holds: acquired, not yet presented */
    int out_of_date_ok = 0;
    for (int k = 0; k < frames; k++) {
        if (k == warm) t_start = now_ms();
        if (k >= fq) {
            /* Godot's _stall_for_frame: frame k - fq's fence (also frees its command buffer) */
            VkFence f = ffence[k % fq];
            VkResult r = vkWaitForFences(dev, 1, &f, VK_TRUE, 10000000000ull);
            if (r) {
                if (!first_err) first_err = r, err_call = "vkWaitForFences";
                errors++;
                break;
            }
        }
        if (cpu_ms > 0) {
            /* CPU work before the acquire (Godot records the scene first); a spin, not a sleep */
            double until = now_ms() + cpu_ms;
            while (now_ms() < until) { }
        }
        if (block && k == 3) usleep(100 * 1000); /* frame 2 is in the presenting thread's wait; the release came at ~50 ms */
        if (recreate && k == 6) {
            /* a new swapchain from the old one while its releases are pending (--release-delay-ms) */
            VkImage nimgs[8];
            uint32_t nn = 0;
            VkSwapchainKHR nsc = make_chain(sc, nimgs, &nn);
            if (!nsc) {
                failed = 1;
                break;
            }
            uint32_t oi = 0;
            VkResult r = vkAcquireNextImageKHR(dev, sc, 0, acq_sems[15], VK_NULL_HANDLE, &oi);
            out_of_date_ok = r == VK_ERROR_OUT_OF_DATE_KHR;
            printf("frame %d: the old swapchain's acquire after the new one was made: %d\n", k, r);
            double d0 = now_ms();
            vkDestroySwapchainKHR(dev, sc, NULL);
            printf("frame %d: the old swapchain destroyed in %.1f ms, releases still pending\n", k, now_ms() - d0);
            sc = nsc;
            memcpy(imgs, nimgs, sizeof(nimgs));
            nimg = nn;
            memset(holding, 0, sizeof(holding));
        }
        uint32_t idx = 0;
        VkSemaphore asem = acq_fence ? VK_NULL_HANDLE : acq_sems[k % 16];
        double a0 = now_ms();
        VkResult r = vkAcquireNextImageKHR(dev, sc, 5000000000ull, asem, acq_fence ? afence : VK_NULL_HANDLE, &idx);
        double aw = now_ms() - a0;
        if (block) printf("frame %d: acquire %d (image %u) after %.1f ms\n", k, r, idx, aw);
        if (block && k == 3) acq3 = aw;
        if (r < 0 || r == VK_TIMEOUT) {
            if (!first_err) first_err = r, err_call = "vkAcquireNextImageKHR";
            errors++;
            if (!lost) printf("frame %d: acquire %d after %.1f ms\n", k, r, aw);
            break;
        }
        if (idx >= nimg) {
            printf("frame %d: acquire gave image %u of %u\n", k, idx, nimg);
            failed = 1;
            break;
        }
        if (holding[idx]) {
            printf("frame %d: acquire gave image %u, which the app still holds\n", k, idx);
            held_twice++;
        }
        holding[idx] = 1;
        if (block && k == 3) break;
        if (cpu2_ms > 0) {
            /* CPU work between the acquire and the submit (recording the passes that draw into the image) */
            double until = now_ms() + cpu2_ms;
            while (now_ms() < until) { }
        }
        if (acq_fence) {
            /* polled first (the poll may answer "not ready" for a moment, never wrongly ready), then waited */
            int polls = 0;
            while (vkGetFenceStatus(dev, afence) == VK_NOT_READY && polls < 1000) polls++, usleep(100);
            r = vkWaitForFences(dev, 1, &afence, VK_TRUE, 5000000000ull);
            if (r) {
                printf("frame %d: the acquire's fence: %d\n", k, r);
                if (!first_err) first_err = r, err_call = "vkWaitForFences (acquire fence)";
                errors++;
                break;
            }
            vkResetFences(dev, 1, &afence);
        }
        /* The frame's work: it waits for the acquire (binary) and, gated, for the emulated GPU. No wait
         * semaphore on the present: the swapchain's own submit follows on the queue (as in
         * present_late_check; a binary one would make a driver block in that submit). */
        int held = gated && (!block || k == 2) && (!destroy || k == 2);
        uint64_t vals[2] = {0, (uint64_t)k + 1};
        if (destroy) vals[1] = open_value;
        VkSemaphore waits[2];
        VkPipelineStageFlags st[2] = {VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT};
        uint32_t nw = 0;
        if (asem) waits[nw++] = asem;
        if (held) waits[nw++] = gate;
        VkTimelineSemaphoreSubmitInfo ts = {VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO, NULL, nw, asem ? vals : &vals[1], 0, NULL};
        VkCommandBuffer cb = cbs[k % fq];
        record(cb, imgs[idx], k, v11 && (block || destroy) && k == 2);
        VkFence f = ffence[k % fq];
        vkResetFences(dev, 1, &f);
        VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, held ? (const void *)&ts : NULL, nw, waits, st, 1, &cb, 0, NULL};
        r = vkQueueSubmit(queue, 1, &si, f);
        if (r) {
            if (!first_err) first_err = r, err_call = "vkQueueSubmit";
            errors++;
            if (!lost) printf("frame %d: submit %d\n", k, r);
            break;
        }
        if ((block || destroy) && held) {
            oth_on = !pthread_create(&oth, NULL, open_later, NULL);
        } else if (held) {
            pthread_mutex_lock(&gpu_lock);
            submitted[k] = now_ms();
            pthread_cond_signal(&gpu_cond);
            pthread_mutex_unlock(&gpu_lock);
        }
        VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, NULL, 0, NULL, 1, &sc, &idx, NULL};
        double p0 = now_ms();
        r = vkQueuePresentKHR(queue, &pi);
        double pw = now_ms() - p0;
        holding[idx] = 0;
        if (k >= warm) {
            acq_total += aw;
            if (aw > acq_max) acq_max = aw;
            pres_total += pw;
            if (pw > pres_max) pres_max = pw;
            counted++;
        }
        frames_done++;
        if (r < 0) {
            if (!first_err) first_err = r, err_call = "vkQueuePresentKHR";
            errors++;
            if (!lost) printf("frame %d: present %d\n", k, r);
            break;
        }
        if (block && k < 2) usleep(30 * 1000); /* frames 0 and 1 rendered and committed */
    }
    t_end = now_ms();
    double idle_cpu = 0;
    if (idle && !errors) {
        struct timespec c0, c1;
        clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &c0);
        usleep(2000 * 1000);
        clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &c1);
        idle_cpu = (c1.tv_sec - c0.tv_sec) * 1e3 + (c1.tv_nsec - c0.tv_nsec) / 1e6;
    }
    pthread_mutex_lock(&gpu_lock);
    gpu_quit = 1;
    pthread_cond_broadcast(&gpu_cond);
    pthread_mutex_unlock(&gpu_lock);
    if (gth_on) pthread_join(gth, NULL);
    if (!oth_on && (block || destroy) && gate) {
        VkSemaphoreSignalInfo ssi = {VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, NULL, gate, open_value};
        vkSignalSemaphore(dev, &ssi);
    }
    if (lat) {
        /* frames the emulated GPU never got to: let them through */
        VkSemaphoreSignalInfo ssi = {VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, NULL, gate, (uint64_t)frames + 1};
        vkSignalSemaphore(dev, &ssi);
    }
    double d0 = now_ms();
    if (!destroy) vkDeviceWaitIdle(dev);
    if (order) usleep(150 * 1000); /* the compositor's queue shows the last frames (its FIFO) before they go */
    vkDestroySwapchainKHR(dev, sc, NULL);
    double dms = now_ms() - d0;
    if (oth_on) pthread_join(oth, NULL);
    if (destroy) vkDeviceWaitIdle(dev);
    printf("swapchain destroyed in %.1f ms\n", dms);
    if (held_twice) {
        printf("FAILED: %d acquire(s) gave an image the app still held\n", held_twice);
        return 1;
    }
    if (lost) {
        if (!errors) {
            printf("FAILED: %d frames and no call answered an error (the connection or the device should be lost)\n", frames_done);
            return 1;
        }
        printf("OK: %s answered %d after %d frames; nothing hung, the swapchain destroyed in %.0f ms\n", err_call, first_err, frames_done, dms);
        return 0;
    }
    if (failed || errors) {
        printf("FAILED: an error on the way (%s %d, see above)\n", err_call, first_err);
        return 1;
    }
    if (block) {
        if (acq3 >= hold_ms / 2.0) {
            printf("FAILED: the 4th acquire waited %.0f ms: for frame 2's GPU work (held %d ms), not for the release of image 0 "
                   "(sent ~50 ms after frame 1's commit)\n", acq3, hold_ms);
            return 1;
        }
        printf("OK: the 4th acquire returned after %.0f ms, once the compositor released an image (frame 2 held %d ms on the GPU)\n",
               acq3, hold_ms);
        return 0;
    }
    if (destroy) {
        if (dms > hold_ms + 2000) {
            printf("FAILED: the destroy took %.0f ms\n", dms);
            return 1;
        }
        printf("OK: destroyed %.0f ms after presents with frame 2 held %d ms and releases pending\n", dms, hold_ms);
        return 0;
    }
    if (idle) {
        if (idle_cpu >= 40) {
            printf("FAILED: %.1f ms of CPU in 2 s with nothing presented\n", idle_cpu);
            return 1;
        }
        printf("OK: %.1f ms of CPU in 2 s with nothing presented (a live swapchain, %d frames before)\n", idle_cpu, frames_done);
        return 0;
    }
    if (recreate) {
        if (!out_of_date_ok) {
            printf("FAILED: the old swapchain's acquire did not answer VK_ERROR_OUT_OF_DATE_KHR\n");
            return 1;
        }
        printf("OK: %d frames across a swapchain made from the old one while releases were pending\n", frames_done);
        return 0;
    }
    double secs = (t_end - t_start) / 1e3;
    printf("game thread per frame: acquire %.3f ms (max %.1f), present %.3f ms (max %.1f), both %.3f ms; %d frames in %.2f s = %.1f fps made\n",
           acq_total / (counted ? counted : 1), acq_max, pres_total / (counted ? counted : 1), pres_max,
           (acq_total + pres_total) / (counted ? counted : 1), counted, secs, secs > 0 ? counted / secs : 0);
    if (import) {
        double avg = acq_total / (counted ? counted : 1);
        if (avg >= max_acq) {
            printf("FAILED: the acquire took %.2f ms per frame on the game's thread (limit %.2f): it waited for the server\n", avg, max_acq);
            return 1;
        }
        printf("OK: the acquire took %.2f ms per frame on the game's thread (limit %.2f)\n", avg, max_acq);
        return 0;
    }
    if (order) {
        printf("OK: %d frames presented%s\n", frames_done, acq_fence ? " (acquire fences)" : "");
        return 0;
    }
    printf("OK: paced run done\n");
    return 0;
}
