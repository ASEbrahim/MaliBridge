/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * What an app has bound in a command buffer, for emulations that record commands of their own
 * into it (the BCn decoder's dispatches) and must leave the app's state as it was: the compute
 * pipeline, compute descriptor sets, push constants and push descriptors. Bind calls are kept as
 * they were made (a later call that covers an earlier one replaces it) and replayed in order. Also
 * what those emulations keep alive for a command buffer until it is begun again, reset or freed.
 */
#define _GNU_SOURCE
#include "server_emu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct bind_sets {
    VkPipelineLayout layout;
    uint32_t first, count, ndyn;
    VkDescriptorSet sets[32];
    uint32_t dyn[64];
} bind_sets;

typedef struct push_consts {
    VkPipelineLayout layout;
    VkShaderStageFlags stages;
    uint32_t offset, size;
    uint8_t data[256];
} push_consts;

typedef struct push_tmpl {
    VkDescriptorUpdateTemplate tmpl;
    VkPipelineLayout layout;
    uint32_t set;
    void *data;
    size_t size;
} push_tmpl;

typedef struct cb_state {
    VkCommandBuffer cb;
    VkCommandPool pool;
    VkPipeline compute_pipeline;
    bind_sets *sets;
    uint32_t nsets, capsets;
    push_consts *pcs;
    uint32_t npcs, cappcs;
    push_tmpl *pts;
    uint32_t npts, cappts;
    void *gfx;            /* graphics-side state (divisor emulation) */
    void *bcn;            /* the BC decoder's scratch buffers (server_emu_bcn.c) */
    void *poly;           /* polygon-mode emulation (server_emu_polygon.c) */
    void *dynrend;        /* the framebuffers of emulated renderings (server_emu_dynrend.c) */
    uint32_t begin_flags; /* VkCommandBufferUsageFlags of the last begin */
    int secondary;        /* allocated at VK_COMMAND_BUFFER_LEVEL_SECONDARY */
    /* Descriptor sets emulations record with (no push descriptors), alive until the next begin. */
    VkDescriptorPool *scratch;
    uint32_t nscratch, cur_scratch, used_scratch;
    struct cb_state *next;
} cb_state;

#define CB_BUCKETS 512
#define SCRATCH_SETS 64
typedef struct cmdstate {
    pthread_mutex_t lock;
    cb_state *b[CB_BUCKETS];
} cmdstate;

/* Packed size of the template data being pushed (set by the raw push handler, server_misc.c). */
__thread size_t vkb_emu_push_size;

static cmdstate *g_states_for(vkb_emu_device *e) { return (cmdstate *)e->cmdstate; }

static unsigned ch(VkCommandBuffer cb)
{
    uint64_t k = (uint64_t)(uintptr_t)cb;
    k ^= k >> 30;
    k *= 0xbf58476d1ce4e5b9ull;
    return (unsigned)(k >> 55) & (CB_BUCKETS - 1);
}

static cb_state *get(vkb_emu_device *e, VkCommandBuffer cb)
{
    cmdstate *s = g_states_for(e);
    pthread_mutex_lock(&s->lock);
    cb_state *c = s->b[ch(cb)];
    while (c && c->cb != cb) c = c->next;
    if (!c) {
        c = calloc(1, sizeof(*c));
        c->cb = cb;
        c->next = s->b[ch(cb)];
        s->b[ch(cb)] = c;
    }
    pthread_mutex_unlock(&s->lock);
    return c;
}

/* A descriptor set of layout `dsl` for a command the emulation records into cb (the BC decoder's,
 * emulated push descriptors); it lives until cb is begun again, when the app has seen the work
 * finish. VK_NULL_HANDLE when even a fresh pool cannot hold it. */
VkDescriptorSet vkb_emu_cb_scratch_set(vkb_emu_device *e, VkCommandBuffer cb, VkDescriptorSetLayout dsl)
{
    cb_state *c = get(e, cb);
    const vkb_dispatch *r = &e->dev->real;
    int fresh = 0;
    for (;;) {
        if (c->cur_scratch >= c->nscratch) {
            const uint32_t per = SCRATCH_SETS * 8;
            VkDescriptorPoolSize sizes[] = {
                {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, per}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, per},
                {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, per}, {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, per},
                {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, per}, {VK_DESCRIPTOR_TYPE_SAMPLER, per},
                {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, per}, {VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, per},
            };
            VkDescriptorPoolCreateInfo pci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, SCRATCH_SETS,
                                              (uint32_t)(sizeof(sizes) / sizeof(sizes[0])), sizes};
            VkDescriptorPool pool;
            VkDescriptorPool *np = realloc(c->scratch, (c->nscratch + 1) * sizeof(*np));
            if (!np) return VK_NULL_HANDLE;
            c->scratch = np;
            if (r->vkCreateDescriptorPool(e->dev->device, &pci, NULL, &pool) != VK_SUCCESS) return VK_NULL_HANDLE;
            c->scratch[c->nscratch++] = pool;
            fresh = 1;
        }
        if (c->used_scratch < SCRATCH_SETS) {
            VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, c->scratch[c->cur_scratch], 1, &dsl};
            VkDescriptorSet set;
            if (r->vkAllocateDescriptorSets(e->dev->device, &ai, &set) == VK_SUCCESS) {
                c->used_scratch++;
                return set;
            }
            if (fresh && c->used_scratch == 0) return VK_NULL_HANDLE; /* larger than a whole pool */
        }
        fresh = 0;
        c->cur_scratch++;
        c->used_scratch = 0;
    }
}

void *vkb_emu_cb_gfx(vkb_emu_device *e, VkCommandBuffer cb, size_t size)
{
    cb_state *c = get(e, cb);
    if (!c->gfx) c->gfx = calloc(1, size);
    return c->gfx;
}

void *vkb_emu_cb_bcn(vkb_emu_device *e, VkCommandBuffer cb, size_t size)
{
    cb_state *c = get(e, cb);
    if (!c->bcn) c->bcn = calloc(1, size);
    return c->bcn;
}

/* The polygon emulation's record of cb, made when `size` is not 0 and there is none. */
void *vkb_emu_cb_poly(vkb_emu_device *e, VkCommandBuffer cb, size_t size)
{
    cb_state *c = get(e, cb);
    if (!c->poly && size) c->poly = calloc(1, size);
    return c->poly;
}

/* The dynamic rendering emulation's framebuffer list of cb, made when there is none. */
void *vkb_emu_cb_dynrend(vkb_emu_device *e, VkCommandBuffer cb, size_t size)
{
    cb_state *c = get(e, cb);
    if (!c->dynrend) c->dynrend = calloc(1, size);
    return c->dynrend;
}

/* What the app made cb with and began it with: its pool (VK_NULL_HANDLE when unseen), its level. */
VkCommandPool vkb_emu_cb_origin(vkb_emu_device *e, VkCommandBuffer cb, int *secondary, uint32_t *begin_flags)
{
    cb_state *c = get(e, cb);
    *secondary = c->secondary;
    *begin_flags = c->begin_flags;
    return c->pool;
}

static void reset(vkb_emu_device *e, cb_state *c)
{
    for (uint32_t i = 0; i < c->nscratch && i <= c->cur_scratch; i++)
        e->dev->real.vkResetDescriptorPool(e->dev->device, c->scratch[i], 0);
    c->cur_scratch = 0;
    c->used_scratch = 0;
    c->compute_pipeline = VK_NULL_HANDLE;
    c->nsets = 0;
    c->npcs = 0;
    for (uint32_t i = 0; i < c->npts; i++) free(c->pts[i].data);
    c->npts = 0;
    vkb_emu_gfx_reset(c->gfx);
    vkb_emu_bcn_cb_release(e, c->bcn);
    vkb_emu_polygon_cb_reset(e, c->cb, c->pool, &c->poly);
    vkb_emu_dynrend_cb_reset(e, c->dynrend);
}

static void destroy(vkb_emu_device *e, cb_state *c)
{
    reset(e, c);
    for (uint32_t j = 0; j < c->nscratch; j++) e->dev->real.vkDestroyDescriptorPool(e->dev->device, c->scratch[j], NULL);
    free(c->scratch);
    free(c->sets);
    free(c->pcs);
    free(c->pts);
    vkb_emu_gfx_free(c->gfx);
    free(c->bcn);
    vkb_emu_polygon_cb_free(e, c->poly);
    vkb_emu_dynrend_cb_free(e, c->dynrend);
    free(c);
}

/* Forgets command buffers the app freed: one handle, or (cb NULL) every one of a pool. */
static void forget(vkb_emu_device *e, VkCommandPool pool, VkCommandBuffer cb)
{
    cmdstate *s = g_states_for(e);
    for (unsigned i = cb ? ch(cb) : 0; i < (cb ? ch(cb) + 1 : CB_BUCKETS); i++) {
        pthread_mutex_lock(&s->lock);
        cb_state **pp = &s->b[i], *gone = NULL;
        while (*pp) {
            cb_state *c = *pp;
            if (cb ? c->cb == cb : c->pool == pool) {
                *pp = c->next;
                c->next = gone;
                gone = c;
            } else {
                pp = &c->next;
            }
        }
        pthread_mutex_unlock(&s->lock);
        while (gone) {
            cb_state *c = gone;
            gone = c->next;
            destroy(e, c);
        }
    }
}

/* ------------------------------------------------------------------ wrappers */

static VKAPI_ATTR VkResult VKAPI_CALL emu_BeginCommandBuffer(VkCommandBuffer cb, const VkCommandBufferBeginInfo *info)
{
    /* What the first begins carry (the tablet's driver crashed inside one; device run 5). */
    static int logged;
    if (__atomic_fetch_add(&logged, 1, __ATOMIC_RELAXED) < 24 && info) {
        char chain[160] = "";
        size_t len = 0;
        for (const VkBaseInStructure *b = info->pNext; b && len < sizeof(chain) - 16; b = b->pNext)
            len += (size_t)snprintf(chain + len, sizeof(chain) - len, " %d", (int)b->sType);
        const VkCommandBufferInheritanceInfo *ii = info->pInheritanceInfo;
        VKB_INFO("begin cb %p: flags 0x%x, inheritance %p%s%s", (void *)cb, info->flags, (const void *)ii, len ? ", chain" : "", chain);
        if (ii) {
            len = 0;
            chain[0] = 0;
            for (const VkBaseInStructure *b = ii->pNext; b && len < sizeof(chain) - 16; b = b->pNext)
                len += (size_t)snprintf(chain + len, sizeof(chain) - len, " %d", (int)b->sType);
            VKB_INFO("  inheritance: sType %d render pass %llx subpass %u framebuffer %llx%s%s", ii->sType,
                     (unsigned long long)ii->renderPass, ii->subpass, (unsigned long long)ii->framebuffer, len ? ", chain" : "", chain);
        }
    }
    cb_state *c = get(vkb_emu_cur(), cb);
    reset(vkb_emu_cur(), c);
    c->begin_flags = info ? info->flags : 0;
    return vkb_emu_real()->vkBeginCommandBuffer(cb, info);
}

static VKAPI_ATTR VkResult VKAPI_CALL emu_AllocateCommandBuffers(VkDevice device, const VkCommandBufferAllocateInfo *info,
                                                                VkCommandBuffer *cbs)
{
    VkResult r = vkb_emu_real()->vkAllocateCommandBuffers(device, info, cbs);
    if (r == VK_SUCCESS) {
        vkb_emu_device *e = vkb_emu_cur();
        for (uint32_t i = 0; i < info->commandBufferCount; i++) {
            forget(e, VK_NULL_HANDLE, cbs[i]); /* a handle the driver reused */
            cb_state *c = get(e, cbs[i]);
            c->pool = info->commandPool;
            c->secondary = info->level == VK_COMMAND_BUFFER_LEVEL_SECONDARY;
        }
    }
    return r;
}

/* A reset, like a begin, means the app has seen the buffer's work finish (no command buffer of a
 * pool being reset may be pending), so what emulations keep for it goes back now. Until these were
 * hooked a command buffer that was reset and then left unbegun kept its decode scratch: DXVK resets
 * a command list's pool and begins only the buffers the next use needs. */
static VKAPI_ATTR VkResult VKAPI_CALL emu_ResetCommandBuffer(VkCommandBuffer cb, VkCommandBufferResetFlags flags)
{
    vkb_emu_device *e = vkb_emu_cur();
    reset(e, get(e, cb));
    return vkb_emu_real()->vkResetCommandBuffer(cb, flags);
}

static VKAPI_ATTR VkResult VKAPI_CALL emu_ResetCommandPool(VkDevice device, VkCommandPool pool, VkCommandPoolResetFlags flags)
{
    vkb_emu_device *e = vkb_emu_cur();
    cmdstate *s = g_states_for(e);
    for (unsigned i = 0; i < CB_BUCKETS; i++) {
        pthread_mutex_lock(&s->lock);
        for (cb_state *c = s->b[i]; c; c = c->next)
            if (c->pool == pool) reset(e, c);
        pthread_mutex_unlock(&s->lock);
    }
    return vkb_emu_real()->vkResetCommandPool(device, pool, flags);
}

static VKAPI_ATTR void VKAPI_CALL emu_FreeCommandBuffers(VkDevice device, VkCommandPool pool, uint32_t n, const VkCommandBuffer *cbs)
{
    vkb_emu_device *e = vkb_emu_cur();
    for (uint32_t i = 0; i < n; i++)
        if (cbs[i]) forget(e, pool, cbs[i]);
    vkb_emu_real()->vkFreeCommandBuffers(device, pool, n, cbs);
}

static VKAPI_ATTR void VKAPI_CALL emu_DestroyCommandPool(VkDevice device, VkCommandPool pool, const VkAllocationCallbacks *pAllocator)
{
    if (pool) forget(vkb_emu_cur(), pool, VK_NULL_HANDLE);
    vkb_emu_real()->vkDestroyCommandPool(device, pool, pAllocator);
}

static VKAPI_ATTR void VKAPI_CALL emu_CmdBindPipeline(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipeline p)
{
    vkb_emu_device *e = vkb_emu_cur();
    if (bp == VK_PIPELINE_BIND_POINT_COMPUTE) get(e, cb)->compute_pipeline = p;
    if (bp == VK_PIPELINE_BIND_POINT_GRAPHICS) vkb_emu_gfx_bind_pipeline(e, cb, p);
    vkb_emu_real()->vkCmdBindPipeline(cb, bp, p);
}

static VKAPI_ATTR void VKAPI_CALL emu_CmdBindDescriptorSets(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout layout,
                                                           uint32_t first, uint32_t count, const VkDescriptorSet *sets,
                                                           uint32_t ndyn, const uint32_t *dyn)
{
    if (bp == VK_PIPELINE_BIND_POINT_COMPUTE && count <= 32 && ndyn <= 64) {
        cb_state *c = get(vkb_emu_cur(), cb);
        /* Drop earlier calls this one covers completely. */
        uint32_t k = 0;
        for (uint32_t i = 0; i < c->nsets; i++) {
            bind_sets *b = &c->sets[i];
            if (b->first >= first && b->first + b->count <= first + count) continue;
            c->sets[k++] = *b;
        }
        c->nsets = k;
        if (c->nsets == c->capsets) {
            c->capsets = c->capsets ? c->capsets * 2 : 8;
            c->sets = realloc(c->sets, c->capsets * sizeof(*c->sets));
        }
        bind_sets *b = &c->sets[c->nsets++];
        b->layout = layout;
        b->first = first;
        b->count = count;
        b->ndyn = ndyn;
        memcpy(b->sets, sets, count * sizeof(*sets));
        if (ndyn) memcpy(b->dyn, dyn, ndyn * sizeof(*dyn));
    }
    vkb_emu_real()->vkCmdBindDescriptorSets(cb, bp, layout, first, count, sets, ndyn, dyn);
}

static VKAPI_ATTR void VKAPI_CALL emu_CmdPushConstants(VkCommandBuffer cb, VkPipelineLayout layout, VkShaderStageFlags stages,
                                                      uint32_t offset, uint32_t size, const void *data)
{
    if ((stages & VK_SHADER_STAGE_COMPUTE_BIT) && offset + size <= 256) {
        cb_state *c = get(vkb_emu_cur(), cb);
        uint32_t k = 0;
        for (uint32_t i = 0; i < c->npcs; i++) {
            push_consts *p = &c->pcs[i];
            if (p->offset >= offset && p->offset + p->size <= offset + size && p->stages == stages) continue;
            c->pcs[k++] = *p;
        }
        c->npcs = k;
        if (c->npcs == c->cappcs) {
            c->cappcs = c->cappcs ? c->cappcs * 2 : 8;
            c->pcs = realloc(c->pcs, c->cappcs * sizeof(*c->pcs));
        }
        push_consts *p = &c->pcs[c->npcs++];
        p->layout = layout;
        p->stages = stages;
        p->offset = offset;
        p->size = size;
        memcpy(p->data + offset, data, size);
    }
    vkb_emu_real()->vkCmdPushConstants(cb, layout, stages, offset, size, data);
}

static VKAPI_ATTR void VKAPI_CALL emu_CmdPushDescriptorSetWithTemplate(VkCommandBuffer cb, VkDescriptorUpdateTemplate tmpl,
                                                                      VkPipelineLayout layout, uint32_t set, const void *data)
{
    /* Only the template's bind point decides; recording it for both costs nothing when replayed
     * into the same bind point it was made for. Sizes are unknown here: the bridge's raw template
     * path hands over the packed size (vkb_emu_push_size). */
    vkb_emu_device *e = vkb_emu_cur();
    cb_state *c = get(e, cb);
    size_t size = vkb_emu_push_size;
    if (size && size < (1u << 20)) {
        if (c->npts == c->cappts) {
            c->cappts = c->cappts ? c->cappts * 2 : 4;
            c->pts = realloc(c->pts, c->cappts * sizeof(*c->pts));
        }
        push_tmpl *p = &c->pts[c->npts++];
        p->tmpl = tmpl;
        p->layout = layout;
        p->set = set;
        p->size = size;
        p->data = malloc(size);
        memcpy(p->data, data, size);
    }
    vkb_emu_real()->vkCmdPushDescriptorSetWithTemplate(cb, tmpl, layout, set, data);
}

/* Puts back what the app had bound for compute after an emulation recorded its own dispatch. */
void vkb_emu_restore_compute(vkb_emu_device *e, VkCommandBuffer cb)
{
    cb_state *c = get(e, cb);
    const vkb_dispatch *r = vkb_emu_real();
    if (c->compute_pipeline) r->vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, c->compute_pipeline);
    for (uint32_t i = 0; i < c->nsets; i++) {
        bind_sets *b = &c->sets[i];
        r->vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, b->layout, b->first, b->count, b->sets, b->ndyn, b->dyn);
    }
    for (uint32_t i = 0; i < c->npts; i++) {
        push_tmpl *p = &c->pts[i];
        r->vkCmdPushDescriptorSetWithTemplate(cb, p->tmpl, p->layout, p->set, p->data);
    }
    for (uint32_t i = 0; i < c->npcs; i++) {
        push_consts *p = &c->pcs[i];
        r->vkCmdPushConstants(cb, p->layout, p->stages, p->offset, p->size, p->data + p->offset);
    }
}

void vkb_emu_cmdstate_install(vkb_srv_table *dev)
{
    vkb_emu_device *e = dev->emu;
    cmdstate *s = calloc(1, sizeof(*s));
    pthread_mutex_init(&s->lock, NULL);
    e->cmdstate = s;
    dev->dt.vkBeginCommandBuffer = emu_BeginCommandBuffer;
    dev->dt.vkAllocateCommandBuffers = emu_AllocateCommandBuffers;
    dev->dt.vkFreeCommandBuffers = emu_FreeCommandBuffers;
    dev->dt.vkDestroyCommandPool = emu_DestroyCommandPool;
    if (dev->real.vkResetCommandBuffer) dev->dt.vkResetCommandBuffer = emu_ResetCommandBuffer;
    if (dev->real.vkResetCommandPool) dev->dt.vkResetCommandPool = emu_ResetCommandPool;
    dev->dt.vkCmdBindPipeline = emu_CmdBindPipeline;
    dev->dt.vkCmdBindDescriptorSets = emu_CmdBindDescriptorSets;
    dev->dt.vkCmdPushConstants = emu_CmdPushConstants;
    if (dev->real.vkCmdPushDescriptorSetWithTemplate) dev->dt.vkCmdPushDescriptorSetWithTemplate = emu_CmdPushDescriptorSetWithTemplate;
}

void vkb_emu_cmdstate_uninstall(vkb_srv_table *dev)
{
    cmdstate *s = dev->emu->cmdstate;
    if (!s) return;
    for (int i = 0; i < CB_BUCKETS; i++) {
        while (s->b[i]) {
            cb_state *c = s->b[i];
            s->b[i] = c->next;
            destroy(dev->emu, c);
        }
    }
    pthread_mutex_destroy(&s->lock);
    free(s);
    dev->emu->cmdstate = NULL;
}
