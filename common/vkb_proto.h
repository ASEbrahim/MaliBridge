/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * Bridge-specific (non-Vulkan) parts of the protocol: hello, server query, memory sharing.
 */
#ifndef VKB_PROTO_H
#define VKB_PROTO_H

#include <stdint.h>

#define VKB_DEFAULT_SOCKET_ENV "VKBRIDGE_SOCKET"

/* How host-visible memory is shared between the client's address space and the GPU. */
enum vkb_mem_strategy {
    VKB_MEM_NONE = 0,      /* no sharing works: host-visible memory is not offered */
    VKB_MEM_HOSTPTR = 1,   /* client memfd, server imports it (VK_EXT_external_memory_host) */
    VKB_MEM_DMABUF = 2,    /* server exports a dma-buf (VK_EXT_external_memory_dma_buf), client mmaps */
    VKB_MEM_AHB = 3,       /* server allocates an AHardwareBuffer (BLOB), client mmaps its dma-buf */
    VKB_MEM_DMAHEAP = 4,   /* server allocates from /dev/dma_heap and imports it as a dma-buf, client mmaps */
};

static inline const char *vkb_mem_strategy_name(uint32_t s)
{
    switch (s) {
    case VKB_MEM_HOSTPTR: return "hostptr";
    case VKB_MEM_DMABUF: return "dmabuf";
    case VKB_MEM_AHB: return "ahb";
    case VKB_MEM_DMAHEAP: return "dmaheap";
    default: return "none";
    }
}

/* Emulated features (server rewrites resources/shaders; client reports them as supported). */
#define VKB_EMU_BCN            (1u << 0)   /* BC1-BC7 textures decoded on upload */
#define VKB_EMU_DIVISOR        (1u << 1)   /* VK_EXT_vertex_attribute_divisor */
#define VKB_EMU_CLIP_DISTANCE  (1u << 2)   /* shaderClipDistance */
#define VKB_EMU_CULL_DISTANCE  (1u << 3)   /* shaderCullDistance (accepted, not applied) */
#define VKB_EMU_POINT_SIZE     (1u << 4)   /* strip PointSize from geometry/tessellation stages */
#define VKB_EMU_DEPTH_CLIP     (1u << 5)   /* VK_EXT_depth_clip_enable via depthClamp */
#define VKB_EMU_MAINT5         (1u << 6)   /* VK_KHR_maintenance5 on a driver without it */
#define VKB_EMU_DMABUF_EXPORT  (1u << 8)   /* dma-buf export of image memory: dma-heap pages imported */
#define VKB_EMU_DUAL_SRC       (1u << 9)   /* dualSrcBlend: first source used, second output dropped (DXVK 3) */
#define VKB_EMU_FILL_MODE      (1u << 10)  /* fillModeNonSolid: wireframe drawn solid (DXVK 3) */
#define VKB_EMU_MULTI_VIEWPORT (1u << 11)  /* multiViewport: first viewport only (DXVK 3) */
#define VKB_EMU_MAINT6         (1u << 12)  /* VK_KHR_maintenance6 on its 1.0 forms (DXVK 3) */
#define VKB_EMU_LOADSTORE_NONE (1u << 13)  /* VK_KHR_load_store_op_none via VK_EXT_load_store_op_none */
#define VKB_EMU_PUSH_DESC      (1u << 14)  /* VK_KHR_push_descriptor via per-command-buffer sets (vkd3d-proton) */
#define VKB_EMU_TEXEL_ALIGN    (1u << 15)  /* storage texel buffers at any texel offset reported (vkd3d-proton 3) */
#define VKB_EMU_DESC_LIMITS    (1u << 16)  /* 1M sampled/storage images and storage buffers per stage; cut to the GPU's (vkd3d-proton) */
#define VKB_EMU_OPAQUE_FD      (1u << 17)  /* OPAQUE_FD external memory as dma-bufs (DXVK shared resources) */
#define VKB_EMU_MULTI_DRAW     (1u << 18)  /* multiDrawIndirect: an indirect draw of n becomes n single draws (Mali-G57 r32-r38) */
#define VKB_EMU_ROBUSTNESS2    (1u << 7)   /* VK_EXT_robustness2 reported, not enforced (GameNative's r44 profile) */
#define VKB_EMU_DYN_RENDER     (1u << 20)  /* VK_KHR_dynamic_rendering as render passes (Mali Vulkan 1.1: Zink) */
#define VKB_EMU_BGR10_VTX      (1u << 21)  /* A2R10G10B10 SNORM/USCALED/SSCALED vertex fetch as the A2B10G10R10 ones, red and blue
                                            * swapped in the vertex shader (Zink's GL_ARB_vertex_type_2_10_10_10_rev: OpenGL 3.3) */

/* VKB_EMU_BGR10_VTX: the RGBA format a BGRA 10-10-10-2 vertex format is fetched as, or UNDEFINED */
static inline VkFormat vkb_bgr10_rgba(VkFormat f)
{
    switch (f) {
    case VK_FORMAT_A2R10G10B10_SNORM_PACK32: return VK_FORMAT_A2B10G10R10_SNORM_PACK32;
    case VK_FORMAT_A2R10G10B10_USCALED_PACK32: return VK_FORMAT_A2B10G10R10_USCALED_PACK32;
    case VK_FORMAT_A2R10G10B10_SSCALED_PACK32: return VK_FORMAT_A2B10G10R10_SSCALED_PACK32;
    default: return VK_FORMAT_UNDEFINED;
    }
}
#define VKB_EMU_UF11_VTX       (1u << 22)  /* B10G11R11_UFLOAT vertex fetch as R32_UINT, unpacked in the vertex shader
                                            * (Zink's GL_ARB_vertex_type_10f_11f_11f_rev: OpenGL 4.4 and up) */
#define VKB_EMU_TIMELINE       (1u << 23)  /* VK_KHR_timeline_semaphore on fences and binary semaphores (Mali r18/r19, Vulkan 1.1:
                                            * gamescope and Zink; server_timeline.c) */
#define VKB_EMU_DMABUF_AHB     (1u << 24)  /* VK_KHR_external_memory_fd + VK_EXT_external_memory_dma_buf over AHardwareBuffers where the
                                            * driver has no dma-buf memory (Kirin, Maleoon, Venus: gamescope; server_dmabuf_ahb.c) */
#define VKB_EMU_ROBUST2_ACCESS (1u << 19)  /* robustBufferAccess2/robustImageAccess2 reported where the driver's robustness2
                                            * has nullDescriptor only (Mali-G715 r54p3); core robustness on instead (DXVK 3) */

/* vkbQueryServer reply (per physical device). */
typedef struct vkb_server_pd_info {
    uint32_t strategy;             /* enum vkb_mem_strategy */
    uint32_t buffer_handle_types;  /* VkExternalMemoryHandleTypeFlags to put on buffers */
    uint32_t shareable_types;      /* server memory type bits that can be shared */
    uint32_t coherent_types;       /* of those, verified coherent without flushes */
    uint64_t import_alignment;     /* hostptr: minImportedHostPointerAlignment */
    uint32_t emu;                  /* VKB_EMU_* the server will perform */
    uint32_t emu_available;        /* VKB_EMU_* the server could perform */
    uint32_t real_api_version;
    uint32_t missing;              /* VKB_EMU_* features the GPU lacks (emulated or not) */
    uint32_t client_knobs;         /* VKB_KNOB_*: the client's switches from the server (per-game presets) */
    uint32_t cached_types;         /* server memory types whose shared pages can be CPU-cached (cached readback); 0: none */
    uint32_t cached_flags;         /* VKB_CACHED_*: who may use them */
    uint32_t image_types;          /* server memory types the self-test's images and plain buffers all allow; 0: unknown (older server) */
    uint32_t heap_max_mb;          /* VKBRIDGE_HEAP_MAX_MB from the server (a per-game preset): device-local heaps reported at most this; 0: nothing said */
    uint32_t spoof_gpu;            /* VKB_SPOOF_*: VKBRIDGE_SPOOF_GPU from the server; 0: nothing said */
    uint32_t dl_types;             /* server types for the client's device-local types without host access, one each (the images'
                                    * first), when resources disagree on one (XRing O3: images type 6, buffers 0x3); then also
                                    * every shareable type is offered. 0: one type, from image_types. An older server's reply
                                    * ends before it (the client reads what is there: vkb_dec_raw_tail); an older client reads
                                    * the first 64 bytes */
    uint32_t driver_id;            /* VkDriverId from the server's self-test (server_driverid.c): the client tells Arm's driver by it;
                                    * 0: unknown or an older server (once reserved), the vendor ID decides */
    uint32_t mem_flags;            /* VKB_MEMF_*; 0 from an older server */
    uint32_t present_timeout_s;    /* VKBRIDGE_PRESENT_TIMEOUT_S from the server (a preset, <cache dir>/env): how long a frame
                                    * may take before the client calls the device lost (client/wsi.c); VKB_PRESENT_TIMEOUT_NONE:
                                    * never; 0: nothing said (an older server too), the client's default */
    uint32_t hitch_ms;             /* VKBRIDGE_HITCH_MS from the server (a preset, <cache dir>/env): a frame that takes longer is
                                    * reported to the server (vkbHitch), which logs what it spent its time on; 0: off */
    uint32_t latlog;               /* VKBRIDGE_LATLOG from the server (a preset, <cache dir>/env): 1, the client times each frame's
                                    * stages and sends them (vkbLatency); 0: off, or an older server */
    uint32_t frame_queue;          /* VKBRIDGE_FRAME_QUEUE from the server (a preset, <cache dir>/env): 1-8, vkQueuePresentKHR
                                    * returns once at most this many minus one frames (its own included) are still rendering
                                    * (client/wsi.c); 0: off, or an older server */
    uint32_t present_wait;         /* VKBRIDGE_PRESENT_WAIT from the server (a preset, <cache dir>/env): VKB_PRESENT_WAIT_OFF, the
                                    * client offers no VK_KHR_present_id/present_wait (client/wsi.c); VKB_PRESENT_WAIT_ON; _COMMIT; 0:
                                    * nothing said (an older server too), on */
    uint32_t api_cap;              /* the highest Vulkan version this process is shown (VK_MAKE_API_VERSION(0, 1, minor, 0)); 0: no cap (an older server too) */
} vkb_server_pd_info;

#define VKB_PRESENT_WAIT_ON 1u
#define VKB_PRESENT_WAIT_OFF 2u
#define VKB_PRESENT_WAIT_COMMIT 3u  /* VKBRIDGE_PRESENT_WAIT=commit: a frame counts as shown once committed to the compositor */

/* vkbLatency (client/latency.c -> server_latency.c), no reply, VKBRIDGE_LATLOG. Sent at each
 * vkQueuePresentKHR of the process, before that present's own work goes to the server, so the
 * server's time of it is when the ordered stream reached the present. It numbers the present (n,
 * from 1 in the process) and carries the frames that are done since the last one: when each was
 * presented by the app, when its rendering was seen done, committed to the compositor, and shown
 * (wp_presentation), all CLOCK_MONOTONIC ns (one kernel: the server's and DroidDeck's clock too),
 * 0 when not seen. The server joins them with its own time of each and writes one line a frame. */
typedef struct vkb_lat_frame {
    uint64_t queued, gpu_done, committed, shown;
    uint32_t n, flags;             /* VKB_LAT_F_* */
} vkb_lat_frame;

#define VKB_LAT_F_ASYNC 1u         /* shown by the presenting thread (dma-buf images), else in the app's present call */
#define VKB_LAT_F_FEEDBACK 2u      /* the compositor was asked when it is shown (wp_presentation) */
#define VKB_LAT_F_DISCARDED 4u     /* ... and said it never was (a later frame replaced it) */
#define VKB_LAT_F_NO_ANSWER 8u     /* ... and had not answered when the record was sent */
#define VKB_LAT_F_FAILED 16u       /* the present failed (out of date, lost) */
#define VKB_LAT_F_OTHER_CLOCK 32u  /* the compositor said it was shown, on a clock other than CLOCK_MONOTONIC (no time) */
#define VKB_LAT_MAX_FRAMES 8

typedef struct vkb_lat_msg {
    uint32_t n;                    /* this present's number; 0: none (records only) */
    uint32_t count;                /* frames below (only these are sent) */
    uint64_t queued;               /* this present, as the app called it */
    vkb_lat_frame f[VKB_LAT_MAX_FRAMES];
} vkb_lat_msg;

/* vkbHitch (client/hitch.c -> server_hitch.c), no reply. A frame over the threshold: its window in
 * CLOCK_MONOTONIC ns (one kernel: the server's clock too) and what the client saw the process wait
 * for in it. Sent once with frame 0 when the client turns it on (the server then keeps its events).
 * Each vkb_hitch_wait: the time in it summed over threads, how many, the longest and where. */
typedef struct vkb_hitch_wait {
    uint64_t ns, n, max_ns;
    uint32_t max_cmd;              /* VKB_CMD_* of the longest (VKB_CMD_COUNT: the swapchain's acquire) */
    char max_thread[16];           /* the thread that waited longest (its name) */
    uint32_t pad;
} vkb_hitch_wait;

enum { VKB_HITCH_W_APP, VKB_HITCH_W_PRESENT, VKB_HITCH_W_RT, VKB_HITCH_W_COUNT };

typedef struct vkb_hitch_msg {
    uint64_t frame;                /* presents so far in the process (0: turned on) */
    uint64_t t_prev, t_start, t_end; /* the frame before's start, this frame's start and end (its present) */
    uint32_t threshold_ms, pad;
    vkb_hitch_wait w[VKB_HITCH_W_COUNT]; /* the app's fence/semaphore/idle waits, the swapchain's own, other round trips */
} vkb_hitch_msg;

#define VKB_PRESENT_TIMEOUT_NONE 0xffffffffu

/* vkb_server_pd_info.mem_flags */
#define VKB_MEMF_UNFLAGGED_BUFFERS 1u /* shared buffers are made without buffer_handle_types (0): the only method the server's
                                       * self-test passed (server_memory.c, the Xclipse 530) */

/* vkb_server_pd_info.client_knobs: two bits per switch, 0 when the server has nothing to say (an
 * older server sent zeros here), else VKB_KNOB_OFF or VKB_KNOB_ON. What the server has for the
 * client: the game's preset file, <cache dir>/env, its own environment, the game's built-in preset
 * (server_presets.c). The guest's own environment still wins (client/conn.c). */
#define VKB_KNOB_OFF 1u
#define VKB_KNOB_ON 2u
#define VKB_KNOB_DESC_BATCH_SHIFT 0   /* VKBRIDGE_DESC_BATCH / VKBRIDGE_NO_DESC_BATCH */
#define VKB_KNOB_QUERY_MIRROR_SHIFT 2 /* VKBRIDGE_QUERY_MIRROR */
#define VKB_KNOB_SYNCFD_SIDE_SHIFT 4   /* VKBRIDGE_SYNCFD_ORDER_MS on: queued requests ahead of a sync-fd export go on the
                                        * flusher's connection (client/conn.c, server_syncfd.c) */
#define VKB_KNOB_SYNC_MIRROR_SHIFT 6  /* VKBRIDGE_SYNC_MIRROR */
#define VKB_KNOB_DMABUF_SYNC_SHIFT 8   /* VKBRIDGE_DMABUF_SYNC_MS: 0 off (client/dmabuf_sync.c, implicit sync on kernels without it) */
#define VKB_KNOB_DMABUF_SYNC_FAKE_SHIFT 10 /* VKBRIDGE_DMABUF_SYNC_FAKE_OLD: the kernel taken as one without dma-buf sync files */
#define VKB_KNOB_GET(word, shift) (((word) >> (shift)) & 3u)

/* vkb_server_pd_info.spoof_gpu (once reserved: an older server sends 0, an older client ignores
 * it). The GPU identity the app is shown (client/physdev.c); the server keeps the real one. */
#define VKB_SPOOF_OFF 1u
#define VKB_SPOOF_NVIDIA 2u
#define VKB_SPOOF_AMD 3u

/* vkAllocateMemory wire extras (after VkMemoryAllocateInfo). */
#define VKB_ALLOC_PLAIN 0u
#define VKB_ALLOC_SHARED 1u
#define VKB_ALLOC_SHARED_CACHED 2u /* shared, with CPU-cached pages: the client flushes and invalidates */

/* vkb_server_pd_info.cached_flags. Cached readback: shared pages from a cached dma-buf (the
 * dma-heap's "system" heap, not "system-uncached"), CPU cache maintenance by DMA_BUF_IOCTL_SYNC on
 * vkFlushMappedMemoryRanges / vkInvalidateMappedMemoryRanges (server_memory.c). An older server
 * sent zeros in these fields: nothing cached. */
#define VKB_CACHED_WSI 1u  /* the client's own frame readback (the copy present path) */
#define VKB_CACHED_APPS 2u /* also offered to apps, as a HOST_CACHED type without HOST_COHERENT */

/* Extra structure the client hangs on create infos to tell the server which shared handle
 * types were injected (so the server can remove them if the driver refuses). Not on the wire. */

#endif
