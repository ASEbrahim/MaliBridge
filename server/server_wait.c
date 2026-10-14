/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Waits that the driver ends with an error it may not return. vkWaitSemaphores and
 * vkWaitForFences may answer VK_SUCCESS, VK_TIMEOUT, VK_ERROR_DEVICE_LOST or out-of-memory; the
 * Mali-G925 driver (Android 16, 2026-10-14, The Witcher 3 through vkd3d-proton) answered
 * VK_ERROR_INITIALIZATION_FAILED (-3) every 7-10 s while the GPU stalled, on two semaphores at
 * once. The bridge passes both calls straight through (the client maps a lost server to
 * VK_ERROR_DEVICE_LOST, never to -3), so the error is the driver's.
 *
 * Here such an error is looked at before the app sees it: the wait is polled (timeout 0); if the
 * semaphores or fences are in fact signalled the app gets VK_SUCCESS. Otherwise the wait is tried
 * again, for at most VKBRIDGE_WAIT_RETRY_MS (default 2000, and never past the app's own timeout),
 * and if the driver keeps failing the app gets the driver's error as before. Not VK_TIMEOUT:
 * vkd3d-proton takes a timeout (and VK_ERROR_DEVICE_LOST) as a lost device and removes it, while
 * it survives other errors (it logs them and retires the submission). Each case is logged with
 * how long the driver waited before failing and where the semaphores stood - the evidence for
 * telling a driver watchdog (seconds) from a spurious failure (immediate).
 * VKBRIDGE_WAIT_FIX=0 passes everything through untouched.
 */
#define _GNU_SOURCE
#include "vkb_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

static void nap(void)
{
    struct timespec ts = {0, 1000000};
    nanosleep(&ts, NULL);
}

static int passthrough(VkResult r)
{
    return r >= 0 || r == VK_ERROR_DEVICE_LOST || r == VK_ERROR_OUT_OF_HOST_MEMORY || r == VK_ERROR_OUT_OF_DEVICE_MEMORY;
}

static double retry_budget(void)
{
    static double v = -1;
    if (v < 0) {
        const char *e = vkb_srv_env("VKBRIDGE_WAIT_RETRY_MS");
        v = e && *e ? atof(e) / 1000.0 : 2.0;
        if (v < 0) v = 0;
    }
    return v;
}

/* The first 16 cases, then every 64th: a stalling game hits this once a frame. */
static int want_log(void)
{
    static unsigned n;
    unsigned k = __atomic_fetch_add(&n, 1, __ATOMIC_RELAXED);
    return k < 16 || k % 64 == 0;
}

static void sem_state(vkb_srv_table *t, VkDevice d, const VkSemaphoreWaitInfo *wi, char *out, size_t cap)
{
    size_t o = 0;
    out[0] = 0;
    for (uint32_t i = 0; wi && i < wi->semaphoreCount && i < 4 && o + 64 < cap; i++) {
        uint64_t v = 0;
        VkResult r = t->real.vkGetSemaphoreCounterValue ? t->real.vkGetSemaphoreCounterValue(d, wi->pSemaphores[i], &v) : VK_ERROR_UNKNOWN;
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
    vkb_srv_table *t = vkb_srv_current_table();
    double t0 = now_s();
    VkResult r = t->real.vkWaitSemaphores(d, wi, timeout);
    if (passthrough(r)) return r;
    double failed_after = now_s() - t0;
    VkResult first = r;
    char st[320];
    int log = want_log();
    if (log) sem_state(t, d, wi, st, sizeof(st));
    double deadline = timeout == UINT64_MAX ? 1e300 : t0 + timeout / 1e9;
    double stop = now_s() + retry_budget();
    if (stop > deadline) stop = deadline;
    int tries = 0;
    for (;;) {
        r = t->real.vkWaitSemaphores(d, wi, 0);
        if (r == VK_SUCCESS || r == VK_ERROR_DEVICE_LOST) break;
        double left = stop - now_s();
        if (left <= 0) break;
        tries++;
        r = t->real.vkWaitSemaphores(d, wi, (uint64_t)(left * 1e9));
        if (r == VK_SUCCESS || r == VK_ERROR_DEVICE_LOST) break;
        if (r == VK_TIMEOUT) break; /* the retry budget ran out, not the app's timeout */
        nap();                      /* a driver failing at once is not spun on */
    }
    VkResult out = r == VK_SUCCESS || r == VK_ERROR_DEVICE_LOST ? r : first;
    if (log) {
        char st2[320];
        sem_state(t, d, wi, st2, sizeof(st2));
        VKB_WARN("vkWaitSemaphores: the driver failed the wait (%d) after %.3f s (timeout %s, %u semaphore(s), %s):%s; "
                 "after %d retry(ies) in %.3f s: %d%s, app gets %d", first, failed_after,
                 timeout == UINT64_MAX ? "infinite" : "finite", wi ? wi->semaphoreCount : 0,
                 wi && (wi->flags & VK_SEMAPHORE_WAIT_ANY_BIT) ? "any" : "all", st, tries, now_s() - t0 - failed_after, r, st2, out);
    }
    return out;
}

static VKAPI_ATTR VkResult VKAPI_CALL w_WaitForFences(VkDevice d, uint32_t n, const VkFence *f, VkBool32 all, uint64_t timeout)
{
    vkb_srv_table *t = vkb_srv_current_table();
    double t0 = now_s();
    VkResult r = t->real.vkWaitForFences(d, n, f, all, timeout);
    if (passthrough(r)) return r;
    double failed_after = now_s() - t0;
    VkResult first = r;
    double deadline = timeout == UINT64_MAX ? 1e300 : t0 + timeout / 1e9;
    double stop = now_s() + retry_budget();
    if (stop > deadline) stop = deadline;
    int tries = 0;
    for (;;) {
        r = t->real.vkWaitForFences(d, n, f, all, 0);
        if (r == VK_SUCCESS || r == VK_ERROR_DEVICE_LOST) break;
        double left = stop - now_s();
        if (left <= 0) break;
        tries++;
        r = t->real.vkWaitForFences(d, n, f, all, (uint64_t)(left * 1e9));
        if (r == VK_SUCCESS || r == VK_ERROR_DEVICE_LOST || r == VK_TIMEOUT) break;
        nap();
    }
    VkResult out = r == VK_SUCCESS || r == VK_ERROR_DEVICE_LOST ? r : first;
    if (want_log())
        VKB_WARN("vkWaitForFences: the driver failed the wait (%d) after %.3f s (%u fence(s), timeout %s); after %d retry(ies): %d, app gets %d",
                 first, failed_after, n, timeout == UINT64_MAX ? "infinite" : "finite", tries, r, out);
    return out;
}

/* Test hook (host): VKBRIDGE_WAIT_FAKE_ERROR=<n> makes the first n waits on each device fail
 * with VK_ERROR_INITIALIZATION_FAILED after the driver's own wait, as the G925 did. */
static int fake_left = -1;
static PFN_vkWaitSemaphores fake_next; /* the driver's, the same for every device */
static VKAPI_ATTR VkResult VKAPI_CALL fake_WaitSemaphores(VkDevice d, const VkSemaphoreWaitInfo *wi, uint64_t timeout)
{
    VkResult r = fake_next(d, wi, timeout);
    if (timeout && __atomic_load_n(&fake_left, __ATOMIC_RELAXED) > 0 && __atomic_sub_fetch(&fake_left, 1, __ATOMIC_RELAXED) >= 0)
        return VK_ERROR_INITIALIZATION_FAILED;
    return r;
}

void vkb_wait_device_init(vkb_srv_table *t)
{
    const char *fk = vkb_srv_env("VKBRIDGE_WAIT_FAKE_ERROR");
    if (fk && atoi(fk) > 0 && t->real.vkWaitSemaphores) {
        __atomic_store_n(&fake_left, atoi(fk), __ATOMIC_RELAXED);
        /* Beneath the fix: its own calls see the fake too (and the app's, with the fix off). */
        if (t->real.vkWaitSemaphores != fake_WaitSemaphores) fake_next = t->real.vkWaitSemaphores;
        if (t->dt.vkWaitSemaphores == fake_next) t->dt.vkWaitSemaphores = fake_WaitSemaphores;
        t->real.vkWaitSemaphores = fake_WaitSemaphores;
        VKB_INFO("device %u: the next %s semaphore wait(s) fail with -3 (VKBRIDGE_WAIT_FAKE_ERROR)", t->id, fk);
    }
    const char *off = vkb_srv_env("VKBRIDGE_WAIT_FIX");
    if (off && !strcmp(off, "0")) return;
    if (t->real.vkWaitSemaphores && t->dt.vkWaitSemaphores) t->dt.vkWaitSemaphores = w_WaitSemaphores;
    if (t->real.vkWaitForFences && t->dt.vkWaitForFences) t->dt.vkWaitForFences = w_WaitForFences;
}
