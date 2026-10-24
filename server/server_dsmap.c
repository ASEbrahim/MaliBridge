/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * Descriptor sets named by the client. Where the client keeps a pool's books it knows an
 * allocation will succeed, so it names the sets itself (VKB_DS_TAG | n) and sends the allocation
 * without waiting (vkbAllocDescSets) - DXVK allocates a set or two per draw, and a round trip
 * each was most of the bridge's remaining cost per frame. Here those names map to the driver's
 * sets: wrappers in the device's dispatch table translate them wherever a set goes back to the
 * driver (updates, template updates, binds, frees), and a pool's reset or destruction forgets its
 * sets. Sets the server named itself (the synchronous path) pass through untouched.
 */
#define _GNU_SOURCE
#include "vkb_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Client names carry 0xDDDD in their top 16 bits. Not the top bit alone: Android tags heap
 * pointers in their top byte (0xb4...), so Mali's own set handles had it set too (tablet run 6). */
#define VKB_DS_TAG 0xDDDD000000000000ull
#define VKB_DS_TAG_MASK 0xFFFF000000000000ull
#define DS_BUCKETS 8192
#define DS_LOCAL 64

typedef struct ds_ent {
    uint64_t id;
    VkDescriptorSet set;
    struct ds_ent *next;      /* bucket chain */
    struct ds_ent *pool_next; /* the pool's sets */
} ds_ent;

typedef struct ds_pool {
    VkDescriptorPool pool;
    ds_ent *sets;
    struct ds_pool *next;
} ds_pool;

/* vkd3d-proton's descriptor heap sets (allocated with a variable descriptor count), for
 * heap_repair(). */
#define HEAP_SETS 64
typedef struct heap_set {
    VkDescriptorSet set;
    VkDescriptorPool pool;
    uint32_t index;        /* the set number it was last bound at, UINT32_MAX before */
} heap_set;

typedef struct vkb_dsmap {
    pthread_mutex_t lock;
    heap_set heap[HEAP_SETS];
    uint32_t nheap, repaired;
    ds_ent *buckets[DS_BUCKETS];
    ds_pool *pools;
    int warned;
    uint64_t min_id, max_id, named, dropped; /* diagnostics: client names seen, and those dropped by resets/frees */
    PFN_vkAllocateDescriptorSets allocate;
    PFN_vkUpdateDescriptorSets update;
    PFN_vkUpdateDescriptorSetWithTemplate update_template;
    PFN_vkCmdBindDescriptorSets bind;
    PFN_vkCmdBindDescriptorSets2 bind2;
    PFN_vkFreeDescriptorSets free_sets;
    PFN_vkResetDescriptorPool reset_pool;
    PFN_vkDestroyDescriptorPool destroy_pool;
} vkb_dsmap;

static unsigned bucket(uint64_t id)
{
    id *= 0x9e3779b97f4a7c15ull;
    return (unsigned)(id >> 51) & (DS_BUCKETS - 1);
}

static vkb_dsmap *cur(void)
{
    vkb_srv_table *t = vkb_srv_current_table();
    return t ? t->dsmap : NULL;
}

static int tagged(VkDescriptorSet s)
{
    return ((uint64_t)s & VKB_DS_TAG_MASK) == VKB_DS_TAG;
}

/* The driver's set for a client name; VK_NULL_HANDLE when its allocation failed. */
static VkDescriptorSet lookup(vkb_dsmap *m, VkDescriptorSet s)
{
    if (!tagged(s)) return s;
    uint64_t id = (uint64_t)s;
    pthread_mutex_lock(&m->lock);
    ds_ent *e = m->buckets[bucket(id)];
    while (e && e->id != id) e = e->next;
    VkDescriptorSet r = e ? e->set : VK_NULL_HANDLE;
    pthread_mutex_unlock(&m->lock);
    return r;
}

static void missing_id(vkb_dsmap *m, const char *where, uint64_t id)
{
    if (__atomic_fetch_add(&m->warned, 1, __ATOMIC_RELAXED) < 20)
        VKB_ERR("%s: client-named descriptor set %llx does not exist (named so far: %llu, ids %llx..%llx, %llu dropped by resets/frees); skipped",
                where, (unsigned long long)id, (unsigned long long)m->named, (unsigned long long)m->min_id, (unsigned long long)m->max_id,
                (unsigned long long)m->dropped);
}
#define missing(m, where) missing_id((m), (where), 0)

static ds_pool *pool_get(vkb_dsmap *m, VkDescriptorPool pool, int create)
{
    for (ds_pool *p = m->pools; p; p = p->next)
        if (p->pool == pool) return p;
    if (!create) return NULL;
    ds_pool *p = calloc(1, sizeof(*p));
    if (!p) return NULL;
    p->pool = pool;
    p->next = m->pools;
    m->pools = p;
    return p;
}

/* Forgets the pool's sets (a reset or destruction freed them); the record goes with destroy. */
static void pool_forget(vkb_dsmap *m, VkDescriptorPool pool, int destroy)
{
    pthread_mutex_lock(&m->lock);
    for (uint32_t i = 0; i < m->nheap;)
        if (m->heap[i].pool == pool) m->heap[i] = m->heap[--m->nheap];
        else i++;
    for (ds_pool **pp = &m->pools; *pp; pp = &(*pp)->next) {
        ds_pool *p = *pp;
        if (p->pool != pool) continue;
        while (p->sets) {
            ds_ent *e = p->sets;
            p->sets = e->pool_next;
            ds_ent **bp = &m->buckets[bucket(e->id)];
            while (*bp && *bp != e) bp = &(*bp)->next;
            if (*bp) *bp = e->next;
            m->dropped++;
            free(e);
        }
        if (destroy) {
            *pp = p->next;
            free(p);
        }
        break;
    }
    pthread_mutex_unlock(&m->lock);
}

/* ------------------------------------------------------------------ the allocation request */

/* vkbAllocDescSets: device, pool, count, then per set its layout and the client's name. */
void vkb_sv_vkbAllocDescSets(vkb_srv_call *c)
{
    VkDevice dev = (VkDevice)(uintptr_t)vkb_dec_u64(&c->d);
    VkDescriptorPool pool;
    vkb_dec_bytes(&c->d, &pool, sizeof(pool));
    uint32_t n = vkb_dec_u32(&c->d);
    if (c->d.err || !c->table || !c->table->dsmap || n == 0 || n > 4096) {
        vkb_srv_bad_message(c);
        return;
    }
    vkb_dsmap *m = c->table->dsmap;
    VkDescriptorSetLayout layouts_local[DS_LOCAL], *layouts = n <= DS_LOCAL ? layouts_local : malloc(n * sizeof(*layouts));
    uint64_t ids_local[DS_LOCAL], *ids = n <= DS_LOCAL ? ids_local : malloc(n * sizeof(*ids));
    VkDescriptorSet sets_local[DS_LOCAL], *sets = n <= DS_LOCAL ? sets_local : malloc(n * sizeof(*sets));
    if (!layouts || !ids || !sets) goto out;
    for (uint32_t i = 0; i < n; i++) {
        vkb_dec_bytes(&c->d, &layouts[i], sizeof(layouts[i]));
        ids[i] = vkb_dec_u64(&c->d);
    }
    if (c->d.err) {
        vkb_srv_bad_message(c);
        goto out;
    }
    VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, pool, n, layouts};
    VkResult r = m->allocate(dev, &ai, sets);
    if (r != VK_SUCCESS) {
        VKB_ERR("allocating %u descriptor sets the client named ahead failed: %d (the client's pool books disagree "
                "with the driver)", n, r);
        goto out;
    }
    pthread_mutex_lock(&m->lock);
    ds_pool *p = pool_get(m, pool, 1);
    for (uint32_t i = 0; i < n; i++) {
        ds_ent *e = malloc(sizeof(*e));
        if (!e || !p) {
            free(e);
            break;
        }
        e->id = ids[i];
        e->set = sets[i];
        m->named++;
        if (!m->min_id || e->id < m->min_id) m->min_id = e->id;
        if (e->id > m->max_id) m->max_id = e->id;
        unsigned b = bucket(e->id);
        e->next = m->buckets[b];
        m->buckets[b] = e;
        e->pool_next = p->sets;
        p->sets = e;
    }
    pthread_mutex_unlock(&m->lock);
out:
    if (layouts != layouts_local) free(layouts);
    if (ids != ids_local) free(ids);
    if (sets != sets_local) free(sets);
}

/* ------------------------------------------------------------------ the packed update */

/*
 * vkbUpdateDescSets: vkUpdateDescriptorSets packed by the client (client/descriptor.c has the
 * layout): device, write count, then per write a u32 (type | UDS_SET | UDS_RANGE | binding << 8),
 * the set when it changes, element and count when not 0 and 1, and per descriptor only what its
 * type reads. Unpacked into the arena and passed down the device's chain like the generic call
 * (set names, buffer view names and the emulations all apply).
 */
#define UDS_SET 0x10u
#define UDS_RANGE 0x20u

void vkb_sv_vkbUpdateDescSets(vkb_srv_call *c)
{
    vkb_dec *d = &c->d;
    VkDevice dev = (VkDevice)(uintptr_t)vkb_dec_u64(d);
    uint32_t n = vkb_dec_u32(d);
    /* Each write takes at least 12 bytes: a count past that is a bad message, not an allocation. */
    if (d->err || !c->table || n == 0 || n > (d->len - d->pos) / 12) {
        vkb_srv_bad_message(c);
        return;
    }
    VkWriteDescriptorSet *w = vkb_dec_alloc(d, n * sizeof(*w));
    VkDescriptorSet set = VK_NULL_HANDLE;
    for (uint32_t i = 0; w && i < n && !d->err; i++) {
        uint32_t hd = vkb_dec_u32(d);
        VkDescriptorType type = (VkDescriptorType)(hd & 0xf);
        if (hd & UDS_SET) set = (VkDescriptorSet)(uintptr_t)vkb_dec_u64(d);
        uint32_t first = 0, count = 1;
        if (hd & UDS_RANGE) {
            first = vkb_dec_u32(d);
            count = vkb_dec_u32(d);
        }
        if (d->err || !set || count == 0 || count > (d->len - d->pos) / 8 || type > VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT) {
            d->err = 1;
            break;
        }
        w[i] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, hd >> 8, first, count, type, NULL, NULL, NULL};
        VkDescriptorImageInfo *img = NULL;
        VkDescriptorBufferInfo *buf = NULL;
        VkBufferView *texel = NULL;
        switch (type) {
        case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
        case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
            w[i].pTexelBufferView = texel = vkb_dec_alloc(d, count * sizeof(*texel));
            break;
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
            w[i].pBufferInfo = buf = vkb_dec_alloc(d, count * sizeof(*buf));
            break;
        default:
            w[i].pImageInfo = img = vkb_dec_alloc(d, count * sizeof(*img));
            break;
        }
        if (!texel && !buf && !img) {
            d->err = 1;
            break;
        }
        for (uint32_t j = 0; j < count && !d->err; j++) {
            if (texel) {
                texel[j] = (VkBufferView)(uintptr_t)vkb_dec_u64(d);
            } else if (buf) {
                buf[j].buffer = (VkBuffer)(uintptr_t)vkb_dec_u64(d);
                buf[j].offset = vkb_dec_u64(d);
                buf[j].range = vkb_dec_u64(d);
            } else {
                int has_sampler = type == VK_DESCRIPTOR_TYPE_SAMPLER || type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                img[j].sampler = has_sampler ? (VkSampler)(uintptr_t)vkb_dec_u64(d) : VK_NULL_HANDLE;
                img[j].imageView = VK_NULL_HANDLE;
                img[j].imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                if (type != VK_DESCRIPTOR_TYPE_SAMPLER) {
                    img[j].imageView = (VkImageView)(uintptr_t)vkb_dec_u64(d);
                    img[j].imageLayout = (VkImageLayout)vkb_dec_u32(d);
                }
            }
        }
    }
    if (!w || d->err) {
        vkb_srv_bad_message(c);
        return;
    }
    if (!c->dt->vkUpdateDescriptorSets) {
        vkb_srv_missing(c, "vkUpdateDescriptorSets");
        return;
    }
    c->dt->vkUpdateDescriptorSets(dev, n, w, 0, NULL);
}

/* ------------------------------------------------------------------ wrappers */

static void VKAPI_CALL ds_UpdateDescriptorSets(VkDevice device, uint32_t nw, const VkWriteDescriptorSet *w, uint32_t nc,
                                               const VkCopyDescriptorSet *cp)
{
    vkb_dsmap *m = cur();
    int any = 0;
    for (uint32_t i = 0; i < nw && !any; i++) any = tagged(w[i].dstSet);
    for (uint32_t i = 0; i < nc && !any; i++) any = tagged(cp[i].srcSet) || tagged(cp[i].dstSet);
    if (!any) {
        m->update(device, nw, w, nc, cp);
        return;
    }
    VkWriteDescriptorSet w_local[DS_LOCAL], *w2 = nw <= DS_LOCAL ? w_local : malloc(nw * sizeof(*w2));
    VkCopyDescriptorSet c_local[DS_LOCAL], *c2 = nc <= DS_LOCAL ? c_local : malloc(nc * sizeof(*c2));
    if ((nw && !w2) || (nc && !c2)) goto out;
    uint32_t kw = 0, kc = 0;
    /* DXVK writes one set per call, a write per binding: one lookup for the run. */
    VkDescriptorSet from = VK_NULL_HANDLE, to = VK_NULL_HANDLE;
    for (uint32_t i = 0; i < nw; i++) {
        w2[kw] = w[i];
        if (w[i].dstSet != from || !from) {
            from = w[i].dstSet;
            to = lookup(m, from);
        }
        w2[kw].dstSet = to;
        if (w2[kw].dstSet) kw++;
        else missing_id(m, "vkUpdateDescriptorSets", (uint64_t)w[i].dstSet);
    }
    for (uint32_t i = 0; i < nc; i++) {
        c2[kc] = cp[i];
        c2[kc].srcSet = lookup(m, cp[i].srcSet);
        c2[kc].dstSet = lookup(m, cp[i].dstSet);
        if (c2[kc].srcSet && c2[kc].dstSet) kc++;
        else missing(m, "vkUpdateDescriptorSets (copy)");
    }
    if (kw || kc) m->update(device, kw, w2, kc, c2);
out:
    if (w2 != w_local) free(w2);
    if (c2 != c_local) free(c2);
}

static void VKAPI_CALL ds_UpdateDescriptorSetWithTemplate(VkDevice device, VkDescriptorSet set, VkDescriptorUpdateTemplate tmpl,
                                                          const void *data)
{
    vkb_dsmap *m = cur();
    VkDescriptorSet s = lookup(m, set);
    if (!s) {
        missing_id(m, "vkUpdateDescriptorSetWithTemplate", (uint64_t)set);
        return;
    }
    m->update_template(device, s, tmpl, data);
}

/*
 * vkd3d-proton v2.12-v3.0 keeps its descriptor heap sets (sets.vk_sets[]) in a union with the
 * descriptor buffer state, where buffers.heap_dirty, a bool, shares its bytes with vk_sets[3]; a
 * command list split into a new command buffer sets heap_dirty = true whatever the mode
 * (command.c, d3d12_command_list_*split*), leaving vk_sets[3] as the set's handle with its low
 * byte 0x01. Desktop drivers have VK_EXT_mutable_descriptor_type and vkd3d uses fewer heap sets,
 * so set 3 is never one; without it (Mali) set 3 is the SRV heap, every draw after a split read
 * its textures through that handle and the GPU hung (The Witcher 3, 2026-10-12; fixed upstream
 * by moving the field). Driver handles are aligned pointers: a handle ending in 0x01 whose other
 * bytes match the heap set last bound at the same set number is that set.
 */
static VkDescriptorSet heap_repair(vkb_dsmap *m, VkDescriptorSet s, uint32_t index)
{
    if (!s || !__atomic_load_n(&m->nheap, __ATOMIC_RELAXED) || tagged(s)) return s;
    VkDescriptorSet found = VK_NULL_HANDLE;
    int matches = 0;
    pthread_mutex_lock(&m->lock);
    if (((uint64_t)s & 0xff) != 0x01) {
        /* Sound: note the set number a heap set goes to (heaps sets sit 128 bytes apart on Mali, two
         * share their upper bytes; the number tells them apart). */
        for (uint32_t i = 0; i < m->nheap; i++)
            if (m->heap[i].set == s) m->heap[i].index = index;
        pthread_mutex_unlock(&m->lock);
        return s;
    }
    for (uint32_t i = 0; i < m->nheap; i++)
        if (((uint64_t)m->heap[i].set & ~0xffull) == ((uint64_t)s & ~0xffull) && m->heap[i].index == index) {
            found = m->heap[i].set;
            matches++;
        }
    pthread_mutex_unlock(&m->lock);
    if (matches != 1) return s;
    if (__atomic_fetch_add(&m->repaired, 1, __ATOMIC_RELAXED) == 0)
        VKB_INFO("vkCmdBindDescriptorSets: set %u bound as %llx, vkd3d-proton's heap set %llx with heap_dirty written over its low byte; "
                 "bound as the heap set (logged once)",
                 index, (unsigned long long)s, (unsigned long long)found);
    return found;
}

static VkResult VKAPI_CALL ds_AllocateDescriptorSets(VkDevice device, const VkDescriptorSetAllocateInfo *ai, VkDescriptorSet *out)
{
    vkb_dsmap *m = cur();
    VkResult r = m->allocate(device, ai, out);
    if (r != VK_SUCCESS) return r;
    int variable = 0;
    for (const VkBaseInStructure *p = ai->pNext; p; p = p->pNext)
        variable |= p->sType == VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO;
    if (!variable) return r;
    pthread_mutex_lock(&m->lock);
    for (uint32_t i = 0; i < ai->descriptorSetCount; i++) {
        if (m->nheap == HEAP_SETS) memmove(m->heap, m->heap + 1, --m->nheap * sizeof(*m->heap)); /* oldest goes */
        m->heap[m->nheap++] = (heap_set){out[i], ai->descriptorPool, UINT32_MAX};
    }
    pthread_mutex_unlock(&m->lock);
    return r;
}

/* Translates n sets into out; 0 when one of them does not exist. */
static int translate(vkb_dsmap *m, uint32_t n, const VkDescriptorSet *in, VkDescriptorSet *out)
{
    for (uint32_t i = 0; i < n; i++) {
        /* VK_NULL_HANDLE is allowed in binds (graphicsPipelineLibrary) and stays so. */
        out[i] = in[i] ? lookup(m, in[i]) : VK_NULL_HANDLE;
        if (in[i] && !out[i]) return 0;
    }
    return 1;
}

/* VK_NULL_HANDLE in a bind is valid only with graphicsPipelineLibrary, which the bridge does not
 * offer on Mali, and Mali reads through it (a null read at 0x18 binding Hades' Vulkan renderer,
 * The Forge, 2026-10-11 - suspected). Without dynamic offsets the bind is split around the holes;
 * with them the offsets cannot be apportioned here, and the bind goes as it is. Logged once. */
static void bind_runs(vkb_dsmap *m, VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout layout, uint32_t first, uint32_t n,
                      const VkDescriptorSet *s, uint32_t ndyn, const uint32_t *dyn)
{
    int holes = 0;
    for (uint32_t i = 0; i < n; i++) holes |= !s[i];
    if (!holes || ndyn) {
        if (holes) {
            static int told_dyn;
            if (!__atomic_fetch_add(&told_dyn, 1, __ATOMIC_RELAXED))
                VKB_WARN("vkCmdBindDescriptorSets with null sets and dynamic offsets: passed as it is (logged once)");
        }
        m->bind(cb, bp, layout, first, n, s, ndyn, dyn);
        return;
    }
    static int told;
    if (!__atomic_fetch_add(&told, 1, __ATOMIC_RELAXED)) VKB_WARN("vkCmdBindDescriptorSets with null sets: bound around them (logged once)");
    for (uint32_t i = 0; i < n;) {
        if (!s[i]) {
            i++;
            continue;
        }
        uint32_t j = i;
        while (j < n && s[j]) j++;
        m->bind(cb, bp, layout, first + i, j - i, s + i, 0, NULL);
        i = j;
    }
}

static void VKAPI_CALL ds_CmdBindDescriptorSets(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout layout, uint32_t first,
                                                uint32_t n, const VkDescriptorSet *sets, uint32_t ndyn, const uint32_t *dyn)
{
    vkb_dsmap *m = cur();
    VkDescriptorSet local[DS_LOCAL], *s = n <= DS_LOCAL ? local : malloc(n * sizeof(*s));
    if (!s) return;
    if (translate(m, n, sets, s)) {
        for (uint32_t i = 0; i < n; i++) s[i] = heap_repair(m, s[i], first + i);
        bind_runs(m, cb, bp, layout, first, n, s, ndyn, dyn);
    }
    else missing(m, "vkCmdBindDescriptorSets");
    if (s != local) free(s);
}

static void VKAPI_CALL ds_CmdBindDescriptorSets2(VkCommandBuffer cb, const VkBindDescriptorSetsInfo *info)
{
    vkb_dsmap *m = cur();
    uint32_t n = info->descriptorSetCount;
    VkDescriptorSet local[DS_LOCAL], *s = n <= DS_LOCAL ? local : malloc(n * sizeof(*s));
    if (!s) return;
    VkBindDescriptorSetsInfo i2 = *info;
    i2.pDescriptorSets = s;
    if (translate(m, n, info->pDescriptorSets, s)) {
        for (uint32_t k = 0; k < n; k++) s[k] = heap_repair(m, s[k], info->firstSet + k);
        m->bind2(cb, &i2);
    }
    else missing(m, "vkCmdBindDescriptorSets2");
    if (s != local) free(s);
}

static VkResult VKAPI_CALL ds_FreeDescriptorSets(VkDevice device, VkDescriptorPool pool, uint32_t n, const VkDescriptorSet *sets)
{
    vkb_dsmap *m = cur();
    VkDescriptorSet local[DS_LOCAL], *s = n <= DS_LOCAL ? local : malloc(n * sizeof(*s));
    if (!s) return VK_ERROR_OUT_OF_HOST_MEMORY;
    /* Named sets come only from pools without FREE_DESCRIPTOR_SET (the client keeps no books on
     * those), so this is an app error at worst: what does not exist is left out. */
    uint32_t k = 0;
    for (uint32_t i = 0; i < n; i++) {
        VkDescriptorSet t = sets[i] ? lookup(m, sets[i]) : VK_NULL_HANDLE;
        if (t || !sets[i]) s[k++] = t;
    }
    if (m->nheap) {
        pthread_mutex_lock(&m->lock);
        for (uint32_t i = 0; i < k; i++)
            for (uint32_t h = 0; h < m->nheap; h++)
                if (m->heap[h].set == s[i]) {
                    m->heap[h] = m->heap[--m->nheap];
                    break;
                }
        pthread_mutex_unlock(&m->lock);
    }
    VkResult r = k ? m->free_sets(device, pool, k, s) : VK_SUCCESS;
    if (s != local) free(s);
    return r;
}

static VkResult VKAPI_CALL ds_ResetDescriptorPool(VkDevice device, VkDescriptorPool pool, VkDescriptorPoolResetFlags flags)
{
    vkb_dsmap *m = cur();
    pool_forget(m, pool, 0);
    return m->reset_pool(device, pool, flags);
}

static void VKAPI_CALL ds_DestroyDescriptorPool(VkDevice device, VkDescriptorPool pool, const VkAllocationCallbacks *a)
{
    vkb_dsmap *m = cur();
    if (pool) pool_forget(m, pool, 1);
    m->destroy_pool(device, pool, a);
}

/* ------------------------------------------------------------------ device hooks */

void vkb_dsmap_device_init(vkb_srv_table *t)
{
    vkb_dispatch *dt = &t->dt;
    if (!dt->vkAllocateDescriptorSets || !dt->vkUpdateDescriptorSets || !dt->vkCmdBindDescriptorSets ||
        !dt->vkResetDescriptorPool || !dt->vkDestroyDescriptorPool || !dt->vkFreeDescriptorSets)
        return;
    vkb_dsmap *m = calloc(1, sizeof(*m));
    if (!m) return;
    pthread_mutex_init(&m->lock, NULL);
    m->allocate = dt->vkAllocateDescriptorSets;
    m->update = dt->vkUpdateDescriptorSets;
    m->update_template = dt->vkUpdateDescriptorSetWithTemplate;
    m->bind = dt->vkCmdBindDescriptorSets;
    m->bind2 = dt->vkCmdBindDescriptorSets2;
    m->free_sets = dt->vkFreeDescriptorSets;
    m->reset_pool = dt->vkResetDescriptorPool;
    m->destroy_pool = dt->vkDestroyDescriptorPool;
    dt->vkAllocateDescriptorSets = ds_AllocateDescriptorSets;
    dt->vkUpdateDescriptorSets = ds_UpdateDescriptorSets;
    if (m->update_template) dt->vkUpdateDescriptorSetWithTemplate = ds_UpdateDescriptorSetWithTemplate;
    dt->vkCmdBindDescriptorSets = ds_CmdBindDescriptorSets;
    if (m->bind2) dt->vkCmdBindDescriptorSets2 = ds_CmdBindDescriptorSets2;
    dt->vkFreeDescriptorSets = ds_FreeDescriptorSets;
    dt->vkResetDescriptorPool = ds_ResetDescriptorPool;
    dt->vkDestroyDescriptorPool = ds_DestroyDescriptorPool;
    t->dsmap = m;
}

void vkb_dsmap_device_destroy(vkb_srv_table *t)
{
    vkb_dsmap *m = t->dsmap;
    if (!m) return;
    vkb_dispatch *dt = &t->dt;
    dt->vkAllocateDescriptorSets = m->allocate;
    dt->vkUpdateDescriptorSets = m->update;
    if (m->update_template) dt->vkUpdateDescriptorSetWithTemplate = m->update_template;
    dt->vkCmdBindDescriptorSets = m->bind;
    if (m->bind2) dt->vkCmdBindDescriptorSets2 = m->bind2;
    dt->vkFreeDescriptorSets = m->free_sets;
    dt->vkResetDescriptorPool = m->reset_pool;
    dt->vkDestroyDescriptorPool = m->destroy_pool;
    while (m->pools) pool_forget(m, m->pools->pool, 1);
    pthread_mutex_destroy(&m->lock);
    free(m);
    t->dsmap = NULL;
}
