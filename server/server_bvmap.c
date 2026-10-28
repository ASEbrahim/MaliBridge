/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * Texel buffer views named by the client. DXVK makes a view for each new slice of a discarded
 * buffer, so few creates repeat and the client's recycling (client/bufview.c) rarely hits:
 * Subnautica gameplay (tablet, 2026-10-12) made ~1,000 vkCreateBufferView a second, each a round
 * trip, most of the bridge's remaining ones. The client now names the view itself (VKB_BV_TAG | n,
 * passed in *pView) and sends the creation without waiting; here those names map to the driver's
 * views, translated wherever a view goes back to the driver: descriptor writes, template updates
 * (both through templates tracked here), push descriptors and vkDestroyBufferView. Views the
 * server named (the synchronous path) pass through untouched.
 *
 * A failed creation (out of memory: nothing else can fail for a valid create info) maps the name
 * to VK_NULL_HANDLE, logged; descriptors written with it reach the null-descriptor dummies of
 * server_nulldesc.c, which sits beneath. Installed outermost, so every layer sees driver views.
 */
#define _GNU_SOURCE
#include "vkb_server.h"

#include <stdlib.h>
#include <string.h>

/* As the descriptor set names (server_dsmap.c): 16 tag bits, never those of a driver pointer. */
#define VKB_BV_TAG 0xBBBB000000000000ull
#define VKB_BV_TAG_MASK 0xFFFF000000000000ull
#define BV_BUCKETS 65536
#define BV_TMPL_BUCKETS 256

typedef struct bv_ent {
    uint64_t id;
    VkBufferView view;
    struct bv_ent *next;
} bv_ent;

/* A template's texel buffer entries; templates without any are not kept. */
typedef struct bv_tmpl {
    VkDescriptorUpdateTemplate tmpl;
    uint32_t count;
    struct bv_tmpl *next;
    VkDescriptorUpdateTemplateEntry entries[];
} bv_tmpl;

typedef struct vkb_bvmap {
    pthread_mutex_t lock;
    bv_ent *buckets[BV_BUCKETS];
    bv_tmpl *tmpls[BV_TMPL_BUCKETS];
    uint64_t named, live;
    int warned;
    PFN_vkCreateBufferView create;
    PFN_vkDestroyBufferView destroy;
    PFN_vkUpdateDescriptorSets update;
    PFN_vkUpdateDescriptorSetWithTemplate update_template;
    PFN_vkCreateDescriptorUpdateTemplate create_template;
    PFN_vkDestroyDescriptorUpdateTemplate destroy_template;
    PFN_vkCmdPushDescriptorSet push;
    PFN_vkCmdPushDescriptorSet2 push2;
    PFN_vkCmdPushDescriptorSetWithTemplate push_template;
} vkb_bvmap;

static vkb_bvmap *cur(void)
{
    vkb_srv_table *t = vkb_srv_current_table();
    return t ? t->bvmap : NULL;
}

static int tagged(VkBufferView v)
{
    return ((uint64_t)v & VKB_BV_TAG_MASK) == VKB_BV_TAG;
}

static int texel_type(VkDescriptorType ty)
{
    return ty == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER || ty == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
}

static unsigned bucket(uint64_t id)
{
    id *= 0x9e3779b97f4a7c15ull;
    return (unsigned)(id >> 48) & (BV_BUCKETS - 1);
}

static unsigned tmpl_bucket(VkDescriptorUpdateTemplate t)
{
    uint64_t k = (uint64_t)(uintptr_t)t * 0x9e3779b97f4a7c15ull;
    return (unsigned)(k >> 56) & (BV_TMPL_BUCKETS - 1);
}

static void missing(vkb_bvmap *m, const char *where, uint64_t id)
{
    if (__atomic_fetch_add(&m->warned, 1, __ATOMIC_RELAXED) < 20)
        VKB_ERR("%s: client-named buffer view %llx does not exist (named so far: %llu); written as null", where,
                (unsigned long long)id, (unsigned long long)m->named);
}

static inline void fix_locked(vkb_bvmap *m, const char *where, VkBufferView *p)
{
    if (!tagged(*p)) return;
    uint64_t id = (uint64_t)*p;
    bv_ent *e = m->buckets[bucket(id)];
    while (e && e->id != id) e = e->next;
    *p = e ? e->view : VK_NULL_HANDLE;
    if (!e) missing(m, where, id);
}

/* Nothing to translate until the client named a view (not live ones only: a stale name must not
 * reach the driver). */
static int any_named(vkb_bvmap *m)
{
    return __atomic_load_n(&m->named, __ATOMIC_RELAXED) != 0;
}

/* The request's own arrays are patched: they live in the server's receive buffer/arena. */
static void fix_writes(vkb_bvmap *m, const char *where, uint32_t n, const VkWriteDescriptorSet *w)
{
    int any = 0;
    for (uint32_t i = 0; i < n && !any; i++) {
        if (!texel_type(w[i].descriptorType) || !w[i].pTexelBufferView) continue;
        for (uint32_t j = 0; j < w[i].descriptorCount; j++) {
            if (tagged(w[i].pTexelBufferView[j])) {
                any = 1;
                break;
            }
        }
    }
    if (!any) return;

    pthread_mutex_lock(&m->lock);
    for (uint32_t i = 0; i < n; i++) {
        if (!texel_type(w[i].descriptorType) || !w[i].pTexelBufferView) continue;
        for (uint32_t j = 0; j < w[i].descriptorCount; j++) {
            fix_locked(m, where, (VkBufferView *)&w[i].pTexelBufferView[j]);
        }
    }
    pthread_mutex_unlock(&m->lock);
}

static void fix_template_data(vkb_bvmap *m, const char *where, VkDescriptorUpdateTemplate tmpl, const void *data)
{
    if (!data) return;
    pthread_mutex_lock(&m->lock);
    bv_tmpl *t = m->tmpls[tmpl_bucket(tmpl)];
    while (t && t->tmpl != tmpl) t = t->next;
    if (!t) {
        pthread_mutex_unlock(&m->lock);
        return;
    }
    for (uint32_t i = 0; i < t->count; i++) {
        const VkDescriptorUpdateTemplateEntry *e = &t->entries[i];
        for (uint32_t j = 0; j < e->descriptorCount; j++) {
            fix_locked(m, where, (VkBufferView *)((uint8_t *)data + e->offset + (size_t)j * e->stride));
        }
    }
    pthread_mutex_unlock(&m->lock);
}

/* ------------------------------------------------------------------ wrappers */

static VkResult VKAPI_CALL bv_CreateBufferView(VkDevice device, const VkBufferViewCreateInfo *ci, const VkAllocationCallbacks *a,
                                               VkBufferView *out)
{
    vkb_bvmap *m = cur();
    if (!out || !tagged(*out)) return m->create(device, ci, a, out);
    uint64_t id = (uint64_t)*out;
    VkBufferView v = VK_NULL_HANDLE;
    VkResult r = m->create(device, ci, a, &v);
    if (r != VK_SUCCESS) {
        static int told;
        if (__atomic_fetch_add(&told, 1, __ATOMIC_RELAXED) < 10)
            VKB_ERR("creating buffer view %llx the client named ahead failed: %d; descriptors written with it will be null",
                    (unsigned long long)id, r);
        v = VK_NULL_HANDLE;
    }
    bv_ent *e = malloc(sizeof(*e));
    if (!e) {
        if (v) m->destroy(device, v, a);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    e->id = id;
    e->view = v;
    pthread_mutex_lock(&m->lock);
    unsigned b = bucket(id);
    e->next = m->buckets[b];
    m->buckets[b] = e;
    m->named++;
    m->live++;
    pthread_mutex_unlock(&m->lock);
    return r; /* *out keeps the client's name (the reply, if any, carries it back) */
}

static void VKAPI_CALL bv_DestroyBufferView(VkDevice device, VkBufferView view, const VkAllocationCallbacks *a)
{
    vkb_bvmap *m = cur();
    if (!tagged(view)) {
        m->destroy(device, view, a);
        return;
    }
    uint64_t id = (uint64_t)view;
    VkBufferView v = VK_NULL_HANDLE;
    int found = 0;
    pthread_mutex_lock(&m->lock);
    for (bv_ent **pp = &m->buckets[bucket(id)]; *pp; pp = &(*pp)->next) {
        bv_ent *e = *pp;
        if (e->id != id) continue;
        *pp = e->next;
        v = e->view;
        free(e);
        m->live--;
        found = 1;
        break;
    }
    pthread_mutex_unlock(&m->lock);
    if (!found) missing(m, "vkDestroyBufferView", id);
    if (v) m->destroy(device, v, a);
}

static void VKAPI_CALL bv_UpdateDescriptorSets(VkDevice device, uint32_t nw, const VkWriteDescriptorSet *w, uint32_t nc,
                                               const VkCopyDescriptorSet *cp)
{
    vkb_bvmap *m = cur();
    if (any_named(m)) fix_writes(m, "vkUpdateDescriptorSets", nw, w);
    m->update(device, nw, w, nc, cp);
}

static void VKAPI_CALL bv_UpdateDescriptorSetWithTemplate(VkDevice device, VkDescriptorSet set, VkDescriptorUpdateTemplate tmpl,
                                                          const void *data)
{
    vkb_bvmap *m = cur();
    if (any_named(m)) fix_template_data(m, "vkUpdateDescriptorSetWithTemplate", tmpl, data);
    m->update_template(device, set, tmpl, data);
}

static void VKAPI_CALL bv_CmdPushDescriptorSet(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout layout, uint32_t set,
                                               uint32_t n, const VkWriteDescriptorSet *w)
{
    vkb_bvmap *m = cur();
    if (any_named(m)) fix_writes(m, "vkCmdPushDescriptorSet", n, w);
    m->push(cb, bp, layout, set, n, w);
}

static void VKAPI_CALL bv_CmdPushDescriptorSet2(VkCommandBuffer cb, const VkPushDescriptorSetInfo *i)
{
    vkb_bvmap *m = cur();
    if (any_named(m)) fix_writes(m, "vkCmdPushDescriptorSet2", i->descriptorWriteCount, i->pDescriptorWrites);
    m->push2(cb, i);
}

static void VKAPI_CALL bv_CmdPushDescriptorSetWithTemplate(VkCommandBuffer cb, VkDescriptorUpdateTemplate tmpl, VkPipelineLayout layout,
                                                           uint32_t set, const void *data)
{
    vkb_bvmap *m = cur();
    if (any_named(m)) fix_template_data(m, "vkCmdPushDescriptorSetWithTemplate", tmpl, data);
    m->push_template(cb, tmpl, layout, set, data);
}

static VkResult VKAPI_CALL bv_CreateDescriptorUpdateTemplate(VkDevice device, const VkDescriptorUpdateTemplateCreateInfo *ci,
                                                             const VkAllocationCallbacks *a, VkDescriptorUpdateTemplate *out)
{
    vkb_bvmap *m = cur();
    VkResult r = m->create_template(device, ci, a, out);
    if (r != VK_SUCCESS) return r;
    uint32_t k = 0;
    for (uint32_t i = 0; i < ci->descriptorUpdateEntryCount; i++) k += texel_type(ci->pDescriptorUpdateEntries[i].descriptorType);
    if (!k) return r;
    bv_tmpl *t = malloc(sizeof(*t) + k * sizeof(t->entries[0]));
    if (!t) {
        VKB_ERR("vkCreateDescriptorUpdateTemplate: out of memory keeping its texel buffer entries; client-named views written through it will be wrong");
        return r;
    }
    t->tmpl = *out;
    t->count = 0;
    for (uint32_t i = 0; i < ci->descriptorUpdateEntryCount; i++)
        if (texel_type(ci->pDescriptorUpdateEntries[i].descriptorType)) t->entries[t->count++] = ci->pDescriptorUpdateEntries[i];
    pthread_mutex_lock(&m->lock);
    unsigned b = tmpl_bucket(*out);
    t->next = m->tmpls[b];
    m->tmpls[b] = t;
    pthread_mutex_unlock(&m->lock);
    return r;
}

static void VKAPI_CALL bv_DestroyDescriptorUpdateTemplate(VkDevice device, VkDescriptorUpdateTemplate tmpl, const VkAllocationCallbacks *a)
{
    vkb_bvmap *m = cur();
    if (tmpl) {
        pthread_mutex_lock(&m->lock);
        for (bv_tmpl **pp = &m->tmpls[tmpl_bucket(tmpl)]; *pp; pp = &(*pp)->next)
            if ((*pp)->tmpl == tmpl) {
                bv_tmpl *t = *pp;
                *pp = t->next;
                free(t);
                break;
            }
        pthread_mutex_unlock(&m->lock);
    }
    m->destroy_template(device, tmpl, a);
}

/* ------------------------------------------------------------------ device hooks */

void vkb_bvmap_device_init(vkb_srv_table *t)
{
    vkb_dispatch *dt = &t->dt;
    if (!dt->vkCreateBufferView || !dt->vkDestroyBufferView || !dt->vkUpdateDescriptorSets) return;
    vkb_bvmap *m = calloc(1, sizeof(*m));
    if (!m) return;
    pthread_mutex_init(&m->lock, NULL);
    m->create = dt->vkCreateBufferView;
    m->destroy = dt->vkDestroyBufferView;
    m->update = dt->vkUpdateDescriptorSets;
    m->update_template = dt->vkUpdateDescriptorSetWithTemplate;
    m->create_template = dt->vkCreateDescriptorUpdateTemplate;
    m->destroy_template = dt->vkDestroyDescriptorUpdateTemplate;
    m->push = dt->vkCmdPushDescriptorSet;
    m->push2 = dt->vkCmdPushDescriptorSet2;
    m->push_template = dt->vkCmdPushDescriptorSetWithTemplate;
    dt->vkCreateBufferView = bv_CreateBufferView;
    dt->vkDestroyBufferView = bv_DestroyBufferView;
    dt->vkUpdateDescriptorSets = bv_UpdateDescriptorSets;
    if (m->create_template && m->destroy_template) {
        dt->vkCreateDescriptorUpdateTemplate = bv_CreateDescriptorUpdateTemplate;
        dt->vkDestroyDescriptorUpdateTemplate = bv_DestroyDescriptorUpdateTemplate;
        if (m->update_template) dt->vkUpdateDescriptorSetWithTemplate = bv_UpdateDescriptorSetWithTemplate;
        if (m->push_template) dt->vkCmdPushDescriptorSetWithTemplate = bv_CmdPushDescriptorSetWithTemplate;
    }
    if (m->push) dt->vkCmdPushDescriptorSet = bv_CmdPushDescriptorSet;
    if (m->push2) dt->vkCmdPushDescriptorSet2 = bv_CmdPushDescriptorSet2;
    t->bvmap = m;
}

/* Views the app left behind are destroyed (the client's parked ones are gone by now). */
void vkb_bvmap_device_destroy(vkb_srv_table *t)
{
    vkb_bvmap *m = t->bvmap;
    if (!m) return;
    vkb_dispatch *dt = &t->dt;
    dt->vkCreateBufferView = m->create;
    dt->vkDestroyBufferView = m->destroy;
    dt->vkUpdateDescriptorSets = m->update;
    if (dt->vkCreateDescriptorUpdateTemplate == bv_CreateDescriptorUpdateTemplate) {
        dt->vkCreateDescriptorUpdateTemplate = m->create_template;
        dt->vkDestroyDescriptorUpdateTemplate = m->destroy_template;
    }
    if (dt->vkUpdateDescriptorSetWithTemplate == bv_UpdateDescriptorSetWithTemplate) dt->vkUpdateDescriptorSetWithTemplate = m->update_template;
    if (dt->vkCmdPushDescriptorSetWithTemplate == bv_CmdPushDescriptorSetWithTemplate) dt->vkCmdPushDescriptorSetWithTemplate = m->push_template;
    if (m->push) dt->vkCmdPushDescriptorSet = m->push;
    if (m->push2) dt->vkCmdPushDescriptorSet2 = m->push2;
    uint64_t left = 0;
    for (unsigned b = 0; b < BV_BUCKETS; b++)
        for (bv_ent *e = m->buckets[b], *nx; e; e = nx) {
            nx = e->next;
            if (e->view) m->destroy(t->device, e->view, NULL);
            free(e);
            left++;
        }
    for (unsigned b = 0; b < BV_TMPL_BUCKETS; b++)
        for (bv_tmpl *x = m->tmpls[b], *nx; x; x = nx) {
            nx = x->next;
            free(x);
        }
    if (m->named) VKB_INFO("device %u: %llu buffer views named by the client, %llu left at the end", t->id, (unsigned long long)m->named,
                           (unsigned long long)left);
    pthread_mutex_destroy(&m->lock);
    free(m);
    t->bvmap = NULL;
}
