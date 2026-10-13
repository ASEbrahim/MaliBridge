/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Descriptor limits above the GPU's (VKB_EMU_DESC_LIMITS). vkd3d-proton's bindless path without
 * descriptor buffers wants 1,000,000 sampled images, storage images and storage buffers per stage
 * after bind (D3D12 resource binding tier 2) and refuses the device otherwise (Deep Rock Galactic
 * DX12, 2026-10-11: "Insufficient descriptor indexing support"); Mali-G720 r44 allows 500,000.
 * The client reports 1,048,576; here every set layout whose sampled-image, storage-image or
 * storage-buffer total goes past what the driver allows has its large bindings (64k and up) cut to
 * share what is allowed, and the rest follows: variable descriptor counts are cut to the binding,
 * pools the driver refuses are retried at the limit, and writes, copies and update templates
 * reaching past the cut binding are shortened or dropped (logged). D3D12 heaps fill from the
 * start, so a game that never uses the top half runs as on a tier-2 GPU; one that does logs it.
 * Layouts within the limits - everything DXVK and Zink make - are not touched.
 */
#define _GNU_SOURCE
#include "vkb_server.h"
#include "server_emu.h"

#include <stdlib.h>
#include <string.h>

#define DL_BUCKETS 1024
#define DL_BIG 65536   /* bindings smaller than this keep their size; the large ones share the rest */
#define DL_LOCAL 64

enum { CAT_SAMPLED, CAT_STORAGE_IMAGE, CAT_STORAGE_BUFFER, CAT_N };

typedef struct dl_binding {
    uint32_t binding, count, asked;
} dl_binding;

typedef struct dl_layout {
    VkDescriptorSetLayout layout;
    uint32_t refs;              /* the layout while it lives, and each of its sets */
    uint32_t n;
    dl_binding *b;              /* the bindings that were cut */
    uint32_t var_count;         /* the variable-count binding's size when cut, else UINT32_MAX */
    struct dl_layout *next;
} dl_layout;

typedef struct dl_set {
    VkDescriptorSet set;
    VkDescriptorPool pool;
    dl_layout *layout;
    struct dl_set *next;
} dl_set;

typedef struct dl_pool {
    VkDescriptorPool pool;
    uint32_t nsets;
    struct dl_pool *next;
} dl_pool;

typedef struct dl_state {
    pthread_mutex_t lock;
    uint32_t limit[2][CAT_N];   /* [update after bind][category]: min of the per-stage and per-set limits */
    dl_layout *layouts[DL_BUCKETS];
    dl_set *sets[DL_BUCKETS];
    dl_pool *pools;             /* pools holding sets of cut layouts */
    uint32_t nlayouts, nsets;   /* read without the lock: 0 = nothing to look up */
    int always_cut;             /* VKBRIDGE_DESC_CUT=1: cut without asking the driver first */
    int told_whole;
    PFN_vkCreateDescriptorSetLayout create_dsl;
    PFN_vkDestroyDescriptorSetLayout destroy_dsl;
    PFN_vkGetDescriptorSetLayoutSupport dsl_support;
    PFN_vkCreateDescriptorPool create_pool;
    PFN_vkDestroyDescriptorPool destroy_pool;
    PFN_vkResetDescriptorPool reset_pool;
    PFN_vkAllocateDescriptorSets allocate;
    PFN_vkFreeDescriptorSets free_sets;
    PFN_vkUpdateDescriptorSets update;
    PFN_vkCreateDescriptorUpdateTemplate create_tmpl;
} dl_state;

static dl_state *states[4096];
static int told_write, told_alloc, told_tmpl;

static dl_state *st(void)
{
    vkb_srv_table *t = vkb_srv_current_table();
    return t && t->id < 4096 ? states[t->id] : NULL;
}

static unsigned hb(uint64_t k)
{
    k ^= k >> 31;
    k *= 0x94d049bb133111ebull;
    return (unsigned)(k >> 54) & (DL_BUCKETS - 1);
}

static unsigned cats(VkDescriptorType t)
{
    switch ((int)t) {
    case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
    case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
    case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER: return 1u << CAT_SAMPLED;
    case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
    case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER: return 1u << CAT_STORAGE_IMAGE;
    case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER: return 1u << CAT_STORAGE_BUFFER;
    /* vkd3d's mutable sets hold images and buffers alike: counted against all three. */
    case VK_DESCRIPTOR_TYPE_MUTABLE_EXT: return (1u << CAT_SAMPLED) | (1u << CAT_STORAGE_IMAGE) | (1u << CAT_STORAGE_BUFFER);
    default: return 0;
    }
}

void vkb_emu_desc_limits_get(const vkb_dispatch *d, VkPhysicalDevice pd, uint32_t out[2][3])
{
    VkPhysicalDeviceProperties p;
    d->vkGetPhysicalDeviceProperties(pd, &p);
    const VkPhysicalDeviceLimits *l = &p.limits;
    VkPhysicalDeviceDescriptorIndexingProperties di = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES};
    if (d->vkGetPhysicalDeviceProperties2 && p.apiVersion >= VK_API_VERSION_1_2) {
        VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &di};
        d->vkGetPhysicalDeviceProperties2(pd, &p2);
    }
#define MIN(a, b) ((a) < (b) ? (a) : (b))
    out[0][CAT_SAMPLED] = MIN(l->maxPerStageDescriptorSampledImages, l->maxDescriptorSetSampledImages);
    out[0][CAT_STORAGE_IMAGE] = MIN(l->maxPerStageDescriptorStorageImages, l->maxDescriptorSetStorageImages);
    out[0][CAT_STORAGE_BUFFER] = MIN(l->maxPerStageDescriptorStorageBuffers, l->maxDescriptorSetStorageBuffers);
    out[1][CAT_SAMPLED] = MIN(di.maxPerStageDescriptorUpdateAfterBindSampledImages, di.maxDescriptorSetUpdateAfterBindSampledImages);
    out[1][CAT_STORAGE_IMAGE] = MIN(di.maxPerStageDescriptorUpdateAfterBindStorageImages, di.maxDescriptorSetUpdateAfterBindStorageImages);
    out[1][CAT_STORAGE_BUFFER] = MIN(di.maxPerStageDescriptorUpdateAfterBindStorageBuffers, di.maxDescriptorSetUpdateAfterBindStorageBuffers);
    /* VKBRIDGE_FAKE_MISSING=desclimits: Mali r44's 500,000 for all of them (host tests). */
    if (vkb_emu_faked("desclimits"))
        for (int u = 0; u < 2; u++)
            for (int c = 0; c < CAT_N; c++) out[u][c] = MIN(out[u][c], 500000);
#undef MIN
}

/* New sizes for ci's bindings in out; 0 when the layout fits as it is (or cannot be made to). */
static int plan(dl_state *s, const VkDescriptorSetLayoutCreateInfo *ci, uint32_t *out)
{
    const uint32_t *lim = s->limit[(ci->flags & VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT) ? 1 : 0];
    uint64_t sum[CAT_N] = {0}, small[CAT_N] = {0};
    uint32_t nbig[CAT_N] = {0};
    for (uint32_t i = 0; i < ci->bindingCount; i++) {
        const VkDescriptorSetLayoutBinding *b = &ci->pBindings[i];
        unsigned m = cats(b->descriptorType);
        for (int c = 0; c < CAT_N; c++) {
            if (!(m & (1u << c))) continue;
            sum[c] += b->descriptorCount;
            if (b->descriptorCount < DL_BIG) small[c] += b->descriptorCount;
            else nbig[c]++;
        }
    }
    uint32_t share[CAT_N];
    int over = 0;
    for (int c = 0; c < CAT_N; c++) {
        share[c] = UINT32_MAX;
        if (sum[c] <= lim[c] || !lim[c]) continue;
        if (!nbig[c] || small[c] >= lim[c]) return 0; /* nothing large to cut: the driver decides */
        share[c] = (uint32_t)((lim[c] - small[c]) / nbig[c]);
        over = 1;
    }
    if (!over) return 0;
    int changed = 0;
    for (uint32_t i = 0; i < ci->bindingCount; i++) {
        const VkDescriptorSetLayoutBinding *b = &ci->pBindings[i];
        unsigned m = cats(b->descriptorType);
        out[i] = b->descriptorCount;
        if (b->descriptorCount < DL_BIG) continue;
        for (int c = 0; c < CAT_N; c++)
            if ((m & (1u << c)) && out[i] > share[c]) out[i] = share[c];
        changed |= out[i] != b->descriptorCount;
    }
    return changed;
}

static int variable_index(const VkDescriptorSetLayoutCreateInfo *ci)
{
    for (const VkBaseInStructure *p = ci->pNext; p; p = p->pNext) {
        if (p->sType != VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO) continue;
        const VkDescriptorSetLayoutBindingFlagsCreateInfo *f = (const void *)p;
        for (uint32_t i = 0; i < f->bindingCount && i < ci->bindingCount; i++)
            if (f->pBindingFlags[i] & VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT) return (int)i;
    }
    return -1;
}

/* ------------------------------------------------------------------ records (under the lock) */

static dl_layout *find_layout(dl_state *s, VkDescriptorSetLayout l)
{
    for (dl_layout *r = s->layouts[hb((uint64_t)l)]; r; r = r->next)
        if (r->layout == l) return r;
    return NULL;
}

static void unref_layout(dl_layout *r)
{
    if (--r->refs) return;
    free(r->b);
    free(r);
}

static dl_set *find_set(dl_state *s, VkDescriptorSet set)
{
    for (dl_set *e = s->sets[hb((uint64_t)set)]; e; e = e->next)
        if (e->set == set) return e;
    return NULL;
}

static dl_pool **find_pool(dl_state *s, VkDescriptorPool pool)
{
    dl_pool **pp = &s->pools;
    while (*pp && (*pp)->pool != pool) pp = &(*pp)->next;
    return pp;
}

static void drop_set(dl_state *s, dl_set **ep)
{
    dl_set *e = *ep;
    *ep = e->next;
    dl_pool **pp = find_pool(s, e->pool);
    if (*pp && !--(*pp)->nsets) {
        dl_pool *p = *pp;
        *pp = p->next;
        free(p);
    }
    unref_layout(e->layout);
    free(e);
    __atomic_sub_fetch(&s->nsets, 1, __ATOMIC_RELAXED);
}

/* How many descriptors from element elem of a binding fit; UINT32_MAX when the binding was not cut. */
static uint32_t room_layout(const dl_layout *r, uint32_t binding, uint32_t elem)
{
    if (!r) return UINT32_MAX;
    for (uint32_t i = 0; i < r->n; i++)
        if (r->b[i].binding == binding) return elem < r->b[i].count ? r->b[i].count - elem : 0;
    return UINT32_MAX;
}

static uint32_t room(dl_state *s, VkDescriptorSet set, uint32_t binding, uint32_t elem)
{
    dl_set *e = find_set(s, set);
    return e ? room_layout(e->layout, binding, elem) : UINT32_MAX;
}

/* A write that starts inside the cut binding and runs past it is how vkd3d fills a heap with null
 * descriptors when it makes it: logged quietly. One that starts past it is a descriptor the game
 * will look for and not find. */
static void tell_cut(int *told, const char *what, uint32_t binding, uint32_t elem, uint32_t count, uint32_t kept)
{
    static int told_short;
    if (kept && __atomic_fetch_add(&told_short, 1, __ATOMIC_RELAXED) < 8)
        VKB_INFO("%s: binding %u elements %u..%u shortened to %u (descriptor limits emulated, first 8 logged)", what, binding, elem,
                 elem + count - 1, kept);
    if (!kept && __atomic_fetch_add(told, 1, __ATOMIC_RELAXED) < 8)
        VKB_WARN("%s: binding %u elements %u..%u are past the descriptors the GPU allows; dropped (descriptor limits emulated, "
                 "first 8 logged)", what, binding, elem, elem + count - 1);
}

/* ------------------------------------------------------------------ wrappers */

static VKAPI_ATTR VkResult VKAPI_CALL d_CreateDescriptorSetLayout(VkDevice device, const VkDescriptorSetLayoutCreateInfo *ci,
                                                                  const VkAllocationCallbacks *a, VkDescriptorSetLayout *out)
{
    dl_state *s = st();
    uint32_t n = ci->bindingCount;
    uint32_t cnt_local[DL_LOCAL], *cnt = n <= DL_LOCAL ? cnt_local : malloc(n * sizeof(*cnt));
    if (!cnt || !plan(s, ci, cnt)) {
        if (cnt != cnt_local) free(cnt);
        return s->create_dsl(device, ci, a, out);
    }
    /* Past the limits the driver advertises, but asked of it as it is first: Mali r44 says
     * 500,000 (and "unsupported" to vkGetDescriptorSetLayoutSupport) yet creates, fills and reads
     * vkd3d's 1,000,000-descriptor sets correctly, index 999,999 included (tablet, 2026-10-11).
     * Cut, The Witcher 3 (D3D12) wrote its heap past 851,968 and never drew a frame. Only a
     * refusal falls back to the cut; VKBRIDGE_DESC_CUT=1 always cuts. */
    if (!s->always_cut) {
        VkResult whole = s->create_dsl(device, ci, a, out);
        if (whole == VK_SUCCESS) {
            if (!__atomic_exchange_n(&s->told_whole, 1, __ATOMIC_RELAXED))
                VKB_INFO("descriptor set layout past the advertised limits taken by the driver as it is (not cut)");
            if (cnt != cnt_local) free(cnt);
            return whole;
        }
        VKB_INFO("descriptor set layout past the advertised limits refused (%d); cut to them", whole);
    }
    VkDescriptorSetLayoutBinding *nb = malloc(n * sizeof(*nb));
    dl_layout *r = calloc(1, sizeof(*r));
    dl_binding *rb = malloc(n * sizeof(*rb));
    VkResult res = VK_ERROR_OUT_OF_HOST_MEMORY;
    if (!nb || !r || !rb) goto out;
    int var = variable_index(ci);
    r->var_count = UINT32_MAX;
    for (uint32_t i = 0; i < n; i++) {
        nb[i] = ci->pBindings[i];
        if (cnt[i] == nb[i].descriptorCount) continue;
        rb[r->n++] = (dl_binding){nb[i].binding, cnt[i], nb[i].descriptorCount};
        VKB_INFO("descriptor set layout%s: binding %u (type %d) cut from %u to %u descriptors (descriptor limits emulated)",
                 (ci->flags & VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT) ? " (update after bind)" : "",
                 nb[i].binding, nb[i].descriptorType, nb[i].descriptorCount, cnt[i]);
        if ((int)i == var) r->var_count = cnt[i];
        nb[i].descriptorCount = cnt[i];
    }
    VkDescriptorSetLayoutCreateInfo c2 = *ci;
    c2.pBindings = nb;
    res = s->create_dsl(device, &c2, a, out);
    if (res == VK_SUCCESS) {
        r->layout = *out;
        r->refs = 1;
        r->b = rb;
        rb = NULL;
        pthread_mutex_lock(&s->lock);
        unsigned h = hb((uint64_t)*out);
        r->next = s->layouts[h];
        s->layouts[h] = r;
        r = NULL;
        s->nlayouts++;
        pthread_mutex_unlock(&s->lock);
    }
out:
    if (cnt != cnt_local) free(cnt);
    free(nb);
    free(rb);
    free(r);
    return res;
}

static VKAPI_ATTR void VKAPI_CALL d_DestroyDescriptorSetLayout(VkDevice device, VkDescriptorSetLayout l, const VkAllocationCallbacks *a)
{
    dl_state *s = st();
    if (l && __atomic_load_n(&s->nlayouts, __ATOMIC_RELAXED)) {
        pthread_mutex_lock(&s->lock);
        for (dl_layout **rp = &s->layouts[hb((uint64_t)l)]; *rp; rp = &(*rp)->next) {
            if ((*rp)->layout != l) continue;
            dl_layout *r = *rp;
            *rp = r->next;
            s->nlayouts--;
            unref_layout(r); /* sets made from it keep their record */
            break;
        }
        pthread_mutex_unlock(&s->lock);
    }
    s->destroy_dsl(device, l, a);
}

static VKAPI_ATTR void VKAPI_CALL d_GetDescriptorSetLayoutSupport(VkDevice device, const VkDescriptorSetLayoutCreateInfo *ci,
                                                                  VkDescriptorSetLayoutSupport *sup)
{
    dl_state *s = st();
    uint32_t n = ci->bindingCount;
    uint32_t cnt_local[DL_LOCAL], *cnt = n <= DL_LOCAL ? cnt_local : malloc(n * sizeof(*cnt));
    VkDescriptorSetLayoutBinding *nb = NULL;
    if (!cnt || !plan(s, ci, cnt) || !(nb = malloc(n * sizeof(*nb)))) {
        s->dsl_support(device, ci, sup);
        goto out;
    }
    /* Mali says "unsupported" to layouts past its limits that it then creates and reads fine (see
     * d_CreateDescriptorSetLayout): when the driver takes the layout as it is, it is supported. */
    VkDescriptorSetLayout probe = VK_NULL_HANDLE;
    if (!s->always_cut && s->create_dsl(device, ci, NULL, &probe) == VK_SUCCESS) {
        s->destroy_dsl(device, probe, NULL);
        sup->supported = VK_TRUE;
        int v = variable_index(ci);
        for (VkBaseOutStructure *p = (VkBaseOutStructure *)sup->pNext; p; p = p->pNext)
            if (p->sType == VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_LAYOUT_SUPPORT)
                ((VkDescriptorSetVariableDescriptorCountLayoutSupport *)p)->maxVariableDescriptorCount =
                    v >= 0 ? ci->pBindings[v].descriptorCount : 0;
        goto out;
    }
    for (uint32_t i = 0; i < n; i++) {
        nb[i] = ci->pBindings[i];
        nb[i].descriptorCount = cnt[i];
    }
    VkDescriptorSetLayoutCreateInfo c2 = *ci;
    c2.pBindings = nb;
    s->dsl_support(device, &c2, sup);
    /* The variable binding is reported at the size asked for: allocations are cut to what fits. */
    int var = variable_index(ci);
    if (sup->supported && var >= 0 && cnt[var] != ci->pBindings[var].descriptorCount)
        for (VkBaseOutStructure *p = (VkBaseOutStructure *)sup->pNext; p; p = p->pNext)
            if (p->sType == VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_LAYOUT_SUPPORT)
                ((VkDescriptorSetVariableDescriptorCountLayoutSupport *)p)->maxVariableDescriptorCount = ci->pBindings[var].descriptorCount;
out:
    if (cnt != cnt_local) free(cnt);
    free(nb);
}

static VKAPI_ATTR VkResult VKAPI_CALL d_CreateDescriptorPool(VkDevice device, const VkDescriptorPoolCreateInfo *ci,
                                                             const VkAllocationCallbacks *a, VkDescriptorPool *out)
{
    dl_state *s = st();
    VkResult r = s->create_pool(device, ci, a, out);
    if (r == VK_SUCCESS || !ci->poolSizeCount) return r;
    /* Refused: sizes past the per-set limit are cut to it (sets of cut layouts take no more). */
    const uint32_t *lim = s->limit[(ci->flags & VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT) ? 1 : 0];
    VkDescriptorPoolSize *ps = malloc(ci->poolSizeCount * sizeof(*ps));
    if (!ps) return r;
    int cut = 0;
    for (uint32_t i = 0; i < ci->poolSizeCount; i++) {
        ps[i] = ci->pPoolSizes[i];
        unsigned m = cats(ps[i].type);
        for (int c = 0; c < CAT_N; c++)
            if ((m & (1u << c)) && lim[c] && ps[i].descriptorCount > lim[c]) {
                ps[i].descriptorCount = lim[c];
                cut = 1;
            }
    }
    if (cut) {
        VkDescriptorPoolCreateInfo c2 = *ci;
        c2.pPoolSizes = ps;
        VkResult r2 = s->create_pool(device, &c2, a, out);
        VKB_INFO("descriptor pool refused (%d); with its sizes cut to the per-set limits: %d (descriptor limits emulated)", r, r2);
        r = r2;
    }
    free(ps);
    return r;
}

static void forget_pool(dl_state *s, VkDescriptorPool pool)
{
    if (!__atomic_load_n(&s->nsets, __ATOMIC_RELAXED)) return;
    pthread_mutex_lock(&s->lock);
    if (*find_pool(s, pool)) {
        for (int i = 0; i < DL_BUCKETS && *find_pool(s, pool); i++)
            for (dl_set **ep = &s->sets[i]; *ep;) {
                if ((*ep)->pool == pool) drop_set(s, ep);
                else ep = &(*ep)->next;
            }
    }
    pthread_mutex_unlock(&s->lock);
}

static VKAPI_ATTR void VKAPI_CALL d_DestroyDescriptorPool(VkDevice device, VkDescriptorPool pool, const VkAllocationCallbacks *a)
{
    dl_state *s = st();
    if (pool) forget_pool(s, pool);
    s->destroy_pool(device, pool, a);
}

static VKAPI_ATTR VkResult VKAPI_CALL d_ResetDescriptorPool(VkDevice device, VkDescriptorPool pool, VkDescriptorPoolResetFlags flags)
{
    dl_state *s = st();
    forget_pool(s, pool);
    return s->reset_pool(device, pool, flags);
}

static VKAPI_ATTR VkResult VKAPI_CALL d_AllocateDescriptorSets(VkDevice device, const VkDescriptorSetAllocateInfo *ai, VkDescriptorSet *out)
{
    dl_state *s = st();
    uint32_t n = ai->descriptorSetCount;
    if (!__atomic_load_n(&s->nlayouts, __ATOMIC_RELAXED) || !n) return s->allocate(device, ai, out);
    uint32_t var_local[DL_LOCAL], *var = n <= DL_LOCAL ? var_local : malloc(n * sizeof(*var));
    if (!var) return VK_ERROR_OUT_OF_HOST_MEMORY;
    int any = 0;
    pthread_mutex_lock(&s->lock);
    for (uint32_t i = 0; i < n; i++) {
        dl_layout *r = find_layout(s, ai->pSetLayouts[i]);
        var[i] = r ? r->var_count : UINT32_MAX;
        any |= r != NULL;
    }
    pthread_mutex_unlock(&s->lock);
    if (!any) {
        if (var != var_local) free(var);
        return s->allocate(device, ai, out);
    }
    /* Variable counts past the cut binding are cut to it (the request is ours: restored after). */
    VkDescriptorSetVariableDescriptorCountAllocateInfo *vc = NULL;
    for (const VkBaseInStructure *p = ai->pNext; p; p = p->pNext)
        if (p->sType == VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO) vc = (void *)p;
    const uint32_t *counts = vc ? vc->pDescriptorCounts : NULL;
    if (vc && vc->descriptorSetCount == n) {
        int cut = 0;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t want = counts[i];
            if (want > var[i]) {
                cut = 1;
                if (__atomic_fetch_add(&told_alloc, 1, __ATOMIC_RELAXED) < 8)
                    VKB_INFO("descriptor set with %u variable descriptors made with %u (descriptor limits emulated)", want, var[i]);
            }
            var[i] = want > var[i] ? var[i] : want;
        }
        if (cut) vc->pDescriptorCounts = var;
    }
    VkResult res = s->allocate(device, ai, out);
    if (vc) vc->pDescriptorCounts = counts;
    if (res == VK_SUCCESS) {
        pthread_mutex_lock(&s->lock);
        for (uint32_t i = 0; i < n; i++) {
            dl_layout *r = find_layout(s, ai->pSetLayouts[i]);
            dl_set *e = r ? malloc(sizeof(*e)) : NULL;
            if (!e) continue;
            *e = (dl_set){out[i], ai->descriptorPool, r, NULL};
            r->refs++;
            unsigned h = hb((uint64_t)out[i]);
            e->next = s->sets[h];
            s->sets[h] = e;
            dl_pool **pp = find_pool(s, ai->descriptorPool);
            if (!*pp && (*pp = calloc(1, sizeof(**pp)))) (*pp)->pool = ai->descriptorPool;
            if (*pp) (*pp)->nsets++;
            __atomic_add_fetch(&s->nsets, 1, __ATOMIC_RELAXED);
        }
        pthread_mutex_unlock(&s->lock);
    }
    if (var != var_local) free(var);
    return res;
}

static VKAPI_ATTR VkResult VKAPI_CALL d_FreeDescriptorSets(VkDevice device, VkDescriptorPool pool, uint32_t n, const VkDescriptorSet *sets)
{
    dl_state *s = st();
    if (__atomic_load_n(&s->nsets, __ATOMIC_RELAXED)) {
        pthread_mutex_lock(&s->lock);
        for (uint32_t i = 0; i < n; i++) {
            if (!sets[i]) continue;
            for (dl_set **ep = &s->sets[hb((uint64_t)sets[i])]; *ep; ep = &(*ep)->next)
                if ((*ep)->set == sets[i]) {
                    drop_set(s, ep);
                    break;
                }
        }
        pthread_mutex_unlock(&s->lock);
    }
    return s->free_sets(device, pool, n, sets);
}

static VKAPI_ATTR void VKAPI_CALL d_UpdateDescriptorSets(VkDevice device, uint32_t nw, const VkWriteDescriptorSet *w, uint32_t nc,
                                                         const VkCopyDescriptorSet *cp)
{
    dl_state *s = st();
    if (!__atomic_load_n(&s->nsets, __ATOMIC_RELAXED)) {
        s->update(device, nw, w, nc, cp);
        return;
    }
    /* vkd3d copies descriptors all the time: one lock for the call, and copies of the arrays only
     * when something reaches past a cut binding. */
    VkWriteDescriptorSet *w2 = NULL;
    VkCopyDescriptorSet *c2 = NULL;
    uint32_t kw = 0, kc = 0;
    pthread_mutex_lock(&s->lock);
    for (uint32_t i = 0; i < nw; i++) {
        uint32_t fit = room(s, w[i].dstSet, w[i].dstBinding, w[i].dstArrayElement);
        if (!w2 && fit >= w[i].descriptorCount) continue;
        if (!w2) {
            if (!(w2 = malloc(nw * sizeof(*w2)))) break;
            memcpy(w2, w, i * sizeof(*w2));
            kw = i;
        }
        if (fit < w[i].descriptorCount) {
            tell_cut(&told_write, "vkUpdateDescriptorSets", w[i].dstBinding, w[i].dstArrayElement, w[i].descriptorCount, fit);
            if (!fit) continue;
        }
        w2[kw] = w[i];
        if (fit < w2[kw].descriptorCount) w2[kw].descriptorCount = fit;
        kw++;
    }
    for (uint32_t i = 0; i < nc; i++) {
        uint32_t fs = room(s, cp[i].srcSet, cp[i].srcBinding, cp[i].srcArrayElement);
        uint32_t fd = room(s, cp[i].dstSet, cp[i].dstBinding, cp[i].dstArrayElement);
        uint32_t fit = fs < fd ? fs : fd;
        if (!c2 && fit >= cp[i].descriptorCount) continue;
        if (!c2) {
            if (!(c2 = malloc(nc * sizeof(*c2)))) break;
            memcpy(c2, cp, i * sizeof(*c2));
            kc = i;
        }
        if (fit < cp[i].descriptorCount) {
            tell_cut(&told_write, "vkUpdateDescriptorSets (copy)", cp[i].dstBinding, cp[i].dstArrayElement, cp[i].descriptorCount, fit);
            if (!fit) continue;
        }
        c2[kc] = cp[i];
        if (fit < c2[kc].descriptorCount) c2[kc].descriptorCount = fit;
        kc++;
    }
    pthread_mutex_unlock(&s->lock);
    if (w2 || c2) {
        if (!w2) kw = nw;
        if (!c2) kc = nc;
        if (kw || kc) s->update(device, kw, w2 ? w2 : w, kc, c2 ? c2 : cp);
    } else {
        s->update(device, nw, w, nc, cp);
    }
    free(w2);
    free(c2);
}

/* Template entries past a cut binding are shortened or dropped once, here, not at every update. */
static VKAPI_ATTR VkResult VKAPI_CALL d_CreateDescriptorUpdateTemplate(VkDevice device, const VkDescriptorUpdateTemplateCreateInfo *ci,
                                                                       const VkAllocationCallbacks *a, VkDescriptorUpdateTemplate *out)
{
    dl_state *s = st();
    if (ci->templateType != VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET || !__atomic_load_n(&s->nlayouts, __ATOMIC_RELAXED))
        return s->create_tmpl(device, ci, a, out);
    VkDescriptorUpdateTemplateEntry *e = malloc((ci->descriptorUpdateEntryCount + 1) * sizeof(*e));
    if (!e) return VK_ERROR_OUT_OF_HOST_MEMORY;
    uint32_t k = 0;
    int cut = 0;
    pthread_mutex_lock(&s->lock);
    dl_layout *r = find_layout(s, ci->descriptorSetLayout);
    for (uint32_t i = 0; i < ci->descriptorUpdateEntryCount; i++) {
        const VkDescriptorUpdateTemplateEntry *x = &ci->pDescriptorUpdateEntries[i];
        uint32_t fit = room_layout(r, x->dstBinding, x->dstArrayElement);
        if (fit < x->descriptorCount) {
            cut = 1;
            tell_cut(&told_tmpl, "descriptor update template", x->dstBinding, x->dstArrayElement, x->descriptorCount, fit);
            if (!fit) continue;
        }
        e[k] = *x;
        if (fit < e[k].descriptorCount) e[k].descriptorCount = fit;
        k++;
    }
    pthread_mutex_unlock(&s->lock);
    VkResult res;
    if (cut && k) {
        VkDescriptorUpdateTemplateCreateInfo c2 = *ci;
        c2.descriptorUpdateEntryCount = k;
        c2.pDescriptorUpdateEntries = e;
        res = s->create_tmpl(device, &c2, a, out);
    } else {
        res = s->create_tmpl(device, ci, a, out);
    }
    free(e);
    return res;
}

/* ------------------------------------------------------------------ install */

void vkb_emu_desc_install(vkb_srv_table *dev)
{
    if (!(dev->emu->flags & VKB_EMU_DESC_LIMITS) || dev->id >= 4096) return;
    vkb_dispatch *dt = &dev->dt;
    if (!dt->vkCreateDescriptorSetLayout || !dt->vkDestroyDescriptorSetLayout || !dt->vkCreateDescriptorPool ||
        !dt->vkDestroyDescriptorPool || !dt->vkResetDescriptorPool || !dt->vkAllocateDescriptorSets || !dt->vkFreeDescriptorSets ||
        !dt->vkUpdateDescriptorSets)
        return;
    dl_state *s = calloc(1, sizeof(*s));
    if (!s) return;
    pthread_mutex_init(&s->lock, NULL);
    vkb_emu_desc_limits_get(&dev->real, dev->physical, s->limit);
    const char *cut = vkb_srv_env("VKBRIDGE_DESC_CUT"); /* <cache dir>/env, or the app's environment */
    s->always_cut = cut && *cut && strcmp(cut, "0");
    s->create_dsl = dt->vkCreateDescriptorSetLayout;
    s->destroy_dsl = dt->vkDestroyDescriptorSetLayout;
    s->dsl_support = dt->vkGetDescriptorSetLayoutSupport;
    s->create_pool = dt->vkCreateDescriptorPool;
    s->destroy_pool = dt->vkDestroyDescriptorPool;
    s->reset_pool = dt->vkResetDescriptorPool;
    s->allocate = dt->vkAllocateDescriptorSets;
    s->free_sets = dt->vkFreeDescriptorSets;
    s->update = dt->vkUpdateDescriptorSets;
    s->create_tmpl = dt->vkCreateDescriptorUpdateTemplate;
    dt->vkCreateDescriptorSetLayout = d_CreateDescriptorSetLayout;
    dt->vkDestroyDescriptorSetLayout = d_DestroyDescriptorSetLayout;
    if (s->dsl_support) dt->vkGetDescriptorSetLayoutSupport = d_GetDescriptorSetLayoutSupport;
    dt->vkCreateDescriptorPool = d_CreateDescriptorPool;
    dt->vkDestroyDescriptorPool = d_DestroyDescriptorPool;
    dt->vkResetDescriptorPool = d_ResetDescriptorPool;
    dt->vkAllocateDescriptorSets = d_AllocateDescriptorSets;
    dt->vkFreeDescriptorSets = d_FreeDescriptorSets;
    dt->vkUpdateDescriptorSets = d_UpdateDescriptorSets;
    if (s->create_tmpl) dt->vkCreateDescriptorUpdateTemplate = d_CreateDescriptorUpdateTemplate;
    states[dev->id] = s;
    VKB_INFO("device %u: descriptor limits emulated (sampled/storage image/storage buffer: %u/%u/%u, after bind %u/%u/%u)", dev->id,
             s->limit[0][0], s->limit[0][1], s->limit[0][2], s->limit[1][0], s->limit[1][1], s->limit[1][2]);
}

void vkb_emu_desc_uninstall(vkb_srv_table *dev)
{
    if (dev->id >= 4096 || !states[dev->id]) return;
    dl_state *s = states[dev->id];
    vkb_dispatch *dt = &dev->dt;
    dt->vkCreateDescriptorSetLayout = s->create_dsl;
    dt->vkDestroyDescriptorSetLayout = s->destroy_dsl;
    dt->vkGetDescriptorSetLayoutSupport = s->dsl_support;
    dt->vkCreateDescriptorPool = s->create_pool;
    dt->vkDestroyDescriptorPool = s->destroy_pool;
    dt->vkResetDescriptorPool = s->reset_pool;
    dt->vkAllocateDescriptorSets = s->allocate;
    dt->vkFreeDescriptorSets = s->free_sets;
    dt->vkUpdateDescriptorSets = s->update;
    dt->vkCreateDescriptorUpdateTemplate = s->create_tmpl;
    for (int i = 0; i < DL_BUCKETS; i++) {
        while (s->sets[i]) drop_set(s, &s->sets[i]);
        while (s->layouts[i]) {
            dl_layout *r = s->layouts[i];
            s->layouts[i] = r->next;
            unref_layout(r);
        }
    }
    while (s->pools) {
        dl_pool *p = s->pools;
        s->pools = p->next;
        free(p);
    }
    pthread_mutex_destroy(&s->lock);
    free(s);
    states[dev->id] = NULL;
}
