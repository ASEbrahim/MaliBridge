/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * A bound on how many pipeline compiles are inside the driver at once, across the whole server.
 *
 * Each app thread that creates a pipeline compiles it in the driver on its own server thread, so a
 * game that compiles on many threads at once - Unreal Engine's "compiling shaders" screen and its
 * PSO precaching (vkd3d-proton and DXVK hand the work to as many threads as there are cores), a
 * Fossilize replay - has as many Mali compilers running side by side. Each holds its own working
 * memory (tens to hundreds of MB for a large shader) and keeps a core at full clock. On a phone the
 * game already holds most of RAM, and a player reported the whole phone going down during such a
 * screen (Xiaomi 17T Pro, an Unreal game, 2026-10-28). The gate lets VKBRIDGE_COMPILE_THREADS
 * compiles in at a time (default: half the cores, at least 2, at most 4); while MemAvailable is
 * under an eighth of RAM (the server's low-memory line, server_memory.c) only one. The others wait
 * before reaching the driver, holding nothing, and the compile watchdog (server_guard.c) times only
 * the compile itself. A create that only asks the driver's cache
 * (VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT) is not held back.
 *
 * VKBRIDGE_COMPILE_THREADS=0 turns the gate off (as before: no bound). The first wait and the first
 * low-memory narrowing are logged once each; the totals go into the session report (server_report.c).
 */
#define _GNU_SOURCE
#include "vkb_server.h"

#include <pthread.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#define GATE_MAX 32
#define MEM_SAMPLE_S 0.25

static pthread_mutex_t gate_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_cond = PTHREAD_COND_INITIALIZER;
static int gate_limit = -1; /* -1: not read yet; 0: off */
static int gate_inside, gate_peak;
static int gate_tight;      /* low memory: one at a time */
static double gate_sampled; /* last MemAvailable sample (monotonic s) */
static uint64_t gate_waits, gate_tight_waits;
static double gate_wait_s, gate_wait_max_s;
static __thread int tls_depth; /* a compile inside a compile (a layer's helper pipeline) passes */

static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

int vkb_compile_gate_default(int cores)
{
    int n = cores / 2;
    if (n < 2) n = 2;
    if (n > 4) n = 4;
    return n;
}

static void gate_env(void)
{
    const char *e = vkb_srv_env("VKBRIDGE_COMPILE_THREADS");
    long cores = sysconf(_SC_NPROCESSORS_ONLN);
    int n = vkb_compile_gate_default(cores > 0 ? (int)cores : 4);
    if (e && *e) {
        char *end = NULL;
        long v = strtol(e, &end, 10);
        if (end != e && v >= 0) n = v > GATE_MAX ? GATE_MAX : (int)v;
    }
    gate_limit = n;
    if (n) VKB_INFO("compile gate: at most %d pipeline compiles in the driver at once (1 while memory is low; VKBRIDGE_COMPILE_THREADS)", n);
}

/* Under the lock: is memory low now? MemAvailable is read at most every MEM_SAMPLE_S. */
static int tight_now(double t)
{
    if (t - gate_sampled < MEM_SAMPLE_S) return gate_tight;
    gate_sampled = t;
    uint64_t avail = vkb_memavail_kb(), total = vkb_memtotal_mb() << 10;
    int tight = gate_tight;
    if (avail && total) {
        if (!tight && avail < total / 8) tight = 1;
        else if (tight && avail > total / 6) tight = 0;
    } else {
        tight = vkb_mem_low();
    }
    if (tight && !gate_tight)
        VKB_ONCE(VKB_LOG_WARN, "compile gate: memory is low (%llu MB free of %llu MB): one pipeline compile at a time until it recovers (logged once)",
                 (unsigned long long)(avail >> 10), (unsigned long long)(total >> 10));
    gate_tight = tight;
    return tight;
}

int vkb_compile_gate_enter(VkPipelineCreateFlags flags)
{
    if (tls_depth++ > 0) return 0;
    if (flags & VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT) return 0;
    pthread_mutex_lock(&gate_lock);
    if (gate_limit < 0) gate_env();
    if (gate_limit == 0) {
        pthread_mutex_unlock(&gate_lock);
        return 0;
    }
    double t0 = now_s(), t = t0;
    int waited = 0;
    for (;;) {
        int limit = tight_now(t) ? 1 : gate_limit;
        if (gate_inside < limit) break;
        if (!waited) {
            waited = 1;
            gate_waits++;
            if (gate_tight) gate_tight_waits++;
            VKB_ONCE(VKB_LOG_INFO, "compile gate: a pipeline compile waits for a free slot (%d in the driver; logged once)", gate_inside);
        }
        /* wake now and then to look at memory again: a slot can also open by memory recovering */
        struct timespec dl;
        clock_gettime(CLOCK_REALTIME, &dl);
        dl.tv_nsec += 100 * 1000 * 1000;
        if (dl.tv_nsec >= 1000000000) {
            dl.tv_sec++;
            dl.tv_nsec -= 1000000000;
        }
        pthread_cond_timedwait(&gate_cond, &gate_lock, &dl);
        t = now_s();
    }
    gate_inside++;
    if (gate_inside > gate_peak) gate_peak = gate_inside;
    if (waited) {
        double w = t - t0;
        gate_wait_s += w;
        if (w > gate_wait_max_s) gate_wait_max_s = w;
    }
    pthread_mutex_unlock(&gate_lock);
    return 1;
}

void vkb_compile_gate_leave(int took)
{
    tls_depth--;
    if (!took) return;
    pthread_mutex_lock(&gate_lock);
    gate_inside--;
    pthread_cond_signal(&gate_cond);
    pthread_mutex_unlock(&gate_lock);
}

void vkb_compile_gate_stats(int *limit, int *peak, uint64_t *waits, uint64_t *tight_waits, double *wait_s, double *wait_max_s)
{
    pthread_mutex_lock(&gate_lock);
    if (limit) *limit = gate_limit < 0 ? 0 : gate_limit;
    if (peak) *peak = gate_peak;
    if (waits) *waits = gate_waits;
    if (tight_waits) *tight_waits = gate_tight_waits;
    if (wait_s) *wait_s = gate_wait_s;
    if (wait_max_s) *wait_max_s = gate_wait_max_s;
    pthread_mutex_unlock(&gate_lock);
}
