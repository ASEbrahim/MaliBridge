/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * stencil_layout (MK-11, Arm erratum 4021056, r41p0-r54p0, fixed r54p1): the driver ignores a
 * separate stencil layout given with VkAttachmentDescriptionStencilLayout or
 * VkAttachmentReferenceStencilLayout and uses the depth one (the image's combined layout) for
 * both aspects. With the depth aspect read-only and the stencil writable, the stencil can be
 * discarded at the end of the render pass. Arm's workaround: give the packed depth/stencil image
 * one combined layout that suits both aspects, instead of a separate stencil layout.
 *
 * The fix does that in vkCreateRenderPass2: for an attachment (its initial and final layouts) or
 * a reference (input, depth/stencil, depth/stencil resolve) of a format with a stencil aspect
 * that carries the stencil struct, the two layouts become the one that means the same for each
 * aspect - depth attachment + stencil read-only is DEPTH_ATTACHMENT_STENCIL_READ_ONLY_OPTIMAL,
 * depth read-only + stencil attachment DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL, and so on;
 * two equal layouts (GENERAL, UNDEFINED, TRANSFER_SRC...) stay what they are; a stencil-only
 * format takes the stencil layout - and the struct is taken out of the chain. Vulkan defines the
 * combined layouts as those pairs, so this is exact on any driver. A pair with no combined form
 * (a depth layout read by shaders with a stencil attachment, say) is left as it is, and counted.
 * The caller's structures are changed for the driver's call and put back after.
 * vkCmdBeginRendering gets the same for a depth and a stencil attachment on one view with layouts
 * that differ (not named in the erratum; the rewrite is exact, so it costs nothing to cover).
 * Image barriers on one aspect are not touched: the erratum is the render pass's layouts.
 *
 * Imitation (host): beneath the fix, a single-subpass render pass whose depth/stencil reference
 * has the stencil struct with a writable stencil and a read-only depth layout loses its stencil
 * at the end of the pass (cleared to 0 with vkCmdClearAttachments before vkCmdEndRenderPass).
 */
#define _GNU_SOURCE
#include "server_errata.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define BUCKETS 256

enum { L_OTHER, L_W, L_R };

typedef struct rp_rec {
    VkRenderPass h;
    struct rp_rec *next;
} rp_rec;

typedef struct cb_rec {
    VkCommandBuffer h;
    VkRect2D area;
    struct cb_rec *next;
} cb_rec;

typedef struct vkb_errata_ds {
    pthread_mutex_t lock;
    vkb_dispatch fix, imi;
    uint64_t passes, passes_changed, layouts, left, renders_changed;
    uint64_t imi_flagged, imi_cleared;
    rp_rec *rp[BUCKETS]; /* the imitation's render passes with the pattern */
    cb_rec *cb[BUCKETS]; /* the imitation's command buffers inside one of them */
} vkb_errata_ds;

static vkb_errata_ds *last_ds;

#define ADD(x, n) __atomic_add_fetch(&(x), (n), __ATOMIC_RELAXED)

static vkb_errata_ds *D(void)
{
    vkb_errata_dev *e = vkb_errata_cur();
    return e && e->ds ? e->ds : __atomic_load_n(&last_ds, __ATOMIC_ACQUIRE);
}

static unsigned hsh(uint64_t k)
{
    k ^= k >> 31;
    k *= 0x9e3779b97f4a7c15ull;
    return (unsigned)(k >> 56) % BUCKETS;
}

/* ---- layouts */

static int depth_class(VkImageLayout l)
{
    switch ((int)l) {
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
    case VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL:
    case VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_STENCIL_READ_ONLY_OPTIMAL:
    case VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL:
        return L_W;
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:
    case VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL:
    case VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL:
    case VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL:
        return L_R;
    default:
        return L_OTHER;
    }
}

static int stencil_class(VkImageLayout l)
{
    switch ((int)l) {
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
    case VK_IMAGE_LAYOUT_STENCIL_ATTACHMENT_OPTIMAL:
    case VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL:
    case VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL:
        return L_W;
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:
    case VK_IMAGE_LAYOUT_STENCIL_READ_ONLY_OPTIMAL:
    case VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_STENCIL_READ_ONLY_OPTIMAL:
    case VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL:
        return L_R;
    default:
        return L_OTHER;
    }
}

/* 2: depth and stencil, 1: stencil only, 0: no stencil aspect */
static int stencil_format(VkFormat f)
{
    switch (f) {
    case VK_FORMAT_D16_UNORM_S8_UINT:
    case VK_FORMAT_D24_UNORM_S8_UINT:
    case VK_FORMAT_D32_SFLOAT_S8_UINT:
        return 2;
    case VK_FORMAT_S8_UINT:
        return 1;
    default:
        return 0;
    }
}

/* The one layout meaning d for the depth aspect and s for the stencil: 1 and *out, or 0 */
static int merge(int kind, VkImageLayout d, VkImageLayout s, VkImageLayout *out)
{
    if (kind == 1) return *out = s, 1;
    if (d == s) return *out = d, 1;
    int dc = depth_class(d), sc = stencil_class(s);
    if (dc == L_OTHER || sc == L_OTHER) return 0;
    *out = dc == L_W ? (sc == L_W ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_STENCIL_READ_ONLY_OPTIMAL)
                     : (sc == L_W ? VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
    return 1;
}

static const VkBaseInStructure *chain_find(const void *pnext, VkStructureType type, const VkBaseInStructure **prev)
{
    *prev = NULL;
    for (const VkBaseInStructure *b = pnext; b; *prev = b, b = b->pNext)
        if (b->sType == type) return b;
    return NULL;
}

/* ---- what the fix changes, put back after the call */

typedef struct {
    void *addr;
    uint64_t old;
    int ptr; /* a pointer (else a VkImageLayout) */
} undo;

typedef struct {
    undo *v;
    unsigned n, cap;
    int oom;
} undo_log;

static void set_layout(undo_log *u, const VkImageLayout *at, VkImageLayout v)
{
    if (*at == v) return;
    if (u->n == u->cap) {
        unsigned cap = u->cap ? u->cap * 2 : 32;
        undo *nv = realloc(u->v, cap * sizeof(*nv));
        if (!nv) {
            u->oom = 1;
            return;
        }
        u->v = nv, u->cap = cap;
    }
    u->v[u->n++] = (undo){(void *)at, (uint64_t)*at, 0};
    *(VkImageLayout *)at = v;
}

static void set_ptr(undo_log *u, const void *const *at, const void *v)
{
    if (u->n == u->cap) {
        unsigned cap = u->cap ? u->cap * 2 : 32;
        undo *nv = realloc(u->v, cap * sizeof(*nv));
        if (!nv) {
            u->oom = 1;
            return;
        }
        u->v = nv, u->cap = cap;
    }
    u->v[u->n++] = (undo){(void *)at, (uint64_t)(uintptr_t)*at, 1};
    *(const void **)at = v;
}

static void undo_all(undo_log *u)
{
    while (u->n) {
        undo *x = &u->v[--u->n];
        if (x->ptr)
            *(const void **)x->addr = (const void *)(uintptr_t)x->old;
        else
            *(VkImageLayout *)x->addr = (VkImageLayout)x->old;
    }
    free(u->v);
}

/* The struct taken out of the chain that starts at *head */
static void chain_unlink(undo_log *u, const void *const *head, const VkBaseInStructure *prev, const VkBaseInStructure *s)
{
    set_ptr(u, prev ? (const void *const *)&prev->pNext : head, s->pNext);
}

/* One reference: 1 changed, 0 nothing to do, -1 left as it is */
static int fix_ref(undo_log *u, const VkRenderPassCreateInfo2 *ci, const VkAttachmentReference2 *r)
{
    if (!r || r->attachment == VK_ATTACHMENT_UNUSED || r->attachment >= ci->attachmentCount) return 0;
    int kind = stencil_format(ci->pAttachments[r->attachment].format);
    const VkBaseInStructure *prev;
    const VkAttachmentReferenceStencilLayout *sl =
        (const void *)chain_find(r->pNext, VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_STENCIL_LAYOUT, &prev);
    if (!kind || !sl) return 0;
    VkImageLayout l;
    if (!merge(kind, r->layout, sl->stencilLayout, &l)) return -1;
    set_layout(u, &r->layout, l);
    chain_unlink(u, &r->pNext, prev, (const VkBaseInStructure *)sl);
    return 1;
}

static VKAPI_ATTR VkResult VKAPI_CALL f_CreateRenderPass2(VkDevice dev, const VkRenderPassCreateInfo2 *ci, const VkAllocationCallbacks *a,
                                                          VkRenderPass *out)
{
    vkb_errata_ds *m = D();
    undo_log u = {0};
    unsigned changed = 0, left = 0;
    for (uint32_t i = 0; i < ci->attachmentCount; i++) {
        const VkAttachmentDescription2 *ad = &ci->pAttachments[i];
        int kind = stencil_format(ad->format);
        const VkBaseInStructure *prev;
        const VkAttachmentDescriptionStencilLayout *sl =
            (const void *)chain_find(ad->pNext, VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_STENCIL_LAYOUT, &prev);
        if (!kind || !sl) continue;
        VkImageLayout li, lf;
        if (merge(kind, ad->initialLayout, sl->stencilInitialLayout, &li) && merge(kind, ad->finalLayout, sl->stencilFinalLayout, &lf)) {
            set_layout(&u, &ad->initialLayout, li);
            set_layout(&u, &ad->finalLayout, lf);
            chain_unlink(&u, &ad->pNext, prev, (const VkBaseInStructure *)sl);
            changed++;
        } else
            left++;
    }
    for (uint32_t s = 0; s < ci->subpassCount; s++) {
        const VkSubpassDescription2 *sd = &ci->pSubpasses[s];
        int r;
        for (uint32_t k = 0; k < sd->inputAttachmentCount; k++)
            if ((r = fix_ref(&u, ci, &sd->pInputAttachments[k]))) r > 0 ? changed++ : left++;
        if ((r = fix_ref(&u, ci, sd->pDepthStencilAttachment))) r > 0 ? changed++ : left++;
        const VkBaseInStructure *prev;
        const VkSubpassDescriptionDepthStencilResolve *rs =
            (const void *)chain_find(sd->pNext, VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE, &prev);
        if (rs && (r = fix_ref(&u, ci, rs->pDepthStencilResolveAttachment))) r > 0 ? changed++ : left++;
    }
    VkResult res;
    if (u.oom) { /* (never) the call as the app made it */
        undo_all(&u);
        u = (undo_log){0};
        changed = 0;
    }
    res = m->fix.vkCreateRenderPass2(dev, ci, a, out);
    undo_all(&u);
    ADD(m->passes, 1);
    if (changed) {
        ADD(m->layouts, changed);
        if (!__atomic_fetch_add(&m->passes_changed, 1, __ATOMIC_RELAXED))
            vkb_errata_info_once("errata: stencil_layout: a render pass's separate stencil layouts merged into combined ones (%u): %d",
                                 changed, res);
    }
    if (left) ADD(m->left, left);
    return res;
}

static VKAPI_ATTR void VKAPI_CALL f_CmdBeginRendering(VkCommandBuffer cb, const VkRenderingInfo *ri)
{
    vkb_errata_ds *m = D();
    const VkRenderingAttachmentInfo *d = ri->pDepthAttachment, *s = ri->pStencilAttachment;
    VkImageLayout l, rl = VK_IMAGE_LAYOUT_UNDEFINED;
    if (d && s && d->imageView && d->imageView == s->imageView && d->imageLayout != s->imageLayout &&
        merge(2, d->imageLayout, s->imageLayout, &l)) {
        int res_both = d->resolveMode != VK_RESOLVE_MODE_NONE && s->resolveMode != VK_RESOLVE_MODE_NONE &&
                       d->resolveImageView && d->resolveImageView == s->resolveImageView;
        if (res_both && d->resolveImageLayout != s->resolveImageLayout && !merge(2, d->resolveImageLayout, s->resolveImageLayout, &rl))
            res_both = 0;
        VkRenderingAttachmentInfo dd = *d, ss = *s;
        dd.imageLayout = ss.imageLayout = l;
        if (res_both && d->resolveImageLayout != s->resolveImageLayout) dd.resolveImageLayout = ss.resolveImageLayout = rl;
        VkRenderingInfo r2 = *ri;
        r2.pDepthAttachment = &dd, r2.pStencilAttachment = &ss;
        if (!__atomic_fetch_add(&m->renders_changed, 1, __ATOMIC_RELAXED))
            vkb_errata_info_once("errata: stencil_layout: vkCmdBeginRendering's depth and stencil layouts merged (0x%x, 0x%x -> 0x%x)",
                                 d->imageLayout, s->imageLayout, l);
        m->fix.vkCmdBeginRendering(cb, &r2);
        return;
    }
    m->fix.vkCmdBeginRendering(cb, ri);
}

/* ---- the imitation */

static int pattern(const VkRenderPassCreateInfo2 *ci)
{
    if (ci->subpassCount != 1) return 0;
    const VkAttachmentReference2 *r = ci->pSubpasses[0].pDepthStencilAttachment;
    if (!r || r->attachment == VK_ATTACHMENT_UNUSED || r->attachment >= ci->attachmentCount ||
        stencil_format(ci->pAttachments[r->attachment].format) != 2)
        return 0;
    const VkBaseInStructure *prev;
    const VkAttachmentReferenceStencilLayout *sl =
        (const void *)chain_find(r->pNext, VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_STENCIL_LAYOUT, &prev);
    return sl && stencil_class(sl->stencilLayout) == L_W && depth_class(r->layout) == L_R;
}

static int rp_has(vkb_errata_ds *m, VkRenderPass h)
{
    int f = 0;
    pthread_mutex_lock(&m->lock);
    for (rp_rec *r = m->rp[hsh((uint64_t)(uintptr_t)h)]; r && !f; r = r->next) f = r->h == h;
    pthread_mutex_unlock(&m->lock);
    return f;
}

static VKAPI_ATTR VkResult VKAPI_CALL i_CreateRenderPass2(VkDevice dev, const VkRenderPassCreateInfo2 *ci, const VkAllocationCallbacks *a,
                                                          VkRenderPass *out)
{
    vkb_errata_ds *m = D();
    VkResult r = m->imi.vkCreateRenderPass2(dev, ci, a, out);
    if (r == VK_SUCCESS && pattern(ci)) {
        rp_rec *x = malloc(sizeof(*x));
        if (x) {
            x->h = *out;
            unsigned k = hsh((uint64_t)(uintptr_t)*out);
            pthread_mutex_lock(&m->lock);
            x->next = m->rp[k];
            m->rp[k] = x;
            pthread_mutex_unlock(&m->lock);
            ADD(m->imi_flagged, 1);
        }
    }
    return r;
}

static VKAPI_ATTR void VKAPI_CALL i_DestroyRenderPass(VkDevice dev, VkRenderPass h, const VkAllocationCallbacks *a)
{
    vkb_errata_ds *m = D();
    if (h) {
        pthread_mutex_lock(&m->lock);
        for (rp_rec **pp = &m->rp[hsh((uint64_t)(uintptr_t)h)]; *pp; pp = &(*pp)->next)
            if ((*pp)->h == h) {
                rp_rec *x = *pp;
                *pp = x->next;
                free(x);
                break;
            }
        pthread_mutex_unlock(&m->lock);
    }
    m->imi.vkDestroyRenderPass(dev, h, a);
}

static void begin(vkb_errata_ds *m, VkCommandBuffer cb, const VkRenderPassBeginInfo *bi)
{
    if (!rp_has(m, bi->renderPass)) return;
    cb_rec *x = malloc(sizeof(*x));
    if (!x) return;
    x->h = cb, x->area = bi->renderArea;
    unsigned k = hsh((uint64_t)(uintptr_t)cb);
    pthread_mutex_lock(&m->lock);
    x->next = m->cb[k];
    m->cb[k] = x;
    pthread_mutex_unlock(&m->lock);
}

/* Before the pass ends: the stencil lost, when the command buffer is in such a pass */
static void end(vkb_errata_ds *m, VkCommandBuffer cb)
{
    cb_rec *x = NULL;
    pthread_mutex_lock(&m->lock);
    for (cb_rec **pp = &m->cb[hsh((uint64_t)(uintptr_t)cb)]; *pp; pp = &(*pp)->next)
        if ((*pp)->h == cb) {
            x = *pp;
            *pp = x->next;
            break;
        }
    pthread_mutex_unlock(&m->lock);
    if (!x) return;
    VkClearAttachment ca = {VK_IMAGE_ASPECT_STENCIL_BIT, 0, {.depthStencil = {0.0f, 0}}};
    VkClearRect cr = {x->area, 0, 1};
    m->imi.vkCmdClearAttachments(cb, 1, &ca, 1, &cr);
    ADD(m->imi_cleared, 1);
    free(x);
}

static VKAPI_ATTR void VKAPI_CALL i_CmdBeginRenderPass(VkCommandBuffer cb, const VkRenderPassBeginInfo *bi, VkSubpassContents c)
{
    vkb_errata_ds *m = D();
    m->imi.vkCmdBeginRenderPass(cb, bi, c);
    begin(m, cb, bi);
}

static VKAPI_ATTR void VKAPI_CALL i_CmdBeginRenderPass2(VkCommandBuffer cb, const VkRenderPassBeginInfo *bi, const VkSubpassBeginInfo *sb)
{
    vkb_errata_ds *m = D();
    m->imi.vkCmdBeginRenderPass2(cb, bi, sb);
    begin(m, cb, bi);
}

static VKAPI_ATTR void VKAPI_CALL i_CmdEndRenderPass(VkCommandBuffer cb)
{
    vkb_errata_ds *m = D();
    end(m, cb);
    m->imi.vkCmdEndRenderPass(cb);
}

static VKAPI_ATTR void VKAPI_CALL i_CmdEndRenderPass2(VkCommandBuffer cb, const VkSubpassEndInfo *se)
{
    vkb_errata_ds *m = D();
    end(m, cb);
    m->imi.vkCmdEndRenderPass2(cb, se);
}

/* ---- installs */

static vkb_errata_ds *ds_get(vkb_srv_table *t, vkb_errata_dev *e)
{
    if (e->ds) return e->ds;
    vkb_errata_ds *m = calloc(1, sizeof(*m));
    if (!m) return NULL;
    pthread_mutex_init(&m->lock, NULL);
    e->ds = m;
    if (!e->probe) __atomic_store_n(&last_ds, m, __ATOMIC_RELEASE);
    return m;
}

void vkb_errata_stencil_install(vkb_srv_table *t, vkb_errata_dev *e)
{
    vkb_errata_ds *m = ds_get(t, e);
    if (!m) return;
    m->fix = t->real;
    ERRATA_HOOK(t, vkCreateRenderPass2, f_CreateRenderPass2);
    if (t->real.vkCmdBeginRendering) ERRATA_HOOK(t, vkCmdBeginRendering, f_CmdBeginRendering);
}

void vkb_errata_stencil_imitate(vkb_srv_table *t, vkb_errata_dev *e)
{
    vkb_errata_ds *m = ds_get(t, e);
    if (!m || !t->real.vkCmdClearAttachments) return;
    m->imi = t->real;
    ERRATA_HOOK(t, vkCreateRenderPass2, i_CreateRenderPass2);
    ERRATA_HOOK(t, vkDestroyRenderPass, i_DestroyRenderPass);
    ERRATA_HOOK(t, vkCmdBeginRenderPass, i_CmdBeginRenderPass);
    ERRATA_HOOK(t, vkCmdBeginRenderPass2, i_CmdBeginRenderPass2);
    ERRATA_HOOK(t, vkCmdEndRenderPass, i_CmdEndRenderPass);
    ERRATA_HOOK(t, vkCmdEndRenderPass2, i_CmdEndRenderPass2);
}

void vkb_errata_ds_destroy(vkb_srv_table *t, vkb_errata_dev *e)
{
    vkb_errata_ds *m = e->ds;
    if (!m) return;
    if (!e->probe && ((e->on | e->imitated) >> VKB_ERRATUM_STENCIL_LAYOUT & 1) &&
        (m->passes_changed || m->left || m->renders_changed || m->imi_flagged))
        VKB_INFO("errata: stencil_layout: %llu render pass(es), %llu with separate stencil layouts merged (%llu layouts), %llu "
                 "left (no combined layout for the pair); %llu vkCmdBeginRendering merged; imitated: %llu pass(es) with the "
                 "pattern, %llu stencil(s) lost",
                 (unsigned long long)m->passes, (unsigned long long)m->passes_changed, (unsigned long long)m->layouts,
                 (unsigned long long)m->left, (unsigned long long)m->renders_changed, (unsigned long long)m->imi_flagged,
                 (unsigned long long)m->imi_cleared);
    for (unsigned k = 0; k < BUCKETS; k++) {
        for (rp_rec *r = m->rp[k], *n; r; r = n) n = r->next, free(r);
        for (cb_rec *r = m->cb[k], *n; r; r = n) n = r->next, free(r);
    }
    pthread_mutex_destroy(&m->lock);
    vkb_errata_ds *exp = m;
    __atomic_compare_exchange_n(&last_ds, &exp, NULL, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    free(m);
    e->ds = NULL;
}
