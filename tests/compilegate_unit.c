/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * compilegate_unit: the compile gate (server/server_compile_gate.c) on its own, with the server's
 * environment, memory and log functions stubbed. 12 threads each "compile" (sleep 20 ms inside the
 * gate) 10 times:
 *   1  VKBRIDGE_COMPILE_THREADS=3, memory plentiful: never more than 3 inside, 3 reached, waits counted;
 *   2  memory low (MemAvailable under an eighth of RAM): never more than 1 inside;
 *   3  memory recovers (over a sixth): 3 again;
 *   4  a cache-only create (FAIL_ON_PIPELINE_COMPILE_REQUIRED) and a nested create pass without a slot;
 *   5  the default for 8, 4 and 12 cores (4, 2, 4).
 * Prints "OK: ..." / "FAILED: ..." last. host/Makefile builds it; host/run_tests.sh runs it.
 */
#include "../server/server_compile_gate.c"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static volatile uint64_t fake_avail_kb = 6000ull << 10, fake_total_mb = 8000;
static const char *fake_threads = "3";

const char *vkb_srv_env(const char *name) { return strcmp(name, "VKBRIDGE_COMPILE_THREADS") ? NULL : fake_threads; }
uint64_t vkb_memavail_kb(void) { return fake_avail_kb; }
uint64_t vkb_memtotal_mb(void) { return fake_total_mb; }
int vkb_mem_low(void) { return 0; }
void vkb_log(int level, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "  log: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

static int inside, inside_max;
static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;

static void *worker(void *arg)
{
    (void)arg;
    for (int i = 0; i < 10; i++) {
        int took = vkb_compile_gate_enter(0);
        pthread_mutex_lock(&m);
        if (++inside > inside_max) inside_max = inside;
        pthread_mutex_unlock(&m);
        usleep(20000);
        pthread_mutex_lock(&m);
        inside--;
        pthread_mutex_unlock(&m);
        vkb_compile_gate_leave(took);
    }
    return NULL;
}

static int burst(void)
{
    pthread_t t[12];
    inside_max = 0;
    for (int i = 0; i < 12; i++) pthread_create(&t[i], NULL, worker, NULL);
    for (int i = 0; i < 12; i++) pthread_join(t[i], NULL);
    return inside_max;
}

int main(void)
{
    int fails = 0;
    int n = burst();
    int limit, peak;
    uint64_t waits, tight;
    double ws, wmax;
    vkb_compile_gate_stats(&limit, &peak, &waits, &tight, &ws, &wmax);
    printf("step 1: limit %d, at most %d inside, peak %d, %llu waits, longest %.0f ms\n", limit, n, peak, (unsigned long long)waits, wmax * 1000);
    if (limit != 3 || n > 3 || peak != 3 || waits == 0) fails++;

    fake_avail_kb = 500ull << 10; /* 500 MB of 8000: under an eighth */
    usleep(300000);               /* past the sample interval */
    n = burst();
    vkb_compile_gate_stats(NULL, NULL, NULL, &tight, NULL, NULL);
    printf("step 2: memory low: at most %d inside, %llu waits while low\n", n, (unsigned long long)tight);
    if (n != 1 || tight == 0) fails++;

    fake_avail_kb = 1200ull << 10; /* over an eighth, under a sixth: still low (the gap keeps it from flapping) */
    usleep(300000);
    n = burst();
    printf("step 3a: 1200 MB free: at most %d inside\n", n);
    if (n != 1) fails++;
    fake_avail_kb = 2000ull << 10; /* over a sixth: recovered */
    usleep(300000);
    n = burst();
    printf("step 3b: 2000 MB free: at most %d inside\n", n);
    if (n != 3) fails++;

    /* step 4: with every slot taken, a cache-only create and a nested one still pass */
    int held[3];
    for (int i = 0; i < 3; i++) held[i] = vkb_compile_gate_enter(0); /* nested on this thread: only the first takes a slot */
    int nested_ok = held[0] == 1 && held[1] == 0 && held[2] == 0;
    for (int i = 2; i >= 0; i--) vkb_compile_gate_leave(held[i]);
    int cache_only = vkb_compile_gate_enter(VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT);
    vkb_compile_gate_leave(cache_only);
    printf("step 4: nested %s, cache-only %s\n", nested_ok ? "passes" : "TOOK A SLOT", cache_only ? "TOOK A SLOT" : "passes");
    if (!nested_ok || cache_only) fails++;
    pthread_mutex_lock(&gate_lock);
    int left = gate_inside;
    pthread_mutex_unlock(&gate_lock);
    if (left != 0) {
        printf("step 4: %d slots left taken\n", left);
        fails++;
    }

    int d8 = vkb_compile_gate_default(8), d4 = vkb_compile_gate_default(4), d12 = vkb_compile_gate_default(12);
    printf("step 5: default for 8/4/12 cores: %d/%d/%d\n", d8, d4, d12);
    if (d8 != 4 || d4 != 2 || d12 != 4) fails++;

    if (fails) printf("FAILED: %d step(s)\n", fails);
    else printf("OK: the gate holds compiles to its limit, to one while memory is low, and lets cache-only and nested creates through\n");
    return fails != 0;
}
