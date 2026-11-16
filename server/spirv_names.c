/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * See spirv_names.h.
 */
#include "spirv_names.h"
#include "spirv_edit.h"

#include <stdlib.h>
#include <string.h>

int vkb_spv_strip_names(const uint32_t *code, size_t size, uint32_t **out, size_t *out_size)
{
    size_t n = size / 4;
    if (!code || (size & 3) || n < 5 || code[0] != 0x07230203u) return -1;
    size_t drop = 0;
    for (size_t i = 5; i < n;) {
        uint32_t op = code[i] & 0xffff, len = code[i] >> 16;
        if (!len || i + len > n) return -1;
        if (op == SpvOpName || op == SpvOpMemberName) drop += len;
        i += len;
    }
    if (!drop) return 0;
    uint32_t *o = malloc((n - drop) * 4);
    if (!o) return -1;
    memcpy(o, code, 20);
    size_t k = 5;
    for (size_t i = 5; i < n;) {
        uint32_t op = code[i] & 0xffff, len = code[i] >> 16;
        if (op != SpvOpName && op != SpvOpMemberName) {
            memcpy(o + k, code + i, (size_t)len * 4);
            k += len;
        }
        i += len;
    }
    *out = o;
    *out_size = k * 4;
    return 1;
}

/* The name's words of an OpName (target id first): equal when the strings are */
static int same_name(const spv_inst *a, const spv_inst *b)
{
    return a->n == b->n && a->n > 2 && !memcmp(a->w + 1, b->w + 1, (size_t)(a->n - 2) * 4);
}

#define NULLS 8

int vkb_spv_names_imitate(const uint32_t *code, size_t size, uint32_t **out, size_t *out_size, unsigned *shared)
{
    spv_mod m;
    if (spv_parse(&m, code, size) < 0) {
        spv_free(&m);
        return -1;
    }
    uint32_t bound = m.header[3];
    uint8_t *kind = calloc(bound ? bound : 1, 1); /* 1: an Input variable, 2: an Output variable (by id) */
    int *name = calloc(bound ? bound : 1, sizeof(int)); /* the instruction index of its OpName, +1 */
    if (!kind || !name) {
        free(kind), free(name), spv_free(&m);
        return -1;
    }
    for (uint32_t i = 0; i < m.count; i++) {
        const spv_inst *in = &m.ins[i];
        if (in->op == SpvOpName && in->n > 2 && in->w[0] < bound) name[in->w[0]] = (int)i + 1;
        else if (in->op == SpvOpVariable && in->n >= 4 && in->w[1] < bound) {
            if (in->w[2] == SpvStorageClassInput) kind[in->w[1]] = 1;
            else if (in->w[2] == SpvStorageClassOutput) kind[in->w[1]] = 2;
        }
    }
    unsigned nshared = 0;
    for (uint32_t o = 1; o < bound; o++) {
        if (kind[o] != 2 || !name[o]) continue;
        int hit = 0;
        for (uint32_t i = 1; i < bound && !hit; i++)
            if (kind[i] == 1 && name[i] && same_name(&m.ins[name[i] - 1], &m.ins[name[o] - 1])) hit = 1;
        if (hit) kind[o] = 3, nshared++;
    }
    int r = 0;
    if (nshared) {
        struct {
            uint32_t type, id;
        } nulls[NULLS];
        unsigned nn = 0;
        for (uint32_t i = 0; i < m.count; i++) {
            spv_inst *in = &m.ins[i];
            if (in->op != SpvOpStore || in->n < 3 || in->w[0] >= bound || kind[in->w[0]] != 3 || in->w[1] >= bound) continue;
            int d = spv_find_def(&m, in->w[1]);
            if (d < 0 || m.ins[d].n < 3) continue;
            uint32_t type = m.ins[d].w[0], id = 0;
            for (unsigned k = 0; k < nn; k++)
                if (nulls[k].type == type) id = nulls[k].id;
            if (!id) {
                if (nn == NULLS) continue;
                id = spv_new_id(&m);
                uint32_t ops[2] = {type, id};
                uint32_t at = spv_types_end(&m);
                spv_insert(&m, at, 46 /* OpConstantNull */, ops, 2);
                if (m.err) {
                    r = -1;
                    break;
                }
                nulls[nn].type = type, nulls[nn++].id = id;
                i++; /* what the loop holds shifted by the insert (it went in before the code) */
                in = &m.ins[i];
            }
            in->w[1] = id;
            r = 1;
        }
    }
    if (r == 1) {
        size_t s = spv_write(&m, out);
        if (!s) r = -1;
        else {
            *out_size = s;
            if (shared) *shared = nshared;
        }
    }
    free(kind), free(name), spv_free(&m);
    return r;
}
