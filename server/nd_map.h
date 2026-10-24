/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * Small hash maps of handles for server_nulldesc.c (a header so tests/ndmap_bench.c times the
 * same code): uint64 keys to uint64 values, chained, growing 4x past two entries a bucket.
 */
#ifndef ND_MAP_H
#define ND_MAP_H

#include <stdint.h>
#include <stdlib.h>

typedef struct nd_ent {
    uint64_t key, val;
    struct nd_ent *next;
} nd_ent;

typedef struct nd_map {
    nd_ent **b;
    uint32_t mask, count;
} nd_map;

static inline unsigned slot(const nd_map *m, uint64_t key)
{
    key *= 0x9e3779b97f4a7c15ull;
    return (unsigned)(key >> 40) & m->mask;
}

static inline nd_ent *map_find(const nd_map *m, uint64_t key)
{
    if (!m->b) return NULL;
    nd_ent *e = m->b[slot(m, key)];
    while (e && e->key != key) e = e->next;
    return e;
}

/* Past two entries a bucket the table grows 4x (up to MAP_MAX_BUCKETS): the set map holds every
 * descriptor set the driver ever handed out (up to MAX_SETS_MAPPED), and with its first 8192
 * buckets kept, each allocation and each update walked chains dozens long (RE2 on the S11 Ultra:
 * nd_AllocateDescriptorSets 8.5% of the server's CPU, the most of any function of its own,
 * 2026-10-21). A failed grow keeps the old table. */
#ifndef MAP_MAX_BUCKETS /* tests/ndmap_bench.c builds the old fixed table with 8192 */
#define MAP_MAX_BUCKETS (1u << 20)
#endif

static inline void map_grow(nd_map *m)
{
    uint32_t nb = (m->mask + 1) * 4;
    if (nb > MAP_MAX_BUCKETS || nb <= m->mask + 1) return;
    nd_ent **b = calloc(nb, sizeof(*b));
    if (!b) return;
    nd_map g = {b, nb - 1, m->count};
    for (uint32_t i = 0; i <= m->mask; i++)
        while (m->b[i]) {
            nd_ent *e = m->b[i];
            m->b[i] = e->next;
            unsigned s = slot(&g, e->key);
            e->next = b[s];
            b[s] = e;
        }
    free(m->b);
    *m = g;
}

/* The entry of key, made (val 0) when missing; NULL out of memory. */
static inline nd_ent *map_get(nd_map *m, uint64_t key, uint32_t buckets)
{
    if (!m->b) {
        if (!(m->b = calloc(buckets, sizeof(*m->b)))) return NULL;
        m->mask = buckets - 1;
    }
    nd_ent *e = map_find(m, key);
    if (e) return e;
    if (m->count >= 2 * (m->mask + 1)) map_grow(m);
    if (!(e = calloc(1, sizeof(*e)))) return NULL;
    e->key = key;
    unsigned s = slot(m, key);
    e->next = m->b[s];
    m->b[s] = e;
    m->count++;
    return e;
}

/* Removes key; returns its value (0 when missing). */
static inline uint64_t map_take(nd_map *m, uint64_t key)
{
    if (!m->b) return 0;
    for (nd_ent **pp = &m->b[slot(m, key)]; *pp; pp = &(*pp)->next)
        if ((*pp)->key == key) {
            nd_ent *e = *pp;
            uint64_t v = e->val;
            *pp = e->next;
            free(e);
            m->count--;
            return v;
        }
    return 0;
}

/* Empties the map; vals are freed when they are pointers. */
static inline void map_clear(nd_map *m, int vals)
{
    for (uint32_t i = 0; m->b && i <= m->mask; i++)
        while (m->b[i]) {
            nd_ent *e = m->b[i];
            m->b[i] = e->next;
            if (vals) free((void *)(uintptr_t)e->val);
            free(e);
        }
    m->count = 0;
}

static inline void map_free(nd_map *m, int vals)
{
    map_clear(m, vals);
    free(m->b);
    m->b = NULL;
}

#endif
