/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * wire_check: the encoder and decoder primitives (common/vkb_wire.h's inline u8/u32/u64/bytes and
 * vkb_enc_raw's padding) against a byte-wise model of the wire format, over random field sequences
 * and buffer capacities, with the buffer-full, failed-encoder and short-input edges. Then the cost
 * of a field through the inline fast path against the out-of-line call chain the primitives were
 * before 2026-11-02 (vkb_enc_u32 -> vkb_enc_bytes -> memcpy library call). Exit 1 when a check fails.
 * No driver, no server.
 */
#define _GNU_SOURCE
#include "vkb_wire.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void vkb_log(int level, const char *fmt, ...) { (void)level; (void)fmt; }
int vkb_log_level(void) { return 0; }
uint64_t vkb_remote(const void *dispatchable) { return (uint64_t)(uintptr_t)dispatchable; }
int vkb_cb_begin_is_secondary(const VkCommandBufferBeginInfo *s) { (void)s; return 1; }

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static uint64_t rng = 0x9e3779b97f4a7c15ull;
static uint64_t next(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
}

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* The model: what the wire format says, written without the library */
typedef struct {
    uint8_t *b;
    size_t len;
} model;
static void m_put(model *m, const void *p, size_t n)
{
    for (size_t i = 0; i < n; i++) m->b[m->len + i] = p ? ((const uint8_t *)p)[i] : 0;
    m->len += n;
}
static void m_align(model *m)
{
    while (m->len & 7) m->b[m->len++] = 0;
}

enum { OP_U8, OP_U32, OP_U64, OP_BYTES, OP_NULLBYTES, OP_RAW, OP_NOPS };
typedef struct {
    int op;
    uint64_t v;
    size_t n;
    uint8_t data[300];
} step;

static void random_steps(step *s, int count)
{
    for (int i = 0; i < count; i++) {
        s[i].op = (int)(next() % OP_NOPS);
        s[i].v = next();
        s[i].n = (size_t)(next() % 300);
        if (next() % 8 == 0) s[i].n = 0;
        for (size_t k = 0; k < sizeof(s[i].data); k++) s[i].data[k] = (uint8_t)next();
    }
}

static void enc_step(vkb_enc *e, const step *s)
{
    switch (s->op) {
    case OP_U8: vkb_enc_u8(e, (uint8_t)s->v); break;
    case OP_U32: vkb_enc_u32(e, (uint32_t)s->v); break;
    case OP_U64: vkb_enc_u64(e, s->v); break;
    case OP_BYTES: vkb_enc_bytes(e, s->data, s->n); break;
    case OP_NULLBYTES: vkb_enc_bytes(e, NULL, s->n); break;
    case OP_RAW: vkb_enc_raw(e, s->data, s->n); break;
    }
}

static void model_step(model *m, const step *s)
{
    switch (s->op) {
    case OP_U8: { uint8_t v = (uint8_t)s->v; m_put(m, &v, 1); break; }
    case OP_U32: { uint32_t v = (uint32_t)s->v; m_put(m, &v, 4); break; }
    case OP_U64: m_put(m, &s->v, 8); break;
    case OP_BYTES: if (s->n) m_put(m, s->data, s->n); break;
    case OP_NULLBYTES: if (s->n) m_put(m, NULL, s->n); break;
    case OP_RAW: m_align(m); if (s->n) m_put(m, s->data, s->n); break;
    }
}

/* Encoder: every field sequence, from buffers of every starting size (0 = first use, small ones grow several times) */
static void encoder(void)
{
    static step steps[200];
    static uint8_t mbuf[200 * 400];
    for (int round = 0; round < 3000; round++) {
        int count = 1 + (int)(next() % 200);
        random_steps(steps, count);
        vkb_enc e;
        vkb_enc_init(&e);
        size_t start_cap = (size_t)(next() % 6) * 7; /* 0, 7, 14, ... */
        if (start_cap) {
            e.buf = malloc(start_cap);
            e.cap = start_cap;
        }
        model m = {mbuf, 0};
        memset(mbuf, 0xcc, sizeof(mbuf));
        for (int i = 0; i < count; i++) {
            enc_step(&e, &steps[i]);
            model_step(&m, &steps[i]);
        }
        CHECK(!e.oom, "round %d: encoder reported oom", round);
        CHECK(e.len == m.len, "round %d: length %zu, model %zu", round, e.len, m.len);
        CHECK(e.len == m.len && !memcmp(e.buf, mbuf, m.len), "round %d: bytes differ from the model (start cap %zu)", round, start_cap);
        vkb_enc_free(&e);
    }
    /* a failed encoder stays failed and adds nothing, whatever room is left */
    vkb_enc e;
    vkb_enc_init(&e);
    vkb_enc_u32(&e, 1);
    e.oom = 1;
    size_t len = e.len;
    vkb_enc_u8(&e, 1);
    vkb_enc_u64(&e, 1);
    vkb_enc_bytes(&e, "abc", 3);
    vkb_enc_raw(&e, "abc", 3);
    CHECK(e.len == len, "oom encoder grew from %zu to %zu", len, e.len);
    vkb_enc_free(&e);
    /* exactly full: the fast path must not write past cap, the next field grows */
    vkb_enc_init(&e);
    e.buf = malloc(8);
    e.cap = 8;
    vkb_enc_u64(&e, 0x1122334455667788ull);
    CHECK(e.len == 8 && e.cap == 8, "exactly full buffer grew");
    vkb_enc_u8(&e, 9);
    CHECK(e.len == 9 && e.cap >= 9 && e.buf[8] == 9, "field past the end");
    vkb_enc_free(&e);
    /* padding at the very end of the buffer (no room for the 8-byte pad store) */
    vkb_enc_init(&e);
    e.buf = malloc(5);
    e.cap = 5;
    vkb_enc_bytes(&e, "abcde", 5);
    size_t off = vkb_enc_raw(&e, "XY", 2);
    CHECK(off == 8 && e.len == 10 && !memcmp(e.buf, "abcde\0\0\0XY", 10), "pad at a full buffer: off %zu len %zu", off, e.len);
    vkb_enc_free(&e);
}

/* Decoder: what the encoder wrote reads back; a short or failed input gives zeros and sets err, as before */
static void decoder(void)
{
    static step steps[200];
    for (int round = 0; round < 3000; round++) {
        int count = 1 + (int)(next() % 200);
        random_steps(steps, count);
        vkb_enc e;
        vkb_enc_init(&e);
        for (int i = 0; i < count; i++) enc_step(&e, &steps[i]);
        vkb_arena a = {0};
        vkb_dec d;
        vkb_dec_init(&d, e.buf, e.len, &a, NULL, 0);
        int ok = 1;
        for (int i = 0; i < count && ok; i++) {
            const step *s = &steps[i];
            uint8_t tmp[300];
            switch (s->op) {
            case OP_U8: ok = vkb_dec_u8(&d) == (uint8_t)s->v; break;
            case OP_U32: ok = vkb_dec_u32(&d) == (uint32_t)s->v; break;
            case OP_U64: ok = vkb_dec_u64(&d) == s->v; break;
            case OP_BYTES: vkb_dec_bytes(&d, tmp, s->n); ok = !memcmp(tmp, s->data, s->n); break;
            case OP_NULLBYTES: vkb_dec_bytes(&d, tmp, s->n); for (size_t k = 0; k < s->n; k++) ok &= tmp[k] == 0; break;
            case OP_RAW: vkb_dec_raw_into(&d, tmp, s->n); ok = !memcmp(tmp, s->data, s->n); break;
            }
            ok &= !d.err;
        }
        CHECK(ok, "round %d: decode of the encoded sequence", round);
        CHECK(d.pos == d.len, "round %d: decoded %zu of %zu bytes", round, d.pos, d.len);
        vkb_arena_free(&a);
        vkb_enc_free(&e);
    }
    uint8_t in[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    vkb_arena a = {0};
    vkb_dec d;
    vkb_dec_init(&d, in, 6, &a, NULL, 0);
    CHECK(vkb_dec_u32(&d) == 0x04030201u && !d.err && d.pos == 4, "u32 read");
    CHECK(vkb_dec_u32(&d) == 0 && d.err && d.pos == 4, "u32 past the end: zero, err, not advanced (pos %zu)", d.pos);
    CHECK(vkb_dec_u8(&d) == 0 && d.pos == 4, "after err a read gives zero and does not advance");
    uint64_t big = ~0ull;
    vkb_dec_init(&d, in, 6, &a, NULL, 0);
    vkb_dec_bytes(&d, &big, 8);
    CHECK(big == 0 && d.err && d.pos == 0, "8 bytes of 6: dst zeroed, err");
    vkb_dec_init(&d, in, 6, &a, NULL, 0);
    vkb_dec_bytes(&d, NULL, 4); /* skipping */
    CHECK(!d.err && d.pos == 4, "skip with NULL dst");
    vkb_dec_bytes(&d, NULL, 4);
    CHECK(d.err && d.pos == 4, "skip past the end");
    vkb_dec_init(&d, in, 0, &a, NULL, 0);
    vkb_dec_bytes(&d, in, 0);
    CHECK(!d.err, "zero bytes from nothing");
}

/* The cost of a field: the inline primitives against the call chain they replaced */
__attribute__((noinline)) static void old_u32(vkb_enc *e, uint32_t v) { vkb_enc_bytes_slow(e, &v, 4); }
__attribute__((noinline)) static void old_u8(vkb_enc *e, uint8_t v) { vkb_enc_bytes_slow(e, &v, 1); }
__attribute__((noinline)) static void old_u64(vkb_enc *e, uint64_t v) { vkb_enc_bytes_slow(e, &v, 8); }
__attribute__((noinline)) static uint32_t old_d32(vkb_dec *d) { uint32_t v; vkb_dec_bytes_slow(d, &v, 4); return v; }

static void bench(void)
{
    enum { FIELDS = 64, REQS = 400000 };
    vkb_enc e;
    vkb_enc_init(&e);
    double best_new = 1e9, best_old = 1e9;
    uint64_t sink = 0;
    for (int rep = 0; rep < 5; rep++) {
        double t0 = now();
        for (int r = 0; r < REQS; r++) {
            vkb_enc_reset(&e);
            for (int i = 0; i < FIELDS / 4; i++) {
                vkb_enc_u64(&e, (uint64_t)r + (uint64_t)i);
                vkb_enc_u32(&e, (uint32_t)i);
                vkb_enc_u32(&e, (uint32_t)r);
                vkb_enc_u8(&e, (uint8_t)i);
            }
            sink += e.len;
        }
        double t1 = now();
        for (int r = 0; r < REQS; r++) {
            vkb_enc_reset(&e);
            for (int i = 0; i < FIELDS / 4; i++) {
                old_u64(&e, (uint64_t)r + (uint64_t)i);
                old_u32(&e, (uint32_t)i);
                old_u32(&e, (uint32_t)r);
                old_u8(&e, (uint8_t)i);
            }
            sink += e.len;
        }
        double t2 = now();
        if (t1 - t0 < best_new) best_new = t1 - t0;
        if (t2 - t1 < best_old) best_old = t2 - t1;
    }
    printf("encode: %.2f ns per field inline, %.2f ns per field before (%.1fx)\n", best_new * 1e9 / ((double)REQS * FIELDS),
           best_old * 1e9 / ((double)REQS * FIELDS), best_old / best_new);
    /* decode */
    vkb_arena a = {0};
    vkb_dec d;
    best_new = best_old = 1e9;
    for (int rep = 0; rep < 5; rep++) {
        double t0 = now();
        for (int r = 0; r < REQS; r++) {
            vkb_dec_init(&d, e.buf, e.len, &a, NULL, 0);
            for (int i = 0; i < FIELDS / 4; i++) {
                sink += vkb_dec_u64(&d);
                sink += vkb_dec_u32(&d);
                sink += vkb_dec_u32(&d);
                sink += vkb_dec_u8(&d);
            }
        }
        double t1 = now();
        for (int r = 0; r < REQS; r++) {
            vkb_dec_init(&d, e.buf, e.len, &a, NULL, 0);
            for (int i = 0; i < FIELDS / 4; i++) {
                uint64_t v;
                vkb_dec_bytes_slow(&d, &v, 8);
                sink += v;
                sink += old_d32(&d);
                sink += old_d32(&d);
                uint8_t b;
                vkb_dec_bytes_slow(&d, &b, 1);
                sink += b;
            }
        }
        double t2 = now();
        if (t1 - t0 < best_new) best_new = t1 - t0;
        if (t2 - t1 < best_old) best_old = t2 - t1;
    }
    printf("decode: %.2f ns per field inline, %.2f ns per field before (%.1fx) (%llu)\n", best_new * 1e9 / ((double)REQS * FIELDS),
           best_old * 1e9 / ((double)REQS * FIELDS), best_old / best_new, (unsigned long long)(sink & 1));
    vkb_enc_free(&e);
}

int main(void)
{
    encoder();
    decoder();
    bench();
    if (fails) {
        printf("FAILED: %d of %d checks\n", fails, checks);
        return 1;
    }
    printf("OK: %d checks passed\n", checks);
    return 0;
}
