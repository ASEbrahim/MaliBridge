/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * What DXVK 3 requires and Mali-G720 (r44p1) does not have, made up for so that DirectX games
 * start at all (Cuphead, 2026-10-11: "Skipping: Device does not support required feature
 * 'dualSrcBlend'"). Each is reported to the client as supported; this file handles the rare use:
 *
 *  - dualSrcBlend (VKB_EMU_DUAL_SRC): blend factors on the second source become the first's, and
 *    the fragment shader's second output (Index 1) goes to an unused location. Approximate - a
 *    game blending with two outputs looks different - and logged once when it happens.
 *  - fillModeNonSolid (VKB_EMU_FILL_MODE): the pipeline itself is made solid; server_emu_polygon.c
 *    gives it a line-list or point-list variant and draws through that (VKBRIDGE_POLYGON_EMU=0:
 *    solid, as it was).
 *  - multiViewport (VKB_EMU_MULTI_VIEWPORT): only the first viewport and scissor are used. D3D
 *    games set one almost always; logged once when more arrive.
 *  - VK_KHR_maintenance6 (VKB_EMU_MAINT6): vkCmdBindDescriptorSets2 / vkCmdPushConstants2 on their
 *    1.0 forms (per bind point named by the stage flags). Exact.
 *  - VK_KHR_load_store_op_none (VKB_EMU_LOADSTORE_NONE): the driver's VK_EXT_load_store_op_none,
 *    same enumerants (server_instance.c enables it). Exact.
 *  - depthClamp (VKB_EMU_DEPTH_CLAMP): a pipeline that clamps depth clips it instead. DXVK 1.10 asks
 *    for the feature at every D3D11 feature level and turns clamping on only where a rasterizer
 *    state disables depth clipping; geometry past the near or far plane is then cut rather than
 *    flattened onto it. Approximate, logged once. Mali-G76 r26p0 lacks the feature (Abathor:
 *    "Requested feature level not supported" down to 9_1).
 *  - multiDrawIndirect (VKB_EMU_MULTI_DRAW): vkCmdDraw(Indexed)Indirect with drawCount > 1 is
 *    recorded as drawCount single indirect draws at offset + i * stride. Exact, a little more CPU
 *    work per draw. Mali-G57 (r32p1, r38p1) lacks the feature and DXVK needs it. The DroidDeck Mali
 *    compatibility layer (The412Banner, GPL-3.0) emulates it the same way.
 */
#define _GNU_SOURCE
#include "vkb_server.h"
#include "server_emu.h"

#include <stdlib.h>
#include <string.h>

typedef struct compat_state {
    PFN_vkCmdBindDescriptorSets bind;
    PFN_vkCmdPushConstants push;
    PFN_vkCmdSetViewport set_viewport;
    PFN_vkCmdSetScissor set_scissor;
    PFN_vkCmdSetViewportWithCount set_viewport_count;
    PFN_vkCmdSetScissorWithCount set_scissor_count;
    PFN_vkCmdDrawIndirect draw_indirect;
    PFN_vkCmdDrawIndexedIndirect draw_indexed_indirect;
} compat_state;

static compat_state *states[4096];

static compat_state *st(void)
{
    vkb_srv_table *t = vkb_srv_current_table();
    return t && t->id < 4096 ? states[t->id] : NULL;
}

static void once(int *flag, const char *what)
{
    if (!__atomic_fetch_add(flag, 1, __ATOMIC_RELAXED)) VKB_WARN("emulation: %s (logged once)", what);
}

static VkBlendFactor first_source(VkBlendFactor f, int *changed)
{
    switch (f) {
    case VK_BLEND_FACTOR_SRC1_COLOR: *changed = 1; return VK_BLEND_FACTOR_SRC_COLOR;
    case VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR: *changed = 1; return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case VK_BLEND_FACTOR_SRC1_ALPHA: *changed = 1; return VK_BLEND_FACTOR_SRC_ALPHA;
    case VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA: *changed = 1; return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    default: return f;
    }
}

/* Pipeline state the driver cannot take; the create info is the server's own copy. */
void vkb_emu_compat_pipeline(vkb_emu_device *e, VkDevice device, VkGraphicsPipelineCreateInfo *ci, VkShaderModule *mods, uint32_t *nmods,
                             uint32_t cap)
{
    static int told_dual, told_fill, told_vp, told_clamp;
    if ((e->flags & VKB_EMU_DEPTH_CLAMP) && ci->pRasterizationState && ci->pRasterizationState->depthClampEnable) {
        ((VkPipelineRasterizationStateCreateInfo *)ci->pRasterizationState)->depthClampEnable = VK_FALSE;
        once(&told_clamp, "depth clamping drawn with depth clipping instead (no depthClamp)");
    }
    if ((e->flags & VKB_EMU_DUAL_SRC) && ci->pColorBlendState) {
        int changed = 0;
        const VkPipelineColorBlendStateCreateInfo *cb = ci->pColorBlendState;
        for (uint32_t i = 0; i < cb->attachmentCount && cb->pAttachments; i++) {
            VkPipelineColorBlendAttachmentState *a = (VkPipelineColorBlendAttachmentState *)&cb->pAttachments[i];
            a->srcColorBlendFactor = first_source(a->srcColorBlendFactor, &changed);
            a->dstColorBlendFactor = first_source(a->dstColorBlendFactor, &changed);
            a->srcAlphaBlendFactor = first_source(a->srcAlphaBlendFactor, &changed);
            a->dstAlphaBlendFactor = first_source(a->dstAlphaBlendFactor, &changed);
        }
        if (changed) once(&told_dual, "dual-source blending approximated with the first source (blending may look off)");
        /* The second output goes even when the blend state did not name it (a shader may declare it). */
        vkb_emu_shader_drop_second_output(e, device, ci, mods, nmods, cap);
    }
    if ((e->flags & VKB_EMU_FILL_MODE) && ci->pRasterizationState && ci->pRasterizationState->polygonMode != VK_POLYGON_MODE_FILL) {
        ((VkPipelineRasterizationStateCreateInfo *)ci->pRasterizationState)->polygonMode = VK_POLYGON_MODE_FILL;
        /* server_emu_polygon.c draws them as lines or points through a variant (and logs that). */
        if (!e->polygon) once(&told_fill, "wireframe/point fill drawn solid (no fillModeNonSolid)");
    }
    if ((e->flags & VKB_EMU_MULTI_VIEWPORT) && ci->pViewportState) {
        VkPipelineViewportStateCreateInfo *vp = (VkPipelineViewportStateCreateInfo *)ci->pViewportState;
        if (vp->viewportCount > 1 || vp->scissorCount > 1) {
            if (vp->viewportCount > 1) vp->viewportCount = 1;
            if (vp->scissorCount > 1) vp->scissorCount = 1;
            once(&told_vp, "multiple viewports: only the first is used (no multiViewport)");
        }
    }
}

static void VKAPI_CALL c_CmdSetViewport(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkViewport *v)
{
    compat_state *s = st();
    if (first > 0) return;
    s->set_viewport(cb, 0, n ? 1 : 0, v);
}

static void VKAPI_CALL c_CmdSetScissor(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkRect2D *r)
{
    compat_state *s = st();
    if (first > 0) return;
    s->set_scissor(cb, 0, n ? 1 : 0, r);
}

static void VKAPI_CALL c_CmdSetViewportWithCount(VkCommandBuffer cb, uint32_t n, const VkViewport *v)
{
    st()->set_viewport_count(cb, n > 1 ? 1 : n, v);
}

static void VKAPI_CALL c_CmdSetScissorWithCount(VkCommandBuffer cb, uint32_t n, const VkRect2D *r)
{
    st()->set_scissor_count(cb, n > 1 ? 1 : n, r);
}

static void VKAPI_CALL c_CmdBindDescriptorSets2(VkCommandBuffer cb, const VkBindDescriptorSetsInfo *i)
{
    compat_state *s = st();
    if (i->stageFlags & VK_SHADER_STAGE_ALL_GRAPHICS)
        s->bind(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, i->layout, i->firstSet, i->descriptorSetCount, i->pDescriptorSets,
                i->dynamicOffsetCount, i->pDynamicOffsets);
    if (i->stageFlags & VK_SHADER_STAGE_COMPUTE_BIT)
        s->bind(cb, VK_PIPELINE_BIND_POINT_COMPUTE, i->layout, i->firstSet, i->descriptorSetCount, i->pDescriptorSets,
                i->dynamicOffsetCount, i->pDynamicOffsets);
}

static void VKAPI_CALL c_CmdPushConstants2(VkCommandBuffer cb, const VkPushConstantsInfo *i)
{
    st()->push(cb, i->layout, i->stageFlags, i->offset, i->size, i->pValues);
}

static int told_multidraw;
#define TELL_MULTIDRAW() \
    do { if (!told_multidraw) once(&told_multidraw, "indirect draws split into single draws (no multiDrawIndirect)"); } while (0)

static void VKAPI_CALL c_CmdDrawIndirect(VkCommandBuffer cb, VkBuffer buf, VkDeviceSize off, uint32_t n, uint32_t stride)
{
    compat_state *s = st();
    if (n <= 1) {
        s->draw_indirect(cb, buf, off, n, stride);
        return;
    }
    TELL_MULTIDRAW();
    for (uint32_t i = 0; i < n; i++) s->draw_indirect(cb, buf, off + (VkDeviceSize)i * stride, 1, stride);
}

static void VKAPI_CALL c_CmdDrawIndexedIndirect(VkCommandBuffer cb, VkBuffer buf, VkDeviceSize off, uint32_t n, uint32_t stride)
{
    compat_state *s = st();
    if (n <= 1) {
        s->draw_indexed_indirect(cb, buf, off, n, stride);
        return;
    }
    TELL_MULTIDRAW();
    for (uint32_t i = 0; i < n; i++) s->draw_indexed_indirect(cb, buf, off + (VkDeviceSize)i * stride, 1, stride);
}

void vkb_emu_compat_install(vkb_srv_table *dev)
{
    vkb_emu_device *e = dev->emu;
    if (!(e->flags & (VKB_EMU_MULTI_VIEWPORT | VKB_EMU_MAINT6 | VKB_EMU_MULTI_DRAW)) || dev->id >= 4096) return;
    compat_state *s = calloc(1, sizeof(*s));
    if (!s) return;
    vkb_dispatch *dt = &dev->dt;
    /* What is installed now (the command-state tracker's wrappers): the 2-forms go through them. */
    s->bind = dt->vkCmdBindDescriptorSets;
    s->push = dt->vkCmdPushConstants;
    s->set_viewport = dt->vkCmdSetViewport;
    s->set_scissor = dt->vkCmdSetScissor;
    s->set_viewport_count = dt->vkCmdSetViewportWithCount;
    s->set_scissor_count = dt->vkCmdSetScissorWithCount;
    s->draw_indirect = dt->vkCmdDrawIndirect;
    s->draw_indexed_indirect = dt->vkCmdDrawIndexedIndirect;
    if (e->flags & VKB_EMU_MULTI_DRAW) {
        if (s->draw_indirect) dt->vkCmdDrawIndirect = c_CmdDrawIndirect;
        if (s->draw_indexed_indirect) dt->vkCmdDrawIndexedIndirect = c_CmdDrawIndexedIndirect;
    }
    if (e->flags & VKB_EMU_MULTI_VIEWPORT) {
        if (s->set_viewport) dt->vkCmdSetViewport = c_CmdSetViewport;
        if (s->set_scissor) dt->vkCmdSetScissor = c_CmdSetScissor;
        if (s->set_viewport_count) dt->vkCmdSetViewportWithCount = c_CmdSetViewportWithCount;
        if (s->set_scissor_count) dt->vkCmdSetScissorWithCount = c_CmdSetScissorWithCount;
    }
    if (e->flags & VKB_EMU_MAINT6) {
        if (!dev->real.vkCmdBindDescriptorSets2) dt->vkCmdBindDescriptorSets2 = c_CmdBindDescriptorSets2;
        if (!dev->real.vkCmdPushConstants2) dt->vkCmdPushConstants2 = c_CmdPushConstants2;
    }
    states[dev->id] = s;
}

void vkb_emu_compat_uninstall(vkb_srv_table *dev)
{
    if (dev->id >= 4096 || !states[dev->id]) return;
    compat_state *s = states[dev->id];
    vkb_dispatch *dt = &dev->dt;
    if (dt->vkCmdSetViewport == c_CmdSetViewport) dt->vkCmdSetViewport = s->set_viewport;
    if (dt->vkCmdSetScissor == c_CmdSetScissor) dt->vkCmdSetScissor = s->set_scissor;
    if (dt->vkCmdSetViewportWithCount == c_CmdSetViewportWithCount) dt->vkCmdSetViewportWithCount = s->set_viewport_count;
    if (dt->vkCmdSetScissorWithCount == c_CmdSetScissorWithCount) dt->vkCmdSetScissorWithCount = s->set_scissor_count;
    if (dt->vkCmdDrawIndirect == c_CmdDrawIndirect) dt->vkCmdDrawIndirect = s->draw_indirect;
    if (dt->vkCmdDrawIndexedIndirect == c_CmdDrawIndexedIndirect) dt->vkCmdDrawIndexedIndirect = s->draw_indexed_indirect;
    if (dt->vkCmdBindDescriptorSets2 == c_CmdBindDescriptorSets2) dt->vkCmdBindDescriptorSets2 = NULL;
    if (dt->vkCmdPushConstants2 == c_CmdPushConstants2) dt->vkCmdPushConstants2 = NULL;
    states[dev->id] = NULL;
    free(s);
}
