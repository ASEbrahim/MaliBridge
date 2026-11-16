/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * pv_view3d_base's copies (pv3d_copy.h): only commands on the caller's command buffer, through its functions.
 */
#include "pv3d_copy.h"

static VkImageMemoryBarrier ibar_l(VkImage i, VkImageLayout from, VkImageLayout to, uint32_t mip, uint32_t base, uint32_t layers)
{
    return (VkImageMemoryBarrier){VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, VK_ACCESS_MEMORY_WRITE_BIT,
                                  VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, from, to, VK_QUEUE_FAMILY_IGNORED,
                                  VK_QUEUE_FAMILY_IGNORED, i, {VK_IMAGE_ASPECT_COLOR_BIT, mip, 1, base, layers}};
}

static VkImageMemoryBarrier ibar(VkImage i, VkImageLayout from, VkImageLayout to, uint32_t mip, uint32_t layers)
{
    return ibar_l(i, from, to, mip, 0, layers);
}

/* the barrier of the first image (the 3D image; with p->arr a 2D-array image: the view's layers) */
static VkImageMemoryBarrier ibar1(const vkb_pv3d_pair *p, VkImageLayout from, VkImageLayout to)
{
    return p->arr ? ibar_l(p->img3d, from, to, p->mip, p->base, p->count) : ibar(p->img3d, from, to, p->mip, 1);
}

/* Everything before is done and visible, everything after waits: these run only for the views that need them,
 * and a barrier of the whole queue is what keeps them exact whatever the app synchronised against */
static void bar(const vkb_pv3d_fns *f, VkCommandBuffer cb, const VkImageMemoryBarrier *b, uint32_t n)
{
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_MEMORY_WRITE_BIT,
                          VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
    f->barrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, n, b);
}

static uint32_t copied(const vkb_pv3d_pair *p)
{
    return p->layers && p->layers < p->count ? p->layers : p->count;
}

static VkImageCopy region(const vkb_pv3d_pair *p, int out)
{
    VkImageSubresourceLayers s3 = {VK_IMAGE_ASPECT_COLOR_BIT, p->mip, p->arr ? p->base : 0, p->arr ? copied(p) : 1},
                             ss = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, copied(p)};
    VkOffset3D o3 = {p->area.offset.x, p->area.offset.y, p->arr ? 0 : (int32_t)p->base}, os = {p->area.offset.x, p->area.offset.y, 0};
    VkExtent3D ex = {p->area.extent.width, p->area.extent.height, p->arr ? 1 : copied(p)};
    return out ? (VkImageCopy){ss, os, s3, o3, ex} : (VkImageCopy){s3, o3, ss, os, ex};
}

static int empty(const vkb_pv3d_pair *p)
{
    return !p->area.extent.width || !p->area.extent.height;
}

void vkb_pv3d_copy_begin(const vkb_pv3d_fns *f, VkCommandBuffer cb, const vkb_pv3d_pair *p, int copy_in)
{
    VkImageMemoryBarrier b[2];
    if (copy_in && !empty(p) && p->l3_start != VK_IMAGE_LAYOUT_UNDEFINED) {
        b[0] = ibar1(p, p->l3_start, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        b[1] = ibar(p->shadow, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, p->count);
        bar(f, cb, b, 2);
        VkImageCopy rg = region(p, 0);
        f->copy(cb, p->img3d, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, p->shadow, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rg);
        uint32_t n = 0;
        b[n++] = ibar1(p, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, p->l3_start);
        if (p->ls_start != VK_IMAGE_LAYOUT_UNDEFINED) b[n++] = ibar(p->shadow, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, p->ls_start, 0, p->count);
        bar(f, cb, b, n);
    } else if (p->ls_start != VK_IMAGE_LAYOUT_UNDEFINED) {
        b[0] = ibar(p->shadow, VK_IMAGE_LAYOUT_UNDEFINED, p->ls_start, 0, p->count);
        bar(f, cb, b, 1);
    }
}

void vkb_pv3d_copy_end(const vkb_pv3d_fns *f, VkCommandBuffer cb, const vkb_pv3d_pair *p, int copy_out)
{
    VkImageMemoryBarrier b[2];
    if (copy_out && !empty(p)) {
        b[0] = ibar(p->shadow, p->ls_end, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, p->count);
        b[1] = ibar1(p, p->l3_start, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        bar(f, cb, b, 2);
        VkImageCopy rg = region(p, 1);
        f->copy(cb, p->shadow, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, p->img3d, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rg);
        b[0] = ibar1(p, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, p->l3_end);
        bar(f, cb, b, 1);
    } else if (p->l3_start != p->l3_end) {
        b[0] = ibar1(p, p->l3_start, p->l3_end);
        bar(f, cb, b, 1);
    }
}

void vkb_pv3d_copy_sync(const vkb_pv3d_fns *f, VkCommandBuffer cb, const vkb_pv3d_pair *p, VkImageLayout ls, VkImageLayout l3_from,
                        VkImageLayout l3_to)
{
    if (empty(p)) {
        vkb_pv3d_copy_layout_p(f, cb, p, l3_from, l3_to);
        return;
    }
    VkImageMemoryBarrier b[2] = {ibar(p->shadow, ls, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, p->count),
                                 ibar1(p, l3_from, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)};
    bar(f, cb, b, 2);
    VkImageCopy rg = region(p, 1);
    f->copy(cb, p->shadow, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, p->img3d, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rg);
    b[0] = ibar(p->shadow, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, ls, 0, p->count);
    b[1] = ibar1(p, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, l3_to);
    bar(f, cb, b, 2);
}

void vkb_pv3d_copy_layout(const vkb_pv3d_fns *f, VkCommandBuffer cb, VkImage img3d, uint32_t mip, VkImageLayout from, VkImageLayout to)
{
    if (from == to || to == VK_IMAGE_LAYOUT_UNDEFINED) return;
    VkImageMemoryBarrier b = ibar(img3d, from, to, mip, 1);
    bar(f, cb, &b, 1);
}

void vkb_pv3d_copy_layout_p(const vkb_pv3d_fns *f, VkCommandBuffer cb, const vkb_pv3d_pair *p, VkImageLayout from, VkImageLayout to)
{
    if (from == to || to == VK_IMAGE_LAYOUT_UNDEFINED) return;
    VkImageMemoryBarrier b = ibar1(p, from, to);
    bar(f, cb, &b, 1);
}
