/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * pv_mutable_linear (PV-02, PowerVR): the image-format query for a LINEAR, dma-buf shareable,
 * mutable-format RGBA8 / BGRA8 image (gamescope's flippable images: a UNORM / sRGB view list) is
 * answered VK_ERROR_FORMAT_NOT_SUPPORTED, yet the driver exports, imports and binds exactly that
 * image. gamescope believes the query and goes on without the mutable flag, and PowerVR then
 * refuses the tiling that follows. The fix lives in the "driver" gamescope sees: where the query is
 * refused for that pattern and the erratum is on (the startup probe pv_mutable_linear saw the
 * refusal beside a working export / import), it is answered with the result for the same image
 * without the mutable flag, the base format's (the driver's own answer for what it does make).
 * Every other query goes through untouched.
 *
 * The pattern: a 2D RGBA8 or BGRA8 image (UNORM or sRGB), MUTABLE_FORMAT, tiling LINEAR or a DRM
 * format modifier query for the LINEAR modifier, with DMA_BUF in the external memory info.
 *
 * Imitation (host): the driver's answer for exactly that pattern turned into NOT_SUPPORTED beneath
 * the fix (the startup probe sees it too: vkb_errata_pvmutable_probe_refuses), while the images
 * themselves are made as before; VKBRIDGE_FAKE_MISSING=pvmutable (server_emu.c) does it at the driver
 * boundary as the Pixel 10 Pro XL's DXT 25.1@6794074 answered (a player log), LINEAR and any modifier alike.
 * Seen there: gamescope's query (B8G8R8A8_UNORM, usage 0x6, flags 0x8, 2 view formats) refused for the
 * LINEAR modifier and for 0x00ffffffffffffff, gamescope then aborted on "modifiers.size() > 0".
 */
#define _GNU_SOURCE
#include "server_errata.h"
#include "server_emu.h"

#include <string.h>

#define DMABUF VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT

int vkb_errata_pvmutable_pattern(const VkPhysicalDeviceImageFormatInfo2 *info)
{
    if (!info || info->type != VK_IMAGE_TYPE_2D || !(info->flags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT)) return 0;
    switch (info->format) {
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB:
        break;
    default:
        return 0;
    }
    int dmabuf = 0, linear = info->tiling == VK_IMAGE_TILING_LINEAR;
    for (const VkBaseInStructure *b = info->pNext; b; b = b->pNext) {
        if (b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO)
            dmabuf |= ((const VkPhysicalDeviceExternalImageFormatInfo *)b)->handleType == DMABUF;
        else if (b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT)
            linear |= info->tiling == VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT &&
                      ((const VkPhysicalDeviceImageDrmFormatModifierInfoEXT *)b)->drmFormatModifier == 0 /* DRM_FORMAT_MOD_LINEAR */;
    }
    return dmabuf && linear;
}

/* The startup probe's own query, with the imitation beneath it when VKBRIDGE_ERRATA_IMITATE names the erratum */
int vkb_errata_pvmutable_probe_refuses(const VkPhysicalDeviceImageFormatInfo2 *info)
{
    if (vkb_emu_pvmutable_refused(info)) return 1; /* VKBRIDGE_FAKE_MISSING=pvmutable: PowerVR's refusal at the driver (the probe asks the driver itself) */
    vkb_errata_dev *e = vkb_errata_cur();
    return e && e->probe && (e->imitated >> VKB_ERRATUM_PV_MUTABLE_LINEAR & 1) && vkb_errata_pvmutable_pattern(info);
}

static const vkb_srv_table *query_inst(void)
{
    const vkb_srv_table *t = vkb_srv_current_table();
    return t && t->is_device ? t->parent : t;
}

static VKAPI_ATTR VkResult VKAPI_CALL pvm_GetPhysicalDeviceImageFormatProperties2(VkPhysicalDevice pd, const VkPhysicalDeviceImageFormatInfo2 *info,
                                                                                 VkImageFormatProperties2 *props)
{
    const vkb_srv_table *t = query_inst();
    if (!t || !t->pvm_f2) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = t->pvm_f2(pd, info, props);
    if (!vkb_errata_pvmutable_pattern(info)) return r;
    if (r == VK_SUCCESS && vkb_errata_imitating(VKB_ERRATUM_PV_MUTABLE_LINEAR)) {
        /* the bug beneath the fix */
        memset(&props->imageFormatProperties, 0, sizeof(props->imageFormatProperties));
        r = VK_ERROR_FORMAT_NOT_SUPPORTED;
        VKB_ONCE(VKB_LOG_WARN, "errata: pv_mutable_linear bug imitated beneath the fix (VKBRIDGE_ERRATA_IMITATE)");
    }
    if (r != VK_ERROR_FORMAT_NOT_SUPPORTED) return r;
    char why[320];
    if (!vkb_errata_pvmutable_driver(t, pd, why, sizeof(why))) return r;
    VkPhysicalDeviceImageFormatInfo2 base = *info;
    base.flags &= ~(VkImageCreateFlags)VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    VkResult r2 = t->pvm_f2(pd, &base, props);
    VKB_ONCE(VKB_LOG_INFO, "errata: pv_mutable_linear: a mutable-format LINEAR dma-buf image query refused by the driver answered with the base format's (%d) (%s) - PV-02",
             r2, why);
    return r2 == VK_SUCCESS ? VK_SUCCESS : r;
}

void vkb_errata_pvmutable_instance_install(vkb_srv_table *t)
{
    if (!t->dt.vkGetPhysicalDeviceImageFormatProperties2) return;
    t->pvm_f2 = t->dt.vkGetPhysicalDeviceImageFormatProperties2;
    t->dt.vkGetPhysicalDeviceImageFormatProperties2 = pvm_GetPhysicalDeviceImageFormatProperties2;
}

/* The fix is in the instance's query (above); a device's install says that it is in force */
void vkb_errata_pvmutable_install(vkb_srv_table *t, vkb_errata_dev *e)
{
    (void)t, (void)e;
    vkb_errata_info_once("errata: pv_mutable_linear: refused mutable-format LINEAR dma-buf image queries answered with the base format's - PV-02");
}

void vkb_errata_pvmutable_imitate(vkb_srv_table *t, vkb_errata_dev *e)
{
    (void)t, (void)e; /* the refusal is in the query (above) and the probe's */
}
