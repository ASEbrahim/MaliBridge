/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * vb_bake (MK-05, option B): dynamic vertex strides wrong even one binding per call.
 *
 * The startup probe vb_strides found a driver (Mali-G68 r38p1, Galaxy A16: MaliBridge#3) whose
 * vkCmdBindVertexBuffers2 pStrides come out wrong in every shape it tries, single-binding calls
 * included: every attribute read with one and the same stride. The split into single calls
 * (vb_split, option A) cannot help there, and Zink binds its vertex buffers with dynamic strides,
 * so Steam's interface drew nothing but black. The fix keeps the driver from drawing with dynamic
 * strides: they are baked into pipeline variants.
 *   - vkCreateGraphicsPipelines: a complete pipeline with VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_
 *     STRIDE is made without it, with the strides its create info names (usually the ones it is
 *     drawn with); its create info is kept, encoded, with its shaders' SPIR-V (modules can be gone
 *     by the time a variant is made: DXVK destroys them right after the pipeline).
 *   - vkCmdBindVertexBuffers2: the strides are noted per command buffer and passed on as they are:
 *     a pipeline with its strides baked in ignores them, and every other one (a library, one linked
 *     from libraries, one that could not be kept) takes them as before.
 *   - vkCmdBindPipeline of a kept pipeline is held; at each draw the variant whose strides are the
 *     bound ones is bound (made on first use, through the layers beneath) when it is not the one
 *     the driver has. Variants live as long as their pipeline.
 * A pipeline that cannot be kept (a module this layer did not see made, a create info that does
 * not encode) is made as the app asked. Pipeline layouts and render passes a kept create info names
 * are destroyed when the last pipeline naming them is.
 *
 * On only where the probe saw the single-binding shape (VKB_PROBE_FLAG_MK_VB_SINGLE), or by
 * VKBRIDGE_ERRATA=+vb_bake.
 *
 * Imitation (host, VKBRIDGE_ERRATA_IMITATE=vb_bake): the G68's shape as seen from a pipeline with
 * several bindings - after a vkCmdBindVertexBuffers2 with pStrides, every binding the command
 * buffer has bound takes that call's last stride.
 */
#define _GNU_SOURCE
#include "vkb_server.h"
#include "server_errata.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define NB 32      /* vertex bindings tracked */
#define NSTAGE 8
#define HB 256     /* hash buckets */

static unsigned hk(uint64_t k)
{
    k ^= k >> 33;
    k *= 0xff51afd7ed558ccdull;
    return (unsigned)(k >> 56) & (HB - 1);
}

typedef struct spv {
    uint32_t refs;
    size_t size;
    uint32_t code[];
} spv;

typedef struct modrec {
    VkShaderModule m;
    spv *code;
    struct modrec *next;
} modrec;

typedef struct objref { /* a layout or render pass a kept create info names */
    uint64_t h;
    int type; /* 0 layout, 1 render pass */
    VkDevice dev;
    uint32_t refs;
    int destroy_pending;
    struct objref *next;
} objref;

typedef struct variant {
    uint32_t key[NB];
    VkPipeline p;
    struct variant *next;
} variant;

typedef struct prec {
    VkPipeline p; /* the app's handle: the variant with the create info's own strides */
    VkDevice dev;
    uint8_t *blob;
    size_t len;
    uint32_t nstage;
    spv *code[NSTAGE]; /* NULL: the stage had no module (inline code is in the blob) */
    VkPipelineLayout layout;
    VkRenderPass rp;
    uint32_t nbind, bind_no[NB];
    uint32_t base_key[NB];
    variant *vars;
    pthread_mutex_t lock;
    struct prec *next;
} prec;

typedef struct cbstate {
    VkCommandBuffer cb;
    prec *bound;           /* a kept pipeline the app bound (graphics), or NULL */
    VkPipeline drv;        /* what the driver has bound (graphics), VK_NULL_HANDLE: unknown */
    VkBuffer buf[NB];
    VkDeviceSize off[NB], size[NB];
    uint32_t stride[NB];
    uint32_t have_buf, have_stride;
    struct cbstate *next;
} cbstate;

typedef struct vkb_errata_vbbake {
    vkb_dispatch next;
    pthread_mutex_t lock;
    modrec *mods[HB];
    prec *pipes[HB];
    objref *objs[HB];
    cbstate *cbs[HB];
    uint64_t made, variants, swaps, unkept;
} vkb_errata_vbbake;

typedef struct vkb_vbbake_imi {
    vkb_dispatch next;
    pthread_mutex_t lock;
    cbstate *cbs[HB];
    uint64_t wrong;
} vkb_vbbake_imi;

/* The last device's, for a call whose own device has none (a server thread without a table: the
 * driver's functions are the same for every device), as server_errata_cmd.c does */
static vkb_errata_vbbake *last_b;
static vkb_vbbake_imi *last_imi;

static vkb_errata_vbbake *B(void)
{
    vkb_errata_dev *e = vkb_errata_cur();
    return e && e->vbbake ? e->vbbake : __atomic_load_n(&last_b, __ATOMIC_ACQUIRE);
}

static vkb_vbbake_imi *I(void)
{
    vkb_errata_dev *e = vkb_errata_cur();
    return e && e->vbbake_imi ? e->vbbake_imi : __atomic_load_n(&last_imi, __ATOMIC_ACQUIRE);
}

/* ---- per command buffer state (lock held) */

static cbstate *cb_get(cbstate **tab, VkCommandBuffer cb, int make)
{
    cbstate **pp = &tab[hk((uint64_t)(uintptr_t)cb)];
    for (cbstate *s = *pp; s; s = s->next)
        if (s->cb == cb) return s;
    if (!make) return NULL;
    cbstate *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->cb = cb;
    s->next = *pp;
    *pp = s;
    return s;
}

static void cb_forget(cbstate **tab, VkCommandBuffer cb)
{
    for (cbstate **pp = &tab[hk((uint64_t)(uintptr_t)cb)]; *pp; pp = &(*pp)->next)
        if ((*pp)->cb == cb) {
            cbstate *s = *pp;
            *pp = s->next;
            free(s);
            return;
        }
}

static void cb_clear(cbstate *s)
{
    VkCommandBuffer cb = s->cb;
    cbstate *next = s->next;
    memset(s, 0, sizeof(*s));
    s->cb = cb;
    s->next = next;
}

/* ---- shader code and object references (lock held) */

static void spv_unref(spv *c)
{
    if (c && !--c->refs) free(c);
}

static spv *mod_code(vkb_errata_vbbake *b, VkShaderModule m)
{
    for (modrec *r = b->mods[hk((uint64_t)(uintptr_t)m)]; r; r = r->next)
        if (r->m == m) return r->code;
    return NULL;
}

static objref *obj_get(vkb_errata_vbbake *b, int type, uint64_t h, VkDevice dev, int make)
{
    objref **pp = &b->objs[hk(h)];
    for (objref *o = *pp; o; o = o->next)
        if (o->h == h && o->type == type) return o;
    if (!make) return NULL;
    objref *o = calloc(1, sizeof(*o));
    if (!o) return NULL;
    o->h = h, o->type = type, o->dev = dev;
    o->next = *pp;
    *pp = o;
    return o;
}

/* Drops a reference; a destroy the app asked for meanwhile is done now */
static void obj_release(vkb_errata_vbbake *b, int type, uint64_t h)
{
    if (!h) return;
    for (objref **pp = &b->objs[hk(h)]; *pp; pp = &(*pp)->next) {
        objref *o = *pp;
        if (o->h != h || o->type != type) continue;
        if (--o->refs) return;
        *pp = o->next;
        if (o->destroy_pending) {
            if (type == 0) b->next.vkDestroyPipelineLayout(o->dev, (VkPipelineLayout)(uintptr_t)h, NULL);
            else b->next.vkDestroyRenderPass(o->dev, (VkRenderPass)(uintptr_t)h, NULL);
        }
        free(o);
        return;
    }
}

static void prec_free(vkb_errata_vbbake *b, prec *r) /* lock held; variants already destroyed */
{
    for (uint32_t i = 0; i < r->nstage; i++) spv_unref(r->code[i]);
    obj_release(b, 0, (uint64_t)(uintptr_t)r->layout);
    obj_release(b, 1, (uint64_t)(uintptr_t)r->rp);
    for (variant *v = r->vars, *nx; v; v = nx) nx = v->next, free(v);
    free(r->blob);
    pthread_mutex_destroy(&r->lock);
    free(r);
}

static prec *pipe_find(vkb_errata_vbbake *b, VkPipeline p)
{
    for (prec *r = b->pipes[hk((uint64_t)(uintptr_t)p)]; r; r = r->next)
        if (r->p == p) return r;
    return NULL;
}

/* ---- the fix */

static int has_dyn(const VkPipelineDynamicStateCreateInfo *d, VkDynamicState s)
{
    for (uint32_t i = 0; d && i < d->dynamicStateCount; i++)
        if (d->pDynamicStates[i] == s) return 1;
    return 0;
}

/* A complete pipeline with the stride dynamic and its own vertex input. Pipeline libraries, and
 * pipelines linked from them, are left as they are: a library's dynamic state is the linked
 * pipeline's (DXVK on a driver with VK_EXT_graphics_pipeline_library, its library bit in
 * VkPipelineCreateFlags2CreateInfoKHR; Mali has none). */
static int wants_bake(const VkGraphicsPipelineCreateInfo *ci)
{
    if (!has_dyn(ci->pDynamicState, VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE)) return 0;
    if (has_dyn(ci->pDynamicState, VK_DYNAMIC_STATE_VERTEX_INPUT_EXT)) return 0;
    if (ci->flags & VK_PIPELINE_CREATE_LIBRARY_BIT_KHR) return 0;
    for (const VkBaseInStructure *n = ci->pNext; n; n = n->pNext) {
        if (n->sType == VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR ||
            n->sType == VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT)
            return 0;
        if (n->sType == VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO_KHR &&
            (((const VkPipelineCreateFlags2CreateInfoKHR *)n)->flags & VK_PIPELINE_CREATE_2_LIBRARY_BIT_KHR))
            return 0;
    }
    const VkPipelineVertexInputStateCreateInfo *vi = ci->pVertexInputState;
    if (!vi || vi->vertexBindingDescriptionCount > NB || ci->stageCount > NSTAGE) return 0;
    for (uint32_t i = 0; i < vi->vertexBindingDescriptionCount; i++)
        if (vi->pVertexBindingDescriptions[i].binding >= NB) return 0;
    return 1;
}

/* The record of ci, with its create info and shader code kept, or NULL when it cannot be kept
 * (lock held). ci is the one the driver gets: the stride taken out of its dynamic list. */
static prec *keep(vkb_errata_vbbake *b, VkDevice dev, const VkGraphicsPipelineCreateInfo *ci)
{
    for (uint32_t i = 0; i < ci->stageCount; i++)
        if (ci->pStages[i].module && !mod_code(b, ci->pStages[i].module)) return NULL; /* made before this layer, or elsewhere */
    prec *r = calloc(1, sizeof(*r));
    if (!r) return NULL;
    VkGraphicsPipelineCreateInfo c = *ci;
    c.flags &= ~(VkPipelineCreateFlags)VK_PIPELINE_CREATE_DERIVATIVE_BIT;
    c.basePipelineHandle = VK_NULL_HANDLE;
    c.basePipelineIndex = -1;
    vkb_enc e;
    vkb_enc_init(&e);
    vkb_enc_VkGraphicsPipelineCreateInfo(&e, &c);
    if (e.oom || !e.buf) {
        vkb_enc_free(&e);
        free(r);
        return NULL;
    }
    r->blob = e.buf, r->len = e.len;
    pthread_mutex_init(&r->lock, NULL);
    r->dev = dev;
    const VkPipelineVertexInputStateCreateInfo *vi = ci->pVertexInputState;
    r->nbind = vi->vertexBindingDescriptionCount;
    for (uint32_t i = 0; i < r->nbind; i++) {
        r->bind_no[i] = vi->pVertexBindingDescriptions[i].binding;
        r->base_key[i] = vi->pVertexBindingDescriptions[i].stride;
    }
    r->nstage = ci->stageCount;
    for (uint32_t i = 0; i < r->nstage; i++)
        if (ci->pStages[i].module && (r->code[i] = mod_code(b, ci->pStages[i].module))) r->code[i]->refs++;
    objref *o;
    if (ci->layout && (o = obj_get(b, 0, (uint64_t)(uintptr_t)ci->layout, dev, 1))) o->refs++, r->layout = ci->layout;
    if (ci->renderPass && (o = obj_get(b, 1, (uint64_t)(uintptr_t)ci->renderPass, dev, 1))) o->refs++, r->rp = ci->renderPass;
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL bk_CreateGraphicsPipelines(VkDevice dev, VkPipelineCache cache, uint32_t n,
                                                                const VkGraphicsPipelineCreateInfo *ci, const VkAllocationCallbacks *a,
                                                                VkPipeline *out)
{
    vkb_errata_vbbake *b = B();
    uint32_t want = 0;
    for (uint32_t i = 0; i < n; i++) want += wants_bake(&ci[i]);
    if (!want) return b->next.vkCreateGraphicsPipelines(dev, cache, n, ci, a, out);
    VkGraphicsPipelineCreateInfo *c = malloc(n * sizeof(*c));
    VkPipelineDynamicStateCreateInfo *ds = malloc(n * sizeof(*ds));
    VkDynamicState **lists = calloc(n, sizeof(*lists));
    prec **recs = calloc(n, sizeof(*recs));
    if (!c || !ds || !lists || !recs) {
        free(c), free(ds), free(lists), free(recs);
        return b->next.vkCreateGraphicsPipelines(dev, cache, n, ci, a, out);
    }
    /* the stride taken out of the dynamic list of each one that is kept; the others as asked */
    pthread_mutex_lock(&b->lock);
    for (uint32_t i = 0; i < n; i++) {
        c[i] = ci[i];
        if (!wants_bake(&ci[i])) continue;
        const VkPipelineDynamicStateCreateInfo *d = ci[i].pDynamicState;
        if (!(lists[i] = malloc(d->dynamicStateCount * sizeof(VkDynamicState)))) continue;
        uint32_t k = 0;
        for (uint32_t j = 0; j < d->dynamicStateCount; j++)
            if (d->pDynamicStates[j] != VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE) lists[i][k++] = d->pDynamicStates[j];
        ds[i] = *d;
        ds[i].dynamicStateCount = k;
        ds[i].pDynamicStates = lists[i];
        c[i].pDynamicState = &ds[i];
        if (!(recs[i] = keep(b, dev, &c[i]))) {
            c[i] = ci[i];
            b->unkept++;
            VKB_ONCE(VKB_LOG_WARN, "errata: vb_bake: a pipeline with dynamic strides could not be kept for variants (a module "
                                   "made elsewhere, or a create info that does not encode); made as asked");
        }
    }
    pthread_mutex_unlock(&b->lock);
    VkResult r = b->next.vkCreateGraphicsPipelines(dev, cache, n, c, a, out);
    pthread_mutex_lock(&b->lock);
    for (uint32_t i = 0; i < n; i++) {
        prec *p = recs[i];
        if (!p) continue;
        if (!out[i]) { /* this one failed */
            prec_free(b, p);
            continue;
        }
        p->p = out[i];
        unsigned h = hk((uint64_t)(uintptr_t)out[i]);
        p->next = b->pipes[h];
        b->pipes[h] = p;
        b->made++;
    }
    pthread_mutex_unlock(&b->lock);
    for (uint32_t i = 0; i < n; i++) free(lists[i]);
    free(lists), free(ds), free(c), free(recs);
    return r;
}

static VKAPI_ATTR void VKAPI_CALL bk_DestroyPipeline(VkDevice dev, VkPipeline p, const VkAllocationCallbacks *a)
{
    vkb_errata_vbbake *b = B();
    prec *r = NULL;
    pthread_mutex_lock(&b->lock);
    for (prec **pp = &b->pipes[hk((uint64_t)(uintptr_t)p)]; p && *pp; pp = &(*pp)->next)
        if ((*pp)->p == p) {
            r = *pp;
            *pp = r->next;
            break;
        }
    pthread_mutex_unlock(&b->lock);
    if (r) {
        for (variant *v = r->vars; v; v = v->next) b->next.vkDestroyPipeline(dev, v->p, a);
        pthread_mutex_lock(&b->lock);
        prec_free(b, r);
        pthread_mutex_unlock(&b->lock);
    }
    b->next.vkDestroyPipeline(dev, p, a);
}

static VKAPI_ATTR VkResult VKAPI_CALL bk_CreateShaderModule(VkDevice dev, const VkShaderModuleCreateInfo *ci, const VkAllocationCallbacks *a,
                                                           VkShaderModule *out)
{
    vkb_errata_vbbake *b = B();
    VkResult r = b->next.vkCreateShaderModule(dev, ci, a, out);
    if (r != VK_SUCCESS || !ci->pCode) return r;
    spv *c = malloc(sizeof(*c) + ci->codeSize);
    modrec *m = calloc(1, sizeof(*m));
    if (!c || !m) {
        free(c), free(m);
        return r;
    }
    c->refs = 1, c->size = ci->codeSize;
    memcpy(c->code, ci->pCode, ci->codeSize);
    m->m = *out, m->code = c;
    pthread_mutex_lock(&b->lock);
    unsigned h = hk((uint64_t)(uintptr_t)*out);
    m->next = b->mods[h];
    b->mods[h] = m;
    pthread_mutex_unlock(&b->lock);
    return r;
}

static VKAPI_ATTR void VKAPI_CALL bk_DestroyShaderModule(VkDevice dev, VkShaderModule m, const VkAllocationCallbacks *a)
{
    vkb_errata_vbbake *b = B();
    pthread_mutex_lock(&b->lock);
    for (modrec **pp = &b->mods[hk((uint64_t)(uintptr_t)m)]; m && *pp; pp = &(*pp)->next)
        if ((*pp)->m == m) {
            modrec *r = *pp;
            *pp = r->next;
            spv_unref(r->code);
            free(r);
            break;
        }
    pthread_mutex_unlock(&b->lock);
    b->next.vkDestroyShaderModule(dev, m, a);
}

static VKAPI_ATTR void VKAPI_CALL bk_DestroyPipelineLayout(VkDevice dev, VkPipelineLayout l, const VkAllocationCallbacks *a)
{
    vkb_errata_vbbake *b = B();
    pthread_mutex_lock(&b->lock);
    objref *o = l ? obj_get(b, 0, (uint64_t)(uintptr_t)l, dev, 0) : NULL;
    if (o) o->destroy_pending = 1;
    pthread_mutex_unlock(&b->lock);
    if (!o) b->next.vkDestroyPipelineLayout(dev, l, a);
}

static VKAPI_ATTR void VKAPI_CALL bk_DestroyRenderPass(VkDevice dev, VkRenderPass rp, const VkAllocationCallbacks *a)
{
    vkb_errata_vbbake *b = B();
    pthread_mutex_lock(&b->lock);
    objref *o = rp ? obj_get(b, 1, (uint64_t)(uintptr_t)rp, dev, 0) : NULL;
    if (o) o->destroy_pending = 1;
    pthread_mutex_unlock(&b->lock);
    if (!o) b->next.vkDestroyRenderPass(dev, rp, a);
}

/* The variant of r with these strides: found, or made from the kept create info */
static VkPipeline variant_for(vkb_errata_vbbake *b, prec *r, const uint32_t *key)
{
    pthread_mutex_lock(&r->lock);
    VkPipeline found = VK_NULL_HANDLE;
    if (!memcmp(key, r->base_key, r->nbind * sizeof(*key))) found = r->p;
    for (variant *v = r->vars; v && !found; v = v->next)
        if (!memcmp(key, v->key, r->nbind * sizeof(*key))) found = v->p;
    if (found) {
        pthread_mutex_unlock(&r->lock);
        return found;
    }
    vkb_arena arena;
    memset(&arena, 0, sizeof(arena));
    vkb_dec d;
    vkb_dec_init(&d, r->blob, r->len, &arena, NULL, 0);
    VkGraphicsPipelineCreateInfo ci;
    memset(&ci, 0, sizeof(ci));
    vkb_dec_VkGraphicsPipelineCreateInfo(&d, &ci);
    VkPipeline p = VK_NULL_HANDLE;
    VkResult res = VK_ERROR_INITIALIZATION_FAILED;
    VkShaderModule mods[NSTAGE] = {0};
    if (!d.err && ci.pVertexInputState && ci.stageCount == r->nstage &&
        ci.pVertexInputState->vertexBindingDescriptionCount == r->nbind) {
        VkPipelineShaderStageCreateInfo *st = (VkPipelineShaderStageCreateInfo *)ci.pStages;
        VkVertexInputBindingDescription *bd = (VkVertexInputBindingDescription *)ci.pVertexInputState->pVertexBindingDescriptions;
        for (uint32_t i = 0; i < r->nbind; i++) bd[i].stride = key[i];
        res = VK_SUCCESS;
        for (uint32_t i = 0; i < r->nstage && res == VK_SUCCESS; i++) {
            if (!r->code[i]) continue;
            VkShaderModuleCreateInfo mi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, r->code[i]->size, r->code[i]->code};
            res = b->next.vkCreateShaderModule(r->dev, &mi, NULL, &mods[i]);
            st[i].module = mods[i];
        }
        if (res == VK_SUCCESS) res = b->next.vkCreateGraphicsPipelines(r->dev, VK_NULL_HANDLE, 1, &ci, NULL, &p);
        for (uint32_t i = 0; i < r->nstage; i++)
            if (mods[i]) b->next.vkDestroyShaderModule(r->dev, mods[i], NULL);
    }
    vkb_arena_free(&arena);
    variant *v = res == VK_SUCCESS && p ? calloc(1, sizeof(*v)) : NULL;
    if (v) {
        memcpy(v->key, key, r->nbind * sizeof(*key));
        v->p = p;
        v->next = r->vars;
        r->vars = v;
        __atomic_add_fetch(&b->variants, 1, __ATOMIC_RELAXED);
        VKB_ONCE(VKB_LOG_INFO, "errata: vb_bake: first pipeline variant with the bound strides baked in - MK-05");
    } else {
        if (p) b->next.vkDestroyPipeline(r->dev, p, NULL);
        p = r->p; /* drawn with the create info's own strides */
        VKB_WARN("errata: vb_bake: a pipeline variant failed (%d, decode %d); drawn with the pipeline's own strides", res, d.err);
    }
    pthread_mutex_unlock(&r->lock);
    return p;
}

/* Before a draw: the variant for the bound strides, when a kept pipeline is bound */
static void prepare(vkb_errata_vbbake *b, VkCommandBuffer cb)
{
    pthread_mutex_lock(&b->lock);
    cbstate *s = cb_get(b->cbs, cb, 0);
    prec *r = s ? s->bound : NULL;
    if (!r) {
        pthread_mutex_unlock(&b->lock);
        return;
    }
    uint32_t key[NB];
    for (uint32_t i = 0; i < r->nbind; i++) {
        uint32_t k = r->bind_no[i];
        key[i] = (s->have_stride >> k & 1) ? s->stride[k] : r->base_key[i];
    }
    VkPipeline drv = s->drv;
    pthread_mutex_unlock(&b->lock);
    VkPipeline p = variant_for(b, r, key);
    if (p == drv) return;
    b->next.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    __atomic_add_fetch(&b->swaps, 1, __ATOMIC_RELAXED);
    pthread_mutex_lock(&b->lock);
    if ((s = cb_get(b->cbs, cb, 0))) s->drv = p;
    pthread_mutex_unlock(&b->lock);
}

static VKAPI_ATTR void VKAPI_CALL bk_CmdBindPipeline(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipeline p)
{
    vkb_errata_vbbake *b = B();
    if (bp != VK_PIPELINE_BIND_POINT_GRAPHICS) {
        b->next.vkCmdBindPipeline(cb, bp, p);
        return;
    }
    pthread_mutex_lock(&b->lock);
    prec *r = pipe_find(b, p);
    cbstate *s = cb_get(b->cbs, cb, 1);
    if (s) s->bound = r;
    if (s && !r) s->drv = p;
    pthread_mutex_unlock(&b->lock);
    if (!r || !s) b->next.vkCmdBindPipeline(cb, bp, p); /* a kept one is bound at the draw, as its variant */
}

static VKAPI_ATTR void VKAPI_CALL bk_CmdBindVertexBuffers2(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkBuffer *bufs,
                                                          const VkDeviceSize *off, const VkDeviceSize *size, const VkDeviceSize *st)
{
    vkb_errata_vbbake *b = B();
    if (st) {
        pthread_mutex_lock(&b->lock);
        cbstate *s = cb_get(b->cbs, cb, 1);
        for (uint32_t j = 0; s && j < n && first + j < NB; j++) s->stride[first + j] = (uint32_t)st[j], s->have_stride |= 1u << (first + j);
        pthread_mutex_unlock(&b->lock);
    }
    b->next.vkCmdBindVertexBuffers2(cb, first, n, bufs, off, size, st);
}

static VKAPI_ATTR VkResult VKAPI_CALL bk_BeginCommandBuffer(VkCommandBuffer cb, const VkCommandBufferBeginInfo *bi)
{
    vkb_errata_vbbake *b = B();
    pthread_mutex_lock(&b->lock);
    cbstate *s = cb_get(b->cbs, cb, 0);
    if (s) cb_clear(s);
    pthread_mutex_unlock(&b->lock);
    return b->next.vkBeginCommandBuffer(cb, bi);
}

static VKAPI_ATTR void VKAPI_CALL bk_CmdExecuteCommands(VkCommandBuffer cb, uint32_t n, const VkCommandBuffer *sec)
{
    vkb_errata_vbbake *b = B();
    b->next.vkCmdExecuteCommands(cb, n, sec);
    pthread_mutex_lock(&b->lock);
    cbstate *s = cb_get(b->cbs, cb, 0);
    if (s) cb_clear(s); /* the primary's state is undefined after it */
    pthread_mutex_unlock(&b->lock);
}

static VKAPI_ATTR void VKAPI_CALL bk_FreeCommandBuffers(VkDevice dev, VkCommandPool pool, uint32_t n, const VkCommandBuffer *cbs)
{
    vkb_errata_vbbake *b = B();
    pthread_mutex_lock(&b->lock);
    for (uint32_t i = 0; i < n; i++)
        if (cbs[i]) cb_forget(b->cbs, cbs[i]);
    pthread_mutex_unlock(&b->lock);
    b->next.vkFreeCommandBuffers(dev, pool, n, cbs);
}

#define DRAW(name, params, args)                      \
    static VKAPI_ATTR void VKAPI_CALL bk_##name params \
    {                                                  \
        vkb_errata_vbbake *b = B();                    \
        prepare(b, cb);                                \
        b->next.name args;                             \
    }

DRAW(vkCmdDraw, (VkCommandBuffer cb, uint32_t a, uint32_t c, uint32_t d, uint32_t e), (cb, a, c, d, e))
DRAW(vkCmdDrawIndexed, (VkCommandBuffer cb, uint32_t a, uint32_t c, uint32_t d, int32_t e, uint32_t f), (cb, a, c, d, e, f))
DRAW(vkCmdDrawIndirect, (VkCommandBuffer cb, VkBuffer a, VkDeviceSize o, uint32_t n, uint32_t s), (cb, a, o, n, s))
DRAW(vkCmdDrawIndexedIndirect, (VkCommandBuffer cb, VkBuffer a, VkDeviceSize o, uint32_t n, uint32_t s), (cb, a, o, n, s))
DRAW(vkCmdDrawIndirectCount, (VkCommandBuffer cb, VkBuffer a, VkDeviceSize o, VkBuffer c, VkDeviceSize co, uint32_t m, uint32_t s),
     (cb, a, o, c, co, m, s))
DRAW(vkCmdDrawIndexedIndirectCount, (VkCommandBuffer cb, VkBuffer a, VkDeviceSize o, VkBuffer c, VkDeviceSize co, uint32_t m, uint32_t s),
     (cb, a, o, c, co, m, s))
DRAW(vkCmdDrawMultiEXT, (VkCommandBuffer cb, uint32_t n, const VkMultiDrawInfoEXT *i, uint32_t ic, uint32_t fi, uint32_t s),
     (cb, n, i, ic, fi, s))
DRAW(vkCmdDrawMultiIndexedEXT,
     (VkCommandBuffer cb, uint32_t n, const VkMultiDrawIndexedInfoEXT *i, uint32_t ic, uint32_t fi, uint32_t s, const int32_t *vo),
     (cb, n, i, ic, fi, s, vo))
DRAW(vkCmdDrawIndirectByteCountEXT, (VkCommandBuffer cb, uint32_t ic, uint32_t fi, VkBuffer cbuf, VkDeviceSize co, uint32_t off, uint32_t st),
     (cb, ic, fi, cbuf, co, off, st))

void vkb_errata_vbbake_install(vkb_srv_table *t, vkb_errata_dev *e)
{
    if (!(e->forced >> VKB_ERRATUM_VB_BAKE & 1) && !vkb_probe_flag(VKB_PROBE_FLAG_MK_VB_SINGLE)) {
        vkb_errata_info_once("errata: vb_bake: the probe saw strides wrong in multi-binding calls only (vb_split fixes that): not installed");
        return;
    }
    vkb_errata_vbbake *b = calloc(1, sizeof(*b));
    if (!b) return;
    pthread_mutex_init(&b->lock, NULL);
    b->next = t->real;
    e->vbbake = b;
    __atomic_store_n(&last_b, b, __ATOMIC_RELEASE);
    ERRATA_HOOK(t, vkCreateGraphicsPipelines, bk_CreateGraphicsPipelines);
    ERRATA_HOOK(t, vkDestroyPipeline, bk_DestroyPipeline);
    ERRATA_HOOK(t, vkCreateShaderModule, bk_CreateShaderModule);
    ERRATA_HOOK(t, vkDestroyShaderModule, bk_DestroyShaderModule);
    ERRATA_HOOK(t, vkDestroyPipelineLayout, bk_DestroyPipelineLayout);
    ERRATA_HOOK(t, vkDestroyRenderPass, bk_DestroyRenderPass);
    ERRATA_HOOK(t, vkCmdBindPipeline, bk_CmdBindPipeline);
    ERRATA_HOOK(t, vkCmdBindVertexBuffers2, bk_CmdBindVertexBuffers2);
    ERRATA_HOOK(t, vkBeginCommandBuffer, bk_BeginCommandBuffer);
    ERRATA_HOOK(t, vkCmdExecuteCommands, bk_CmdExecuteCommands);
    ERRATA_HOOK(t, vkFreeCommandBuffers, bk_FreeCommandBuffers);
    ERRATA_HOOK(t, vkCmdDraw, bk_vkCmdDraw);
    ERRATA_HOOK(t, vkCmdDrawIndexed, bk_vkCmdDrawIndexed);
    ERRATA_HOOK(t, vkCmdDrawIndirect, bk_vkCmdDrawIndirect);
    ERRATA_HOOK(t, vkCmdDrawIndexedIndirect, bk_vkCmdDrawIndexedIndirect);
    ERRATA_HOOK(t, vkCmdDrawIndirectCount, bk_vkCmdDrawIndirectCount);
    ERRATA_HOOK(t, vkCmdDrawIndexedIndirectCount, bk_vkCmdDrawIndexedIndirectCount);
    ERRATA_HOOK(t, vkCmdDrawMultiEXT, bk_vkCmdDrawMultiEXT);
    ERRATA_HOOK(t, vkCmdDrawMultiIndexedEXT, bk_vkCmdDrawMultiIndexedEXT);
    ERRATA_HOOK(t, vkCmdDrawIndirectByteCountEXT, bk_vkCmdDrawIndirectByteCountEXT);
}

/* At device destroy (server_errata.c): the totals, then the tables (the device takes what
 * pipelines and modules are left) */
void vkb_errata_vbbake_destroy(vkb_errata_dev *e)
{
    vkb_errata_vbbake *b = e->vbbake;
    vkb_vbbake_imi *m = e->vbbake_imi;
    if (b && (b->made || b->unkept))
        VKB_INFO("errata: vb_bake: %llu pipeline(s) with their strides baked in, %llu variant(s) made, %llu variant bind(s); "
                 "%llu with dynamic strides made as asked (not kept) - MK-05",
                 (unsigned long long)b->made, (unsigned long long)b->variants, (unsigned long long)b->swaps,
                 (unsigned long long)b->unkept);
    if (m && m->wrong) VKB_INFO("errata: vb_bake: imitated: %llu call(s) whose last stride went to every binding", (unsigned long long)m->wrong);
    if (b) {
        vkb_errata_vbbake *exp = b;
        __atomic_compare_exchange_n(&last_b, &exp, NULL, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
        for (unsigned h = 0; h < HB; h++) {
            for (modrec *r = b->mods[h], *nx; r; r = nx) nx = r->next, spv_unref(r->code), free(r);
            for (prec *r = b->pipes[h], *nx; r; r = nx) {
                nx = r->next;
                for (uint32_t i = 0; i < r->nstage; i++) spv_unref(r->code[i]);
                for (variant *v = r->vars, *vn; v; v = vn) vn = v->next, free(v);
                free(r->blob);
                pthread_mutex_destroy(&r->lock);
                free(r);
            }
            for (objref *o = b->objs[h], *nx; o; o = nx) nx = o->next, free(o);
            for (cbstate *s = b->cbs[h], *nx; s; s = nx) nx = s->next, free(s);
        }
        pthread_mutex_destroy(&b->lock);
        free(b);
    }
    if (m) {
        vkb_vbbake_imi *exp = m;
        __atomic_compare_exchange_n(&last_imi, &exp, NULL, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
        for (unsigned h = 0; h < HB; h++)
            for (cbstate *s = m->cbs[h], *nx; s; s = nx) nx = s->next, free(s);
        pthread_mutex_destroy(&m->lock);
        free(m);
    }
    e->vbbake = NULL, e->vbbake_imi = NULL;
}

/* ---- the imitation (host): every bound binding takes the call's last stride */

static VKAPI_ATTR void VKAPI_CALL im_CmdBindVertexBuffers2(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkBuffer *bufs,
                                                          const VkDeviceSize *off, const VkDeviceSize *size, const VkDeviceSize *st)
{
    vkb_vbbake_imi *m = I();
    pthread_mutex_lock(&m->lock);
    cbstate *s = cb_get(m->cbs, cb, 1);
    for (uint32_t j = 0; s && j < n && first + j < NB; j++) {
        uint32_t k = first + j;
        s->buf[k] = bufs[j], s->off[k] = off[j], s->size[k] = size ? size[j] : VK_WHOLE_SIZE;
        s->have_buf |= 1u << k;
    }
    cbstate c;
    memset(&c, 0, sizeof(c));
    if (s) c = *s;
    pthread_mutex_unlock(&m->lock);
    m->next.vkCmdBindVertexBuffers2(cb, first, n, bufs, off, size, st);
    if (!st || !n || !s) return;
    VkDeviceSize last = st[n - 1];
    for (uint32_t k = 0; k < NB; k++)
        if (c.have_buf >> k & 1) m->next.vkCmdBindVertexBuffers2(cb, k, 1, &c.buf[k], &c.off[k], &c.size[k], &last);
    __atomic_add_fetch(&m->wrong, 1, __ATOMIC_RELAXED);
}

static VKAPI_ATTR void VKAPI_CALL im_CmdBindVertexBuffers(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkBuffer *bufs,
                                                         const VkDeviceSize *off)
{
    vkb_vbbake_imi *m = I();
    pthread_mutex_lock(&m->lock);
    cbstate *s = cb_get(m->cbs, cb, 1);
    for (uint32_t j = 0; s && j < n && first + j < NB; j++) {
        uint32_t k = first + j;
        s->buf[k] = bufs[j], s->off[k] = off[j], s->size[k] = VK_WHOLE_SIZE;
        s->have_buf |= 1u << k;
    }
    pthread_mutex_unlock(&m->lock);
    m->next.vkCmdBindVertexBuffers(cb, first, n, bufs, off);
}

static VKAPI_ATTR VkResult VKAPI_CALL im_BeginCommandBuffer(VkCommandBuffer cb, const VkCommandBufferBeginInfo *bi)
{
    vkb_vbbake_imi *m = I();
    pthread_mutex_lock(&m->lock);
    cbstate *s = cb_get(m->cbs, cb, 0);
    if (s) cb_clear(s);
    pthread_mutex_unlock(&m->lock);
    return m->next.vkBeginCommandBuffer(cb, bi);
}

void vkb_errata_vbbake_imitate(vkb_srv_table *t, vkb_errata_dev *e)
{
    vkb_vbbake_imi *m = calloc(1, sizeof(*m));
    if (!m) return;
    pthread_mutex_init(&m->lock, NULL);
    m->next = t->real;
    e->vbbake_imi = m;
    __atomic_store_n(&last_imi, m, __ATOMIC_RELEASE);
    ERRATA_HOOK(t, vkCmdBindVertexBuffers2, im_CmdBindVertexBuffers2);
    ERRATA_HOOK(t, vkCmdBindVertexBuffers, im_CmdBindVertexBuffers);
    ERRATA_HOOK(t, vkBeginCommandBuffer, im_BeginCommandBuffer);
}
