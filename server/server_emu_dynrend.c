/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * VK_KHR_dynamic_rendering on a driver without it (Mali Vulkan 1.1 drivers: G57 / G610 / G710 on
 * r32p1). Mesa's Zink requires it since 25.x ("ZINK: VK_KHR_dynamic_rendering required!", a debug
 * build's only word; a release build falls back to llvmpipe in silence), and Zink draws Steam's UI
 * (CEF -> ANGLE -> Zink) and every OpenGL game, so on such a phone Steam mode showed a black or
 * software-drawn picture.
 *
 * Each rendering becomes a render pass of one subpass:
 *   - vkCmdBeginRendering: a render pass made from the attachments (the views' formats and sample
 *     counts, the info's load/store ops and layouts as initial = final = reference layout, colour
 *     and depth/stencil resolves, the view mask), cached per device; a framebuffer of the views,
 *     kept by the command buffer until it is begun again, reset or freed (server_emu_cmdstate.c);
 *     vkCmdBeginRenderPass with the clear values in attachment order. A suspending rendering
 *     stores and a resuming one loads (the two halves become two passes).
 *   - vkCmdEndRendering: vkCmdEndRenderPass.
 *   - a pipeline made with VkPipelineRenderingCreateInfo (renderPass VK_NULL_HANDLE): a render
 *     pass compatible with it (same formats, the pipeline's sample count, view mask), subpass 0;
 *   - a secondary command buffer begun with VkCommandBufferInheritanceRenderingInfo: the same
 *     compatible pass in its inheritance info.
 * A null colour view becomes VK_ATTACHMENT_UNUSED at its index (vkCmdClearAttachments' indices
 * stay the app's). The aspect of a depth/stencil view the app did not attach is loaded and stored
 * (kept as it was). Not done: fragment shading rate and density map attachments (the drivers this
 * is for have neither), local read (not offered).
 * Needs vkCreateRenderPass2 (VK_KHR_create_renderpass2, which these drivers have); depth/stencil
 * resolves need VK_KHR_depth_stencil_resolve. VKBRIDGE_EMU_DYNREND=0 turns it off.
 *
 * Our own code from the Vulkan specification (no other implementation looked at).
 */
#define _GNU_SOURCE
#include "vkb_server.h"
#include "server_emu.h"

#include <stdlib.h>
#include <string.h>

#define MAXC 8
#define MAP_BUCKETS 512

/* What a render pass is made from (zeroed, then filled: compared and hashed as bytes). */
typedef struct att_key {
    uint32_t format, samples, load, store, layout;
    uint32_t res_format, res_layout; /* res_format 0: no resolve */
} att_key;

typedef struct rp_key {
    uint32_t view_mask, ncolor, has_ds;
    att_key color[MAXC];
    uint32_t ds_format, ds_samples, dload, dstore, sload, sstore, dlayout, slayout;
    uint32_t ds_res_format, ds_res_dlayout, ds_res_slayout, dres_mode, sres_mode;
} rp_key;

typedef struct rp_rec {
    rp_key key;
    VkRenderPass rp;
    struct rp_rec *next;
} rp_rec;

typedef struct view_rec {
    VkImageView view;
    VkFormat format;
    VkSampleCountFlagBits samples;
    struct view_rec *next;
} view_rec;

typedef struct img_rec {
    VkImage image;
    VkSampleCountFlagBits samples;
    struct img_rec *next;
} img_rec;

/* A command buffer's framebuffers (released at its next begin, reset or free). */
typedef struct cb_fbs {
    VkFramebuffer *fb;
    uint32_t n, cap;
} cb_fbs;

typedef struct dr_state {
    pthread_mutex_t lock;
    rp_rec *rps[MAP_BUCKETS];
    view_rec *views[MAP_BUCKETS];
    img_rec *imgs[MAP_BUCKETS];
    uint32_t nrp;
    int ds_resolve; /* VK_KHR_depth_stencil_resolve enabled */
    PFN_vkCreateRenderPass2 create_rp2;
    PFN_vkDestroyRenderPass destroy_rp;
    PFN_vkCreateFramebuffer create_fb;
    PFN_vkDestroyFramebuffer destroy_fb;
    PFN_vkCmdBeginRenderPass begin_rp;
    PFN_vkCmdEndRenderPass end_rp;
    PFN_vkCreateGraphicsPipelines create_gp;
    PFN_vkBeginCommandBuffer begin_cb;
    PFN_vkCreateImage create_image;
    PFN_vkDestroyImage destroy_image;
    PFN_vkCreateImageView create_view;
    PFN_vkDestroyImageView destroy_view;
    PFN_vkCmdBindVertexBuffers bind_vb;
    uint64_t passes, renderings, pipelines, secondaries;
} dr_state;

static dr_state *states[4096];

static dr_state *st(void)
{
    vkb_srv_table *t = vkb_srv_current_table();
    return t && t->id < 4096 ? states[t->id] : NULL;
}

static unsigned hb(uint64_t k)
{
    k ^= k >> 31;
    k *= 0x94d049bb133111ebull;
    return (unsigned)(k >> 55) & (MAP_BUCKETS - 1);
}

static unsigned key_hash(const rp_key *k)
{
    const uint8_t *p = (const uint8_t *)k;
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < sizeof(*k); i++) h = (h ^ p[i]) * 1099511628211ull;
    return hb(h);
}

/* ------------------------------------------------------------------ images and views */

static VKAPI_ATTR VkResult VKAPI_CALL d_CreateImage(VkDevice device, const VkImageCreateInfo *ci, const VkAllocationCallbacks *a, VkImage *out)
{
    dr_state *s = st();
    VkResult r = s->create_image(device, ci, a, out);
    if (r == VK_SUCCESS && ci->samples > VK_SAMPLE_COUNT_1_BIT) {
        img_rec *m = calloc(1, sizeof(*m));
        if (m) {
            m->image = *out;
            m->samples = ci->samples;
            pthread_mutex_lock(&s->lock);
            unsigned b = hb((uint64_t)(uintptr_t)*out);
            m->next = s->imgs[b];
            s->imgs[b] = m;
            pthread_mutex_unlock(&s->lock);
        }
    }
    return r;
}

static VKAPI_ATTR void VKAPI_CALL d_DestroyImage(VkDevice device, VkImage image, const VkAllocationCallbacks *a)
{
    dr_state *s = st();
    if (image) {
        pthread_mutex_lock(&s->lock);
        for (img_rec **pp = &s->imgs[hb((uint64_t)(uintptr_t)image)]; *pp; pp = &(*pp)->next)
            if ((*pp)->image == image) {
                img_rec *m = *pp;
                *pp = m->next;
                free(m);
                break;
            }
        pthread_mutex_unlock(&s->lock);
    }
    s->destroy_image(device, image, a);
}

static VKAPI_ATTR VkResult VKAPI_CALL d_CreateImageView(VkDevice device, const VkImageViewCreateInfo *ci, const VkAllocationCallbacks *a,
                                                        VkImageView *out)
{
    dr_state *s = st();
    VkResult r = s->create_view(device, ci, a, out);
    if (r != VK_SUCCESS) return r;
    view_rec *v = calloc(1, sizeof(*v));
    if (!v) return r;
    v->view = *out;
    v->format = ci->format;
    v->samples = VK_SAMPLE_COUNT_1_BIT;
    pthread_mutex_lock(&s->lock);
    for (img_rec *m = s->imgs[hb((uint64_t)(uintptr_t)ci->image)]; m; m = m->next)
        if (m->image == ci->image) {
            v->samples = m->samples;
            break;
        }
    unsigned b = hb((uint64_t)(uintptr_t)*out);
    v->next = s->views[b];
    s->views[b] = v;
    pthread_mutex_unlock(&s->lock);
    return r;
}

static VKAPI_ATTR void VKAPI_CALL d_DestroyImageView(VkDevice device, VkImageView view, const VkAllocationCallbacks *a)
{
    dr_state *s = st();
    if (view) {
        pthread_mutex_lock(&s->lock);
        for (view_rec **pp = &s->views[hb((uint64_t)(uintptr_t)view)]; *pp; pp = &(*pp)->next)
            if ((*pp)->view == view) {
                view_rec *v = *pp;
                *pp = v->next;
                free(v);
                break;
            }
        pthread_mutex_unlock(&s->lock);
    }
    s->destroy_view(device, view, a);
}

/* The view's format and sample count (0: not seen; format UNDEFINED then). */
static VkFormat view_info(dr_state *s, VkImageView view, VkSampleCountFlagBits *samples)
{
    VkFormat f = VK_FORMAT_UNDEFINED;
    *samples = VK_SAMPLE_COUNT_1_BIT;
    pthread_mutex_lock(&s->lock);
    for (view_rec *v = s->views[hb((uint64_t)(uintptr_t)view)]; v; v = v->next)
        if (v->view == view) {
            f = v->format;
            *samples = v->samples;
            break;
        }
    pthread_mutex_unlock(&s->lock);
    return f;
}

/* ------------------------------------------------------------------ render passes */

static int has_depth(VkFormat f)
{
    switch (f) {
    case VK_FORMAT_D16_UNORM: case VK_FORMAT_X8_D24_UNORM_PACK32: case VK_FORMAT_D32_SFLOAT:
    case VK_FORMAT_D16_UNORM_S8_UINT: case VK_FORMAT_D24_UNORM_S8_UINT: case VK_FORMAT_D32_SFLOAT_S8_UINT:
        return 1;
    default:
        return 0;
    }
}

static int has_stencil(VkFormat f)
{
    return f == VK_FORMAT_S8_UINT || f == VK_FORMAT_D16_UNORM_S8_UINT || f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT_S8_UINT;
}

/* A layout of one aspect (separate depth/stencil layouts) as the other aspect's counterpart. */
static VkImageLayout stencil_for(VkImageLayout depth)
{
    switch (depth) {
    case VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL: return VK_IMAGE_LAYOUT_STENCIL_ATTACHMENT_OPTIMAL;
    case VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL: return VK_IMAGE_LAYOUT_STENCIL_READ_ONLY_OPTIMAL;
    default: return depth;
    }
}

static VkImageLayout depth_for(VkImageLayout stencil)
{
    switch (stencil) {
    case VK_IMAGE_LAYOUT_STENCIL_ATTACHMENT_OPTIMAL: return VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    case VK_IMAGE_LAYOUT_STENCIL_READ_ONLY_OPTIMAL: return VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
    default: return stencil;
    }
}

static int separate_layout(VkImageLayout l)
{
    return l == VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL || l == VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL ||
           l == VK_IMAGE_LAYOUT_STENCIL_ATTACHMENT_OPTIMAL || l == VK_IMAGE_LAYOUT_STENCIL_READ_ONLY_OPTIMAL;
}

static VkRenderPass make_pass(dr_state *s, VkDevice device, const rp_key *k)
{
    unsigned b = key_hash(k);
    pthread_mutex_lock(&s->lock);
    for (rp_rec *r = s->rps[b]; r; r = r->next)
        if (!memcmp(&r->key, k, sizeof(*k))) {
            VkRenderPass rp = r->rp;
            pthread_mutex_unlock(&s->lock);
            return rp;
        }
    pthread_mutex_unlock(&s->lock);

    VkAttachmentDescription2 att[2 * MAXC + 2];
    VkAttachmentDescriptionStencilLayout att_sl[2];
    VkAttachmentReference2 cref[MAXC], rref[MAXC], dsref, dsres;
    VkAttachmentReferenceStencilLayout ref_sl[2];
    VkSubpassDescriptionDepthStencilResolve dsr;
    uint32_t na = 0;
    int any_res = 0;
    memset(att, 0, sizeof(att));
    for (uint32_t i = 0; i < k->ncolor; i++) {
        const att_key *c = &k->color[i];
        cref[i] = (VkAttachmentReference2){VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2, NULL, VK_ATTACHMENT_UNUSED, c->layout,
                                           VK_IMAGE_ASPECT_COLOR_BIT};
        rref[i] = (VkAttachmentReference2){VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2, NULL, VK_ATTACHMENT_UNUSED,
                                           VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_ASPECT_COLOR_BIT};
        if (!c->format) continue;
        att[na] = (VkAttachmentDescription2){VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2, NULL, 0, (VkFormat)c->format,
                                             (VkSampleCountFlagBits)c->samples, (VkAttachmentLoadOp)c->load,
                                             (VkAttachmentStoreOp)c->store, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                             VK_ATTACHMENT_STORE_OP_DONT_CARE, (VkImageLayout)c->layout, (VkImageLayout)c->layout};
        cref[i].attachment = na++;
        if (c->res_format) {
            att[na] = (VkAttachmentDescription2){VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2, NULL, 0, (VkFormat)c->res_format,
                                                 VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_STORE,
                                                 VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE,
                                                 (VkImageLayout)c->res_layout, (VkImageLayout)c->res_layout};
            rref[i].attachment = na++;
            rref[i].layout = (VkImageLayout)c->res_layout;
            any_res = 1;
        }
    }
    VkSubpassDescription2 sp = {VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2, NULL, 0, VK_PIPELINE_BIND_POINT_GRAPHICS, k->view_mask, 0, NULL,
                                k->ncolor, k->ncolor ? cref : NULL, any_res ? rref : NULL, NULL, 0, NULL};
    if (k->has_ds) {
        VkFormat f = (VkFormat)k->ds_format;
        VkImageLayout dl = (VkImageLayout)k->dlayout, sl = (VkImageLayout)k->slayout;
        att[na] = (VkAttachmentDescription2){VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2, NULL, 0, f, (VkSampleCountFlagBits)k->ds_samples,
                                             (VkAttachmentLoadOp)k->dload, (VkAttachmentStoreOp)k->dstore,
                                             (VkAttachmentLoadOp)k->sload, (VkAttachmentStoreOp)k->sstore, dl, dl};
        VkImageAspectFlags asp = (has_depth(f) ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) | (has_stencil(f) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
        dsref = (VkAttachmentReference2){VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2, NULL, na, dl, asp};
        if (has_stencil(f) && has_depth(f) && dl != sl) {
            att_sl[0] = (VkAttachmentDescriptionStencilLayout){VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_STENCIL_LAYOUT, NULL, sl, sl};
            att[na].pNext = &att_sl[0];
            ref_sl[0] = (VkAttachmentReferenceStencilLayout){VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_STENCIL_LAYOUT, NULL, sl};
            dsref.pNext = &ref_sl[0];
        }
        na++;
        sp.pDepthStencilAttachment = &dsref;
        if (k->ds_res_format && s->ds_resolve) {
            VkImageLayout rdl = (VkImageLayout)k->ds_res_dlayout, rsl = (VkImageLayout)k->ds_res_slayout;
            att[na] = (VkAttachmentDescription2){VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2, NULL, 0, (VkFormat)k->ds_res_format,
                                                 VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_STORE,
                                                 VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_STORE, rdl, rdl};
            dsres = (VkAttachmentReference2){VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2, NULL, na, rdl, asp};
            if (has_stencil(f) && has_depth(f) && rdl != rsl) {
                att_sl[1] = (VkAttachmentDescriptionStencilLayout){VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_STENCIL_LAYOUT, NULL, rsl, rsl};
                att[na].pNext = &att_sl[1];
                ref_sl[1] = (VkAttachmentReferenceStencilLayout){VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_STENCIL_LAYOUT, NULL, rsl};
                dsres.pNext = &ref_sl[1];
            }
            na++;
            dsr = (VkSubpassDescriptionDepthStencilResolve){VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE, NULL,
                                                            (VkResolveModeFlagBits)k->dres_mode, (VkResolveModeFlagBits)k->sres_mode,
                                                            &dsres};
            sp.pNext = &dsr;
        }
    }
    VkRenderPassCreateInfo2 ci = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2, NULL, 0, na, na ? att : NULL, 1, &sp, 0, NULL, 0, NULL};
    VkRenderPass rp = VK_NULL_HANDLE;
    vkb_srv_table *t = vkb_srv_current_table();
    VkResult r = s->create_rp2(device, &ci, NULL, &rp);
    if (r != VK_SUCCESS) {
        static int told;
        if (__atomic_fetch_add(&told, 1, __ATOMIC_RELAXED) < 4)
            VKB_ERR("dynamic rendering emulation: vkCreateRenderPass2 failed (%d): %u colour, depth/stencil %u (format %u), view mask 0x%x",
                    r, k->ncolor, k->has_ds, k->ds_format, k->view_mask);
        return VK_NULL_HANDLE;
    }
    rp_rec *n = calloc(1, sizeof(*n));
    if (!n) return rp; /* leaks one pass; still right */
    n->key = *k;
    n->rp = rp;
    pthread_mutex_lock(&s->lock);
    /* another thread may have made the same one meanwhile: keep the first */
    for (rp_rec *o = s->rps[b]; o; o = o->next)
        if (!memcmp(&o->key, k, sizeof(*k))) {
            VkRenderPass keep = o->rp;
            pthread_mutex_unlock(&s->lock);
            s->destroy_rp(device, rp, NULL);
            free(n);
            return keep;
        }
    n->next = s->rps[b];
    s->rps[b] = n;
    uint32_t total = ++s->nrp;
    pthread_mutex_unlock(&s->lock);
    if (total == 1 || total == 64 || total == 1024)
        VKB_INFO("dynamic rendering emulation: %u render pass(es) made on device %u", total, t ? t->id : 0);
    return rp;
}

/* A pass compatible with a pipeline's or a secondary's formats (ops and layouts do not count). */
static VkRenderPass compat_pass(dr_state *s, VkDevice device, uint32_t view_mask, uint32_t ncolor, const VkFormat *colors, VkFormat depth,
                                VkFormat stencil, VkSampleCountFlagBits samples)
{
    rp_key k;
    memset(&k, 0, sizeof(k));
    k.view_mask = view_mask;
    k.ncolor = ncolor < MAXC ? ncolor : MAXC;
    if (!samples) samples = VK_SAMPLE_COUNT_1_BIT;
    for (uint32_t i = 0; i < k.ncolor; i++) {
        att_key *c = &k.color[i];
        c->format = colors ? colors[i] : VK_FORMAT_UNDEFINED;
        if (!c->format) continue;
        c->layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        c->samples = samples;
        c->load = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        c->store = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    }
    VkFormat ds = depth ? depth : stencil;
    if (ds) {
        k.has_ds = 1;
        k.ds_format = ds;
        k.ds_samples = samples;
        k.dload = k.sload = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        k.dstore = k.sstore = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        k.dlayout = k.slayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    }
    return make_pass(s, device, &k);
}

/* ------------------------------------------------------------------ pipelines and secondaries */

static VKAPI_ATTR VkResult VKAPI_CALL d_CreateGraphicsPipelines(VkDevice device, VkPipelineCache cache, uint32_t n,
                                                               const VkGraphicsPipelineCreateInfo *infos, const VkAllocationCallbacks *a,
                                                               VkPipeline *out)
{
    dr_state *s = st();
    for (uint32_t i = 0; i < n; i++) {
        VkGraphicsPipelineCreateInfo *ci = (VkGraphicsPipelineCreateInfo *)&infos[i];
        if (ci->renderPass) continue; /* VkPipelineRenderingCreateInfo is ignored then */
        if (ci->flags & VK_PIPELINE_CREATE_LIBRARY_BIT_KHR) continue; /* pipeline libraries: not on the drivers this is for */
        /* The server's copy: changed in place, the rendering info taken out for the driver. */
        VkPipelineRenderingCreateInfo *ri = vkb_emu_chain_take(ci, VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO);
        VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
        if (ci->pMultisampleState && ci->pMultisampleState->rasterizationSamples) samples = ci->pMultisampleState->rasterizationSamples;
        ci->renderPass = ri ? compat_pass(s, device, ri->viewMask, ri->colorAttachmentCount, ri->pColorAttachmentFormats, ri->depthAttachmentFormat,
                                          ri->stencilAttachmentFormat, samples)
                            : compat_pass(s, device, 0, 0, NULL, VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, samples);
        ci->subpass = 0;
        __atomic_fetch_add(&s->pipelines, 1, __ATOMIC_RELAXED);
    }
    return s->create_gp(device, cache, n, infos, a, out);
}

static VKAPI_ATTR VkResult VKAPI_CALL d_BeginCommandBuffer(VkCommandBuffer cb, const VkCommandBufferBeginInfo *info)
{
    dr_state *s = st();
    VkCommandBufferInheritanceInfo *ii = info ? (VkCommandBufferInheritanceInfo *)info->pInheritanceInfo : NULL;
    if (ii && !ii->renderPass && (info->flags & VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT)) {
        VkCommandBufferInheritanceRenderingInfo *ri = vkb_emu_chain_take(ii, VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_RENDERING_INFO);
        if (ri) {
            vkb_srv_table *t = vkb_srv_current_table();
            ii->renderPass = compat_pass(s, t->device, ri->viewMask, ri->colorAttachmentCount, ri->pColorAttachmentFormats,
                                         ri->depthAttachmentFormat, ri->stencilAttachmentFormat, ri->rasterizationSamples);
            ii->subpass = 0;
            ii->framebuffer = VK_NULL_HANDLE;
            __atomic_fetch_add(&s->secondaries, 1, __ATOMIC_RELAXED);
        }
    }
    return s->begin_cb(cb, info);
}

/* ------------------------------------------------------------------ rendering */

void vkb_emu_dynrend_cb_reset(vkb_emu_device *e, void *p)
{
    cb_fbs *f = p;
    if (!f || !f->n) return;
    dr_state *s = e->dev->id < 4096 ? states[e->dev->id] : NULL;
    if (s)
        for (uint32_t i = 0; i < f->n; i++) s->destroy_fb(e->dev->device, f->fb[i], NULL);
    f->n = 0;
}

void vkb_emu_dynrend_cb_free(vkb_emu_device *e, void *p)
{
    cb_fbs *f = p;
    if (!f) return;
    vkb_emu_dynrend_cb_reset(e, f);
    free(f->fb);
    free(f);
}

static VkAttachmentLoadOp load_op(VkAttachmentLoadOp op, int resuming)
{
    return resuming ? VK_ATTACHMENT_LOAD_OP_LOAD : op;
}

static VkAttachmentStoreOp store_op(VkAttachmentStoreOp op, int suspending)
{
    return suspending ? VK_ATTACHMENT_STORE_OP_STORE : op;
}

static VKAPI_ATTR void VKAPI_CALL d_CmdBeginRendering(VkCommandBuffer cb, const VkRenderingInfo *ri)
{
    dr_state *s = st();
    vkb_srv_table *t = vkb_srv_current_table();
    int resuming = (ri->flags & VK_RENDERING_RESUMING_BIT) != 0, suspending = (ri->flags & VK_RENDERING_SUSPENDING_BIT) != 0;
    rp_key k;
    memset(&k, 0, sizeof(k));
    k.view_mask = ri->viewMask;
    k.ncolor = ri->colorAttachmentCount < MAXC ? ri->colorAttachmentCount : MAXC;
    VkImageView views[2 * MAXC + 2];
    VkClearValue clears[2 * MAXC + 2];
    uint32_t nv = 0;
    for (uint32_t i = 0; i < k.ncolor; i++) {
        const VkRenderingAttachmentInfo *a = &ri->pColorAttachments[i];
        att_key *c = &k.color[i];
        if (!a->imageView) continue;
        c->layout = a->imageLayout;
        VkSampleCountFlagBits sm;
        c->format = view_info(s, a->imageView, &sm);
        if (!c->format) {
            c->layout = 0;
            VKB_ONCE(VKB_LOG_ERROR, "dynamic rendering emulation: colour attachment %u's view %p was not seen made; left out (logged once)", i,
                     (void *)a->imageView);
            continue;
        }
        c->samples = sm;
        c->load = load_op(a->loadOp, resuming);
        c->store = store_op(a->storeOp, suspending);
        clears[nv] = a->clearValue;
        views[nv++] = a->imageView;
        if (a->resolveMode != VK_RESOLVE_MODE_NONE && a->resolveImageView) {
            VkSampleCountFlagBits rs;
            c->res_format = view_info(s, a->resolveImageView, &rs);
            if (c->res_format) {
                c->res_layout = a->resolveImageLayout;
                memset(&clears[nv], 0, sizeof(clears[nv]));
                views[nv++] = a->resolveImageView;
            }
        }
    }
    const VkRenderingAttachmentInfo *da = ri->pDepthAttachment && ri->pDepthAttachment->imageView ? ri->pDepthAttachment : NULL;
    const VkRenderingAttachmentInfo *sa = ri->pStencilAttachment && ri->pStencilAttachment->imageView ? ri->pStencilAttachment : NULL;
    if (da && sa && da->imageView != sa->imageView) {
        VKB_ONCE(VKB_LOG_ERROR, "dynamic rendering emulation: depth and stencil on different views; the stencil one left out (logged once)");
        sa = NULL;
    }
    const VkRenderingAttachmentInfo *dsa = da ? da : sa;
    if (dsa) {
        VkSampleCountFlagBits sm;
        VkFormat f = view_info(s, dsa->imageView, &sm);
        if (f) {
            k.has_ds = 1;
            k.ds_format = f;
            k.ds_samples = sm;
            /* the aspect not attached is kept as it was */
            k.dload = da ? load_op(da->loadOp, resuming) : has_depth(f) ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            k.dstore = da ? store_op(da->storeOp, suspending) : has_depth(f) ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
            k.sload = sa ? load_op(sa->loadOp, resuming) : has_stencil(f) ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            k.sstore = sa ? store_op(sa->storeOp, suspending) : has_stencil(f) ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
            VkImageLayout dl = da ? da->imageLayout : depth_for(sa->imageLayout);
            VkImageLayout sl = sa ? sa->imageLayout : stencil_for(da->imageLayout);
            if (!separate_layout(dl) && !separate_layout(sl)) sl = dl; /* combined layouts: one for both */
            k.dlayout = dl;
            k.slayout = sl;
            VkClearValue cv;
            memset(&cv, 0, sizeof(cv));
            cv.depthStencil.depth = da ? da->clearValue.depthStencil.depth : 0.0f;
            cv.depthStencil.stencil = sa ? sa->clearValue.depthStencil.stencil : 0;
            clears[nv] = cv;
            views[nv++] = dsa->imageView;
            /* depth/stencil resolve: the depth's view and modes, the stencil's mode */
            const VkRenderingAttachmentInfo *ra = da && da->resolveMode && da->resolveImageView ? da
                                                : sa && sa->resolveMode && sa->resolveImageView ? sa : NULL;
            if (ra && s->ds_resolve) {
                VkSampleCountFlagBits rs;
                VkFormat rf = view_info(s, ra->resolveImageView, &rs);
                if (rf) {
                    k.ds_res_format = rf;
                    k.dres_mode = da ? da->resolveMode : VK_RESOLVE_MODE_NONE;
                    k.sres_mode = sa ? sa->resolveMode : VK_RESOLVE_MODE_NONE;
                    VkImageLayout rdl = da && da->resolveImageView ? da->resolveImageLayout : depth_for(sa->resolveImageLayout);
                    VkImageLayout rsl = sa && sa->resolveImageView ? sa->resolveImageLayout : stencil_for(da->resolveImageLayout);
                    if (!separate_layout(rdl) && !separate_layout(rsl)) rsl = rdl;
                    k.ds_res_dlayout = rdl;
                    k.ds_res_slayout = rsl;
                    memset(&clears[nv], 0, sizeof(clears[nv]));
                    views[nv++] = ra->resolveImageView;
                }
            } else if (ra) {
                VKB_ONCE(VKB_LOG_ERROR, "dynamic rendering emulation: a depth/stencil resolve without VK_KHR_depth_stencil_resolve; not resolved (logged once)");
            }
        }
    }
    VkRenderPass rp = make_pass(s, t->device, &k);
    if (!rp) return;
    uint32_t w = (uint32_t)(ri->renderArea.offset.x > 0 ? ri->renderArea.offset.x : 0) + ri->renderArea.extent.width;
    uint32_t h = (uint32_t)(ri->renderArea.offset.y > 0 ? ri->renderArea.offset.y : 0) + ri->renderArea.extent.height;
    VkFramebufferCreateInfo fci = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, NULL, 0, rp, nv, nv ? views : NULL, w ? w : 1, h ? h : 1,
                                   ri->viewMask ? 1 : (ri->layerCount ? ri->layerCount : 1)};
    VkFramebuffer fb = VK_NULL_HANDLE;
    VkResult r = s->create_fb(t->device, &fci, NULL, &fb);
    if (r != VK_SUCCESS) {
        VKB_ONCE(VKB_LOG_ERROR, "dynamic rendering emulation: vkCreateFramebuffer failed (%d); rendering dropped (logged once)", r);
        return;
    }
    cb_fbs *f = vkb_emu_cb_dynrend(t->emu, cb, sizeof(cb_fbs));
    if (f && f->n == f->cap) {
        uint32_t cap = f->cap ? f->cap * 2 : 8;
        VkFramebuffer *nf = realloc(f->fb, cap * sizeof(*nf));
        if (nf) {
            f->fb = nf;
            f->cap = cap;
        }
    }
    if (f && f->n < f->cap) f->fb[f->n++] = fb;
    VkRenderPassBeginInfo bi = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, NULL, rp, fb, ri->renderArea, nv, nv ? clears : NULL};
    s->begin_rp(cb, &bi, (ri->flags & VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT) ? VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS
                                                                                            : VK_SUBPASS_CONTENTS_INLINE);
    uint64_t nr = __atomic_add_fetch(&s->renderings, 1, __ATOMIC_RELAXED);
    if (nr == 1) VKB_INFO("dynamic rendering emulation: first vkCmdBeginRendering as a render pass (%u colour, depth/stencil %u)", k.ncolor, k.has_ds);
}

static VKAPI_ATTR void VKAPI_CALL d_CmdEndRendering(VkCommandBuffer cb)
{
    st()->end_rp(cb);
}

/* vkCmdBindVertexBuffers2 on a driver without it, for Zink (client/icd.c: beyond_version). The
 * sizes only bound the reads (dropped); strides would need dynamic stride, which such a device
 * does not offer (no VK_EXT_extended_dynamic_state), so they are not expected. */
static VKAPI_ATTR void VKAPI_CALL d_CmdBindVertexBuffers2(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkBuffer *buffers,
                                                          const VkDeviceSize *offsets, const VkDeviceSize *sizes, const VkDeviceSize *strides)
{
    if (strides) VKB_ONCE(VKB_LOG_ERROR, "dynamic rendering emulation: vkCmdBindVertexBuffers2 with strides on a driver without it; strides dropped (logged once)");
    st()->bind_vb(cb, first, n, buffers, offsets);
}

/* ------------------------------------------------------------------ install */

static int dev_has_ext(vkb_srv_table *dev, const char *name)
{
    const vkb_dispatch *r = &dev->real;
    if (!r->vkEnumerateDeviceExtensionProperties) return 0;
    uint32_t n = 0;
    r->vkEnumerateDeviceExtensionProperties(dev->physical, NULL, &n, NULL);
    VkExtensionProperties *e = calloc(n ? n : 1, sizeof(*e));
    if (!e) return 0;
    r->vkEnumerateDeviceExtensionProperties(dev->physical, NULL, &n, e);
    int have = 0;
    for (uint32_t i = 0; i < n && !have; i++) have = !strcmp(e[i].extensionName, name);
    free(e);
    return have;
}

/* vkCreateDevice enables it whenever the driver has it and this emulation is on (server_instance.c). */
static int ds_resolve_enabled(vkb_srv_table *dev)
{
    VkPhysicalDeviceProperties p;
    dev->real.vkGetPhysicalDeviceProperties(dev->physical, &p);
    return (p.apiVersion >= VK_API_VERSION_1_2 && dev->api_version >= VK_API_VERSION_1_2) || dev_has_ext(dev, "VK_KHR_depth_stencil_resolve");
}

void vkb_emu_dynrend_install(vkb_srv_table *dev)
{
    if (!(dev->emu->flags & VKB_EMU_DYN_RENDER) || dev->id >= 4096) return;
    vkb_dispatch *dt = &dev->dt;
    if (!dt->vkCreateRenderPass2 || !dt->vkCmdBeginRenderPass || !dt->vkCmdEndRenderPass) {
        VKB_ERR("dynamic rendering emulation: no vkCreateRenderPass2 on device %u (VK_KHR_create_renderpass2 not enabled); not installed", dev->id);
        return;
    }
    dr_state *s = calloc(1, sizeof(*s));
    if (!s) return;
    pthread_mutex_init(&s->lock, NULL);
    s->create_rp2 = dt->vkCreateRenderPass2;
    s->destroy_rp = dt->vkDestroyRenderPass;
    s->create_fb = dt->vkCreateFramebuffer;
    s->destroy_fb = dt->vkDestroyFramebuffer;
    s->begin_rp = dt->vkCmdBeginRenderPass;
    s->end_rp = dt->vkCmdEndRenderPass;
    s->create_gp = dt->vkCreateGraphicsPipelines;
    s->begin_cb = dt->vkBeginCommandBuffer;
    s->create_image = dt->vkCreateImage;
    s->destroy_image = dt->vkDestroyImage;
    s->create_view = dt->vkCreateImageView;
    s->destroy_view = dt->vkDestroyImageView;
    s->bind_vb = dt->vkCmdBindVertexBuffers;
    s->ds_resolve = ds_resolve_enabled(dev);
    states[dev->id] = s;
    dt->vkCreateGraphicsPipelines = d_CreateGraphicsPipelines;
    dt->vkBeginCommandBuffer = d_BeginCommandBuffer;
    dt->vkCreateImage = d_CreateImage;
    dt->vkDestroyImage = d_DestroyImage;
    dt->vkCreateImageView = d_CreateImageView;
    dt->vkDestroyImageView = d_DestroyImageView;
    dt->vkCmdBeginRendering = d_CmdBeginRendering;
    dt->vkCmdEndRendering = d_CmdEndRendering;
    if (!dt->vkCmdBindVertexBuffers2) dt->vkCmdBindVertexBuffers2 = d_CmdBindVertexBuffers2;
    VKB_INFO("dynamic rendering emulated on device %u: renderings as render passes%s", dev->id,
             s->ds_resolve ? "" : " (no depth/stencil resolve: VK_KHR_depth_stencil_resolve not enabled)");
}

void vkb_emu_dynrend_uninstall(vkb_srv_table *dev)
{
    if (dev->id >= 4096 || !states[dev->id]) return;
    dr_state *s = states[dev->id];
    states[dev->id] = NULL;
    if (s->renderings || s->pipelines)
        VKB_INFO("dynamic rendering emulation, device %u: %llu rendering(s), %llu pipeline(s), %llu secondary begin(s), %u render pass(es)",
                 dev->id, (unsigned long long)s->renderings, (unsigned long long)s->pipelines, (unsigned long long)s->secondaries, s->nrp);
    for (int i = 0; i < MAP_BUCKETS; i++) {
        while (s->rps[i]) {
            rp_rec *r = s->rps[i];
            s->rps[i] = r->next;
            s->destroy_rp(dev->device, r->rp, NULL);
            free(r);
        }
        while (s->views[i]) {
            view_rec *v = s->views[i];
            s->views[i] = v->next;
            free(v);
        }
        while (s->imgs[i]) {
            img_rec *m = s->imgs[i];
            s->imgs[i] = m->next;
            free(m);
        }
    }
    pthread_mutex_destroy(&s->lock);
    free(s);
}
