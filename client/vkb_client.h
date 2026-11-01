/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * MaliBridge - client ICD (libvulkan_droidbridge.so, glibc).
 *
 * A Vulkan driver for the Vulkan loader inside the Linux session. It owns no GPU: every call is
 * encoded and executed by the server (server/) on the device's real driver.
 * Command buffers are recorded locally and shipped at vkEndCommandBuffer; mapped memory is
 * shared pages (see server_memory.c); presentation (WSI) is implemented here.
 */
#ifndef VKB_CLIENT_H
#define VKB_CLIENT_H

#define VK_USE_PLATFORM_WAYLAND_KHR 1
#define VK_USE_PLATFORM_XCB_KHR 1
#define VK_USE_PLATFORM_XLIB_KHR 1

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>

#include "vkb_wire.h"
#include "vkb_proto.h"
#include "vkb_caps.h"
#include "vkb_log.h"
#include <vulkan/vulkan.h>
#include <vulkan/vk_icd.h>

#define VKB_EXPORT __attribute__((visibility("default")))
#define VKB_API_VERSION VK_MAKE_API_VERSION(0, 1, 4, VK_HEADER_VERSION)

/* ------------------------------------------------------------------ small hash map (u64 -> ptr) */
typedef struct vkb_map {
    uint64_t *keys;
    void **vals;
    uint32_t cap, count;
    pthread_mutex_t lock;
} vkb_map;
void vkb_map_init(vkb_map *m);
void vkb_map_destroy(vkb_map *m);
void vkb_map_put(vkb_map *m, uint64_t k, void *v);
void *vkb_map_get(vkb_map *m, uint64_t k);
void *vkb_map_remove(vkb_map *m, uint64_t k);

/* ------------------------------------------------------------------ dispatchable objects */
typedef struct vkb_object {
    VK_LOADER_DATA loader;     /* must be first: the loader keeps its dispatch pointer here */
    VkObjectType type;
    uint64_t remote;           /* the server's handle */
    uint32_t table;            /* the server's dispatch table for calls on this object */
} vkb_object;

typedef struct vkb_mem_type_map {
    uint32_t server_type;
    uint8_t shared;
    uint8_t cached;          /* shared with CPU-cached pages (cached readback, VKB_CACHED_APPS) */
} vkb_mem_type_map;

struct vkb_instance;

typedef struct vkb_physdev {
    vkb_object obj;
    struct vkb_instance *instance;
    vkb_server_pd_info info;
    vkb_pd_caps caps;
    VkPhysicalDeviceProperties props;
    VkPhysicalDeviceMemoryProperties server_mem;
    VkPhysicalDeviceMemoryProperties client_mem;   /* what the app sees */
    vkb_mem_type_map type_map[VK_MAX_MEMORY_TYPES];
    int readback_type;                              /* server type for the client's own cached frame readback; -1: none */
    uint32_t heap_map[VK_MAX_MEMORY_HEAPS];         /* client heap -> server heap */
    VkExtensionProperties *exts;                    /* filtered device extensions */
    uint32_t next;
    uint32_t aliased;                               /* vkb_ext_aliases[i] offered under the app name: bit i */
    int ready;
    pthread_mutex_t lock;
    struct vkb_physdev *list_next;
} vkb_physdev;

typedef struct vkb_instance {
    vkb_object obj;
    uint32_t api_version;
    vkb_physdev *pds;
    pthread_mutex_t lock;
    uint8_t ext_surface, ext_wayland, ext_xcb, ext_xlib, ext_headless, ext_surface_caps2, ext_debug_utils;
    uint8_t dxvk;            /* the app's engine is DXVK (its VkApplicationInfo): no present waits by default (wsi.c) */
} vkb_instance;

struct vkb_queue;

typedef struct vkb_device {
    vkb_object obj;
    vkb_physdev *pd;
    vkb_instance *instance;
    struct vkb_queue *queues;
    pthread_mutex_t lock;
    vkb_map memories;        /* VkDeviceMemory -> vkb_memory* */
    vkb_map templates;       /* VkDescriptorUpdateTemplate -> vkb_template* */
    vkb_map pools;           /* VkCommandPool -> vkb_cmdpool* */
    vkb_map unshared;        /* VkBuffer/VkImage that cannot live in shared memory -> (void*)1 */
    vkb_map shared_images;   /* VkImage that may live in shared memory -> (void*)1 */
    struct vkb_bv_cache *bufviews; /* bufview.c */
    struct vkb_rq_cache *buf_reqs; /* memory.c: buffer requirements by create info */
    struct vkb_qm_dev *qm;         /* query.c: query result mirrors */
    struct vkb_sm_dev *sm;         /* syncmirror.c: fence and event status mirrors */
    const char **enabled_exts;
    uint32_t enabled_ext_count;
    uint8_t ext_swapchain;
    uint8_t present_ids;     /* VK_KHR_present_id or present_id2 enabled: the swapchain asks the compositor when frames are shown (wsi.c) */
    uint32_t api_version;
} vkb_device;

typedef struct vkb_queue {
    vkb_object obj;
    vkb_device *device;
    uint32_t family, index, flags;
    struct vkb_queue *next;
} vkb_queue;

typedef struct vkb_cmdbuf {
    vkb_object obj;
    vkb_device *device;
    VkCommandPool pool;
    VkCommandBufferLevel level;
    vkb_enc stream;
    size_t entry;            /* offset of the open entry's payload */
    int recording;
    int broken;
    uint32_t family;         /* the pool's queue family (UINT32_MAX: unknown) */
    struct vkb_qop *qops;    /* query.c: queries ended / reset by the recorded commands */
    uint32_t nqops, qops_cap;
    int rendering_suspends;  /* query.c: the open vkCmdBeginRendering said VK_RENDERING_SUSPENDING_BIT */
    struct vkb_evop *evops;  /* syncmirror.c: events set / reset by the recorded commands */
    uint32_t nevops, evops_cap;
    struct vkb_cmdbuf *pool_next, *pool_prev;
    uint64_t qv_rec;         /* qvisible.c: this recording's number */
} vkb_cmdbuf;

typedef struct vkb_cmdpool {
    vkb_cmdbuf *head;
    uint32_t family;
} vkb_cmdpool;

typedef struct vkb_memory {
    void *ptr;               /* client mapping (shared allocations) */
    int placed;              /* ptr is where the app placed it (VK_EXT_map_memory_placed) */
    VkDeviceSize size;
    uint32_t client_type;
    int shared;
    int cached;              /* CPU-cached pages: reads need vkInvalidateMappedMemoryRanges first */
} vkb_memory;

static inline vkb_device *vkb_dev(VkDevice d) { return (vkb_device *)d; }
static inline vkb_physdev *vkb_pd(VkPhysicalDevice p) { return (vkb_physdev *)p; }
int vkb_physdev_has_ext(VkPhysicalDevice p, const char *list); /* "A|B": the device (as the app sees it) has one */
static inline vkb_instance *vkb_inst(VkInstance i) { return (vkb_instance *)i; }
static inline vkb_cmdbuf *vkb_cb(VkCommandBuffer c) { return (vkb_cmdbuf *)c; }

/* ------------------------------------------------------------------ calls */
typedef struct vkb_call {
    vkb_enc e;
    vkb_dec d;
    uint32_t cmd, table, flags;
    struct vkb_conn *conn;
} vkb_call;

void vkb_call_begin(vkb_call *c, uint32_t cmd, uint32_t table);
/* Sends the request and waits for the reply. 0 = failed (the call is already released). */
int vkb_call_exec(vkb_call *c);
void vkb_call_end(vkb_call *c);
/* Sends without waiting for a reply (void commands without outputs); ordered by vkb_msg_hdr.seq. */
int vkb_call_exec_async(vkb_call *c);
/* Client-only call flag: a status poll, sent with no barrier (vkb_call_poll_next marks this
 * thread's next call so). vkb_async_seq_last: the seq of this thread's last async request;
 * vkb_async_ran: 1 once a synchronous call has seen every async request up to seq run. */
#define VKB_CALL_POLL 0x40000000u
void vkb_call_poll_next(void);
uint64_t vkb_async_seq_last(void);
int vkb_async_ran(uint64_t seq);
int vkb_async_results(void);
/* Descriptor batching: asynchronous requests framed as one vkbBatch message (on by default, VKBRIDGE_DESC_BATCH=0 or
 * VKBRIDGE_NO_DESC_BATCH=1 off) and vkUpdateDescriptorSets packed (off unless VKBRIDGE_DESC_PACK=1); vkb_async_framed
 * and vkb_desc_packed are its two halves (conn.c). */
int vkb_desc_batch(void);
/* VKBRIDGE_MODE in the guest: safe (the default) or performance (the client's defaults by mode) */
enum { VKB_CLIENT_MODE_SAFE, VKB_CLIENT_MODE_PERFORMANCE };
int vkb_client_mode(void);
int vkb_async_framed(void);
int vkb_desc_packed(void);
/* vkb_server_pd_info.client_knobs from the first vkbQueryServer (conn.c): the server's switches */
void vkb_client_knobs_set(uint32_t w);
int vkb_query_mirror_wanted(void);
int vkb_sync_mirror_wanted(void);
/* The server's switch at shift (VKB_KNOB_*): 1 on, 0 off, -1 nothing said (*known 0: not in yet) */
int vkb_client_knob(int shift, int *known);
/* Implicit sync for dma-bufs on kernels without dma-buf sync files (dmabuf_sync.c) */
void vkb_dmabuf_sync_submit(void);
void vkb_stats_count(uint32_t cmd, int async, size_t bytes);
extern uint32_t vkb_stat_refused; /* refused vkAllocateDescriptorSets, for VKBRIDGE_STATS */
extern uint32_t vkb_stat_pool_full; /* pools handed back full after their growth (descriptor.c) */
uint32_t vkb_table_of(const void *dispatchable);
int vkb_connected(void);
/* Per-thread transport cleanup for fork children. */
void vkb_conn_after_fork(void);

/* Command streams (cmdbuf.c). */
vkb_enc *vkb_cmd_record_begin(VkCommandBuffer cb, uint32_t cmd);
void vkb_cmd_record_end(VkCommandBuffer cb);

/* Dispatchable objects created by the server (generated stubs call this). */
void *vkb_wrap(VkObjectType type, const void *parent, uint64_t remote);

/* physdev.c */
int vkb_physdev_ready(vkb_physdev *pd);
uint32_t vkb_mem_bits_to_client(vkb_physdev *pd, uint32_t server_bits, int allow_shared);
/* a buffer's: also the shared types where its mask has host-visible types but none of them */
uint32_t vkb_mem_buffer_bits_to_client(vkb_physdev *pd, uint32_t server_bits, int allow_shared);

/* memory.c */
void vkb_memory_init_device(vkb_device *d);
VkResult vkb_memory_alloc_readback(VkDevice device, VkBuffer buffer, VkDeviceSize size, VkDeviceMemory *pMemory);
int vkb_memory_is_cached(VkDevice device, VkDeviceMemory memory);
void vkb_memory_destroy_device(vkb_device *d);

/* bufview.c */
void vkb_bufview_init_device(vkb_device *d);
void vkb_bufview_destroy_device(vkb_device *d);
void vkb_bufview_buffer_destroyed(vkb_device *d, VkBuffer buffer);

/* syncmirror.c */
void vkb_quiet_over(void); /* icd.c: the poll quiet windows end */
void vkb_hostop_note(uint64_t key); /* icd.c: a host reset/set of a fence (handle*2) or event (handle*2+1) */
void vkb_event_hostop(VkEvent event); /* icd.c */
void vkb_sm_init_device(vkb_device *d);
void vkb_sm_destroy_device(vkb_device *d);
void vkb_sm_fence_created(vkb_device *d, const VkFenceCreateInfo *ci, VkFence fence);
void vkb_sm_fence_destroyed(vkb_device *d, VkFence fence);
void vkb_sm_fence_unknown(vkb_device *d, VkFence fence);
void vkb_sm_fences_reset(vkb_device *d, uint32_t n, const VkFence *fences);
VkResult vkb_sm_queue_submit(VkQueue queue, uint32_t n, const VkSubmitInfo *si, VkFence fence);
VkResult vkb_sm_queue_submit2(VkQueue queue, uint32_t n, const VkSubmitInfo2 *si, VkFence fence);
int vkb_sm_fence_status(vkb_device *d, VkFence fence, VkResult *out);
VkResult vkb_sm_wait_fences(vkb_device *d, uint32_t n, const VkFence *fences, VkBool32 all, uint64_t timeout);
void vkb_sm_event_created(vkb_device *d, const VkEventCreateInfo *ci, VkEvent ev);
void vkb_sm_event_destroyed(vkb_device *d, VkEvent ev);
void vkb_sm_event_host(vkb_device *d, VkEvent ev, uint32_t set);
int vkb_sm_event_status(vkb_device *d, VkEvent ev, VkResult *out);
void vkb_sm_cb_event(vkb_cmdbuf *cb, VkEvent ev, uint32_t set);
void vkb_sm_cb_clear(vkb_cmdbuf *cb);
void vkb_sm_cb_execute(vkb_cmdbuf *primary, const vkb_cmdbuf *secondary);
void vkb_sm_cb_end(vkb_cmdbuf *cb);
void vkb_sm_sem_created(vkb_device *d, const VkSemaphoreCreateInfo *ci, VkSemaphore s);
void vkb_sm_sem_destroyed(vkb_device *d, VkSemaphore s);
void vkb_sm_sem_settle(vkb_device *d, VkSemaphore s);
void vkb_sm_sem_unknown(vkb_device *d, VkSemaphore s);
void vkb_sm_sem_host_signal(vkb_device *d, VkSemaphore s, uint64_t v);
int vkb_sm_sem_value(vkb_device *d, VkSemaphore s, uint64_t *out, VkResult *r);
VkResult vkb_sm_wait_sems(vkb_device *d, const VkSemaphoreWaitInfo *wi, uint64_t timeout);

/* query.c */
void vkb_query_init_device(vkb_device *d);
void vkb_query_destroy_device(vkb_device *d);
void vkb_query_cb_clear(vkb_cmdbuf *cb);
void vkb_query_cb_end(vkb_cmdbuf *cb);
void vkb_query_cb_execute(vkb_cmdbuf *primary, const vkb_cmdbuf *secondary);
const char *vkb_query_why_wire(void);
/* qvisible.c: VKBRIDGE_QUERY_VISIBLE_MS, the answer for an occlusion query its done work left "not ready" */
int vkb_qvis_on(void);
void vkb_qvis_pool_create(VkQueryPool pool, const VkQueryPoolCreateInfo *ci);
void vkb_qvis_pool_destroy(VkQueryPool pool);
void vkb_qvis_reset(VkQueryPool pool, uint32_t first, uint32_t count);
void vkb_qvis_end(vkb_cmdbuf *cb, VkQueryPool pool, uint32_t query);
void vkb_qvis_cb_begin(vkb_cmdbuf *cb);
void vkb_qvis_cb_end(vkb_cmdbuf *cb);
void vkb_qvis_cb_execute(vkb_cmdbuf *primary, vkb_cmdbuf *secondary);
void vkb_qvis_submit(VkCommandBuffer cb);
void vkb_qvis_after(vkb_device *d, VkQueryPool pool, uint32_t first, uint32_t count, size_t size, void *data, VkDeviceSize stride,
                    VkQueryResultFlags flags, VkResult *r);
VkResult vkb_qvis_imitate(VkQueryPool pool, uint32_t first, uint32_t count, VkResult r);
void vkb_query_wire_answer(vkb_device *d, VkQueryPool pool, uint32_t first, uint32_t count, VkResult r);
int vkb_query_results(vkb_device *d, VkQueryPool pool, uint32_t first, uint32_t count, size_t size, void *data,
                      VkDeviceSize stride, VkQueryResultFlags flags, VkResult *result);

/* icd.c */
PFN_vkVoidFunction vkb_lookup_proc(const char *name, int level_max, vkb_device *dev, vkb_instance *inst);
int vkb_device_ext_enabled(const vkb_device *d, const char *name);

/* memory.c */
void *vkb_chain_clone(const void *pNext, void **mem);
int vkb_is_client_device_ext(const char *n);

/* misc */
int vkb_memfd(const char *name, size_t size);
void vkb_frames_present(void); /* frames.c: frame statistics for the server's session report */
void vkb_frames_swapchain(void); /* frames.c: a swapchain made */
void vkb_frames_external(void);  /* frames.c: memory the app exported or imported as an fd */
uint64_t vkb_frames_presented(void); /* frames.c: presents so far in this process */
/* hitch.c: the per-hitch line's client half (VKBRIDGE_HITCH_MS). vkb_hitch_active: 0 while off. */
extern int vkb_hitch_active;
extern __thread int vkb_hitch_in_present;   /* inside the swapchain's own wait (wsi.c) */
uint64_t vkb_hitch_now(void);
void vkb_hitch_server_ms(uint32_t ms);      /* vkbQueryServer's hitch_ms (physdev.c) */
void vkb_hitch_rt(uint32_t cmd, uint64_t ns); /* a round trip (conn.c) */
void vkb_hitch_waited(int kind, uint32_t cmd, uint64_t ns); /* VKB_HITCH_W_*; cmd VKB_CMD_COUNT: an acquire */
void vkb_hitch_present(void);              /* at each vkQueuePresentKHR (wsi.c) */
/* latency.c: the latency log's client half (VKBRIDGE_LATLOG). vkb_lat_active: 0 while off. A frame
 * number 0 is no frame: every call below takes it and does nothing. */
extern int vkb_lat_active;
enum { VKB_LAT_DONE, VKB_LAT_SHOWN };
int vkb_lat_on(void);                       /* decides from the guest's environment the first time */
uint64_t vkb_lat_now(void);
void vkb_lat_server(uint32_t on);
/* VK_KHR_present_id/present_wait (and id2/wait2) offered: unless VKBRIDGE_PRESENT_WAIT=0 in the guest, or the
 * server's vkbQueryServer present_wait says off (wsi.c) */
int vkb_wsi_present_wait_on(const vkb_physdev *pd);
/* VKBRIDGE_PRESENT_WAIT=commit: a frame counts as shown once committed, no compositor feedback asked (wsi.c) */
int vkb_wsi_present_wait_commit(const vkb_physdev *pd);
/* their features (VkPhysicalDevicePresent{Id,Wait,Id2,Wait2}FeaturesKHR) in a vkGetPhysicalDeviceFeatures2 chain */
void vkb_wsi_patch_features(const vkb_physdev *pd, VkBaseOutStructure *b);
void vkb_wsi_frame_queue_server(uint32_t n); /* vkbQueryServer's frame_queue (physdev.c); the guest's VKBRIDGE_FRAME_QUEUE beats it */           /* vkbQueryServer's latlog (physdev.c) */
uint32_t vkb_lat_present(void);             /* at each vkQueuePresentKHR, first: the frame's number, 0 off */
void vkb_lat_stamp(uint32_t n, int what, uint64_t t); /* VKB_LAT_DONE / VKB_LAT_SHOWN at t (0: now) */
void vkb_lat_flag(uint32_t n, uint32_t flags);        /* VKB_LAT_F_* (vkb_proto.h) */
void vkb_lat_presented(uint32_t n, int ok);           /* the present call is over: committed now, or failed */
void vkb_lat_flush(void);                             /* a swapchain destroyed: the completed records sent */
/* dxvkcfg.c: what DXVK was given (DXVK_CONFIG, DXVK_CONFIG_FILE, dxvk.conf) and whether its own log
 * shows it applied, logged once per process at the first present */
void vkb_dxvkcfg_engine(const VkApplicationInfo *app);
void vkb_dxvkcfg_check(void);
int vkb_dxvkcfg_own_latency(void); /* DXVK with a maxFrameLatency in its DXVK_CONFIG (the frame queue stands aside, wsi.c) */

/* conn.c: logs the first time each call returns each error. The test stays at the call site, so a
 * call that succeeds costs one compare. */
void vkb_first_error(const char *call, VkResult r);
static inline VkResult vkb_note_error(const char *call, VkResult r)
{
    if (__builtin_expect(r < 0, 0)) vkb_first_error(call, r);
    return r;
}

/* The generated wire calls (common/gen): a lost device is logged from every call that returns it. */
static inline VkResult vkb_note_lost(const char *call, VkResult r)
{
    if (__builtin_expect(r == VK_ERROR_DEVICE_LOST, 0)) vkb_first_error(call, r);
    return r;
}

/* Calls that may only return VK_SUCCESS or an error (a submit, a sparse bind, the idle waits): any
 * other result is logged as well. A driver answering one of them with a positive code is a driver
 * bug the game cannot handle (DXVK latches it as a device reset). */
static inline VkResult vkb_note_fail(const char *call, VkResult r)
{
    if (__builtin_expect(r != VK_SUCCESS, 0)) vkb_first_error(call, r);
    return r;
}

/* vkWaitForFences / vkWaitSemaphores: errors, and with no timeout (UINT64_MAX) anything but
 * VK_SUCCESS - such a wait may not time out, and DXVK's timeline wait treats VK_TIMEOUT as a lost
 * device without a log line of its own (Stray on the Dimensity 9500). */
static inline VkResult vkb_note_wait(const char *call, const char *endless, VkResult r, uint64_t timeout)
{
    if (__builtin_expect(r < 0, 0)) vkb_first_error(call, r);
    else if (__builtin_expect(r != VK_SUCCESS && timeout == UINT64_MAX, 0)) vkb_first_error(endless, r);
    return r;
}

static inline VkResult vkb_device_lost(const char *call)
{
    vkb_first_error(call, VK_ERROR_DEVICE_LOST);
    return VK_ERROR_DEVICE_LOST;
}

#endif
