/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * The fallback for an occlusion query the driver never reports available.
 *
 * Deep Rock Galactic's Space Rig stall on the S11 Ultra (2026-10-27): 512+ occlusion queries ended
 * in render-pass secondaries (DXVK's tiler mode), their primary submitted and done, and the Mali
 * driver saying "not ready" for minutes while UE4's render thread spun on them. The bridge hands
 * the driver the app's pattern intact (querystuck_check burst); the fault is the driver's.
 *
 * VKBRIDGE_QUERY_VISIBLE_MS=<ms> (default 500, 0 = off, 100 at least). A vkGetQueryPoolResults of
 * one occlusion query, without WAIT_BIT or WITH_STATUS_BIT, that would answer "not ready" is
 * answered VK_SUCCESS with VKBRIDGE_QUERY_VISIBLE_VALUE samples (default 1; availability 1 when
 * asked for) only when all of these hold:
 *   - the app has polled it "not ready" without a break for <ms> since its last reset, or (once
 *     per submission) another query of the same primary recording was answered so already: UE4
 *     waits on its queries one after another, and 500+ of them at <ms> each would be minutes;
 *   - its last End was recorded after that reset, in a primary whose recording was ended and
 *     submitted at least <ms> ago, or in a secondary whose recording was ended and executed by
 *     such a primary (the recording table below; anything it no longer covers is left alone);
 *   - the server, asked again with the app's own flags at that moment, still says "not ready" (if
 *     it says ready, the app gets the driver's own answer).
 * A query never ended, ended in a command buffer still recording or never submitted, or in one
 * submitted less than <ms> ago, is never answered here: those are the app's to wait for.
 *
 * Why 500 ms by default: a healthy driver makes a query available when its submission finishes,
 * well inside one frame (the host suite's slowest poll is under 2 ms; a 10 fps game's frame is
 * 100 ms). The stuck ones on the Ultra were 550 ms past their submit and never came. 500 ms is 5
 * frames even at 10 fps, so a slow but healthy GPU is never answered for, and DRG's stall becomes
 * one hitch of about half a second.
 *
 * Picture: an occlusion result only decides whether something is drawn (UE4 and DXVK's
 * predication draw on any nonzero count); "visible" draws what might have been culled - extra GPU
 * work, never a missing object. DXVK's predicates read the query on the GPU, not through this.
 *
 * Cost while on: per occlusion pool, 32 bytes a query (DXVK's 16384-query pool: 512 KB); a 2.6 MB
 * recording table; per query End and per recording, a few stores; per "not ready" poll, one map
 * lookup and a clock read. Other query types are not tracked.
 */
#define _GNU_SOURCE
#include "vkb_client.h"
#include "gen/vkb_client_gen.h"

#include <string.h>
#include <time.h>

typedef struct qv_query {
    uint64_t reset_ns; /* last host or command reset (recorded) */
    uint64_t end_rec;  /* the recording that last ended it */
    uint64_t end_ns;
    uint64_t nr_ns;    /* first "not ready" of the current run (0: none) */
} qv_query;

typedef struct qv_pool {
    uint32_t count;
    qv_query *q;
} qv_pool;

static vkb_map pools;
static uint64_t rec_counter;

/* Each recording, by number, without command buffer pointers (those can be freed under it): a
 * direct-mapped table, an entry valid while its rec matches. ~3,300 recordings a second in Deep
 * Rock Galactic: 64k entries cover ~20 s, and a query the table no longer covers is never
 * answered (left to the driver). */
#define RECS (1u << 16)
typedef struct qv_rec {
    uint64_t rec;    /* 0: free */
    uint64_t parent; /* a secondary: the recording of the primary that last executed it */
    uint64_t sub_ns; /* a primary: its last submit of this recording */
    uint8_t ended, secondary;
    uint8_t vis;     /* a primary: one of its queries was answered by the fallback */
} qv_rec;
static qv_rec *recs;

static int vis_ms = -1; /* VKBRIDGE_QUERY_VISIBLE_MS */
static int vis_value = 1; /* VKBRIDGE_QUERY_VISIBLE_VALUE: the sample count answered */

static void cfg_once(void)
{
    const char *v = getenv("VKBRIDGE_QUERY_VISIBLE_MS");
    int m = v && *v ? atoi(v) : 500;
    const char *val = getenv("VKBRIDGE_QUERY_VISIBLE_VALUE");
    if (val && *val && atoi(val) > 0) vis_value = atoi(val);
    m = m <= 0 ? 0 : m < 100 ? 100 : m;
    vkb_map_init(&pools);
    if (m && !(recs = calloc(RECS, sizeof(*recs)))) m = 0;
    __atomic_store_n(&vis_ms, m, __ATOMIC_RELEASE);
}

static pthread_once_t cfg_once_v = PTHREAD_ONCE_INIT;

/* Nonzero while the fallback is on (its tracking with it). */
int vkb_qvis_on(void)
{
    if (__atomic_load_n(&vis_ms, __ATOMIC_ACQUIRE) < 0) pthread_once(&cfg_once_v, cfg_once);
    return vis_ms;
}

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static long ms_ago(uint64_t t, uint64_t now) { return t ? (long)((now - t) / 1000000) : -1; }

static qv_rec *rec_slot(uint64_t rec) { return recs && rec ? &recs[rec & (RECS - 1)] : NULL; }

/* Writers: the thread recording (begin, end, execute) or submitting that command buffer. A reader
 * checks rec before and after it reads the rest (rec_read); a torn entry is never trusted. */
static void rec_begin(uint64_t rec, int secondary)
{
    qv_rec *r = rec_slot(rec);
    if (!r) return;
    __atomic_store_n(&r->rec, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&r->parent, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&r->sub_ns, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&r->ended, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&r->secondary, (uint8_t)secondary, __ATOMIC_RELAXED);
    __atomic_store_n(&r->vis, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&r->rec, rec, __ATOMIC_RELEASE);
}

static int rec_read(uint64_t rec, qv_rec *out)
{
    qv_rec *r = rec_slot(rec);
    if (!r || __atomic_load_n(&r->rec, __ATOMIC_ACQUIRE) != rec) return 0;
    out->parent = __atomic_load_n(&r->parent, __ATOMIC_ACQUIRE);
    out->sub_ns = __atomic_load_n(&r->sub_ns, __ATOMIC_ACQUIRE);
    out->ended = __atomic_load_n(&r->ended, __ATOMIC_ACQUIRE);
    out->secondary = __atomic_load_n(&r->secondary, __ATOMIC_ACQUIRE);
    out->vis = __atomic_load_n(&r->vis, __ATOMIC_ACQUIRE);
    out->rec = rec;
    return __atomic_load_n(&r->rec, __ATOMIC_ACQUIRE) == rec;
}

static qv_rec *rec_live(uint64_t rec)
{
    qv_rec *r = rec_slot(rec);
    return r && __atomic_load_n(&r->rec, __ATOMIC_ACQUIRE) == rec ? r : NULL;
}

static qv_pool *pool_of(VkQueryPool p) { return p ? vkb_map_get(&pools, (uint64_t)p) : NULL; }

void vkb_qvis_pool_create(VkQueryPool pool, const VkQueryPoolCreateInfo *ci)
{
    if (!vkb_qvis_on() || !pool || !ci->queryCount || ci->queryType != VK_QUERY_TYPE_OCCLUSION) return;
    qv_pool *p = calloc(1, sizeof(*p));
    if (!p || !(p->q = calloc(ci->queryCount, sizeof(*p->q)))) {
        free(p);
        return;
    }
    p->count = ci->queryCount;
    vkb_map_put(&pools, (uint64_t)pool, p);
}

void vkb_qvis_pool_destroy(VkQueryPool pool)
{
    if (!vkb_qvis_on() || !pool) return;
    qv_pool *p = vkb_map_remove(&pools, (uint64_t)pool);
    if (p) {
        free(p->q);
        free(p);
    }
}

/* A host vkResetQueryPool, or a recorded vkCmdResetQueryPool (timed at record time). */
void vkb_qvis_reset(VkQueryPool pool, uint32_t first, uint32_t count)
{
    if (!vkb_qvis_on()) return;
    qv_pool *p = pool_of(pool);
    if (!p || first >= p->count) return;
    uint32_t end = count > p->count - first ? p->count : first + count;
    uint64_t t = now_ns();
    for (uint32_t q = first; q < end; q++) p->q[q].reset_ns = t;
}

void vkb_qvis_end(vkb_cmdbuf *cb, VkQueryPool pool, uint32_t query)
{
    if (!vkb_qvis_on()) return;
    qv_pool *p = pool_of(pool);
    if (!p || query >= p->count) return;
    p->q[query].end_rec = cb->qv_rec;
    p->q[query].end_ns = now_ns();
}

void vkb_qvis_cb_begin(vkb_cmdbuf *cb)
{
    if (!vkb_qvis_on()) return;
    cb->qv_rec = __atomic_add_fetch(&rec_counter, 1, __ATOMIC_RELAXED);
    rec_begin(cb->qv_rec, cb->level == VK_COMMAND_BUFFER_LEVEL_SECONDARY);
}

void vkb_qvis_cb_end(vkb_cmdbuf *cb)
{
    if (!vkb_qvis_on()) return;
    qv_rec *r = rec_live(cb->qv_rec);
    if (r) __atomic_store_n(&r->ended, 1, __ATOMIC_RELEASE);
}

void vkb_qvis_cb_execute(vkb_cmdbuf *primary, vkb_cmdbuf *secondary)
{
    if (!vkb_qvis_on()) return;
    qv_rec *r = rec_live(secondary->qv_rec);
    if (r) __atomic_store_n(&r->parent, primary->qv_rec, __ATOMIC_RELEASE);
}

void vkb_qvis_submit(VkCommandBuffer c)
{
    if (!vkb_qvis_on() || !c) return;
    qv_rec *r = rec_live(vkb_cb(c)->qv_rec);
    if (r) __atomic_store_n(&r->sub_ns, now_ns(), __ATOMIC_RELEASE);
}

/* Host tests only (VKBRIDGE_TEST_QUERY_NEVER_FROM=<q>): occlusion queries from q on are never
 * available, whatever the driver says - the Ultra's driver as seen from the app. */
VkResult vkb_qvis_imitate(VkQueryPool pool, uint32_t first, uint32_t count, VkResult r)
{
    static long from = -2;
    if (from == -2) {
        const char *e = getenv("VKBRIDGE_TEST_QUERY_NEVER_FROM");
        from = e && *e ? atol(e) : -1;
    }
    if (from < 0 || r != VK_SUCCESS || !vkb_qvis_on() || !pool_of(pool)) return r;
    return (long)first + (long)count > from ? VK_NOT_READY : r;
}

static uint32_t vis_answered;

/* After every vkGetQueryPoolResults: *r is what the app would get (the mirror's, the quiet
 * window's or the server's answer); the fallback may replace a "not ready" here. */
void vkb_qvis_after(vkb_device *d, VkQueryPool pool, uint32_t first, uint32_t count, size_t size, void *data, VkDeviceSize stride,
                    VkQueryResultFlags flags, VkResult *r)
{
    if (!vkb_qvis_on() || (*r != VK_SUCCESS && *r != VK_NOT_READY)) return;
    qv_pool *p = pool_of(pool);
    if (!p || first >= p->count) return;
    if (*r == VK_SUCCESS) {
        uint32_t end = count > p->count - first ? p->count : first + count;
        for (uint32_t q = first; q < end; q++) p->q[q].nr_ns = 0;
        return;
    }
    qv_query *x = &p->q[first];
    uint64_t now = now_ns(), lim = (uint64_t)vis_ms * 1000000ull;
    uint64_t nr = x->nr_ns, reset = x->reset_ns, end_ns = x->end_ns, end_rec = x->end_rec;
    if (!nr || nr < reset) x->nr_ns = nr = now; /* a new run of "not ready" */
    if (count != 1 || !data || (flags & (VK_QUERY_RESULT_WAIT_BIT | VK_QUERY_RESULT_WITH_STATUS_BIT_KHR))) return;
    int wide = (flags & VK_QUERY_RESULT_64_BIT) != 0, avail = (flags & VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) != 0;
    size_t one = wide ? 8 : 4;
    if (size < one * (avail ? 2 : 1)) return;
    if (!end_rec || end_ns < reset) return;
    qv_rec e, prim;
    if (!rec_read(end_rec, &e) || !e.ended) return;
    if (e.secondary) {
        if (!e.parent || !rec_read(e.parent, &prim) || !prim.ended) return;
    } else {
        prim = e;
    }
    if (!prim.sub_ns || prim.sub_ns < end_ns || now - prim.sub_ns < lim) return;
    if (!prim.vis && now - nr < lim) return;
    /* The driver's answer now, with the app's flags: the mirror or the quiet window may have
     * answered for it. */
    VkResult sr = vkb_qvis_imitate(pool, first, 1, vkb_wire_vkGetQueryPoolResults((VkDevice)d, pool, first, 1, size, data, stride, flags));
    if (sr == VK_SUCCESS) {
        x->nr_ns = 0;
        *r = sr;
        return;
    }
    if (sr != VK_NOT_READY) return;
    if (wide) {
        uint64_t v[2] = {(uint64_t)vis_value, 1};
        memcpy(data, v, one * (avail ? 2 : 1));
    } else {
        uint32_t v[2] = {(uint32_t)vis_value, 1};
        memcpy(data, v, one * (avail ? 2 : 1));
    }
    x->nr_ns = 0;
    qv_rec *pr = rec_live(prim.rec);
    if (pr) __atomic_store_n(&pr->vis, 1, __ATOMIC_RELEASE);
    uint32_t n = __atomic_add_fetch(&vis_answered, 1, __ATOMIC_RELAXED);
    if (n <= 3 || n == 10 || n == 100 || n == 1000 || n == 10000 || n % 100000 == 0)
        VKB_WARN("query visible #%u: pool %llx query %u answered %d sample%s after %ld ms not ready (End %ld ms ago in a %s, "
                 "its primary submitted %ld ms ago%s; the driver still says not ready; VKBRIDGE_QUERY_VISIBLE_MS=%d)",
                 n, (unsigned long long)(uintptr_t)pool, first, vis_value, vis_value == 1 ? "" : "s", ms_ago(nr, now), ms_ago(end_ns, now),
                 e.secondary ? "secondary" : "primary", ms_ago(prim.sub_ns, now), prim.vis ? ", a sibling answered before" : "", vis_ms);
    *r = VK_SUCCESS;
}
