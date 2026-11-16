/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * Window-system integration implemented by the bridge client: the server's GPU has no Linux
 * window systems, so swapchains are built here from ordinary images and handed to the
 * compositor as dma-bufs (Wayland, zwp_linux_dmabuf_v1) or copied out through shared memory
 * (Wayland wl_shm, X11 PutImage), or not shown at all (VK_EXT_headless_surface).
 */
#ifndef VKB_WSI_H
#define VKB_WSI_H

#include "vkb_client.h"

struct wl_display;
struct wl_surface;

enum vkb_surface_kind { VKB_SURF_WAYLAND, VKB_SURF_XCB, VKB_SURF_HEADLESS };

typedef struct vkb_surface {
    enum vkb_surface_kind kind;
    struct wl_display *wl_display;
    struct wl_surface *wl_surface;
    void *xcb_conn;      /* xcb_connection_t* */
    uint32_t xcb_window;
    struct vkb_wl_display_info *wl_info;   /* cached formats/modifiers per wl_display */
} vkb_surface;

typedef struct vkb_swapchain vkb_swapchain;
typedef struct vkb_pacer vkb_pacer; /* the frame pacer (pacer.c) */

typedef struct vkb_swap_image {
    VkImage image;
    VkDeviceMemory memory;
    int busy;                 /* held by the compositor (or acquired by the app) */
    int acquired;
    void *backend;            /* backend per-image data (wl_buffer...) */
    uint32_t stride, offset;
    uint64_t modifier;
    int dmabuf_fd;
    int ahb_sock;             /* gralloc image: socket holding its AHardwareBuffer for the compositor, -1 */
    vkb_swapchain *chain;
} vkb_swap_image;

typedef struct vkb_wsi_backend {
    /* Called after images exist; may fail (e.g. no dma-buf support) - the generic code then
     * retries with copy_present = 1. */
    VkResult (*init)(vkb_swapchain *sc);
    /* The image's rendering is complete (and, with copy_present, its pixels are in sc->staging). */
    VkResult (*present)(vkb_swapchain *sc, uint32_t index, const VkPresentRegionKHR *region);
    /* Wait until an image may be acquired (dispatching events); 0 timeout = poll. */
    VkResult (*wait_free)(vkb_swapchain *sc, uint64_t timeout_ns);
    void (*destroy)(vkb_swapchain *sc);
    /* Optional, dma-buf images only: present without waiting for the rendering. The backend
     * submits the waits itself and shows the image from its own thread once the GPU is done. */
    VkResult (*present_async)(vkb_swapchain *sc, VkQueue queue, uint32_t index, uint32_t nwait, const VkSemaphore *wait);
    /* Optional, with present_async: wait (bounded) until at most `max` of the frames given to
     * present_async are still rendering (VKBRIDGE_FRAME_QUEUE, wsi.c). */
    void (*wait_queue)(vkb_swapchain *sc, uint32_t max);
} vkb_wsi_backend;

struct vkb_swapchain {
    vkb_device *dev;
    vkb_surface *surface;
    VkSwapchainCreateInfoKHR info;
    VkFormat *view_formats;
    uint32_t view_format_count;
    uint32_t count;
    vkb_swap_image *images;
    const vkb_wsi_backend *backend;
    void *bdata;
    int dmabuf;               /* images are exported dma-bufs */
    int ahb;                  /* ... of gralloc AHardwareBuffers the compositor also gets (banner_ahb_v1) */
    int copy_present;         /* pixels are read back into `staging` at present */
    /* Read-back resources (copy_present). */
    VkBuffer staging;
    VkDeviceMemory staging_mem;
    uint8_t *staging_ptr;
    VkDeviceSize staging_size;
    int staging_cached;       /* CPU-cached pages (cached readback): invalidated before each read */
    VkCommandPool pool;
    uint32_t pool_family;
    VkCommandBuffer *copy_cbs;
    VkFence fence;
    VkResult status;          /* VK_ERROR_OUT_OF_DATE_KHR / SURFACE_LOST once broken */
    pthread_mutex_t lock;
    uint64_t presents;
    uint64_t acq_waits, acq_wait_ns; /* acquires that found no free image, and how long they waited (atomics; wsi_wayland.c logs them) */
    int retired;
    uint32_t lat_n;           /* the latency log's number of the present being made by the app's call (latency.c), 0 */
    /* Present ids (VK_KHR_present_id/id2) and their waits (present_wait/wait2), wsi.c. With `lock`. */
    pthread_cond_t pw_cond;   /* (CLOCK_MONOTONIC) a frame with an id shown; the chain retired or being destroyed */
    uint64_t pw_cur;          /* the id of the present being made by the app's call (0: none); the backend's from there */
    uint64_t pw_queued;       /* the highest id presented */
    uint64_t pw_done;         /* the highest id shown (vkb_wsi_pw_committed) */
    uint64_t pw_commits;      /* frames with an id handed to the compositor */
    struct { uint64_t id, t, commit; } pw_pending[16]; /* handed over, the compositor's word on them awaited */
    uint32_t pw_npending;
    uint32_t pw_waiters;      /* threads in vkWaitForPresent(2)KHR */
    int pw_closing;           /* vkDestroySwapchainKHR has begun: waits end (out of date) */
    uint64_t pw_waits, pw_timeouts, pw_wait_ns, pw_wait_max_ns, pw_unanswered; /* for the line at the end */
    /* Copy presents (X11, wl_shm, headless) whose frame was not rendered within VKBRIDGE_PRESENT_HOLD_MS:
     * shown by the swapchain's own thread once it is (wsi.c late_*). With `lock`. */
    pthread_cond_t late_cond; /* (CLOCK_MONOTONIC) a late frame shown, or one handed over */
    pthread_t late_thread;
    int late_on, late_quit, late_pending;
    uint32_t late_index, late_lat;
    uint64_t late_pid, late_t0;
    uint64_t late_frames, late_max_ns; /* for the line at the end */
    /* The frame pacer (VKBRIDGE_PACER=1, a DXVK app, dma-buf images; pacer.c), else NULL. Its present waits are answered by
     * the pacer: done at the present while not engaged, at the frame's GPU done + the hold while engaged (wsi.c). */
    vkb_pacer *pacer;
    uint64_t pq_cur;          /* the app's present call (CLOCK_MONOTONIC ns) for the presenting thread, 0: not paced */
    struct { uint64_t id, done; } pw_gd[16]; /* (with `lock`) present ids whose GPU work the presenting thread saw done, and when */
    uint64_t pw_gdone;        /* (with `lock`) the highest of them */
};

/* Formats every surface offers. */
extern const VkSurfaceFormatKHR vkb_wsi_formats[];
extern const uint32_t vkb_wsi_format_count;

/* Wayland */
extern const vkb_wsi_backend vkb_wsi_wayland_backend;
int vkb_wl_load(void);
VkResult vkb_wl_surface_extent(vkb_surface *s, VkExtent2D *e);
/* Connects the swapchain to the compositor ahead of its images (sc->bdata); 1 if they should be
 * gralloc buffers: the compositor advertises banner_ahb_v1 and wants them now (its mode event). */
int vkb_wl_ahb_wanted(vkb_swapchain *sc);
/* X11 */
extern const vkb_wsi_backend vkb_wsi_x11_backend;
int vkb_xcb_load(void);
VkResult vkb_xcb_surface_extent(vkb_surface *s, VkExtent2D *e);
void *vkb_xlib_to_xcb(void *dpy);
/* Headless */
extern const vkb_wsi_backend vkb_wsi_headless_backend;

/* Helpers in wsi.c */
/* VKBRIDGE_WSI_AHB=0, or the server cannot allocate AHardwareBuffers: no gralloc swapchains. */
int vkb_wsi_ahb_disabled(void);
/* The swapchain image sets kept for reuse of one device freed (vkDestroyDevice; wsi.c). */
void vkb_wsi_pool_drop_device(struct vkb_device *dev);
uint32_t vkb_drm_format(VkFormat f);
/* A frame's fence waited for in 10 s steps, a slow frame warned about once, VK_ERROR_DEVICE_LOST
 * only on the server's loss or past VKBRIDGE_PRESENT_TIMEOUT_S (default 120 s); reset after. */
VkResult vkb_wsi_wait_frame(vkb_swapchain *sc, VkFence fence);
/* A present with id `id` (0: none) was handed to the compositor (or the window system). feedback: the
 * compositor was asked when it is shown (wp_presentation): vkb_wsi_pw_shown then, else the frame
 * counts as shown now. */
void vkb_wsi_pw_committed(vkb_swapchain *sc, uint64_t id, int feedback);
/* The compositor showed the frame with that id (or dropped it for a later one). */
void vkb_wsi_pw_shown(vkb_swapchain *sc, uint64_t id);
/* A copy present's GPU part: the waits and the read-back submitted, then the frame waited for on the
 * CPU up to VKBRIDGE_PRESENT_HOLD_MS. *late = 1: not rendered by then, handed to the swapchain's
 * thread, which shows it once it is (the caller is done with it); else rendered (or an error). */
VkResult vkb_wsi_wait_rendering(vkb_swapchain *sc, VkQueue queue, uint32_t index, uint32_t nwait, const VkSemaphore *wait,
                                uint64_t pid, int *late);

/* The frame pacer (pacer.c). */
int vkb_pacer_wanted(void);                     /* VKBRIDGE_PACER=1 in the guest's environment, else the server's switch */
int vkb_pacer_setting(int *from, int *final);   /* the same without a line; *from 0 guest, 1 server, 2 default */
int vkb_wsi_present_wait_setting(const vkb_physdev *pd, int *from); /* vkb_wsi_present_wait_on without a line; *from VKB_PWF_* */
#define VKB_PWF_GUEST 0
#define VKB_PWF_SERVER 1
#define VKB_PWF_PACER 2
#define VKB_PWF_DEFAULT_DXVK 3
#define VKB_PWF_DEFAULT 4
vkb_pacer *vkb_pacer_create(vkb_swapchain *sc); /* NULL: not paced (off, not DXVK, copy present, DXVK paces itself) */
void vkb_pacer_destroy(vkb_pacer *p);
uint64_t vkb_pacer_hold_ns(vkb_pacer *p);       /* 0 while not engaged */
void vkb_pacer_pw_call(vkb_pacer *p);           /* a vkWaitForPresentKHR on the swapchain */
/* The presenting thread, per frame in present order: the app's present call, when its GPU work was seen done, how long the
 * thread waited for it, and the swapchain's acquires that waited so far. */
void vkb_pacer_frame(vkb_pacer *p, uint64_t present_ns, uint64_t done_ns, uint64_t waited_ns, uint64_t acq_total);
/* The presenting thread saw the GPU work of the frame with present id `id` done at `t` (paced swapchains). */
void vkb_wsi_pw_gpu_done(vkb_swapchain *sc, uint64_t id, uint64_t t);

#endif
