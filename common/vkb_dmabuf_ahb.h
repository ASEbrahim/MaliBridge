/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * What the bridge server tells DroidDeck's compositor (the same process) about a dma-buf it handed
 * out over an AHardwareBuffer (server/server_dmabuf_ahb.c, VKB_EMU_DMABUF_AHB): the buffer and
 * the image its memory was made for, so the compositor can make the same image over the same pages.
 *
 *   int vkb_dmabuf_ahb_lookup(int fd, vkb_dmabuf_ahb_info *out);
 *     1   fd is one of the bridge's: out filled, out->ahb holds one reference for the caller
 *         (AHardwareBuffer_release it once imported or not needed);
 *     0   not one of the bridge's (import it as a dma-buf, as before);
 *    -1   out->abi / out->struct_size not understood (nothing filled).
 *
 * The caller sets abi and struct_size. Fields are only ever added at the end; a larger struct
 * from a newer caller is filled up to what this server knows, a smaller one is refused.
 */
#ifndef VKB_DMABUF_AHB_H
#define VKB_DMABUF_AHB_H

#include <stdint.h>

#define VKB_DMABUF_AHB_ABI 1
#define VKB_DMABUF_AHB_LOOKUP "vkb_dmabuf_ahb_lookup"

typedef struct vkb_dmabuf_ahb_info {
    uint32_t abi;          /* in: VKB_DMABUF_AHB_ABI */
    uint32_t struct_size;  /* in: sizeof(vkb_dmabuf_ahb_info) */
    void *ahb;             /* out: AHardwareBuffer* (BLOB), one reference for the caller */
    uint64_t bytes;        /* out: the pages' size */
    uint32_t has_image;    /* out: 1 when the memory was made for the image described below */
    uint32_t format;       /* VkFormat, as the driver holds the image */
    uint32_t width, height;
    uint32_t tiling;       /* VkImageTiling (LINEAR or OPTIMAL) */
    uint32_t usage;        /* VkImageUsageFlags */
    uint32_t flags;        /* VkImageCreateFlags */
    uint32_t mip_levels, array_layers, samples;
    uint32_t view_format_count; /* VkImageFormatListCreateInfo, up to 8 */
    uint32_t view_formats[8];
    uint64_t row_pitch;    /* LINEAR: the exporter's row pitch; 0 for OPTIMAL */
    uint64_t image_bytes;  /* the exporter's vkGetImageMemoryRequirements size */
} vkb_dmabuf_ahb_info;

typedef int (*PFN_vkb_dmabuf_ahb_lookup)(int fd, vkb_dmabuf_ahb_info *out);

#endif
