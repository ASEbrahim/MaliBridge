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
} vkb_server_pd_info;

/* vkb_server_pd_info.client_knobs: two bits per switch, 0 when the server has nothing to say (an
 * older server sent zeros here), else VKB_KNOB_OFF or VKB_KNOB_ON. What the server has for the
 * client: the game's preset file, <cache dir>/env, its own environment, the game's built-in preset
 * (server_presets.c). The guest's own environment still wins (client/conn.c). */
#define VKB_KNOB_OFF 1u
#define VKB_KNOB_ON 2u
#define VKB_KNOB_DESC_BATCH_SHIFT 0   /* VKBRIDGE_DESC_BATCH / VKBRIDGE_NO_DESC_BATCH */
#define VKB_KNOB_QUERY_MIRROR_SHIFT 2 /* VKBRIDGE_QUERY_MIRROR */
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
