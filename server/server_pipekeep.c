/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * Kept graphics pipeline create infos (server_pipekeep.h), for the layers that make variants of a
 * pipeline after its vkCreateGraphicsPipelines has returned: vb_bake (server_errata_vbbake.c, the
 * vertex strides baked in) and the vertex input emulation (server_vidyn.c, the vertex input of
 * vkCmdSetVertexInputEXT made static).
 *
 * A kept create info is encoded with the wire's own encoder, without derivative state, and keeps a
 * counted reference to each stage's SPIR-V: modules can be gone by the time a variant is made
 * (DXVK destroys them right after the pipeline), so the store keeps a copy of the code of every
 * module made through its layer while the module lives, and a kept create info holds on to it
 * after. A stage given inline (VkShaderModuleCreateInfo chained to it) is in the encoding itself.
 * The pipeline layout and render pass a kept create info names are kept too: an app's destroy of
 * one is held until the last kept create info naming it is released.
 *
 * A variant is decoded from the encoding, its stages given fresh modules made from the kept code,
 * edited by the caller and made through the layer's next with no pipeline cache (the persistent
 * cache beneath takes pipelines made without one); the modules are destroyed right after.
 */
#define _GNU_SOURCE
#include "server_pipekeep.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define HB 256  /* hash buckets */
#define NSTAGE 8

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

struct vkb_pk_ci {
    VkDevice dev;
    uint8_t *blob;
    size_t len;
    uint32_t nstage;
    spv *code[NSTAGE]; /* NULL: the stage had no module (inline code is in the blob) */
    VkPipelineLayout layout;
    VkRenderPass rp;
};

struct vkb_pk {
    const vkb_dispatch *next;
    pthread_mutex_t lock;
    modrec *mods[HB];
    objref *objs[HB];
};

vkb_pk *vkb_pk_create(const vkb_dispatch *next)
{
    vkb_pk *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->next = next;
    pthread_mutex_init(&s->lock, NULL);
    return s;
}

static void spv_unref(spv *c)
{
    if (c && !--c->refs) free(c);
}

void vkb_pk_destroy(vkb_pk *s)
{
    if (!s) return;
    for (unsigned h = 0; h < HB; h++) {
        for (modrec *r = s->mods[h], *nx; r; r = nx) nx = r->next, spv_unref(r->code), free(r);
        for (objref *o = s->objs[h], *nx; o; o = nx) nx = o->next, free(o);
    }
    pthread_mutex_destroy(&s->lock);
    free(s);
}

/* ---- shader code (lock held) */

static spv *mod_code(vkb_pk *s, VkShaderModule m)
{
    for (modrec *r = s->mods[hk((uint64_t)(uintptr_t)m)]; r; r = r->next)
        if (r->m == m) return r->code;
    return NULL;
}

void vkb_pk_module_created(vkb_pk *s, VkShaderModule m, const VkShaderModuleCreateInfo *ci)
{
    if (!s || !m || !ci->pCode) return;
    spv *c = malloc(sizeof(*c) + ci->codeSize);
    modrec *r = calloc(1, sizeof(*r));
    if (!c || !r) {
        free(c), free(r);
        return;
    }
    c->refs = 1, c->size = ci->codeSize;
    memcpy(c->code, ci->pCode, ci->codeSize);
    r->m = m, r->code = c;
    pthread_mutex_lock(&s->lock);
    unsigned h = hk((uint64_t)(uintptr_t)m);
    r->next = s->mods[h];
    s->mods[h] = r;
    pthread_mutex_unlock(&s->lock);
}

void vkb_pk_module_destroyed(vkb_pk *s, VkShaderModule m)
{
    if (!s || !m) return;
    pthread_mutex_lock(&s->lock);
    for (modrec **pp = &s->mods[hk((uint64_t)(uintptr_t)m)]; *pp; pp = &(*pp)->next)
        if ((*pp)->m == m) {
            modrec *r = *pp;
            *pp = r->next;
            spv_unref(r->code);
            free(r);
            break;
        }
    pthread_mutex_unlock(&s->lock);
}

/* ---- object references (lock held) */

static objref *obj_get(vkb_pk *s, int type, uint64_t h, VkDevice dev, int make)
{
    objref **pp = &s->objs[hk(h)];
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
static void obj_release(vkb_pk *s, int type, uint64_t h)
{
    if (!h) return;
    for (objref **pp = &s->objs[hk(h)]; *pp; pp = &(*pp)->next) {
        objref *o = *pp;
        if (o->h != h || o->type != type) continue;
        if (--o->refs) return;
        *pp = o->next;
        if (o->destroy_pending) {
            if (type == 0) s->next->vkDestroyPipelineLayout(o->dev, (VkPipelineLayout)(uintptr_t)h, NULL);
            else s->next->vkDestroyRenderPass(o->dev, (VkRenderPass)(uintptr_t)h, NULL);
        }
        free(o);
        return;
    }
}

static int obj_destroy(vkb_pk *s, int type, uint64_t h)
{
    if (!s || !h) return 0;
    pthread_mutex_lock(&s->lock);
    objref *o = obj_get(s, type, h, VK_NULL_HANDLE, 0);
    if (o) o->destroy_pending = 1;
    pthread_mutex_unlock(&s->lock);
    return o != NULL;
}

int vkb_pk_layout_destroy(vkb_pk *s, VkPipelineLayout l) { return obj_destroy(s, 0, (uint64_t)(uintptr_t)l); }

int vkb_pk_render_pass_destroy(vkb_pk *s, VkRenderPass rp) { return obj_destroy(s, 1, (uint64_t)(uintptr_t)rp); }

/* ---- kept create infos */

vkb_pk_ci *vkb_pk_keep(vkb_pk *s, VkDevice dev, const VkGraphicsPipelineCreateInfo *ci)
{
    if (!s || ci->stageCount > NSTAGE) return NULL;
    vkb_pk_ci *k = calloc(1, sizeof(*k));
    if (!k) return NULL;
    VkGraphicsPipelineCreateInfo c = *ci;
    c.flags &= ~(VkPipelineCreateFlags)VK_PIPELINE_CREATE_DERIVATIVE_BIT;
    c.basePipelineHandle = VK_NULL_HANDLE;
    c.basePipelineIndex = -1;
    vkb_enc e;
    vkb_enc_init(&e);
    vkb_enc_VkGraphicsPipelineCreateInfo(&e, &c);
    if (e.oom || !e.buf) {
        vkb_enc_free(&e);
        free(k);
        return NULL;
    }
    pthread_mutex_lock(&s->lock);
    for (uint32_t i = 0; i < ci->stageCount; i++)
        if (ci->pStages[i].module && !mod_code(s, ci->pStages[i].module)) { /* made before this layer, or elsewhere */
            pthread_mutex_unlock(&s->lock);
            vkb_enc_free(&e);
            free(k);
            return NULL;
        }
    k->blob = e.buf, k->len = e.len;
    k->dev = dev;
    k->nstage = ci->stageCount;
    for (uint32_t i = 0; i < k->nstage; i++)
        if (ci->pStages[i].module && (k->code[i] = mod_code(s, ci->pStages[i].module))) k->code[i]->refs++;
    objref *o;
    if (ci->layout && (o = obj_get(s, 0, (uint64_t)(uintptr_t)ci->layout, dev, 1))) o->refs++, k->layout = ci->layout;
    if (ci->renderPass && (o = obj_get(s, 1, (uint64_t)(uintptr_t)ci->renderPass, dev, 1))) o->refs++, k->rp = ci->renderPass;
    pthread_mutex_unlock(&s->lock);
    return k;
}

void vkb_pk_release(vkb_pk *s, vkb_pk_ci *k)
{
    if (!k) return;
    pthread_mutex_lock(&s->lock);
    for (uint32_t i = 0; i < k->nstage; i++) spv_unref(k->code[i]);
    obj_release(s, 0, (uint64_t)(uintptr_t)k->layout);
    obj_release(s, 1, (uint64_t)(uintptr_t)k->rp);
    pthread_mutex_unlock(&s->lock);
    free(k->blob);
    free(k);
}

size_t vkb_pk_size(const vkb_pk_ci *k)
{
    size_t n = k ? k->len : 0;
    for (uint32_t i = 0; k && i < k->nstage; i++)
        if (k->code[i]) n += k->code[i]->size;
    return n;
}

VkResult vkb_pk_make(vkb_pk *s, const vkb_pk_ci *k, vkb_pk_edit edit, void *ctx, VkPipeline *out)
{
    *out = VK_NULL_HANDLE;
    vkb_arena arena;
    memset(&arena, 0, sizeof(arena));
    vkb_dec d;
    vkb_dec_init(&d, k->blob, k->len, &arena, NULL, 0);
    VkGraphicsPipelineCreateInfo ci;
    memset(&ci, 0, sizeof(ci));
    vkb_dec_VkGraphicsPipelineCreateInfo(&d, &ci);
    VkResult res = VK_ERROR_INITIALIZATION_FAILED;
    VkShaderModule mods[NSTAGE] = {0};
    if (!d.err && ci.stageCount == k->nstage) {
        VkPipelineShaderStageCreateInfo *st = (VkPipelineShaderStageCreateInfo *)ci.pStages;
        res = VK_SUCCESS;
        for (uint32_t i = 0; i < k->nstage && res == VK_SUCCESS; i++) {
            if (!k->code[i]) continue;
            VkShaderModuleCreateInfo mi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, k->code[i]->size, k->code[i]->code};
            res = s->next->vkCreateShaderModule(k->dev, &mi, NULL, &mods[i]);
            st[i].module = mods[i];
        }
        if (res == VK_SUCCESS && edit) res = edit(&ci, &arena, ctx);
        if (res == VK_SUCCESS) res = s->next->vkCreateGraphicsPipelines(k->dev, VK_NULL_HANDLE, 1, &ci, NULL, out);
        for (uint32_t i = 0; i < k->nstage; i++)
            if (mods[i]) s->next->vkDestroyShaderModule(k->dev, mods[i], NULL);
    }
    vkb_arena_free(&arena);
    if (res != VK_SUCCESS && *out) {
        s->next->vkDestroyPipeline(k->dev, *out, NULL);
        *out = VK_NULL_HANDLE;
    }
    if (res == VK_SUCCESS && !*out) res = VK_ERROR_INITIALIZATION_FAILED;
    return res;
}
