/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * Reserved (sparse) buffers for vkd3d-proton on a driver without sparse memory (Mali: no
 * sparseBinding, no queue family with VK_QUEUE_SPARSE_BINDING_BIT).
 *
 * At feature level 12_0 vkd3d-proton makes D3D12 reserved buffers as Vulkan sparse buffers and
 * wants their device address at creation; the driver takes the sparse flags but gives such a buffer
 * no memory, so the address is 0 and the game stops at "Failed to get VA for sparse resource"
 * (Mega Man Star Force Legacy Collection, Elden Ring). Reserved textures are not
 * this layer's: without sparseResidencyImage2D vkd3d gives them full memory itself, and no sparse
 * feature is reported here, so that fallback stays on.
 *
 * What it does, for instances whose engine is vkd3d only:
 *   - the graphics queue families say VK_QUEUE_SPARSE_BINDING_BIT, so vkd3d makes its sparse queue
 *     (without one its tile-mapping path has no queue at all);
 *   - a buffer created with sparse flags is made without them and bound at once to memory of its
 *     own (with a device address when it asks for one): every tile is always resident;
 *   - vkQueueBindSparse binds nothing and becomes a submit with the bind's waits, signals (timeline
 *     values included) and fence, so everything waiting on it goes on.
 * Tile remaps don't move contents (one D3D12 heap tile shown in two resources is two copies), and
 * a tile mapped to nothing still reads its old contents: right for games that map once (Elden Ring
 * by ANV's experience), not for virtual-texture streaming.
 *
 * VKBRIDGE_EMU_SPARSE: auto (the default: on for vkd3d when the driver lacks sparseBinding), 1
 * (also where the driver has it, for host tests with VKBRIDGE_FAKE_MISSING=sparse), 0 (off).
 * VKBRIDGE_SPARSE_MAX_MB: the memory all of a device's reserved buffers may take (1024 by
 * default); past it a create fails with VK_ERROR_OUT_OF_DEVICE_MEMORY, said once.
 */
#define _GNU_SOURCE
#include "vkb_server.h"
#include "server_emu.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define SPARSE_ALIGN 65536u
#define SPARSE_MAX_MB_DEFAULT 1024u
#define SPARSE_FLAGS (VK_BUFFER_CREATE_SPARSE_BINDING_BIT | VK_BUFFER_CREATE_SPARSE_RESIDENCY_BIT | VK_BUFFER_CREATE_SPARSE_ALIASED_BIT)

typedef struct sp_buf {
    VkBuffer buf;
    VkDeviceMemory mem;
    VkDeviceSize size;
    struct sp_buf *next;
} sp_buf;

typedef struct sp_dev {
    pthread_mutex_t lock;
    sp_buf *bufs;
    VkDeviceSize used, budget;
    int over_said;
    uint64_t binds;
    PFN_vkCreateBuffer create;
    PFN_vkDestroyBuffer destroy;
    PFN_vkGetBufferMemoryRequirements reqs;
    PFN_vkGetBufferMemoryRequirements2 reqs2;
    PFN_vkQueueBindSparse bind;
    PFN_vkQueueSubmit submit;
} sp_dev;

static sp_dev *devs[VKB_MAX_TABLES];

/* -1: off, 0: auto, 1: on */
static int mode(void)
{
    const char *v = vkb_srv_env("VKBRIDGE_EMU_SPARSE");
    if (!v || !*v || !strcmp(v, "auto")) return 0;
    return strcmp(v, "0") ? 1 : -1;
}

static int engine_vkd3d(const vkb_srv_table *inst) { return inst && inst->vkd3d; }

/* Whether this instance's devices on pd get the emulation. */
static int wanted(vkb_srv_table *inst, VkPhysicalDevice pd)
{
    int m = mode();
    if (m < 0 || !engine_vkd3d(inst)) return 0;
    if (m > 0) return 1;
    VkPhysicalDeviceFeatures f;
    memset(&f, 0, sizeof(f));
    inst->real.vkGetPhysicalDeviceFeatures(pd, &f);
    return !f.sparseBinding || vkb_emu_faked("sparse");
}

/* ------------------------------------------------------------------ queue families (instance) */

static PFN_vkGetPhysicalDeviceQueueFamilyProperties next_qfp;
static PFN_vkGetPhysicalDeviceQueueFamilyProperties2 next_qfp2;

static VkQueueFlags flags_for(vkb_srv_table *inst, VkPhysicalDevice pd, VkQueueFlags f)
{
    if (vkb_emu_faked("sparse")) f &= ~VK_QUEUE_SPARSE_BINDING_BIT; /* host: as Mali reports it */
    if ((f & VK_QUEUE_GRAPHICS_BIT) && wanted(inst, pd)) f |= VK_QUEUE_SPARSE_BINDING_BIT;
    return f;
}

static VKAPI_ATTR void VKAPI_CALL sp_GetPhysicalDeviceQueueFamilyProperties(VkPhysicalDevice pd, uint32_t *n, VkQueueFamilyProperties *p)
{
    next_qfp(pd, n, p);
    vkb_srv_table *inst = vkb_srv_current_table();
    for (uint32_t i = 0; p && i < *n; i++) p[i].queueFlags = flags_for(inst, pd, p[i].queueFlags);
}

static VKAPI_ATTR void VKAPI_CALL sp_GetPhysicalDeviceQueueFamilyProperties2(VkPhysicalDevice pd, uint32_t *n, VkQueueFamilyProperties2 *p)
{
    next_qfp2(pd, n, p);
    vkb_srv_table *inst = vkb_srv_current_table();
    for (uint32_t i = 0; p && i < *n; i++)
        p[i].queueFamilyProperties.queueFlags = flags_for(inst, pd, p[i].queueFamilyProperties.queueFlags);
}

/* Host imitation of Mali's features: no sparse at all (VKBRIDGE_FAKE_MISSING=sparse). */
static PFN_vkGetPhysicalDeviceFeatures next_feat;
static PFN_vkGetPhysicalDeviceFeatures2 next_feat2;

static void no_sparse(VkPhysicalDeviceFeatures *f)
{
    f->sparseBinding = f->sparseResidencyBuffer = f->sparseResidencyImage2D = f->sparseResidencyImage3D = VK_FALSE;
    f->sparseResidency2Samples = f->sparseResidency4Samples = f->sparseResidency8Samples = VK_FALSE;
    f->sparseResidency16Samples = f->sparseResidencyAliased = f->shaderResourceResidency = VK_FALSE;
}

static VKAPI_ATTR void VKAPI_CALL sp_GetPhysicalDeviceFeatures(VkPhysicalDevice pd, VkPhysicalDeviceFeatures *f)
{
    next_feat(pd, f);
    no_sparse(f);
}

static VKAPI_ATTR void VKAPI_CALL sp_GetPhysicalDeviceFeatures2(VkPhysicalDevice pd, VkPhysicalDeviceFeatures2 *f)
{
    next_feat2(pd, f);
    no_sparse(&f->features);
}

void vkb_sparse_instance_install(vkb_srv_table *t)
{
    if (vkb_emu_faked("sparse")) {
        if (t->dt.vkGetPhysicalDeviceFeatures) next_feat = t->dt.vkGetPhysicalDeviceFeatures, t->dt.vkGetPhysicalDeviceFeatures = sp_GetPhysicalDeviceFeatures;
        if (t->dt.vkGetPhysicalDeviceFeatures2) next_feat2 = t->dt.vkGetPhysicalDeviceFeatures2, t->dt.vkGetPhysicalDeviceFeatures2 = sp_GetPhysicalDeviceFeatures2;
        VKB_ONCE(VKB_LOG_INFO, "imitating a driver without sparse memory (VKBRIDGE_FAKE_MISSING=sparse)");
    }
    /* the queue flags: the emulation's, and the imitation's (which takes it off) */
    if (mode() < 0 && !vkb_emu_faked("sparse")) return;
    if (t->dt.vkGetPhysicalDeviceQueueFamilyProperties) {
        next_qfp = t->dt.vkGetPhysicalDeviceQueueFamilyProperties;
        t->dt.vkGetPhysicalDeviceQueueFamilyProperties = sp_GetPhysicalDeviceQueueFamilyProperties;
    }
    if (t->dt.vkGetPhysicalDeviceQueueFamilyProperties2) {
        next_qfp2 = t->dt.vkGetPhysicalDeviceQueueFamilyProperties2;
        t->dt.vkGetPhysicalDeviceQueueFamilyProperties2 = sp_GetPhysicalDeviceQueueFamilyProperties2;
    }
}

/* ------------------------------------------------------------------ buffers (device) */

static sp_dev *cur(void)
{
    vkb_srv_table *t = vkb_srv_current_table();
    return t && t->id < VKB_MAX_TABLES ? devs[t->id] : NULL;
}

static uint32_t mem_type(vkb_srv_table *t, uint32_t bits)
{
    VkPhysicalDeviceMemoryProperties mp;
    t->real.vkGetPhysicalDeviceMemoryProperties(t->physical, &mp);
    for (int pass = 0; pass < 2; pass++)
        for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
            VkMemoryPropertyFlags f = mp.memoryTypes[i].propertyFlags;
            if (!(bits & (1u << i)) || (f & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT)) continue;
            if (pass == 0 && !(f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) continue;
            return i;
        }
    return UINT32_MAX;
}

static VKAPI_ATTR VkResult VKAPI_CALL sp_CreateBuffer(VkDevice d, const VkBufferCreateInfo *ci, const VkAllocationCallbacks *a, VkBuffer *out)
{
    sp_dev *s = cur();
    if (!s) return VK_ERROR_DEVICE_LOST;
    if (!(ci->flags & SPARSE_FLAGS)) return s->create(d, ci, a, out);
    vkb_srv_table *t = vkb_srv_current_table();
    VkBufferCreateInfo c = *ci;
    c.flags &= ~(VkBufferCreateFlags)SPARSE_FLAGS;
    c.size = (ci->size + SPARSE_ALIGN - 1) & ~(VkDeviceSize)(SPARSE_ALIGN - 1);
    pthread_mutex_lock(&s->lock);
    int over = s->used + c.size > s->budget;
    if (over && !s->over_said) {
        s->over_said = 1;
        VKB_WARN("sparse emulation: a reserved buffer of %llu MB refused: %llu of %llu MB already taken (VKBRIDGE_SPARSE_MAX_MB)",
                 (unsigned long long)(c.size >> 20), (unsigned long long)(s->used >> 20), (unsigned long long)(s->budget >> 20));
    }
    if (!over) s->used += c.size;
    pthread_mutex_unlock(&s->lock);
    if (over) return VK_ERROR_OUT_OF_DEVICE_MEMORY;

    VkBuffer b = VK_NULL_HANDLE;
    VkDeviceMemory m = VK_NULL_HANDLE;
    VkResult r = s->create(d, &c, a, &b);
    if (r == VK_SUCCESS) {
        VkMemoryRequirements mr;
        t->real.vkGetBufferMemoryRequirements(d, b, &mr);
        VkMemoryAllocateFlagsInfo fl = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, NULL, VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT, 0};
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, mem_type(t, mr.memoryTypeBits)};
        if (c.usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) ai.pNext = &fl;
        r = ai.memoryTypeIndex == UINT32_MAX ? VK_ERROR_OUT_OF_DEVICE_MEMORY : t->real.vkAllocateMemory(d, &ai, NULL, &m);
        if (r == VK_SUCCESS) r = t->real.vkBindBufferMemory(d, b, m, 0);
    }
    sp_buf *e = r == VK_SUCCESS ? calloc(1, sizeof(*e)) : NULL;
    if (!e) {
        if (m) t->real.vkFreeMemory(d, m, NULL);
        if (b) s->destroy(d, b, a);
        pthread_mutex_lock(&s->lock);
        s->used -= c.size;
        pthread_mutex_unlock(&s->lock);
        VKB_WARN("sparse emulation: a reserved buffer of %llu KB could not be backed (%d)", (unsigned long long)(c.size >> 10),
                 r == VK_SUCCESS ? VK_ERROR_OUT_OF_HOST_MEMORY : r);
        return r == VK_SUCCESS ? VK_ERROR_OUT_OF_HOST_MEMORY : r;
    }
    e->buf = b, e->mem = m, e->size = c.size;
    pthread_mutex_lock(&s->lock);
    e->next = s->bufs, s->bufs = e;
    pthread_mutex_unlock(&s->lock);
    VKB_ONCE(VKB_LOG_INFO, "sparse emulation: reserved buffers get all their memory at creation (first: %llu KB)",
             (unsigned long long)(c.size >> 10));
    *out = b;
    return VK_SUCCESS;
}

/* Takes the buffer's entry off the list; NULL when it is not a reserved one. */
static sp_buf *take(sp_dev *s, VkBuffer b)
{
    pthread_mutex_lock(&s->lock);
    sp_buf **pp = &s->bufs;
    while (*pp && (*pp)->buf != b) pp = &(*pp)->next;
    sp_buf *e = *pp;
    if (e) *pp = e->next, s->used -= e->size;
    pthread_mutex_unlock(&s->lock);
    return e;
}

static int is_reserved(sp_dev *s, VkBuffer b)
{
    pthread_mutex_lock(&s->lock);
    sp_buf *e = s->bufs;
    while (e && e->buf != b) e = e->next;
    pthread_mutex_unlock(&s->lock);
    return e != NULL;
}

static VKAPI_ATTR void VKAPI_CALL sp_DestroyBuffer(VkDevice d, VkBuffer b, const VkAllocationCallbacks *a)
{
    sp_dev *s = cur();
    if (!s) return;
    sp_buf *e = b ? take(s, b) : NULL;
    s->destroy(d, b, a);
    if (e) {
        vkb_srv_current_table()->real.vkFreeMemory(d, e->mem, NULL);
        free(e);
    }
}

/* The tile size vkd3d sizes its tiles by; the buffer needs no memory from the app. */
static void reserved_reqs(VkMemoryRequirements *r)
{
    r->alignment = r->alignment > SPARSE_ALIGN ? r->alignment : SPARSE_ALIGN;
    r->size = (r->size + r->alignment - 1) & ~(r->alignment - 1);
}

static VKAPI_ATTR void VKAPI_CALL sp_GetBufferMemoryRequirements(VkDevice d, VkBuffer b, VkMemoryRequirements *r)
{
    sp_dev *s = cur();
    if (!s) return;
    s->reqs(d, b, r);
    if (is_reserved(s, b)) reserved_reqs(r);
}

static VKAPI_ATTR void VKAPI_CALL sp_GetBufferMemoryRequirements2(VkDevice d, const VkBufferMemoryRequirementsInfo2 *i, VkMemoryRequirements2 *r)
{
    sp_dev *s = cur();
    if (!s) return;
    s->reqs2(d, i, r);
    if (is_reserved(s, i->buffer)) reserved_reqs(&r->memoryRequirements);
}

/* ------------------------------------------------------------------ binds */

/* Nothing to bind (every tile is resident); the bind's semaphores and fence are a submit's. */
static VKAPI_ATTR VkResult VKAPI_CALL sp_QueueBindSparse(VkQueue q, uint32_t n, const VkBindSparseInfo *bi, VkFence fence)
{
    sp_dev *s = cur();
    if (!s) return VK_ERROR_DEVICE_LOST;
    if (!n) return fence ? s->submit(q, 0, NULL, fence) : VK_SUCCESS;
    VkSubmitInfo *si = calloc(n, sizeof(*si));
    VkTimelineSemaphoreSubmitInfo *ti = calloc(n, sizeof(*ti));
    VkPipelineStageFlags *stages = NULL;
    uint32_t waits = 0;
    for (uint32_t i = 0; i < n; i++) waits += bi[i].waitSemaphoreCount;
    stages = calloc(waits ? waits : 1, sizeof(*stages));
    if (!si || !ti || !stages) {
        free(si), free(ti), free(stages);
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    uint32_t w = 0;
    for (uint32_t i = 0; i < n; i++) {
        si[i].sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si[i].waitSemaphoreCount = bi[i].waitSemaphoreCount;
        si[i].pWaitSemaphores = bi[i].pWaitSemaphores;
        si[i].pWaitDstStageMask = stages + w;
        for (uint32_t k = 0; k < bi[i].waitSemaphoreCount; k++) stages[w++] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        si[i].signalSemaphoreCount = bi[i].signalSemaphoreCount;
        si[i].pSignalSemaphores = bi[i].pSignalSemaphores;
        for (const VkBaseInStructure *p = bi[i].pNext; p; p = p->pNext)
            if (p->sType == VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO) {
                ti[i] = *(const VkTimelineSemaphoreSubmitInfo *)p;
                ti[i].pNext = NULL;
                si[i].pNext = &ti[i];
            }
        __atomic_add_fetch(&s->binds, bi[i].bufferBindCount + bi[i].imageOpaqueBindCount + bi[i].imageBindCount, __ATOMIC_RELAXED);
    }
    VKB_ONCE(VKB_LOG_INFO, "sparse emulation: vkQueueBindSparse answered by a submit of its semaphores and fence (tiles stay resident)");
    VkResult r = s->submit(q, n, si, fence);
    free(si), free(ti), free(stages);
    return r;
}

/* ------------------------------------------------------------------ install */

void vkb_sparse_device_init(vkb_srv_table *t)
{
    if (!t || t->id >= VKB_MAX_TABLES || !wanted(t->parent, t->physical)) return;
    sp_dev *s = calloc(1, sizeof(*s));
    if (!s) return;
    pthread_mutex_init(&s->lock, NULL);
    const char *v = vkb_srv_env("VKBRIDGE_SPARSE_MAX_MB");
    s->budget = (VkDeviceSize)(v && *v ? strtoull(v, NULL, 10) : SPARSE_MAX_MB_DEFAULT) << 20;
    s->create = t->dt.vkCreateBuffer, t->dt.vkCreateBuffer = sp_CreateBuffer;
    s->destroy = t->dt.vkDestroyBuffer, t->dt.vkDestroyBuffer = sp_DestroyBuffer;
    s->reqs = t->dt.vkGetBufferMemoryRequirements, t->dt.vkGetBufferMemoryRequirements = sp_GetBufferMemoryRequirements;
    if (t->dt.vkGetBufferMemoryRequirements2)
        s->reqs2 = t->dt.vkGetBufferMemoryRequirements2, t->dt.vkGetBufferMemoryRequirements2 = sp_GetBufferMemoryRequirements2;
    s->submit = t->dt.vkQueueSubmit;
    s->bind = t->dt.vkQueueBindSparse, t->dt.vkQueueBindSparse = sp_QueueBindSparse;
    devs[t->id] = s;
    VKB_INFO("device %u: sparse emulation on (vkd3d-proton's reserved buffers backed in full, budget %llu MB)", t->id,
             (unsigned long long)(s->budget >> 20));
}

void vkb_sparse_device_destroy(vkb_srv_table *t)
{
    sp_dev *s = t && t->id < VKB_MAX_TABLES ? devs[t->id] : NULL;
    if (!s) return;
    devs[t->id] = NULL;
    for (sp_buf *e = s->bufs, *nx; e; e = nx) {
        nx = e->next;
        t->real.vkDestroyBuffer(t->device, e->buf, NULL);
        t->real.vkFreeMemory(t->device, e->mem, NULL);
        free(e);
    }
    if (s->binds) VKB_INFO("device %u: sparse emulation: %llu bind(s) answered", t->id, (unsigned long long)s->binds);
    pthread_mutex_destroy(&s->lock);
    free(s);
}
