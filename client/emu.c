/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * Client side of feature emulation: the app sees a feature when the GPU has it or the server
 * emulates it (vkb_server_pd_info.emu), and does not see one the GPU lacks (.missing).
 *
 * Note: see common/vk_caps.c for actual implementation
 */
#define _GNU_SOURCE
#include "emu.h"
#include "gen/vkb_client_gen.h"
#include "vkb_caps.h"
#include "vkb_emu_formats.h"

#include <unistd.h>

const VkExtensionProperties *vkb_emu_extensions(vkb_physdev *pd, uint32_t *n)
{
    return vkb_caps_emu_extensions(&pd->caps, n);
}

void vkb_emu_patch_limits(vkb_physdev *pd, VkPhysicalDeviceProperties *p)
{
    vkb_caps_patch_properties(&pd->caps, p, 0);
}

static void client_get_props2_cb(VkPhysicalDevice p, VkPhysicalDeviceProperties2 *out, void *user)
{
    vkb_wire_vkGetPhysicalDeviceProperties2(p, out);
}

void vkb_emu_patch_properties_chain(vkb_physdev *pd, VkBaseOutStructure *b)
{
    vkb_caps_patch_properties_chain(&pd->caps, (VkPhysicalDevice)pd, b, client_get_props2_cb, NULL);
}

void vkb_emu_patch_features(vkb_physdev *pd, VkPhysicalDeviceFeatures *f)
{
    vkb_caps_patch_features(&pd->caps, f);
}

void vkb_emu_patch_features_chain(vkb_physdev *pd, VkBaseOutStructure *b)
{
    if (b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_IMAGE_COPY_FEATURES) {
        /* Host image copy hands the driver host pointers: never offered across the bridge. */
        ((VkPhysicalDeviceHostImageCopyFeatures *)b)->hostImageCopy = VK_FALSE;
        return;
    }

    vkb_caps_patch_features_chain(&pd->caps, b, /*is_remote_bridge=*/ 1, pd->info.strategy != VKB_MEM_NONE);
}

static void get_pd_format_properties(VkPhysicalDevice p, VkFormat format, VkFormatProperties *out, void *user)
{
    vkb_wire_vkGetPhysicalDeviceFormatProperties(p, format, out);
}

static void get_pd_format_properties2(VkPhysicalDevice p, VkFormat format, VkFormatProperties2 *out, void *user)
{
    vkb_wire_vkGetPhysicalDeviceFormatProperties2(p, format, out);
}

int vkb_emu_format_properties(vkb_physdev *pd, VkFormat format, VkFormatProperties *fp, VkFormatProperties2 *fp2)
{
    return vkb_caps_format_properties2(&pd->caps, (VkPhysicalDevice)pd, format, fp, fp2,
                                        get_pd_format_properties, get_pd_format_properties2, NULL);
}

int vkb_emu_is_emulated_format(vkb_physdev *pd, VkFormat format)
{
    return vkb_is_bc(format) && (pd->caps.emu & VKB_EMU_BCN);
}

VkFormat vkb_emu_real_format(vkb_physdev *pd, VkFormat format, VkImageUsageFlags *usage, VkImageCreateFlags *flags)
{
    if (!vkb_emu_is_emulated_format(pd, format)) return format;
    /* As the server creates it (server_emu_bcn.c): decoded format, mutable, extended usage. */
    *flags &= ~(VkImageCreateFlags)VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT;
    *flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
    *usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    return vkb_bc_view_format(format);
}

int vkb_emu_planar_ext_usage(vkb_physdev *pd, VkFormat format, VkImageTiling tiling,
                             VkImageUsageFlags usage, VkImageCreateFlags *flags)
{
    return vkb_caps_planar_ext_usage(&pd->caps, (VkPhysicalDevice)pd, format, tiling, usage,
                                     flags, get_pd_format_properties, NULL);
}

/* Image creation is rewritten on the server (it knows the decode); nothing to do here. */
void vkb_emu_image_create_info(vkb_device *dev, VkImageCreateInfo *ci) { (void)dev; (void)ci; }
void vkb_emu_image_created(vkb_device *dev, const VkImageCreateInfo *ci, VkImage image) { (void)dev; (void)ci; (void)image; }
void vkb_emu_image_destroyed(vkb_device *dev, VkImage image) { (void)dev; (void)image; }
