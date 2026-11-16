/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim
 *
 * pv_suspend (PowerVR, driverID IMAGINATION_PROPRIETARY): dynamic renderings that suspend and resume reach the driver as
 * whole render pass instances, because Imagination's driver faults on the chains.
 *
 * What was shown (Pixel 10 Pro XL, DXT-48-1536, driver 25.1; RE3 952060 on DX12, 11-13 runs):
 * vkd3d-proton 3.1 turns its "tiler_suspend_resume" on for this driver (device_workarounds.c, with Arm, Qualcomm and the
 * Mesa tilers): every rendering is begun SUSPENDING when a command list may end inside it, and finished by RESUMING
 * parts, often empty and in fixup command buffers of their own (BR(S) Draw ER, BR(R) ER; a command buffer that starts
 * with BR(R) ER). At vkQueueSubmit2 the driver follows such a chain into the next sub-command of the submit
 * (IterateDynamicRenderSegments, across command buffers) and reads that one's scissor/depth-bias state, which it does not
 * have: SIGSEGV at vulkan.powervr.so+0x1887fc (GrabScissorDepthBiasBuffers+0x4c, address 0x48), the device lost, RE3
 * back on DX11 and black (a player log, and every DX12 start on the Pixel). Arm's driver gets the same pattern and works.
 *
 * The fix, at the driver's boundary (beneath every layer, which still see the app's flags): a part that SUSPENDS ends as
 * a whole pass, every attachment stored and nothing resolved yet (resolves belong to the last part); a part that RESUMES
 * begins as a whole pass, every attachment loaded. Every part is sent, empty ones too: leaving the empty resumed parts out
 * (an earlier form, 11-13 run4) showed stale and black frames on the Pixel and a slower menu (19-20 fps against 36 with
 * every part sent, run5), so that shortcut is gone. Other drivers: nothing installed.
 * VKBRIDGE_PV_SUSPEND=0: off; =1: on whatever the driver (host tests). Our own code from the Vulkan specification.
 */
#define _GNU_SOURCE
#include "vkb_server.h"

#include <stdlib.h>
#include <string.h>

#define MAXC 16

static PFN_vkCmdBeginRendering next_begin[VKB_MAX_TABLES];
static uint64_t n_whole;

static void count(void)
{
    uint64_t w = __atomic_add_fetch(&n_whole, 1, __ATOMIC_RELAXED);
    if (w == 1)
        VKB_INFO("errata: pv_suspend: a suspended or resumed dynamic rendering reached PowerVR's driver as whole passes (it faults on "
                 "the chains: vulkan.powervr.so GrabScissorDepthBiasBuffers at submit)");
}

static VKAPI_ATTR void VKAPI_CALL pvs_CmdBeginRendering(VkCommandBuffer cb, const VkRenderingInfo *ri)
{
    vkb_srv_table *t = vkb_srv_current_table();
    PFN_vkCmdBeginRendering next = t && t->id < VKB_MAX_TABLES ? next_begin[t->id] : NULL;
    if (!next) return;
    const VkRenderingFlags sr = VK_RENDERING_SUSPENDING_BIT | VK_RENDERING_RESUMING_BIT;
    if (!ri || !(ri->flags & sr) || ri->colorAttachmentCount > MAXC) {
        if (ri && (ri->flags & sr))
            VKB_ONCE(VKB_LOG_WARN, "errata: pv_suspend: a rendering with more than %d colour attachments suspends or resumes; sent as it is "
                                   "(logged once)", MAXC);
        next(cb, ri);
        return;
    }
    int resuming = (ri->flags & VK_RENDERING_RESUMING_BIT) != 0, suspending = (ri->flags & VK_RENDERING_SUSPENDING_BIT) != 0;
    VkRenderingInfo info = *ri;
    VkRenderingAttachmentInfo col[MAXC], dep, sten;
    info.flags &= ~sr;
    for (uint32_t i = 0; i < ri->colorAttachmentCount; i++) {
        col[i] = ri->pColorAttachments[i];
        if (resuming) col[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        if (suspending) {
            col[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            col[i].resolveMode = VK_RESOLVE_MODE_NONE;
            col[i].resolveImageView = VK_NULL_HANDLE;
        }
    }
    info.pColorAttachments = ri->colorAttachmentCount ? col : NULL;
    if (ri->pDepthAttachment) {
        dep = *ri->pDepthAttachment;
        if (resuming) dep.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        if (suspending) dep.storeOp = VK_ATTACHMENT_STORE_OP_STORE, dep.resolveMode = VK_RESOLVE_MODE_NONE, dep.resolveImageView = VK_NULL_HANDLE;
        info.pDepthAttachment = &dep;
    }
    if (ri->pStencilAttachment) {
        sten = *ri->pStencilAttachment;
        if (resuming) sten.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        if (suspending) sten.storeOp = VK_ATTACHMENT_STORE_OP_STORE, sten.resolveMode = VK_RESOLVE_MODE_NONE, sten.resolveImageView = VK_NULL_HANDLE;
        info.pStencilAttachment = &sten;
    }
    count();
    next(cb, &info);
}

void vkb_pvsusp_device_init(vkb_srv_table *t)
{
    if (!t || t->id >= VKB_MAX_TABLES || !t->dt.vkCmdBeginRendering) return;
    const char *v = vkb_srv_env("VKBRIDGE_PV_SUSPEND");
    int on = v && *v ? strcmp(v, "0") != 0
                     : vkb_driver_id(&t->real, t->physical, t->api_version) == VK_DRIVER_ID_IMAGINATION_PROPRIETARY;
    if (!on) return;
    next_begin[t->id] = t->dt.vkCmdBeginRendering;
    t->dt.vkCmdBeginRendering = t->real.vkCmdBeginRendering = pvs_CmdBeginRendering;
}

/* when an installed device goes: the process's totals so far (a line only where it was installed) */
void vkb_pvsusp_device_destroy(vkb_srv_table *t)
{
    if (!t || t->id >= VKB_MAX_TABLES || !next_begin[t->id]) return;
    next_begin[t->id] = NULL;
    VKB_INFO("errata: pv_suspend: device %u gone; %llu rendering part(s) begun as whole passes so far", t->id,
             (unsigned long long)__atomic_load_n(&n_whole, __ATOMIC_RELAXED));
}
