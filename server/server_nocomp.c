/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Experiment: images made without the driver's framebuffer compression (AFBC on Mali), through
 * VK_EXT_image_compression_control. VKBRIDGE_NO_COMPRESSION=all: every image; =alias: images made
 * with VK_IMAGE_CREATE_ALIAS_BIT (D3D12 placed resources sharing a heap). Off unless asked for.
 */
#define _GNU_SOURCE
#include "vkb_server.h"

#include <string.h>

static PFN_vkCreateImage next_create; /* the driver's, the same for every device */
static int mode;                      /* 1 all, 2 alias only */

static VKAPI_ATTR VkResult VKAPI_CALL nc_CreateImage(VkDevice d, const VkImageCreateInfo *ci, const VkAllocationCallbacks *a, VkImage *out)
{
    if (mode == 1 || (mode == 2 && (ci->flags & VK_IMAGE_CREATE_ALIAS_BIT))) {
        int has = 0;
        for (const VkBaseInStructure *p = ci->pNext; p; p = p->pNext)
            has |= p->sType == VK_STRUCTURE_TYPE_IMAGE_COMPRESSION_CONTROL_EXT;
        if (!has && ci->tiling != VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT) {
            VkImageCompressionControlEXT cc = {VK_STRUCTURE_TYPE_IMAGE_COMPRESSION_CONTROL_EXT, ci->pNext, VK_IMAGE_COMPRESSION_DISABLED_EXT, 0, NULL};
            VkImageCreateInfo c2 = *ci;
            c2.pNext = &cc;
            return next_create(d, &c2, a, out);
        }
    }
    return next_create(d, ci, a, out);
}

int vkb_nocomp_wanted(void)
{
    const char *v = vkb_srv_env("VKBRIDGE_NO_COMPRESSION");
    if (!v || !*v || !strcmp(v, "0")) return 0;
    return !strcmp(v, "alias") ? 2 : 1;
}

void vkb_nocomp_device_init(vkb_srv_table *t, int enabled)
{
    if (!enabled || !t->real.vkCreateImage) return;
    mode = vkb_nocomp_wanted();
    if (!mode) return;
    if (t->real.vkCreateImage != nc_CreateImage) next_create = t->real.vkCreateImage;
    if (t->dt.vkCreateImage == next_create) t->dt.vkCreateImage = nc_CreateImage;
    t->real.vkCreateImage = nc_CreateImage;
    VKB_INFO("device %u: images made without compression (%s)", t->id, mode == 1 ? "all" : "aliased only");
}
