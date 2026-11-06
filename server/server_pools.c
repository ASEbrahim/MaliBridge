/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * Elastic descriptor pools. Desktop drivers commonly allocate past a pool's maxSets and sizes, and
 * games developed on them rely on it; Mali (r44) refuses with VK_ERROR_OUT_OF_POOL_MEMORY, which
 * the game never expects (Hades' Vulkan renderer: 8192 sets a frame from a pool made for 8192, the
 * rest null - a black screen with the game running). When the driver refuses, the allocation is
 * retried in an overflow pool made like the app's; the app keeps its one handle. Resets and
 * destruction take the overflow pools along, and freed sets go back to the pool that holds them.
 * Installed beneath the set-name map (server_dsmap.c), so named and plain allocations both grow.
 *
 * Overflow pools are kept through resets for the next frame, so an app pool that once grew stays
 * that big in the driver (DXVK's pools on the XRing O3 grew to 8 times their size,
 * in the same game whose server the system closed for lack of memory, #18). While memory is low
 * (vkb_mem_low), a reset frees the overflow pools nothing was allocated from since the last one.
 *
 * A set bigger than the pool (more of a type than a pool like the app's has) fits in no overflow
 * pool either: when an overflow pool that is empty refuses it, no more are made for it, and a
 * set of that layout asked for again with as many variable descriptors or more is refused after
 * the app's pool alone (until a layout is destroyed: handles come back). The size is part of
 * it: a layout with a variable descriptor count (bindless heaps: vkd3d-proton, native engines)
 * refused at 8000 descriptors still gets overflow pools at 16, as without the memo. Before, each such request made overflow pools up to the cap of
 * 64: Detroit: Become Human (a player log, G925) got 384 for six pools of 45056 sets, 8 MB each in
 * Mali's heap, 3 GB at the game's start.
 */
#define _GNU_SOURCE
#include "vkb_server.h"

#include <stdlib.h>
#include <string.h>

#define SET_BUCKETS 4096

typedef struct ep_pool {
    VkDescriptorPool app;            /* the handle the app knows */
    VkDescriptorPoolCreateInfo ci;   /* how to make another like it (pNext dropped) */
    VkDescriptorPoolSize *sizes;
    VkDescriptorPool *extra;         /* overflow pools, in order */
    uint32_t nextra, cur;            /* allocations try extra[cur..] after the app's pool */
    uint32_t used;                   /* extra[0..used) allocated from since the last reset */
    int can_free;
    /* Layouts an empty overflow pool refused (a set of one bigger than the pool), while no layout
     * was destroyed since (a destroyed layout's handle can come back as another layout): asked
     * again, they are refused after the app's pool alone, as the driver would refuse them. */
    VkDescriptorSetLayout refused[4];
    uint32_t refused_count[4];       /* ... with this many variable descriptors (0: none asked); as many or more refused */
    uint32_t nrefused, refused_next;
    uint64_t refused_gen;
    struct ep_pool *next;
} ep_pool;

typedef struct ep_set {              /* sets living in an overflow pool of a FREE_DESCRIPTOR_SET pool */
    VkDescriptorSet set;
    VkDescriptorPool real;
    struct ep_set *next;
} ep_set;

typedef struct vkb_pools {
    pthread_mutex_t lock;
    ep_pool *pools[SET_BUCKETS];     /* by the app's handle (DXVK makes thousands, resets them often) */
    ep_set *sets[SET_BUCKETS];
    PFN_vkCreateDescriptorPool create;
    PFN_vkDestroyDescriptorPool destroy;
    PFN_vkResetDescriptorPool reset;
    PFN_vkAllocateDescriptorSets allocate;
    PFN_vkFreeDescriptorSets free_sets;
    PFN_vkDestroyDescriptorSetLayout destroy_layout;
    uint64_t layout_gen;             /* layouts destroyed (atomic) */
    unsigned long grown;
    unsigned long oversize;          /* sets refused by an empty overflow pool */
} vkb_pools;

static vkb_pools *states[4096];
static uint64_t g_extra_live, g_extra_trimmed; /* overflow pools alive, freed at low-memory resets (atomic) */

uint64_t vkb_pools_overflow_live(void) { return __atomic_load_n(&g_extra_live, __ATOMIC_RELAXED); }

static vkb_pools *st(void)
{
    vkb_srv_table *t = vkb_srv_current_table();
    return t && t->id < 4096 ? states[t->id] : NULL;
}

static unsigned sb(VkDescriptorSet s)
{
    uint64_t k = (uint64_t)(uintptr_t)s;
    k ^= k >> 29;
    k *= 0xbf58476d1ce4e5b9ull;
    return (unsigned)(k >> 52) & (SET_BUCKETS - 1);
}

static unsigned pb(VkDescriptorPool p)
{
    return sb((VkDescriptorSet)(uintptr_t)p);
}

static ep_pool *find(vkb_pools *s, VkDescriptorPool pool)
{
    for (ep_pool *p = s->pools[pb(pool)]; p; p = p->next)
        if (p->app == pool) return p;
    return NULL;
}

static VKAPI_ATTR VkResult VKAPI_CALL ep_CreateDescriptorPool(VkDevice device, const VkDescriptorPoolCreateInfo *ci,
                                                              const VkAllocationCallbacks *a, VkDescriptorPool *out)
{
    vkb_pools *s = st();
    VkResult r = s->create(device, ci, a, out);
    if (r != VK_SUCCESS) return r;
    ep_pool *p = calloc(1, sizeof(*p));
    if (!p) return r;
    p->app = *out;
    p->ci = *ci;
    p->ci.pNext = NULL; /* inline uniform block / mutable type infos: the overflow pool goes without */
    p->sizes = malloc((ci->poolSizeCount ? ci->poolSizeCount : 1) * sizeof(*p->sizes));
    if (!p->sizes) {
        free(p);
        return r;
    }
    if (ci->poolSizeCount) memcpy(p->sizes, ci->pPoolSizes, ci->poolSizeCount * sizeof(*p->sizes));
    p->ci.pPoolSizes = p->sizes;
    p->can_free = (ci->flags & VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT) != 0;
    /* Overflow pools only for plain pools: one with a pNext (inline uniform blocks, mutable
     * descriptors) could not be copied faithfully. */
    if (ci->pNext) p->ci.maxSets = 0;
    pthread_mutex_lock(&s->lock);
    p->next = s->pools[pb(p->app)];
    s->pools[pb(p->app)] = p;
    pthread_mutex_unlock(&s->lock);
    return r;
}

static void forget_sets_of(vkb_pools *s, VkDescriptorPool real)
{
    for (int i = 0; i < SET_BUCKETS; i++)
        for (ep_set **pp = &s->sets[i]; *pp;) {
            if ((*pp)->real == real) {
                ep_set *e = *pp;
                *pp = e->next;
                free(e);
            } else {
                pp = &(*pp)->next;
            }
        }
}

static VKAPI_ATTR void VKAPI_CALL ep_DestroyDescriptorPool(VkDevice device, VkDescriptorPool pool, const VkAllocationCallbacks *a)
{
    vkb_pools *s = st();
    pthread_mutex_lock(&s->lock);
    for (ep_pool **pp = &s->pools[pb(pool)]; *pp; pp = &(*pp)->next)
        if ((*pp)->app == pool) {
            ep_pool *p = *pp;
            *pp = p->next;
            for (uint32_t i = 0; i < p->nextra; i++) {
                if (p->can_free) forget_sets_of(s, p->extra[i]);
                s->destroy(device, p->extra[i], NULL);
            }
            __atomic_sub_fetch(&g_extra_live, p->nextra, __ATOMIC_RELAXED);
            free(p->extra);
            free(p->sizes);
            free(p);
            break;
        }
    pthread_mutex_unlock(&s->lock);
    s->destroy(device, pool, a);
}

static VKAPI_ATTR VkResult VKAPI_CALL ep_ResetDescriptorPool(VkDevice device, VkDescriptorPool pool, VkDescriptorPoolResetFlags flags)
{
    vkb_pools *s = st();
    pthread_mutex_lock(&s->lock);
    ep_pool *p = find(s, pool);
    if (p) {
        for (uint32_t i = 0; i < p->nextra; i++) {
            if (p->can_free) forget_sets_of(s, p->extra[i]);
            s->reset(device, p->extra[i], 0);
        }
        /* Memory low: the ones idle since the last reset go (a reset pool holds no set the GPU
         * still uses, so neither do they); made again if the app needs them. */
        uint32_t drop = vkb_mem_low() && p->nextra > p->used ? p->nextra - p->used : 0;
        for (uint32_t i = p->used; drop && i < p->nextra; i++) s->destroy(device, p->extra[i], NULL);
        if (drop) {
            p->nextra = p->used;
            __atomic_sub_fetch(&g_extra_live, drop, __ATOMIC_RELAXED);
            uint64_t n = __atomic_add_fetch(&g_extra_trimmed, drop, __ATOMIC_RELAXED), was = n - drop;
            if ((was ^ n) > was) /* the first time, then each time it passes a power of two */
                VKB_INFO("descriptor pools: memory low, %llu idle overflow pools freed at resets so far (made again when needed)",
                         (unsigned long long)n);
        }
        p->cur = 0;
        p->used = 0;
    }
    pthread_mutex_unlock(&s->lock);
    return s->reset(device, pool, flags);
}

/* The variable descriptor count asked for set i (0 without VkDescriptorSetVariableDescriptorCountAllocateInfo) */
static uint32_t var_count(const VkDescriptorSetAllocateInfo *ai, uint32_t i)
{
    for (const VkBaseInStructure *b = ai->pNext; b; b = b->pNext)
        if (b->sType == VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO) {
            const VkDescriptorSetVariableDescriptorCountAllocateInfo *v = (const void *)b;
            return v->pDescriptorCounts && i < v->descriptorSetCount ? v->pDescriptorCounts[i] : 0;
        }
    return 0;
}

static VKAPI_ATTR VkResult VKAPI_CALL ep_AllocateDescriptorSets(VkDevice device, const VkDescriptorSetAllocateInfo *ai, VkDescriptorSet *out)
{
    vkb_pools *s = st();
    VkResult r = s->allocate(device, ai, out);
    if (r != VK_ERROR_OUT_OF_POOL_MEMORY && r != VK_ERROR_FRAGMENTED_POOL) return r;
    pthread_mutex_lock(&s->lock);
    ep_pool *p = find(s, ai->descriptorPool);
    if (!p || !p->ci.maxSets) {
        pthread_mutex_unlock(&s->lock);
        return r;
    }
    uint64_t gen = __atomic_load_n(&s->layout_gen, __ATOMIC_RELAXED);
    if (p->nrefused && p->refused_gen == gen)
        for (uint32_t i = 0; i < ai->descriptorSetCount; i++)
            for (uint32_t k = 0; k < p->nrefused; k++)
                if (ai->pSetLayouts[i] == p->refused[k] && var_count(ai, i) >= p->refused_count[k]) {
                    s->oversize++;
                    pthread_mutex_unlock(&s->lock);
                    return r;
                }
    VkDescriptorSetAllocateInfo a2 = *ai;
    for (;;) {
        if (p->cur >= p->nextra) {
            if (p->nextra >= 64) break; /* 64 times what the app asked for: something else is wrong */
            VkDescriptorPool np;
            VkDescriptorPool *grown = realloc(p->extra, (p->nextra + 1) * sizeof(*grown));
            if (!grown || s->create(device, &p->ci, NULL, &np) != VK_SUCCESS) {
                if (grown) p->extra = grown;
                break;
            }
            p->extra = grown;
            p->extra[p->nextra++] = np;
            __atomic_add_fetch(&g_extra_live, 1, __ATOMIC_RELAXED);
            if (!s->grown++)
                VKB_INFO("descriptor pools: an allocation past what a pool was made for goes to an overflow pool (maxSets %u)", p->ci.maxSets);
        }
        a2.descriptorPool = p->extra[p->cur];
        r = s->allocate(device, &a2, out);
        if (r == VK_SUCCESS) {
            if (p->cur + 1 > p->used) p->used = p->cur + 1;
            if (p->can_free)
                for (uint32_t i = 0; i < ai->descriptorSetCount; i++) {
                    ep_set *e = malloc(sizeof(*e));
                    if (!e) continue;
                    e->set = out[i];
                    e->real = a2.descriptorPool;
                    unsigned b = sb(out[i]);
                    e->next = s->sets[b];
                    s->sets[b] = e;
                }
            break;
        }
        if (r != VK_ERROR_OUT_OF_POOL_MEMORY && r != VK_ERROR_FRAGMENTED_POOL) break;
        /* An overflow pool nothing was allocated from since the last reset is empty, and it is
         * made like the app's: a set it refuses no pool like it can hold (more of a type than the
         * pool has), so another one cannot help. Detroit: Become Human (a player log, G925) asked a
         * FREE_DESCRIPTOR_SET pool of 45056 sets, still empty, for a set of 8426 image samplers
         * and 12800 storage buffers; each refusal made overflow pools until the 64 cap, 384 of
         * them for 6 pools, 8 MB each in Mali's heap: 3 GB at the game's start for nothing. */
        if (p->cur >= p->used) {
            if (ai->descriptorSetCount == 1) {
                if (p->refused_gen != gen) p->nrefused = p->refused_next = 0;
                p->refused_gen = gen;
                uint32_t vc = var_count(ai, 0), k = 0;
                while (k < p->nrefused && p->refused[k] != ai->pSetLayouts[0]) k++;
                if (k < p->nrefused) {
                    if (vc < p->refused_count[k]) p->refused_count[k] = vc; /* the smallest size refused */
                } else {
                    k = p->refused_next++ & 3;
                    p->refused[k] = ai->pSetLayouts[0];
                    p->refused_count[k] = vc;
                    if (p->nrefused < 4) p->nrefused++;
                }
            }
            if (!s->oversize++)
                VKB_INFO("descriptor pools: a set that an empty pool like the app's refuses (bigger than the pool) is refused "
                         "without more overflow pools (logged once)");
            break;
        }
        p->cur++;
    }
    pthread_mutex_unlock(&s->lock);
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL ep_FreeDescriptorSets(VkDevice device, VkDescriptorPool pool, uint32_t n, const VkDescriptorSet *sets)
{
    vkb_pools *s = st();
    VkDescriptorSet local[64], *home = n <= 64 ? local : malloc(n * sizeof(*home));
    if (!home) return s->free_sets(device, pool, n, sets);
    uint32_t nhome = 0;
    pthread_mutex_lock(&s->lock);
    for (uint32_t i = 0; i < n; i++) {
        if (!sets[i]) continue;
        VkDescriptorPool real = VK_NULL_HANDLE;
        for (ep_set **pp = &s->sets[sb(sets[i])]; *pp; pp = &(*pp)->next)
            if ((*pp)->set == sets[i]) {
                ep_set *e = *pp;
                real = e->real;
                *pp = e->next;
                free(e);
                break;
            }
        if (real) s->free_sets(device, real, 1, &sets[i]);
        else home[nhome++] = sets[i];
    }
    pthread_mutex_unlock(&s->lock);
    VkResult r = nhome ? s->free_sets(device, pool, nhome, home) : VK_SUCCESS;
    if (home != local) free(home);
    return r;
}

static VKAPI_ATTR void VKAPI_CALL ep_DestroyDescriptorSetLayout(VkDevice device, VkDescriptorSetLayout l, const VkAllocationCallbacks *a)
{
    vkb_pools *s = st();
    if (l) __atomic_add_fetch(&s->layout_gen, 1, __ATOMIC_RELAXED);
    s->destroy_layout(device, l, a);
}

void vkb_pools_device_init(vkb_srv_table *t)
{
    vkb_dispatch *dt = &t->dt;
    if (t->id >= 4096 || vkb_srv_env("VKBRIDGE_NO_ELASTIC_POOLS") || !dt->vkCreateDescriptorPool || !dt->vkAllocateDescriptorSets) return;
    vkb_pools *s = calloc(1, sizeof(*s));
    if (!s) return;
    pthread_mutex_init(&s->lock, NULL);
    s->create = dt->vkCreateDescriptorPool;
    s->destroy = dt->vkDestroyDescriptorPool;
    s->reset = dt->vkResetDescriptorPool;
    s->allocate = dt->vkAllocateDescriptorSets;
    s->free_sets = dt->vkFreeDescriptorSets;
    s->destroy_layout = dt->vkDestroyDescriptorSetLayout;
    dt->vkCreateDescriptorPool = ep_CreateDescriptorPool;
    dt->vkDestroyDescriptorPool = ep_DestroyDescriptorPool;
    dt->vkResetDescriptorPool = ep_ResetDescriptorPool;
    dt->vkAllocateDescriptorSets = ep_AllocateDescriptorSets;
    dt->vkFreeDescriptorSets = ep_FreeDescriptorSets;
    if (s->destroy_layout) dt->vkDestroyDescriptorSetLayout = ep_DestroyDescriptorSetLayout;
    states[t->id] = s;
}

void vkb_pools_device_destroy(vkb_srv_table *t)
{
    if (t->id >= 4096 || !states[t->id]) return;
    vkb_pools *s = states[t->id];
    vkb_dispatch *dt = &t->dt;
    dt->vkCreateDescriptorPool = s->create;
    dt->vkDestroyDescriptorPool = s->destroy;
    dt->vkResetDescriptorPool = s->reset;
    dt->vkAllocateDescriptorSets = s->allocate;
    dt->vkFreeDescriptorSets = s->free_sets;
    if (s->destroy_layout) dt->vkDestroyDescriptorSetLayout = s->destroy_layout;
    for (int b = 0; b < SET_BUCKETS; b++)
        while (s->pools[b]) {
            ep_pool *p = s->pools[b];
            s->pools[b] = p->next;
            for (uint32_t i = 0; i < p->nextra; i++) t->real.vkDestroyDescriptorPool(t->device, p->extra[i], NULL);
            __atomic_sub_fetch(&g_extra_live, p->nextra, __ATOMIC_RELAXED);
            free(p->extra);
            free(p->sizes);
            free(p);
        }
    for (int i = 0; i < SET_BUCKETS; i++)
        while (s->sets[i]) {
            ep_set *e = s->sets[i];
            s->sets[i] = e->next;
            free(e);
        }
    if (s->grown || s->oversize)
        VKB_INFO("device %u: %lu overflow descriptor pools made, %lu allocation(s) no pool like the app's could hold", t->id, s->grown,
                 s->oversize);
    pthread_mutex_destroy(&s->lock);
    free(s);
    states[t->id] = NULL;
}
