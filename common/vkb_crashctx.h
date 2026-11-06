/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * What a server thread's current driver call was given, for the crash line (server_crashctx.c,
 * server_main.c). The generated handlers of a few commands note their arguments right before the
 * driver's call: plain stores into thread-local memory, nothing waits, nothing is looked up. Only
 * the crash handler reads them (and the records server_crashctx.c keeps of recently made images,
 * memory and pipelines, which it looks the noted handles up in).
 */
#ifndef VKB_CRASHCTX_H
#define VKB_CRASHCTX_H

#include <stdint.h>
#include <vulkan/vulkan.h>

enum { VKB_ARGS_NONE = 0, VKB_ARGS_BIND_IMAGE = 1, VKB_ARGS_BIND_IMAGE2 = 2, VKB_ARGS_ALLOC = 3 };

typedef struct vkb_srv_args {
    uint32_t kind; /* VKB_ARGS_*: cleared at each request (server_conn.c run_request) */
    uint32_t n;
    uint64_t a, b, c, d;
    const void *p; /* the call's own array (alive while the call runs); for an allocation, its path (a literal) */
} vkb_srv_args;
extern __thread vkb_srv_args vkb_srv_args_now;

/* Inside a command buffer's replay (server_conn.c vkb_sv_vkEndCommandBuffer, which clears them at
 * its start): the pipelines the stream has bound so far and the last vkCmdSetLineWidth. */
extern __thread uint64_t vkb_srv_replay_gfx, vkb_srv_replay_comp;
extern __thread float vkb_srv_replay_lw;
extern __thread uint32_t vkb_srv_replay_lw_set;

static inline void vkb_srv_note_bind_image(VkImage image, VkDeviceMemory memory, VkDeviceSize offset)
{
    vkb_srv_args_now.a = (uint64_t)(uintptr_t)image;
    vkb_srv_args_now.b = (uint64_t)(uintptr_t)memory;
    vkb_srv_args_now.c = (uint64_t)offset;
    vkb_srv_args_now.kind = VKB_ARGS_BIND_IMAGE;
}

static inline void vkb_srv_note_bind_image2(uint32_t n, const VkBindImageMemoryInfo *infos)
{
    vkb_srv_args_now.n = n;
    vkb_srv_args_now.p = infos;
    vkb_srv_args_now.kind = VKB_ARGS_BIND_IMAGE2;
}

/* vkAllocateMemory (server_memory.c), before the driver's calls: size, memory type, the dedicated
 * image, how it is made (a string literal) and the dma-buf's inode (an import's fd, or the export
 * emulation's pages once it has them; 0 none). the one server died in Mali's
 * vkAllocateMemory with two dma-buf allocations in flight and the line could not say which. */
static inline void vkb_srv_note_alloc(uint64_t size, uint32_t type, uint64_t dedicated_image, const char *path, uint64_t ino)
{
    vkb_srv_args_now.a = size;
    vkb_srv_args_now.b = type;
    vkb_srv_args_now.c = dedicated_image;
    vkb_srv_args_now.d = ino;
    vkb_srv_args_now.p = path;
    vkb_srv_args_now.kind = VKB_ARGS_ALLOC;
}

/* Returns non-zero to leave the bind out: VK_NULL_HANDLE (invalid; RADV crashes on it), which also
 * leaves its bind point with nothing bound, so the draws after it are left out too (server_conn.c). */
static inline int vkb_srv_note_bind_pipeline(VkPipelineBindPoint bp, VkPipeline p)
{
    if (bp == VK_PIPELINE_BIND_POINT_GRAPHICS) vkb_srv_replay_gfx = (uint64_t)(uintptr_t)p;
    else if (bp == VK_PIPELINE_BIND_POINT_COMPUTE) vkb_srv_replay_comp = (uint64_t)(uintptr_t)p;
    return p == VK_NULL_HANDLE;
}

static inline void vkb_srv_note_line_width(float w)
{
    vkb_srv_replay_lw = w;
    vkb_srv_replay_lw_set = 1;
}

/* vkCreateImage, after the driver made it: recorded with what the driver asks for it (and its DRM
 * format modifier) and the format and tiling the client asked for. */
struct vkb_srv_call;
void vkb_srv_note_image(struct vkb_srv_call *c, VkDevice device, const VkImageCreateInfo *ci, VkImage image, VkFormat asked_format,
                        VkImageTiling asked_tiling);

/* vkCreateGraphicsPipelines: what the first VKB_PIPE_SNAP create infos asked for, taken before the
 * driver's call (the emulations rewrite the infos in place), recorded with the handles after it. */
#define VKB_PIPE_SNAP 8
typedef struct vkb_srv_pipe_snap {
    float line_width;
    uint8_t has_rs, dyn_line_width, topology, polygon_mode;
} vkb_srv_pipe_snap;
void vkb_srv_note_pipelines_pre(vkb_srv_pipe_snap *s, uint32_t n, const VkGraphicsPipelineCreateInfo *ci);
void vkb_srv_note_pipelines(const vkb_srv_pipe_snap *s, uint32_t n, const VkPipeline *out);

#endif
