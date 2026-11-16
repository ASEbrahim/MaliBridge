/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * pv_view3d_base's copies (PV-04, server_errata_view3d.c): the slices of a 3D image a pass renders through a 2D-array
 * image of the bridge's own (the "shadow"), copied in before the pass, out after it, and out in the middle of it where
 * the fix splits a pass so reads of the 3D image see what was rendered so far. On its own (only Vulkan's headers):
 * the fix, the startup probe and MaliBridge check's device cases record the same commands.
 */
#ifndef VKB_PV3D_COPY_H
#define VKB_PV3D_COPY_H

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>

typedef struct vkb_pv3d_fns {
    PFN_vkCmdPipelineBarrier barrier;
    PFN_vkCmdCopyImage copy;
} vkb_pv3d_fns;

typedef struct vkb_pv3d_pair {
    VkImage img3d, shadow;          /* the 3D image and its 2D-array image of the bridge's own */
    uint32_t mip, base, count;      /* the 3D image's mip level, its first slice in the view, the slices (= the shadow's layers) */
    VkRect2D area;                  /* the part of each slice the pass covers (inside the mip's size) */
    VkImageLayout l3_start, l3_end; /* the 3D image's layout when the pass begins, after it */
    VkImageLayout ls_start, ls_end; /* the shadow's layout the pass starts in, ends in */
    uint32_t layers;                /* the shadow's layers the pass renders (framebuffer layers, a view mask's highest
                                       view + 1), the ones copied; 0 or more than count: count */
    int arr;                        /* img3d is a plain 2D-array image (its layers base..base+count-1 are the slices), not a 3D image
                                       (pv_fbloop_split's imitation puts the same shadow beneath a plain feedback-loop attachment) */
} vkb_pv3d_pair;

/* Before the pass: the shadow ready in ls_start, with the area of the slices copied into it when copy_in. The 3D
 * image is left in l3_start. */
void vkb_pv3d_copy_begin(const vkb_pv3d_fns *f, VkCommandBuffer cb, const vkb_pv3d_pair *p, int copy_in);
/* After the pass: the area copied back into the slices when copy_out; the 3D image ends in l3_end either way. */
void vkb_pv3d_copy_end(const vkb_pv3d_fns *f, VkCommandBuffer cb, const vkb_pv3d_pair *p, int copy_out);
/* Between two parts of a split pass: the area copied back into the slices; the shadow is in ls before and after
 * (the layout the first part ends it in, the second starts it in), the 3D image goes from l3_from to l3_to. */
void vkb_pv3d_copy_sync(const vkb_pv3d_fns *f, VkCommandBuffer cb, const vkb_pv3d_pair *p, VkImageLayout ls, VkImageLayout l3_from,
                        VkImageLayout l3_to);
/* The 3D image's mip level from one layout to another (nothing when they are the same) */
void vkb_pv3d_copy_layout(const vkb_pv3d_fns *f, VkCommandBuffer cb, VkImage img3d, uint32_t mip, VkImageLayout from, VkImageLayout to);
/* ... the same for the pair's image (its layers too, when p->arr) */
void vkb_pv3d_copy_layout_p(const vkb_pv3d_fns *f, VkCommandBuffer cb, const vkb_pv3d_pair *p, VkImageLayout from, VkImageLayout to);

#endif
