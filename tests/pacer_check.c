/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * pacer_check: the frame pacer (VKBRIDGE_PACER, client/pacer.c) against a game shaped like a DXVK game
 * (host/pacer_test.sh runs it under tests/fake_nested_wayland.py, dma-buf images from the server's memfd
 * stand-ins, so the swapchain's presenting thread runs as on a tablet).
 *
 * The app calls itself "DXVK" (VkApplicationInfo.pEngineName), so the bridge treats it as DXVK: present
 * wait is not offered unless VKBRIDGE_PACER=1 or VKBRIDGE_PRESENT_WAIT=1. The swapchain is MAILBOX: what the bridge
 * sees under gamescope, whose WSI layer sends MAILBOX to the driver and does FIFO itself while DXVK still believes
 * it presents FIFO (so DXVK waits for presents; the layer passes vkWaitForPresentKHR through). Each frame k:
 *   - DXVK's frame latency gate (SyncFrameLatency, maxFrameLatency --gate M, default 3): before frame k
 *     starts, frame k-M's GPU work is done (its fence: DXVK's finish thread) and, when present wait is
 *     offered, vkWaitForPresentKHR(id of frame k-M) returned (DXVK's dxvk-frame thread);
 *   - --cpu-ms of CPU work (a sleep: the game thread), acquire, one submit, present with present id k+1.
 * The GPU is emulated (present_path_check's lat mode): frame k's submit waits for a timeline value that a
 * thread signals --gpu-ms after the later of the submit and frame k-1's end (one queue, back to back).
 * Prints, over the last --measure frames: fps, present->GPU done (p50, in ms and in GPU frames), the
 * share of frames that reached the GPU after it went idle ("late") and whether present wait was offered.
 * Always "OK: ..." when the run completed (the shell judges the numbers); "FAILED" on errors, "SKIPPED"
 * without a compositor (exit 3).
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
static VkPhysicalDevice pd;
static VkSurfaceKHR surface;
static uint32_t family;
static VkCommandPool pool;
static VkExtent2D extent;
static VkSemaphore gate;
static int have_pw;

#define I(name) static PFN_##name name
I(vkCreateInstance); I(vkEnumeratePhysicalDevices); I(vkGetPhysicalDeviceQueueFamilyProperties); I(vkCreateDevice);
I(vkGetDeviceQueue); I(vkCreateCommandPool); I(vkAllocateCommandBuffers); I(vkBeginCommandBuffer); I(vkEndCommandBuffer);
I(vkResetCommandBuffer); I(vkCmdPipelineBarrier); I(vkCmdClearColorImage); I(vkQueueSubmit); I(vkCreateSemaphore);
I(vkSignalSemaphore); I(vkDeviceWaitIdle); I(vkGetPhysicalDeviceSurfaceSupportKHR); I(vkCreateSwapchainKHR);
I(vkGetSwapchainImagesKHR); I(vkAcquireNextImageKHR); I(vkQueuePresentKHR); I(vkCreateFence); I(vkWaitForFences);
I(vkResetFences); I(vkDestroySwapchainKHR); I(vkGetPhysicalDeviceSurfaceCapabilitiesKHR); I(vkGetPhysicalDeviceFeatures2);
I(vkEnumerateDeviceExtensionProperties); I(vkWaitForPresentKHR);
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

static int wayland_surface(void **display, void **surface_out)
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
    *surface_out = wl.marshal_constructor(wl.compositor, 0 /* create_surface */, wl.surface_iface, NULL);
    *display = d;
    return *surface_out != NULL;
}

/* ------------------------------------------------------------------ the emulated GPU */

static pthread_mutex_t gpu_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gpu_cond = PTHREAD_COND_INITIALIZER;
static double *submitted, *gpu_start, *gpu_end; /* frame -> ms, 0 = not yet */
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
        gpu_start[k] = start;
        gpu_end[k] = end;
    }
    return NULL;
}

/* ------------------------------------------------------------------ setup */

static int setup(const char *engine)
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
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "vkbridge-pacer-check", 1, engine, 1, VK_API_VERSION_1_2};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 2, iexts};
    if (vkCreateInstance(&ici, NULL, &inst)) {
        printf("FAILED: setup: vkCreateInstance\n");
        return 2;
    }
    G(vkEnumeratePhysicalDevices); G(vkGetPhysicalDeviceQueueFamilyProperties); G(vkCreateDevice); G(vkGetDeviceQueue);
    G(vkCreateCommandPool); G(vkAllocateCommandBuffers); G(vkBeginCommandBuffer); G(vkEndCommandBuffer); G(vkResetCommandBuffer);
    G(vkCmdPipelineBarrier); G(vkCmdClearColorImage); G(vkQueueSubmit); G(vkCreateSemaphore); G(vkSignalSemaphore);
    G(vkDeviceWaitIdle); G(vkGetPhysicalDeviceSurfaceSupportKHR); G(vkCreateSwapchainKHR); G(vkGetSwapchainImagesKHR);
    G(vkAcquireNextImageKHR); G(vkQueuePresentKHR); G(vkCreateFence); G(vkWaitForFences); G(vkResetFences);
    G(vkDestroySwapchainKHR); G(vkGetPhysicalDeviceSurfaceCapabilitiesKHR); G(vkGetPhysicalDeviceFeatures2);
    G(vkEnumerateDeviceExtensionProperties); G(vkWaitForPresentKHR);
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
    /* DXVK enables present_id + present_wait when the feature reads true (dxvk_adapter.cpp) */
    VkPhysicalDevicePresentWaitFeaturesKHR pwf = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR};
    VkPhysicalDevicePresentIdFeaturesKHR pif = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR, &pwf};
    VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &pif};
    vkGetPhysicalDeviceFeatures2(pd, &f2);
    uint32_t ne = 0;
    vkEnumerateDeviceExtensionProperties(pd, NULL, &ne, NULL);
    VkExtensionProperties *ep = calloc(ne ? ne : 1, sizeof(*ep));
    vkEnumerateDeviceExtensionProperties(pd, NULL, &ne, ep);
    int ext_id = 0, ext_wait = 0;
    for (uint32_t i = 0; i < ne; i++) {
        if (!strcmp(ep[i].extensionName, "VK_KHR_present_id")) ext_id = 1;
        if (!strcmp(ep[i].extensionName, "VK_KHR_present_wait")) ext_wait = 1;
    }
    free(ep);
    have_pw = ext_id && ext_wait && pif.presentId && pwf.presentWait && vkWaitForPresentKHR;
    float prio = 1;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, family, 1, &prio};
    VkPhysicalDevicePresentWaitFeaturesKHR pwe = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR, NULL, VK_TRUE};
    VkPhysicalDevicePresentIdFeaturesKHR pie = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR, &pwe, VK_TRUE};
    VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, have_pw ? (void *)&pie : NULL};
    f12.timelineSemaphore = VK_TRUE;
    const char *dexts[3] = {"VK_KHR_swapchain", "VK_KHR_present_id", "VK_KHR_present_wait"};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f12, 0, 1, &qci, 0, NULL, have_pw ? 3 : 1, dexts, NULL};
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
    VkSemaphoreTypeCreateInfo ti = {VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, NULL, VK_SEMAPHORE_TYPE_TIMELINE, 0};
    VkSemaphoreCreateInfo tci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &ti};
    if (vkCreateSemaphore(dev, &tci, NULL, &gate)) {
        printf("FAILED: setup: no timeline semaphore\n");
        return 2;
    }
    return 0;
}

static void record(VkCommandBuffer cb, VkImage img, int k)
{
    vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(cb, &bi);
    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, img, range};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    VkClearColorValue c = {.float32 = {(float)(k & 0xff) / 255.0f, 0.0f, (float)0x5a / 255.0f, 1.0f}};
    vkCmdClearColorImage(cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c, 1, &range);
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = 0;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    vkEndCommandBuffer(cb);
}

static int cmp_d(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

static double pct(double *v, int n, double p)
{
    if (n <= 0) return 0;
    qsort(v, (size_t)n, sizeof(double), cmp_d);
    int i = (int)(p * n);
    return v[i >= n ? n - 1 : i];
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    int frames = 900, measure = 300, M = 3, images = 5;
    double cpu_ms = 1;
    const char *engine = "DXVK";
    gpu_ms = 12;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--measure") && i + 1 < argc) measure = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--gate") && i + 1 < argc) M = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--images") && i + 1 < argc) images = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--cpu-ms") && i + 1 < argc) cpu_ms = atof(argv[++i]);
        else if (!strcmp(argv[i], "--gpu-ms") && i + 1 < argc) gpu_ms = atof(argv[++i]);
        else if (!strcmp(argv[i], "--engine") && i + 1 < argc) engine = argv[++i];
        else {
            printf("FAILED: unknown argument %s\n", argv[i]);
            return 2;
        }
    }
    if (M < 1) M = 1;
    if (M > 6) M = 6;
    if (measure > frames / 2) measure = frames / 2;
    int s = setup(engine);
    if (s) return s;
    VkSwapchainCreateInfoKHR sci = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, NULL, 0, surface, (uint32_t)images, VK_FORMAT_B8G8R8A8_UNORM,
                                    VK_COLOR_SPACE_SRGB_NONLINEAR_KHR, extent, 1, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                    VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
                                    VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_PRESENT_MODE_MAILBOX_KHR, VK_TRUE, VK_NULL_HANDLE};
    VkSwapchainKHR sc = VK_NULL_HANDLE;
    if (vkCreateSwapchainKHR(dev, &sci, NULL, &sc)) {
        printf("FAILED: setup: vkCreateSwapchainKHR\n");
        return 2;
    }
    VkImage imgs[16];
    uint32_t nimg = 16;
    vkGetSwapchainImagesKHR(dev, sc, &nimg, imgs);
    printf("engine %s, %u images, gate %d, gpu %.1f ms, cpu %.1f ms, present wait %s\n", engine, nimg, M, gpu_ms, cpu_ms,
           have_pw ? "offered (the gate also waits for it)" : "not offered (the gate is the frame's fence)");

    VkCommandBuffer cbs[8];
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 8};
    vkAllocateCommandBuffers(dev, &cai, cbs);
    VkFence ffence[8];
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, NULL, VK_FENCE_CREATE_SIGNALED_BIT};
    for (int i = 0; i < 8; i++) vkCreateFence(dev, &fci, NULL, &ffence[i]);
    VkSemaphore acq_sems[16];
    VkSemaphoreCreateInfo bci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (int i = 0; i < 16; i++) vkCreateSemaphore(dev, &bci, NULL, &acq_sems[i]);

    submitted = calloc((size_t)frames + 1, sizeof(double));
    gpu_start = calloc((size_t)frames + 1, sizeof(double));
    gpu_end = calloc((size_t)frames + 1, sizeof(double));
    double *presented = calloc((size_t)frames + 1, sizeof(double));
    gpu_frames = frames;
    pthread_t gth;
    int gth_on = !pthread_create(&gth, NULL, gpu_thread, NULL);
    int errors = 0;
    const char *err_call = "";
    VkResult first_err = VK_SUCCESS;
    for (int k = 0; k < frames; k++) {
        if (k >= M) {
            /* DXVK's frame latency gate: frame k-M's GPU work done (the finish thread) and, with present
             * wait, its present waited for (the dxvk-frame thread) */
            VkFence f = ffence[(k - M) % 8];
            VkResult r = vkWaitForFences(dev, 1, &f, VK_TRUE, 10000000000ull);
            if (!r && have_pw) r = vkWaitForPresentKHR(dev, sc, (uint64_t)(k - M) + 1, 10000000000ull);
            if (r) {
                first_err = r, err_call = "the frame gate";
                errors++;
                break;
            }
        }
        /* the game's CPU work for the frame (sampled input, simulation, recording) */
        if (cpu_ms > 0) sleep_until_ms(now_ms() + cpu_ms);
        uint32_t idx = 0;
        VkSemaphore asem = acq_sems[k % 16];
        VkResult r = vkAcquireNextImageKHR(dev, sc, 5000000000ull, asem, VK_NULL_HANDLE, &idx);
        if (r < 0 || r == VK_TIMEOUT) {
            first_err = r, err_call = "vkAcquireNextImageKHR";
            errors++;
            break;
        }
        uint64_t vals[2] = {0, (uint64_t)k + 1};
        VkSemaphore waits[2] = {asem, gate};
        VkPipelineStageFlags st[2] = {VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT};
        VkTimelineSemaphoreSubmitInfo ts = {VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO, NULL, 2, vals, 0, NULL};
        VkCommandBuffer cb = cbs[k % 8];
        VkFence f = ffence[k % 8];
        if (k >= 8) vkWaitForFences(dev, 1, &f, VK_TRUE, 10000000000ull); /* its command buffer is free */
        record(cb, imgs[idx], k);
        vkResetFences(dev, 1, &f);
        VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, &ts, 2, waits, st, 1, &cb, 0, NULL};
        r = vkQueueSubmit(queue, 1, &si, f);
        if (r) {
            first_err = r, err_call = "vkQueueSubmit";
            errors++;
            break;
        }
        pthread_mutex_lock(&gpu_lock);
        submitted[k] = now_ms();
        pthread_cond_signal(&gpu_cond);
        pthread_mutex_unlock(&gpu_lock);
        uint64_t id = (uint64_t)k + 1;
        VkPresentIdKHR pid = {VK_STRUCTURE_TYPE_PRESENT_ID_KHR, NULL, 1, &id};
        VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, have_pw ? (const void *)&pid : NULL, 0, NULL, 1, &sc, &idx, NULL};
        presented[k] = now_ms();
        r = vkQueuePresentKHR(queue, &pi);
        if (r < 0) {
            first_err = r, err_call = "vkQueuePresentKHR";
            errors++;
            break;
        }
    }
    pthread_mutex_lock(&gpu_lock);
    gpu_quit = 1;
    pthread_cond_broadcast(&gpu_cond);
    pthread_mutex_unlock(&gpu_lock);
    if (gth_on) pthread_join(gth, NULL);
    VkSemaphoreSignalInfo ssi = {VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, NULL, gate, (uint64_t)frames + 1};
    vkSignalSemaphore(dev, &ssi);
    vkDeviceWaitIdle(dev);
    vkDestroySwapchainKHR(dev, sc, NULL);
    if (errors) {
        printf("FAILED: %s answered %d\n", err_call, first_err);
        return 1;
    }
    /* the last `measure` frames whose GPU work finished */
    int last = frames - 1;
    while (last > 0 && !gpu_end[last]) last--;
    int first = last - measure + 1;
    if (first < 1) first = 1;
    int n = last - first + 1;
    if (n < 2) {
        printf("FAILED: only %d frames finished on the emulated GPU\n", n);
        return 1;
    }
    double *lat = calloc((size_t)n, sizeof(double));
    int late = 0;
    for (int k = first; k <= last; k++) {
        lat[k - first] = gpu_end[k] - presented[k];
        if (gpu_start[k] > gpu_end[k - 1] + 0.5) late++; /* the GPU waited for this frame */
    }
    double secs = (gpu_end[last] - gpu_end[first - 1]) / 1e3;
    double fps = secs > 0 ? n / secs : 0;
    double p50 = pct(lat, n, 0.5), p90 = pct(lat, n, 0.9);
    printf("RESULT fps %.2f present->done p50 %.1f ms p90 %.1f ms = %.2f frames; late %.1f%%; present wait %s\n", fps, p50, p90,
           p50 / gpu_ms, 100.0 * late / n, have_pw ? "on" : "off");
    printf("OK: %d frames, last %d measured\n", frames, n);
    return 0;
}
