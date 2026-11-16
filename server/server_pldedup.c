/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * Pipeline layouts made the same way share one driver layout (PowerVR).
 *
 * Imagination's driver (Pixel 10 Pro XL, DXT-48-1536, 25.1@6794074) keeps a host table in every
 * pipeline layout for each uniform or storage buffer descriptor its set layouts declare: 40 bytes a
 * descriptor for each shader stage it builds (four for VK_SHADER_STAGE_ALL), whatever the set's
 * flags (update after bind, variable count). Images and texel buffers cost nothing there. Measured on
 * the phone with the driver alone (plbench, 2026-11-11): six vkd3d-proton bindless sets of 1,000,000
 * cost 305 MB and 99-118 ms a pipeline layout (the CBV set and the raw SSBO set, 152.6 MB each);
 * one sampled-image set of 1,000,000 costs 0. vkd3d-proton makes two pipeline layouts a root
 * signature (graphics and compute), each over the same six sets, so a D3D12 game's root signatures
 * take the server's heap up by ~610 MB each: RE3 (952060) went 207 MB -> 3.2 GB in one second of
 * vkCreatePipelineLayout calls (Pixel, first run; the server's handler 119 ms a call)
 * and Android's low-memory killer ended the app; with the descriptor cut forced to 131,072 it still
 * reached 5.5 GB at 177 layouts.
 *
 * Non-dispatchable handles need not be unique (Vulkan "Object Model"): a pipeline layout asked for
 * like one still alive gets that layout's handle back, and the driver's layout is destroyed when
 * the last of them is. "Like" is by content: the flags, the push constant ranges and, for each set,
 * what its set layout was made with (flags, bindings, binding flags, immutable sampler handles), not
 * the set layout's handle - vkd3d-proton makes a set layout of its own for each root signature's
 * static samplers, and layouts whose set layouts are identically defined are compatible (Vulkan
 * "Pipeline Layout Compatibility"), so a shared layout works with sets made from any of them. A set
 * layout a shared driver layout was made with is kept for the driver until that layout goes (its
 * destroy is held back, never done twice). Set layouts with a pNext the server does not read count
 * as unlike any other; pipeline layouts with a pNext are made as they are. Installed outermost, so
 * every layer of the server and the driver see one layout; the client keeps nothing per pipeline
 * layout (it does per set layout, so set layouts themselves are never shared). The driver's cost is
 * logged (heap and time around its call) for the first layouts that cost 1 MB or more and at each
 * power of two of the layouts asked for. PowerVR only; VKBRIDGE_PL_SHARE=0 turns it off there, =1
 * turns it on on any GPU.
 *
 * One wide push range (2026-11-12, a player log): vkd3d-proton's root-signature layouts all use the same
 * seven bindless sets and differ only in their push constant range (root constants and root VAs), so
 * sharing by content still left RE3 with 29 driver layouts, ~4 GB of the driver's heap, mem_avail 0
 * and a driver SEGV in vkQueueSubmit2 at 80 s. A layout whose sets hold VKBRIDGE_PL_WIDE (65536)
 * uniform/storage buffer descriptors or more, and whose push ranges lie in [0, maxPushConstantsSize)
 * of the graphics stages only or of compute only, is made by the driver over the same sets with ONE
 * range [0, maxPushConstantsSize) of all graphics stages (VS|TCS|TES|GS|FS) or of compute: one driver
 * layout a set list and kind instead of one a root signature. A pipeline's push block lies inside it
 * (vkd3d-proton moves root signatures past maxPushConstantsSize to a push UBO). vkCmdPushConstants(2)
 * on such a layout get the wide range's stages (VUID-vkCmdPushConstants-offset-01796 asks for every
 * stage of a range a byte overlaps); graphics stages the app did not name receive bytes their shaders
 * do not read, and compute stays its own range, so a bind point never sees the other's pushes.
 * VKBRIDGE_PL_WIDE=0 turns this part off; layouts under the threshold are shared only when alike.
 */
#define _GNU_SOURCE
#include "vkb_server.h"

#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define PL_BUCKETS 256
#define PL_LINES 16
#define PL_MAX_SETS 32
#define PL_MAX_RANGES 16
#define PL_WIDE_MIN 65536 /* uniform/storage buffer descriptors in a layout's sets: ~10 MB a layout on PowerVR */
#define PL_WIDE_SLOTS 4096 /* wide driver layouts alive at once in the process (one a set list and kind a device: a handful) */
#define PL_WIDE_GFX (VK_SHADER_STAGE_ALL_GRAPHICS)

/* a set layout the app made: what it was made with (hash), and whether the driver must keep it */
typedef struct sl_rec {
    VkDescriptorSetLayout sl;
    uint64_t content;
    uint64_t bufs;   /* uniform/storage buffer descriptors it declares (what the PowerVR driver copies per layout) */
    uint32_t pins;   /* shared driver pipeline layouts made with it */
    int gone;        /* the app destroyed it while pinned: destroyed for the driver at the last unpin */
    struct sl_rec *next;
} sl_rec;

typedef struct pl_ent {
    VkPipelineLayout pl;
    uint32_t refs;
    uint32_t wide;   /* 0, or the stages of the one wide push range the driver layout was made with */
    uint32_t len;
    uint64_t hash;
    uint32_t nsets;
    VkDescriptorSetLayout made_with[PL_MAX_SETS]; /* the set layouts the driver layout was made with (pinned) */
    struct pl_ent *next;
    uint32_t key[]; /* flags, set count, set contents (2 words each), range count, ranges (3 words each) */
} pl_ent;

typedef struct pl_state {
    VkDevice dev;
    pthread_mutex_t lock;
    pl_ent *b[PL_BUCKETS];
    sl_rec *sl[PL_BUCKETS];
    PFN_vkCreatePipelineLayout create;
    PFN_vkDestroyPipelineLayout destroy;
    PFN_vkCreateDescriptorSetLayout create_sl;
    PFN_vkDestroyDescriptorSetLayout destroy_sl;
    PFN_vkCmdPushConstants push;
    PFN_vkCmdPushConstants2 push2;
    uint64_t wide_min;  /* 0: no wide push ranges */
    uint32_t wide_size; /* maxPushConstantsSize */
    uint32_t asked, made, shared, live, wide_made, wide_shared;
    long long driver_bytes; /* heap the driver took making layouts (measured, all of them) */
    int told;
} pl_state;

static pl_state *states[4096];

/* The driver layouts made with a wide push range, process-wide (driver handles are unique while alive): open
 * addressing, read without a lock on every vkCmdPushConstants; 1 marks a slot whose layout went. */
static uint64_t wide_reg[PL_WIDE_SLOTS];
static uint32_t wide_stages_of[PL_WIDE_SLOTS];
static const void *wide_owner[PL_WIDE_SLOTS]; /* the device state that made it (under wide_lock) */
static uint32_t wide_live; /* wide layouts in the registry, under wide_lock; freed slots are reused */
static uint32_t wide_full_told;
static int wide_any;
static pthread_mutex_t wide_lock = PTHREAD_MUTEX_INITIALIZER;

static unsigned wide_slot(uint64_t h)
{
    h ^= h >> 29;
    h *= 0xbf58476d1ce4e5b9ull;
    return (unsigned)(h >> 40) & (PL_WIDE_SLOTS - 1);
}

/* The wide range's stages for a layout, or 0 when it was made as asked. */
static uint32_t wide_stages(VkPipelineLayout l)
{
    if (!__atomic_load_n(&wide_any, __ATOMIC_ACQUIRE) || !l) return 0;
    uint64_t h = (uint64_t)(uintptr_t)l;
    for (unsigned i = wide_slot(h), n = 0; n < PL_WIDE_SLOTS; n++, i = (i + 1) & (PL_WIDE_SLOTS - 1)) {
        uint64_t v = __atomic_load_n(&wide_reg[i], __ATOMIC_ACQUIRE);
        if (!v) return 0;
        if (v == h) return __atomic_load_n(&wide_stages_of[i], __ATOMIC_RELAXED);
    }
    return 0;
}

/* A wide layout made by the driver goes in before the app has its handle. The layout is wide either way (a set list
 * and kind is always made the same way, so identically defined layouts stay compatible); a full registry (thousands
 * alive, never seen) only leaves its pushes as the app names them, said once. */
static void wide_add(const void *owner, VkPipelineLayout l, uint32_t stages)
{
    uint64_t h = (uint64_t)(uintptr_t)l;
    int full = 1;
    pthread_mutex_lock(&wide_lock);
    if (wide_live < PL_WIDE_SLOTS / 2)
        for (unsigned i = wide_slot(h), n = 0; n < PL_WIDE_SLOTS; n++, i = (i + 1) & (PL_WIDE_SLOTS - 1)) {
            uint64_t v = wide_reg[i];
            if (v && v != 1) continue;
            wide_owner[i] = owner;
            __atomic_store_n(&wide_stages_of[i], stages, __ATOMIC_RELAXED);
            __atomic_store_n(&wide_reg[i], h, __ATOMIC_RELEASE);
            wide_live++;
            full = 0;
            break;
        }
    __atomic_store_n(&wide_any, 1, __ATOMIC_RELEASE);
    int tell = full && !wide_full_told++;
    pthread_mutex_unlock(&wide_lock);
    if (tell) VKB_WARN("pipeline layouts: %u wide layouts alive, the registry is full: pushes on later ones keep the app's stages", wide_live);
}

/* Under wide_lock: with nothing left, the slots go back to empty (tombstones would make every lookup long). */
static void wide_compact(void)
{
    if (wide_live) return;
    __atomic_store_n(&wide_any, 0, __ATOMIC_RELEASE);
    for (unsigned i = 0; i < PL_WIDE_SLOTS; i++) {
        __atomic_store_n(&wide_reg[i], 0, __ATOMIC_RELEASE);
        wide_owner[i] = NULL;
    }
}

/* Every wide layout a device's state made leaves the registry (the device goes; its layouts go beneath it). */
static void wide_remove_owner(const void *owner)
{
    pthread_mutex_lock(&wide_lock);
    for (unsigned i = 0; i < PL_WIDE_SLOTS; i++) {
        uint64_t v = wide_reg[i];
        if (!v || v == 1 || wide_owner[i] != owner) continue;
        __atomic_store_n(&wide_reg[i], 1, __ATOMIC_RELEASE);
        wide_owner[i] = NULL;
        if (wide_live) wide_live--;
    }
    wide_compact();
    pthread_mutex_unlock(&wide_lock);
}

static void wide_remove(VkPipelineLayout l)
{
    uint64_t h = (uint64_t)(uintptr_t)l;
    pthread_mutex_lock(&wide_lock);
    for (unsigned i = wide_slot(h), n = 0; n < PL_WIDE_SLOTS; n++, i = (i + 1) & (PL_WIDE_SLOTS - 1)) {
        uint64_t v = wide_reg[i];
        if (!v) break;
        if (v == h) {
            __atomic_store_n(&wide_reg[i], 1, __ATOMIC_RELEASE); /* a tombstone keeps the chains behind it */
            wide_owner[i] = NULL;
            if (wide_live) wide_live--;
            break;
        }
    }
    wide_compact();
    pthread_mutex_unlock(&wide_lock);
}

/* The device's state: from the request's table, else (a call from a thread of the server's own) by its handle. */
static pl_state *cur(VkDevice dev)
{
    vkb_srv_table *t = vkb_srv_current_table();
    pl_state *s = t && t->id < 4096 ? states[t->id] : NULL;
    if (s && s->dev == dev) return s;
    for (int i = 0; i < 4096; i++)
        if (states[i] && states[i]->dev == dev) return states[i];
    return NULL;
}

static long long heap_now(void)
{
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 33))
    struct mallinfo2 mi = mallinfo2();
    return (long long)(size_t)mi.uordblks + (long long)(size_t)mi.hblkhd;
#elif defined(__GLIBC__)
    struct mallinfo mi = mallinfo();
    return (long long)(size_t)mi.uordblks + (long long)(size_t)mi.hblkhd;
#else
    struct mallinfo mi = mallinfo(); /* bionic's scudo counts every block in uordblks */
    return (long long)(size_t)mi.uordblks;
#endif
}

static double ms_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static uint64_t mix(uint64_t h, uint64_t v)
{
    for (int i = 0; i < 8; i++, v >>= 8) h = (h ^ (v & 0xff)) * 1099511628211ull;
    return h;
}

static unsigned hb(uint64_t k)
{
    k ^= k >> 31;
    k *= 0x94d049bb133111ebull;
    return (unsigned)(k >> 56) & (PL_BUCKETS - 1);
}

static sl_rec *sl_find(pl_state *s, VkDescriptorSetLayout l) /* under s->lock */
{
    for (sl_rec *r = s->sl[hb((uint64_t)(uintptr_t)l)]; r; r = r->next)
        if (r->sl == l) return r;
    return NULL;
}

/* What a set layout is made with, as one number; a pNext the server does not read makes it unlike any other. */
static uint64_t sl_content(const VkDescriptorSetLayoutCreateInfo *ci, VkDescriptorSetLayout made)
{
    uint64_t h = 1469598103934665603ull;
    h = mix(h, ci->flags);
    h = mix(h, ci->bindingCount);
    for (const VkBaseInStructure *p = ci->pNext; p; p = p->pNext) {
        if (p->sType != VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO) return mix(mix(h, 0xbad), (uint64_t)(uintptr_t)made);
        const VkDescriptorSetLayoutBindingFlagsCreateInfo *f = (const void *)p;
        h = mix(h, 0xf1a6);
        h = mix(h, f->bindingCount);
        for (uint32_t i = 0; i < f->bindingCount && f->pBindingFlags; i++) h = mix(h, f->pBindingFlags[i]);
    }
    for (uint32_t i = 0; i < ci->bindingCount; i++) {
        const VkDescriptorSetLayoutBinding *b = &ci->pBindings[i];
        h = mix(h, b->binding);
        h = mix(h, (uint64_t)b->descriptorType);
        h = mix(h, b->descriptorCount);
        h = mix(h, b->stageFlags);
        int samplers = b->descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER || b->descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        h = mix(h, samplers && b->pImmutableSamplers ? 1 : 0);
        for (uint32_t j = 0; samplers && b->pImmutableSamplers && j < b->descriptorCount; j++)
            h = mix(h, (uint64_t)(uintptr_t)b->pImmutableSamplers[j]);
    }
    return h;
}

static VKAPI_ATTR VkResult VKAPI_CALL pl_CreateDescriptorSetLayout(VkDevice device, const VkDescriptorSetLayoutCreateInfo *ci,
                                                                   const VkAllocationCallbacks *a, VkDescriptorSetLayout *out)
{
    pl_state *s = cur(device);
    VkResult r = s->create_sl(device, ci, a, out);
    if (r != VK_SUCCESS) return r;
    sl_rec *rec = calloc(1, sizeof(*rec));
    if (!rec) return r; /* unknown to the sharing: its pipeline layouts are keyed by its handle */
    rec->sl = *out;
    rec->content = sl_content(ci, *out);
    for (uint32_t i = 0; i < ci->bindingCount; i++) {
        VkDescriptorType ty = ci->pBindings[i].descriptorType;
        if (ty == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || ty == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ||
            ty == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC || ty == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC)
            rec->bufs += ci->pBindings[i].descriptorCount;
    }
    pthread_mutex_lock(&s->lock);
    unsigned k = hb((uint64_t)(uintptr_t)*out);
    rec->next = s->sl[k];
    s->sl[k] = rec;
    pthread_mutex_unlock(&s->lock);
    return r;
}

static VKAPI_ATTR void VKAPI_CALL pl_DestroyDescriptorSetLayout(VkDevice device, VkDescriptorSetLayout l, const VkAllocationCallbacks *a)
{
    pl_state *s = cur(device);
    if (!l || !s) return;
    pthread_mutex_lock(&s->lock);
    for (sl_rec **rp = &s->sl[hb((uint64_t)(uintptr_t)l)]; *rp; rp = &(*rp)->next) {
        if ((*rp)->sl != l) continue;
        if ((*rp)->pins) {
            (*rp)->gone = 1; /* a shared driver layout was made with it: destroyed when that goes */
            pthread_mutex_unlock(&s->lock);
            return;
        }
        sl_rec *r = *rp;
        *rp = r->next;
        free(r);
        break;
    }
    pthread_mutex_unlock(&s->lock);
    s->destroy_sl(device, l, a);
}

/* The stages of the wide push range a create info can be made with (under s->lock), or 0: its sets hold
 * wide_min uniform/storage buffer descriptors or more, all known to the server, and its ranges fit in
 * [0, maxPushConstantsSize) of the graphics stages only or of compute only (no range: graphics). */
static uint32_t wide_kind(pl_state *s, const VkPipelineLayoutCreateInfo *ci)
{
    if (!s->wide_min || ci->pNext || ci->setLayoutCount > PL_MAX_SETS || ci->pushConstantRangeCount > PL_MAX_RANGES) return 0;
    uint64_t bufs = 0;
    for (uint32_t i = 0; i < ci->setLayoutCount; i++) {
        sl_rec *r = ci->pSetLayouts[i] ? sl_find(s, ci->pSetLayouts[i]) : NULL;
        if (!r) return 0;
        bufs += r->bufs;
    }
    if (bufs < s->wide_min) return 0;
    uint32_t all = 0;
    for (uint32_t i = 0; i < ci->pushConstantRangeCount; i++) {
        const VkPushConstantRange *p = &ci->pPushConstantRanges[i];
        if (p->offset > s->wide_size || p->size > s->wide_size - p->offset) return 0;
        all |= p->stageFlags;
    }
    if (!all || !(all & ~PL_WIDE_GFX)) return PL_WIDE_GFX;
    return all == VK_SHADER_STAGE_COMPUTE_BIT ? VK_SHADER_STAGE_COMPUTE_BIT : 0;
}

/* The key words of a create info (under s->lock), or 0 when it is not shared (pNext, too big). With wide, the ranges
 * are left out: every layout over the same sets of that kind gets the one driver layout. */
static uint32_t make_key(pl_state *s, const VkPipelineLayoutCreateInfo *ci, uint32_t *k, uint32_t cap, uint32_t wide)
{
    if (ci->pNext || ci->setLayoutCount > PL_MAX_SETS || ci->pushConstantRangeCount > PL_MAX_RANGES) return 0;
    uint32_t n = 0;
    k[n++] = ci->flags;
    k[n++] = ci->setLayoutCount;
    for (uint32_t i = 0; i < ci->setLayoutCount; i++) {
        sl_rec *r = ci->pSetLayouts[i] ? sl_find(s, ci->pSetLayouts[i]) : NULL;
        /* unknown (or VK_NULL_HANDLE, allowed with graphicsPipelineLibrary): by its handle */
        uint64_t h = r ? r->content : (uint64_t)(uintptr_t)ci->pSetLayouts[i] ^ 0x5e75e75e75e75e75ull;
        k[n++] = (uint32_t)h;
        k[n++] = (uint32_t)(h >> 32);
    }
    if (wide) {
        k[n++] = 0xffffffffu; /* no range count is this: wide keys never meet exact ones */
        k[n++] = wide;
        return n <= cap ? n : 0;
    }
    k[n++] = ci->pushConstantRangeCount;
    for (uint32_t i = 0; i < ci->pushConstantRangeCount; i++) {
        k[n++] = ci->pPushConstantRanges[i].stageFlags;
        k[n++] = ci->pPushConstantRanges[i].offset;
        k[n++] = ci->pPushConstantRanges[i].size;
    }
    return n <= cap ? n : 0;
}

static uint64_t key_hash(const uint32_t *k, uint32_t n)
{
    uint64_t h = 1469598103934665603ull;
    for (uint32_t i = 0; i < n; i++) h = mix(h, k[i]);
    return h;
}

static pl_ent *find_key(pl_state *s, const uint32_t *k, uint32_t n, uint64_t h) /* under s->lock */
{
    for (pl_ent *e = s->b[h % PL_BUCKETS]; e; e = e->next)
        if (e->hash == h && e->len == n && !memcmp(e->key, k, n * sizeof(*k))) return e;
    return NULL;
}

/* Under s->lock: the entry's set layouts are let go; returns how many must now be destroyed for the driver (in out). */
static uint32_t unpin(pl_state *s, pl_ent *e, VkDescriptorSetLayout *out)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < e->nsets; i++) {
        sl_rec **rp = &s->sl[hb((uint64_t)(uintptr_t)e->made_with[i])];
        while (*rp && (*rp)->sl != e->made_with[i]) rp = &(*rp)->next;
        if (!*rp || !(*rp)->pins) continue;
        if (--(*rp)->pins || !(*rp)->gone) continue;
        sl_rec *r = *rp;
        *rp = r->next;
        out[n++] = r->sl;
        free(r);
    }
    return n;
}

static VKAPI_ATTR VkResult VKAPI_CALL pl_CreatePipelineLayout(VkDevice device, const VkPipelineLayoutCreateInfo *ci,
                                                              const VkAllocationCallbacks *a, VkPipelineLayout *out)
{
    pl_state *s = cur(device);
    uint32_t k[2 + PL_MAX_SETS * 2 + 1 + PL_MAX_RANGES * 3];
    uint32_t asked = __atomic_add_fetch(&s->asked, 1, __ATOMIC_RELAXED);
    pthread_mutex_lock(&s->lock);
    uint32_t wide = wide_kind(s, ci);
    uint32_t n = make_key(s, ci, k, sizeof(k) / sizeof(k[0]), wide);
    uint64_t h = n ? key_hash(k, n) : 0;
    pl_ent *e = n ? find_key(s, k, n, h) : NULL;
    if (e) {
        e->refs++;
        *out = e->pl;
        uint32_t sh = ++s->shared, refs = e->refs, made = s->made;
        uint32_t wsh = wide ? ++s->wide_shared : s->wide_shared;
        pthread_mutex_unlock(&s->lock);
        char wt[64] = "";
        if (wsh) snprintf(wt, sizeof(wt), " (%u over one wide push range)", wsh);
        if (sh <= 4 || !(asked & (asked - 1)))
            VKB_INFO("pipeline layout %u: made like one alive (%u set(s), %u push range(s)): shares it (%u users); so far %u asked, "
                     "%u made by the driver, %u shared%s",
                     asked, ci->setLayoutCount, ci->pushConstantRangeCount, refs, asked, made, sh, wt);
        return VK_SUCCESS;
    }
    pthread_mutex_unlock(&s->lock);
    VkPipelineLayoutCreateInfo wci = *ci;
    VkPushConstantRange wr = {wide, 0, s->wide_size};
    if (wide) {
        wci.pushConstantRangeCount = 1;
        wci.pPushConstantRanges = &wr;
    }
    long long h0 = heap_now();
    double t0 = ms_now();
    VkResult r = s->create(device, &wci, a, out);
    long long dh = heap_now() - h0;
    double dt = ms_now() - t0;
    if (r != VK_SUCCESS) return r;
    if (wide) wide_add(s, *out, wide); /* before the app has the handle: its pushes find it */
    uint32_t made = __atomic_add_fetch(&s->made, 1, __ATOMIC_RELAXED);
    if (wide) __atomic_add_fetch(&s->wide_made, 1, __ATOMIC_RELAXED);
    if (dh > 0) __atomic_add_fetch(&s->driver_bytes, dh, __ATOMIC_RELAXED);
    if ((dh >= (1 << 20) && __atomic_fetch_add(&s->told, 1, __ATOMIC_RELAXED) < PL_LINES) || !(asked & (asked - 1)))
        VKB_INFO("pipeline layout %u: %u set(s), %u push range(s)%s: the driver took %+lld MB of heap in %.1f ms; so far %u asked, %u made "
                 "by the driver, %u shared; the driver took ~%lld MB for them so far",
                 asked, ci->setLayoutCount, ci->pushConstantRangeCount,
                 wide == VK_SHADER_STAGE_COMPUTE_BIT ? ", made with one wide compute push range" : wide ? ", made with one wide graphics push range" : "",
                 dh >> 20, dt, asked, made, __atomic_load_n(&s->shared, __ATOMIC_RELAXED), __atomic_load_n(&s->driver_bytes, __ATOMIC_RELAXED) >> 20);
    if (!n) return r;
    pl_ent *ne = malloc(sizeof(*ne) + n * sizeof(*k));
    if (!ne) return r; /* made, not shared (a wide one stays in the registry until it goes) */
    ne->pl = *out;
    ne->refs = 1;
    ne->wide = wide;
    ne->len = n;
    ne->hash = h;
    ne->nsets = ci->setLayoutCount;
    memcpy(ne->key, k, n * sizeof(*k));
    pthread_mutex_lock(&s->lock);
    /* the key again: a set layout may have gone meanwhile (then this one is made, not shared) */
    uint32_t k2[sizeof(k) / sizeof(k[0])];
    uint32_t n2 = make_key(s, ci, k2, sizeof(k2) / sizeof(k2[0]), wide);
    if (n2 != n || memcmp(k2, k, n * sizeof(*k))) {
        pthread_mutex_unlock(&s->lock);
        free(ne);
        return r;
    }
    pl_ent *other = find_key(s, k, n, h); /* another thread made the same one meanwhile: that one is kept */
    if (other) {
        other->refs++;
        s->shared++;
        if (wide) s->wide_shared++;
        VkPipelineLayout mine = *out;
        *out = other->pl;
        pthread_mutex_unlock(&s->lock);
        free(ne);
        if (wide) wide_remove(mine);
        s->destroy(device, mine, a);
        return r;
    }
    for (uint32_t i = 0; i < ci->setLayoutCount; i++) {
        ne->made_with[i] = ci->pSetLayouts[i];
        sl_rec *sr = ci->pSetLayouts[i] ? sl_find(s, ci->pSetLayouts[i]) : NULL;
        if (sr) sr->pins++;
        else ne->made_with[i] = VK_NULL_HANDLE;
    }
    ne->next = s->b[h % PL_BUCKETS];
    s->b[h % PL_BUCKETS] = ne;
    s->live++;
    pthread_mutex_unlock(&s->lock);
    return r;
}

/* The state a command's call reaches: the request's device; from a thread of the server's own (no request), any
 * device's, as every PowerVR device's table beneath this layer holds the same functions. */
static pl_state *cur_cmd(void)
{
    vkb_srv_table *t = vkb_srv_current_table();
    pl_state *s = t && t->id < 4096 ? states[t->id] : NULL;
    for (int i = 0; !s && i < 4096; i++) s = states[i];
    return s;
}

/* Pushes on a layout made with a wide range name all of that range's stages (VUID-vkCmdPushConstants-offset-01796). */
static VKAPI_ATTR void VKAPI_CALL pl_CmdPushConstants(VkCommandBuffer cb, VkPipelineLayout l, VkShaderStageFlags st, uint32_t off,
                                                      uint32_t size, const void *data)
{
    pl_state *s = cur_cmd();
    uint32_t w = wide_stages(l);
    if (s) s->push(cb, l, w ? w : st, off, size, data);
}

static VKAPI_ATTR void VKAPI_CALL pl_CmdPushConstants2(VkCommandBuffer cb, const VkPushConstantsInfo *info)
{
    pl_state *s = cur_cmd();
    if (!s) return;
    uint32_t w = info ? wide_stages(info->layout) : 0;
    if (!s->push2) { /* a state whose table had no vkCmdPushConstants2: the same push through vkCmdPushConstants */
        if (info && info->layout) s->push(cb, info->layout, w ? w : info->stageFlags, info->offset, info->size, info->pValues);
        return;
    }
    if (!w) {
        s->push2(cb, info);
        return;
    }
    VkPushConstantsInfo wi = *info;
    wi.stageFlags = w;
    s->push2(cb, &wi);
}

static VKAPI_ATTR void VKAPI_CALL pl_DestroyPipelineLayout(VkDevice device, VkPipelineLayout pl, const VkAllocationCallbacks *a)
{
    pl_state *s = cur(device);
    if (!pl || !s) return;
    VkDescriptorSetLayout late[PL_MAX_SETS];
    uint32_t nlate = 0;
    pthread_mutex_lock(&s->lock);
    for (int i = 0; i < PL_BUCKETS; i++)
        for (pl_ent **ep = &s->b[i]; *ep; ep = &(*ep)->next) {
            if ((*ep)->pl != pl) continue;
            pl_ent *e = *ep;
            if (--e->refs) {
                pthread_mutex_unlock(&s->lock);
                return; /* still used by another of the app's layouts */
            }
            *ep = e->next;
            s->live--;
            nlate = unpin(s, e, late);
            free(e);
            goto real;
        }
real:
    pthread_mutex_unlock(&s->lock);
    wide_remove(pl); /* before the driver may hand the handle out again */
    s->destroy(device, pl, a);
    for (uint32_t i = 0; i < nlate; i++) s->destroy_sl(device, late[i], NULL); /* the app's destroy, held back for the driver's layout */
}

void vkb_pldedup_device_init(vkb_srv_table *t)
{
    vkb_dispatch *dt = &t->dt;
    if (t->id >= 4096 || !dt->vkCreatePipelineLayout || !dt->vkDestroyPipelineLayout || !dt->vkCreateDescriptorSetLayout ||
        !dt->vkDestroyDescriptorSetLayout)
        return;
    const char *v = vkb_srv_env("VKBRIDGE_PL_SHARE");
    int on = v && *v ? strcmp(v, "0") != 0
                     : vkb_driver_id(&t->real, t->physical, t->api_version) == VK_DRIVER_ID_IMAGINATION_PROPRIETARY;
    if (!on) return;
    pl_state *s = calloc(1, sizeof(*s));
    if (!s) return;
    pthread_mutex_init(&s->lock, NULL);
    s->dev = t->device;
    s->create = dt->vkCreatePipelineLayout;
    s->destroy = dt->vkDestroyPipelineLayout;
    s->create_sl = dt->vkCreateDescriptorSetLayout;
    s->destroy_sl = dt->vkDestroyDescriptorSetLayout;
    dt->vkCreatePipelineLayout = pl_CreatePipelineLayout;
    dt->vkDestroyPipelineLayout = pl_DestroyPipelineLayout;
    dt->vkCreateDescriptorSetLayout = pl_CreateDescriptorSetLayout;
    dt->vkDestroyDescriptorSetLayout = pl_DestroyDescriptorSetLayout;
    const char *w = vkb_srv_env("VKBRIDGE_PL_WIDE");
    s->wide_min = w && *w ? strtoull(w, NULL, 10) : PL_WIDE_MIN;
    if (t->real.vkGetPhysicalDeviceProperties) {
        VkPhysicalDeviceProperties p;
        memset(&p, 0, sizeof(p));
        t->real.vkGetPhysicalDeviceProperties(t->physical, &p);
        s->wide_size = p.limits.maxPushConstantsSize & ~3u;
    }
    if (!dt->vkCmdPushConstants || s->wide_size < 4) s->wide_min = 0;
    if (s->wide_min) {
        s->push = dt->vkCmdPushConstants;
        s->push2 = dt->vkCmdPushConstants2;
        dt->vkCmdPushConstants = pl_CmdPushConstants;
        if (dt->vkCmdPushConstants2) dt->vkCmdPushConstants2 = pl_CmdPushConstants2;
    }
    states[t->id] = s;
    VKB_INFO("device %u: pipeline layouts made the same way share one driver layout (the driver keeps a table per buffer descriptor "
             "in each; VKBRIDGE_PL_SHARE=0: off)",
             t->id);
    if (s->wide_min)
        VKB_INFO("device %u: layouts over sets of %llu or more uniform/storage buffer descriptors share one driver layout a set list, "
                 "made with one wide push range [0, %u) of the graphics stages or of compute (VKBRIDGE_PL_WIDE=0: off)",
                 t->id, (unsigned long long)s->wide_min, s->wide_size);
}

/* Before the device goes: driver layouts and held-back set layouts still alive are destroyed beneath it on PowerVR
 * (server_children.c) or left to the driver as any leftover; here only the records go. */
void vkb_pldedup_device_destroy(vkb_srv_table *t)
{
    if (t->id >= 4096 || !states[t->id]) return;
    pl_state *s = states[t->id];
    t->dt.vkCreatePipelineLayout = s->create;
    t->dt.vkDestroyPipelineLayout = s->destroy;
    t->dt.vkCreateDescriptorSetLayout = s->create_sl;
    t->dt.vkDestroyDescriptorSetLayout = s->destroy_sl;
    if (s->wide_min) {
        if (t->dt.vkCmdPushConstants == pl_CmdPushConstants) t->dt.vkCmdPushConstants = s->push;
        if (t->dt.vkCmdPushConstants2 == pl_CmdPushConstants2) t->dt.vkCmdPushConstants2 = s->push2;
    }
    char wt[96] = "";
    if (s->wide_made) snprintf(wt, sizeof(wt), "; %u made with one wide push range, %u shared over one", s->wide_made, s->wide_shared);
    if (s->asked)
        VKB_INFO("device %u: pipeline layouts: %u asked, %u made by the driver, %u shared, %u still alive; the driver took ~%lld MB for them%s",
                 t->id, s->asked, s->made, s->shared, s->live, s->driver_bytes >> 20, wt);
    /* the device's wide layouts leave the registry, shared or not (the driver destroys them beneath, server_children.c) */
    wide_remove_owner(s);
    for (int i = 0; i < PL_BUCKETS; i++) {
        while (s->b[i]) {
            pl_ent *e = s->b[i];
            s->b[i] = e->next;
            free(e);
        }
        while (s->sl[i]) {
            sl_rec *r = s->sl[i];
            s->sl[i] = r->next;
            free(r);
        }
    }
    pthread_mutex_destroy(&s->lock);
    free(s);
    states[t->id] = NULL;
}
