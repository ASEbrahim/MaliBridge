/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * See spirv_fragdepth.h.
 */
#include "spirv_fragdepth.h"

#include <stdlib.h>
#include <string.h>

enum {
    OP_NAME = 5,
    OP_MEMBER_NAME = 6,
    OP_ENTRY_POINT = 15,
    OP_EXECUTION_MODE = 16,
    OP_VARIABLE = 59,
    OP_STORE = 62,
    OP_DECORATE = 71,
    OP_MEMBER_DECORATE = 72,
    OP_EXECUTION_MODE_ID = 331,
    OP_DECORATE_ID = 332,
    OP_DECORATE_STRING = 5632,
};
enum { MODEL_FRAGMENT = 4, DEC_BUILTIN = 11, BUILTIN_FRAG_DEPTH = 22 };
enum { MODE_DEPTH_REPLACING = 12, MODE_DEPTH_GREATER = 14, MODE_DEPTH_LESS = 15, MODE_DEPTH_UNCHANGED = 16 };

#define MAX_VARS 4

typedef struct {
    uint32_t var[MAX_VARS];
    unsigned nvar;
    unsigned entries;
    int fragment;
} scan;

static int valid_header(const uint32_t *w, size_t n) { return n >= 5 && w[0] == 0x07230203u; }

/* Words of an OpEntryPoint before its interface: opcode, model, id, then the name (nul-terminated, padded) */
static size_t entry_interface_start(const uint32_t *w, size_t len)
{
    size_t k = 3;
    while (k < len) {
        uint32_t x = w[k++];
        if (!(x & 0xff000000u) || !(x & 0x00ff0000u) || !(x & 0x0000ff00u) || !(x & 0x000000ffu)) break;
    }
    return k;
}

static int is_var(const scan *s, uint32_t id)
{
    for (unsigned i = 0; i < s->nvar; i++)
        if (s->var[i] == id) return 1;
    return 0;
}

static int do_scan(const uint32_t *w, size_t n, scan *s)
{
    memset(s, 0, sizeof(*s));
    for (size_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len || i + len > n) return -1;
        if (op == OP_ENTRY_POINT && len >= 3) {
            s->entries++;
            if (w[i + 1] == MODEL_FRAGMENT) s->fragment = 1;
        }
        if (op == OP_DECORATE && len >= 4 && w[i + 2] == DEC_BUILTIN && w[i + 3] == BUILTIN_FRAG_DEPTH && s->nvar < MAX_VARS)
            s->var[s->nvar++] = w[i + 1];
        i += len;
    }
    return 0;
}

int vkb_spv_fragdepth_writes(const uint32_t *code, size_t bytes)
{
    size_t n = bytes / 4;
    scan s;
    if (!code || !valid_header(code, n) || do_scan(code, n, &s) < 0) return 0;
    return s.fragment && s.nvar > 0;
}

int vkb_spv_strip_fragdepth(const uint32_t *code, size_t bytes, uint32_t **out, size_t *out_bytes, const char **why)
{
    size_t n = bytes / 4;
    scan s;
    *out = NULL;
    *out_bytes = 0;
    if (why) *why = "";
    if (!code || !valid_header(code, n) || do_scan(code, n, &s) < 0) {
        if (why) *why = "not a readable SPIR-V module";
        return code && valid_header(code, n) ? -1 : 0;
    }
    if (!s.fragment || !s.nvar) return 0;
    if (s.entries != 1) {
        if (why) *why = "more than one entry point";
        return -1;
    }
    /* Every other use of the variable refuses: only names, decorations, the interface, the
     * declaration and plain stores to it are known to be removable. */
    for (size_t i = 5; i < n;) {
        uint32_t op = code[i] & 0xffff, len = code[i] >> 16;
        int allowed = op == OP_NAME || op == OP_MEMBER_NAME || op == OP_DECORATE || op == OP_MEMBER_DECORATE || op == OP_DECORATE_ID ||
                      op == OP_DECORATE_STRING || op == OP_ENTRY_POINT || op == OP_VARIABLE;
        if (op == OP_STORE && len >= 3 && is_var(&s, code[i + 1])) {
            for (uint32_t k = 2; k < len; k++)
                if (is_var(&s, code[i + k])) {
                    if (why) *why = "the depth output stored into itself";
                    return -1;
                }
        } else if (!allowed) {
            for (uint32_t k = 1; k < len; k++)
                if (is_var(&s, code[i + k])) {
                    if (why) *why = "the depth output is used other than by stores (a load, an access chain or a call)";
                    return -1;
                }
        }
        i += len;
    }
    uint32_t *o = malloc(bytes);
    if (!o) {
        if (why) *why = "out of memory";
        return -1;
    }
    memcpy(o, code, 5 * 4);
    size_t on = 5;
    for (size_t i = 5; i < n;) {
        uint32_t op = code[i] & 0xffff, len = code[i] >> 16;
        int drop = 0;
        if ((op == OP_NAME || op == OP_DECORATE || op == OP_DECORATE_ID || op == OP_DECORATE_STRING) && len >= 2 && is_var(&s, code[i + 1]))
            drop = 1;
        else if (op == OP_VARIABLE && len >= 4 && is_var(&s, code[i + 2]))
            drop = 1;
        else if (op == OP_STORE && len >= 3 && is_var(&s, code[i + 1]))
            drop = 1;
        else if ((op == OP_EXECUTION_MODE || op == OP_EXECUTION_MODE_ID) && len >= 3 &&
                 (code[i + 2] == MODE_DEPTH_REPLACING || code[i + 2] == MODE_DEPTH_GREATER || code[i + 2] == MODE_DEPTH_LESS ||
                  code[i + 2] == MODE_DEPTH_UNCHANGED))
            drop = 1;
        if (drop) {
            i += len;
            continue;
        }
        if (op == OP_ENTRY_POINT) {
            size_t start = on;
            size_t iface = entry_interface_start(code + i, len);
            memcpy(o + on, code + i, iface * 4);
            on += iface;
            for (size_t k = iface; k < len; k++)
                if (!is_var(&s, code[i + k])) o[on++] = code[i + k];
            o[start] = (uint32_t)((on - start) << 16) | OP_ENTRY_POINT;
        } else {
            memcpy(o + on, code + i, (size_t)len * 4);
            on += len;
        }
        i += len;
    }
    *out = o;
    *out_bytes = on * 4;
    return 1;
}
