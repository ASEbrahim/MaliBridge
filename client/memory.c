/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Device memory on the client side: the memory-type translation of physdev.c applied to every
 * allocation and requirement, and mapping of shared allocations (server_memory.c explains how
 * they are shared).
 */
#define _GNU_SOURCE
#include "vkb_client.h"
#include "gen/vkb_client_gen.h"
#include "emu.h"

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
    VkMemoryRequirements req;
    VkBool32 prefers_dedicated, requires_dedicated;
    struct rq_entry *next;
} rq_entry;

typedef struct vkb_rq_cache {
    pthread_mutex_t lock;
    vkb_map buffers;              /* VkBuffer -> rq_entry* (entries live as long as the device) */
    rq_entry *buckets[RQ_HB];
    uint32_t count;
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
 * VKBRIDGE_DEDICATED_HINT=1 clears it, =0 passes the driver's answer through; default: 1 on Arm
 * (Mali) GPUs, 0 on others (their drivers' preference is not known to cost anything).
 */
#define DED_KEEP_MIN ((VkDeviceSize)32 << 20)

void vkb_memory_init_device(vkb_device *d)
{
    vkb_rq_cache *c = calloc(1, sizeof(*c));
    if (c) {
        pthread_mutex_init(&c->lock, NULL);
        vkb_map_init(&c->buffers);
        vkb_map_init(&c->ded_keep);
        const char *e = getenv("VKBRIDGE_DEDICATED_HINT");
        c->ded_clear = e && *e ? atoi(e) != 0 : d->pd->props.vendorID == 0x13B5;
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
    vkb_map_destroy(&c->buffers);
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

/* The kept answer for buffer, if any: 1 = filled in. dedicated may be NULL. */
static int rq_lookup(vkb_device *dev, VkBuffer buffer, VkMemoryRequirements *req, VkMemoryDedicatedRequirements *dedicated)
{
    vkb_rq_cache *c = dev->buf_reqs;
    rq_entry *e = c ? vkb_map_get(&c->buffers, (uint64_t)buffer) : NULL;
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

static void rq_store(vkb_device *dev, VkBuffer buffer, const VkMemoryRequirements *req, const VkMemoryDedicatedRequirements *dedicated)
{
    vkb_rq_cache *c = dev->buf_reqs;
    rq_entry *e = c ? vkb_map_get(&c->buffers, (uint64_t)buffer) : NULL;
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

static int can_share(vkb_device *dev)
{
    return dev->pd->info.strategy != VKB_MEM_NONE && dev->pd->info.buffer_handle_types;
}

static VkResult create_buffer(VkDevice device, const VkBufferCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator,
                              VkBuffer *pBuffer)
{
    vkb_device *dev = vkb_dev(device);
    if (!can_share(dev) || (pCreateInfo->flags & VK_BUFFER_CREATE_SPARSE_BINDING_BIT)) {
        VkResult r = vkb_wire_vkCreateBuffer(device, pCreateInfo, pAllocator, pBuffer);
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
        r = vkb_wire_vkCreateBuffer(device, &ci, pAllocator, pBuffer);
        free(mem);
        if (r != VK_SUCCESS && r != VK_ERROR_OUT_OF_HOST_MEMORY && r != VK_ERROR_OUT_OF_DEVICE_MEMORY) {
            r = vkb_wire_vkCreateBuffer(device, pCreateInfo, pAllocator, pBuffer);
            if (r == VK_SUCCESS) vkb_map_put(&dev->unshared, (uint64_t)*pBuffer, (void *)1);
        }
        return r;
    }
    ci.pNext = &ext;
    r = vkb_wire_vkCreateBuffer(device, &ci, pAllocator, pBuffer);
    if (r != VK_SUCCESS && r != VK_ERROR_OUT_OF_HOST_MEMORY && r != VK_ERROR_OUT_OF_DEVICE_MEMORY) {
        VKB_ONCE(VKB_LOG_WARN, "a buffer cannot be created shareable (%d); it gets device-only memory (logged once)", r);
        r = vkb_wire_vkCreateBuffer(device, pCreateInfo, pAllocator, pBuffer);
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
    if (can_share(dev) && ci.tiling == VK_IMAGE_TILING_LINEAR && !(ci.flags & VK_IMAGE_CREATE_SPARSE_BINDING_BIT)) {
        int had = 0;
        for (VkBaseOutStructure *b = (VkBaseOutStructure *)ci.pNext; b; b = b->pNext)
            had |= b->sType == VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
        if (!had) {
            VkExternalMemoryImageCreateInfo ext = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, ci.pNext,
                                                   dev->pd->info.buffer_handle_types};
            VkImageCreateInfo c2 = ci;
            c2.pNext = &ext;
            VkResult r = vkb_wire_vkCreateImage(device, &c2, pAllocator, pImage);
            if (r == VK_SUCCESS) {
                vkb_map_put(&dev->shared_images, (uint64_t)*pImage, (void *)1);
                vkb_emu_image_created(dev, pCreateInfo, *pImage);
                ded_track(dev, (uint64_t)*pImage, pCreateInfo->pNext, 0);
                return r;
            }
        }
    }
    VkResult r = vkb_wire_vkCreateImage(device, &ci, pAllocator, pImage);
    if (r == VK_SUCCESS) {
        vkb_emu_image_created(dev, pCreateInfo, *pImage);
        ded_track(dev, (uint64_t)*pImage, pCreateInfo->pNext, pCreateInfo->tiling == VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT);
    }
    return r;
}

VKAPI_ATTR void VKAPI_CALL vkb_ep_vkDestroyImage(VkDevice device, VkImage image, const VkAllocationCallbacks *pAllocator)
{
    if (!image) return;
    vkb_device *dev = vkb_dev(device);
    vkb_map_remove(&dev->shared_images, (uint64_t)image);
    ded_forget(dev, (uint64_t)image);
    vkb_emu_image_destroyed(dev, image);
    vkb_wire_vkDestroyImage(device, image, pAllocator);
}

/* ------------------------------------------------------------------ requirements */

static void fix_reqs(vkb_device *dev, VkMemoryRequirements *r, int allow_shared)
{
    r->memoryTypeBits = vkb_mem_bits_to_client(dev->pd, r->memoryTypeBits, allow_shared);
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
    fix_reqs(dev, pReq, buffer_shareable(dev, buffer));
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
        fix_reqs(dev, &pReq->memoryRequirements, buffer_shareable(dev, pInfo->buffer));
        if (plain) rq_store(dev, pInfo->buffer, &pReq->memoryRequirements, ded);
    }
    ded_hint(dev, pReq, ded_kept(dev, (uint64_t)pInfo->buffer));
}

VKAPI_ATTR void VKAPI_CALL vkb_ep_vkGetImageMemoryRequirements(VkDevice device, VkImage image, VkMemoryRequirements *pReq)
{
    vkb_wire_vkGetImageMemoryRequirements(device, image, pReq);
    fix_reqs(vkb_dev(device), pReq, image_shareable(vkb_dev(device), image));
}

VKAPI_ATTR void VKAPI_CALL vkb_ep_vkGetImageMemoryRequirements2(VkDevice device, const VkImageMemoryRequirementsInfo2 *pInfo,
                                                                VkMemoryRequirements2 *pReq)
{
    vkb_device *dev = vkb_dev(device);
    vkb_wire_vkGetImageMemoryRequirements2(device, pInfo, pReq);
    fix_reqs(dev, &pReq->memoryRequirements, image_shareable(dev, pInfo->image));
    /* A plane of a disjoint image (VkImagePlaneMemoryRequirementsInfo) is treated alike. */
    ded_hint(dev, pReq, ded_kept(dev, (uint64_t)pInfo->image));
}

VKAPI_ATTR void VKAPI_CALL vkb_ep_vkGetDeviceBufferMemoryRequirements(VkDevice device, const VkDeviceBufferMemoryRequirements *pInfo,
                                                                      VkMemoryRequirements2 *pReq)
{
    vkb_device *dev = vkb_dev(device);
    if (!can_share(dev)) {
        vkb_wire_vkGetDeviceBufferMemoryRequirements(device, pInfo, pReq);
        fix_reqs(dev, &pReq->memoryRequirements, 0);
        ded_hint(dev, pReq, ded_external(pInfo->pCreateInfo->pNext));
        return;
    }
    VkBufferCreateInfo ci = *pInfo->pCreateInfo;
    VkExternalMemoryBufferCreateInfo ext = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO, ci.pNext,
                                            dev->pd->info.buffer_handle_types};
    int had = 0;
    for (VkBaseOutStructure *b = (VkBaseOutStructure *)ci.pNext; b; b = b->pNext)
        had |= b->sType == VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO;
    if (!had) ci.pNext = &ext;
    VkDeviceBufferMemoryRequirements info = *pInfo;
    info.pCreateInfo = &ci;
    vkb_wire_vkGetDeviceBufferMemoryRequirements(device, &info, pReq);
    fix_reqs(dev, &pReq->memoryRequirements, 1);
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
        VkDeviceSize a = pd->info.import_alignment ? pd->info.import_alignment : 4096;
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
        return VK_ERROR_DEVICE_LOST;
    }
    VkResult r = (VkResult)vkb_dec_u32(&c.d);
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
    return alloc_memory(device, pAllocateInfo, ai, ct, pd->type_map[ct].shared, pd->type_map[ct].cached, pMemory);
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
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
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

static VkResult ranges_call(VkDevice device, uint32_t cmd, uint32_t n, const VkMappedMemoryRange *r)
{
    vkb_device *dev = vkb_dev(device);
    vkb_call c;
    vkb_call_begin(&c, cmd, dev->obj.table);
    vkb_enc_u64(&c.e, dev->obj.remote);
    vkb_enc_u32(&c.e, n);
    for (uint32_t i = 0; i < n; i++) vkb_enc_VkMappedMemoryRange(&c.e, &r[i]);
    if (!vkb_call_exec(&c)) return VK_ERROR_DEVICE_LOST;
    VkResult res = (VkResult)vkb_dec_u32(&c.d);
    vkb_call_end(&c);
    return res;
}

VKAPI_ATTR VkResult VKAPI_CALL vkb_ep_vkFlushMappedMemoryRanges(VkDevice device, uint32_t n, const VkMappedMemoryRange *r)
{
    return ranges_call(device, VKB_CMD_vkFlushMappedMemoryRanges, n, r);
}

VKAPI_ATTR VkResult VKAPI_CALL vkb_ep_vkInvalidateMappedMemoryRanges(VkDevice device, uint32_t n, const VkMappedMemoryRange *r)
{
    return ranges_call(device, VKB_CMD_vkInvalidateMappedMemoryRanges, n, r);
}
