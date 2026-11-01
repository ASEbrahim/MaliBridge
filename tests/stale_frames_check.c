/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * stale_frames_check: what the compositor receives is the frame the app presented, every time.
 *
 * Elden Ring on the S11 Ultra (2026-10-28, ELDENRING-STALE-FRAMES): with two swapchain images in
 * turn, one of them showed an older picture for runs of frames while the other moved on (a camera
 * pan going back and forth, a jump played again). The bridge committed each present once, in
 * order, after its fence; what was in the image at that moment was not checked. This check is
 * that: the program is its own Wayland compositor (libwayland-server, opened at run time, on a
 * socketpair), so it sees each wl_surface.commit and reads the committed dma-buf at once - the
 * earliest any compositor can read it - and again when it sends the buffer back
 * (wl_buffer.release), to catch a frame drawn into a buffer the compositor still holds.
 *
 * The app side is vkd3d-proton's swapchain, as far as the bridge can tell (vkd3d-proton
 * libs/vkd3d/swapchain.c: dxgi_vk_swap_chain_present_callback, run on the queue's submission
 * thread after the frame's own submits):
 *   - the frame: a slow GPU job (a random number of 64 MB buffer copies, 0-~30 ms on a desktop
 *     GPU), then the DXGI back buffer (one of 3, in turn) cleared to the frame's colour, then a
 *     barrier to ALL_COMMANDS as D3D12's transition to PRESENT gives;
 *   - acquire (MAILBOX, 3 images, a binary semaphore), the blit back buffer -> swapchain image with
 *     vkd3d's barriers (srcStage BLIT, no source access: the frame's barrier is relied on), a
 *     binary release semaphore, vkQueuePresentKHR waiting on it;
 *   - the CPU kept 2 frames ahead at most (a fence per back buffer), as vkd3d's frame latency does.
 * Colour of frame k: R = k & 255, G = k >> 8 & 255, B = 0x5a.
 *
 *   stale_frames_check [--frames N] [--hold-ms M] [--seed S] [--slow K]
 *     --hold-ms M  the compositor keeps a buffer M ms past the next commit before releasing it
 *                  (DroidDeck's paced release; default 0: released at the next commit)
 *     --slow K     at most K copies per frame (default 200)
 *     --implicit-sync  the compositor waits for the dma-buf's implicit fences before reading (what
 *                  desktop compositors do, and Mesa's own WSI needs: the native baseline). Off by
 *                  default, as on Android: DroidDeck's compositor reads what it is given.
 *
 * Exit 0 and "OK: ..." when every commit showed its own frame and every buffer still held it at
 * release; 1 and "FAILED: ..." with the first cases otherwise; 3 "SKIPPED" without
 * libwayland-server/-client. VK_DRIVER_FILES picks the driver (the bridge, or native for a
 * baseline).
 */
#define _GNU_SOURCE
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

/* ------------------------------------------------------------------ libwayland-server */

struct wl_message {
    const char *name;
    const char *signature;
    const struct wl_interface **types;
};
struct wl_interface {
    const char *name;
    int version;
    int method_count;
    const struct wl_message *methods;
    int event_count;
    const struct wl_message *events;
};
struct wl_resource;
struct wl_client;

static struct {
    void *(*display_create)(void);
    void *(*display_get_event_loop)(void *);
    int (*event_loop_dispatch)(void *, int);
    void (*display_flush_clients)(void *);
    struct wl_client *(*client_create)(void *, int);
    void *(*global_create)(void *, const struct wl_interface *, int, void *,
                           void (*)(struct wl_client *, void *, uint32_t, uint32_t));
    struct wl_resource *(*resource_create)(struct wl_client *, const struct wl_interface *, int, uint32_t);
    void (*resource_set_implementation)(struct wl_resource *, const void *, void *, void (*)(struct wl_resource *));
    void *(*resource_get_user_data)(struct wl_resource *);
    void (*resource_post_event)(struct wl_resource *, uint32_t, ...);
    void (*resource_destroy)(struct wl_resource *);
    int (*resource_get_version)(struct wl_resource *);
    const struct wl_interface *compositor_iface, *surface_iface, *buffer_iface, *callback_iface, *region_iface;
} ws;

/* zwp_linux_dmabuf_v1 and its params, as wayland-scanner emits them (the bridge client's own copy,
 * client/wsi_wayland.c, with the server library's wl_buffer). */
static const struct wl_interface params_iface;
static const struct wl_interface *dmabuf_types[8];
static const struct wl_message dmabuf_requests[] = {{"destroy", "", dmabuf_types + 0}, {"create_params", "n", dmabuf_types + 6}};
static const struct wl_message dmabuf_events[] = {{"format", "u", dmabuf_types + 0}, {"modifier", "3uuu", dmabuf_types + 0}};
static const struct wl_interface dmabuf_iface = {"zwp_linux_dmabuf_v1", 3, 2, dmabuf_requests, 2, dmabuf_events};
static const struct wl_message params_requests[] = {
    {"destroy", "", dmabuf_types + 0},
    {"add", "huuuuu", dmabuf_types + 0},
    {"create", "iiuu", dmabuf_types + 0},
    {"create_immed", "2niiuu", dmabuf_types + 7},
};
static const struct wl_message params_events[] = {{"created", "n", dmabuf_types + 7}, {"failed", "", dmabuf_types + 0}};
static const struct wl_interface params_iface = {"zwp_linux_buffer_params_v1", 3, 4, params_requests, 2, params_events};

/* ------------------------------------------------------------------ the compositor */

#define MAX_BUFS 16
#define MAX_CASES 12

typedef struct buf {
    struct wl_resource *res;
    int fd;
    uint32_t offset, stride, w, h;
    int held;            /* committed and not yet released */
    int frame;           /* the frame it showed at its commit */
    double release_at;   /* ms, 0: none due */
} buf;

static buf bufs[MAX_BUFS];
static void *g_display;
static volatile int g_quit;
static int hold_ms;
static struct wl_resource *g_attached;
static buf *g_current;
static pthread_mutex_t res_lock = PTHREAD_MUTEX_INITIALIZER;
static int commits, stale, future, garbage, overwritten, back_in_time, last_seen = -1;
static char cases[MAX_CASES][160];
static int ncases;

static void note(const char *fmt, int a, int b, int c)
{
    if (ncases < MAX_CASES) snprintf(cases[ncases++], sizeof(cases[0]), fmt, a, b, c);
}

/* The compositor's own GPU (the desktop driver directly, not the bridge: DroidDeck's compositor is a
 * native Vulkan device beside the server's): each committed dma-buf imported once as a buffer, and
 * the centre pixel copied out of it at each read, as DroidDeck's compositor copies the frame. */
static struct {
    VkDevice dev;
    VkQueue q;
    VkCommandPool pool;
    VkCommandBuffer cb;
    VkFence fence;
    VkBuffer out;
    VkDeviceMemory out_mem;
    uint8_t *out_ptr;
    VkPhysicalDeviceMemoryProperties mp;
    PFN_vkGetDeviceProcAddr gdpa;
#define C(n) PFN_##n n
    C(vkCreateBuffer); C(vkGetBufferMemoryRequirements); C(vkAllocateMemory); C(vkBindBufferMemory); C(vkMapMemory);
    C(vkGetMemoryFdPropertiesKHR); C(vkBeginCommandBuffer); C(vkEndCommandBuffer); C(vkCmdCopyBuffer); C(vkQueueSubmit);
    C(vkWaitForFences); C(vkResetFences); C(vkResetCommandBuffer); C(vkCmdPipelineBarrier); C(vkDestroyBuffer); C(vkFreeMemory);
#undef C
} cv;

static uint32_t cv_type(uint32_t bits, VkMemoryPropertyFlags want)
{
    for (uint32_t t = 0; t < cv.mp.memoryTypeCount; t++)
        if ((bits >> t & 1) && (cv.mp.memoryTypes[t].propertyFlags & want) == want) return t;
    return UINT32_MAX;
}

static int cv_init(void)
{
    const char *icd = getenv("STALE_COMPOSITOR_ICD");
    void *h = dlopen(icd && *icd ? icd : "libvulkan_radeon.so", RTLD_NOW | RTLD_LOCAL);
    if (!h) return 0;
    PFN_vkGetInstanceProcAddr ig = (PFN_vkGetInstanceProcAddr)dlsym(h, "vk_icdGetInstanceProcAddr");
    VkResult (*neg)(uint32_t *) = (VkResult (*)(uint32_t *))dlsym(h, "vk_icdNegotiateLoaderICDInterfaceVersion");
    if (!ig) return 0;
    uint32_t v = 5;
    if (neg) neg(&v);
    PFN_vkCreateInstance ci = (PFN_vkCreateInstance)ig(NULL, "vkCreateInstance");
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "stale-frames compositor", 1, NULL, 0, VK_API_VERSION_1_1};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 0, NULL};
    VkInstance in;
    if (!ci || ci(&ici, NULL, &in)) return 0;
    PFN_vkEnumeratePhysicalDevices epd = (PFN_vkEnumeratePhysicalDevices)ig(in, "vkEnumeratePhysicalDevices");
    PFN_vkGetPhysicalDeviceProperties gpp = (PFN_vkGetPhysicalDeviceProperties)ig(in, "vkGetPhysicalDeviceProperties");
    PFN_vkGetPhysicalDeviceMemoryProperties gmp = (PFN_vkGetPhysicalDeviceMemoryProperties)ig(in, "vkGetPhysicalDeviceMemoryProperties");
    PFN_vkCreateDevice cd = (PFN_vkCreateDevice)ig(in, "vkCreateDevice");
    cv.gdpa = (PFN_vkGetDeviceProcAddr)ig(in, "vkGetDeviceProcAddr");
    VkPhysicalDevice pds[8];
    uint32_t n = 8;
    epd(in, &n, pds);
    if (!n) return 0;
    VkPhysicalDevice pd = pds[0];
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties pp;
        gpp(pds[i], &pp);
        if (pp.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            pd = pds[i];
            break;
        }
    }
    gmp(pd, &cv.mp);
    float prio = 1;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, 0, 1, &prio};
    const char *exts[] = {"VK_KHR_external_memory_fd", "VK_EXT_external_memory_dma_buf"};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &qci, 0, NULL, 2, exts, NULL};
    if (cd(pd, &dci, NULL, &cv.dev)) return 0;
#define D(n) cv.n = (PFN_##n)cv.gdpa(cv.dev, #n)
    D(vkCreateBuffer); D(vkGetBufferMemoryRequirements); D(vkAllocateMemory); D(vkBindBufferMemory); D(vkMapMemory);
    D(vkGetMemoryFdPropertiesKHR); D(vkBeginCommandBuffer); D(vkEndCommandBuffer); D(vkCmdCopyBuffer); D(vkQueueSubmit);
    D(vkWaitForFences); D(vkResetFences); D(vkResetCommandBuffer); D(vkCmdPipelineBarrier); D(vkDestroyBuffer); D(vkFreeMemory);
#undef D
    ((PFN_vkGetDeviceQueue)cv.gdpa(cv.dev, "vkGetDeviceQueue"))(cv.dev, 0, 0, &cv.q);
    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, 0};
    ((PFN_vkCreateCommandPool)cv.gdpa(cv.dev, "vkCreateCommandPool"))(cv.dev, &pci, NULL, &cv.pool);
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, cv.pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    ((PFN_vkAllocateCommandBuffers)cv.gdpa(cv.dev, "vkAllocateCommandBuffers"))(cv.dev, &cai, &cv.cb);
    /* Mesa's dispatchable objects carry the loader's magic slot: without a loader, set nothing */
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    ((PFN_vkCreateFence)cv.gdpa(cv.dev, "vkCreateFence"))(cv.dev, &fci, NULL, &cv.fence);
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, 4096, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE};
    cv.vkCreateBuffer(cv.dev, &bci, NULL, &cv.out);
    VkMemoryRequirements req;
    cv.vkGetBufferMemoryRequirements(cv.dev, cv.out, &req);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size,
                               cv_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    if (ai.memoryTypeIndex == UINT32_MAX || cv.vkAllocateMemory(cv.dev, &ai, NULL, &cv.out_mem)) return 0;
    cv.vkBindBufferMemory(cv.dev, cv.out, cv.out_mem, 0);
    cv.vkMapMemory(cv.dev, cv.out_mem, 0, VK_WHOLE_SIZE, 0, (void **)&cv.out_ptr);
    return 1;
}

typedef struct import {
    VkBuffer b;
    VkDeviceMemory m;
} import;
static import imports[MAX_BUFS];

/* The frame number in the buffer's centre pixel (BGRA in memory), -1 if it is no frame colour,
 * -2 if it could not be read. */
static int implicit_sync;
static int read_frame(buf *b)
{
    if (implicit_sync) {
        /* a compositor that honours the dma-buf's implicit fences (Mesa's own WSI relies on them) */
        struct pollfd pf = {b->fd, POLLIN, 0};
        poll(&pf, 1, 1000);
    }
    import *im = &imports[b - bufs];
    if (!im->b) {
        off_t size = lseek(b->fd, 0, SEEK_END);
        if (size <= 0) return -2;
        VkExternalMemoryBufferCreateInfo eb = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO, NULL,
                                               VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
        VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &eb, 0, (VkDeviceSize)size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                  VK_SHARING_MODE_EXCLUSIVE};
        if (cv.vkCreateBuffer(cv.dev, &bci, NULL, &im->b)) return -2;
        VkMemoryRequirements req;
        cv.vkGetBufferMemoryRequirements(cv.dev, im->b, &req);
        VkMemoryFdPropertiesKHR fp = {VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
        cv.vkGetMemoryFdPropertiesKHR(cv.dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, b->fd, &fp);
        uint32_t bits = req.memoryTypeBits & (fp.memoryTypeBits ? fp.memoryTypeBits : ~0u);
        VkImportMemoryFdInfoKHR imp = {VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR, NULL, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
                                       dup(b->fd)};
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &imp, (VkDeviceSize)size, cv_type(bits, 0)};
        if (ai.memoryTypeIndex == UINT32_MAX || cv.vkAllocateMemory(cv.dev, &ai, NULL, &im->m)) {
            static int said;
            if (!said++) printf("compositor: importing the %lld-byte dma-buf failed\n", (long long)size);
            close(imp.fd);
            cv.vkDestroyBuffer(cv.dev, im->b, NULL);
            im->b = VK_NULL_HANDLE;
            return -2;
        }
        cv.vkBindBufferMemory(cv.dev, im->b, im->m, 0);
    }
    cv.vkResetCommandBuffer(cv.cb, 0);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    cv.vkBeginCommandBuffer(cv.cb, &bi);
    VkBufferCopy r = {(VkDeviceSize)b->offset + (VkDeviceSize)(b->h / 2) * b->stride + (VkDeviceSize)(b->w / 2) * 4, 0, 4};
    cv.vkCmdCopyBuffer(cv.cb, im->b, cv.out, 1, &r);
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
    cv.vkCmdPipelineBarrier(cv.cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    cv.vkEndCommandBuffer(cv.cb);
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cv.cb, 0, NULL};
    if (cv.vkQueueSubmit(cv.q, 1, &si, cv.fence)) return -2;
    cv.vkWaitForFences(cv.dev, 1, &cv.fence, VK_TRUE, UINT64_MAX);
    cv.vkResetFences(cv.dev, 1, &cv.fence);
    const uint8_t *p = cv.out_ptr;
    if (p[0] != 0x5a) return -1;
    return p[2] | p[1] << 8;
}

static void release(buf *b)
{
    int f = read_frame(b);
    if (f != b->frame) {
        overwritten++;
        note("buffer %d drawn into while the compositor held it: showed frame %d at its commit, %d at its release",
             (int)(b - bufs), b->frame, f);
    }
    b->held = 0;
    b->release_at = 0;
    ws.resource_post_event(b->res, 0 /* wl_buffer.release */);
}

static void buffer_destroy_req(struct wl_client *c, struct wl_resource *r) { (void)c; ws.resource_destroy(r); }
static const void *buffer_impl[] = {(void *)buffer_destroy_req};
static void buffer_gone(struct wl_resource *r)
{
    buf *b = ws.resource_get_user_data(r);
    import *im = &imports[b - bufs];
    if (im->b) cv.vkDestroyBuffer(cv.dev, im->b, NULL);
    if (im->m) cv.vkFreeMemory(cv.dev, im->m, NULL);
    im->b = VK_NULL_HANDLE;
    im->m = VK_NULL_HANDLE;
    close(b->fd);
    if (g_current == b) g_current = NULL;
    memset(b, 0, sizeof(*b));
    b->fd = -1;
}

typedef struct params {
    int fd;
    uint32_t offset, stride;
} params;

static void params_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; ws.resource_destroy(r); }
static void params_add(struct wl_client *c, struct wl_resource *r, int32_t fd, uint32_t plane, uint32_t offset, uint32_t stride,
                       uint32_t hi, uint32_t lo)
{
    (void)c, (void)plane, (void)hi, (void)lo;
    params *p = ws.resource_get_user_data(r);
    if (p->fd >= 0) close(p->fd);
    p->fd = fd;
    p->offset = offset;
    p->stride = stride;
}
static void params_create(struct wl_client *c, struct wl_resource *r, int32_t w, int32_t h, uint32_t f, uint32_t fl)
{
    (void)c, (void)w, (void)h, (void)f, (void)fl;
    ws.resource_post_event(r, 1 /* failed: only create_immed here */);
}
static void params_create_immed(struct wl_client *c, struct wl_resource *r, uint32_t id, int32_t w, int32_t h, uint32_t f, uint32_t fl)
{
    (void)f, (void)fl;
    params *p = ws.resource_get_user_data(r);
    buf *b = NULL;
    for (int i = 0; i < MAX_BUFS && !b; i++)
        if (!bufs[i].res) b = &bufs[i];
    struct wl_resource *br = ws.resource_create(c, ws.buffer_iface, 1, id);
    if (!b || !br) return;
    b->res = br;
    b->fd = p->fd;
    p->fd = -1;
    b->offset = p->offset;
    b->stride = p->stride;
    b->w = (uint32_t)w;
    b->h = (uint32_t)h;
    b->frame = -1;
    ws.resource_set_implementation(br, buffer_impl, b, buffer_gone);
}
static const void *params_impl[] = {(void *)params_destroy, (void *)params_add, (void *)params_create, (void *)params_create_immed};
static void params_gone(struct wl_resource *r)
{
    params *p = ws.resource_get_user_data(r);
    if (p->fd >= 0) close(p->fd);
    free(p);
}

static void dmabuf_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; ws.resource_destroy(r); }
static void dmabuf_create_params(struct wl_client *c, struct wl_resource *r, uint32_t id)
{
    params *p = calloc(1, sizeof(*p));
    p->fd = -1;
    struct wl_resource *pr = ws.resource_create(c, &params_iface, ws.resource_get_version(r), id);
    ws.resource_set_implementation(pr, params_impl, p, params_gone);
}
static const void *dmabuf_impl[] = {(void *)dmabuf_destroy, (void *)dmabuf_create_params};
static void dmabuf_bind(struct wl_client *c, void *data, uint32_t version, uint32_t id)
{
    (void)data;
    struct wl_resource *r = ws.resource_create(c, &dmabuf_iface, (int)version, id);
    ws.resource_set_implementation(r, dmabuf_impl, NULL, NULL);
    static const uint32_t fmts[] = {0x34325258 /* XR24 */, 0x34325241 /* AR24 */, 0x34324258 /* XB24 */, 0x34324241 /* AB24 */};
    for (int i = 0; i < 4; i++) {
        ws.resource_post_event(r, 0, fmts[i]);
        if (version >= 3) ws.resource_post_event(r, 1, fmts[i], 0u, 0u); /* DRM_FORMAT_MOD_LINEAR */
    }
}

static void surface_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; ws.resource_destroy(r); }
static void surface_attach(struct wl_client *c, struct wl_resource *r, struct wl_resource *b, int32_t x, int32_t y)
{
    (void)c, (void)r, (void)x, (void)y;
    g_attached = b;
}
static void surface_damage(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y, int32_t w, int32_t h)
{
    (void)c, (void)r, (void)x, (void)y, (void)w, (void)h;
}
static void surface_frame(struct wl_client *c, struct wl_resource *r, uint32_t id)
{
    struct wl_resource *cb = ws.resource_create(c, ws.callback_iface, 1, id);
    (void)r;
    ws.resource_post_event(cb, 0, (uint32_t)now_ms());
    ws.resource_destroy(cb);
}
static void surface_region(struct wl_client *c, struct wl_resource *r, struct wl_resource *g) { (void)c, (void)r, (void)g; }
static void surface_commit(struct wl_client *c, struct wl_resource *r)
{
    (void)c, (void)r;
    if (!g_attached) return;
    buf *b = ws.resource_get_user_data(g_attached);
    g_attached = NULL;
    int k = commits++;
    int f = read_frame(b);
    b->frame = f;
    if (f < 0) {
        garbage++;
        note("commit %d: buffer %d holds no frame colour (%d)", k, (int)(b - bufs), f);
    } else if (f < k) {
        stale++;
        note("commit %d: buffer %d shows the older frame %d", k, (int)(b - bufs), f);
    } else if (f > k) {
        future++;
        note("commit %d: buffer %d shows the later frame %d", k, (int)(b - bufs), f);
    }
    if (f >= 0 && f < last_seen) back_in_time++;
    if (f >= 0) last_seen = f;
    if (g_current && g_current != b && g_current->held) g_current->release_at = now_ms() + hold_ms;
    b->held = 1;
    b->release_at = 0;
    g_current = b;
}
static void surface_int(struct wl_client *c, struct wl_resource *r, int32_t v) { (void)c, (void)r, (void)v; }
static const void *surface_impl[] = {(void *)surface_destroy, (void *)surface_attach, (void *)surface_damage, (void *)surface_frame,
                                     (void *)surface_region,  (void *)surface_region, (void *)surface_commit, (void *)surface_int,
                                     (void *)surface_int,     (void *)surface_damage, (void *)surface_damage};

static void region_destroy(struct wl_client *c, struct wl_resource *r) { (void)c; ws.resource_destroy(r); }
static const void *region_impl[] = {(void *)region_destroy, (void *)surface_damage, (void *)surface_damage};

static void comp_create_surface(struct wl_client *c, struct wl_resource *r, uint32_t id)
{
    struct wl_resource *s = ws.resource_create(c, ws.surface_iface, ws.resource_get_version(r), id);
    ws.resource_set_implementation(s, surface_impl, NULL, NULL);
}
static void comp_create_region(struct wl_client *c, struct wl_resource *r, uint32_t id)
{
    struct wl_resource *s = ws.resource_create(c, ws.region_iface, 1, id);
    (void)r;
    ws.resource_set_implementation(s, region_impl, NULL, NULL);
}
static const void *comp_impl[] = {(void *)comp_create_surface, (void *)comp_create_region};
static void comp_bind(struct wl_client *c, void *data, uint32_t version, uint32_t id)
{
    (void)data;
    struct wl_resource *r = ws.resource_create(c, ws.compositor_iface, (int)version, id);
    ws.resource_set_implementation(r, comp_impl, NULL, NULL);
}

static void *compositor_thread(void *arg)
{
    void *loop = arg;
    while (!g_quit) {
        ws.event_loop_dispatch(loop, 1);
        double t = now_ms();
        for (int i = 0; i < MAX_BUFS; i++)
            if (bufs[i].res && bufs[i].held && bufs[i].release_at && bufs[i].release_at <= t) release(&bufs[i]);
        ws.display_flush_clients(g_display);
    }
    return NULL;
}

/* A compositor on one end of a socketpair; the other end's fd, -1 without libwayland-server. */
static int compositor_start(pthread_t *th)
{
    void *l = dlopen("libwayland-server.so.0", RTLD_NOW | RTLD_LOCAL);
    if (!l) return -1;
#define L(f, n) (*(void **)&ws.f = dlsym(l, n))
    int ok = 1;
    ok &= !!L(display_create, "wl_display_create");
    ok &= !!L(display_get_event_loop, "wl_display_get_event_loop");
    ok &= !!L(event_loop_dispatch, "wl_event_loop_dispatch");
    ok &= !!L(display_flush_clients, "wl_display_flush_clients");
    ok &= !!L(client_create, "wl_client_create");
    ok &= !!L(global_create, "wl_global_create");
    ok &= !!L(resource_create, "wl_resource_create");
    ok &= !!L(resource_set_implementation, "wl_resource_set_implementation");
    ok &= !!L(resource_get_user_data, "wl_resource_get_user_data");
    ok &= !!L(resource_post_event, "wl_resource_post_event");
    ok &= !!L(resource_destroy, "wl_resource_destroy");
    ok &= !!L(resource_get_version, "wl_resource_get_version");
    ok &= !!L(compositor_iface, "wl_compositor_interface");
    ok &= !!L(surface_iface, "wl_surface_interface");
    ok &= !!L(buffer_iface, "wl_buffer_interface");
    ok &= !!L(callback_iface, "wl_callback_interface");
    ok &= !!L(region_iface, "wl_region_interface");
#undef L
    if (!ok || !cv_init()) return -1;
    dmabuf_types[6] = &params_iface;
    dmabuf_types[7] = ws.buffer_iface;
    for (int i = 0; i < MAX_BUFS; i++) bufs[i].fd = -1;
    g_display = ws.display_create();
    if (!g_display) return -1;
    ws.global_create(g_display, ws.compositor_iface, 4, NULL, comp_bind);
    ws.global_create(g_display, &dmabuf_iface, 3, NULL, dmabuf_bind);
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) != 0) return -1;
    if (!ws.client_create(g_display, sv[0])) return -1;
    if (pthread_create(th, NULL, compositor_thread, ws.display_get_event_loop(g_display)) != 0) return -1;
    return sv[1];
}

/* ------------------------------------------------------------------ the app's Wayland surface */

static struct {
    void *(*connect_to_fd)(int);
    int (*roundtrip)(void *);
    void *(*marshal_constructor)(void *, uint32_t, const void *, ...);
    void *(*marshal_constructor_versioned)(void *, uint32_t, const void *, uint32_t, ...);
    int (*add_listener)(void *, void (**)(void), void *);
    const void *registry_iface, *compositor_iface, *surface_iface;
    void *compositor;
} wc;

static void reg_global(void *data, void *registry, uint32_t name, const char *iface, uint32_t version)
{
    (void)data, (void)version;
    if (!strcmp(iface, "wl_compositor") && !wc.compositor)
        wc.compositor = wc.marshal_constructor_versioned(registry, 0 /* bind */, wc.compositor_iface, 4, name, "wl_compositor", 4u, NULL);
}
static void reg_remove(void *data, void *registry, uint32_t name) { (void)data, (void)registry, (void)name; }
static void (*reg_listener[2])(void) = {(void (*)(void))reg_global, (void (*)(void))reg_remove};

static int app_surface(int fd, void **display, void **surface)
{
    void *l = dlopen("libwayland-client.so.0", RTLD_NOW);
    if (!l) return 0;
    wc.connect_to_fd = dlsym(l, "wl_display_connect_to_fd");
    wc.roundtrip = dlsym(l, "wl_display_roundtrip");
    wc.marshal_constructor = dlsym(l, "wl_proxy_marshal_constructor");
    wc.marshal_constructor_versioned = dlsym(l, "wl_proxy_marshal_constructor_versioned");
    wc.add_listener = dlsym(l, "wl_proxy_add_listener");
    wc.registry_iface = dlsym(l, "wl_registry_interface");
    wc.compositor_iface = dlsym(l, "wl_compositor_interface");
    wc.surface_iface = dlsym(l, "wl_surface_interface");
    if (!wc.connect_to_fd || !wc.roundtrip || !wc.marshal_constructor || !wc.marshal_constructor_versioned || !wc.add_listener ||
        !wc.registry_iface || !wc.compositor_iface || !wc.surface_iface)
        return 0;
    void *d = wc.connect_to_fd(fd);
    if (!d) return 0;
    void *reg = wc.marshal_constructor(d, 1 /* wl_display.get_registry */, wc.registry_iface, NULL);
    wc.add_listener(reg, reg_listener, NULL);
    wc.roundtrip(d);
    if (!wc.compositor) return 0;
    *surface = wc.marshal_constructor(wc.compositor, 0 /* create_surface */, wc.surface_iface, NULL);
    *display = d;
    return *surface != NULL;
}

/* ------------------------------------------------------------------ Vulkan */

static PFN_vkGetInstanceProcAddr gipa;
static VkInstance inst;
static VkDevice dev;
#define I(name) static PFN_##name name
I(vkCreateInstance); I(vkEnumeratePhysicalDevices); I(vkGetPhysicalDeviceQueueFamilyProperties); I(vkCreateDevice);
I(vkGetDeviceQueue); I(vkCreateCommandPool); I(vkAllocateCommandBuffers); I(vkBeginCommandBuffer); I(vkEndCommandBuffer);
I(vkResetCommandBuffer); I(vkCmdPipelineBarrier); I(vkCmdCopyBuffer); I(vkCmdClearColorImage); I(vkCmdBlitImage);
I(vkQueueSubmit); I(vkCreateSemaphore); I(vkDeviceWaitIdle); I(vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
I(vkGetPhysicalDeviceSurfacePresentModesKHR); I(vkGetPhysicalDeviceSurfaceSupportKHR); I(vkCreateSwapchainKHR);
I(vkGetSwapchainImagesKHR); I(vkAcquireNextImageKHR); I(vkQueuePresentKHR); I(vkCreateFence); I(vkWaitForFences);
I(vkResetFences); I(vkCreateBuffer); I(vkCreateImage); I(vkGetBufferMemoryRequirements); I(vkGetImageMemoryRequirements);
I(vkAllocateMemory); I(vkBindBufferMemory); I(vkBindImageMemory); I(vkGetPhysicalDeviceMemoryProperties);
I(vkDestroySwapchainKHR);
#undef I

typedef struct {
    VkStructureType sType;
    const void *pNext;
    VkFlags flags;
    void *display;
    void *surface;
} wl_surface_ci;
typedef VkResult (VKAPI_PTR *pfn_create_wl_surface)(VkInstance, const wl_surface_ci *, const VkAllocationCallbacks *, VkSurfaceKHR *);

static VkPhysicalDeviceMemoryProperties memprops;
static VkDeviceMemory alloc(VkMemoryRequirements req)
{
    uint32_t t = 0;
    for (; t < memprops.memoryTypeCount; t++)
        if ((req.memoryTypeBits >> t & 1) && (memprops.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) break;
    if (t == memprops.memoryTypeCount)
        for (t = 0; t < memprops.memoryTypeCount && !(req.memoryTypeBits >> t & 1); t++) {}
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size, t};
    VkDeviceMemory m = VK_NULL_HANDLE;
    vkAllocateMemory(dev, &ai, NULL, &m);
    return m;
}

#define NBACK 3     /* DXGI back buffers */
#define LATENCY 2   /* frames the CPU may run ahead */
#define W 256
#define H 256
#define BIG (64u << 20)

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    int frames = 400, slow = 200;
    unsigned seed = 1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--hold-ms") && i + 1 < argc) hold_ms = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc) seed = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--slow") && i + 1 < argc) slow = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--implicit-sync")) implicit_sync = 1;
    }
    srand(seed);
    if (frames > 65535) frames = 65535;
    pthread_t cth;
    int cfd = compositor_start(&cth);
    void *wdisplay = NULL, *wsurface = NULL;
    if (cfd < 0 || !app_surface(cfd, &wdisplay, &wsurface)) {
        printf("SKIPPED: no libwayland-server/-client, or no native driver for the compositor (STALE_COMPOSITOR_ICD)\n");
        return 3;
    }

    void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib) {
        printf("FAILED: loader: %s\n", dlerror());
        return 2;
    }
    gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
#define G(n) n = (PFN_##n)gipa(inst, #n)
    G(vkCreateInstance);
    const char *iexts[2] = {"VK_KHR_surface", "VK_KHR_wayland_surface"};
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "vkbridge-stale-frames-check", 1, "vkd3d", 1, VK_API_VERSION_1_2};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 2, iexts};
    if (vkCreateInstance(&ici, NULL, &inst)) {
        printf("FAILED: setup: vkCreateInstance\n");
        return 2;
    }
    G(vkEnumeratePhysicalDevices); G(vkGetPhysicalDeviceQueueFamilyProperties); G(vkCreateDevice); G(vkGetDeviceQueue);
    G(vkCreateCommandPool); G(vkAllocateCommandBuffers); G(vkBeginCommandBuffer); G(vkEndCommandBuffer); G(vkResetCommandBuffer);
    G(vkCmdPipelineBarrier); G(vkCmdCopyBuffer); G(vkCmdClearColorImage); G(vkCmdBlitImage); G(vkQueueSubmit);
    G(vkCreateSemaphore); G(vkDeviceWaitIdle); G(vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
    G(vkGetPhysicalDeviceSurfacePresentModesKHR); G(vkGetPhysicalDeviceSurfaceSupportKHR); G(vkCreateSwapchainKHR);
    G(vkGetSwapchainImagesKHR); G(vkAcquireNextImageKHR); G(vkQueuePresentKHR); G(vkCreateFence); G(vkWaitForFences);
    G(vkResetFences); G(vkCreateBuffer); G(vkCreateImage); G(vkGetBufferMemoryRequirements); G(vkGetImageMemoryRequirements);
    G(vkAllocateMemory); G(vkBindBufferMemory); G(vkBindImageMemory); G(vkGetPhysicalDeviceMemoryProperties);
    G(vkDestroySwapchainKHR);
#undef G
    uint32_t n = 1;
    VkPhysicalDevice pd;
    vkEnumeratePhysicalDevices(inst, &n, &pd);
    if (!n) {
        printf("FAILED: setup: no GPU\n");
        return 2;
    }
    vkGetPhysicalDeviceMemoryProperties(pd, &memprops);
    pfn_create_wl_surface create = (pfn_create_wl_surface)gipa(inst, "vkCreateWaylandSurfaceKHR");
    wl_surface_ci wci = {VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR, NULL, 0, wdisplay, wsurface};
    VkSurfaceKHR surface;
    if (!create || create(inst, &wci, NULL, &surface)) {
        printf("FAILED: setup: vkCreateWaylandSurfaceKHR\n");
        return 2;
    }
    uint32_t nq = 16, family = UINT32_MAX;
    VkQueueFamilyProperties qp[16];
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qp);
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
    const char *dexts[1] = {"VK_KHR_swapchain"};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &qci, 0, NULL, 1, dexts, NULL};
    if (vkCreateDevice(pd, &dci, NULL, &dev)) {
        printf("FAILED: setup: vkCreateDevice\n");
        return 2;
    }
    VkQueue queue;
    vkGetDeviceQueue(dev, family, 0, &queue);

    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surface, &caps);
    VkPresentModeKHR modes[8], mode = VK_PRESENT_MODE_FIFO_KHR;
    uint32_t nm = 8;
    vkGetPhysicalDeviceSurfacePresentModesKHR(pd, surface, &nm, modes);
    for (uint32_t i = 0; i < nm; i++)
        if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR) mode = modes[i];
    uint32_t want = caps.minImageCount > 3 ? caps.minImageCount : 3;
    VkSwapchainCreateInfoKHR sci = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, NULL, 0, surface, want, VK_FORMAT_B8G8R8A8_UNORM,
                                    VK_COLOR_SPACE_SRGB_NONLINEAR_KHR, {W, H}, 1,
                                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0,
                                    NULL, VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR, VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, mode, VK_TRUE,
                                    VK_NULL_HANDLE};
    VkSwapchainKHR sc;
    if (vkCreateSwapchainKHR(dev, &sci, NULL, &sc)) {
        printf("FAILED: setup: vkCreateSwapchainKHR\n");
        return 2;
    }
    uint32_t nimg = 8;
    VkImage simg[8];
    vkGetSwapchainImagesKHR(dev, sc, &nimg, simg);

    /* the slow job's buffers and the DXGI back buffers */
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, BIG,
                              VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE};
    VkBuffer big[2];
    for (int i = 0; i < 2; i++) {
        VkMemoryRequirements req;
        vkCreateBuffer(dev, &bci, NULL, &big[i]);
        vkGetBufferMemoryRequirements(dev, big[i], &req);
        vkBindBufferMemory(dev, big[i], alloc(req), 0);
    }
    VkImageCreateInfo imci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, VK_FORMAT_B8G8R8A8_UNORM, {W, H, 1}, 1, 1,
                              VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL,
                              VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                              VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
    VkImage back[NBACK];
    for (int i = 0; i < NBACK; i++) {
        VkMemoryRequirements req;
        vkCreateImage(dev, &imci, NULL, &back[i]);
        vkGetImageMemoryRequirements(dev, back[i], &req);
        vkBindImageMemory(dev, back[i], alloc(req), 0);
    }

    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, family};
    VkCommandPool pool;
    vkCreateCommandPool(dev, &pci, NULL, &pool);
    VkCommandBuffer game_cb[LATENCY], blit_cb[8];
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, LATENCY};
    vkAllocateCommandBuffers(dev, &cai, game_cb);
    cai.commandBufferCount = nimg;
    vkAllocateCommandBuffers(dev, &cai, blit_cb);
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, NULL, VK_FENCE_CREATE_SIGNALED_BIT};
    VkFence game_fence[LATENCY], blit_fence[8];
    for (int i = 0; i < LATENCY; i++) vkCreateFence(dev, &fci, NULL, &game_fence[i]);
    for (uint32_t i = 0; i < nimg; i++) vkCreateFence(dev, &fci, NULL, &blit_fence[i]);
    VkSemaphoreCreateInfo semci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkSemaphore acq[8], rel[8];
    for (uint32_t i = 0; i < 8; i++) {
        vkCreateSemaphore(dev, &semci, NULL, &acq[i]);
        vkCreateSemaphore(dev, &semci, NULL, &rel[i]);
    }
    printf("%u swapchain images, present mode %d, %d frames, up to %d x 64 MB copies before each, compositor hold %d ms\n", nimg, mode,
           frames, slow, hold_ms);

    const VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    const VkImageSubresourceLayers layers = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    int back_layout_ok[NBACK] = {0};
    int failed = 0;
    double t0 = now_ms();
    for (int k = 0; k < frames && !failed; k++) {
        /* the game's frame */
        int slot = k % LATENCY, bb = k % NBACK;
        vkWaitForFences(dev, 1, &game_fence[slot], VK_TRUE, UINT64_MAX);
        vkResetFences(dev, 1, &game_fence[slot]);
        VkCommandBuffer cb = game_cb[slot];
        vkResetCommandBuffer(cb, 0);
        VkCommandBufferBeginInfo cbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        vkBeginCommandBuffer(cb, &cbi);
        int copies = slow > 0 ? rand() % (slow + 1) : 0;
        VkBufferCopy bcopy = {0, 0, BIG};
        VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT,
                              VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT};
        for (int c = 0; c < copies; c++) {
            vkCmdCopyBuffer(cb, big[c & 1], big[!(c & 1)], 1, &bcopy);
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
        }
        /* the back buffer drawn last, after the slow job (as a frame's final pass) */
        VkImageMemoryBarrier tb = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                                   back_layout_ok[bb] ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, back[bb], range};
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &tb);
        VkClearColorValue col = {.float32 = {(k >> 8 & 255) / 255.0f, 0, 0, 1}};
        /* B8G8R8A8: float32[0] is R ... written below as exact bytes through uint-like floats */
        col.float32[0] = (float)(k & 255) / 255.0f;
        col.float32[1] = (float)(k >> 8 & 255) / 255.0f;
        col.float32[2] = (float)0x5a / 255.0f;
        col.float32[3] = 1.0f;
        vkCmdClearColorImage(cb, back[bb], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &col, 1, &range);
        /* D3D12's RENDER_TARGET -> PRESENT: everything written made available, for any later stage */
        VkImageMemoryBarrier pb = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT,
                                   VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, back[bb], range};
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &pb);
        back_layout_ok[bb] = 1;
        vkEndCommandBuffer(cb);
        VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cb, 0, NULL};
        if (vkQueueSubmit(queue, 1, &si, game_fence[slot])) {
            printf("frame %d: game submit failed\n", k);
            failed = 1;
            break;
        }

        /* vkd3d's present callback: acquire, blit, present */
        uint32_t idx = 0;
        VkSemaphore as = acq[k % 8];
        VkResult r = vkAcquireNextImageKHR(dev, sc, 5000000000ull, as, VK_NULL_HANDLE, &idx);
        if (r < 0 || r == VK_TIMEOUT) {
            printf("frame %d: acquire %d\n", k, r);
            failed = 1;
            break;
        }
        vkWaitForFences(dev, 1, &blit_fence[idx], VK_TRUE, UINT64_MAX);
        vkResetFences(dev, 1, &blit_fence[idx]);
        VkCommandBuffer bc = blit_cb[idx];
        vkResetCommandBuffer(bc, 0);
        vkBeginCommandBuffer(bc, &cbi);
        VkImageMemoryBarrier b2[2] = {
            {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, simg[idx], range},
            {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, 0, VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, back[bb], range}};
        /* srcStage BLIT, no source access: vkd3d's own (swapchain.c record_render_pass) */
        vkCmdPipelineBarrier(bc, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 2, b2);
        VkImageBlit blit = {layers, {{0, 0, 0}, {W, H, 1}}, layers, {{0, 0, 0}, {W, H, 1}}};
        vkCmdBlitImage(bc, back[bb], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, simg[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                       VK_FILTER_NEAREST);
        VkImageMemoryBarrier toPresent = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, 0,
                                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_QUEUE_FAMILY_IGNORED,
                                          VK_QUEUE_FAMILY_IGNORED, simg[idx], range};
        vkCmdPipelineBarrier(bc, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &toPresent);
        vkEndCommandBuffer(bc);
        VkPipelineStageFlags ws_stage = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo bsi = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 1, &as, &ws_stage, 1, &bc, 1, &rel[idx]};
        if (vkQueueSubmit(queue, 1, &bsi, blit_fence[idx])) {
            printf("frame %d: blit submit failed\n", k);
            failed = 1;
            break;
        }
        VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, NULL, 1, &rel[idx], 1, &sc, &idx, NULL};
        r = vkQueuePresentKHR(queue, &pi);
        if (r < 0) {
            printf("frame %d: present %d\n", k, r);
            failed = 1;
            break;
        }
    }
    vkDeviceWaitIdle(dev);
    double secs = (now_ms() - t0) / 1e3;
    /* the last frames shown and released (the presenting thread commits after the fences) */
    for (int i = 0; i < 300 && commits < frames; i++) usleep(10000);
    vkDestroySwapchainKHR(dev, sc, NULL);
    usleep(100000);
    g_quit = 1;
    pthread_join(cth, NULL);

    pthread_mutex_lock(&res_lock);
    printf("%d frames in %.1f s; %d commits: %d showed an older frame, %d a later one, %d no frame colour; %d went back in time; "
           "%d buffers drawn into while held\n",
           frames, secs, commits, stale, future, garbage, back_in_time, overwritten);
    for (int i = 0; i < ncases; i++) printf("  %s\n", cases[i]);
    pthread_mutex_unlock(&res_lock);
    if (failed || commits != frames || stale || future || garbage || overwritten) {
        printf("FAILED: %s\n", failed ? "a call failed" : commits != frames ? "not every frame was committed" : "the compositor got a frame other than the one presented");
        return 1;
    }
    printf("OK: %d of %d commits showed their own frame, none drawn into while held\n", commits, frames);
    return 0;
}
