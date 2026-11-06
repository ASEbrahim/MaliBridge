/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * What a process leaves alive when it ends without destroying its device. Wine/DXVK games end that
 * way as a rule; the server's cleanup (server_conn.c vkb_proc_cleanup) frees their memory and then
 * destroys the device with the rest still alive in it. Destroying a device with live objects is
 * not allowed, and drivers do not free those objects for it: their host memory stays in the
 * server, which lives for the whole session. Tab S11, G925, test12: the server's heap
 * 195 -> 617 -> 1037 -> 1446 MB after three Skyrim SE runs in one session, never back; the session
 * ended with Android's low-memory kill. Lavapipe keeps the same (host/exitheap_test.sh).
 *
 * Here the objects whose driver memory is large - descriptor pools (their sets go with them),
 * command pools (their command buffers too), pipelines, shader modules, pipeline layouts,
 * descriptor set layouts, pipeline caches, query pools - are noted as the driver makes them, at
 * the bottom of the device's dispatch (under every layer: the handles are the driver's, the
 * layers' own objects included), and forgotten as the driver destroys them. When a process's
 * cleanup destroys its device, whatever is still noted is destroyed first, through the driver's
 * own entry points, after the device is idle and every layer has let go of its own objects.
 * A device the app destroys itself is left as before (the notes are dropped, nothing destroyed).
 * VKBRIDGE_EXIT_CHILDREN=0: not installed (the old cleanup).
 */
#define _GNU_SOURCE
#include "vkb_server.h"

#include <stdlib.h>
#include <string.h>

enum { K_CMDPOOL, K_PIPELINE, K_DESCPOOL, K_PLAYOUT, K_DSLAYOUT, K_MODULE, K_PCACHE, K_QUERYPOOL, K_N };
static const char *const k_name[K_N] = {"command pool", "pipeline", "descriptor pool", "pipeline layout", "set layout",
                                        "shader module", "pipeline cache", "query pool"};

typedef struct { /* open addressing, linear probing, backward-shift deletion; 0 = empty slot */
    uint64_t *slot;
    size_t cap, n;
} hset;

typedef struct vkb_children {
    VkDevice dev;
    hset set[K_N];
    /* the driver's entry points (what the device's dispatch had before) */
    PFN_vkCreateCommandPool create_cmdpool;
    PFN_vkDestroyCommandPool destroy_cmdpool;
    PFN_vkCreateGraphicsPipelines create_gfx;
    PFN_vkCreateComputePipelines create_comp;
    PFN_vkDestroyPipeline destroy_pipeline;
    PFN_vkCreateDescriptorPool create_descpool;
    PFN_vkDestroyDescriptorPool destroy_descpool;
    PFN_vkCreatePipelineLayout create_playout;
    PFN_vkDestroyPipelineLayout destroy_playout;
    PFN_vkCreateDescriptorSetLayout create_dslayout;
    PFN_vkDestroyDescriptorSetLayout destroy_dslayout;
    PFN_vkCreateShaderModule create_module;
    PFN_vkDestroyShaderModule destroy_module;
    PFN_vkCreatePipelineCache create_pcache;
    PFN_vkDestroyPipelineCache destroy_pcache;
    PFN_vkCreateQueryPool create_querypool;
    PFN_vkDestroyQueryPool destroy_querypool;
    struct vkb_children *next;
} vkb_children;

/* One lock for all devices: creates and destroys of these objects are not per-draw calls. */
static pthread_mutex_t ch_lock = PTHREAD_MUTEX_INITIALIZER;
static vkb_children *ch_list;

static size_t h_pos(const hset *h, uint64_t v)
{
    uint64_t x = v * 0x9E3779B97F4A7C15ull;
    return (size_t)(x >> 17) & (h->cap - 1);
}

static int h_add(hset *h, uint64_t v);

static int h_grow(hset *h)
{
    hset g = {calloc(h->cap ? h->cap * 2 : 256, sizeof(uint64_t)), h->cap ? h->cap * 2 : 256, 0};
    if (!g.slot) return -1;
    for (size_t i = 0; i < h->cap; i++)
        if (h->slot[i]) h_add(&g, h->slot[i]);
    free(h->slot);
    *h = g;
    return 0;
}

static int h_add(hset *h, uint64_t v)
{
    if (!v) return 0;
    if ((h->n + 1) * 4 > h->cap * 3 && h_grow(h)) return -1;
    size_t i = h_pos(h, v);
    while (h->slot[i]) {
        if (h->slot[i] == v) return 0;
        i = (i + 1) & (h->cap - 1);
    }
    h->slot[i] = v;
    h->n++;
    return 0;
}

static void h_del(hset *h, uint64_t v)
{
    if (!v || !h->cap) return;
    size_t i = h_pos(h, v);
    while (h->slot[i] && h->slot[i] != v) i = (i + 1) & (h->cap - 1);
    if (!h->slot[i]) return;
    h->slot[i] = 0;
    h->n--;
    for (size_t j = (i + 1) & (h->cap - 1); h->slot[j]; j = (j + 1) & (h->cap - 1)) {
        size_t home = h_pos(h, h->slot[j]);
        /* move slot j back to i when its home is not in (i, j] (cyclically) */
        if ((j > i && (home <= i || home > j)) || (j < i && home <= i && home > j)) {
            h->slot[i] = h->slot[j];
            h->slot[j] = 0;
            i = j;
        }
    }
}

static vkb_children *find(VkDevice dev) /* under ch_lock */
{
    for (vkb_children *c = ch_list; c; c = c->next)
        if (c->dev == dev) return c;
    return NULL;
}

static void note(VkDevice dev, int k, uint64_t h, int add)
{
    pthread_mutex_lock(&ch_lock);
    vkb_children *c = find(dev);
    if (c) {
        if (add)
            h_add(&c->set[k], h);
        else
            h_del(&c->set[k], h);
    }
    pthread_mutex_unlock(&ch_lock);
}

static vkb_children *get(VkDevice dev)
{
    pthread_mutex_lock(&ch_lock);
    vkb_children *c = find(dev);
    pthread_mutex_unlock(&ch_lock);
    return c; /* lives until its device's table is destroyed, after the device's last call */
}

#define H(x) ((uint64_t)(uintptr_t)(x))

static VKAPI_ATTR VkResult VKAPI_CALL ch_CreateCommandPool(VkDevice d, const VkCommandPoolCreateInfo *ci, const VkAllocationCallbacks *a, VkCommandPool *p)
{
    VkResult r = get(d)->create_cmdpool(d, ci, a, p);
    if (r == VK_SUCCESS) note(d, K_CMDPOOL, H(*p), 1);
    return r;
}
static VKAPI_ATTR void VKAPI_CALL ch_DestroyCommandPool(VkDevice d, VkCommandPool p, const VkAllocationCallbacks *a)
{
    note(d, K_CMDPOOL, H(p), 0);
    get(d)->destroy_cmdpool(d, p, a);
}

static void note_pipelines(VkDevice d, uint32_t n, const VkPipeline *p)
{
    if (!p) return;
    pthread_mutex_lock(&ch_lock);
    vkb_children *c = find(d);
    for (uint32_t i = 0; c && i < n; i++) h_add(&c->set[K_PIPELINE], H(p[i])); /* failed ones are VK_NULL_HANDLE */
    pthread_mutex_unlock(&ch_lock);
}
static VKAPI_ATTR VkResult VKAPI_CALL ch_CreateGraphicsPipelines(VkDevice d, VkPipelineCache pc, uint32_t n, const VkGraphicsPipelineCreateInfo *ci,
                                                                  const VkAllocationCallbacks *a, VkPipeline *p)
{
    VkResult r = get(d)->create_gfx(d, pc, n, ci, a, p);
    note_pipelines(d, n, p);
    return r;
}
static VKAPI_ATTR VkResult VKAPI_CALL ch_CreateComputePipelines(VkDevice d, VkPipelineCache pc, uint32_t n, const VkComputePipelineCreateInfo *ci,
                                                                 const VkAllocationCallbacks *a, VkPipeline *p)
{
    VkResult r = get(d)->create_comp(d, pc, n, ci, a, p);
    note_pipelines(d, n, p);
    return r;
}
static VKAPI_ATTR void VKAPI_CALL ch_DestroyPipeline(VkDevice d, VkPipeline p, const VkAllocationCallbacks *a)
{
    note(d, K_PIPELINE, H(p), 0);
    get(d)->destroy_pipeline(d, p, a);
}

#define PAIR(K, Obj, CI, cf, df)                                                                                               \
    static VKAPI_ATTR VkResult VKAPI_CALL ch_Create##Obj(VkDevice d, const CI *ci, const VkAllocationCallbacks *a, Vk##Obj *o) \
    {                                                                                                                          \
        VkResult r = get(d)->cf(d, ci, a, o);                                                                                  \
        if (r == VK_SUCCESS) note(d, K, H(*o), 1);                                                                             \
        return r;                                                                                                              \
    }                                                                                                                          \
    static VKAPI_ATTR void VKAPI_CALL ch_Destroy##Obj(VkDevice d, Vk##Obj o, const VkAllocationCallbacks *a)                   \
    {                                                                                                                          \
        note(d, K, H(o), 0);                                                                                                   \
        get(d)->df(d, o, a);                                                                                                   \
    }
PAIR(K_DESCPOOL, DescriptorPool, VkDescriptorPoolCreateInfo, create_descpool, destroy_descpool)
PAIR(K_PLAYOUT, PipelineLayout, VkPipelineLayoutCreateInfo, create_playout, destroy_playout)
PAIR(K_DSLAYOUT, DescriptorSetLayout, VkDescriptorSetLayoutCreateInfo, create_dslayout, destroy_dslayout)
PAIR(K_MODULE, ShaderModule, VkShaderModuleCreateInfo, create_module, destroy_module)
PAIR(K_PCACHE, PipelineCache, VkPipelineCacheCreateInfo, create_pcache, destroy_pcache)
PAIR(K_QUERYPOOL, QueryPool, VkQueryPoolCreateInfo, create_querypool, destroy_querypool)
#undef PAIR

/* Both tables: the layers above take their next from t->dt, and some call t->real directly. */
#define SWAP(field, saved, wrapper)                                         \
    do {                                                                    \
        c->saved = t->real.field;                                           \
        t->real.field = wrapper;                                            \
    } while (0)

void vkb_children_device_init(vkb_srv_table *t)
{
    const char *e = vkb_srv_env("VKBRIDGE_EXIT_CHILDREN");
    if (e && *e == '0') return;
    vkb_dispatch *r = &t->real;
    if (!r->vkCreateCommandPool || !r->vkDestroyCommandPool || !r->vkCreateGraphicsPipelines || !r->vkCreateComputePipelines ||
        !r->vkDestroyPipeline || !r->vkCreateDescriptorPool || !r->vkDestroyDescriptorPool || !r->vkCreatePipelineLayout ||
        !r->vkDestroyPipelineLayout || !r->vkCreateDescriptorSetLayout || !r->vkDestroyDescriptorSetLayout || !r->vkCreateShaderModule ||
        !r->vkDestroyShaderModule || !r->vkCreatePipelineCache || !r->vkDestroyPipelineCache || !r->vkCreateQueryPool ||
        !r->vkDestroyQueryPool)
        return;
    vkb_children *c = calloc(1, sizeof(*c));
    if (!c) return;
    c->dev = t->device;
    SWAP(vkCreateCommandPool, create_cmdpool, ch_CreateCommandPool);
    SWAP(vkDestroyCommandPool, destroy_cmdpool, ch_DestroyCommandPool);
    SWAP(vkCreateGraphicsPipelines, create_gfx, ch_CreateGraphicsPipelines);
    SWAP(vkCreateComputePipelines, create_comp, ch_CreateComputePipelines);
    SWAP(vkDestroyPipeline, destroy_pipeline, ch_DestroyPipeline);
    SWAP(vkCreateDescriptorPool, create_descpool, ch_CreateDescriptorPool);
    SWAP(vkDestroyDescriptorPool, destroy_descpool, ch_DestroyDescriptorPool);
    SWAP(vkCreatePipelineLayout, create_playout, ch_CreatePipelineLayout);
    SWAP(vkDestroyPipelineLayout, destroy_playout, ch_DestroyPipelineLayout);
    SWAP(vkCreateDescriptorSetLayout, create_dslayout, ch_CreateDescriptorSetLayout);
    SWAP(vkDestroyDescriptorSetLayout, destroy_dslayout, ch_DestroyDescriptorSetLayout);
    SWAP(vkCreateShaderModule, create_module, ch_CreateShaderModule);
    SWAP(vkDestroyShaderModule, destroy_module, ch_DestroyShaderModule);
    SWAP(vkCreatePipelineCache, create_pcache, ch_CreatePipelineCache);
    SWAP(vkDestroyPipelineCache, destroy_pcache, ch_DestroyPipelineCache);
    SWAP(vkCreateQueryPool, create_querypool, ch_CreateQueryPool);
    SWAP(vkDestroyQueryPool, destroy_querypool, ch_DestroyQueryPool);
    pthread_mutex_lock(&ch_lock);
    c->next = ch_list;
    ch_list = c;
    pthread_mutex_unlock(&ch_lock);
}
#undef SWAP

/* destroy_left: the process ended without destroying the device (the device is idle, the layers
 * have let go of theirs): what is still noted is destroyed, children before what they were made
 * from. 0: the app destroys the device itself; the notes go, as before nothing is destroyed. */
void vkb_children_device_destroy(vkb_srv_table *t, int destroy_left)
{
    pthread_mutex_lock(&ch_lock);
    vkb_children **pp = &ch_list;
    while (*pp && (*pp)->dev != t->device) pp = &(*pp)->next;
    vkb_children *c = *pp;
    if (c) *pp = c->next;
    pthread_mutex_unlock(&ch_lock);
    if (!c) return;
    unsigned long left[K_N] = {0}, total = 0;
    for (int k = 0; k < K_N; k++) {
        hset *h = &c->set[k];
        for (size_t i = 0; i < h->cap; i++) {
            uint64_t v = h->slot[i];
            if (!v) continue;
            left[k]++;
            if (!destroy_left) continue;
            VkDevice d = c->dev;
            switch (k) {
            case K_CMDPOOL: c->destroy_cmdpool(d, (VkCommandPool)(uintptr_t)v, NULL); break;
            case K_PIPELINE: c->destroy_pipeline(d, (VkPipeline)(uintptr_t)v, NULL); break;
            case K_DESCPOOL: c->destroy_descpool(d, (VkDescriptorPool)(uintptr_t)v, NULL); break;
            case K_PLAYOUT: c->destroy_playout(d, (VkPipelineLayout)(uintptr_t)v, NULL); break;
            case K_DSLAYOUT: c->destroy_dslayout(d, (VkDescriptorSetLayout)(uintptr_t)v, NULL); break;
            case K_MODULE: c->destroy_module(d, (VkShaderModule)(uintptr_t)v, NULL); break;
            case K_PCACHE: c->destroy_pcache(d, (VkPipelineCache)(uintptr_t)v, NULL); break;
            case K_QUERYPOOL: c->destroy_querypool(d, (VkQueryPool)(uintptr_t)v, NULL); break;
            }
        }
        total += left[k];
        free(h->slot);
    }
    /* the driver's entry points back where they were (the table may still be read after this) */
    t->real.vkCreateCommandPool = c->create_cmdpool;
    t->real.vkDestroyCommandPool = c->destroy_cmdpool;
    t->real.vkCreateGraphicsPipelines = c->create_gfx;
    t->real.vkCreateComputePipelines = c->create_comp;
    t->real.vkDestroyPipeline = c->destroy_pipeline;
    t->real.vkCreateDescriptorPool = c->create_descpool;
    t->real.vkDestroyDescriptorPool = c->destroy_descpool;
    t->real.vkCreatePipelineLayout = c->create_playout;
    t->real.vkDestroyPipelineLayout = c->destroy_playout;
    t->real.vkCreateDescriptorSetLayout = c->create_dslayout;
    t->real.vkDestroyDescriptorSetLayout = c->destroy_dslayout;
    t->real.vkCreateShaderModule = c->create_module;
    t->real.vkDestroyShaderModule = c->destroy_module;
    t->real.vkCreatePipelineCache = c->create_pcache;
    t->real.vkDestroyPipelineCache = c->destroy_pcache;
    t->real.vkCreateQueryPool = c->create_querypool;
    t->real.vkDestroyQueryPool = c->destroy_querypool;
    if (destroy_left && total) {
        char line[512];
        int n = 0;
        for (int k = 0; k < K_N; k++)
            if (left[k]) n += snprintf(line + n, n < (int)sizeof(line) ? sizeof(line) - (size_t)n : 0, "%s %lu %s(s)", n ? "," : "", left[k], k_name[k]);
        VKB_INFO("device %u: left alive by its process, destroyed before the device:%s", t->id, line);
    }
    free(c);
}
