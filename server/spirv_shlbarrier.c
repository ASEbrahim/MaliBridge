/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * shl_barrier: a no-op OpBitFieldInsert after each left shift by a constant (see spirv_shlbarrier.h).
 * A port of Lee Gao's MaliOptimizationBarrierPass
 * (https://github.com/leegao/SPIRV-Tools, source/opt/mali_optimization_barrier_pass.cpp), rewritten
 * in C over the module's words; the changes are listed in spirv_shlbarrier.h. Its notice:
 *
 *   Copyright (c) 2025 Lee Gao
 *
 *   Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file
 *   except in compliance with the License. You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software distributed under the
 *   License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
 *   either express or implied. See the License for the specific language governing permissions
 *   and limitations under the License.
 *
 * One pass over the words to learn the types and constants and find the shifts (most modules have
 * none and end there, untouched), a second to write the new code: the constants it needs right
 * before the first OpFunction, each shift with a new result id and the barrier after it.
 */
#include "spirv_shlbarrier.h"

#include <stdlib.h>
#include <string.h>

enum {
    OP_TYPE_INT = 21, OP_TYPE_VECTOR = 23, OP_CONSTANT = 43, OP_CONSTANT_COMPOSITE = 44, OP_CONSTANT_NULL = 46,
    OP_FUNCTION = 54, OP_FUNCTION_END = 56, OP_SHL_LOGICAL = 196, OP_BITFIELD_INSERT = 201,
};

#define MAX_BOUND (1u << 22)

typedef struct {
    uint16_t op;      /* the defining opcode */
    uint8_t width;    /* OpTypeInt: its width; OpTypeVector: its component's (when an integer) */
    uint8_t sign;     /* OpTypeInt: signedness */
    uint8_t lanes;    /* OpTypeVector: component count */
    uint8_t is_zero;  /* a constant whose value is 0 in every lane */
    uint8_t constant; /* OpConstant / OpConstantComposite / OpConstantNull */
    uint8_t barrier;  /* the Base of an OpBitFieldInsert that inserts 0 bits at 0 (a barrier) */
    uint32_t type;    /* a constant's result type; OpTypeVector: its component type */
    uint32_t zero;    /* a type: a zero constant of it (existing or to be made), 0 if none */
} id_info;

/* A 32-bit integer scalar (1) or vector (lane count), else 0 */
static unsigned int32_type(const id_info *ii, uint32_t bound, uint32_t t)
{
    if (!t || t >= bound) return 0;
    if (ii[t].op == OP_TYPE_INT) return ii[t].width == 32 ? 1 : 0;
    if (ii[t].op == OP_TYPE_VECTOR && ii[t].width == 32) return ii[t].lanes;
    return 0;
}

static int is_int_type(const id_info *ii, uint32_t bound, uint32_t t)
{
    return t && t < bound && (ii[t].op == OP_TYPE_INT || (ii[t].op == OP_TYPE_VECTOR && ii[t].width));
}

/* The new globals, in the order they are written */
typedef struct {
    uint32_t words[3 + 16];
    unsigned n;
} global;

typedef struct {
    global *g;
    unsigned n, cap;
} globals;

static int add_global(globals *gl, const uint32_t *w, unsigned nw)
{
    if (gl->n == gl->cap) {
        unsigned cap = gl->cap ? gl->cap * 2 : 8;
        global *g = realloc(gl->g, cap * sizeof(*g));
        if (!g) return 0;
        gl->g = g, gl->cap = cap;
    }
    memcpy(gl->g[gl->n].words, w, nw * 4);
    gl->g[gl->n++].n = nw;
    return 1;
}

int vkb_spv_shl_barrier(const uint32_t *code, size_t size, uint32_t **out, size_t *out_size, vkb_shlbarrier_stats *st,
                        const char **why)
{
    vkb_shlbarrier_stats local;
    if (!st) st = &local;
    memset(st, 0, sizeof(*st));
    *why = NULL;
    *out = NULL;
    *out_size = 0;
    size_t n = size / 4;
    if (n < 5 || (size & 3) || code[0] != 0x07230203u) return 0;
    uint32_t bound = code[3];
    if (!bound || bound > MAX_BOUND) {
        *why = "id bound out of range";
        return -1;
    }
    id_info *ii = calloc(bound, sizeof(*ii));
    if (!ii) {
        *why = "out of memory";
        return -1;
    }

    /* 1: types, constants, the shifts and the barriers already there */
    size_t first_fn = 0, cand = 0;
    int in_fn = 0;
    for (size_t i = 5; i < n;) {
        uint32_t op = code[i] & 0xffff, len = code[i] >> 16;
        if (!len || i + len > n) {
            free(ii);
            *why = "malformed instruction stream";
            return -1;
        }
        const uint32_t *w = code + i + 1;
        if (op == OP_FUNCTION) {
            in_fn = 1;
            if (!first_fn) first_fn = i;
        } else if (op == OP_FUNCTION_END)
            in_fn = 0;
        else if (op == OP_TYPE_INT && len >= 4 && w[0] < bound) {
            ii[w[0]].op = OP_TYPE_INT, ii[w[0]].width = w[1] <= 64 ? (uint8_t)w[1] : 0, ii[w[0]].sign = w[2] != 0;
        } else if (op == OP_TYPE_VECTOR && len >= 4 && w[0] < bound && w[1] < bound) {
            id_info *v = &ii[w[0]];
            v->op = OP_TYPE_VECTOR, v->type = w[1], v->lanes = w[2] <= 16 ? (uint8_t)w[2] : 0;
            v->width = ii[w[1]].op == OP_TYPE_INT ? ii[w[1]].width : 0;
        } else if (!in_fn && (op == OP_CONSTANT || op == OP_CONSTANT_COMPOSITE || op == OP_CONSTANT_NULL) && len >= 3 && w[0] < bound &&
                   w[1] < bound) {
            id_info *c = &ii[w[1]];
            c->op = (uint16_t)op, c->type = w[0], c->constant = 1;
            if (op == OP_CONSTANT_NULL)
                c->is_zero = 1;
            else if (op == OP_CONSTANT) {
                int z = len >= 4;
                for (uint32_t k = 2; k < len - 1; k++) z &= w[k] == 0;
                c->is_zero = (uint8_t)z;
            } else {
                int z = len >= 4;
                for (uint32_t k = 2; k < len - 1; k++) z &= w[k] < bound && ii[w[k]].is_zero;
                c->is_zero = (uint8_t)z;
            }
            /* the first OpConstant / OpConstantComposite zero of an integer type: the one reused */
            if (c->is_zero && op != OP_CONSTANT_NULL && is_int_type(ii, bound, w[0]) && !ii[w[0]].zero) ii[w[0]].zero = w[1];
        } else if (in_fn && op == OP_SHL_LOGICAL && len == 5)
            cand++;
        i += len;
    }
    if (!cand) {
        free(ii);
        return 0;
    }
    /* the barriers already there (a second run over the pass's own output) */
    for (size_t i = first_fn; i < n;) {
        uint32_t op = code[i] & 0xffff, len = code[i] >> 16;
        const uint32_t *w = code + i + 1;
        if (op == OP_BITFIELD_INSERT && len == 7 && w[2] < bound && w[3] < bound && w[4] < bound && w[5] < bound &&
            ii[w[3]].is_zero && ii[w[4]].is_zero && ii[w[5]].is_zero && ii[w[4]].op == OP_CONSTANT && ii[w[5]].op == OP_CONSTANT)
            ii[w[2]].barrier = 1;
        i += len;
    }

    /* 2: the shifts to barrier and the constants they need */
    uint32_t next = bound;
    globals gl = {0};
    uint32_t u0 = 0, uint_t = 0;
    int fail = 0;
    size_t extra = 0;
    unsigned targets = 0;
    for (size_t i = first_fn; i < n && !fail;) {
        uint32_t op = code[i] & 0xffff, len = code[i] >> 16;
        const uint32_t *w = code + i + 1;
        i += len;
        if (op != OP_SHL_LOGICAL || len != 5 || w[3] >= bound || !ii[w[3]].constant) continue;
        uint32_t t = w[0];
        unsigned lanes = int32_type(ii, bound, t);
        if (!lanes) {
            if (is_int_type(ii, bound, t)) st->skipped_width++;
            continue;
        }
        if (w[1] < bound && ii[w[1]].barrier) {
            st->already++;
            continue;
        }
        if (!u0) { /* the Offset and Count: a 32-bit integer 0 (unsigned if there is one) */
            for (uint32_t id = 1; id < bound && !u0; id++)
                if (ii[id].op == OP_TYPE_INT && ii[id].width == 32 && !ii[id].sign && ii[id].zero) u0 = ii[id].zero;
            for (uint32_t id = 1; id < bound && !u0; id++)
                if (ii[id].op == OP_TYPE_INT && ii[id].width == 32 && ii[id].zero) u0 = ii[id].zero;
            for (uint32_t id = 1; id < bound && !u0 && !uint_t; id++)
                if (ii[id].op == OP_TYPE_INT && ii[id].width == 32 && !ii[id].sign) uint_t = id;
            if (!u0) {
                if (!uint_t) {
                    uint_t = next++;
                    const uint32_t tw[] = {(4u << 16) | OP_TYPE_INT, uint_t, 32, 0};
                    fail |= !add_global(&gl, tw, 4);
                }
                u0 = next++;
                const uint32_t cw[] = {(4u << 16) | OP_CONSTANT, uint_t, u0, 0};
                fail |= !add_global(&gl, cw, 4);
                if (uint_t < bound) ii[uint_t].zero = u0;
            }
        }
        if (!ii[t].zero) {
            uint32_t c = lanes == 1 ? t : ii[t].type;
            if (!ii[c].zero) {
                ii[c].zero = next++;
                const uint32_t cw[] = {(4u << 16) | OP_CONSTANT, c, ii[c].zero, 0};
                fail |= !add_global(&gl, cw, 4);
            }
            if (lanes > 1) {
                uint32_t cw[3 + 16];
                cw[0] = ((3u + lanes) << 16) | OP_CONSTANT_COMPOSITE, cw[1] = t, cw[2] = ii[t].zero = next++;
                for (unsigned k = 0; k < lanes; k++) cw[3 + k] = ii[c].zero;
                fail |= !add_global(&gl, cw, 3 + lanes);
            }
        }
        targets++;
        extra += 7;
        if (lanes == 1) st->scalar++;
        else st->vector++;
    }
    global *g = gl.g;
    unsigned ng = gl.n;
    if (fail) {
        free(g);
        free(ii);
        *why = "out of memory";
        return -1;
    }
    if (!targets) {
        free(g);
        free(ii);
        return 0;
    }
    if ((uint64_t)next + targets > MAX_BOUND) {
        free(g);
        free(ii);
        memset(st, 0, sizeof(*st));
        *why = "id bound out of range";
        return -1;
    }
    for (unsigned k = 0; k < ng; k++) extra += g[k].n;

    /* 3: the new code */
    uint32_t *o = malloc((n + extra) * 4);
    if (!o) {
        free(g);
        free(ii);
        memset(st, 0, sizeof(*st));
        *why = "out of memory";
        return -1;
    }
    memcpy(o, code, first_fn * 4);
    size_t p = first_fn;
    for (unsigned k = 0; k < ng; k++) {
        memcpy(o + p, g[k].words, g[k].n * 4);
        p += g[k].n;
    }
    for (size_t i = first_fn; i < n;) {
        uint32_t op = code[i] & 0xffff, len = code[i] >> 16;
        const uint32_t *w = code + i + 1;
        memcpy(o + p, code + i, len * 4);
        if (op == OP_SHL_LOGICAL && len == 5 && w[3] < bound && ii[w[3]].constant && int32_type(ii, bound, w[0]) &&
            !(w[1] < bound && ii[w[1]].barrier)) {
            uint32_t t = w[0], r = w[1], tmp = next++;
            o[p + 2] = tmp;
            p += len;
            o[p++] = (7u << 16) | OP_BITFIELD_INSERT;
            o[p++] = t;
            o[p++] = r;
            o[p++] = tmp;
            o[p++] = ii[t].zero;
            o[p++] = u0;
            o[p++] = u0;
        } else
            p += len;
        i += len;
    }
    o[3] = next;
    free(g);
    free(ii);
    *out = o;
    *out_size = p * 4;
    return 1;
}
