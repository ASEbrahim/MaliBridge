/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * Device memory on the client side: the memory-type translation of physdev.c applied to every
 * allocation and requirement, and mapping of shared allocations (server_memory.c explains how
 * they are shared).
 */
#define _GNU_SOURCE
#include "vkb_page.h"
#include "vkb_client.h"
#include "gen/vkb_client_gen.h"
#include "emu.h"
#include "vkb_emu_formats.h"

#include <errno.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/*
 * Buffer requirements by create info. Hades' Vulkan renderer made ~1,000-1,600 vkCreateBuffer and
 * as many vkGetBufferMemoryRequirements2 round trips per 5 s, for buffers created alike. What a
 * buffer needs is a function of its create info (VK_KHR_maintenance4's
 * vkGetDeviceBufferMemoryRequirements rests on that), so the answer for a create info is kept and
 * buffers created with the same one get it without a round trip. Plain create infos only (no
 * pNext, exclusive sharing); the key also has whether the buffer may take shared memory, which
 * changes the memory types reported.
 */
#define RQ_HB 1024
#define RQ_MAX 4096 /* distinct create infos kept; later ones are not cached */

typedef struct rq_entry {
    VkDeviceSize size;
    VkBufferUsageFlags usage;
    VkBufferCreateFlags flags;
    int shareable;
    int have, have_dedicated;     /* answers known: requirements; dedicated (VkMemoryDedicatedRequirements) */
    int owned;                    /* one image's own answers (from its create's reply, rq_image_reqs): freed with the image */
    VkMemoryRequirements req;
    VkBool32 prefers_dedicated, requires_dedicated;
    struct rq_entry *next;
} rq_entry;

/* Images the same way (also maintenance4's: vkGetDeviceImageMemoryRequirements): optimal tiling,
 * no pNext and exclusive sharing once the client has changed the create info, and no BC formats -
 * the server may make one of those transcoded or decoded (VKBRIDGE_BC_TX_BUDGET) from the same
 * create info, at different sizes. Dark Souls III asks for each texture it streams in. */
typedef struct irq_entry {
    rq_entry a;                   /* the answers (its key fields unused) */
    VkImageCreateFlags flags;
    VkImageType type;
    VkFormat format;
    VkExtent3D extent;
    uint32_t mips, layers;
    VkSampleCountFlagBits samples;
    VkImageUsageFlags usage;
    struct irq_entry *next;
} irq_entry;

typedef struct vkb_rq_cache {
    pthread_mutex_t lock;
    vkb_map buffers;              /* VkBuffer -> rq_entry* (entries live as long as the device) */
    rq_entry *buckets[RQ_HB];
    uint32_t count;
    vkb_map images;               /* VkImage -> rq_entry* (the irq_entry's) */
    irq_entry *ibuckets[RQ_HB];
    uint32_t icount;
    /* the dedicated-allocation hint (ded_hint below) */
    int ded_clear;                /* clear prefersDedicatedAllocation where safe */
    int ded_imitate;              /* VKBRIDGE_TEST_PREFER_DEDICATED: report it set, as Mali does (host tests) */
    vkb_map ded_keep;             /* VkBuffer/VkImage the app made for external memory -> (void*)1 */
    uint64_t ded_cleared;         /* answers changed (one log line at the first) */
} vkb_rq_cache;

/*
 * The dedicated-allocation hint. Mali answers prefersDedicatedAllocation = VK_TRUE for most
 * images, and DXVK follows it: one VkDeviceMemory per texture. On kbase every allocation is a VMA
 * of its own, and through the bridge each one is also an allocation the one server process holds
 * for every client (and, when shared, an fd and a mapping on both sides). Julius Pinsker's GTA V
 * measurements (leegao/bionic-vulkan-wrapper PR #228, MIT): 20,651 of 20,665 vkAllocateMemory calls
 * were dedicated images and the process's mappings reached 24-35k on the way to vm.max_map_count
 * (65,530); with the preference cleared DXVK sub-allocates from its chunks and they stayed at
 * 8-15k. The idea is that PR's; the code here is the bridge's own and narrower:
 *   - requiresDedicatedAllocation is never touched, nor the preference when it is set;
 *   - resources the app made for external memory keep the preference (an image or buffer created
 *     with VkExternalMemory*CreateInfo, an image with DRM format modifier tiling or bound to a
 *     swapchain): they may be exported or imported, where dedicated memory is what drivers and
 *     the other side expect. The handle types the bridge adds for its own shared memory do not
 *     count: the app never asked for those;
 *   - resources of DED_KEEP_MIN bytes or more keep it (large render targets: few allocations, and
 *     a sub-allocator gains nothing from them).
 * The hint is only a hint: memory bound to a sub-allocated range is valid either way.
 * VKBRIDGE_DEDICATED_HINT=1 clears it, =0 passes the driver's answer through; default: 1 on Arm's
 * driver (Mali; not Mesa PanVK), 0 on others (their drivers' preference is not known to cost anything).
 */
#define DED_KEEP_MIN ((VkDeviceSize)32 << 20)

/* Arm's driver: driverID ARM_PROPRIETARY from the server's self-test; vendor 0x13B5 where it has
 * none (an older server, a driver without VK_KHR_driver_properties). Mesa PanVK has Arm's vendor ID. */
static int arm_driver(const vkb_physdev *pd)
{
    return pd->info.driver_id ? pd->info.driver_id == VK_DRIVER_ID_ARM_PROPRIETARY : pd->props.vendorID == 0x13B5;
}

void vkb_memory_init_device(vkb_device *d)
{
    vkb_rq_cache *c = calloc(1, sizeof(*c));
    if (c) {
        pthread_mutex_init(&c->lock, NULL);
        vkb_map_init(&c->buffers);
        vkb_map_init(&c->images);
        vkb_map_init(&c->ded_keep);
        const char *e = getenv("VKBRIDGE_DEDICATED_HINT");
        c->ded_clear = e && *e ? atoi(e) != 0 : arm_driver(d->pd);
        c->ded_imitate = getenv("VKBRIDGE_TEST_PREFER_DEDICATED") && atoi(getenv("VKBRIDGE_TEST_PREFER_DEDICATED")) == 1;
        VKB_INFO("dedicated allocation hint: %s (VKBRIDGE_DEDICATED_HINT=%s)%s",
                 c->ded_clear ? "prefersDedicatedAllocation cleared for ordinary buffers and images" : "the driver's answer passed through",
                 e && *e ? e : c->ded_clear ? "1, the default on Arm" : "0, the default", c->ded_imitate ? "; imitating Mali's preference" : "");
    }
    d->buf_reqs = c;
}

/* Made for external memory by the app itself (see above): keeps the driver's preference. */
static int ded_external(const void *pNext)
{
    for (const VkBaseInStructure *b = pNext; b; b = b->pNext)
        switch (b->sType) {
        case VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO:
            if (((const VkExternalMemoryBufferCreateInfo *)b)->handleTypes) return 1;
            break;
        case VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO:
            if (((const VkExternalMemoryImageCreateInfo *)b)->handleTypes) return 1;
            break;
        case VK_STRUCTURE_TYPE_IMAGE_SWAPCHAIN_CREATE_INFO_KHR:
        case VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT:
        case VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT:
        case VK_STRUCTURE_TYPE_EXTERNAL_FORMAT_ANDROID:
            return 1;
        default:
            break;
        }
    return 0;
}

static void ded_track(vkb_device *dev, uint64_t handle, const void *pNext, int drm_tiling)
{
    vkb_rq_cache *c = dev->buf_reqs;
    if (c && c->ded_clear && (drm_tiling || ded_external(pNext))) vkb_map_put(&c->ded_keep, handle, (void *)1);
}

static void ded_forget(vkb_device *dev, uint64_t handle)
{
    if (dev->buf_reqs) vkb_map_remove(&dev->buf_reqs->ded_keep, handle);
}

/* The answer in pReq's VkMemoryDedicatedRequirements, if any, as the app gets it. keep: the
 * resource keeps the driver's preference whatever the switch says. */
static void ded_hint(vkb_device *dev, VkMemoryRequirements2 *pReq, int keep)
{
    vkb_rq_cache *c = dev->buf_reqs;
    if (!c || (!c->ded_clear && !c->ded_imitate)) return;
    for (VkBaseOutStructure *b = pReq->pNext; b; b = b->pNext) {
        if (b->sType != VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS) continue;
        VkMemoryDedicatedRequirements *d = (VkMemoryDedicatedRequirements *)b;
        if (c->ded_imitate) d->prefersDedicatedAllocation = VK_TRUE;
        if (!c->ded_clear || keep || d->requiresDedicatedAllocation || !d->prefersDedicatedAllocation ||
            pReq->memoryRequirements.size >= DED_KEEP_MIN)
            continue;
        d->prefersDedicatedAllocation = VK_FALSE;
        if (__atomic_fetch_add(&c->ded_cleared, 1, __ATOMIC_RELAXED) == 0)
            VKB_INFO("dedicated allocation hint: first preference cleared (%llu bytes)", (unsigned long long)pReq->memoryRequirements.size);
    }
}

static int ded_kept(vkb_device *dev, uint64_t handle)
{
    vkb_rq_cache *c = dev->buf_reqs;
    return c && c->ded_clear && vkb_map_get(&c->ded_keep, handle) != NULL;
}

void vkb_memory_destroy_device(vkb_device *d)
{
    vkb_rq_cache *c = d->buf_reqs;
    if (!c) return;
    for (unsigned b = 0; b < RQ_HB; b++)
        for (rq_entry *e = c->buckets[b], *nx; e; e = nx) {
            nx = e->next;
            free(e);
        }
    for (unsigned b = 0; b < RQ_HB; b++)
        for (irq_entry *e = c->ibuckets[b], *nx; e; e = nx) {
            nx = e->next;
            free(e);
        }
    vkb_map_destroy(&c->buffers);
    vkb_map_destroy(&c->images);
    vkb_map_destroy(&c->ded_keep);
    pthread_mutex_destroy(&c->lock);
    free(c);
    d->buf_reqs = NULL;
}

static void rq_buffer_created(vkb_device *dev, const VkBufferCreateInfo *ci, VkBuffer buffer, int shareable)
{
    vkb_rq_cache *c = dev->buf_reqs;
    if (!c || ci->pNext || ci->sharingMode != VK_SHARING_MODE_EXCLUSIVE) return;
    uint64_t h = ci->size * 0x9e3779b97f4a7c15ull ^ (uint64_t)ci->usage << 20 ^ (uint64_t)ci->flags << 52 ^ (uint64_t)shareable;
    h ^= h >> 31;
    h *= 0xbf58476d1ce4e5b9ull;
    h ^= h >> 29;
    pthread_mutex_lock(&c->lock);
    rq_entry **head = &c->buckets[h & (RQ_HB - 1)], *e = *head;
    while (e && !(e->size == ci->size && e->usage == ci->usage && e->flags == ci->flags && e->shareable == shareable)) e = e->next;
    if (!e && c->count < RQ_MAX && (e = calloc(1, sizeof(*e)))) {
        e->size = ci->size;
        e->usage = ci->usage;
        e->flags = ci->flags;
        e->shareable = shareable;
        e->next = *head;
        *head = e;
        c->count++;
    }
    pthread_mutex_unlock(&c->lock);
    if (e) vkb_map_put(&c->buffers, (uint64_t)buffer, e);
}

static void rq_image_created(vkb_device *dev, const VkImageCreateInfo *ci, VkFormat app_format, VkImage image)
{
    vkb_rq_cache *c = dev->buf_reqs;
    if (!c || ci->pNext || ci->sharingMode != VK_SHARING_MODE_EXCLUSIVE || ci->tiling != VK_IMAGE_TILING_OPTIMAL || vkb_is_bc(app_format) ||
        vkb_is_bc(ci->format) || (ci->flags & (VK_IMAGE_CREATE_SPARSE_BINDING_BIT | VK_IMAGE_CREATE_DISJOINT_BIT)))
        return;
    uint64_t h = (uint64_t)ci->format * 0x9e3779b97f4a7c15ull ^ (uint64_t)ci->extent.width << 8 ^ (uint64_t)ci->extent.height << 24 ^
                 (uint64_t)ci->extent.depth << 40 ^ (uint64_t)ci->mipLevels << 48 ^ (uint64_t)ci->arrayLayers << 54 ^ (uint64_t)ci->usage << 4 ^
                 (uint64_t)ci->flags << 33 ^ (uint64_t)ci->samples << 60 ^ (uint64_t)ci->imageType << 62;
    h ^= h >> 31;
    h *= 0xbf58476d1ce4e5b9ull;
    h ^= h >> 29;
    pthread_mutex_lock(&c->lock);
    irq_entry **head = &c->ibuckets[h & (RQ_HB - 1)], *e = *head;
    while (e && !(e->flags == ci->flags && e->type == ci->imageType && e->format == ci->format && e->extent.width == ci->extent.width &&
                  e->extent.height == ci->extent.height && e->extent.depth == ci->extent.depth && e->mips == ci->mipLevels &&
                  e->layers == ci->arrayLayers && e->samples == ci->samples && e->usage == ci->usage))
        e = e->next;
    if (!e && c->icount < RQ_MAX && (e = calloc(1, sizeof(*e)))) {
        e->flags = ci->flags;
        e->type = ci->imageType;
        e->format = ci->format;
        e->extent = ci->extent;
        e->mips = ci->mipLevels;
        e->layers = ci->arrayLayers;
        e->samples = ci->samples;
        e->usage = ci->usage;
        e->next = *head;
        *head = e;
        c->icount++;
    }
    pthread_mutex_unlock(&c->lock);
    if (e) vkb_map_put(&c->images, (uint64_t)image, &e->a);
}

/* The kept answer for a buffer or image (map: the cache's buffers or images), if any: 1 = filled
 * in. dedicated may be NULL. */
static int rq_lookup_in(vkb_device *dev, int images, uint64_t handle, VkMemoryRequirements *req, VkMemoryDedicatedRequirements *dedicated)
{
    vkb_rq_cache *c = dev->buf_reqs;
    rq_entry *e = c ? vkb_map_get(images ? &c->images : &c->buffers, handle) : NULL;
    if (!e) return 0;
    int hit = 0;
    pthread_mutex_lock(&c->lock);
    if (e->have && (!dedicated || e->have_dedicated)) {
        *req = e->req;
        if (dedicated) {
            dedicated->prefersDedicatedAllocation = e->prefers_dedicated;
            dedicated->requiresDedicatedAllocation = e->requires_dedicated;
        }
        hit = 1;
    }
    pthread_mutex_unlock(&c->lock);
    return hit;
}

static void rq_store_in(vkb_device *dev, int images, uint64_t handle, const VkMemoryRequirements *req,
                        const VkMemoryDedicatedRequirements *dedicated)
{
    vkb_rq_cache *c = dev->buf_reqs;
    rq_entry *e = c ? vkb_map_get(images ? &c->images : &c->buffers, handle) : NULL;
    if (!e) return;
    pthread_mutex_lock(&c->lock);
    e->req = *req;
    e->have = 1;
    if (dedicated) {
        e->prefers_dedicated = dedicated->prefersDedicatedAllocation;
        e->requires_dedicated = dedicated->requiresDedicatedAllocation;
        e->have_dedicated = 1;
    }
    pthread_mutex_unlock(&c->lock);
}

static int image_shareable(vkb_device *dev, VkImage i);
static void fix_reqs(vkb_device *dev, VkMemoryRequirements *r, int allow_shared);

/* VKBRIDGE_FAST_CREATE: the requirements vkCreateImage's reply carried (q), kept for the image as vkGetImageMemoryRequirements2
 * would have kept them (the driver's answer with the client's memory-type translation; the dedicated hint is applied when asked).
 * An image of a create info the cache keys gets them in that entry; any other (BC, a pNext, LINEAR...) an entry of its own. */
static void rq_image_reqs(vkb_device *dev, VkImage image, const vkb_fast_reqs *q)
{
    vkb_rq_cache *c = dev->buf_reqs;
    if (!c || !q->have || !image) return;
    if (!vkb_map_get(&c->images, (uint64_t)image)) {
        rq_entry *e = calloc(1, sizeof(*e));
        if (!e) return;
        e->owned = 1;
        vkb_map_put(&c->images, (uint64_t)image, e);
    }
    VkMemoryRequirements r = q->req;
    fix_reqs(dev, &r, image_shareable(dev, image));
    VkMemoryDedicatedRequirements ded = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS, NULL, q->prefers, q->requires};
    rq_store_in(dev, 1, (uint64_t)image, &r, &ded);
    /* VKBRIDGE_TEST_FAST_CREATE_VERIFY=1 (a proof run, not for play: a round trip a create): the reply's answer against the
     * server's own vkGetImageMemoryRequirements2 asked as before, byte for byte; a line at the 1st, 2nd, 4th ... and every mismatch */
    static int verify = -1;
    if (verify < 0) verify = getenv("VKBRIDGE_TEST_FAST_CREATE_VERIFY") && !strcmp(getenv("VKBRIDGE_TEST_FAST_CREATE_VERIFY"), "1");
    if (!verify) return;
    VkMemoryDedicatedRequirements wd = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
    VkMemoryRequirements2 w = {VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, &wd};
    VkImageMemoryRequirementsInfo2 info = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2, NULL, image};
    vkb_wire_vkGetImageMemoryRequirements2((VkDevice)dev, &info, &w);
    fix_reqs(dev, &w.memoryRequirements, image_shareable(dev, image));
    int same = !memcmp(&w.memoryRequirements, &r, sizeof(r)) && wd.prefersDedicatedAllocation == q->prefers &&
               wd.requiresDedicatedAllocation == q->requires;
    static uint64_t n, bad;
    uint64_t k = __atomic_add_fetch(&n, 1, __ATOMIC_RELAXED), b = same ? __atomic_load_n(&bad, __ATOMIC_RELAXED) : __atomic_add_fetch(&bad, 1, __ATOMIC_RELAXED);
    if (!same)
        VKB_WARN("fast create verify: image %llx: reply %llu/%llu/0x%x ded %u/%u, server %llu/%llu/0x%x ded %u/%u", (unsigned long long)(uint64_t)image,
                 (unsigned long long)r.size, (unsigned long long)r.alignment, r.memoryTypeBits, q->prefers, q->requires,
                 (unsigned long long)w.memoryRequirements.size, (unsigned long long)w.memoryRequirements.alignment, w.memoryRequirements.memoryTypeBits,
                 wd.prefersDedicatedAllocation, wd.requiresDedicatedAllocation);
    if (!(k & (k - 1))) vkb_log_info_always("fast create verify: %llu answers from create replies compared with the server's, %llu differ", (unsigned long long)k,
                                 (unsigned long long)b);
}

/* The creates the server runs ahead of queued work (server_conn.c fc_kind, which checks again): sent so only when they are of that
 * kind, so that one the server would make wait is sent as before (the queue written out first). Images: OPTIMAL, not sparse, a
 * chain of format lists and stencil usage only, BC only when the server asks for no BC rule weighing bytes alive (fc & 2). */
static int fast_image(int fc, const VkImageCreateInfo *ci, VkFormat app_format)
{
    if (fc & 4) return 1;
    if (ci->tiling != VK_IMAGE_TILING_OPTIMAL ||
        (ci->flags & (VK_IMAGE_CREATE_SPARSE_BINDING_BIT | VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT | VK_IMAGE_CREATE_SPARSE_ALIASED_BIT)))
        return 0;
    for (const VkBaseInStructure *b = ci->pNext; b; b = b->pNext)
        if (b->sType != VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO && b->sType != VK_STRUCTURE_TYPE_IMAGE_STENCIL_USAGE_CREATE_INFO) return 0;
    return !(vkb_is_bc(app_format) || vkb_is_bc(ci->format)) || (fc & 2);
}

/* vkCreateImage / vkCreateBuffer on the wire, with VKB_F_FAST_CREATE when on and of the kind (vkb_fast_create); images always ask
 * for their requirements in the reply then (VKB_F_CREATE_REQS); q: what it carried */
static VkResult wire_create_image(VkDevice device, const VkImageCreateInfo *ci, VkFormat app_format, const VkAllocationCallbacks *a,
                                  VkImage *pImage, vkb_fast_reqs *q)
{
    q->have = 0;
    int fc = vkb_fast_create();
    if (!fc) return vkb_wire_vkCreateImage(device, ci, a, pImage);
    vkb_call_fast_next(VKB_F_CREATE_REQS | (fast_image(fc, ci, app_format) ? VKB_F_FAST_CREATE : 0));
    VkResult r = vkb_wire_vkCreateImage(device, ci, a, pImage);
    vkb_fast_reqs_take(q);
    return r;
}

static VkResult wire_create_buffer(VkDevice device, const VkBufferCreateInfo *ci, const VkAllocationCallbacks *a, VkBuffer *pBuffer)
{
    int fc = vkb_fast_create(), fast = fc != 0;
    if (fc && !(fc & 4)) {
        fast = !(ci->flags & (VK_BUFFER_CREATE_SPARSE_BINDING_BIT | VK_BUFFER_CREATE_SPARSE_RESIDENCY_BIT | VK_BUFFER_CREATE_SPARSE_ALIASED_BIT));
        for (const VkBaseInStructure *b = ci->pNext; b && fast; b = b->pNext)
            fast = b->sType == VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO || b->sType == VK_STRUCTURE_TYPE_BUFFER_USAGE_FLAGS_2_CREATE_INFO;
    }
    if (fast) vkb_call_fast_next(VKB_F_FAST_CREATE);
    return vkb_wire_vkCreateBuffer(device, ci, a, pBuffer);
}

static int rq_lookup(vkb_device *dev, VkBuffer buffer, VkMemoryRequirements *req, VkMemoryDedicatedRequirements *dedicated)
{
    return rq_lookup_in(dev, 0, (uint64_t)buffer, req, dedicated);
}

static void rq_store(vkb_device *dev, VkBuffer buffer, const VkMemoryRequirements *req, const VkMemoryDedicatedRequirements *dedicated)
{
    rq_store_in(dev, 0, (uint64_t)buffer, req, dedicated);
}

void vkb_memory_unmap_free(vkb_memory *m)
{
    if (m->ptr) munmap(m->ptr, (size_t)m->size);
    free(m);
}

/* ------------------------------------------------------------------ resources */

/* Shallow copy of a pNext chain (known structures only), in one malloc'd block (*mem). */
void *vkb_chain_clone(const void *pNext, void **mem)
{
    size_t total = 0;
    for (const VkBaseInStructure *b = pNext; b; b = b->pNext) total += (vkb_struct_size(b->sType) + 15) & ~(size_t)15;
    uint8_t *block = calloc(1, total ? total : 16);
    *mem = block;
    VkBaseOutStructure *head = NULL, *prev = NULL;
    size_t off = 0;
    for (const VkBaseInStructure *b = pNext; b; b = b->pNext) {
        size_t sz = vkb_struct_size(b->sType);
        if (!sz) continue;
        VkBaseOutStructure *n = (VkBaseOutStructure *)(block + off);
        memcpy(n, b, sz);
        n->pNext = NULL;
        off += (sz + 15) & ~(size_t)15;
        if (prev) prev->pNext = n;
        else head = n;
        prev = n;
    }
    return head;
}

/* Buffers may take shared memory: with the bridge's handle types, or without any where the server's
 * self-test passed only that (VKB_MEMF_UNFLAGGED_BUFFERS; then buffer_handle_types is 0). */
static int can_share(vkb_device *dev)
{
    return dev->pd->info.strategy != VKB_MEM_NONE &&
           (dev->pd->info.buffer_handle_types || (dev->pd->info.mem_flags & VKB_MEMF_UNFLAGGED_BUFFERS));
}

static VkResult create_buffer(VkDevice device, const VkBufferCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator,
                              VkBuffer *pBuffer)
{
    vkb_device *dev = vkb_dev(device);
    if (!can_share(dev) || (pCreateInfo->flags & VK_BUFFER_CREATE_SPARSE_BINDING_BIT)) {
        VkResult r = wire_create_buffer(device, pCreateInfo, pAllocator, pBuffer);
        if (r == VK_SUCCESS) vkb_map_put(&dev->unshared, (uint64_t)*pBuffer, (void *)1);
        return r;
    }
    /* Every buffer may be bound to shared memory: say so at creation, as the spec requires. */
    VkBufferCreateInfo ci = *pCreateInfo;
    VkExternalMemoryBufferCreateInfo ext = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO, ci.pNext,
                                            dev->pd->info.buffer_handle_types};
    int had = 0;
    for (VkBaseOutStructure *b = (VkBaseOutStructure *)ci.pNext; b; b = b->pNext) {
        if (b->sType == VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO) had = 1;
    }
    VkResult r;
    if (had) {
        /* The app has its own external handle types: ours are added to a copy of its chain. */
        void *mem = NULL;
        ci.pNext = vkb_chain_clone(ci.pNext, &mem);
        for (VkBaseOutStructure *b = (VkBaseOutStructure *)ci.pNext; b; b = b->pNext)
            if (b->sType == VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO)
                ((VkExternalMemoryBufferCreateInfo *)b)->handleTypes |= dev->pd->info.buffer_handle_types;
        r = wire_create_buffer(device, &ci, pAllocator, pBuffer);
        free(mem);
        if (r != VK_SUCCESS && r != VK_ERROR_OUT_OF_HOST_MEMORY && r != VK_ERROR_OUT_OF_DEVICE_MEMORY) {
            r = wire_create_buffer(device, pCreateInfo, pAllocator, pBuffer);
            if (r == VK_SUCCESS) vkb_map_put(&dev->unshared, (uint64_t)*pBuffer, (void *)1);
        }
        return r;
    }
    ci.pNext = &ext;
    r = wire_create_buffer(device, &ci, pAllocator, pBuffer);
    if (r != VK_SUCCESS && r != VK_ERROR_OUT_OF_HOST_MEMORY && r != VK_ERROR_OUT_OF_DEVICE_MEMORY) {
        VKB_ONCE(VKB_LOG_WARN, "a buffer cannot be created shareable (%d); it gets device-only memory (logged once)", r);
        r = wire_create_buffer(device, pCreateInfo, pAllocator, pBuffer);
        if (r == VK_SUCCESS) vkb_map_put(&dev->unshared, (uint64_t)*pBuffer, (void *)1);
    }
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL vkb_ep_vkCreateBuffer(VkDevice device, const VkBufferCreateInfo *pCreateInfo,
                                                     const VkAllocationCallbacks *pAllocator, VkBuffer *pBuffer)
{
    vkb_device *dev = vkb_dev(device);
    VkResult r = create_buffer(device, pCreateInfo, pAllocator, pBuffer);
    if (r == VK_SUCCESS) {
        rq_buffer_created(dev, pCreateInfo, *pBuffer, can_share(dev) && !vkb_map_get(&dev->unshared, (uint64_t)*pBuffer));
        ded_track(dev, (uint64_t)*pBuffer, pCreateInfo->pNext, 0);
    }
    return r;
}

VKAPI_ATTR void VKAPI_CALL vkb_ep_vkDestroyBuffer(VkDevice device, VkBuffer buffer, const VkAllocationCallbacks *pAllocator)
{
    if (!buffer) return;
    vkb_bufview_buffer_destroyed(vkb_dev(device), buffer);
    if (vkb_dev(device)->buf_reqs) vkb_map_remove(&vkb_dev(device)->buf_reqs->buffers, (uint64_t)buffer);
    ded_forget(vkb_dev(device), (uint64_t)buffer);
    vkb_map_remove(&vkb_dev(device)->unshared, (uint64_t)buffer);
    vkb_wire_vkDestroyBuffer(device, buffer, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL vkb_ep_vkCreateImage(VkDevice device, const VkImageCreateInfo *pCreateInfo,
                                                    const VkAllocationCallbacks *pAllocator, VkImage *pImage)
{
    vkb_device *dev = vkb_dev(device);
    VkImageCreateInfo ci = *pCreateInfo;
    vkb_emu_image_create_info(dev, &ci);
    if (vkb_emu_planar_ext_usage(dev->pd, ci.format, ci.tiling, ci.usage, &ci.flags)) {
        static int told;
        if (!__atomic_fetch_add(&told, 1, __ATOMIC_RELAXED))
            VKB_INFO("multi-planar image (format %d, usage 0x%x): made without VK_IMAGE_CREATE_EXTENDED_USAGE_BIT, which the "
                     "driver may refuse on such formats; the format covers the usage (logged once)", ci.format, ci.usage);
    }
    /* Linear images may be mapped by the app: those get the shared handle type when allowed. */
    /* (not with unflagged buffers: the self-test proved buffers only) */
    if (can_share(dev) && dev->pd->info.buffer_handle_types && ci.tiling == VK_IMAGE_TILING_LINEAR &&
        !(ci.flags & VK_IMAGE_CREATE_SPARSE_BINDING_BIT)) {
        int had = 0;
        for (VkBaseOutStructure *b = (VkBaseOutStructure *)ci.pNext; b; b = b->pNext)
            had |= b->sType == VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
        if (!had) {
            VkExternalMemoryImageCreateInfo ext = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, ci.pNext,
                                                   dev->pd->info.buffer_handle_types};
            VkImageCreateInfo c2 = ci;
            c2.pNext = &ext;
            vkb_fast_reqs q;
            VkResult r = wire_create_image(device, &c2, pCreateInfo->format, pAllocator, pImage, &q);
            if (r == VK_SUCCESS) {
                vkb_map_put(&dev->shared_images, (uint64_t)*pImage, (void *)1);
                vkb_emu_image_created(dev, pCreateInfo, *pImage);
                ded_track(dev, (uint64_t)*pImage, pCreateInfo->pNext, 0);
                rq_image_reqs(dev, *pImage, &q);
                return r;
            }
        }
    }
    vkb_fast_reqs q;
    VkResult r = wire_create_image(device, &ci, pCreateInfo->format, pAllocator, pImage, &q);
    if (r == VK_SUCCESS) {
        rq_image_created(dev, &ci, pCreateInfo->format, *pImage);
        vkb_emu_image_created(dev, pCreateInfo, *pImage);
        ded_track(dev, (uint64_t)*pImage, pCreateInfo->pNext, pCreateInfo->tiling == VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT);
        rq_image_reqs(dev, *pImage, &q);
    }
    return r;
}

VKAPI_ATTR void VKAPI_CALL vkb_ep_vkDestroyImage(VkDevice device, VkImage image, const VkAllocationCallbacks *pAllocator)
{
    if (!image) return;
    vkb_device *dev = vkb_dev(device);
    vkb_map_remove(&dev->shared_images, (uint64_t)image);
    if (dev->buf_reqs) {
        rq_entry *e = vkb_map_remove(&dev->buf_reqs->images, (uint64_t)image);
        if (e && e->owned) free(e);
    }
    ded_forget(dev, (uint64_t)image);
    vkb_emu_image_destroyed(dev, image);
    vkb_wire_vkDestroyImage(device, image, pAllocator);
}

/* ------------------------------------------------------------------ requirements */

static void fix_reqs(vkb_device *dev, VkMemoryRequirements *r, int allow_shared)
{
    r->memoryTypeBits = vkb_mem_bits_to_client(dev->pd, r->memoryTypeBits, allow_shared);
}

/* Buffers: vkb_mem_buffer_bits_to_client (physdev.c) adds the shared types where the driver's
 * mask has host-visible types but none of those (the XRing O3). */
static void fix_buf_reqs(vkb_device *dev, VkMemoryRequirements *r, int allow_shared)
{
    r->memoryTypeBits = vkb_mem_buffer_bits_to_client(dev->pd, r->memoryTypeBits, allow_shared);
}

static int buffer_shareable(vkb_device *dev, VkBuffer b)
{
    return can_share(dev) && !vkb_map_get(&dev->unshared, (uint64_t)b);
}

static int image_shareable(vkb_device *dev, VkImage i)
{
    return vkb_map_get(&dev->shared_images, (uint64_t)i) != NULL;
}

VKAPI_ATTR void VKAPI_CALL vkb_ep_vkGetBufferMemoryRequirements(VkDevice device, VkBuffer buffer, VkMemoryRequirements *pReq)
{
    vkb_device *dev = vkb_dev(device);
    if (rq_lookup(dev, buffer, pReq, NULL)) return;
    vkb_wire_vkGetBufferMemoryRequirements(device, buffer, pReq);
    fix_buf_reqs(dev, pReq, buffer_shareable(dev, buffer));
    rq_store(dev, buffer, pReq, NULL);
}

VKAPI_ATTR void VKAPI_CALL vkb_ep_vkGetBufferMemoryRequirements2(VkDevice device, const VkBufferMemoryRequirementsInfo2 *pInfo,
                                                                 VkMemoryRequirements2 *pReq)
{
    vkb_device *dev = vkb_dev(device);
    /* Kept answers cover VkMemoryDedicatedRequirements (DXVK chains it) and nothing else. */
    VkMemoryDedicatedRequirements *ded = NULL;
    int plain = !pInfo->pNext;
    for (VkBaseOutStructure *b = pReq->pNext; b && plain; b = b->pNext) {
        if (b->sType == VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS && !ded) ded = (VkMemoryDedicatedRequirements *)b;
        else plain = 0;
    }
    /* The cache keeps the driver's answer; the hint is applied to what the app gets. */
    if (!(plain && rq_lookup(dev, pInfo->buffer, &pReq->memoryRequirements, ded))) {
        vkb_wire_vkGetBufferMemoryRequirements2(device, pInfo, pReq);
        fix_buf_reqs(dev, &pReq->memoryRequirements, buffer_shareable(dev, pInfo->buffer));
        if (plain) rq_store(dev, pInfo->buffer, &pReq->memoryRequirements, ded);
    }
    ded_hint(dev, pReq, ded_kept(dev, (uint64_t)pInfo->buffer));
}

VKAPI_ATTR void VKAPI_CALL vkb_ep_vkGetImageMemoryRequirements(VkDevice device, VkImage image, VkMemoryRequirements *pReq)
{
    vkb_device *dev = vkb_dev(device);
    if (rq_lookup_in(dev, 1, (uint64_t)image, pReq, NULL)) return;
    vkb_wire_vkGetImageMemoryRequirements(device, image, pReq);
    fix_reqs(dev, pReq, image_shareable(dev, image));
    rq_store_in(dev, 1, (uint64_t)image, pReq, NULL);
}

VKAPI_ATTR void VKAPI_CALL vkb_ep_vkGetImageMemoryRequirements2(VkDevice device, const VkImageMemoryRequirementsInfo2 *pInfo,
                                                                VkMemoryRequirements2 *pReq)
{
    vkb_device *dev = vkb_dev(device);
    /* As for buffers: kept answers cover VkMemoryDedicatedRequirements only, and not planes. */
    VkMemoryDedicatedRequirements *ded = NULL;
    int plain = !pInfo->pNext;
    for (VkBaseOutStructure *b = pReq->pNext; b && plain; b = b->pNext) {
        if (b->sType == VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS && !ded) ded = (VkMemoryDedicatedRequirements *)b;
        else plain = 0;
    }
    if (!(plain && rq_lookup_in(dev, 1, (uint64_t)pInfo->image, &pReq->memoryRequirements, ded))) {
        vkb_wire_vkGetImageMemoryRequirements2(device, pInfo, pReq);
        fix_reqs(dev, &pReq->memoryRequirements, image_shareable(dev, pInfo->image));
        if (plain) rq_store_in(dev, 1, (uint64_t)pInfo->image, &pReq->memoryRequirements, ded);
    }
    /* A plane of a disjoint image (VkImagePlaneMemoryRequirementsInfo) is treated alike. */
    ded_hint(dev, pReq, ded_kept(dev, (uint64_t)pInfo->image));
}

VKAPI_ATTR void VKAPI_CALL vkb_ep_vkGetDeviceBufferMemoryRequirements(VkDevice device, const VkDeviceBufferMemoryRequirements *pInfo,
                                                                      VkMemoryRequirements2 *pReq)
{
    vkb_device *dev = vkb_dev(device);
    /* Asked of the buffer create_buffer would make: unshared without sharing or when sparse, else
     * with the bridge's handle types (added to the app's own, if it has some) - they change the
     * masks on the XRing O3. */
    if (!can_share(dev) || (pInfo->pCreateInfo->flags & VK_BUFFER_CREATE_SPARSE_BINDING_BIT)) {
        vkb_wire_vkGetDeviceBufferMemoryRequirements(device, pInfo, pReq);
        fix_buf_reqs(dev, &pReq->memoryRequirements, 0);
        ded_hint(dev, pReq, ded_external(pInfo->pCreateInfo->pNext));
        return;
    }
    VkBufferCreateInfo ci = *pInfo->pCreateInfo;
    VkExternalMemoryBufferCreateInfo ext = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO, ci.pNext,
                                            dev->pd->info.buffer_handle_types};
    int had = 0;
    for (VkBaseOutStructure *b = (VkBaseOutStructure *)ci.pNext; b; b = b->pNext)
        had |= b->sType == VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO;
    void *mem = NULL;
    if (had) {
        ci.pNext = vkb_chain_clone(ci.pNext, &mem);
        for (VkBaseOutStructure *b = (VkBaseOutStructure *)ci.pNext; b; b = b->pNext)
            if (b->sType == VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO)
                ((VkExternalMemoryBufferCreateInfo *)b)->handleTypes |= dev->pd->info.buffer_handle_types;
    } else {
        ci.pNext = &ext;
    }
    VkDeviceBufferMemoryRequirements info = *pInfo;
    info.pCreateInfo = &ci;
    vkb_wire_vkGetDeviceBufferMemoryRequirements(device, &info, pReq);
    free(mem);
    fix_buf_reqs(dev, &pReq->memoryRequirements, 1);
    ded_hint(dev, pReq, ded_external(pInfo->pCreateInfo->pNext));
}

VKAPI_ATTR void VKAPI_CALL vkb_ep_vkGetDeviceImageMemoryRequirements(VkDevice device, const VkDeviceImageMemoryRequirements *pInfo,
                                                                     VkMemoryRequirements2 *pReq)
{
    vkb_device *dev = vkb_dev(device);
    VkImageCreateInfo ci = *pInfo->pCreateInfo;
    vkb_emu_image_create_info(dev, &ci);
    VkDeviceImageMemoryRequirements info = *pInfo;
    info.pCreateInfo = &ci;
    vkb_wire_vkGetDeviceImageMemoryRequirements(device, &info, pReq);
    fix_reqs(dev, &pReq->memoryRequirements, 0);
    ded_hint(dev, pReq, ded_external(pInfo->pCreateInfo->pNext) || pInfo->pCreateInfo->tiling == VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT);
    if (!pReq->memoryRequirements.size || !pReq->memoryRequirements.memoryTypeBits) {
        /* Deep Rock Galactic on D3D12 asked vkd3d for a heap of size 0; a requirement of 0 bytes
         * (the driver's answer for an image it cannot make) would explain it. The first few. */
        static int told;
        if (__atomic_fetch_add(&told, 1, __ATOMIC_RELAXED) < 16)
            VKB_WARN("image requirements: size %llu, types 0x%x for format %d (asked %d) %ux%ux%u mips %u layers %u samples %d tiling %d "
                     "usage 0x%x flags 0x%x type %d", (unsigned long long)pReq->memoryRequirements.size,
                     pReq->memoryRequirements.memoryTypeBits, ci.format, pInfo->pCreateInfo->format, ci.extent.width, ci.extent.height,
                     ci.extent.depth, ci.mipLevels, ci.arrayLayers, ci.samples, ci.tiling, ci.usage, ci.flags, ci.imageType);
    }
}

VKAPI_ATTR void VKAPI_CALL vkb_ep_vkGetImageSparseMemoryRequirements(VkDevice device, VkImage image, uint32_t *pCount,
                                                                     VkSparseImageMemoryRequirements *pReq)
{
    vkb_wire_vkGetImageSparseMemoryRequirements(device, image, pCount, pReq);
}

VKAPI_ATTR void VKAPI_CALL vkb_ep_vkGetImageSparseMemoryRequirements2(VkDevice device, const VkImageSparseMemoryRequirementsInfo2 *pInfo,
                                                                      uint32_t *pCount, VkSparseImageMemoryRequirements2 *pReq)
{
    vkb_wire_vkGetImageSparseMemoryRequirements2(device, pInfo, pCount, pReq);
}

VKAPI_ATTR void VKAPI_CALL vkb_ep_vkGetDeviceImageSparseMemoryRequirements(VkDevice device, const VkDeviceImageMemoryRequirements *pInfo,
                                                                           uint32_t *pCount, VkSparseImageMemoryRequirements2 *pReq)
{
    vkb_wire_vkGetDeviceImageSparseMemoryRequirements(device, pInfo, pCount, pReq);
}

VKAPI_ATTR VkResult VKAPI_CALL vkb_ep_vkGetMemoryFdPropertiesKHR(VkDevice device, VkExternalMemoryHandleTypeFlagBits handleType, int fd,
                                                                 VkMemoryFdPropertiesKHR *pProps)
{
    VkResult r = vkb_wire_vkGetMemoryFdPropertiesKHR(device, handleType, fd, pProps);
    if (r != VK_SUCCESS) return r;
    vkb_physdev *pd = vkb_dev(device)->pd;
    uint32_t server_bits = pProps->memoryTypeBits;
    pProps->memoryTypeBits = vkb_mem_bits_to_client(pd, server_bits, 0);
    /* Mali puts a dma-buf only in its host-visible cached type, which the client offers only as a
     * shared type (refused for imports): the bits came out 0, and an app that imports in a type
     * both the fd and the resource allow (gamescope's DMA-BUF patch for other GPUs: "No compatible
     * image memory type (image=0x1, fd=0x0)") had none. The device-local types without host
     * access (where images go) take an importable fd too: the server imports an fd with a
     * dedicated image or buffer in a type the fd and the resource both allow, and one without in
     * the type asked, as before (server_memory.c, fit_import_type). */
    if (server_bits)
        for (uint32_t i = 0; i < pd->client_mem.memoryTypeCount; i++) {
            VkMemoryPropertyFlags f = pd->client_mem.memoryTypes[i].propertyFlags;
            VkMemoryPropertyFlags sf = pd->server_mem.memoryTypes[pd->type_map[i].server_type].propertyFlags;
            if (!pd->type_map[i].shared && (f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) && !(f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
                !((f | sf) & (VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT | VK_MEMORY_PROPERTY_PROTECTED_BIT)))
                pProps->memoryTypeBits |= 1u << i;
        }
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL vkb_ep_vkBindBufferMemory(VkDevice device, VkBuffer buffer, VkDeviceMemory memory, VkDeviceSize offset)
{
    return vkb_wire_vkBindBufferMemory(device, buffer, memory, offset);
}

VKAPI_ATTR VkResult VKAPI_CALL vkb_ep_vkBindBufferMemory2(VkDevice device, uint32_t n, const VkBindBufferMemoryInfo *p)
{
    return vkb_wire_vkBindBufferMemory2(device, n, p);
}

VKAPI_ATTR VkResult VKAPI_CALL vkb_ep_vkBindImageMemory(VkDevice device, VkImage image, VkDeviceMemory memory, VkDeviceSize offset)
{
    return vkb_wire_vkBindImageMemory(device, image, memory, offset);
}

VKAPI_ATTR VkResult VKAPI_CALL vkb_ep_vkBindImageMemory2(VkDevice device, uint32_t n, const VkBindImageMemoryInfo *p)
{
    return vkb_wire_vkBindImageMemory2(device, n, p);
}

VKAPI_ATTR void VKAPI_CALL vkb_ep_vkGetImageSubresourceLayout(VkDevice device, VkImage image, const VkImageSubresource *pSubresource,
                                                              VkSubresourceLayout *pLayout)
{
    vkb_wire_vkGetImageSubresourceLayout(device, image, pSubresource, pLayout);
}

/* ------------------------------------------------------------------ allocation and mapping */

static void close_import_fds(const void *pNext)
{
    for (const VkBaseInStructure *b = pNext; b; b = b->pNext) {
        if (b->sType == VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR) {
            int fd = ((const VkImportMemoryFdInfoKHR *)b)->fd;
            if (fd >= 0) close(fd);
        }
    }
}

/* One allocation in server type ai->memoryTypeIndex; ct is the client type (for the log, and
 * UINT32_MAX for the client's own hidden readback type). */
static VkResult alloc_memory(VkDevice device, const VkMemoryAllocateInfo *pAllocateInfo, VkMemoryAllocateInfo ai, uint32_t ct,
                             int shared, int cached, VkDeviceMemory *pMemory)
{
    vkb_device *dev = vkb_dev(device);
    vkb_physdev *pd = dev->pd;
    /* An app-imported dma-buf/opaque fd keeps its own memory; it is not one of our shared pages. */
    for (const VkBaseInStructure *b = ai.pNext; b; b = b->pNext)
        if (b->sType == VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR) shared = 0;
    if (!shared || pd->info.strategy == VKB_MEM_HOSTPTR) cached = 0;
    uint32_t strategy = pd->info.strategy;
    int memfd = -1;
    void *ptr = NULL;
    if (shared && strategy == VKB_MEM_HOSTPTR) {
        VkDeviceSize a = vkb_import_align(pd->info.import_alignment); /* never below the kernel's page (16 KB option) */
        ai.allocationSize = (ai.allocationSize + a - 1) & ~(a - 1);
        memfd = vkb_memfd("vkbridge-memory", (size_t)ai.allocationSize);
        if (memfd < 0) return VK_ERROR_OUT_OF_HOST_MEMORY;
        ptr = mmap(NULL, (size_t)ai.allocationSize, PROT_READ | PROT_WRITE, MAP_SHARED, memfd, 0);
        if (ptr == MAP_FAILED) {
            close(memfd);
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
    }
    vkb_call c;
    vkb_call_begin(&c, VKB_CMD_vkAllocateMemory, dev->obj.table);
    vkb_enc_u64(&c.e, dev->obj.remote);
    vkb_enc_u8(&c.e, 1);
    vkb_enc_VkMemoryAllocateInfo(&c.e, &ai);
    vkb_enc_u32(&c.e, cached ? VKB_ALLOC_SHARED_CACHED : shared ? VKB_ALLOC_SHARED : VKB_ALLOC_PLAIN);
    vkb_enc_fd(&c.e, memfd);
    int ok = vkb_call_exec(&c);
    if (memfd >= 0) close(memfd);
    if (!ok) {
        if (ptr) munmap(ptr, (size_t)ai.allocationSize);
        return vkb_device_lost("vkAllocateMemory");
    }
    VkResult r = vkb_note_lost("vkAllocateMemory", (VkResult)vkb_dec_u32(&c.d));
    VkDeviceMemory mem = (VkDeviceMemory)vkb_dec_u64(&c.d);
    int fd = vkb_dec_fd(&c.d);
    uint64_t size = vkb_dec_u64(&c.d);
    vkb_call_end(&c);
    if (r != VK_SUCCESS) {
        static int told;
        if (__atomic_fetch_add(&told, 1, __ATOMIC_RELAXED) < 8)
            VKB_WARN("vkAllocateMemory of %llu bytes in memory type %u (flags 0x%x) = server type %u (flags 0x%x)%s failed: %d",
                     (unsigned long long)pAllocateInfo->allocationSize, ct, ct < pd->client_mem.memoryTypeCount ? pd->client_mem.memoryTypes[ct].propertyFlags : 0,
                     ai.memoryTypeIndex,
                     ai.memoryTypeIndex < pd->server_mem.memoryTypeCount ? pd->server_mem.memoryTypes[ai.memoryTypeIndex].propertyFlags : 0,
                     cached ? ", shared, cached" : shared ? ", shared" : "", r);
        if (ptr) munmap(ptr, (size_t)ai.allocationSize);
        if (fd >= 0) close(fd);
        return r;
    }
    /* The import succeeded: the fd the app handed over now belongs to the implementation, and the
     * server holds its own copy. */
    close_import_fds(pAllocateInfo->pNext);
    vkb_memory *m = calloc(1, sizeof(*m));
    m->client_type = ct;
    m->shared = shared;
    m->cached = cached;
    m->nosync = shared && !cached && ai.memoryTypeIndex < 32 && (pd->info.coherent_types & (1u << ai.memoryTypeIndex));
    m->size = shared && strategy == VKB_MEM_HOSTPTR ? ai.allocationSize : size;
    if (shared && strategy != VKB_MEM_HOSTPTR) {
        if (fd < 0) {
            VKB_ERR("shared allocation came back without its dma-buf");
        } else {
            ptr = mmap(NULL, (size_t)m->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            if (ptr == MAP_FAILED) {
                VKB_ERR("cannot map shared memory (%llu bytes): %s", (unsigned long long)m->size, strerror(errno));
                ptr = NULL;
            }
        }
    }
    if (fd >= 0) close(fd);
    m->ptr = ptr;
    vkb_map_put(&dev->memories, (uint64_t)mem, m);
    *pMemory = mem;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkb_ep_vkAllocateMemory(VkDevice device, const VkMemoryAllocateInfo *pAllocateInfo,
                                                       const VkAllocationCallbacks *pAllocator, VkDeviceMemory *pMemory)
{
    (void)pAllocator;
    vkb_physdev *pd = vkb_dev(device)->pd;
    uint32_t ct = pAllocateInfo->memoryTypeIndex;
    if (ct >= pd->client_mem.memoryTypeCount) {
        VKB_ONCE(VKB_LOG_WARN, "vkAllocateMemory in memory type %u: there are only %u", ct, pd->client_mem.memoryTypeCount);
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    VkMemoryAllocateInfo ai = *pAllocateInfo;
    ai.memoryTypeIndex = pd->type_map[ct].server_type;
    VkResult r = alloc_memory(device, pAllocateInfo, ai, ct, pd->type_map[ct].shared, pd->type_map[ct].cached, pMemory);
    /* Memory exported or imported as an fd: how a process without a swapchain of the bridge's
     * shows its frames (nested gamescope, Xwayland, Zink under X), for the report (frames.c). */
    for (const VkBaseInStructure *b = pAllocateInfo->pNext; b && r == VK_SUCCESS; b = b->pNext)
        if (b->sType == VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR ||
            (b->sType == VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO && ((const VkExportMemoryAllocateInfo *)b)->handleTypes)) {
            vkb_frames_external();
            break;
        }
    return r;
}

/* The client's own readback memory (client/wsi.c): `size` bytes for `buffer` in the hidden cached
 * readback type (pd->readback_type) when the server offers one and the buffer may live there. The
 * caller invalidates (vkInvalidateMappedMemoryRanges) before each read. VK_ERROR_FEATURE_NOT_PRESENT:
 * no such type here; allocate as usual. */
VkResult vkb_memory_alloc_readback(VkDevice device, VkBuffer buffer, VkDeviceSize size, VkDeviceMemory *pMemory)
{
    vkb_device *dev = vkb_dev(device);
    int t = dev->pd->readback_type;
    if (t < 0 || !buffer_shareable(dev, buffer)) return VK_ERROR_FEATURE_NOT_PRESENT;
    VkMemoryRequirements req;
    vkb_wire_vkGetBufferMemoryRequirements(device, buffer, &req); /* the server's types */
    if (!(req.memoryTypeBits & (1u << t))) return VK_ERROR_FEATURE_NOT_PRESENT;
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, size > req.size ? size : req.size, (uint32_t)t};
    return alloc_memory(device, &ai, ai, UINT32_MAX, 1, 1, pMemory);
}

/* 1 when memory has CPU-cached pages (reads need an invalidate first). */
int vkb_memory_is_cached(VkDevice device, VkDeviceMemory memory)
{
    vkb_memory *m = vkb_map_get(&vkb_dev(device)->memories, (uint64_t)memory);
    return m && m->cached;
}

VKAPI_ATTR void VKAPI_CALL vkb_ep_vkFreeMemory(VkDevice device, VkDeviceMemory memory, const VkAllocationCallbacks *pAllocator)
{
    (void)pAllocator;
    if (!memory) return;
    vkb_device *dev = vkb_dev(device);
    vkb_memory *m = vkb_map_remove(&dev->memories, (uint64_t)memory);
    vkb_call c;
    vkb_call_begin(&c, VKB_CMD_vkFreeMemory, dev->obj.table);
    vkb_enc_u64(&c.e, dev->obj.remote);
    vkb_enc_u64(&c.e, (uint64_t)memory);
    vkb_call_exec_async(&c);
    if (m) vkb_memory_unmap_free(m);
}

static void unplace(vkb_memory *m, int reserve);

VKAPI_ATTR VkResult VKAPI_CALL vkb_ep_vkMapMemory(VkDevice device, VkDeviceMemory memory, VkDeviceSize offset, VkDeviceSize size,
                                                  VkMemoryMapFlags flags, void **ppData)
{
    (void)size;
    (void)flags;
    vkb_memory *m = vkb_map_get(&vkb_dev(device)->memories, (uint64_t)memory);
    if (!m || !m->ptr) {
        VKB_ONCE(VKB_LOG_WARN, "vkMapMemory on memory that is not host-visible through the bridge (logged once)");
        *ppData = NULL;
        return VK_ERROR_MEMORY_MAP_FAILED;
    }
    *ppData = (uint8_t *)m->ptr + offset;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkb_ep_vkUnmapMemory(VkDevice device, VkDeviceMemory memory)
{
    /* The mapping stays until vkFreeMemory: mapping again is free, and nothing else reuses it.
     * A placed one leaves the app's range. */
    vkb_memory *m = vkb_map_get(&vkb_dev(device)->memories, (uint64_t)memory);
    if (m && m->placed) unplace(m, 0);
}

/* VK_EXT_map_memory_placed: Wine maps memory for 32-bit (WoW64) programs at addresses below 4 GB
 * it reserved itself; without it, Celeste (x86 .NET/FNA) got a mapping that "does not fit 32-bit
 * pointer" and crashed. The client already holds a mapping of every shared allocation, so placing
 * moves that mapping (mremap) to the app's address, and unmapping moves it back out, leaving the
 * app's range reserved (PROT_NONE) when asked to. Whole allocations only (no range placing). */
static size_t map_len(const vkb_memory *m)
{
    size_t page = vkb_page_size();
    return ((size_t)m->size + page - 1) & ~(page - 1);
}

static int move_mapping(vkb_memory *m, void *to)
{
    void *r = mremap(m->ptr, map_len(m), map_len(m), MREMAP_MAYMOVE | MREMAP_FIXED, to);
    if (r == MAP_FAILED) {
        VKB_ERR("placed mapping: mremap to %p failed: %s", to, strerror(errno));
        return 0;
    }
    m->ptr = r;
    return 1;
}

/* The mapping leaves the app's range; reserve = the range stays reserved, otherwise it is unmapped. */
static void unplace(vkb_memory *m, int reserve)
{
    void *old = m->ptr;
    size_t len = map_len(m);
    void *away = mmap(NULL, len, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (away == MAP_FAILED || !move_mapping(m, away)) {
        if (away != MAP_FAILED) munmap(away, len);
        return;
    }
    m->placed = 0;
    if (reserve) mmap(old, len, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0);
}

VKAPI_ATTR VkResult VKAPI_CALL vkb_ep_vkMapMemory2(VkDevice device, const VkMemoryMapInfo *pInfo, void **ppData)
{
    if (!(pInfo->flags & VK_MEMORY_MAP_PLACED_BIT_EXT))
        return vkb_ep_vkMapMemory(device, pInfo->memory, pInfo->offset, pInfo->size, pInfo->flags, ppData);
    const VkMemoryMapPlacedInfoEXT *pl = NULL;
    for (const VkBaseInStructure *b = pInfo->pNext; b; b = b->pNext)
        if (b->sType == VK_STRUCTURE_TYPE_MEMORY_MAP_PLACED_INFO_EXT) pl = (const void *)b;
    vkb_memory *m = vkb_map_get(&vkb_dev(device)->memories, (uint64_t)pInfo->memory);
    if (!m || !m->ptr || !pl || !pl->pPlacedAddress || pInfo->offset) {
        *ppData = NULL;
        return VK_ERROR_MEMORY_MAP_FAILED;
    }
    if (m->ptr != pl->pPlacedAddress && !move_mapping(m, pl->pPlacedAddress)) {
        *ppData = NULL;
        return VK_ERROR_MEMORY_MAP_FAILED;
    }
    m->placed = 1;
    *ppData = m->ptr;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkb_ep_vkUnmapMemory2(VkDevice device, const VkMemoryUnmapInfo *pInfo)
{
    vkb_memory *m = vkb_map_get(&vkb_dev(device)->memories, (uint64_t)pInfo->memory);
    if (m && m->placed) unplace(m, (pInfo->flags & VK_MEMORY_UNMAP_RESERVE_BIT_EXT) != 0);
    return VK_SUCCESS;
}

/*
 * Flushes and invalidates that change nothing, answered here. Wine's WoW64 OpenGL (32-bit games: Portal and RE6 under
 * WineD3D) backs each buffer with memory of its own Vulkan device in a HOST_COHERENT type and calls
 * vkFlushMappedMemoryRanges on every glFlushMappedBufferRange, coherent or not (dlls/opengl32/unix_wgl.c flush_buffer):
 * ~1,500 a frame in Portal's menu, ~94,000 round trips in 10 s on WineD3D's thread, 46 us each = 44% of its time. For
 * a HOST_COHERENT type the spec makes them no-ops, and here they are in fact: such a type is the bridge's shared memory
 * in a server type the self-test found coherent without flushes (server_memory.c test_type), its pages are the very
 * pages the GPU reads (uncached dma-heap pages, or the host pointers the test passed), nothing writes them through the
 * server's own mapping, and the driver's call on the server has no effect on them. So when every range is such memory
 * (nosync), the call returns VK_SUCCESS without a message - what the server answers for them (they are mapped once and
 * have no dma-buf to sync). Anything else - memory not known here, unshared (plain) memory, a shared type that needs
 * flushes, cached readback (DMA_BUF_IOCTL_SYNC on the server) - goes to the server as before.
 * On by default since the device A/B (new S10+, test58: Portal WineD3D +8.9% fps, RE6 +2%, no clock drop, same picture);
 * VKBRIDGE_FLUSH_ELIDE=0 turns it off for diagnosis only. The first answered
 * call says so once a process (with the memory's type), so a log shows which arm ran.
 */
static const int elide_default = 0; /* v0.5.5.4: off by default; VKBRIDGE_FLUSH_ELIDE=1 */

static int elide_on(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("VKBRIDGE_FLUSH_ELIDE");
        v = e && *e ? atoi(e) != 0 : elide_default;
    }
    return v;
}

static int ranges_nosync(vkb_device *dev, uint32_t n, const VkMappedMemoryRange *r)
{
    if (!n || !elide_on()) return 0;
    for (uint32_t i = 0; i < n; i++) {
        vkb_memory *m = vkb_map_get(&dev->memories, (uint64_t)r[i].memory);
        if (!m || !m->nosync) return 0;
    }
    return 1;
}

static VkResult ranges_call(VkDevice device, uint32_t cmd, uint32_t n, const VkMappedMemoryRange *r)
{
    vkb_device *dev = vkb_dev(device);
    if (ranges_nosync(dev, n, r)) {
        static int told;
        if (!__atomic_exchange_n(&told, 1, __ATOMIC_RELAXED)) {
            vkb_memory *m = vkb_map_get(&dev->memories, (uint64_t)r[0].memory);
            uint32_t ct = m ? m->client_type : UINT32_MAX;
            vkb_log_info_always("flush elide: %s of coherent shared memory answered in the client, no round trip (memory type %u, flags 0x%x; "
                                "VKBRIDGE_FLUSH_ELIDE=0 sends them)", vkb_cmd_names[cmd], ct,
                                ct < dev->pd->client_mem.memoryTypeCount ? dev->pd->client_mem.memoryTypes[ct].propertyFlags : 0);
        }
        return VK_SUCCESS;
    }
    vkb_call c;
    vkb_call_begin(&c, cmd, dev->obj.table);
    vkb_enc_u64(&c.e, dev->obj.remote);
    vkb_enc_u32(&c.e, n);
    for (uint32_t i = 0; i < n; i++) vkb_enc_VkMappedMemoryRange(&c.e, &r[i]);
    if (!vkb_call_exec(&c)) return vkb_device_lost(vkb_cmd_names[cmd]);
    VkResult res = (VkResult)vkb_dec_u32(&c.d);
    vkb_call_end(&c);
    return vkb_note_lost(vkb_cmd_names[cmd], res);
}

VKAPI_ATTR VkResult VKAPI_CALL vkb_ep_vkFlushMappedMemoryRanges(VkDevice device, uint32_t n, const VkMappedMemoryRange *r)
{
    return ranges_call(device, VKB_CMD_vkFlushMappedMemoryRanges, n, r);
}

VKAPI_ATTR VkResult VKAPI_CALL vkb_ep_vkInvalidateMappedMemoryRanges(VkDevice device, uint32_t n, const VkMappedMemoryRange *r)
{
    return ranges_call(device, VKB_CMD_vkInvalidateMappedMemoryRanges, n, r);
}
