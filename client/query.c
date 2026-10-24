/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * Query results answered from a client-side mirror.
 *
 * Deep Rock Galactic (DX11 through DXVK 3.1, tablet) made ~10,400 vkGetQueryPoolResults round
 * trips a second, occlusion and timestamp queries polled one by one, most of them already ready:
 * each one blocks a game thread for 50-100 us, and the game ran at 13.9 fps with the GPU mostly
 * idle. The quiet window in icd.c only helps polls that come back not ready.
 *
 * So every occlusion/timestamp pool gets a small buffer in shared, verified-coherent memory:
 *   u64 value[count]   the query's result, written by the GPU
 *   u32 ready[count]   nonzero once value[] holds the current result
 * A primary command buffer that ends queries (vkCmdEndQuery[Indexed], vkCmdWriteTimestamp[2],
 * including those of the secondaries it executes) gets, appended at vkEndCommandBuffer:
 *   barrier; vkCmdCopyQueryPoolResults(64-bit, WAIT) -> value[]; barrier; vkCmdFillBuffer(1) -> ready[];
 *   barrier transfer -> host
 * The separate ready word is written only after the values are (a barrier apart), so a reader
 * that sees it set never sees a stale or torn value. vkResetQueryPool clears ready[] from the
 * host; vkCmdResetQueryPool is followed by a vkCmdFillBuffer(0) of ready[] at the same place in
 * the stream (both legal only outside render passes), which, like the reset itself, takes effect
 * when it executes.
 *
 * A query is answered from the mirror only while it is "mirrored": set when a command buffer
 * appended its copy and by a host reset (ready cleared), cleared by a command buffer that ended it
 * but could not copy it (a queue family without graphics/compute). Anything else (unmirrored queries, other
 * query types, VK_QUERY_RESULT_WITH_STATUS_BIT_KHR, WAIT on a query not ready yet) goes to the
 * server as before. Limit: a multiview render pass ends N consecutive queries per vkCmdEndQuery;
 * only the first is copied, so the others are right only if they were never mirrored before.
 *
 * VKBRIDGE_QUERY_MIRROR=0 turns this off.
 */
#define _GNU_SOURCE
#include "vkb_client.h"
#include "gen/vkb_client_gen.h"

#include <string.h>

typedef struct vkb_qmirror {
    VkQueryPool pool;
    uint32_t count;
    VkBuffer buf;
    VkDeviceMemory mem;
    uint64_t *value;
    uint32_t *ready;
    VkDeviceSize ready_off;
    uint8_t *mirrored;       /* per query, atomic */
} vkb_qmirror;

typedef struct vkb_qop {
    vkb_qmirror *m;
    uint32_t first, count;
    uint32_t reset;          /* 0: ended (count 1), 1: reset on the GPU */
} vkb_qop;

typedef struct vkb_qm_dev {
    vkb_map pools;           /* VkQueryPool -> vkb_qmirror* */
    pthread_mutex_t lock;
    int disabled;            /* no coherent shared memory type, or switched off */
    int fam_known;
    uint32_t fam_count;
    VkQueueFlags *fam_flags;
} vkb_qm_dev;

static int mirror_enabled(void)
{
    static int v = -1;
    if (v < 0) {
        v = vkb_query_mirror_wanted(); /* the guest's VKBRIDGE_QUERY_MIRROR, else the server's switch */
    }
    return v;
}

void vkb_query_init_device(vkb_device *d)
{
    vkb_qm_dev *qm = calloc(1, sizeof(*qm));
    if (!qm) return;
    vkb_map_init(&qm->pools);
    pthread_mutex_init(&qm->lock, NULL);
    qm->disabled = !mirror_enabled() || d->pd->info.strategy == VKB_MEM_NONE || !d->pd->info.coherent_types;
    d->qm = qm;
}

static void mirror_free(vkb_device *d, vkb_qmirror *m)
{
    extern VKAPI_ATTR void VKAPI_CALL vkb_ep_vkDestroyBuffer(VkDevice, VkBuffer, const VkAllocationCallbacks *);
    extern VKAPI_ATTR void VKAPI_CALL vkb_ep_vkFreeMemory(VkDevice, VkDeviceMemory, const VkAllocationCallbacks *);
    if (m->buf) vkb_ep_vkDestroyBuffer((VkDevice)d, m->buf, NULL);
    if (m->mem) vkb_ep_vkFreeMemory((VkDevice)d, m->mem, NULL);
    free(m->mirrored);
    free(m);
}

void vkb_query_destroy_device(vkb_device *d)
{
    vkb_qm_dev *qm = d->qm;
    if (!qm) return;
    /* Pools the app never destroyed. */
    for (uint32_t i = 0; i < qm->pools.cap; i++)
        if (qm->pools.keys[i] && qm->pools.keys[i] != ~(uint64_t)0) mirror_free(d, qm->pools.vals[i]);
    vkb_map_destroy(&qm->pools);
    pthread_mutex_destroy(&qm->lock);
    free(qm->fam_flags);
    free(qm);
    d->qm = NULL;
}

static vkb_qmirror *mirror_of(vkb_device *d, VkQueryPool pool)
{
    return d->qm && pool ? vkb_map_get(&d->qm->pools, (uint64_t)pool) : NULL;
}

static VkQueueFlags family_flags(vkb_device *d, uint32_t family)
{
    vkb_qm_dev *qm = d->qm;
    pthread_mutex_lock(&qm->lock);
    if (!qm->fam_known) {
        uint32_t n = 0;
        vkb_wire_vkGetPhysicalDeviceQueueFamilyProperties((VkPhysicalDevice)d->pd, &n, NULL);
        VkQueueFamilyProperties *p = calloc(n ? n : 1, sizeof(*p));
        qm->fam_flags = calloc(n ? n : 1, sizeof(*qm->fam_flags));
        if (p && qm->fam_flags) {
            vkb_wire_vkGetPhysicalDeviceQueueFamilyProperties((VkPhysicalDevice)d->pd, &n, p);
            for (uint32_t i = 0; i < n; i++) qm->fam_flags[i] = p[i].queueFlags;
            qm->fam_count = n;
        }
        free(p);
        qm->fam_known = 1;
    }
    VkQueueFlags f = family < qm->fam_count ? qm->fam_flags[family] : 0;
    pthread_mutex_unlock(&qm->lock);
    return f;
}

/* ------------------------------------------------------------------ pools */

static vkb_qmirror *mirror_create(vkb_device *d, VkQueryPool pool, uint32_t count)
{
    extern VKAPI_ATTR VkResult VKAPI_CALL vkb_ep_vkCreateBuffer(VkDevice, const VkBufferCreateInfo *, const VkAllocationCallbacks *, VkBuffer *);
    extern VKAPI_ATTR void VKAPI_CALL vkb_ep_vkGetBufferMemoryRequirements(VkDevice, VkBuffer, VkMemoryRequirements *);
    extern VKAPI_ATTR VkResult VKAPI_CALL vkb_ep_vkAllocateMemory(VkDevice, const VkMemoryAllocateInfo *, const VkAllocationCallbacks *,
                                                                  VkDeviceMemory *);
    VkDevice device = (VkDevice)d;
    vkb_physdev *pd = d->pd;
    vkb_qmirror *m = calloc(1, sizeof(*m));
    if (!m || !(m->mirrored = calloc(count, 1))) {
        free(m);
        return NULL;
    }
    m->pool = pool;
    m->count = count;
    m->ready_off = (VkDeviceSize)count * 8;
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, m->ready_off + (VkDeviceSize)count * 4,
                              VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
    if (vkb_ep_vkCreateBuffer(device, &bci, NULL, &m->buf) != VK_SUCCESS) goto fail;
    VkMemoryRequirements req;
    vkb_ep_vkGetBufferMemoryRequirements(device, m->buf, &req);
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < pd->client_mem.memoryTypeCount && type == UINT32_MAX; i++) {
        if (!(req.memoryTypeBits & (1u << i)) || !pd->type_map[i].shared) continue;
        if (!(pd->info.coherent_types & (1u << pd->type_map[i].server_type))) continue;
        if (pd->client_mem.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) type = i;
    }
    if (type == UINT32_MAX) {
        VKB_INFO("query mirror: no coherent shared memory type for a buffer; query results stay on the wire");
        pthread_mutex_lock(&d->qm->lock);
        d->qm->disabled = 1;
        pthread_mutex_unlock(&d->qm->lock);
        goto fail;
    }
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size, type};
    if (vkb_ep_vkAllocateMemory(device, &ai, NULL, &m->mem) != VK_SUCCESS) goto fail;
    if (vkb_wire_vkBindBufferMemory(device, m->buf, m->mem, 0) != VK_SUCCESS) goto fail;
    vkb_memory *vm = vkb_map_get(&d->memories, (uint64_t)m->mem);
    if (!vm || !vm->ptr) goto fail;
    memset(vm->ptr, 0, (size_t)bci.size);
    m->value = (uint64_t *)vm->ptr;
    m->ready = (uint32_t *)((uint8_t *)vm->ptr + m->ready_off);
    return m;
fail:
    VKB_ONCE(VKB_LOG_WARN, "query mirror: a pool could not get its mirror; its results stay on the wire (logged once)");
    mirror_free(d, m);
    return NULL;
}

VKAPI_ATTR VkResult VKAPI_CALL vkb_ep_vkCreateQueryPool(VkDevice device, const VkQueryPoolCreateInfo *pCreateInfo,
                                                        const VkAllocationCallbacks *pAllocator, VkQueryPool *pQueryPool)
{
    VkResult r = vkb_wire_vkCreateQueryPool(device, pCreateInfo, pAllocator, pQueryPool);
    vkb_device *d = vkb_dev(device);
    if (r != VK_SUCCESS || !d->qm || d->qm->disabled || !pCreateInfo->queryCount) return r;
    if (pCreateInfo->queryType != VK_QUERY_TYPE_OCCLUSION && pCreateInfo->queryType != VK_QUERY_TYPE_TIMESTAMP) return r;
    vkb_qmirror *m = mirror_create(d, *pQueryPool, pCreateInfo->queryCount);
    if (m) vkb_map_put(&d->qm->pools, (uint64_t)*pQueryPool, m);
    return r;
}

/* The spec has every command that uses the pool completed before it is destroyed, and the
 * mirror's commands sit in those same command buffers: freeing it now is safe. */
VKAPI_ATTR void VKAPI_CALL vkb_ep_vkDestroyQueryPool(VkDevice device, VkQueryPool queryPool, const VkAllocationCallbacks *pAllocator)
{
    if (!queryPool) return;
    vkb_device *d = vkb_dev(device);
    vkb_wire_vkDestroyQueryPool(device, queryPool, pAllocator);
    vkb_qmirror *m = d->qm ? vkb_map_remove(&d->qm->pools, (uint64_t)queryPool) : NULL;
    if (m) mirror_free(d, m);
}

/* DXVK resets queries from the host (~7,000 a second in Deep Rock Galactic). */
VKAPI_ATTR void VKAPI_CALL vkb_ep_vkResetQueryPool(VkDevice device, VkQueryPool queryPool, uint32_t firstQuery, uint32_t queryCount)
{
    vkb_qmirror *m = mirror_of(vkb_dev(device), queryPool);
    if (m && firstQuery < m->count) {
        uint32_t end = queryCount > m->count - firstQuery ? m->count : firstQuery + queryCount;
        /* A reset query is unavailable until it is ended and that work runs, which is what the
         * cleared ready word says: it stays mirrored. DXVK polls queries whose command buffer is
         * still recording; sent to the server, those were ~5,400 round trips a second in DRG. A
         * command buffer that ends one without being able to copy it unmirrors it (below). */
        for (uint32_t q = firstQuery; q < end; q++) {
            __atomic_store_n(&m->ready[q], 0, __ATOMIC_RELEASE);
            __atomic_store_n(&m->mirrored[q], 1, __ATOMIC_RELEASE);
        }
    }
    vkb_wire_vkResetQueryPool(device, queryPool, firstQuery, queryCount);
}

/* ------------------------------------------------------------------ recording */

static void qop_add(vkb_cmdbuf *cb, vkb_qmirror *m, uint32_t first, uint32_t count, uint32_t reset)
{
    if (cb->nqops == cb->qops_cap) {
        uint32_t cap = cb->qops_cap ? cb->qops_cap * 2 : 32;
        vkb_qop *n = realloc(cb->qops, cap * sizeof(*n));
        if (!n) {
            /* Cannot track it: a lost reset could make a later WAIT copy hang, so the command
             * buffer fails (out of host memory) and nothing is copied. */
            cb->broken = 1;
            if (!reset)
                for (uint32_t q = first; q < first + count && q < m->count; q++) __atomic_store_n(&m->mirrored[q], 0, __ATOMIC_RELEASE);
            return;
        }
        cb->qops = n;
        cb->qops_cap = cap;
    }
    cb->qops[cb->nqops++] = (vkb_qop){m, first, count, reset};
}

void vkb_query_cb_clear(vkb_cmdbuf *cb)
{
    cb->nqops = 0;
    if (cb->qops_cap > 4096) {
        free(cb->qops);
        cb->qops = NULL;
        cb->qops_cap = 0;
    }
}

static void ended(VkCommandBuffer commandBuffer, VkQueryPool queryPool, uint32_t query)
{
    vkb_cmdbuf *cb = vkb_cb(commandBuffer);
    vkb_qmirror *m = mirror_of(cb->device, queryPool);
    if (m && cb->recording && query < m->count) qop_add(cb, m, query, 1, 0);
}

static const VkMemoryBarrier xfer_xfer = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT,
                                          VK_ACCESS_TRANSFER_WRITE_BIT};

VKAPI_ATTR void VKAPI_CALL vkb_ep_vkCmdResetQueryPool(VkCommandBuffer commandBuffer, VkQueryPool queryPool, uint32_t firstQuery,
                                                      uint32_t queryCount)
{
    vkb_wire_vkCmdResetQueryPool(commandBuffer, queryPool, firstQuery, queryCount);
    vkb_cmdbuf *cb = vkb_cb(commandBuffer);
    vkb_qmirror *m = mirror_of(cb->device, queryPool);
    if (!m || !cb->recording || firstQuery >= m->count) return;
    uint32_t n = queryCount > m->count - firstQuery ? m->count - firstQuery : queryCount;
    /* After earlier writes of the mirror (earlier command buffers' copies and fills). */
    vkb_wire_vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &xfer_xfer, 0, NULL,
                                  0, NULL);
    vkb_wire_vkCmdFillBuffer(commandBuffer, m->buf, m->ready_off + (VkDeviceSize)firstQuery * 4, (VkDeviceSize)n * 4, 0);
    qop_add(cb, m, firstQuery, n, 1);
}

VKAPI_ATTR void VKAPI_CALL vkb_ep_vkCmdEndQuery(VkCommandBuffer commandBuffer, VkQueryPool queryPool, uint32_t query)
{
    vkb_wire_vkCmdEndQuery(commandBuffer, queryPool, query);
    ended(commandBuffer, queryPool, query);
}

VKAPI_ATTR void VKAPI_CALL vkb_ep_vkCmdEndQueryIndexedEXT(VkCommandBuffer commandBuffer, VkQueryPool queryPool, uint32_t query,
                                                          uint32_t index)
{
    vkb_wire_vkCmdEndQueryIndexedEXT(commandBuffer, queryPool, query, index);
    ended(commandBuffer, queryPool, query);
}

VKAPI_ATTR void VKAPI_CALL vkb_ep_vkCmdWriteTimestamp(VkCommandBuffer commandBuffer, VkPipelineStageFlagBits pipelineStage,
                                                      VkQueryPool queryPool, uint32_t query)
{
    vkb_wire_vkCmdWriteTimestamp(commandBuffer, pipelineStage, queryPool, query);
    ended(commandBuffer, queryPool, query);
}

VKAPI_ATTR void VKAPI_CALL vkb_ep_vkCmdWriteTimestamp2(VkCommandBuffer commandBuffer, VkPipelineStageFlags2 stage, VkQueryPool queryPool,
                                                       uint32_t query)
{
    vkb_wire_vkCmdWriteTimestamp2(commandBuffer, stage, queryPool, query);
    ended(commandBuffer, queryPool, query);
}

/* A secondary's queries are copied by the primary that executes it (each time it does). */
void vkb_query_cb_execute(vkb_cmdbuf *primary, const vkb_cmdbuf *secondary)
{
    if (!primary->recording) return;
    for (uint32_t i = 0; i < secondary->nqops; i++) {
        const vkb_qop *o = &secondary->qops[i];
        qop_add(primary, o->m, o->first, o->count, o->reset);
    }
}

static int item_cmp(const void *a, const void *b)
{
    const vkb_qop *x = a, *y = b;
    if (x->m != y->m) return (uintptr_t)x->m < (uintptr_t)y->m ? -1 : 1;
    return x->first < y->first ? -1 : x->first > y->first;
}

/* Called by vkEndCommandBuffer while the command buffer still records. */
void vkb_query_cb_end(vkb_cmdbuf *cb)
{
    if (!cb->nqops) return;
    if (cb->level != VK_COMMAND_BUFFER_LEVEL_PRIMARY) return; /* kept for vkCmdExecuteCommands */
    VkCommandBuffer commandBuffer = (VkCommandBuffer)cb;
    /* Queries ended and not reset again later in the command buffer: those are available at its
     * end (a WAIT copy of any other one could wait forever). Walk backwards collecting resets. */
    vkb_qop *items = malloc(cb->nqops * sizeof(*items));
    vkb_qop *resets = malloc(cb->nqops * sizeof(*resets));
    uint32_t ni = 0, nr = 0;
    if (!items || !resets) {
        for (uint32_t i = 0; i < cb->nqops; i++)
            if (!cb->qops[i].reset) __atomic_store_n(&cb->qops[i].m->mirrored[cb->qops[i].first], 0, __ATOMIC_RELEASE);
        goto out;
    }
    for (uint32_t i = cb->nqops; i-- > 0;) {
        const vkb_qop *o = &cb->qops[i];
        if (o->reset) {
            resets[nr++] = *o;
            continue;
        }
        int covered = 0;
        for (uint32_t j = 0; j < nr && !covered; j++)
            covered = resets[j].m == o->m && o->first >= resets[j].first && o->first - resets[j].first < resets[j].count;
        if (!covered) items[ni++] = *o;
    }
    if (!ni) goto out;
    VkQueueFlags fam = cb->family == UINT32_MAX ? 0 : family_flags(cb->device, cb->family);
    if (!(fam & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) || cb->broken || cb->stream.oom) {
        /* vkCmdCopyQueryPoolResults needs a graphics or compute queue. */
        for (uint32_t i = 0; i < ni; i++) __atomic_store_n(&items[i].m->mirrored[items[i].first], 0, __ATOMIC_RELEASE);
        goto out;
    }
    qsort(items, ni, sizeof(*items), item_cmp);
    /* Runs of consecutive queries of one pool, duplicates dropped: items[i].count = run length. */
    uint32_t nrun = 0;
    for (uint32_t i = 0; i < ni; i++) {
        if (nrun && items[nrun - 1].m == items[i].m && items[i].first <= items[nrun - 1].first + items[nrun - 1].count) {
            uint32_t end = items[i].first + 1, cur = items[nrun - 1].first + items[nrun - 1].count;
            if (end > cur) items[nrun - 1].count = end - items[nrun - 1].first;
            continue;
        }
        items[nrun] = items[i];
        items[nrun].count = 1;
        nrun++;
    }
    /* Everything recorded before: the copy must not start before the queries' commands are done.
     * A transfer-only barrier let Mali run it early: a compute pass timed 0.15 ms instead of 20 ms
     * (gpubench, tablet), the end timestamp read before it was written. */
    VkMemoryBarrier all = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT};
    vkb_wire_vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &all, 0, NULL,
                                  0, NULL);
    for (uint32_t i = 0; i < nrun; i++)
        vkb_wire_vkCmdCopyQueryPoolResults(commandBuffer, items[i].m->pool, items[i].first, items[i].count, items[i].m->buf,
                                           (VkDeviceSize)items[i].first * 8, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
    vkb_wire_vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &xfer_xfer, 0, NULL,
                                  0, NULL);
    for (uint32_t i = 0; i < nrun; i++)
        vkb_wire_vkCmdFillBuffer(commandBuffer, items[i].m->buf, items[i].m->ready_off + (VkDeviceSize)items[i].first * 4,
                                 (VkDeviceSize)items[i].count * 4, 1);
    VkMemoryBarrier host = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
    vkb_wire_vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, NULL, 0, NULL);
    for (uint32_t i = 0; i < nrun; i++)
        for (uint32_t q = items[i].first; q < items[i].first + items[i].count; q++)
            __atomic_store_n(&items[i].m->mirrored[q], 1, __ATOMIC_RELEASE);
out:
    free(items);
    free(resets);
    vkb_query_cb_clear(cb);
}

/* ------------------------------------------------------------------ results */

/* 1 = answered from the mirror (*result set); 0 = ask the server. */
/* Why the last vkb_query_results call said "ask the server" (the first few are logged by the
 * caller: on the tablet DRG still made ~5,400 query round trips a second with the mirror on). */
static __thread const char *why_wire;
const char *vkb_query_why_wire(void) { return why_wire ? why_wire : "?"; }

int vkb_query_results(vkb_device *d, VkQueryPool pool, uint32_t first, uint32_t count, size_t size, void *data, VkDeviceSize stride,
                      VkQueryResultFlags flags, VkResult *result)
{
    why_wire = "status flag";
    if (flags & VK_QUERY_RESULT_WITH_STATUS_BIT_KHR) return 0;
    vkb_qmirror *m = mirror_of(d, pool);
    why_wire = !m ? (d->qm && d->qm->disabled ? "mirror disabled" : "pool not mirrored") : "range";
    if (!m || first >= m->count || count > m->count - first || !count) return 0;
    int b64 = (flags & VK_QUERY_RESULT_64_BIT) != 0, avail = (flags & VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) != 0;
    size_t elem = (b64 ? 8 : 4) * (avail ? 2 : 1);
    why_wire = "size";
    if ((count - 1) * stride + elem > size) return 0;
    int all = 1;
    why_wire = "query not mirrored (reset, or not ended in a finished primary)";
    for (uint32_t q = first; q < first + count; q++) {
        if (!__atomic_load_n(&m->mirrored[q], __ATOMIC_ACQUIRE)) return 0;
        all &= __atomic_load_n(&m->ready[q], __ATOMIC_ACQUIRE) != 0;
    }
    why_wire = "WAIT on a query not ready";
    if (!all && (flags & VK_QUERY_RESULT_WAIT_BIT)) return 0; /* the server waits */
    int ready_all = 1;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t q = first + i;
        uint8_t *out = (uint8_t *)data + (size_t)i * stride;
        int ready = __atomic_load_n(&m->ready[q], __ATOMIC_ACQUIRE) != 0;
        uint64_t v = 0;
        if (ready) v = __atomic_load_n(&m->value[q], __ATOMIC_RELAXED);
        ready_all &= ready;
        /* Not available: the value is written only with PARTIAL (0 is a valid partial result). */
        if (ready || (flags & VK_QUERY_RESULT_PARTIAL_BIT)) {
            if (b64) memcpy(out, &v, 8);
            else {
                uint32_t v32 = (uint32_t)v;
                memcpy(out, &v32, 4);
            }
        }
        if (avail) {
            if (b64) {
                uint64_t a = ready;
                memcpy(out + 8, &a, 8);
            } else {
                uint32_t a = ready;
                memcpy(out + 4, &a, 4);
            }
        }
    }
    /* With PARTIAL every value is written: Mesa then says VK_SUCCESS (the spec only requires
     * VK_NOT_READY without WAIT and PARTIAL), and so does this, to match the wire. */
    *result = ready_all || (flags & VK_QUERY_RESULT_PARTIAL_BIT) ? VK_SUCCESS : VK_NOT_READY;
    return 1;
}
