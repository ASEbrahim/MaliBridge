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
 *   u64 value[count]   the query's result, written by the GPU; PENDING (all ones) while it has none
 * A primary command buffer that ends queries (vkCmdEndQuery[Indexed], vkCmdWriteTimestamp[2],
 * including those of the secondaries it executes) gets, appended at vkEndCommandBuffer:
 *   barrier; vkCmdCopyQueryPoolResults(64-bit, WAIT) -> value[]; barrier transfer -> host
 * vkResetQueryPool sets value[] to PENDING from the host; vkCmdResetQueryPool is followed by a
 * vkCmdFillBuffer(PENDING) of value[] at the same place in the stream (both legal only outside
 * render passes), which, like the reset itself, takes effect when it executes.
 *
 * One word per query, not a value and a separate "ready" word (v0.5.4.1 and before): Batman:
 * Arkham Asylum (UE3 through DXVK, Galaxy Tab S12+) read walls as 0 samples, a reused
 * slot's old value, and drew the lit rooms behind them. Nothing orders the host's view of two GPU
 * writes to different places before a fence wait: a GPU cache can write the ready word's line back
 * before the value's, however the commands are ordered on the GPU. Writes to one place are seen in
 * order, so a reader now sees PENDING or this use's value, never the last use's. No real result is
 * all ones: occlusion counts are far below it, and a timestamp would have to run for centuries.
 *
 * A query is answered from the mirror only while it is "mirrored": set when a command buffer
 * appended its copy and by a host reset (value PENDING), cleared by a command buffer that ended it
 * but could not copy it (a queue family without graphics/compute). Anything else (unmirrored queries, other
 * query types, VK_QUERY_RESULT_WITH_STATUS_BIT_KHR, WAIT on a query not ready yet) goes to the
 * server as before. Limit: a multiview render pass ends N consecutive queries per vkCmdEndQuery;
 * only the first is copied; the others (mirrored by a host reset) are found by the stuck-query
 * check below, which asks the server about a query the mirror has said "not ready" for too long.
 *
 * VKBRIDGE_QUERY_MIRROR=0 turns this off, VKBRIDGE_QUERY_MIRROR_OCCLUSION=0 for occlusion pools only.
 */
#define _GNU_SOURCE
#include "vkb_client.h"
#include "gen/vkb_client_gen.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#define PENDING (~(uint64_t)0)

typedef struct vkb_qmirror {
    VkQueryPool pool;
    VkQueryType type;
    uint32_t count;
    VkBuffer buf;
    VkDeviceMemory mem;
    uint64_t *value;         /* per query, atomic; PENDING = no result yet */
    uint8_t *mirrored;       /* per query, atomic */
    uint32_t *nr_polls;      /* per query: not-ready answers from the mirror in a row (stuck_ask) */
    uint32_t *nr_since;      /* per query: ms clock of the first of them */
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

/* VKBRIDGE_QUERY_MIRROR_OCCLUSION=0 (guest env): occlusion results from the server, timestamps
 * still from the mirror. For a game whose occlusion results would still come wrong from the mirror
 * on some driver (a per-game preset), at the cost of its round trips. */
static int occlusion_mirrored(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("VKBRIDGE_QUERY_MIRROR_OCCLUSION");
        v = !(e && !strcmp(e, "0"));
        if (!v) VKB_INFO("query mirror: occlusion results from the server (VKBRIDGE_QUERY_MIRROR_OCCLUSION=0), timestamps mirrored");
    }
    return v;
}

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
    free(m->nr_polls);
    free(m->nr_since);
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
    if (!m || !(m->mirrored = calloc(count, 1)) || !(m->nr_polls = calloc(count, 4)) || !(m->nr_since = calloc(count, 4))) {
        if (m) {
            free(m->mirrored);
            free(m->nr_polls);
        }
        free(m);
        return NULL;
    }
    m->pool = pool;
    m->count = count;
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, (VkDeviceSize)count * 8,
                              VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
    if (vkb_ep_vkCreateBuffer(device, &bci, NULL, &m->buf) != VK_SUCCESS) goto fail;
    VkMemoryRequirements req;
    vkb_ep_vkGetBufferMemoryRequirements(device, m->buf, &req);
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < pd->client_mem.memoryTypeCount && type == UINT32_MAX; i++) {
        if (!(req.memoryTypeBits & (1u << i)) || !pd->type_map[i].shared) continue;
        if (!(pd->info.coherent_types & (1u << pd->type_map[i].server_type))) continue;
        if (pd->type_map[i].cached) continue; /* CPU-cached pages need invalidates: the values would go stale */
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
    memset(vm->ptr, 0xff, (size_t)bci.size); /* PENDING */
    m->value = (uint64_t *)vm->ptr;
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
    if (r == VK_SUCCESS) vkb_qvis_pool_create(*pQueryPool, pCreateInfo);
    vkb_device *d = vkb_dev(device);
    if (r != VK_SUCCESS || !d->qm || d->qm->disabled || !pCreateInfo->queryCount) return r;
    if (pCreateInfo->queryType != VK_QUERY_TYPE_OCCLUSION && pCreateInfo->queryType != VK_QUERY_TYPE_TIMESTAMP) return r;
    if (pCreateInfo->queryType == VK_QUERY_TYPE_OCCLUSION && !occlusion_mirrored()) return r;
    vkb_qmirror *m = mirror_create(d, *pQueryPool, pCreateInfo->queryCount);
    if (m) m->type = pCreateInfo->queryType;
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
    vkb_qvis_pool_destroy(queryPool);
    vkb_qmirror *m = d->qm ? vkb_map_remove(&d->qm->pools, (uint64_t)queryPool) : NULL;
    if (m) mirror_free(d, m);
}

/* DXVK resets queries from the host (~7,000 a second in Deep Rock Galactic). */
VKAPI_ATTR void VKAPI_CALL vkb_ep_vkResetQueryPool(VkDevice device, VkQueryPool queryPool, uint32_t firstQuery, uint32_t queryCount)
{
    vkb_qmirror *m = mirror_of(vkb_dev(device), queryPool);
    if (m && firstQuery < m->count) {
        uint32_t end = queryCount > m->count - firstQuery ? m->count : firstQuery + queryCount;
        /* A reset query is unavailable until it is ended and that work runs, which is what
         * PENDING says: it stays mirrored. DXVK polls queries whose command buffer is
         * still recording; sent to the server, those were ~5,400 round trips a second in DRG. A
         * command buffer that ends one without being able to copy it unmirrors it (below). */
        for (uint32_t q = firstQuery; q < end; q++) {
            __atomic_store_n(&m->value[q], PENDING, __ATOMIC_RELEASE);
            __atomic_store_n(&m->mirrored[q], 1, __ATOMIC_RELEASE);
            __atomic_store_n(&m->nr_polls[q], 0, __ATOMIC_RELAXED);
        }
    }
    vkb_qvis_reset(queryPool, firstQuery, queryCount);
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
    if (cb->recording) vkb_qvis_end(cb, queryPool, query);
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
    if (cb->recording) vkb_qvis_reset(queryPool, firstQuery, queryCount);
    vkb_qmirror *m = mirror_of(cb->device, queryPool);
    if (!m || !cb->recording || firstQuery >= m->count) return;
    uint32_t n = queryCount > m->count - firstQuery ? m->count - firstQuery : queryCount;
    /* After earlier writes of the mirror (earlier copies and fills of the same values). */
    vkb_wire_vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &xfer_xfer, 0, NULL,
                                  0, NULL);
    vkb_wire_vkCmdFillBuffer(commandBuffer, m->buf, (VkDeviceSize)firstQuery * 8, (VkDeviceSize)n * 8, 0xffffffffu); /* PENDING */
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

/* Appends the copies of the queries ended so far (and not reset again after) to a primary command
 * buffer that still records, outside any render pass, and forgets them: at vkEndCommandBuffer, and
 * with VKBRIDGE_QUERY_EARLY=1 also after each render pass that ended occlusion queries. */
static void flush_qops(vkb_cmdbuf *cb)
{
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
    /* Everything recorded before: the copy must not start before the queries' commands are done
     * (nor before a vkCmdResetQueryPool's fill of the same values).
     * A transfer-only barrier let Mali run it early: a compute pass timed 0.15 ms instead of 20 ms
     * (gpubench, tablet), the end timestamp read before it was written. */
    VkMemoryBarrier all = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT};
    vkb_wire_vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &all, 0, NULL,
                                  0, NULL);
    /* Test hooks (host tests only), each leaving out a flush's copies, so its values stay PENDING:
     * VKBRIDGE_TEST_QUERY_LOSE_READY=1, the first flush (a command buffer's end, or with
     * VKBRIDGE_QUERY_EARLY=1 a render pass's end): an update lost on the way, as in Deep Rock
     * Galactic's stall on the S11 Ultra (the GPU done, the server idle, the mirror never ready);
     * stuck_ask must end it. VKBRIDGE_TEST_QUERY_LOSE_VALUE=N, the Nth flush: its values reaching
     * the host late. With a separate ready word that was a stale answer; now it can only
     * be a late one. */
    static int lose = -1, lose_value = -1;
    static uint32_t flushes;
    if (lose < 0) lose = getenv("VKBRIDGE_TEST_QUERY_LOSE_READY") && atoi(getenv("VKBRIDGE_TEST_QUERY_LOSE_READY")) == 1;
    if (lose_value < 0) lose_value = getenv("VKBRIDGE_TEST_QUERY_LOSE_VALUE") ? atoi(getenv("VKBRIDGE_TEST_QUERY_LOSE_VALUE")) : 0;
    int skip = lose && __atomic_exchange_n(&lose, 0, __ATOMIC_ACQ_REL);
    if (skip) VKB_WARN("query mirror: test: this command buffer's ready words are left out (VKBRIDGE_TEST_QUERY_LOSE_READY=1)");
    if (lose_value > 0 && __atomic_add_fetch(&flushes, 1, __ATOMIC_RELAXED) == (uint32_t)lose_value) {
        skip = 1;
        VKB_WARN("query mirror: test: this command buffer's values are left out (VKBRIDGE_TEST_QUERY_LOSE_VALUE)");
    }
    for (uint32_t i = 0; i < nrun && !skip; i++)
        vkb_wire_vkCmdCopyQueryPoolResults(commandBuffer, items[i].m->pool, items[i].first, items[i].count, items[i].m->buf,
                                           (VkDeviceSize)items[i].first * 8, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
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

/* Called by vkEndCommandBuffer while the command buffer still records. */
void vkb_query_cb_end(vkb_cmdbuf *cb)
{
    if (!cb->nqops) return;
    if (cb->level != VK_COMMAND_BUFFER_LEVEL_PRIMARY) return; /* kept for vkCmdExecuteCommands */
    flush_qops(cb);
}

/* ------------------------------------------------------------------ early copies (lite mode)
 *
 * Stray (UE4, D3D11 through DXVK, S11 Ultra 2026-10-26): the render thread waits for last frame's
 * occlusion results (UE4's GetQueryData spins on GetData with a timeout read from
 * QueryPerformanceCounter, the clock_gettime in its profile). From the mirror a result is ready
 * only when the whole command buffer that ended it is done, since the copy sits at its end, and
 * DXVK records about a frame per command buffer: the thread waited for the GPU's whole frame
 * (14.0 fps, GPU 47% busy, the thread 98% of a core). With the mirror off the server answers from
 * the driver, which has a query once its render pass is done: 15.2 fps, the thread 43% of a core.
 *
 * VKBRIDGE_QUERY_EARLY=1 (guest env, off by default) copies a primary's ended queries right after
 * each render pass that ended an occlusion query (vkCmdEndRenderPass[2], vkCmdEndRendering[2]),
 * with the same barriers as at the end: results are ready when that pass is done. Not after a
 * dynamic rendering that suspends (nothing may come between it and its resume). Timestamps alone
 * still wait for the end. Costs per such pass one barrier from all earlier work and the copies:
 * on a tiler that keeps the next pass's vertex work from overlapping this pass's fragment work if
 * the driver serializes on it (to be measured on the device). */
static int early_mode(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("VKBRIDGE_QUERY_EARLY");
        v = e && *e && strcmp(e, "0") != 0;
        if (v) VKB_INFO("query mirror: occlusion results copied after each render pass that ended one (VKBRIDGE_QUERY_EARLY=1)");
    }
    return v;
}

static void pass_ended(VkCommandBuffer commandBuffer)
{
    vkb_cmdbuf *cb = vkb_cb(commandBuffer);
    if (!cb->nqops || !cb->recording || cb->level != VK_COMMAND_BUFFER_LEVEL_PRIMARY || !early_mode()) return;
    int occ = 0;
    for (uint32_t i = 0; i < cb->nqops && !occ; i++) occ = !cb->qops[i].reset && cb->qops[i].m->type == VK_QUERY_TYPE_OCCLUSION;
    if (occ) flush_qops(cb);
}

VKAPI_ATTR void VKAPI_CALL vkb_ep_vkCmdEndRenderPass(VkCommandBuffer commandBuffer)
{
    vkb_wire_vkCmdEndRenderPass(commandBuffer);
    pass_ended(commandBuffer);
}

VKAPI_ATTR void VKAPI_CALL vkb_ep_vkCmdEndRenderPass2(VkCommandBuffer commandBuffer, const VkSubpassEndInfo *pSubpassEndInfo)
{
    vkb_wire_vkCmdEndRenderPass2(commandBuffer, pSubpassEndInfo);
    pass_ended(commandBuffer);
}

VKAPI_ATTR void VKAPI_CALL vkb_ep_vkCmdBeginRendering(VkCommandBuffer commandBuffer, const VkRenderingInfo *pRenderingInfo)
{
    vkb_wire_vkCmdBeginRendering(commandBuffer, pRenderingInfo);
    vkb_cb(commandBuffer)->rendering_suspends = pRenderingInfo && (pRenderingInfo->flags & VK_RENDERING_SUSPENDING_BIT);
}

VKAPI_ATTR void VKAPI_CALL vkb_ep_vkCmdEndRendering(VkCommandBuffer commandBuffer)
{
    vkb_wire_vkCmdEndRendering(commandBuffer);
    if (!vkb_cb(commandBuffer)->rendering_suspends) pass_ended(commandBuffer);
}

VKAPI_ATTR void VKAPI_CALL vkb_ep_vkCmdEndRendering2KHR(VkCommandBuffer commandBuffer, const VkRenderingEndInfoKHR *pRenderingEndInfo)
{
    vkb_wire_vkCmdEndRendering2KHR(commandBuffer, pRenderingEndInfo);
    if (!vkb_cb(commandBuffer)->rendering_suspends) pass_ended(commandBuffer);
}

/* ------------------------------------------------------------------ results */

/* Why the last vkb_query_results call said "ask the server" (the first few are logged by the
 * caller: on the tablet DRG still made ~5,400 query round trips a second with the mirror on). */
static __thread const char *why_wire;

/* VKBRIDGE_QUERY_NOTREADY_WIRE=1 (guest env, off by default; an A/B arm for the early copies):
 * a query the mirror has not got yet is asked of the server, whose driver has it once its render
 * pass is done; icd.c's 1 ms quiet window then answers repeated polls of it. */
static int notready_wire(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("VKBRIDGE_QUERY_NOTREADY_WIRE");
        v = e && *e && strcmp(e, "0") != 0;
    }
    return v;
}
const char *vkb_query_why_wire(void) { return why_wire ? why_wire : "?"; }

/* ------------------------------------------------------------------ stuck queries
 *
 * Deep Rock Galactic on the S11 Ultra (2026-10-27, rc7 and rc9, 5 of 6 Space Rig loads): UE4's
 * render thread spun at 100% of a core for minutes in this file's not-ready answer (61% of its
 * samples), with no syscalls, every DXVK thread idle, the server idle and no bridge call pending,
 * until UE4 gave up ("GameThread timed out waiting for RenderThread after 120.00 secs"). The GPU
 * had the query; the mirror's ready word was never set. With the mirror off every load
 * passed. Whatever leaves a value PENDING (an update lost on the way, a query ended where no copy
 * follows it - a multiview pass ends N queries per vkCmdEndQuery and only the first is copied),
 * the mirror must not say "not ready" forever: no call of the app's ever reaches the
 * server again to notice.
 *
 * So a query the mirror has answered "not ready" for 200 ms in a row is asked of the server once.
 * If the driver has it, the query leaves the mirror (answered by the server, the authority, until
 * its next reset or copy mirrors it again) and the
 * case is logged; if not, the count starts over. The normal case is untouched: a result reaches
 * the mirror at most a GPU frame after its submit (Stray at 14 fps: ~70 ms),
 * and a poll that sees it ready costs no round trip, as before. The clock is read on the first 64
 * not-ready answers of a run and then every 64th, so a tight spin pays almost nothing for it. */
static uint32_t stuck_ms(void)
{
    static int v = -2;
    if (v == -2) {
        const char *e = getenv("VKBRIDGE_QUERY_STUCK_MS");
        v = e && *e ? atoi(e) : 200;
        if (v < 0) v = 0;
    }
    return (uint32_t)v;
}

static uint32_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
}

/* The query this thread's last vkb_query_results sent to the server as stuck (vkb_query_wire_answer). */
static __thread struct {
    vkb_qmirror *m;
    VkQueryPool pool;
    uint32_t first, count, ms;
} stuck_probe;

/* One not-ready answer for query q: 1 when it has been not ready long enough to ask the server. */
static int stuck_ask(vkb_qmirror *m, uint32_t q, uint32_t *held)
{
    uint32_t limit = stuck_ms();
    if (!limit) return 0;
    uint32_t n = __atomic_add_fetch(&m->nr_polls[q], 1, __ATOMIC_RELAXED);
    if (n == 1) {
        __atomic_store_n(&m->nr_since[q], now_ms(), __ATOMIC_RELAXED);
        return 0;
    }
    if (n > 64 && (n & 63)) return 0;
    uint32_t t = now_ms() - __atomic_load_n(&m->nr_since[q], __ATOMIC_RELAXED);
    if (t < limit) return 0;
    /* Start over: if the server says not ready either, the next question comes a period later. */
    __atomic_store_n(&m->nr_polls[q], 0, __ATOMIC_RELAXED);
    *held = t;
    return 1;
}

static uint32_t stuck_found; /* queries the server had while the mirror said not ready (logged) */

/* icd.c, after a vkGetQueryPoolResults that went to the server. */
void vkb_query_wire_answer(vkb_device *d, VkQueryPool pool, uint32_t first, uint32_t count, VkResult r)
{
    (void)d;
    vkb_qmirror *m = stuck_probe.m;
    if (!m || stuck_probe.pool != pool || stuck_probe.first != first || stuck_probe.count != count) return;
    stuck_probe.m = NULL;
    if (r != VK_SUCCESS) {
        VKB_ONCE(VKB_LOG_INFO, "query mirror: a query not ready for %u ms is not ready on the server either (logged once)", stuck_probe.ms);
        return;
    }
    for (uint32_t q = first; q < first + count && q < m->count; q++) __atomic_store_n(&m->mirrored[q], 0, __ATOMIC_RELEASE);
    uint32_t n = __atomic_add_fetch(&stuck_found, 1, __ATOMIC_RELAXED);
    if (n <= 5 || (n & (n - 1)) == 0)
        VKB_WARN("query mirror: query %u+%u of pool %llx was not ready in the mirror for %u ms but done on the GPU; answered by the "
                 "server until it is reset (%u so far)",
                 first, count, (unsigned long long)(uintptr_t)pool, stuck_probe.ms, n);
}

/* 1 = answered from the mirror (*result set); 0 = ask the server. */
int vkb_query_results(vkb_device *d, VkQueryPool pool, uint32_t first, uint32_t count, size_t size, void *data, VkDeviceSize stride,
                      VkQueryResultFlags flags, VkResult *result)
{
    stuck_probe.m = NULL;
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
    uint32_t first_nr = UINT32_MAX;
    why_wire = "query not mirrored (reset, or not ended in a finished primary)";
    for (uint32_t q = first; q < first + count; q++) {
        if (!__atomic_load_n(&m->mirrored[q], __ATOMIC_ACQUIRE)) return 0;
        int rq = __atomic_load_n(&m->value[q], __ATOMIC_ACQUIRE) != PENDING;
        all &= rq;
        if (!rq && first_nr == UINT32_MAX) first_nr = q;
    }
    why_wire = "WAIT on a query not ready";
    if (!all && (flags & VK_QUERY_RESULT_WAIT_BIT)) return 0; /* the server waits */
    /* VKBRIDGE_QUERY_NOTREADY_WIRE=1 asks the server on every not-ready answer, so a query stuck in the
     * mirror is never answered "not ready" from it alone; the stuck check below is for the default. */
    why_wire = "not ready in the mirror (VKBRIDGE_QUERY_NOTREADY_WIRE=1)";
    if (!all && notready_wire()) return 0; /* the driver may have it before the mirror's copy ran */
    uint32_t held = 0;
    if (!all && stuck_ask(m, first_nr, &held)) {
        why_wire = "not ready in the mirror for VKBRIDGE_QUERY_STUCK_MS (asked once)";
        stuck_probe.m = m;
        stuck_probe.pool = pool;
        stuck_probe.first = first;
        stuck_probe.count = count;
        stuck_probe.ms = held;
        return 0;
    }
    int ready_all = 1;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t q = first + i;
        uint8_t *out = (uint8_t *)data + (size_t)i * stride;
        /* One load: the answer and its value come from the same word. */
        uint64_t v = __atomic_load_n(&m->value[q], __ATOMIC_ACQUIRE);
        int ready = v != PENDING;
        if (!ready) v = 0;
        ready_all &= ready;
        /* A run of not-ready answers ends when the query is seen ready. */
        if (ready && __atomic_load_n(&m->nr_polls[q], __ATOMIC_RELAXED)) __atomic_store_n(&m->nr_polls[q], 0, __ATOMIC_RELAXED);
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
