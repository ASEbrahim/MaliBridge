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
 * The first case of each kind is logged in full, then a tally at 10, 100, 1000, ... cases; a wait
 * whose driver error reaches the app is always logged.
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
 * Other non-blocking calls (timeout 0, a query read without WAIT_BIT) are never failed.
 */
#define _GNU_SOURCE
#include "server_errata.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MS 1000000ull
#define SEC 1000000000ull
#define NO_DEADLINE_CAP (30 * SEC)
#define PROGRESS_EVERY (10 * SEC)

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
                     give_up == UINT64_MAX ? "the app's deadline" : "the retry cap");
            next_log += PROGRESS_EVERY;
        }
        uint64_t end = deadline < give_up ? deadline : give_up;
        tries++;
        uint64_t s = now_ns();
        r = w->wait(w->c, end - now);
        if (legal(r) && r != VK_TIMEOUT) break; /* done, or a legal error: the app's */
        if (r == VK_TIMEOUT) continue;          /* the deadline or the cap: decided at the top */
        last_bad = r;
        if (now_ns() - s < 2 * MS) { /* failing at once: back off */
            uint64_t nw = now_ns(), left = end > nw ? end - nw : 0;
            nap_ns(nap < left ? nap : left);
            if (nap < 20 * MS) nap *= 2;
        } else
            nap = MS / 2;
    }
    outcome = r == VK_SUCCESS ? O_SUCCESS : r == VK_TIMEOUT ? O_TIMEOUT : !legal(r) ? O_DRIVER_ERROR : O_OTHER;
    count(w->kind, outcome, first, r, failed_after, tries, now_ns() - start, w->timeout, w->detail);
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
    if (legal(r)) return r;
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
    if (legal(r)) return r;
    sem_ctx x = {nx, d, wi};
    waitop w = {K_SEM, sem_wait, sem_poll, &x, timeout, t0, ""};
    if (!__atomic_load_n(&tally[K_SEM][O_COUNT], __ATOMIC_RELAXED)) sem_state(nx, d, wi, w.detail, sizeof(w.detail));
    return retry(&w, r);
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
    if (cap_override_ns >= 0) VKB_INFO("wait_retry: retrying for at most %s ms (VKBRIDGE_WAIT_RETRY_MS)", cap);
}

/* ------------------------------------------------------------------ the imitation (host tests) */

enum { HOW_ONCE, HOW_ALT, HOW_ALL, HOW_FIRST };
static int imi_how;
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

static VKAPI_ATTR VkResult VKAPI_CALL imi_WaitForFences(VkDevice d, uint32_t n, const VkFence *f, VkBool32 all, uint64_t timeout)
{
    const vkb_dispatch *nx = vkb_errata_next(1);
    if (!timeout || !imi_take(K_FENCE)) return nx->vkWaitForFences(d, n, f, all, timeout);
    nx->vkWaitForFences(d, n, f, all, timeout < SLICE ? timeout : SLICE);
    return VK_ERROR_INITIALIZATION_FAILED;
}

static VKAPI_ATTR VkResult VKAPI_CALL imi_WaitSemaphores(VkDevice d, const VkSemaphoreWaitInfo *wi, uint64_t timeout)
{
    const vkb_dispatch *nx = vkb_errata_next(1);
    if (!timeout || !imi_take(K_SEM)) return nx->vkWaitSemaphores(d, wi, timeout);
    nx->vkWaitSemaphores(d, wi, timeout < SLICE ? timeout : SLICE);
    return VK_ERROR_INITIALIZATION_FAILED;
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
    if (imi_how == HOW_FIRST)
        VKB_INFO("wait_retry: device %u: the first %ld blocking call(s) of each kind (%s) fail with -3", t->id, n, ks);
    else
        VKB_INFO("wait_retry: device %u: %s (%s) fails with -3", t->id,
                 imi_how == HOW_ONCE ? "every blocking wait, once" : imi_how == HOW_ALL ? "every blocking call" : "every other blocking call", ks);
}
