/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * hotpath_bench: two of the server's per-request costs on their own, with the code the server runs
 * (common/vkb_wire.c's arena, server/nd_map.h). Built twice by host/Makefile: hotpath_bench (as
 * shipped) and hotpath_bench_old (ARENA_SPARE_MAX=0, MAP_MAX_BUCKETS=8192: the arena and the
 * null-descriptor set map before 2026-10-21). Prints ns per operation; exit 1 when a check fails.
 *
 *   arena: a request decoding ~600 KiB in 64 B..4 KiB pieces (a command buffer's commands at
 *          vkEndCommandBuffer), then the reset after it. glibc's mmap threshold is set to 64 KiB so
 *          a block past it is an mmap/munmap each, as on bionic (scudo's largest size class is
 *          64 KiB, and a block is 64 KiB plus its header).
 *   ndmap: nd_AllocateDescriptorSets's map_get of a descriptor set the driver handed out before,
 *          with 50k / 200k / 1M sets mapped (the map keeps every set handle it has seen).
 */
#define _GNU_SOURCE
#include "vkb_wire.h"
#include "nd_map.h"

#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

void vkb_log(int level, const char *fmt, ...) { (void)level; (void)fmt; }
int vkb_log_level(void) { return 0; }
/* the halves' own, which the generated codecs call; unused here */
uint64_t vkb_remote(const void *dispatchable) { return (uint64_t)(uintptr_t)dispatchable; }
int vkb_cb_begin_is_secondary(const VkCommandBufferBeginInfo *s) { return 0; }

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static uint64_t rng = 0x243f6a8885a308d3ull;
static uint64_t next(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
}

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static void arena(int requests)
{
    vkb_arena a = {0};
    size_t total = 0;
    unsigned allocs = 0;
    double t0 = now();
    for (int r = 0; r < requests; r++) {
        size_t used = 0;
        while (used < 600u << 10) {
            size_t n = 64 + next() % 4033;
            uint8_t *p = vkb_arena_alloc(&a, n);
            CHECK(p, "arena alloc of %zu", n);
            if (!p) return;
            /* zeroed, as the decoders rely on */
            CHECK(p[0] == 0 && p[n - 1] == 0, "arena memory not zeroed");
            memset(p, 0xa5, n);
            used += n;
            allocs++;
        }
        if (r % 64 == 0) {
            uint8_t *big = vkb_arena_alloc(&a, 300u << 10); /* a large array now and then */
            CHECK(big && big[0] == 0 && big[(300u << 10) - 1] == 0, "large arena alloc");
        }
        total += used;
        vkb_arena_reset(&a);
    }
    double t = now() - t0;
    CHECK(a.nspare <= 32, "spare blocks %u", a.nspare);
    vkb_arena_free(&a);
    CHECK(!a.head && !a.spare && !a.nspare, "arena not empty after free");
    printf("arena: %d requests of ~600 KiB: %.0f us per request, %.1f ns per allocation\n", requests, t * 1e6 / requests,
           t * 1e9 / allocs);
}

static void ndmap(uint32_t sets, uint32_t lookups)
{
    nd_map m = {0};
    /* Mali's set handles: heap pointers, tagged in the top byte on Android. */
    for (uint32_t i = 0; i < sets; i++) {
        nd_ent *e = map_get(&m, 0xb400007000000000ull + (uint64_t)i * 0x40, 8192);
        CHECK(e, "map_get insert");
        if (e) e->val = i;
    }
    CHECK(m.count == sets, "count %u of %u", m.count, sets);
    uint64_t sum = 0;
    double t0 = now();
    for (uint32_t i = 0; i < lookups; i++) {
        uint32_t k = (uint32_t)(next() % sets);
        nd_ent *e = map_get(&m, 0xb400007000000000ull + (uint64_t)k * 0x40, 8192);
        if (!e || e->val != k) {
            CHECK(0, "map_get of set %u", k);
            break;
        }
        sum += e->val;
    }
    double t = now() - t0;
    CHECK(m.count == sets, "lookups added entries");
    CHECK(map_take(&m, 0xb400007000000000ull) == 0 && m.count == sets - 1, "map_take");
    uint32_t buckets = m.mask + 1;
    map_free(&m, 0);
    printf("ndmap: %7u sets mapped, %7u buckets: %.1f ns per lookup (%llu)\n", sets, buckets, t * 1e9 / lookups,
           (unsigned long long)(sum & 1));
}

int main(int argc, char **argv)
{
    int requests = argc > 1 ? atoi(argv[1]) : 2000;
    mallopt(M_MMAP_THRESHOLD, 64 * 1024);
    arena(requests);
    ndmap(50000, 2000000);
    ndmap(200000, 2000000);
    ndmap(1000000, 2000000);
    if (fails) printf("%d check(s) failed\n", fails);
    else printf("ok\n");
    return fails != 0;
}
