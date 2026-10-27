/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * Wayland presentation.
 *
 *  - dma-buf: swapchain images are linear exportable images; each becomes a wl_buffer through
 *    zwp_linux_dmabuf_v1 once, and a present is attach + commit (no copy). This is the path for
 *    gamescope (and its WSI layer, which turns X11 games' swapchains into Wayland ones).
 *    On DroidDeck's compositor with zero-copy on (banner_ahb_v1, mode 1) the images are gralloc
 *    AHardwareBuffers allocated by the server; each also goes to the compositor once
 *    (banner_ahb_v1.attach), which then shows the frame on its own display layer instead of
 *    copying it. The dma-buf wl_buffer stays the same, so every other path still works.
 *  - wl_shm: for compositors without dma-buf (KWin compositing in software on the desktop): the
 *    frame is read back and copied into a shared-memory buffer.
 *
 * libwayland-client is opened at run time, so the driver loads in programs that never touch
 * Wayland. Our objects live on a private event queue, like Mesa's WSI.
 */
#define _GNU_SOURCE
#include "wsi.h"
#include "gen/vkb_client_gen.h"

#include <dlfcn.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------------ libwayland-client */

struct wl_proxy;
struct wl_event_queue;
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

static struct {
    void *lib;
    struct wl_proxy *(*proxy_marshal_constructor)(struct wl_proxy *, uint32_t, const struct wl_interface *, ...);
    struct wl_proxy *(*proxy_marshal_constructor_versioned)(struct wl_proxy *, uint32_t, const struct wl_interface *, uint32_t, ...);
    void (*proxy_marshal)(struct wl_proxy *, uint32_t, ...);
    int (*proxy_add_listener)(struct wl_proxy *, void (**)(void), void *);
    void (*proxy_destroy)(struct wl_proxy *);
    void *(*proxy_create_wrapper)(void *);
    void (*proxy_wrapper_destroy)(void *);
    void (*proxy_set_queue)(struct wl_proxy *, struct wl_event_queue *);
    uint32_t (*proxy_get_version)(struct wl_proxy *);
    struct wl_event_queue *(*display_create_queue)(struct wl_display *);
    void (*event_queue_destroy)(struct wl_event_queue *);
    int (*display_roundtrip_queue)(struct wl_display *, struct wl_event_queue *);
    int (*display_dispatch_queue_pending)(struct wl_display *, struct wl_event_queue *);
    int (*display_prepare_read_queue)(struct wl_display *, struct wl_event_queue *);
    int (*display_read_events)(struct wl_display *);
    void (*display_cancel_read)(struct wl_display *);
    int (*display_flush)(struct wl_display *);
    int (*display_get_fd)(struct wl_display *);
    int (*display_get_error)(struct wl_display *);
    const struct wl_interface *registry_iface, *callback_iface, *buffer_iface, *shm_iface, *shm_pool_iface;
} wl;

int vkb_wl_load(void)
{
    static int state;
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&lock);
    if (!state) {
        wl.lib = dlopen("libwayland-client.so.0", RTLD_NOW | RTLD_LOCAL);
        state = -1;
        if (wl.lib) {
#define L(f, n) (*(void **)&wl.f = dlsym(wl.lib, n))
            int ok = 1;
            ok &= !!L(proxy_marshal_constructor, "wl_proxy_marshal_constructor");
            ok &= !!L(proxy_marshal_constructor_versioned, "wl_proxy_marshal_constructor_versioned");
            ok &= !!L(proxy_marshal, "wl_proxy_marshal");
            ok &= !!L(proxy_add_listener, "wl_proxy_add_listener");
            ok &= !!L(proxy_destroy, "wl_proxy_destroy");
            ok &= !!L(proxy_create_wrapper, "wl_proxy_create_wrapper");
            ok &= !!L(proxy_wrapper_destroy, "wl_proxy_wrapper_destroy");
            ok &= !!L(proxy_set_queue, "wl_proxy_set_queue");
            ok &= !!L(proxy_get_version, "wl_proxy_get_version");
            ok &= !!L(display_create_queue, "wl_display_create_queue");
            ok &= !!L(event_queue_destroy, "wl_event_queue_destroy");
            ok &= !!L(display_roundtrip_queue, "wl_display_roundtrip_queue");
            ok &= !!L(display_dispatch_queue_pending, "wl_display_dispatch_queue_pending");
            ok &= !!L(display_prepare_read_queue, "wl_display_prepare_read_queue");
            ok &= !!L(display_read_events, "wl_display_read_events");
            ok &= !!L(display_cancel_read, "wl_display_cancel_read");
            ok &= !!L(display_flush, "wl_display_flush");
            ok &= !!L(display_get_fd, "wl_display_get_fd");
            ok &= !!L(display_get_error, "wl_display_get_error");
            ok &= !!L(registry_iface, "wl_registry_interface");
            ok &= !!L(callback_iface, "wl_callback_interface");
            ok &= !!L(buffer_iface, "wl_buffer_interface");
            ok &= !!L(shm_iface, "wl_shm_interface");
            ok &= !!L(shm_pool_iface, "wl_shm_pool_interface");
#undef L
            if (ok) state = 1;
            else VKB_ERR("libwayland-client lacks functions this driver needs");
        } else {
            VKB_WARN("no libwayland-client: %s", dlerror());
        }
    }
    pthread_mutex_unlock(&lock);
    return state > 0;
}

/* zwp_linux_dmabuf_v1 / zwp_linux_buffer_params_v1, as wayland-scanner would emit them. */
static const struct wl_interface params_iface;
static const struct wl_interface *dmabuf_types[8];
static const struct wl_message dmabuf_requests[] = {
    {"destroy", "", dmabuf_types + 0},
    {"create_params", "n", dmabuf_types + 6},
};
static const struct wl_message dmabuf_events[] = {
    {"format", "u", dmabuf_types + 0},
    {"modifier", "3uuu", dmabuf_types + 0},
};
static const struct wl_interface dmabuf_iface = {"zwp_linux_dmabuf_v1", 3, 2, dmabuf_requests, 2, dmabuf_events};
static const struct wl_message params_requests[] = {
    {"destroy", "", dmabuf_types + 0},
    {"add", "huuuuu", dmabuf_types + 0},
    {"create", "iiuu", dmabuf_types + 0},
    {"create_immed", "2niiuu", dmabuf_types + 7},
};
static const struct wl_message params_events[] = {
    {"created", "n", dmabuf_types + 7},
    {"failed", "", dmabuf_types + 0},
};
static const struct wl_interface params_iface = {"zwp_linux_buffer_params_v1", 3, 4, params_requests, 2, params_events};

/* banner_ahb_v1 (DroidDeck's compositor, waylandcomp/protocols/banner-ahb-v1.xml), version 2. */
static const struct wl_interface *ahb_types[9];
static const struct wl_message ahb_requests[] = {
    {"destroy", "", ahb_types + 0},
    {"attach", "ohuuuuuu", ahb_types + 1},
};
static const struct wl_message ahb_events[] = {
    {"mode", "2u", ahb_types + 0},
};
static const struct wl_interface ahb_iface = {"banner_ahb_v1", 2, 2, ahb_requests, 1, ahb_events};

static void init_types(void)
{
    /* types[0..5] stay NULL (no object arguments); [6] = params, [7] = wl_buffer */
    dmabuf_types[6] = &params_iface;
    dmabuf_types[7] = wl.buffer_iface;
    ahb_types[1] = wl.buffer_iface; /* attach's buffer; the rest stay NULL */
}

/* ------------------------------------------------------------------ per-swapchain state */

#define WL_DISPLAY_GET_REGISTRY 1
#define WL_REGISTRY_BIND 0
#define WL_SURFACE_ATTACH 1
#define WL_SURFACE_DAMAGE 2
#define WL_SURFACE_FRAME 3
#define WL_SURFACE_COMMIT 6
#define WL_SURFACE_DAMAGE_BUFFER 9
#define WL_BUFFER_DESTROY 0
#define WL_SHM_CREATE_POOL 0
#define WL_SHM_POOL_CREATE_BUFFER 0
#define WL_SHM_POOL_DESTROY 1
#define DMABUF_DESTROY 0
#define DMABUF_CREATE_PARAMS 1
#define PARAMS_DESTROY 0
#define PARAMS_ADD 1
#define PARAMS_CREATE_IMMED 3
#define AHB_DESTROY 0
#define AHB_ATTACH 1
#define WL_SHM_FORMAT_ARGB8888 0u
#define WL_SHM_FORMAT_XRGB8888 1u
#define FOURCC(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))
#define DRM_XRGB8888 FOURCC('X', 'R', '2', '4')
#define DRM_ARGB8888 FOURCC('A', 'R', '2', '4')
#define DRM_XBGR8888 FOURCC('X', 'B', '2', '4')
#define DRM_ABGR8888 FOURCC('A', 'B', '2', '4')
#define MAX_FMTS 64

typedef struct shm_buf {
    struct wl_proxy *buffer;
    uint8_t *ptr;
    int busy;
} shm_buf;

typedef struct wl_state {
    struct wl_display *display;
    struct wl_event_queue *queue;
    void *display_wrapper;
    void *surface_wrapper;
    struct wl_proxy *registry;
    struct wl_proxy *shm;
    struct wl_proxy *dmabuf;
    uint32_t dmabuf_version;
    struct wl_proxy *ahb;          /* banner_ahb_v1 */
    uint32_t ahb_version;
    int ahb_mode;                  /* the compositor's last mode event (-1 = none yet) */
    int connected;                 /* globals bound and their initial events in */
    int built;                     /* the swapchain is up: a mode change retires it */
    uint32_t shm_formats[MAX_FMTS];
    int nshm;
    struct { uint32_t format; uint64_t modifier; } dmabuf_mods[256];
    int nmods;
    uint32_t dmabuf_formats[MAX_FMTS];
    int nformats;
    struct wl_proxy **buffers;     /* dma-buf wl_buffers, one per image */
    shm_buf *shm_bufs;
    int nshm_bufs;
    struct wl_proxy *shm_pool;
    uint8_t *shm_map;
    size_t shm_size;
    uint32_t shm_stride, shm_format;
    int swizzle;                   /* RGBA frame into an ARGB/XRGB shm buffer */
    struct wl_proxy *frame;        /* pending frame callback */
    vkb_swapchain *sc;
    /* Presenting thread (dma-buf): once it runs, it alone dispatches this queue. */
    pthread_t thread;
    int thread_on, quit, dead;
    int efd;                       /* wakes the thread for a new frame or to quit */
    pthread_cond_t changed;        /* with sc->lock: an image came back, or the chain broke */
    VkFence *fences;               /* one per image: its rendering is done */
    uint32_t ring[64], head, len;  /* frames waiting for their rendering, in present order */
} wl_state;

/* The compositor's zero-copy switch. A live swapchain built for the other mode is retired (out of
 * date) so the program rebuilds it the way the compositor wants now; until then it presents as
 * before. Not when its gralloc swapchain could not be built: the rebuild would be the same. */
static void ahb_mode(void *data, struct wl_proxy *ahb, uint32_t enabled)
{
    (void)ahb;
    wl_state *s = data;
    s->ahb_mode = enabled ? 1 : 0;
    vkb_swapchain *sc = s->sc;
    if (!s->built || !sc || s->ahb_mode == sc->ahb || (s->ahb_mode && vkb_wsi_ahb_disabled())) return;
    VKB_INFO("Wayland: the compositor switched zero-copy %s; retiring the %ux%u swapchain to rebuild it %s", s->ahb_mode ? "on" : "off",
             sc->info.imageExtent.width, sc->info.imageExtent.height, s->ahb_mode ? "from gralloc buffers" : "from ordinary dma-bufs");
    pthread_mutex_lock(&sc->lock);
    if (sc->status >= 0) sc->status = VK_ERROR_OUT_OF_DATE_KHR;
    pthread_mutex_unlock(&sc->lock);
}

static void registry_global(void *data, struct wl_proxy *reg, uint32_t name, const char *iface, uint32_t version)
{
    wl_state *s = data;
    if (!strcmp(iface, "wl_shm") && !s->shm) {
        s->shm = wl.proxy_marshal_constructor_versioned(reg, WL_REGISTRY_BIND, wl.shm_iface, 1, name, wl.shm_iface->name, 1, NULL);
    } else if (!strcmp(iface, "zwp_linux_dmabuf_v1") && !s->dmabuf && version >= 2) {
        uint32_t v = version < 3 ? version : 3;
        s->dmabuf = wl.proxy_marshal_constructor_versioned(reg, WL_REGISTRY_BIND, &dmabuf_iface, v, name, dmabuf_iface.name, v, NULL);
        s->dmabuf_version = v;
    } else if (!strcmp(iface, "banner_ahb_v1") && !s->ahb && !vkb_wsi_ahb_disabled()) {
        static const void *ahb_listener[] = {(void *)ahb_mode};
        uint32_t v = version < 2 ? version : 2;
        s->ahb = wl.proxy_marshal_constructor_versioned(reg, WL_REGISTRY_BIND, &ahb_iface, v, name, ahb_iface.name, v, NULL);
        s->ahb_version = v;
        if (s->ahb) wl.proxy_add_listener(s->ahb, (void (**)(void))ahb_listener, s);
    }
}

static void registry_global_remove(void *data, struct wl_proxy *reg, uint32_t name)
{
    (void)data;
    (void)reg;
    (void)name;
}

static const void *registry_listener[] = {(void *)registry_global, (void *)registry_global_remove};

static void shm_format(void *data, struct wl_proxy *shm, uint32_t format)
{
    (void)shm;
    wl_state *s = data;
    if (s->nshm < MAX_FMTS) s->shm_formats[s->nshm++] = format;
}

static const void *shm_listener[] = {(void *)shm_format};

static void dmabuf_format(void *data, struct wl_proxy *d, uint32_t format)
{
    (void)d;
    wl_state *s = data;
    if (s->nformats < MAX_FMTS) s->dmabuf_formats[s->nformats++] = format;
}

static void dmabuf_modifier(void *data, struct wl_proxy *d, uint32_t format, uint32_t hi, uint32_t lo)
{
    (void)d;
    wl_state *s = data;
    if (s->nmods < 256) {
        s->dmabuf_mods[s->nmods].format = format;
        s->dmabuf_mods[s->nmods].modifier = ((uint64_t)hi << 32) | lo;
        s->nmods++;
    }
}

static const void *dmabuf_listener[] = {(void *)dmabuf_format, (void *)dmabuf_modifier};

static void buffer_release(void *data, struct wl_proxy *buffer)
{
    (void)buffer;
    vkb_swap_image *img = data;
    pthread_mutex_lock(&img->chain->lock);
    img->busy = 0;
    pthread_mutex_unlock(&img->chain->lock);
}

static const void *buffer_listener[] = {(void *)buffer_release};

static void shm_buffer_release(void *data, struct wl_proxy *buffer)
{
    (void)buffer;
    ((shm_buf *)data)->busy = 0;
}

static const void *shm_buffer_listener[] = {(void *)shm_buffer_release};

static void frame_done(void *data, struct wl_proxy *cb, uint32_t t)
{
    (void)t;
    wl_state *s = data;
    if (s->frame == cb) s->frame = NULL;
    wl.proxy_destroy(cb);
}

static const void *frame_listener[] = {(void *)frame_done};

/* Dispatch our queue, waiting up to timeout_ns for events. -1 on a dead connection. */
static int dispatch_timeout(wl_state *s, uint64_t timeout_ns)
{
    if (wl.display_dispatch_queue_pending(s->display, s->queue) < 0) return -1;
    while (wl.display_prepare_read_queue(s->display, s->queue) != 0) {
        if (wl.display_dispatch_queue_pending(s->display, s->queue) < 0) return -1;
    }
    if (wl.display_flush(s->display) < 0 && errno != EAGAIN) {
        wl.display_cancel_read(s->display);
        return -1;
    }
    struct pollfd p = {wl.display_get_fd(s->display), POLLIN, 0};
    int ms = timeout_ns == UINT64_MAX ? -1 : (int)((timeout_ns + 999999) / 1000000);
    int r = poll(&p, 1, ms);
    if (r <= 0) {
        wl.display_cancel_read(s->display);
        return r < 0 && errno != EINTR ? -1 : 0;
    }
    if (wl.display_read_events(s->display) < 0) return -1;
    return wl.display_dispatch_queue_pending(s->display, s->queue) < 0 ? -1 : 1;
}

/* dispatch_timeout, also returning (0) when efd is written. */
static int dispatch_or_wake(wl_state *s, int efd)
{
    if (wl.display_dispatch_queue_pending(s->display, s->queue) < 0) return -1;
    while (wl.display_prepare_read_queue(s->display, s->queue) != 0) {
        if (wl.display_dispatch_queue_pending(s->display, s->queue) < 0) return -1;
    }
    if (wl.display_flush(s->display) < 0 && errno != EAGAIN) {
        wl.display_cancel_read(s->display);
        return -1;
    }
    struct pollfd p[2] = {{wl.display_get_fd(s->display), POLLIN, 0}, {efd, POLLIN, 0}};
    int r = poll(p, 2, -1);
    if (p[1].revents & POLLIN) {
        uint64_t v;
        if (read(efd, &v, sizeof(v)) < 0) { /* nothing to drain */ }
    }
    if (r <= 0 || !(p[0].revents & (POLLIN | POLLERR | POLLHUP))) {
        wl.display_cancel_read(s->display);
        return r < 0 && errno != EINTR ? -1 : 0;
    }
    if (wl.display_read_events(s->display) < 0) return -1;
    return wl.display_dispatch_queue_pending(s->display, s->queue) < 0 ? -1 : 1;
}

static VkResult wl_present(vkb_swapchain *sc, uint32_t index, const VkPresentRegionKHR *region);

/* Waits for each queued frame's rendering, then shows it; between frames, takes the
 * compositor's events (buffer releases, frame callbacks) so images come back to the game. */
static void *present_thread(void *arg)
{
    vkb_swapchain *sc = arg;
    wl_state *s = sc->bdata;
    for (;;) {
        pthread_mutex_lock(&sc->lock);
        int have = s->len > 0, quit = s->quit;
        uint32_t index = have ? s->ring[s->head] : 0;
        if (have) {
            s->head = (s->head + 1) % 64;
            s->len--;
        }
        pthread_mutex_unlock(&sc->lock);
        if (have) {
            /* A slow frame (a game loading) is waited for; only a real loss or a frame past the limit
             * (VKBRIDGE_PRESENT_TIMEOUT_S) ends the swapchain (wsi.c) */
            VkResult r = vkb_wsi_wait_frame(sc, s->fences[index]);
            if (r >= 0 && !s->dead) r = wl_present(sc, index, NULL);
            pthread_mutex_lock(&sc->lock);
            if (r < 0 || s->dead) {
                sc->images[index].busy = 0;
                if (r < 0 && sc->status >= 0) sc->status = r == VK_ERROR_DEVICE_LOST ? r : VK_ERROR_SURFACE_LOST_KHR;
                if (r < 0) s->dead = 1;
            }
            pthread_cond_broadcast(&s->changed);
            pthread_mutex_unlock(&sc->lock);
            continue;
        }
        if (quit) break;
        int d;
        if (s->dead) {
            struct pollfd p = {s->efd, POLLIN, 0};
            uint64_t v;
            if (poll(&p, 1, -1) > 0 && read(s->efd, &v, sizeof(v)) < 0) { /* nothing to drain */ }
            d = 0;
        } else {
            d = dispatch_or_wake(s, s->efd);
        }
        pthread_mutex_lock(&sc->lock);
        if (d < 0) {
            s->dead = 1;
            if (sc->status >= 0) sc->status = VK_ERROR_SURFACE_LOST_KHR;
        }
        pthread_cond_broadcast(&s->changed);
        pthread_mutex_unlock(&sc->lock);
    }
    return NULL;
}

static VkResult wl_present_async(vkb_swapchain *sc, VkQueue queue, uint32_t index, uint32_t nwait, const VkSemaphore *wait)
{
    wl_state *s = sc->bdata;
    VkDevice device = (VkDevice)sc->dev;
    if (!s->thread_on) {
        s->fences = calloc(sc->count, sizeof(VkFence));
        s->efd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        if (!s->fences || s->efd < 0) return VK_ERROR_OUT_OF_HOST_MEMORY;
        VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        for (uint32_t i = 0; i < sc->count; i++)
            if (vkb_wire_vkCreateFence(device, &fci, NULL, &s->fences[i]) != VK_SUCCESS) return VK_ERROR_OUT_OF_HOST_MEMORY;
        pthread_cond_init(&s->changed, NULL);
        if (pthread_create(&s->thread, NULL, present_thread, sc) != 0) return VK_ERROR_OUT_OF_HOST_MEMORY;
        s->thread_on = 1;
    }
    VkPipelineStageFlags *stages = alloca((nwait ? nwait : 1) * sizeof(VkPipelineStageFlags));
    for (uint32_t i = 0; i < nwait; i++) stages[i] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, nwait, wait, stages, 0, NULL, 0, NULL};
    VkResult r = vkb_wire_vkQueueSubmit(queue, 1, &si, s->fences[index]);
    pthread_mutex_lock(&sc->lock);
    if (r != VK_SUCCESS || s->len == 64) {
        sc->images[index].busy = 0;
        pthread_mutex_unlock(&sc->lock);
        return r != VK_SUCCESS ? r : VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    s->ring[(s->head + s->len) % 64] = index;
    s->len++;
    r = sc->status;
    pthread_mutex_unlock(&sc->lock);
    uint64_t one = 1;
    if (write(s->efd, &one, sizeof(one)) < 0) { /* the counter is already set */ }
    return r < 0 ? r : VK_SUCCESS;
}

static int has_u32(const uint32_t *a, int n, uint32_t v)
{
    for (int i = 0; i < n; i++)
        if (a[i] == v) return 1;
    return 0;
}

static int dmabuf_supports(wl_state *s, uint32_t fourcc, uint64_t modifier)
{
    if (s->nmods) {
        for (int i = 0; i < s->nmods; i++) {
            /* DRM_FORMAT_MOD_INVALID means "implicit": treat as linear for our linear buffers. */
            if (s->dmabuf_mods[i].format == fourcc &&
                (s->dmabuf_mods[i].modifier == modifier || s->dmabuf_mods[i].modifier == 0x00ffffffffffffffull))
                return 1;
        }
        return 0;
    }
    return has_u32(s->dmabuf_formats, s->nformats, fourcc);
}

/* ------------------------------------------------------------------ backend */

VkResult vkb_wl_surface_extent(vkb_surface *s, VkExtent2D *e)
{
    (void)s;
    e->width = e->height = 0xFFFFFFFFu;
    return VK_SUCCESS;
}

static void wl_teardown(wl_state *s);

/* The swapchain's own queue and registry, its globals bound and their initial events taken (the
 * dma-buf formats, banner_ahb_v1's mode). Once per wl_state: vkb_wl_ahb_wanted may have done it. */
static VkResult wl_connect(vkb_swapchain *sc)
{
    if (sc->bdata) return ((wl_state *)sc->bdata)->connected ? VK_SUCCESS : VK_ERROR_SURFACE_LOST_KHR;
    vkb_surface *surface = sc->surface;
    wl_state *s = calloc(1, sizeof(*s));
    if (!s) return VK_ERROR_OUT_OF_HOST_MEMORY;
    sc->bdata = s;
    s->sc = sc;
    s->ahb_mode = -1;
    s->display = surface->wl_display;
    init_types();
    s->queue = wl.display_create_queue(s->display);
    s->display_wrapper = wl.proxy_create_wrapper(s->display);
    wl.proxy_set_queue(s->display_wrapper, s->queue);
    s->surface_wrapper = wl.proxy_create_wrapper(surface->wl_surface);
    wl.proxy_set_queue(s->surface_wrapper, s->queue);
    s->registry = wl.proxy_marshal_constructor(s->display_wrapper, WL_DISPLAY_GET_REGISTRY, wl.registry_iface, NULL);
    wl.proxy_add_listener(s->registry, (void (**)(void))registry_listener, s);
    if (wl.display_roundtrip_queue(s->display, s->queue) < 0) return VK_ERROR_SURFACE_LOST_KHR;
    if (s->shm) wl.proxy_add_listener(s->shm, (void (**)(void))shm_listener, s);
    if (s->dmabuf) wl.proxy_add_listener(s->dmabuf, (void (**)(void))dmabuf_listener, s);
    if (wl.display_roundtrip_queue(s->display, s->queue) < 0) return VK_ERROR_SURFACE_LOST_KHR;
    s->connected = 1;
    return VK_SUCCESS;
}

int vkb_wl_ahb_wanted(vkb_swapchain *sc)
{
    if (wl_connect(sc) != VK_SUCCESS) {
        wl_teardown(sc->bdata); /* wl_init tries again and reports it */
        sc->bdata = NULL;
        return 0;
    }
    wl_state *s = sc->bdata;
    if (!s->ahb) return 0;
    if (s->ahb_version >= 2) return s->ahb_mode == 1;
    /* A version 1 compositor has no mode: the environment decides. */
    const char *e = getenv("VKBRIDGE_WSI_AHB");
    return e && !strcmp(e, "1");
}

static VkResult wl_init(vkb_swapchain *sc)
{
    VkResult cr = wl_connect(sc);
    if (cr != VK_SUCCESS) return cr;
    wl_state *s = sc->bdata;

    uint32_t w = sc->info.imageExtent.width, h = sc->info.imageExtent.height;
    int opaque = sc->info.compositeAlpha == VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    int rgba = sc->info.imageFormat == VK_FORMAT_R8G8B8A8_UNORM || sc->info.imageFormat == VK_FORMAT_R8G8B8A8_SRGB;
    if (!sc->copy_present) {
        if (!s->dmabuf) {
            VKB_INFO("Wayland: the compositor has no zwp_linux_dmabuf_v1");
            return VK_ERROR_FEATURE_NOT_PRESENT;
        }
        uint32_t fourcc = rgba ? (opaque ? DRM_XBGR8888 : DRM_ABGR8888) : (opaque ? DRM_XRGB8888 : DRM_ARGB8888);
        uint64_t mod = sc->images[0].modifier;
        if (!dmabuf_supports(s, fourcc, mod)) {
            uint32_t alt = rgba ? DRM_ABGR8888 : DRM_ARGB8888;
            if (!dmabuf_supports(s, alt, mod)) {
                VKB_INFO("Wayland: the compositor takes no %.4s dma-buf with modifier 0x%llx", (char *)&fourcc, (unsigned long long)mod);
                return VK_ERROR_FORMAT_NOT_SUPPORTED;
            }
            fourcc = alt;
        }
        /* A bad plane is a protocol error, and that ends the whole connection - the wl_shm
         * fallback with it (tablet run 10: "invalid stride 0 for plane 0", then a black screen).
         * Checked here, so such a swapchain falls back to copies instead. */
        for (uint32_t i = 0; i < sc->count; i++) {
            const vkb_swap_image *img = &sc->images[i];
            if (img->dmabuf_fd < 0 || img->stride < w * 4u) {
                VKB_WARN("Wayland: swapchain image %u has an unusable dma-buf plane (%ux%u, fd %d, offset %u, stride %u, "
                         "modifier 0x%llx); presenting through copies", i, w, h, img->dmabuf_fd, img->offset, img->stride,
                         (unsigned long long)img->modifier);
                return VK_ERROR_FORMAT_NOT_SUPPORTED;
            }
        }
        s->buffers = calloc(sc->count, sizeof(*s->buffers));
        for (uint32_t i = 0; i < sc->count; i++) {
            vkb_swap_image *img = &sc->images[i];
            struct wl_proxy *params = wl.proxy_marshal_constructor(s->dmabuf, DMABUF_CREATE_PARAMS, &params_iface, NULL);
            wl.proxy_marshal(params, PARAMS_ADD, img->dmabuf_fd, 0u, img->offset, img->stride, (uint32_t)(img->modifier >> 32),
                             (uint32_t)(img->modifier & 0xffffffffu));
            s->buffers[i] = wl.proxy_marshal_constructor(params, PARAMS_CREATE_IMMED, wl.buffer_iface, NULL, (int32_t)w, (int32_t)h, fourcc, 0u);
            wl.proxy_marshal(params, PARAMS_DESTROY);
            wl.proxy_destroy(params);
            wl.proxy_add_listener(s->buffers[i], (void (**)(void))buffer_listener, img);
            /* The buffer's AHardwareBuffer, once, before it is ever attached to the surface: the
             * server wrote its handle into this socket; the compositor reads it from the other end. */
            if (sc->ahb && s->ahb && img->ahb_sock >= 0) {
                wl.proxy_marshal(s->ahb, AHB_ATTACH, s->buffers[i], img->ahb_sock, w, h, img->stride / 4u, 0u, 0u, sc->count);
                close(img->ahb_sock); /* libwayland sent its own copy */
                img->ahb_sock = -1;
            }
        }
        if (wl.display_roundtrip_queue(s->display, s->queue) < 0 || wl.display_get_error(s->display)) {
            VKB_ERR("Wayland: the compositor refused the dma-bufs");
            return VK_ERROR_SURFACE_LOST_KHR;
        }
        VKB_INFO("Wayland: %u dma-buf buffers %ux%u %.4s, stride %u, modifier 0x%llx%s", sc->count, w, h, (char *)&fourcc,
                 sc->images[0].stride, (unsigned long long)sc->images[0].modifier,
                 sc->ahb ? ", gralloc AHardwareBuffers handed to the compositor (banner_ahb_v1)" : "");
        s->built = 1;
        return VK_SUCCESS;
    }
    if (!s->shm) {
        VKB_ERR("Wayland: the compositor offers neither dma-buf nor wl_shm");
        return VK_ERROR_SURFACE_LOST_KHR;
    }
    uint32_t fmt;
    if (rgba && has_u32(s->shm_formats, s->nshm, opaque ? DRM_XBGR8888 : DRM_ABGR8888)) {
        fmt = opaque ? DRM_XBGR8888 : DRM_ABGR8888;
    } else {
        fmt = opaque ? WL_SHM_FORMAT_XRGB8888 : WL_SHM_FORMAT_ARGB8888;
        s->swizzle = rgba;
    }
    s->shm_format = fmt;
    s->shm_stride = w * 4;
    s->nshm_bufs = (int)sc->count + 1;
    size_t one = (size_t)s->shm_stride * h;
    s->shm_size = one * (size_t)s->nshm_bufs;
    int fd = vkb_memfd("vkbridge-wl-shm", s->shm_size);
    if (fd < 0) return VK_ERROR_OUT_OF_HOST_MEMORY;
    s->shm_map = mmap(NULL, s->shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (s->shm_map == MAP_FAILED) {
        s->shm_map = NULL;
        close(fd);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    s->shm_pool = wl.proxy_marshal_constructor(s->shm, WL_SHM_CREATE_POOL, wl.shm_pool_iface, NULL, fd, (int32_t)s->shm_size);
    close(fd);
    s->shm_bufs = calloc((size_t)s->nshm_bufs, sizeof(shm_buf));
    for (int i = 0; i < s->nshm_bufs; i++) {
        s->shm_bufs[i].ptr = s->shm_map + one * (size_t)i;
        s->shm_bufs[i].buffer = wl.proxy_marshal_constructor(s->shm_pool, WL_SHM_POOL_CREATE_BUFFER, wl.buffer_iface, NULL,
                                                             (int32_t)(one * (size_t)i), (int32_t)w, (int32_t)h, (int32_t)s->shm_stride, fmt);
        wl.proxy_add_listener(s->shm_bufs[i].buffer, (void (**)(void))shm_buffer_listener, &s->shm_bufs[i]);
    }
    VKB_INFO("Wayland: presenting %ux%u through wl_shm (format 0x%x%s)", w, h, fmt, s->swizzle ? ", swizzled" : "");
    return VK_SUCCESS;
}

static void copy_frame(wl_state *s, const uint8_t *src, uint8_t *dst, uint32_t w, uint32_t h)
{
    size_t row = (size_t)w * 4;
    if (!s->swizzle) {
        memcpy(dst, src, row * h);
        return;
    }
    for (size_t i = 0; i < (size_t)w * h; i++) {
        dst[i * 4 + 0] = src[i * 4 + 2];
        dst[i * 4 + 1] = src[i * 4 + 1];
        dst[i * 4 + 2] = src[i * 4 + 0];
        dst[i * 4 + 3] = src[i * 4 + 3];
    }
}

static VkResult wl_present(vkb_swapchain *sc, uint32_t index, const VkPresentRegionKHR *region)
{
    wl_state *s = sc->bdata;
    uint32_t w = sc->info.imageExtent.width, h = sc->info.imageExtent.height;
    if (wl.display_get_error(s->display)) return VK_ERROR_SURFACE_LOST_KHR;
    /* FIFO: one frame per compositor frame callback (bounded, so a hidden window cannot hang
     * the game forever). */
    if (sc->info.presentMode == VK_PRESENT_MODE_FIFO_KHR || sc->info.presentMode == VK_PRESENT_MODE_FIFO_RELAXED_KHR) {
        struct timespec t0;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        while (s->frame) {
            if (dispatch_timeout(s, 20000000ull) < 0) return VK_ERROR_SURFACE_LOST_KHR;
            struct timespec t1;
            clock_gettime(CLOCK_MONOTONIC, &t1);
            if ((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000 > 250) break;
        }
    }
    struct wl_proxy *buffer;
    if (!sc->copy_present) {
        buffer = s->buffers[index];
    } else {
        shm_buf *b = NULL;
        for (int tries = 0; !b && tries < 50; tries++) {
            for (int i = 0; i < s->nshm_bufs; i++)
                if (!s->shm_bufs[i].busy) {
                    b = &s->shm_bufs[i];
                    break;
                }
            if (!b && dispatch_timeout(s, 10000000ull) < 0) return VK_ERROR_SURFACE_LOST_KHR;
        }
        if (!b) return VK_SUCCESS; /* compositor holds every buffer: drop this frame */
        copy_frame(s, sc->staging_ptr, b->ptr, w, h);
        b->busy = 1;
        buffer = b->buffer;
    }
    if (sc->info.presentMode == VK_PRESENT_MODE_FIFO_KHR || sc->info.presentMode == VK_PRESENT_MODE_FIFO_RELAXED_KHR) {
        if (s->frame) wl.proxy_destroy(s->frame);
        s->frame = wl.proxy_marshal_constructor(s->surface_wrapper, WL_SURFACE_FRAME, wl.callback_iface, NULL);
        wl.proxy_add_listener(s->frame, (void (**)(void))frame_listener, s);
    }
    wl.proxy_marshal(s->surface_wrapper, WL_SURFACE_ATTACH, buffer, 0, 0);
    int v4 = wl.proxy_get_version(s->surface_wrapper) >= 4;
    if (region && region->rectangleCount && region->pRectangles) {
        for (uint32_t i = 0; i < region->rectangleCount; i++) {
            const VkRectLayerKHR *r = &region->pRectangles[i];
            wl.proxy_marshal(s->surface_wrapper, v4 ? WL_SURFACE_DAMAGE_BUFFER : WL_SURFACE_DAMAGE, r->offset.x, r->offset.y,
                             (int32_t)r->extent.width, (int32_t)r->extent.height);
        }
    } else {
        wl.proxy_marshal(s->surface_wrapper, v4 ? WL_SURFACE_DAMAGE_BUFFER : WL_SURFACE_DAMAGE, 0, 0, (int32_t)w, (int32_t)h);
    }
    wl.proxy_marshal(s->surface_wrapper, WL_SURFACE_COMMIT);
    wl.display_flush(s->display);
    if (wl.display_dispatch_queue_pending(s->display, s->queue) < 0) return VK_ERROR_SURFACE_LOST_KHR;
    return VK_SUCCESS;
}

static VkResult wl_wait_free(vkb_swapchain *sc, uint64_t timeout_ns)
{
    wl_state *s = sc->bdata;
    if (s->thread_on) {
        /* The presenting thread takes the events; wait for it to hand an image back. */
        struct timespec dl;
        clock_gettime(CLOCK_REALTIME, &dl);
        uint64_t t = timeout_ns > 3600ull * 1000000000ull ? 3600ull * 1000000000ull : timeout_ns;
        dl.tv_sec += (time_t)(t / 1000000000ull);
        dl.tv_nsec += (long)(t % 1000000000ull);
        if (dl.tv_nsec >= 1000000000L) {
            dl.tv_sec++;
            dl.tv_nsec -= 1000000000L;
        }
        VkResult r = VK_TIMEOUT;
        pthread_mutex_lock(&sc->lock);
        for (;;) {
            if (sc->status < 0) {
                r = sc->status;
                break;
            }
            int free_one = 0;
            for (uint32_t i = 0; i < sc->count && !free_one; i++) free_one = !sc->images[i].busy && !sc->images[i].acquired;
            if (free_one) {
                r = VK_SUCCESS;
                break;
            }
            if (pthread_cond_timedwait(&s->changed, &sc->lock, &dl) != 0) break;
        }
        pthread_mutex_unlock(&sc->lock);
        return r;
    }
    int r = dispatch_timeout(s, timeout_ns);
    if (r < 0) return VK_ERROR_SURFACE_LOST_KHR;
    return r == 0 ? VK_TIMEOUT : VK_SUCCESS;
}

static void wl_teardown(wl_state *s)
{
    if (!s) return;
    vkb_swapchain *sc = s->sc;
    if (s->frame) wl.proxy_destroy(s->frame);
    if (s->buffers) {
        for (uint32_t i = 0; i < sc->count; i++)
            if (s->buffers[i]) {
                wl.proxy_marshal(s->buffers[i], WL_BUFFER_DESTROY);
                wl.proxy_destroy(s->buffers[i]);
            }
        free(s->buffers);
    }
    if (s->shm_bufs) {
        for (int i = 0; i < s->nshm_bufs; i++)
            if (s->shm_bufs[i].buffer) {
                wl.proxy_marshal(s->shm_bufs[i].buffer, WL_BUFFER_DESTROY);
                wl.proxy_destroy(s->shm_bufs[i].buffer);
            }
        free(s->shm_bufs);
    }
    if (s->shm_pool) {
        wl.proxy_marshal(s->shm_pool, WL_SHM_POOL_DESTROY);
        wl.proxy_destroy(s->shm_pool);
    }
    if (s->shm_map) munmap(s->shm_map, s->shm_size);
    if (s->dmabuf) {
        wl.proxy_marshal(s->dmabuf, DMABUF_DESTROY);
        wl.proxy_destroy(s->dmabuf);
    }
    if (s->ahb) {
        wl.proxy_marshal(s->ahb, AHB_DESTROY);
        wl.proxy_destroy(s->ahb);
    }
    if (s->shm) wl.proxy_destroy(s->shm);
    if (s->registry) wl.proxy_destroy(s->registry);
    if (s->surface_wrapper) wl.proxy_wrapper_destroy(s->surface_wrapper);
    if (s->display_wrapper) wl.proxy_wrapper_destroy(s->display_wrapper);
    if (s->display) wl.display_flush(s->display);
    if (s->queue) wl.event_queue_destroy(s->queue);
    free(s);
}

static void wl_destroy(vkb_swapchain *sc)
{
    wl_state *s = sc->bdata;
    if (s && s->thread_on) {
        /* Frames already presented are shown (or dropped, on a broken chain) first. */
        pthread_mutex_lock(&sc->lock);
        s->quit = 1;
        pthread_mutex_unlock(&sc->lock);
        uint64_t one = 1;
        if (write(s->efd, &one, sizeof(one)) < 0) { /* the counter is already set */ }
        pthread_join(s->thread, NULL);
        pthread_cond_destroy(&s->changed);
        s->thread_on = 0;
    }
    if (s && s->fences) {
        for (uint32_t i = 0; i < sc->count; i++)
            if (s->fences[i]) vkb_wire_vkDestroyFence((VkDevice)sc->dev, s->fences[i], NULL);
        free(s->fences);
        s->fences = NULL;
    }
    if (s && s->efd > 0) close(s->efd);
    wl_teardown(sc->bdata);
    sc->bdata = NULL;
}

const vkb_wsi_backend vkb_wsi_wayland_backend = {wl_init, wl_present, wl_wait_free, wl_destroy, wl_present_async};
