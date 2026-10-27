/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * wait_retry (MK-01): waits the driver ends with an error it may not return. Arm's blob waits
 * with ppoll() and does not restart it when a signal interrupts it (EINTR); the wait then fails
 * with VK_ERROR_INITIALIZATION_FAILED (-3) - Citra/Azahar retry their present waits for it. In the
 * in-app server the runtime signals threads routinely (garbage collection, profiling): the
 * Mali-G925 (Android 16, The Witcher 3 and Stray through vkd3d-proton) answered vkWaitSemaphores
 * with -3 every 7-10 s, and vkd3d-proton retires a submission it cannot wait for.
 *
 * An erratum of server_errata.c, on for every driver: the fix sits just above the driver
 * (t->real), so the bridge's own waits are covered as well as the app's. On a correct driver it
 * costs one comparison per wait. Covered: vkWaitForFences, vkWaitSemaphores, vkQueueWaitIdle,
 * vkDeviceWaitIdle, vkGetQueryPoolResults with VK_QUERY_RESULT_WAIT_BIT, and vkGetFenceStatus
 * (polled waits). vkAcquireNextImageKHR is the client's own (client/wsi*.c), which waits on a
 * fence over the wire: covered by the fence wait.
 *
 * A wait the driver fails with an error it may not return (anything but success, a timeout,
 * VK_NOT_READY, device lost or out of memory) is tried again, for the app's whole timeout:
 *   - before each retry the wait is polled (timeout 0; vkGetQueryPoolResults without WAIT_BIT),
 *     so signalled fences or semaphores, finished queries, end it at once with VK_SUCCESS;
 *   - at the app's own deadline the app gets VK_TIMEOUT, which is the right answer then;
 *   - with no deadline (UINT64_MAX, the idle waits, a query wait) the driver's error reaches the
 *     app only after the driver has kept failing for 30 s (a GPU that is truly gone must not hang
 *     the app silently); a line every 10 s meanwhile. VKBRIDGE_WAIT_RETRY_MS caps the retrying
 *     (finite timeouts too; the driver's error then);
 *   - a driver that fails at once is not spun on: naps from 0.5 ms, doubling to 20 ms.
 * vkGetFenceStatus failed the same way is asked again three times (1 ms apart), then answered
 * VK_NOT_READY: the app polls again.
 *
 * An early VK_TIMEOUT: a fence or semaphore wait the driver ends with VK_TIMEOUT before the
 * app's timeout has passed - on a wait with no timeout (UINT64_MAX), which may not time out at all -
 * is not done yet: the wait goes on, by the same loop (polled first, so what is signalled ends it at
 * once), until the work finishes or the app's real deadline; with no deadline, until the 30 s cap
 * (VKBRIDGE_WAIT_RETRY_MS changes it), a line every 10 s, and the cap's VK_TIMEOUT is always logged. Stray on the Dimensity 9500 (Mali-G1-
 * Ultra, r54p1) ended with DXGI_ERROR_DEVICE_RESET and no error in either log: DXVK's finisher
 * thread waits on its timelines with no timeout and latches anything but VK_SUCCESS as a lost
 * device, silently; the bridge passed the driver's VK_TIMEOUT through as a legal result. A timeout
 * within EARLY_SLACK of the deadline (drivers round the timeout to their clock) is the app's. A
 * driver that follows the spec never takes this path; the first case per kind is logged with the
 * time it took and the semaphore values, then a tally.
 * The first case of each kind is logged in full, then a tally at 10, 100, 1000, ... cases; a wait
 * whose driver error reaches the app is always logged.
 *
 * A semaphore wait past its deadline with the work already submitted: DXVK since
 * 852d454f6 (2026-10-08; Proton Experimental 11.0-20261014 has it) waits on its timelines for
 * 5 s, not for ever, and takes a VK_TIMEOUT there as a lost device ("DxvkSubmissionQueue: Timeout
 * occured, GPU likely hung."): Stray on the Dimensity 9500 (Mali-G1-Ultra, r54p1) then ended with
 * DXGI_ERROR_DEVICE_RESET at its next frame, the driver having lost nothing - one submission of the
 * loading burst took longer than 5 s on that GPU. A vkWaitSemaphores whose timeout of at least
 * GRACE_MIN runs out while every semaphore it waits for (one, for VK_SEMAPHORE_WAIT_ANY_BIT) has a
 * signal of the wanted value already submitted to a queue is waited on for up to
 * VKBRIDGE_WAIT_GRACE_S more (default 25 s; 0 turns it off): the work is on the GPU, a later answer
 * beats a lost device. What nothing has been submitted to signal (a host signal still to come, a
 * frame not yet submitted) times out at the app's deadline as before, as do polls and short
 * timeouts. The values submitted are taken from vkQueueSubmit / vkQueueSubmit2 (one lock per
 * submit); the first case is logged with how long the work took, then a tally; a wait the grace did
 * not save is always logged.
 *
 * Switches: VKBRIDGE_ERRATA=-wait_retry turns the fix off (VKBRIDGE_WAIT_FIX=0 still does).
 * The imitation (VKBRIDGE_ERRATA_IMITATE=wait_retry on host builds; VKBRIDGE_WAIT_FAKE_ERROR set
 * imitates on any build) fails waits with -3 beneath the fix, after up to 20 ms of the driver's
 * own wait, as an interrupting signal would:
 *   VKBRIDGE_WAIT_FAKE_ERROR=<how>[:kind+kind...]
 *     how:  unset or "once" every blocking wait the app or the bridge makes, but not the fix's
 *                           retries of it (a signal interrupts each wait once); "alt" every other
 *                           blocking call of each kind, per thread, retries too; "all" every one
 *                           (only a cap or the app's deadline ends a wait); <n> the first n calls
 *                           of each kind
 *     kind: fence, sem, idle (both idle waits), query, status (vkGetFenceStatus); default all but
 *           status (an app polling a fence status that never answers would never end)
 *   VKBRIDGE_WAIT_FAKE_RESULT=timeout: the fence and semaphore waits taken end with VK_TIMEOUT
 *           instead of -3, and only when the driver's own wait of up to 20 ms did not finish and the
 *           app's timeout is longer than that (the early timeout above; -3 stays on the other kinds)
 * Other non-blocking calls (timeout 0, a query read without WAIT_BIT) are never failed.
 */
#define _GNU_SOURCE
#include "server_errata.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MS 1000000ull
#define SEC 1000000000ull
#define NO_DEADLINE_CAP (30 * SEC)
#define PROGRESS_EVERY (10 * SEC)
#define EARLY_SLACK (2 * MS) /* a VK_TIMEOUT this close to the deadline is the app's */
#define GRACE_MIN SEC         /* shorter timeouts are polls or the app's own pacing: never extended */
#define GRACE_DEFAULT (25 * SEC)

enum { K_FENCE, K_SEM, K_QUEUE_IDLE, K_DEVICE_IDLE, K_QUERY, K_STATUS, K_COUNT };
static const char *const kind_name[K_COUNT] = {"vkWaitForFences", "vkWaitSemaphores", "vkQueueWaitIdle",
                                               "vkDeviceWaitIdle", "vkGetQueryPoolResults (WAIT_BIT)", "vkGetFenceStatus"};

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * SEC + (uint64_t)ts.tv_nsec;
}

static void nap_ns(uint64_t ns)
{
    struct timespec ts = {(time_t)(ns / SEC), (long)(ns % SEC)};
    nanosleep(&ts, NULL);
}

/* What a wait may return; anything else is the driver's bug */
static int legal(VkResult r)
{
    return r >= 0 || r == VK_ERROR_DEVICE_LOST || r == VK_ERROR_OUT_OF_HOST_MEMORY || r == VK_ERROR_OUT_OF_DEVICE_MEMORY;
}

/* A VK_TIMEOUT before the app's deadline (t0: when its call began) */
static int early_timeout(VkResult r, uint64_t t0, uint64_t timeout)
{
    if (r != VK_TIMEOUT || timeout <= EARLY_SLACK) return 0;
    uint64_t took = now_ns() - t0;
    return timeout == UINT64_MAX || took + EARLY_SLACK < timeout;
}

/* VKBRIDGE_WAIT_RETRY_MS, read at each install (< 0: not set) */
static int64_t cap_override_ns = -1;
/* In the fix's retries (the imitation's "once" spares them) */
static __thread int retrying;

/* ------------------------------------------------------------------ the tally and the log */

enum { O_SUCCESS, O_TIMEOUT, O_DRIVER_ERROR, O_OTHER, O_COUNT };
static unsigned long tally[K_COUNT][O_COUNT + 1]; /* [O_COUNT]: cases */

static void count(int kind, int outcome, VkResult first, VkResult out, uint64_t failed_after, int tries, uint64_t retried,
                  uint64_t timeout, const char *detail)
{
    __atomic_add_fetch(&tally[kind][outcome], 1, __ATOMIC_RELAXED);
    unsigned long n = __atomic_add_fetch(&tally[kind][O_COUNT], 1, __ATOMIC_RELAXED);
    char tmo[48];
    if (timeout == UINT64_MAX)
        snprintf(tmo, sizeof(tmo), "no timeout");
    else if (!timeout)
        snprintf(tmo, sizeof(tmo), "a poll");
    else
        snprintf(tmo, sizeof(tmo), "timeout %.3f s", timeout / 1e9);
    if (n == 1 || outcome == O_DRIVER_ERROR)
        VKB_WARN("wait_retry: %s: the driver failed the wait (%d) after %.3f s (%s)%s; %d retry(ies) in %.3f s: the app gets %d%s",
                 kind_name[kind], first, failed_after / 1e9, tmo, detail ? detail : "", tries, retried / 1e9, out,
                 outcome == O_SUCCESS ? " (VK_SUCCESS)" : outcome == O_TIMEOUT ? " (VK_TIMEOUT, its deadline)"
                 : outcome == O_DRIVER_ERROR ? " (the driver's error: it kept failing)" : "");
    unsigned long p = 10;
    while (p < n && p < 1000000000ul) p *= 10;
    if (n >= 10 && n == p)
        VKB_INFO("wait_retry: %s: %lu wait(s) failed by the driver so far: %lu ended in VK_SUCCESS, %lu in VK_TIMEOUT at the "
                 "app's deadline, %lu with the driver's error, %lu otherwise", kind_name[kind], n,
                 __atomic_load_n(&tally[kind][O_SUCCESS], __ATOMIC_RELAXED), __atomic_load_n(&tally[kind][O_TIMEOUT], __ATOMIC_RELAXED),
                 __atomic_load_n(&tally[kind][O_DRIVER_ERROR], __ATOMIC_RELAXED), __atomic_load_n(&tally[kind][O_OTHER], __ATOMIC_RELAXED));
}

/* An early VK_TIMEOUT's own tally (the -3 cases, every 7-10 s on Mali, would hide its first line),
 * apart for waits with no timeout (DXVK's timelines) and finite ones */
static unsigned long early_tally[K_COUNT * 2][O_COUNT + 1];
#define EARLY_IX(kind, timeout) ((kind) * 2 + ((timeout) == UINT64_MAX))

static void count_early(int kind, int outcome, VkResult out, uint64_t failed_after, int tries, uint64_t waited, uint64_t timeout,
                        const char *detail)
{
    unsigned long *et = early_tally[EARLY_IX(kind, timeout)];
    __atomic_add_fetch(&et[outcome], 1, __ATOMIC_RELAXED);
    unsigned long n = __atomic_add_fetch(&et[O_COUNT], 1, __ATOMIC_RELAXED);
    char tmo[48];
    if (timeout == UINT64_MAX)
        snprintf(tmo, sizeof(tmo), "no timeout");
    else
        snprintf(tmo, sizeof(tmo), "timeout %.3f s", timeout / 1e9);
    if (n == 1 || outcome == O_DRIVER_ERROR || (outcome == O_TIMEOUT && timeout == UINT64_MAX))
        VKB_WARN("wait_retry: %s: the driver returned VK_TIMEOUT after %.3f s, before the app's deadline (%s)%s; waited on "
                 "(%d more wait(s), %.3f s): the app gets %d%s", kind_name[kind], failed_after / 1e9, tmo, detail ? detail : "",
                 tries, waited / 1e9, out, out == VK_SUCCESS ? " (VK_SUCCESS)" : out == VK_TIMEOUT ? " (VK_TIMEOUT, its deadline)" : "");
    unsigned long p = 10;
    while (p < n && p < 1000000000ul) p *= 10;
    if (n >= 10 && n == p)
        VKB_INFO("wait_retry: %s (%s): %lu early VK_TIMEOUT(s) from the driver so far: %lu ended in VK_SUCCESS, %lu in VK_TIMEOUT "
                 "at the app's deadline, %lu otherwise", kind_name[kind], timeout == UINT64_MAX ? "no timeout" : "finite", n,
                 __atomic_load_n(&et[O_SUCCESS], __ATOMIC_RELAXED), __atomic_load_n(&et[O_TIMEOUT], __ATOMIC_RELAXED),
                 __atomic_load_n(&et[O_DRIVER_ERROR], __ATOMIC_RELAXED) + __atomic_load_n(&et[O_OTHER], __ATOMIC_RELAXED));
}

/* ------------------------------------------------------------------ the retry loop */

typedef struct {
    int kind;
    VkResult (*wait)(void *c, uint64_t timeout); /* the blocking call (the idle waits ignore timeout) */
    VkResult (*poll)(void *c);                   /* the non-blocking one, or NULL */
    void *c;
    uint64_t timeout; /* the app's (UINT64_MAX: none) */
    uint64_t t0;      /* when the app's call began */
    char detail[320]; /* for the first log line */
} waitop;

static VkResult retry_loop(waitop *w, VkResult first);
static VkResult retry(waitop *w, VkResult first)
{
    retrying++;
    VkResult r = retry_loop(w, first);
    retrying--;
    return r;
}

static VkResult retry_loop(waitop *w, VkResult first)
{
    uint64_t start = now_ns(), failed_after = start - w->t0;
    uint64_t deadline = w->timeout == UINT64_MAX || w->timeout > UINT64_MAX - w->t0 ? UINT64_MAX : w->t0 + w->timeout;
    /* an early VK_TIMEOUT keeps waiting, but a wait with no deadline still ends at the 30 s cap as in
     * rc5: no path in the server waits forever (a wait that long is a hung GPU, not a slow one) */
    int early = first == VK_TIMEOUT;
    uint64_t cap = cap_override_ns >= 0 ? (uint64_t)cap_override_ns : deadline == UINT64_MAX ? NO_DEADLINE_CAP : UINT64_MAX;
    uint64_t give_up = cap > UINT64_MAX - start ? UINT64_MAX : start + cap;
    uint64_t next_log = start + PROGRESS_EVERY, nap = MS / 2;
    VkResult r = first, last_bad = first;
    int tries = 0, outcome;
    for (;;) {
        if (w->poll) {
            VkResult p = w->poll(w->c);
            if (p == VK_SUCCESS || (p < 0 && legal(p))) {
                r = p;
                break;
            }
        }
        uint64_t now = now_ns();
        if (now >= deadline) {
            r = VK_TIMEOUT;
            break;
        }
        if (now >= give_up) {
            r = last_bad;
            break;
        }
        if (now >= next_log) {
            VKB_WARN("wait_retry: %s: the driver has failed the wait for %.0f s (%d retries, last %d); retrying until %s",
                     kind_name[w->kind], (now - start) / 1e9, tries, last_bad,
                     give_up != UINT64_MAX ? "the retry cap" : deadline != UINT64_MAX ? "the app's deadline" : "it is done (no timeout)");
            next_log += PROGRESS_EVERY;
        }
        uint64_t end = deadline < give_up ? deadline : give_up;
        tries++;
        uint64_t s = now_ns();
        r = w->wait(w->c, end - now);
        if (legal(r) && r != VK_TIMEOUT) break; /* done, or a legal error: the app's */
        if (r != VK_TIMEOUT) last_bad = r;      /* a VK_TIMEOUT: the deadline or the cap decide at the top */
        if (now_ns() - s < 2 * MS) {            /* failing (or timing out) at once: back off */
            uint64_t nw = now_ns(), left = end > nw ? end - nw : 0;
            nap_ns(nap < left ? nap : left);
            if (nap < 20 * MS) nap *= 2;
        } else
            nap = MS / 2;
    }
    outcome = r == VK_SUCCESS ? O_SUCCESS : r == VK_TIMEOUT ? O_TIMEOUT : !legal(r) ? O_DRIVER_ERROR : O_OTHER;
    if (early)
        count_early(w->kind, outcome, r, failed_after, tries, now_ns() - start, w->timeout, w->detail);
    else
        count(w->kind, outcome, first, r, failed_after, tries, now_ns() - start, w->timeout, w->detail);
    return r;
}

/* ------------------------------------------------------------------ submitted signals (the grace) */

static void sem_state(const vkb_dispatch *nx, VkDevice d, const VkSemaphoreWaitInfo *wi, char *out, size_t cap);

/* VKBRIDGE_WAIT_GRACE_S, read at each install */
static uint64_t grace_ns = GRACE_DEFAULT;

/* The highest value submitted to signal each timeline semaphore: open addressing, keyed by the
 * server's handle (unique in this process across devices), cleared at vkDestroySemaphore. Binary
 * semaphores land here too (their value is ignored); only vkWaitSemaphores, timelines only, reads. */
#define SIG_SLOTS 8192
static struct {
    uint64_t h, v;
} sig[SIG_SLOTS];
static pthread_mutex_t sig_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned sig_used;
static int sig_full_said;

static unsigned sig_hash(uint64_t h)
{
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdull;
    h ^= h >> 33;
    return (unsigned)h & (SIG_SLOTS - 1);
}

/* Under sig_lock: the slot of h, or (add) a free one; -1 if there is none */
static int sig_slot(uint64_t h, int add)
{
    unsigned i = sig_hash(h);
    int tomb = -1;
    for (unsigned n = 0; n < SIG_SLOTS; n++, i = (i + 1) & (SIG_SLOTS - 1)) {
        if (sig[i].h == h) return (int)i;
        if (sig[i].h == 1 && tomb < 0) tomb = (int)i; /* 1: a cleared slot (handles are never 1) */
        if (!sig[i].h) {
            if (!add) return -1;
            if (tomb >= 0) return tomb;
            if (sig_used + 1 >= SIG_SLOTS - SIG_SLOTS / 8) return -1; /* keep probes short */
            sig_used++;
            return (int)i;
        }
    }
    return add ? tomb : -1;
}

static void sig_note(VkSemaphore s, uint64_t v)
{
    uint64_t h = (uint64_t)(uintptr_t)s;
    if (h <= 1) return;
    int i = sig_slot(h, 1);
    if (i < 0) {
        if (!sig_full_said) {
            sig_full_said = 1;
            VKB_INFO("wait_retry: %u semaphores with submitted signals tracked, no room for more: their waits get no grace", sig_used);
        }
        return;
    }
    sig[i].h = h;
    if (v > sig[i].v) sig[i].v = v;
}

/* The highest value submitted to signal s; 0 if none is known */
static uint64_t sig_submitted(VkSemaphore s)
{
    pthread_mutex_lock(&sig_lock);
    int i = sig_slot((uint64_t)(uintptr_t)s, 0);
    uint64_t v = i >= 0 ? sig[i].v : 0;
    pthread_mutex_unlock(&sig_lock);
    return v;
}

static VKAPI_ATTR VkResult VKAPI_CALL w_QueueSubmit(VkQueue q, uint32_t n, const VkSubmitInfo *si, VkFence f)
{
    const vkb_dispatch *nx = vkb_errata_next(0);
    VkResult r = nx->vkQueueSubmit(q, n, si, f);
    if (r != VK_SUCCESS || !grace_ns) return r;
    pthread_mutex_lock(&sig_lock);
    for (uint32_t i = 0; i < n; i++) {
        const VkTimelineSemaphoreSubmitInfo *ts = NULL;
        for (const VkBaseInStructure *p = si[i].pNext; p; p = p->pNext)
            if (p->sType == VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO) ts = (const VkTimelineSemaphoreSubmitInfo *)p;
        if (!ts || !ts->pSignalSemaphoreValues) continue;
        uint32_t m = si[i].signalSemaphoreCount < ts->signalSemaphoreValueCount ? si[i].signalSemaphoreCount : ts->signalSemaphoreValueCount;
        for (uint32_t k = 0; k < m; k++) sig_note(si[i].pSignalSemaphores[k], ts->pSignalSemaphoreValues[k]);
    }
    pthread_mutex_unlock(&sig_lock);
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL w_QueueSubmit2(VkQueue q, uint32_t n, const VkSubmitInfo2 *si, VkFence f)
{
    const vkb_dispatch *nx = vkb_errata_next(0);
    VkResult r = nx->vkQueueSubmit2(q, n, si, f);
    if (r != VK_SUCCESS || !grace_ns) return r;
    pthread_mutex_lock(&sig_lock);
    for (uint32_t i = 0; i < n; i++)
        for (uint32_t k = 0; k < si[i].signalSemaphoreInfoCount; k++)
            sig_note(si[i].pSignalSemaphoreInfos[k].semaphore, si[i].pSignalSemaphoreInfos[k].value);
    pthread_mutex_unlock(&sig_lock);
    return r;
}

static VKAPI_ATTR void VKAPI_CALL w_DestroySemaphore(VkDevice d, VkSemaphore s, const VkAllocationCallbacks *a)
{
    const vkb_dispatch *nx = vkb_errata_next(0);
    if (s) {
        pthread_mutex_lock(&sig_lock);
        int i = sig_slot((uint64_t)(uintptr_t)s, 0);
        if (i >= 0) sig[i].h = 1, sig[i].v = 0;
        pthread_mutex_unlock(&sig_lock);
    }
    nx->vkDestroySemaphore(d, s, a);
}

/* 1: what the wait still waits for has been submitted (all of it, or one for WAIT_ANY) */
static int sems_in_flight(const vkb_dispatch *nx, VkDevice d, const VkSemaphoreWaitInfo *wi)
{
    int any = (wi->flags & VK_SEMAPHORE_WAIT_ANY_BIT) != 0, pending = 0;
    for (uint32_t i = 0; i < wi->semaphoreCount; i++) {
        uint64_t v = 0;
        if (nx->vkGetSemaphoreCounterValue && nx->vkGetSemaphoreCounterValue(d, wi->pSemaphores[i], &v) == VK_SUCCESS &&
            v >= wi->pValues[i])
            continue; /* reached meanwhile: the poll below ends the wait */
        int sub = sig_submitted(wi->pSemaphores[i]) >= wi->pValues[i];
        if (any && sub) return 1;
        if (!any && !sub) return 0;
        pending++;
    }
    return !any || !pending;
}

static unsigned long grace_tally[O_COUNT + 1];

/* A VK_TIMEOUT at the app's deadline (t0 + timeout): wait on if the work is on the GPU */
static VkResult grace_wait(const vkb_dispatch *nx, VkDevice d, const VkSemaphoreWaitInfo *wi, uint64_t timeout, uint64_t t0)
{
    if (!grace_ns || timeout < GRACE_MIN || timeout == UINT64_MAX || !wi || !wi->semaphoreCount) return VK_TIMEOUT;
    if (!sems_in_flight(nx, d, wi)) return VK_TIMEOUT;
    char detail[320] = "";
    if (!__atomic_load_n(&grace_tally[O_COUNT], __ATOMIC_RELAXED)) sem_state(nx, d, wi, detail, sizeof(detail));
    uint64_t start = now_ns(), end = start + grace_ns, nap = MS / 2;
    VkResult r = VK_TIMEOUT;
    int tries = 0;
    retrying++;
    for (;;) {
        VkResult p = nx->vkWaitSemaphores(d, wi, 0);
        if (p == VK_SUCCESS || (p < 0 && legal(p))) {
            r = p;
            break;
        }
        uint64_t now = now_ns();
        if (now >= end) {
            r = VK_TIMEOUT;
            break;
        }
        tries++;
        uint64_t s = now_ns();
        r = nx->vkWaitSemaphores(d, wi, end - now);
        if (r == VK_SUCCESS || (r < 0 && legal(r))) break;
        if (now_ns() - s < 2 * MS) { /* -3 or an early timeout at once: back off */
            nap_ns(nap);
            if (nap < 20 * MS) nap *= 2;
        } else
            nap = MS / 2;
    }
    retrying--;
    int outcome = r == VK_SUCCESS ? O_SUCCESS : r == VK_TIMEOUT ? O_TIMEOUT : O_OTHER;
    __atomic_add_fetch(&grace_tally[outcome], 1, __ATOMIC_RELAXED);
    unsigned long n = __atomic_add_fetch(&grace_tally[O_COUNT], 1, __ATOMIC_RELAXED);
    double total = (now_ns() - t0) / 1e9;
    if (n == 1 || outcome != O_SUCCESS)
        VKB_WARN("wait_retry: vkWaitSemaphores: the app's deadline (timeout %.3f s) passed with the work submitted and not done%s; "
                 "waited on past it (%d more wait(s)): the app gets %d%s after %.3f s in all%s",
                 timeout / 1e9, detail, tries, r, r == VK_SUCCESS ? " (VK_SUCCESS)" : r == VK_TIMEOUT ? " (VK_TIMEOUT)" : "", total,
                 outcome == O_TIMEOUT ? ": still not done at the end of VKBRIDGE_WAIT_GRACE_S" : "");
    unsigned long p = 10;
    while (p < n && p < 1000000000ul) p *= 10;
    if (n >= 10 && n == p)
        VKB_INFO("wait_retry: vkWaitSemaphores: %lu wait(s) past the app's deadline with the work submitted so far: %lu ended in "
                 "VK_SUCCESS, %lu in VK_TIMEOUT, %lu otherwise", n, __atomic_load_n(&grace_tally[O_SUCCESS], __ATOMIC_RELAXED),
                 __atomic_load_n(&grace_tally[O_TIMEOUT], __ATOMIC_RELAXED), __atomic_load_n(&grace_tally[O_OTHER], __ATOMIC_RELAXED));
    return r;
}

/* ------------------------------------------------------------------ the fix */

typedef struct {
    const vkb_dispatch *nx;
    VkDevice d;
    uint32_t n;
    const VkFence *f;
    VkBool32 all;
} fence_ctx;
static VkResult fence_wait(void *c, uint64_t timeout)
{
    fence_ctx *x = c;
    return x->nx->vkWaitForFences(x->d, x->n, x->f, x->all, timeout);
}
static VkResult fence_poll(void *c)
{
    return fence_wait(c, 0);
}

static VKAPI_ATTR VkResult VKAPI_CALL w_WaitForFences(VkDevice d, uint32_t n, const VkFence *f, VkBool32 all, uint64_t timeout)
{
    const vkb_dispatch *nx = vkb_errata_next(0);
    uint64_t t0 = now_ns();
    VkResult r = nx->vkWaitForFences(d, n, f, all, timeout);
    if (legal(r) && !early_timeout(r, t0, timeout)) return r;
    fence_ctx x = {nx, d, n, f, all};
    waitop w = {K_FENCE, fence_wait, fence_poll, &x, timeout, t0, ""};
    snprintf(w.detail, sizeof(w.detail), ", %u fence(s), %s", n, all ? "all" : "any");
    return retry(&w, r);
}

typedef struct {
    const vkb_dispatch *nx;
    VkDevice d;
    const VkSemaphoreWaitInfo *wi;
} sem_ctx;
static VkResult sem_wait(void *c, uint64_t timeout)
{
    sem_ctx *x = c;
    return x->nx->vkWaitSemaphores(x->d, x->wi, timeout);
}
static VkResult sem_poll(void *c)
{
    return sem_wait(c, 0);
}

/* Where the first few semaphores stand (the first log line of a kind only) */
static void sem_state(const vkb_dispatch *nx, VkDevice d, const VkSemaphoreWaitInfo *wi, char *out, size_t cap)
{
    size_t o = (size_t)snprintf(out, cap, ", %u semaphore(s), %s:", wi ? wi->semaphoreCount : 0,
                                wi && (wi->flags & VK_SEMAPHORE_WAIT_ANY_BIT) ? "any" : "all");
    for (uint32_t i = 0; wi && i < wi->semaphoreCount && i < 4 && o + 64 < cap; i++) {
        uint64_t v = 0;
        VkResult r = nx->vkGetSemaphoreCounterValue ? nx->vkGetSemaphoreCounterValue(d, wi->pSemaphores[i], &v) : VK_ERROR_UNKNOWN;
        if (r == VK_SUCCESS)
            o += (size_t)snprintf(out + o, cap - o, " [%p wants %llu, at %llu]", (void *)wi->pSemaphores[i],
                                  (unsigned long long)wi->pValues[i], (unsigned long long)v);
        else
            o += (size_t)snprintf(out + o, cap - o, " [%p wants %llu, value unreadable (%d)]", (void *)wi->pSemaphores[i],
                                  (unsigned long long)wi->pValues[i], r);
    }
}

static VKAPI_ATTR VkResult VKAPI_CALL w_WaitSemaphores(VkDevice d, const VkSemaphoreWaitInfo *wi, uint64_t timeout)
{
    const vkb_dispatch *nx = vkb_errata_next(0);
    uint64_t t0 = now_ns();
    VkResult r = nx->vkWaitSemaphores(d, wi, timeout);
    if (!legal(r) || early_timeout(r, t0, timeout)) {
        sem_ctx x = {nx, d, wi};
        waitop w = {K_SEM, sem_wait, sem_poll, &x, timeout, t0, ""};
        if (!__atomic_load_n(r == VK_TIMEOUT ? &early_tally[EARLY_IX(K_SEM, timeout)][O_COUNT] : &tally[K_SEM][O_COUNT], __ATOMIC_RELAXED))
            sem_state(nx, d, wi, w.detail, sizeof(w.detail));
        r = retry(&w, r);
    }
    return r == VK_TIMEOUT ? grace_wait(nx, d, wi, timeout, t0) : r;
}

typedef struct {
    const vkb_dispatch *nx;
    VkQueue q;
    VkDevice d;
} idle_ctx;
static VkResult queue_idle(void *c, uint64_t timeout)
{
    idle_ctx *x = c;
    return x->nx->vkQueueWaitIdle(x->q);
}
static VkResult device_idle(void *c, uint64_t timeout)
{
    idle_ctx *x = c;
    return x->nx->vkDeviceWaitIdle(x->d);
}

static VKAPI_ATTR VkResult VKAPI_CALL w_QueueWaitIdle(VkQueue q)
{
    const vkb_dispatch *nx = vkb_errata_next(0);
    uint64_t t0 = now_ns();
    VkResult r = nx->vkQueueWaitIdle(q);
    if (legal(r)) return r;
    idle_ctx x = {nx, q, VK_NULL_HANDLE};
    waitop w = {K_QUEUE_IDLE, queue_idle, NULL, &x, UINT64_MAX, t0, ""};
    return retry(&w, r);
}

static VKAPI_ATTR VkResult VKAPI_CALL w_DeviceWaitIdle(VkDevice d)
{
    const vkb_dispatch *nx = vkb_errata_next(0);
    uint64_t t0 = now_ns();
    VkResult r = nx->vkDeviceWaitIdle(d);
    if (legal(r)) return r;
    idle_ctx x = {nx, VK_NULL_HANDLE, d};
    waitop w = {K_DEVICE_IDLE, device_idle, NULL, &x, UINT64_MAX, t0, ""};
    return retry(&w, r);
}

typedef struct {
    const vkb_dispatch *nx;
    VkDevice d;
    VkQueryPool pool;
    uint32_t first, count;
    size_t size;
    void *data;
    VkDeviceSize stride;
    VkQueryResultFlags flags;
} query_ctx;
static VkResult query_wait(void *c, uint64_t timeout)
{
    query_ctx *x = c;
    return x->nx->vkGetQueryPoolResults(x->d, x->pool, x->first, x->count, x->size, x->data, x->stride, x->flags);
}
/* Without WAIT_BIT (nor PARTIAL_BIT): VK_SUCCESS once every query is available, all written */
static VkResult query_poll(void *c)
{
    query_ctx *x = c;
    return x->nx->vkGetQueryPoolResults(x->d, x->pool, x->first, x->count, x->size, x->data, x->stride,
                                        x->flags & ~(VkQueryResultFlags)(VK_QUERY_RESULT_WAIT_BIT | VK_QUERY_RESULT_PARTIAL_BIT));
}

static VKAPI_ATTR VkResult VKAPI_CALL w_GetQueryPoolResults(VkDevice d, VkQueryPool pool, uint32_t first, uint32_t count, size_t size,
                                                            void *data, VkDeviceSize stride, VkQueryResultFlags flags)
{
    const vkb_dispatch *nx = vkb_errata_next(0);
    uint64_t t0 = now_ns();
    VkResult r = nx->vkGetQueryPoolResults(d, pool, first, count, size, data, stride, flags);
    if (legal(r) || !(flags & VK_QUERY_RESULT_WAIT_BIT)) return r;
    query_ctx x = {nx, d, pool, first, count, size, data, stride, flags};
    waitop w = {K_QUERY, query_wait, query_poll, &x, UINT64_MAX, t0, ""};
    snprintf(w.detail, sizeof(w.detail), ", %u quer%s", count, count == 1 ? "y" : "ies");
    return retry(&w, r);
}

static VKAPI_ATTR VkResult VKAPI_CALL w_GetFenceStatus(VkDevice d, VkFence f)
{
    const vkb_dispatch *nx = vkb_errata_next(0);
    uint64_t t0 = now_ns();
    VkResult r = nx->vkGetFenceStatus(d, f);
    if (legal(r)) return r;
    VkResult first = r;
    int tries = 0;
    retrying++;
    while (tries < 3 && !legal(r)) {
        nap_ns(MS);
        tries++;
        r = nx->vkGetFenceStatus(d, f);
    }
    retrying--;
    VkResult out = legal(r) ? r : VK_NOT_READY;
    count(K_STATUS, out == VK_SUCCESS ? O_SUCCESS : O_OTHER, first, out, 0, tries, now_ns() - t0, 0,
          legal(r) ? NULL : ", still failing: answered VK_NOT_READY");
    return out;
}

void vkb_wait_retry_install(vkb_srv_table *t, vkb_errata_dev *e)
{
    (void)e;
    const char *cap = vkb_srv_env("VKBRIDGE_WAIT_RETRY_MS");
    cap_override_ns = cap && *cap ? (int64_t)(atof(cap) > 0 ? atof(cap) * 1e6 : 0) : -1;
    ERRATA_HOOK(t, vkWaitForFences, w_WaitForFences);
    ERRATA_HOOK(t, vkWaitSemaphores, w_WaitSemaphores);
    ERRATA_HOOK(t, vkQueueWaitIdle, w_QueueWaitIdle);
    ERRATA_HOOK(t, vkDeviceWaitIdle, w_DeviceWaitIdle);
    ERRATA_HOOK(t, vkGetQueryPoolResults, w_GetQueryPoolResults);
    ERRATA_HOOK(t, vkGetFenceStatus, w_GetFenceStatus);
    const char *g = vkb_srv_env("VKBRIDGE_WAIT_GRACE_S");
    grace_ns = g && *g ? (uint64_t)(atof(g) > 0 ? atof(g) * 1e9 : 0) : GRACE_DEFAULT;
    if (grace_ns) {
        ERRATA_HOOK(t, vkQueueSubmit, w_QueueSubmit);
        ERRATA_HOOK(t, vkQueueSubmit2, w_QueueSubmit2);
        ERRATA_HOOK(t, vkDestroySemaphore, w_DestroySemaphore);
    }
    if (g && *g) VKB_INFO("wait_retry: semaphore waits past their deadline with the work submitted: %s (VKBRIDGE_WAIT_GRACE_S=%s)",
                          grace_ns ? "waited on" : "not waited on", g);
    if (cap_override_ns >= 0) VKB_INFO("wait_retry: retrying for at most %s ms (VKBRIDGE_WAIT_RETRY_MS)", cap);
}

/* ------------------------------------------------------------------ the imitation (host tests) */

enum { HOW_ONCE, HOW_ALT, HOW_ALL, HOW_FIRST };
static int imi_how;
static int imi_timeout; /* VKBRIDGE_WAIT_FAKE_RESULT=timeout: an early VK_TIMEOUT, not -3 */
static unsigned imi_kinds;
static long imi_left[K_COUNT];
static __thread unsigned imi_flip[K_COUNT];

/* 1: this blocking call is failed */
static int imi_take(int kind)
{
    /* the startup probe's wait_eintr: its fence and semaphore waits fail, the app's counts untouched */
    vkb_errata_dev *pe = vkb_errata_cur();
    if (pe && pe->probe) return kind == K_FENCE || kind == K_SEM;
    if (!(imi_kinds >> kind & 1)) return 0;
    if (imi_how == HOW_ALL) return 1;
    if (imi_how == HOW_ONCE) return !retrying;
    if (imi_how == HOW_ALT) return !(imi_flip[kind]++ & 1);
    return __atomic_load_n(&imi_left[kind], __ATOMIC_RELAXED) > 0 && __atomic_sub_fetch(&imi_left[kind], 1, __ATOMIC_RELAXED) >= 0;
}

#define SLICE (20 * MS) /* the signal comes within 20 ms of the driver's wait */

/* An early VK_TIMEOUT in place of -3 (never in the probe's wait_eintr) */
static int imi_early(void)
{
    vkb_errata_dev *pe = vkb_errata_cur();
    return imi_timeout && !(pe && pe->probe);
}

static VKAPI_ATTR VkResult VKAPI_CALL imi_WaitForFences(VkDevice d, uint32_t n, const VkFence *f, VkBool32 all, uint64_t timeout)
{
    const vkb_dispatch *nx = vkb_errata_next(1);
    if (!timeout || (imi_early() && timeout <= SLICE) || !imi_take(K_FENCE)) return nx->vkWaitForFences(d, n, f, all, timeout);
    VkResult r = nx->vkWaitForFences(d, n, f, all, timeout < SLICE ? timeout : SLICE);
    return !imi_early() ? VK_ERROR_INITIALIZATION_FAILED : r;
}

static VKAPI_ATTR VkResult VKAPI_CALL imi_WaitSemaphores(VkDevice d, const VkSemaphoreWaitInfo *wi, uint64_t timeout)
{
    const vkb_dispatch *nx = vkb_errata_next(1);
    if (!timeout || (imi_early() && timeout <= SLICE) || !imi_take(K_SEM)) return nx->vkWaitSemaphores(d, wi, timeout);
    VkResult r = nx->vkWaitSemaphores(d, wi, timeout < SLICE ? timeout : SLICE);
    return !imi_early() ? VK_ERROR_INITIALIZATION_FAILED : r;
}

static VKAPI_ATTR VkResult VKAPI_CALL imi_QueueWaitIdle(VkQueue q)
{
    const vkb_dispatch *nx = vkb_errata_next(1);
    VkResult r = nx->vkQueueWaitIdle(q);
    return r == VK_SUCCESS && imi_take(K_QUEUE_IDLE) ? VK_ERROR_INITIALIZATION_FAILED : r;
}

static VKAPI_ATTR VkResult VKAPI_CALL imi_DeviceWaitIdle(VkDevice d)
{
    const vkb_dispatch *nx = vkb_errata_next(1);
    VkResult r = nx->vkDeviceWaitIdle(d);
    return r == VK_SUCCESS && imi_take(K_DEVICE_IDLE) ? VK_ERROR_INITIALIZATION_FAILED : r;
}

static VKAPI_ATTR VkResult VKAPI_CALL imi_GetQueryPoolResults(VkDevice d, VkQueryPool pool, uint32_t first, uint32_t count, size_t size,
                                                              void *data, VkDeviceSize stride, VkQueryResultFlags flags)
{
    const vkb_dispatch *nx = vkb_errata_next(1);
    VkResult r = nx->vkGetQueryPoolResults(d, pool, first, count, size, data, stride, flags);
    return r == VK_SUCCESS && (flags & VK_QUERY_RESULT_WAIT_BIT) && imi_take(K_QUERY) ? VK_ERROR_INITIALIZATION_FAILED : r;
}

static VKAPI_ATTR VkResult VKAPI_CALL imi_GetFenceStatus(VkDevice d, VkFence f)
{
    const vkb_dispatch *nx = vkb_errata_next(1);
    return imi_take(K_STATUS) ? VK_ERROR_INITIALIZATION_FAILED : nx->vkGetFenceStatus(d, f);
}

void vkb_wait_retry_imitate(vkb_srv_table *t, vkb_errata_dev *e)
{
    if (e->probe) { /* the probe's own waits only (imi_take): nothing here changes */
        ERRATA_HOOK(t, vkWaitForFences, imi_WaitForFences);
        ERRATA_HOOK(t, vkWaitSemaphores, imi_WaitSemaphores);
        return;
    }
    const char *v = vkb_srv_env("VKBRIDGE_WAIT_FAKE_ERROR");
    if (!v) v = "";
    long n = 0;
    imi_how = !*v || *v == ':' || !strncmp(v, "once", 4) ? HOW_ONCE : !strncmp(v, "alt", 3) ? HOW_ALT : !strncmp(v, "all", 3) ? HOW_ALL
              : (n = atol(v), HOW_FIRST);
    const char *k = strchr(v, ':');
    imi_kinds = 0;
    static const char *const names[] = {"fence", "sem", "idle", "query", "status"};
    static const unsigned bits[] = {1u << K_FENCE, 1u << K_SEM, 1u << K_QUEUE_IDLE | 1u << K_DEVICE_IDLE, 1u << K_QUERY, 1u << K_STATUS};
    for (const char *c = k ? k + 1 : NULL; c && *c;) {
        size_t len = strcspn(c, "+, ");
        int found = 0;
        for (int i = 0; i < 5; i++)
            if (strlen(names[i]) == len && !strncmp(c, names[i], len)) imi_kinds |= bits[i], found = 1;
        if (!found && len) VKB_WARN("wait_retry: VKBRIDGE_WAIT_FAKE_ERROR: no wait kind %.*s (fence, sem, idle, query, status)", (int)len, c);
        c += len;
        while (*c == '+' || *c == ',' || *c == ' ') c++;
    }
    if (!k) imi_kinds = bits[0] | bits[1] | bits[2] | bits[3];
    const char *res = vkb_srv_env("VKBRIDGE_WAIT_FAKE_RESULT");
    imi_timeout = res && !strcmp(res, "timeout");
    for (int i = 0; i < K_COUNT; i++) __atomic_store_n(&imi_left[i], n, __ATOMIC_RELAXED);
    ERRATA_HOOK(t, vkWaitForFences, imi_WaitForFences);
    ERRATA_HOOK(t, vkWaitSemaphores, imi_WaitSemaphores);
    ERRATA_HOOK(t, vkQueueWaitIdle, imi_QueueWaitIdle);
    ERRATA_HOOK(t, vkDeviceWaitIdle, imi_DeviceWaitIdle);
    ERRATA_HOOK(t, vkGetQueryPoolResults, imi_GetQueryPoolResults);
    ERRATA_HOOK(t, vkGetFenceStatus, imi_GetFenceStatus);
    char ks[96] = "";
    for (int i = 0; i < 5; i++)
        if ((imi_kinds & bits[i]) == bits[i]) snprintf(ks + strlen(ks), sizeof(ks) - strlen(ks), "%s%s", *ks ? "," : "", names[i]);
    const char *how = imi_timeout ? "-3, fence and semaphore waits with an early VK_TIMEOUT" : "-3";
    if (imi_how == HOW_FIRST)
        VKB_INFO("wait_retry: device %u: the first %ld blocking call(s) of each kind (%s) fail with %s", t->id, n, ks, how);
    else
        VKB_INFO("wait_retry: device %u: %s (%s) fails with %s", t->id,
                 imi_how == HOW_ONCE ? "every blocking wait, once" : imi_how == HOW_ALL ? "every blocking call" : "every other blocking call", ks, how);
}
