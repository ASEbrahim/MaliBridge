/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * What a lost device was given: written to the log at its first loss, with no setting (ledger row 225).
 *
 * Why: a player log (POCO X6 Pro, Mali-G615 r44p1, Resident Evil Village): the device was lost 25-30 s
 * into play, and the log said only "device 17 LOST: vkWaitForFences returned VK_ERROR_DEVICE_LOST", the
 * suspicion and the latch (server_lost.c). Nothing on what the GPU was running or what the driver saw.
 * The hang detector (server_hang.c, VKBRIDGE_HANG_DETECT=1) names the job, but costs a fence and an empty
 * submission per submit and a hook on every command: not for every game. This is the part that is cheap
 * enough to be on always:
 *
 *   On the submit path (server_lost.c's wrappers, beneath every layer), per device: a ring of the last
 *   VKB_LL_RING submissions (when, which queue, how many batches, command buffers, waits and signals,
 *   the fence, the first signal semaphore and its value, the first VKB_LL_CBS command buffers, the
 *   driver's answer); per wait or fence status that answered VK_SUCCESS: what it said finished. Plain
 *   and relaxed atomic stores, a clock read (vDSO), one atomic add: no lock, no wait, no driver call.
 *   At vkEndCommandBuffer (server_conn.c, the client's stream): one entry of a server-wide table by
 *   command buffer (commands, bytes, draws, dispatches, copies to images, the last pipelines bound). At a
 *   pipeline's creation (server_guard.c, which hashes its stages anyway): one entry of a server-wide table
 *   by pipeline (its stages' SPIR-V hashes, the names of the dumps and of the errata lines).
 *
 *   At a device's first loss (the latch), once per device and at most LL_MAX_RECORDS times per server
 *   run, on a NEW thread (the game's thread never waits): the record (the submissions, newest last, each
 *   "finished" when a wait said so or a later one on its queue finished, else "NOT seen finished"; the
 *   command buffers of the newest; the oldest not seen finished), bounded in lines (the details at info level: the
 *   session report's list of errors gets the head lines and the one summary line, not each submission). Then, when the
 *   driver itself said lost and offers VK_EXT_device_fault (enabled at vkCreateDevice where the driver
 *   lists it and its deviceFault feature says yes; a create that fails with it is made again without it),
 *   the driver's own fault report (vkGetDeviceFaultInfoEXT: description, fault addresses, vendor
 *   records) - armed like a compile (vkb_guard_run): a fault inside it (a player log's r54p3 driver faulted
 *   inside its own fault recording) or a call past LL_QUERY_S jumps back; the device is then stopped
 *   (never touched or destroyed again: a lock the driver held stays held) and the memory fault watchdog
 *   covers a call that blocks behind it. One line in the session report (malibridge-report.md).
 *   A device that answered lost but was never latched (a program that ends at its first lost answer)
 *   has its record written when it goes, without the driver query.
 *
 * The client answers finished fences itself (server_lost.c), so the server knows "finished" only where
 * a wait reached it: "NOT seen finished" is no proof a job ran long. The record is evidence for a reader,
 * not a verdict.
 *
 * VKBRIDGE_LOST_REPORT=0: off (the emergency switch: no ring, no tables, VK_EXT_device_fault not asked
 * for unless the hang detector wants it).
 * Imitation (host tests: lavapipe has no VK_EXT_device_fault), VKBRIDGE_TEST_DEVICE_FAULT=<mode>: the
 * driver offers it (it is not passed to lavapipe); va: its report has a page fault's address and a vendor
 * record; crash: the query faults inside the driver; hang: it never comes back; refuse: vkCreateDevice
 * refuses it once. VKBRIDGE_TEST_LOSTLOG_BENCH=<n>: at each device's creation, n recorded submissions
 * timed (the per-submit cost, logged).
 */
#define _GNU_SOURCE
#include "vkb_lostlog.h"

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define LL_MAX_RECORDS 4   /* records per server run */
#define LL_QUERY_S 2.0     /* the driver's fault report: its time limit */
#define LL_SHOW 12         /* submissions shown */
#define LL_SHOW_CB 6       /* ... of which the newest have their command buffers shown */
#define LL_SAID_S 30       /* an unconfirmed lost answer: recorded when its device goes within this */
#define LL_FAULT_MAX 8     /* fault addresses and vendor records shown */
#define CB_SLOTS 4096      /* server-wide tables (powers of two) */
#define PIPE_SLOTS 4096

/* ------------------------------------------------------------------ switches */

static int ll_off = -1;
static int test_mode; /* VKBRIDGE_TEST_DEVICE_FAULT: 1 va, 2 crash, 3 hang, 4 refuse */
static int records;   /* written this run */

static void env_once(void)
{
    if (__atomic_load_n(&ll_off, __ATOMIC_ACQUIRE) >= 0) return;
    const char *s = vkb_srv_env_global("VKBRIDGE_LOST_REPORT");
    int off = s && *s == '0';
    const char *m = getenv("VKBRIDGE_TEST_DEVICE_FAULT");
    test_mode = !m || !*m ? 0 : !strcmp(m, "crash") ? 2 : !strcmp(m, "hang") ? 3 : !strcmp(m, "refuse") ? 4 : 1;
    if (test_mode) VKB_WARN("VKBRIDGE_TEST_DEVICE_FAULT=%s: the driver offers VK_EXT_device_fault (imitated)", m);
    if (off) VKB_INFO("loss records off (VKBRIDGE_LOST_REPORT=0)");
    __atomic_store_n(&ll_off, off, __ATOMIC_RELEASE);
}

int vkb_ll_wanted(void)
{
    env_once();
    return !ll_off;
}

int vkb_ll_test_mode(void)
{
    env_once();
    return ll_off ? 0 : test_mode;
}

/* CLOCK_MONOTONIC (vDSO): a command buffer recorded again after its submission is told from one recorded before it,
 * which the coarse clock's 4 ms ticks could not */
static inline uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* ------------------------------------------------------------------ server-wide tables */

typedef struct cb_rec {
    uint64_t cb;   /* 0 while written */
    uint64_t t_ns;
    uint32_t cmds, bytes, draws, disp, copies, npipes;
    uint64_t pipes[VKB_LL_PIPES];
} cb_rec;

typedef struct pipe_rec {
    uint64_t p; /* 0 while written */
    uint64_t h[3];
    uint32_t stg[3];
    uint32_t nh, compute;
} pipe_rec;

static cb_rec cbs[CB_SLOTS];
static pipe_rec pipes[PIPE_SLOTS];

static inline uint32_t slot_of(uint64_t h, uint32_t n)
{
    h ^= h >> 17;
    h *= 0x9e3779b97f4a7c15ull;
    return (uint32_t)(h >> 40) & (n - 1);
}

void vkb_ll_cb_recorded(VkCommandBuffer cb, uint32_t cmds, size_t bytes, uint32_t draws, uint32_t disp, uint32_t copies,
                        const uint64_t *pl, uint32_t np)
{
    if (__builtin_expect(__atomic_load_n(&ll_off, __ATOMIC_RELAXED) != 0, 0) || !cb) return; /* off, or before the first device (nothing to lose yet) */
    cb_rec *r = &cbs[slot_of((uint64_t)(uintptr_t)cb, CB_SLOTS)];
    __atomic_store_n(&r->cb, 0, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    r->t_ns = now_ns();
    r->cmds = cmds, r->bytes = bytes > UINT32_MAX ? UINT32_MAX : (uint32_t)bytes, r->draws = draws, r->disp = disp, r->copies = copies;
    r->npipes = np > VKB_LL_PIPES ? VKB_LL_PIPES : np;
    for (uint32_t i = 0; i < r->npipes; i++) r->pipes[i] = pl[i];
    __atomic_store_n(&r->cb, (uint64_t)(uintptr_t)cb, __ATOMIC_RELEASE);
}

void vkb_ll_pipeline(VkPipeline p, const uint64_t *h, const uint32_t *stg, uint32_t nh, int compute)
{
    if (__builtin_expect(__atomic_load_n(&ll_off, __ATOMIC_RELAXED) != 0, 0) || !p) return;
    pipe_rec *r = &pipes[slot_of((uint64_t)(uintptr_t)p, PIPE_SLOTS)];
    __atomic_store_n(&r->p, 0, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    r->nh = nh > 3 ? 3 : nh;
    for (uint32_t i = 0; i < r->nh; i++) r->h[i] = h[i], r->stg[i] = stg ? stg[i] : 0;
    r->compute = (uint32_t)compute;
    __atomic_store_n(&r->p, (uint64_t)(uintptr_t)p, __ATOMIC_RELEASE);
}

/* A copy of the table's entry for h: 1 if found (and not rewritten while read). */
static int cb_find(uint64_t h, cb_rec *out)
{
    cb_rec *r = &cbs[slot_of(h, CB_SLOTS)];
    if (!h || __atomic_load_n(&r->cb, __ATOMIC_ACQUIRE) != h) return 0;
    *out = *r;
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    return __atomic_load_n(&r->cb, __ATOMIC_RELAXED) == h;
}

static int pipe_find(uint64_t h, pipe_rec *out)
{
    pipe_rec *r = &pipes[slot_of(h, PIPE_SLOTS)];
    if (!h || __atomic_load_n(&r->p, __ATOMIC_ACQUIRE) != h) return 0;
    *out = *r;
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    return __atomic_load_n(&r->p, __ATOMIC_RELAXED) == h;
}

/* ------------------------------------------------------------------ the submit path */

vkb_ll_sub *vkb_ll_submit(vkb_ll *l, int kind, VkQueue q, uint32_t n, const void *infos, VkFence f)
{
    uint32_t seq = __atomic_add_fetch(&l->n, 1, __ATOMIC_RELAXED);
    vkb_ll_sub *e = &l->ring[(seq - 1) & (VKB_LL_RING - 1)];
    __atomic_store_n(&e->seq, 0, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    e->kind = (uint8_t)kind;
    e->r = VKB_LL_IN_DRIVER; /* the answer is stored when the call returns (vkb_ll_answer) */
    e->t_ns = now_ns();
    e->queue = (uint64_t)(uintptr_t)q;
    e->fence = (uint64_t)(uintptr_t)f;
    e->nbatch = (uint16_t)(n > 0xffff ? 0xffff : n);
    uint32_t ncb = 0, nw = 0, ns = 0, k = 0;
    uint64_t sem = 0, val = 0;
    if (kind == 0) {
        const VkSubmitInfo *si = infos;
        for (uint32_t i = 0; si && i < n; i++) {
            ncb += si[i].commandBufferCount, nw += si[i].waitSemaphoreCount, ns += si[i].signalSemaphoreCount;
            for (uint32_t c = 0; c < si[i].commandBufferCount && k < VKB_LL_CBS; c++) e->cb[k++] = (uint64_t)(uintptr_t)si[i].pCommandBuffers[c];
            if (!sem && si[i].signalSemaphoreCount) {
                sem = (uint64_t)(uintptr_t)si[i].pSignalSemaphores[0];
                for (const VkBaseInStructure *b = si[i].pNext; b; b = b->pNext)
                    if (b->sType == VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO) {
                        const VkTimelineSemaphoreSubmitInfo *ti = (const VkTimelineSemaphoreSubmitInfo *)b;
                        if (ti->signalSemaphoreValueCount && ti->pSignalSemaphoreValues) val = ti->pSignalSemaphoreValues[0];
                        break;
                    }
            }
        }
    } else if (kind == 1) {
        const VkSubmitInfo2 *si = infos;
        for (uint32_t i = 0; si && i < n; i++) {
            ncb += si[i].commandBufferInfoCount, nw += si[i].waitSemaphoreInfoCount, ns += si[i].signalSemaphoreInfoCount;
            for (uint32_t c = 0; c < si[i].commandBufferInfoCount && k < VKB_LL_CBS; c++)
                e->cb[k++] = (uint64_t)(uintptr_t)si[i].pCommandBufferInfos[c].commandBuffer;
            if (!sem && si[i].signalSemaphoreInfoCount)
                sem = (uint64_t)(uintptr_t)si[i].pSignalSemaphoreInfos[0].semaphore, val = si[i].pSignalSemaphoreInfos[0].value;
        }
    } else {
        const VkBindSparseInfo *bi = infos;
        for (uint32_t i = 0; bi && i < n; i++) {
            nw += bi[i].waitSemaphoreCount, ns += bi[i].signalSemaphoreCount;
            if (!sem && bi[i].signalSemaphoreCount) sem = (uint64_t)(uintptr_t)bi[i].pSignalSemaphores[0];
        }
    }
    for (uint32_t c = k; c < VKB_LL_CBS; c++) e->cb[c] = 0;
    e->ncb = (uint16_t)(ncb > 0xffff ? 0xffff : ncb), e->nwait = (uint16_t)(nw > 0xffff ? 0xffff : nw), e->nsig = (uint16_t)(ns > 0xffff ? 0xffff : ns);
    e->sig_sem = sem, e->sig_val = val;
    __atomic_store_n(&e->seq, seq, __ATOMIC_RELEASE);
    return e;
}

static inline void done_add(vkb_ll *l, uint64_t h, uint64_t v, uint32_t after)
{
    vkb_ll_done *d = &l->done[__atomic_fetch_add(&l->dn, 1, __ATOMIC_RELAXED) & (VKB_LL_DONE - 1)];
    __atomic_store_n(&d->h, 0, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    d->v = v, d->after = after;
    __atomic_store_n(&d->h, h, __ATOMIC_RELEASE);
}

/* A fence wait or status that answered VK_SUCCESS: each fence of a wait for all of them (or the only one) finished. */
void vkb_ll_fences_done(vkb_ll *l, uint32_t n, const VkFence *f, VkBool32 all)
{
    if (!f || !n || (!all && n > 1)) return;
    uint32_t after = __atomic_load_n(&l->n, __ATOMIC_ACQUIRE);
    for (uint32_t i = 0; i < n && i < 4; i++)
        if (f[i]) done_add(l, (uint64_t)(uintptr_t)f[i], 0, after);
}

void vkb_ll_sems_done(vkb_ll *l, const VkSemaphoreWaitInfo *wi)
{
    if (!wi || !wi->pSemaphores || !wi->pValues || (wi->semaphoreCount > 1 && (wi->flags & VK_SEMAPHORE_WAIT_ANY_BIT))) return;
    uint32_t after = __atomic_load_n(&l->n, __ATOMIC_ACQUIRE);
    for (uint32_t i = 0; i < wi->semaphoreCount && i < 4; i++)
        if (wi->pSemaphores[i]) done_add(l, (uint64_t)(uintptr_t)wi->pSemaphores[i], wi->pValues[i], after);
}

/* ------------------------------------------------------------------ the record */

static const char *const kind_names[3] = {"vkQueueSubmit", "vkQueueSubmit2", "vkQueueBindSparse"};

static const char *stage_name(uint32_t s)
{
    switch (s) {
    case VK_SHADER_STAGE_VERTEX_BIT: return "vs";
    case VK_SHADER_STAGE_FRAGMENT_BIT: return "fs";
    case VK_SHADER_STAGE_COMPUTE_BIT: return "cs";
    case VK_SHADER_STAGE_GEOMETRY_BIT: return "gs";
    case VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT: return "tcs";
    case VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT: return "tes";
    default: return "stage";
    }
}

#define ADD(buf, n, ...)                                                                                       \
    do {                                                                                                       \
        if ((n) >= 0 && (size_t)(n) < sizeof(buf)) (n) += snprintf((buf) + (n), sizeof(buf) - (size_t)(n), __VA_ARGS__); \
    } while (0)

static int finished(const vkb_ll *l, const vkb_ll_sub *s)
{
    uint32_t dn = __atomic_load_n(&l->dn, __ATOMIC_ACQUIRE);
    for (uint32_t k = 0; k < VKB_LL_DONE && k < dn; k++) {
        const vkb_ll_done *d = &l->done[(dn - 1 - k) & (VKB_LL_DONE - 1)];
        uint64_t h = __atomic_load_n(&d->h, __ATOMIC_ACQUIRE);
        if (!h || d->after < s->seq) continue; /* said before this submission was recorded: of an earlier one */
        if (s->fence && h == s->fence && !d->v) return 1;
        if (s->sig_sem && h == s->sig_sem && s->sig_val && d->v >= s->sig_val) return 1;
    }
    return 0;
}

typedef struct snap {
    vkb_ll_sub s[VKB_LL_RING];
    int fin[VKB_LL_RING]; /* 1 a wait said so, 2 a later one on its queue finished */
    uint32_t n, first, total;
} snap;

/* The ring as it is now, oldest first. */
static void take(const vkb_ll *l, snap *sn)
{
    uint32_t total = __atomic_load_n(&l->n, __ATOMIC_ACQUIRE);
    uint32_t first = total > VKB_LL_RING ? total - VKB_LL_RING : 0;
    sn->n = 0;
    sn->total = total;
    for (uint32_t i = first; i < total; i++) {
        const vkb_ll_sub *e = &l->ring[i & (VKB_LL_RING - 1)];
        if (__atomic_load_n(&e->seq, __ATOMIC_ACQUIRE) != i + 1) continue;
        vkb_ll_sub c = *e;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (__atomic_load_n(&e->seq, __ATOMIC_RELAXED) != i + 1) continue; /* rewritten while read */
        sn->s[sn->n] = c;
        sn->fin[sn->n] = finished(l, &c);
        sn->n++;
    }
    /* a submission on a queue finished: every earlier one on that queue did (a fence's signal covers them) */
    for (int i = (int)sn->n - 1; i >= 0; i--)
        if (sn->fin[i])
            for (int k = i - 1; k >= 0; k--)
                if (!sn->fin[k] && sn->s[k].queue == sn->s[i].queue) sn->fin[k] = 2;
}

static void say_cb(const vkb_ll *l, uint64_t h, uint64_t sub_ns, uint64_t ref_ns)
{
    char line[900];
    int n = 0;
    cb_rec c;
    uint32_t id = l->t->id;
    if (!cb_find(h, &c)) {
        VKB_INFO("loss record, device %u:     command buffer %#llx: no record (not a stream of the client's, or recorded long before)", id,
                (unsigned long long)h);
        return;
    }
    ADD(line, n, "command buffer %#llx: %u commands (%u bytes), %u draws, %u dispatches, %u copies to images", (unsigned long long)h, c.cmds,
        c.bytes, c.draws, c.disp, c.copies);
    if (c.t_ns > sub_ns) ADD(line, n, " (recorded again after this submission: what it holds now)");
    for (uint32_t i = 0; i < c.npipes; i++) {
        pipe_rec p;
        ADD(line, n, "%s", i ? ", " : "; pipelines bound, last first: ");
        if (!pipe_find(c.pipes[i], &p)) {
            ADD(line, n, "%#llx", (unsigned long long)c.pipes[i]);
            continue;
        }
        ADD(line, n, "%s %#llx (", p.compute ? "compute" : "graphics", (unsigned long long)c.pipes[i]);
        for (uint32_t k = 0; k < p.nh; k++) ADD(line, n, "%s%s h-%016llx", k ? " " : "", stage_name(p.stg[k]), (unsigned long long)p.h[k]);
        if (!p.nh) ADD(line, n, "stages not hashed");
        ADD(line, n, ")");
    }
    (void)ref_ns;
    VKB_INFO("loss record, device %u:     %s", id, line);
}

/* The submissions, newest last; into sum (the report's line) the oldest not seen finished. */
static void say_record(vkb_ll *l, uint64_t ref_ns, const char *head, char *sum, size_t sum_cap)
{
    static snap sn; /* under say_lock */
    vkb_srv_table *t = l->t;
    uint32_t id = t->id;
    take(l, &sn);
    VKB_ERR("loss record, device %u (pid %d, %s): %s; what it was given, newest last: the last %u of %u submission(s) "
            "(\"finished\": a wait or fence status said so, or a later one on its queue did; the client answers finished fences "
            "itself, so NOT seen finished is no proof a job ran long)",
            id, t->proc ? t->proc->pid : 0, t->proc && t->proc->comm[0] ? t->proc->comm : "?", head, sn.n < LL_SHOW ? sn.n : LL_SHOW, sn.total);
    uint32_t from = sn.n > LL_SHOW ? sn.n - LL_SHOW : 0;
    for (uint32_t i = from; i < sn.n; i++) {
        const vkb_ll_sub *s = &sn.s[i];
        char sig[96] = "";
        if (s->sig_sem && s->sig_val) snprintf(sig, sizeof(sig), " (semaphore %#llx to %llu)", (unsigned long long)s->sig_sem, (unsigned long long)s->sig_val);
        char st[64];
        if (s->r == VKB_LL_IN_DRIVER) snprintf(st, sizeof(st), "still in the driver's vkQueueSubmit when this was written");
        else if (s->r) snprintf(st, sizeof(st), "the driver answered %d", s->r);
        else snprintf(st, sizeof(st), "%s", sn.fin[i] == 1 ? "finished" : sn.fin[i] == 2 ? "finished (a later one on its queue did)" : "NOT seen finished");
        double before = ref_ns > s->t_ns ? (ref_ns - s->t_ns) / 1e9 : 0;
        VKB_INFO("loss record, device %u:   submission %u of %u, %.3f s before the loss: %s, queue %#llx, %u batch(es), %u command buffer(s), "
                "%u wait(s), %u signal(s)%s, fence %#llx: %s",
                id, s->seq, sn.total, before, kind_names[s->kind < 3 ? s->kind : 0], (unsigned long long)s->queue, s->nbatch, s->ncb, s->nwait,
                s->nsig, sig, (unsigned long long)s->fence, st);
        if (i + LL_SHOW_CB >= sn.n)
            for (uint32_t c = 0; c < VKB_LL_CBS && c < s->ncb; c++)
                if (s->cb[c]) say_cb(l, s->cb[c], s->t_ns, ref_ns);
        if (s->ncb > VKB_LL_CBS && i + LL_SHOW_CB >= sn.n)
            VKB_INFO("loss record, device %u:     (%u more command buffer(s) not named)", id, s->ncb - VKB_LL_CBS);
    }
    int oldest = -1;
    for (uint32_t i = 0; i < sn.n && oldest < 0; i++)
        if (!sn.fin[i] && (!sn.s[i].r || sn.s[i].r == VKB_LL_IN_DRIVER)) oldest = (int)i;
    int n = 0;
    sum[0] = 0;
    if (oldest >= 0) {
        const vkb_ll_sub *s = &sn.s[oldest];
        double before = ref_ns > s->t_ns ? (ref_ns - s->t_ns) / 1e9 : 0;
        VKB_ERR("loss record, device %u:   the oldest submission not seen finished: %u of %u, %.3f s before the loss", id, s->seq, sn.total, before);
        if (n >= 0 && (size_t)n < sum_cap)
            n += snprintf(sum + n, sum_cap - (size_t)n, "submission %u of %u not seen finished (%.3f s before the loss, %u command buffer(s))", s->seq,
                          sn.total, before, s->ncb);
        cb_rec c;
        if (s->ncb && cb_find(s->cb[0], &c) && n >= 0 && (size_t)n < sum_cap)
            n += snprintf(sum + n, sum_cap - (size_t)n, ", its first: %u commands, %u draws, %u dispatches", c.cmds, c.draws, c.disp);
    } else {
        VKB_ERR("loss record, device %u:   every submission recorded was seen finished (or refused by the driver)", id);
        snprintf(sum, sum_cap, "%u submission(s), every one seen finished", sn.total);
    }
}

/* ------------------------------------------------------------------ the driver's fault report */

typedef struct query {
    vkb_ll *l;
    VkResult r;
    VkDeviceFaultCountsEXT counts;
    VkDeviceFaultInfoEXT info;
    VkDeviceFaultAddressInfoEXT addr[LL_FAULT_MAX];
    VkDeviceFaultVendorInfoEXT vend[LL_FAULT_MAX];
    uint64_t binary;
} query;

static void imitate(query *q)
{
    vkb_srv_table *t = q->l->t;
    if (test_mode == 2) {
        /* a player log: the driver faults inside its fault report (a driver query writing through 0x101) */
        PFN_vkGetPhysicalDeviceQueueFamilyProperties f = t->parent ? t->parent->real.vkGetPhysicalDeviceQueueFamilyProperties : NULL;
        if (f) f(t->physical, (uint32_t *)(uintptr_t)0x101, NULL);
        *(volatile int *)(uintptr_t)0x101 = 1; /* no query: fault here */
    }
    if (test_mode == 3)
        for (;;) {
            struct timespec ts = {0, 50000000};
            nanosleep(&ts, NULL);
        }
    q->r = VK_SUCCESS;
    q->counts.addressInfoCount = 1, q->counts.vendorInfoCount = 1, q->binary = 0;
    snprintf(q->info.description, sizeof(q->info.description), "imitated: a GPU page fault");
    q->addr[0].addressType = VK_DEVICE_FAULT_ADDRESS_TYPE_READ_INVALID_EXT;
    q->addr[0].reportedAddress = 0x7f1234560000ull;
    q->addr[0].addressPrecision = 4096;
    snprintf(q->vend[0].description, sizeof(q->vend[0].description), "imitated: job timeout");
    q->vend[0].vendorFaultCode = 4;
    q->vend[0].vendorFaultData = 0x58;
}

static void run_query(void *arg)
{
    query *q = arg;
    if (q->l->t->device_fault == 2) {
        imitate(q);
        return;
    }
    VkDevice dev = q->l->t->device;
    VkDeviceFaultCountsEXT c = {VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT};
    q->r = q->l->fault_info(dev, &c, NULL);
    if (q->r != VK_SUCCESS && q->r != VK_INCOMPLETE) return;
    q->binary = c.vendorBinarySize;
    if (c.addressInfoCount > LL_FAULT_MAX) c.addressInfoCount = LL_FAULT_MAX;
    if (c.vendorInfoCount > LL_FAULT_MAX) c.vendorInfoCount = LL_FAULT_MAX;
    c.vendorBinarySize = 0; /* its size only */
    q->info.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT;
    q->info.pAddressInfos = c.addressInfoCount ? q->addr : NULL;
    q->info.pVendorInfos = c.vendorInfoCount ? q->vend : NULL;
    q->info.pVendorBinaryData = NULL;
    q->r = q->l->fault_info(dev, &c, &q->info);
    q->counts = c;
}

static void printable(char *s, size_t cap)
{
    s[cap - 1] = 0;
    for (; *s; s++)
        if ((unsigned char)*s < 0x20 || (unsigned char)*s >= 0x7f || *s == '"') *s = '?';
}

static const char *addr_kind(VkDeviceFaultAddressTypeEXT k)
{
    static const char *const names[] = {"none", "read", "write", "execute", "pc unknown", "pc invalid", "pc fault"};
    return (unsigned)k < 7 ? names[k] : "?";
}

/* The driver's report (or why none), its lines; into sum the report's part. */
static void say_fault(vkb_ll *l, char *sum, size_t cap)
{
    vkb_srv_table *t = l->t;
    uint32_t id = t->id;
    const char *none = NULL;
    if (!l->driver_said) none = "no fault report asked for: the bridge took the device as lost, the driver did not say so";
    else if (t->device_fault == -1) none = "no fault report from the driver (VK_EXT_device_fault refused at vkCreateDevice)";
    else if (t->device_fault == -2) none = "no fault report from the driver (it lists VK_EXT_device_fault without its deviceFault feature)";
    else if (t->device_fault <= 0 || (t->device_fault == 1 && !l->fault_info))
        none = "no fault report from the driver (it does not offer VK_EXT_device_fault)";
    else if (!vkb_guard_armable()) none = "no fault report asked for: driver crash containment is off or used up";
    if (none) {
        VKB_ERR("loss record, device %u: %s", id, none);
        snprintf(sum, cap, "%s", none);
        return;
    }
    static query q; /* under say_lock */
    memset(&q, 0, sizeof(q));
    q.l = l;
    int sig = 0;
    int g = vkb_guard_run("vkGetDeviceFaultInfoEXT (the loss record)", LL_QUERY_S, run_query, &q, &sig);
    if (g != VKB_GUARD_DONE) {
        char why[200];
        if (g == VKB_GUARD_CRASHED)
            snprintf(why, sizeof(why), "the driver's fault report faulted (signal %d) and was stopped there", sig);
        else
            snprintf(why, sizeof(why), "the driver's fault report did not come back within %.0f s and was interrupted", LL_QUERY_S);
        VKB_ERR("loss record, device %u: %s: the device is left as it is (a lock the driver held stays held), its program's further "
                "calls are refused, the server goes on",
                id, why);
        snprintf(sum, cap, "%s", why);
        vkb_guard_contained_elsewhere("vkGetDeviceFaultInfoEXT", id);
        vkb_lost_stop(t, g == VKB_GUARD_CRASHED ? "its driver's fault report faulted" : "its driver's fault report did not come back");
        return;
    }
    if (q.r != VK_SUCCESS && q.r != VK_INCOMPLETE) {
        VKB_ERR("loss record, device %u: the driver's fault report: vkGetDeviceFaultInfoEXT answered %d", id, q.r);
        snprintf(sum, cap, "the driver's fault report: vkGetDeviceFaultInfoEXT answered %d", q.r);
        return;
    }
    printable(q.info.description, sizeof(q.info.description));
    VKB_ERR("loss record, device %u: the driver's fault report (VK_EXT_device_fault, %s): \"%s\", %u address(es), %u vendor record(s)%s", id,
            q.r == VK_SUCCESS ? "VK_SUCCESS" : "VK_INCOMPLETE", q.info.description, q.counts.addressInfoCount, q.counts.vendorInfoCount,
            "");
    if (q.binary) VKB_INFO("loss record, device %u:   and %llu bytes of vendor binary data (not read)", id, (unsigned long long)q.binary);
    int n = snprintf(sum, cap, "driver: \"%s\"", q.info.description);
    for (uint32_t i = 0; i < q.counts.addressInfoCount && i < LL_FAULT_MAX; i++) {
        VKB_INFO("loss record, device %u:   fault address %#llx (%s, precision %llu)", id, (unsigned long long)q.addr[i].reportedAddress,
                addr_kind(q.addr[i].addressType), (unsigned long long)q.addr[i].addressPrecision);
        vkb_hang_fault_addr(t, q.addr[i].reportedAddress); /* with the hang detector on: the buffer it falls in */
        if (i == 0 && n >= 0 && (size_t)n < cap)
            n += snprintf(sum + n, cap - (size_t)n, ", fault address %#llx (%s)", (unsigned long long)q.addr[i].reportedAddress,
                          addr_kind(q.addr[i].addressType));
    }
    for (uint32_t i = 0; i < q.counts.vendorInfoCount && i < LL_FAULT_MAX; i++) {
        printable(q.vend[i].description, sizeof(q.vend[i].description));
        VKB_INFO("loss record, device %u:   vendor: \"%s\" code %#llx data %#llx", id, q.vend[i].description,
                (unsigned long long)q.vend[i].vendorFaultCode, (unsigned long long)q.vend[i].vendorFaultData);
        if (i == 0 && n >= 0 && (size_t)n < cap)
            n += snprintf(sum + n, cap - (size_t)n, ", vendor \"%s\" code %#llx", q.vend[i].description, (unsigned long long)q.vend[i].vendorFaultCode);
    }
    /* A GPU fault at an address: the next launch of this game gets the hang detector, which names the writer
     * (server_hang.c; God of War, a player log: the record alone could not). */
    if (q.counts.addressInfoCount) vkb_hang_arm_next(t, q.info.description);
}

static pthread_mutex_t say_lock = PTHREAD_MUTEX_INITIALIZER; /* one record at a time (the static buffers) */

/* 1: this one may be written (LL_MAX_RECORDS a run) */
static int record_take(void)
{
    int k = __atomic_add_fetch(&records, 1, __ATOMIC_RELAXED);
    if (k == LL_MAX_RECORDS + 1) VKB_WARN("loss records: %d written this run, no more", LL_MAX_RECORDS);
    return k <= LL_MAX_RECORDS;
}

static void write_record(vkb_ll *l, int query_driver)
{
    char rec_sum[300], fault_sum[400], line[760];
    pthread_mutex_lock(&say_lock);
    if (query_driver) {
        say_record(l, l->lost_ns, l->why, rec_sum, sizeof(rec_sum));
        say_fault(l, fault_sum, sizeof(fault_sum));
    } else {
        char head[300];
        snprintf(head, sizeof(head), "said lost (%s) and not confirmed before the device went (no fault report asked for)", l->said ? l->said : "?");
        say_record(l, l->said_ns, head, rec_sum, sizeof(rec_sum));
        snprintf(fault_sum, sizeof(fault_sum), "said lost, not confirmed");
    }
    pthread_mutex_unlock(&say_lock);
    snprintf(line, sizeof(line), "loss record: device %u, %s; %s", l->t->id, rec_sum, fault_sum);
    vkb_report_lost_cause(l->t->proc ? l->t->proc->pid : 0, line);
}

static void *helper_main(void *arg)
{
    vkb_ll *l = arg;
    /* the guard's time-out signal and the faults reach this thread whatever its creator blocked */
    sigset_t s;
    sigemptyset(&s);
    sigaddset(&s, SIGSEGV), sigaddset(&s, SIGBUS), sigaddset(&s, SIGILL), sigaddset(&s, SIGFPE);
    for (int k = SIGRTMIN; k <= SIGRTMAX; k++) sigaddset(&s, k);
    pthread_sigmask(SIG_UNBLOCK, &s, NULL);
    write_record(l, 1);
    __atomic_store_n(&l->helper, 2, __ATOMIC_RELEASE);
    return NULL;
}

/* ------------------------------------------------------------------ server_lost.c's events */

void vkb_ll_lost(vkb_ll *l, const char *why, int driver_said)
{
    if (!l || __atomic_exchange_n(&l->reported, 1, __ATOMIC_ACQ_REL)) return;
    l->lost_ns = now_ns();
    snprintf(l->why, sizeof(l->why), "lost (%s)", why ? why : "?");
    l->driver_said = driver_said;
    if (!record_take()) return;
    __atomic_store_n(&l->helper, 1, __ATOMIC_RELEASE);
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    pthread_t th;
    if (pthread_create(&th, &a, helper_main, l) != 0) {
        __atomic_store_n(&l->helper, 0, __ATOMIC_RELEASE);
        VKB_WARN("loss record, device %u: no thread for it", l->t->id);
    }
    pthread_attr_destroy(&a);
}

void vkb_ll_said(vkb_ll *l, const char *what)
{
    if (!l || l->said) return;
    l->said_ns = now_ns();
    __atomic_store_n(&l->said, what, __ATOMIC_RELEASE);
}

void vkb_ll_unsaid(vkb_ll *l)
{
    if (l) __atomic_store_n(&l->said, NULL, __ATOMIC_RELEASE);
}

int vkb_ll_busy(vkb_ll *l, double wait_s)
{
    if (!l) return 0;
    for (int i = 0; __atomic_load_n(&l->helper, __ATOMIC_ACQUIRE) == 1; i++) {
        if (i >= (int)(wait_s * 100)) return 1;
        struct timespec ts = {0, 10000000};
        nanosleep(&ts, NULL);
    }
    return 0;
}

static void bench(vkb_srv_table *t, int n)
{
    vkb_ll *b = calloc(1, sizeof(*b));
    if (!b) return;
    b->t = t;
    VkCommandBuffer cb[2] = {(VkCommandBuffer)(uintptr_t)0x1000, (VkCommandBuffer)(uintptr_t)0x2000};
    VkSemaphore sem = (VkSemaphore)(uintptr_t)0x3000;
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 2, cb, 1, &sem};
    VkFence f = (VkFence)(uintptr_t)0x4000;
    struct timespec a, z;
    clock_gettime(CLOCK_MONOTONIC, &a);
    for (int i = 0; i < n; i++) vkb_ll_answer(vkb_ll_submit(b, 0, (VkQueue)(uintptr_t)0x5000, 1, &si, f), VK_SUCCESS);
    clock_gettime(CLOCK_MONOTONIC, &z);
    double sub = ((z.tv_sec - a.tv_sec) * 1e9 + (z.tv_nsec - a.tv_nsec)) / n;
    clock_gettime(CLOCK_MONOTONIC, &a);
    for (int i = 0; i < n; i++) vkb_ll_fences_done(b, 1, &f, VK_TRUE);
    clock_gettime(CLOCK_MONOTONIC, &z);
    double fin = ((z.tv_sec - a.tv_sec) * 1e9 + (z.tv_nsec - a.tv_nsec)) / n;
    VKB_WARN("loss record bench (VKBRIDGE_TEST_LOSTLOG_BENCH=%d): %.1f ns per submission recorded (1 batch, 2 command buffers, a signal, "
             "a fence), %.1f ns per finished fence wait recorded",
             n, sub, fin);
    free(b);
}

vkb_ll *vkb_ll_device_init(vkb_srv_table *t)
{
    env_once();
    if (ll_off) return NULL;
    vkb_ll *l = calloc(1, sizeof(*l));
    if (!l) return NULL;
    l->t = t;
    if (t->device_fault == 1) {
        PFN_vkGetDeviceProcAddr gdpa = (PFN_vkGetDeviceProcAddr)vkb_gipa(t->instance, "vkGetDeviceProcAddr");
        if (gdpa) l->fault_info = (PFN_vkGetDeviceFaultInfoEXT)gdpa(t->device, "vkGetDeviceFaultInfoEXT");
    }
    const char *b = getenv("VKBRIDGE_TEST_LOSTLOG_BENCH");
    if (b && atoi(b) > 0) bench(t, atoi(b));
    return l;
}

void vkb_ll_device_destroy(vkb_ll *l)
{
    if (!l) return;
    const char *said = __atomic_load_n(&l->said, __ATOMIC_ACQUIRE);
    if (said && !__atomic_exchange_n(&l->reported, 1, __ATOMIC_ACQ_REL) && now_ns() - l->said_ns < (uint64_t)LL_SAID_S * 1000000000ull &&
        record_take())
        write_record(l, 0); /* memory only: no driver call */
    if (__atomic_load_n(&l->helper, __ATOMIC_ACQUIRE) == 1) return; /* still running (vkb_lost_settle said so): kept */
    free(l);
}
