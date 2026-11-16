/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * pcache_mem_layer: a Vulkan layer for the SERVER's side of host tests that makes a pipeline cache hold real
 * memory, as Mali's does (~17 KB a pipeline in a player log, 'pipeline caches 399142K' for 22964 pipelines), where
 * lavapipe's keeps none (its cache data is one 4130-byte header whatever it holds). host/precachemem_test.sh
 * loads it into the server only (VK_INSTANCE_LAYERS=VK_LAYER_VKB_pcache_mem), so the server's heap (mallinfo,
 * its 'mem:' lines) shows what its pipeline caches cost, for an unchanged server binary of any build.
 *
 * Model (a driver cache keyed by pipeline, entries copied, never shared):
 *   - a pipeline created with a cache adds an entry to it, keyed by the pipeline (stage module handle +
 *     specialization data + entry name); an entry is PCMEM_ENTRY bytes (default 16384, its 8-byte key included)
 *     of heap, allocated and touched;
 *   - vkGetPipelineCacheData = the driver's header (its first 32 bytes: vendor, device, cache UUID), PCMEM_PAD
 *     bytes (default 0), every entry (key + bytes), a trailer; vkCreatePipelineCache with such data makes the same
 *     entries again (each a new allocation) and hands the driver its header alone (a cache with nothing in it);
 *   - vkMergePipelineCaches copies into dst the entries of each src that dst lacks; vkDestroyPipelineCache frees.
 * PCMEM_ENTRY=88 PCMEM_PAD=888: RADV's sizes (944 + 88 a pipeline), for host/pcache_test.sh's byte-counting steps.
 */
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EXPORT __attribute__((visibility("default")))
#define MAGIC 0x564b424d454d4331ull /* "VKBMEMC1" */

typedef struct entry {
    uint64_t key;
    uint8_t *bytes;
} entry;

typedef struct blob {
    VkPipelineCache cache;
    entry *e;
    size_t n, cap;
    struct blob *next;
} blob;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static blob *blobs;
static size_t entry_bytes, pad_bytes;
#define HDR 32
static PFN_vkGetInstanceProcAddr next_gipa;
static PFN_vkGetDeviceProcAddr next_gdpa;
static PFN_vkCreatePipelineCache next_create_cache;
static PFN_vkDestroyPipelineCache next_destroy_cache;
static PFN_vkGetPipelineCacheData next_get_data;
static PFN_vkMergePipelineCaches next_merge;
static PFN_vkCreateGraphicsPipelines next_gfx;
static PFN_vkCreateComputePipelines next_compute;
static PFN_vkDestroyDevice next_destroy_device;

static uint64_t fnv(uint64_t h, const void *p, size_t n)
{
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 0x100000001b3ull;
    return h;
}

static uint64_t stage_key(uint64_t h, const VkPipelineShaderStageCreateInfo *s)
{
    uint64_t m = (uint64_t)s->module;
    h = fnv(h, &m, sizeof(m));
    h = fnv(h, &s->stage, sizeof(s->stage));
    if (s->pName) h = fnv(h, s->pName, strlen(s->pName));
    if (s->pSpecializationInfo && s->pSpecializationInfo->pData) h = fnv(h, s->pSpecializationInfo->pData, s->pSpecializationInfo->dataSize);
    return h;
}

/* with lock held */
static blob *find(VkPipelineCache c)
{
    for (blob *b = blobs; b; b = b->next)
        if (b->cache == c) return b;
    return NULL;
}

static int has(const blob *b, uint64_t key)
{
    for (size_t i = 0; i < b->n; i++)
        if (b->e[i].key == key) return 1;
    return 0;
}

static void add(blob *b, uint64_t key, const uint8_t *from)
{
    if (has(b, key)) return;
    if (b->n == b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 64;
        entry *g = realloc(b->e, cap * sizeof(*g));
        if (!g) return;
        b->e = g;
        b->cap = cap;
    }
    uint8_t *bytes = malloc(entry_bytes);
    if (!bytes) return;
    if (from) memcpy(bytes, from, entry_bytes);
    else memset(bytes, (int)(key & 0xff), entry_bytes); /* touched: resident, as the driver's are */
    b->e[b->n].key = key;
    b->e[b->n].bytes = bytes;
    b->n++;
}

static void kb_env(void)
{
    if (entry_bytes) return;
    const char *v = getenv("PCMEM_ENTRY"), *p = getenv("PCMEM_PAD");
    long e = v && *v ? strtol(v, NULL, 10) : 16384;
    entry_bytes = (size_t)(e > 16 ? e : 16) - 8;
    pad_bytes = p && *p ? (size_t)strtol(p, NULL, 10) : 0;
}

static VKAPI_ATTR VkResult VKAPI_CALL l_CreatePipelineCache(VkDevice d, const VkPipelineCacheCreateInfo *ci, const VkAllocationCallbacks *a,
                                                            VkPipelineCache *out)
{
    VkPipelineCacheCreateInfo real = *ci;
    const uint8_t *data = ci->pInitialData;
    size_t nent = 0;
    const uint8_t *ents = NULL;
    uint64_t tr[2]; /* entries, magic */
    if (data && ci->initialDataSize >= HDR + sizeof(tr)) {
        memcpy(tr, data + ci->initialDataSize - sizeof(tr), sizeof(tr));
        if (tr[1] == MAGIC && HDR + pad_bytes + tr[0] * (8 + entry_bytes) + sizeof(tr) == ci->initialDataSize) {
            nent = tr[0];
            ents = data + HDR + pad_bytes;
        }
        real.initialDataSize = HDR; /* the driver's header, nothing in it */
    } else {
        real.initialDataSize = 0;
        real.pInitialData = NULL;
    }
    VkResult r = next_create_cache(d, &real, a, out);
    if (r != VK_SUCCESS) return r;
    blob *b = calloc(1, sizeof(*b));
    if (!b) return r;
    b->cache = *out;
    for (size_t i = 0; i < nent; i++) {
        uint64_t key;
        memcpy(&key, ents + i * (8 + entry_bytes), 8);
        add(b, key, ents + i * (8 + entry_bytes) + 8);
    }
    pthread_mutex_lock(&lock);
    b->next = blobs;
    blobs = b;
    pthread_mutex_unlock(&lock);
    return r;
}

static VKAPI_ATTR void VKAPI_CALL l_DestroyPipelineCache(VkDevice d, VkPipelineCache c, const VkAllocationCallbacks *a)
{
    pthread_mutex_lock(&lock);
    for (blob **pp = &blobs; *pp; pp = &(*pp)->next) {
        if ((*pp)->cache != c) continue;
        blob *b = *pp;
        *pp = b->next;
        for (size_t i = 0; i < b->n; i++) free(b->e[i].bytes);
        free(b->e);
        free(b);
        break;
    }
    pthread_mutex_unlock(&lock);
    next_destroy_cache(d, c, a);
}

static VKAPI_ATTR VkResult VKAPI_CALL l_GetPipelineCacheData(VkDevice d, VkPipelineCache c, size_t *size, void *out)
{
    size_t rs = 0;
    VkResult r = next_get_data(d, c, &rs, NULL);
    if (r != VK_SUCCESS) return r;
    if (rs < HDR) return VK_ERROR_INITIALIZATION_FAILED;
    pthread_mutex_lock(&lock);
    blob *b = find(c);
    size_t n = b ? b->n : 0, total = HDR + pad_bytes + n * (8 + entry_bytes) + 2 * sizeof(uint64_t);
    if (!out) {
        *size = total;
        pthread_mutex_unlock(&lock);
        return VK_SUCCESS;
    }
    if (*size < total) {
        *size = 0; /* all or nothing (no partial entries) */
        pthread_mutex_unlock(&lock);
        return VK_INCOMPLETE;
    }
    uint8_t hdr[HDR];
    size_t got = HDR;
    r = next_get_data(d, c, &got, hdr); /* the first 32 bytes: VK_INCOMPLETE */
    if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || got != HDR) {
        pthread_mutex_unlock(&lock);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    uint8_t *p = out;
    memcpy(p, hdr, HDR);
    memset(p + HDR, 0, pad_bytes);
    p += HDR + pad_bytes;
    for (size_t i = 0; i < n; i++) {
        memcpy(p, &b->e[i].key, 8);
        memcpy(p + 8, b->e[i].bytes, entry_bytes);
        p += 8 + entry_bytes;
    }
    uint64_t tr[2] = {n, MAGIC};
    memcpy(p, tr, sizeof(tr));
    pthread_mutex_unlock(&lock);
    *size = total;
    return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL l_MergePipelineCaches(VkDevice d, VkPipelineCache dst, uint32_t n, const VkPipelineCache *src)
{
    VkResult r = next_merge(d, dst, n, src);
    if (r != VK_SUCCESS) return r;
    pthread_mutex_lock(&lock);
    blob *db = find(dst);
    for (uint32_t i = 0; db && i < n; i++) {
        blob *sb = find(src[i]);
        for (size_t k = 0; sb && k < sb->n; k++) add(db, sb->e[k].key, sb->e[k].bytes);
    }
    pthread_mutex_unlock(&lock);
    return r;
}

static void note(VkPipelineCache c, uint64_t key)
{
    if (!c) return;
    pthread_mutex_lock(&lock);
    blob *b = find(c);
    if (b) add(b, key, NULL);
    pthread_mutex_unlock(&lock);
}

static VKAPI_ATTR VkResult VKAPI_CALL l_CreateGraphicsPipelines(VkDevice d, VkPipelineCache c, uint32_t n, const VkGraphicsPipelineCreateInfo *ci,
                                                                const VkAllocationCallbacks *a, VkPipeline *out)
{
    VkResult r = next_gfx(d, c, n, ci, a, out);
    for (uint32_t i = 0; r == VK_SUCCESS && i < n; i++) {
        uint64_t h = 0xcbf29ce484222325ull, lay = (uint64_t)ci[i].layout, rp = (uint64_t)ci[i].renderPass;
        for (uint32_t s = 0; s < ci[i].stageCount; s++) h = stage_key(h, &ci[i].pStages[s]);
        h = fnv(fnv(fnv(h, &lay, 8), &rp, 8), &ci[i].subpass, 4);
        if (ci[i].pRasterizationState) h = fnv(h, &ci[i].pRasterizationState->polygonMode, 6 * 4);
        if (ci[i].pColorBlendState && ci[i].pColorBlendState->pAttachments)
            h = fnv(h, ci[i].pColorBlendState->pAttachments, ci[i].pColorBlendState->attachmentCount * sizeof(*ci[i].pColorBlendState->pAttachments));
        if (ci[i].pVertexInputState && ci[i].pVertexInputState->pVertexAttributeDescriptions)
            h = fnv(h, ci[i].pVertexInputState->pVertexAttributeDescriptions,
                    ci[i].pVertexInputState->vertexAttributeDescriptionCount * sizeof(*ci[i].pVertexInputState->pVertexAttributeDescriptions));
        note(c, h);
    }
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL l_CreateComputePipelines(VkDevice d, VkPipelineCache c, uint32_t n, const VkComputePipelineCreateInfo *ci,
                                                               const VkAllocationCallbacks *a, VkPipeline *out)
{
    VkResult r = next_compute(d, c, n, ci, a, out);
    for (uint32_t i = 0; r == VK_SUCCESS && i < n; i++) note(c, stage_key(0x84222325cbf29ce4ull, &ci[i].stage));
    return r;
}

static VKAPI_ATTR void VKAPI_CALL l_DestroyDevice(VkDevice d, const VkAllocationCallbacks *a) { next_destroy_device(d, a); }

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL l_GetDeviceProcAddr(VkDevice d, const char *name);

static VKAPI_ATTR VkResult VKAPI_CALL l_CreateDevice(VkPhysicalDevice pd, const VkDeviceCreateInfo *ci, const VkAllocationCallbacks *a, VkDevice *out)
{
    VkLayerDeviceCreateInfo *lci = (VkLayerDeviceCreateInfo *)ci->pNext;
    while (lci && !(lci->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO && lci->function == VK_LAYER_LINK_INFO))
        lci = (VkLayerDeviceCreateInfo *)lci->pNext;
    if (!lci) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetInstanceProcAddr gipa = lci->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr gdpa = lci->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    lci->u.pLayerInfo = lci->u.pLayerInfo->pNext;
    PFN_vkCreateDevice create = (PFN_vkCreateDevice)gipa(VK_NULL_HANDLE, "vkCreateDevice");
    VkResult r = create(pd, ci, a, out);
    if (r != VK_SUCCESS) return r;
    next_gdpa = gdpa; /* one driver below: the same entry points for every device */
#define G(n) next_##n = (PFN_vk##n)gdpa(*out, "vk" #n)
    next_create_cache = (PFN_vkCreatePipelineCache)gdpa(*out, "vkCreatePipelineCache");
    next_destroy_cache = (PFN_vkDestroyPipelineCache)gdpa(*out, "vkDestroyPipelineCache");
    next_get_data = (PFN_vkGetPipelineCacheData)gdpa(*out, "vkGetPipelineCacheData");
    next_merge = (PFN_vkMergePipelineCaches)gdpa(*out, "vkMergePipelineCaches");
    next_gfx = (PFN_vkCreateGraphicsPipelines)gdpa(*out, "vkCreateGraphicsPipelines");
    next_compute = (PFN_vkCreateComputePipelines)gdpa(*out, "vkCreateComputePipelines");
    next_destroy_device = (PFN_vkDestroyDevice)gdpa(*out, "vkDestroyDevice");
#undef G
    kb_env();
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL l_CreateInstance(const VkInstanceCreateInfo *ci, const VkAllocationCallbacks *a, VkInstance *out)
{
    VkLayerInstanceCreateInfo *lci = (VkLayerInstanceCreateInfo *)ci->pNext;
    while (lci && !(lci->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO && lci->function == VK_LAYER_LINK_INFO))
        lci = (VkLayerInstanceCreateInfo *)lci->pNext;
    if (!lci) return VK_ERROR_INITIALIZATION_FAILED;
    next_gipa = lci->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    lci->u.pLayerInfo = lci->u.pLayerInfo->pNext;
    PFN_vkCreateInstance create = (PFN_vkCreateInstance)next_gipa(VK_NULL_HANDLE, "vkCreateInstance");
    return create(ci, a, out);
}

static PFN_vkVoidFunction own(const char *name)
{
#define O(n) if (!strcmp(name, "vk" #n)) return (PFN_vkVoidFunction)l_##n
    O(CreatePipelineCache);
    O(DestroyPipelineCache);
    O(GetPipelineCacheData);
    O(MergePipelineCaches);
    O(CreateGraphicsPipelines);
    O(CreateComputePipelines);
    O(DestroyDevice);
#undef O
    return NULL;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL l_GetDeviceProcAddr(VkDevice d, const char *name)
{
    if (!strcmp(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)l_GetDeviceProcAddr;
    PFN_vkVoidFunction f = own(name);
    return f ? f : next_gdpa(d, name);
}

EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkb_pcmem_GetDeviceProcAddr(VkDevice d, const char *name) { return l_GetDeviceProcAddr(d, name); }

EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkb_pcmem_GetInstanceProcAddr(VkInstance inst, const char *name)
{
    if (!strcmp(name, "vkCreateInstance")) return (PFN_vkVoidFunction)l_CreateInstance;
    if (!strcmp(name, "vkCreateDevice")) return (PFN_vkVoidFunction)l_CreateDevice;
    if (!strcmp(name, "vkGetInstanceProcAddr")) return (PFN_vkVoidFunction)vkb_pcmem_GetInstanceProcAddr;
    if (!strcmp(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)l_GetDeviceProcAddr;
    PFN_vkVoidFunction f = own(name);
    if (f) return f;
    return next_gipa ? next_gipa(inst, name) : NULL;
}
