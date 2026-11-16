/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * MaliBridge - server side (bionic on the device, glibc for host tests).
 *
 * The dma-buf exports this server made, as far as an importer can name them: a small register of
 * shape (format, extent, mips, layers), the modifier the driver chose, and the one plane layout
 * vkGetImageSubresourceLayout gave for it. A dma-buf import that passes such a layout back is that
 * export's consumer, whatever modifier it names (server_emu_layer.c).
 */
#ifndef VKB_SERVER_DMABUF_META_H
#define VKB_SERVER_DMABUF_META_H

#include <vulkan/vulkan.h>

typedef struct {
    VkFormat format;
    uint32_t width, height, mips, layers;
    uint64_t modifier;           /* the driver's DRM format modifier (0: DRM_FORMAT_MOD_LINEAR) */
    VkSubresourceLayout layout;  /* the exporter's vkGetImageSubresourceLayout answer */
} vkb_meta_shape;

/* Remembers an export's shape and layout (the register keeps the newest few dozen). */
void vkb_meta_note(const vkb_meta_shape *s);

/* The modifier of an export whose shape and plane layout `s` carries, 0 where there is none.
 * `s->modifier` is ignored: an importer that names the LINEAR modifier while passing a compressed
 * export's layout is the case this answers. A zero size in `s->layout` matches any size. */
uint64_t vkb_meta_match(const vkb_meta_shape *s);

/* Forgets everything (the unit test). */
void vkb_meta_reset(void);

#endif
