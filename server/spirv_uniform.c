/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * Workgroup uniformity of one function's structured control flow (see spirv_uniform.h).
 *
 * 1  the module's globals: uniform memory (variables and the built-ins that are the same for a
 *    workgroup), Private variables (and whether a function other than this one writes them);
 * 2  the function's constructs in order (header, merge, condition, parent), its blocks (innermost
 *    construct, terminator) and its candidate values (the results of the operations that can be
 *    uniform), all taken as uniform;
 * 3  rounds that lower what is shown not to be: values, variables (a store that is not uniform),
 *    constructs (a non-uniform condition, a loop exit from a non-uniform point), until nothing
 *    changes.
 */
#include "spirv_uniform.h"

#include <stdlib.h>
#include <string.h>

enum {
    OP_EXT_INST = 12, OP_TYPE_POINTER = 32, OP_FUNCTION = 54, OP_FUNCTION_END = 56, OP_FUNCTION_CALL = 57, OP_VARIABLE = 59,
    OP_LOAD = 61, OP_STORE = 62, OP_COPY_MEMORY = 63, OP_COPY_MEMORY_SIZED = 64, OP_ACCESS_CHAIN = 65,
    OP_IN_BOUNDS_ACCESS_CHAIN = 66, OP_PTR_ACCESS_CHAIN = 67, OP_IN_BOUNDS_PTR_ACCESS_CHAIN = 70, OP_DECORATE = 71,
    OP_VECTOR_EXTRACT_DYNAMIC = 77, OP_VECTOR_INSERT_DYNAMIC = 78, OP_VECTOR_SHUFFLE = 79, OP_COMPOSITE_CONSTRUCT = 80,
    OP_COMPOSITE_EXTRACT = 81, OP_COMPOSITE_INSERT = 82, OP_COPY_OBJECT = 83, OP_TRANSPOSE = 84, OP_IMAGE_FETCH = 95, OP_PHI = 245,
    OP_LOOP_MERGE = 246, OP_SELECTION_MERGE = 247, OP_LABEL = 248, OP_BRANCH = 249, OP_BRANCH_CONDITIONAL = 250,
    OP_SWITCH = 251, OP_KILL = 252, OP_RETURN = 253, OP_RETURN_VALUE = 254, OP_UNREACHABLE = 255,
    OP_TERMINATE_INVOCATION = 4416, OP_DEMOTE = 5380,
};

enum { SC_UNIFORM_CONSTANT = 0, SC_INPUT = 1, SC_UNIFORM = 2, SC_PRIVATE = 6, SC_FUNCTION = 7, SC_PUSH_CONSTANT = 9 };
enum { BI_NUM_WORKGROUPS = 24, BI_WORKGROUP_SIZE = 25, BI_WORKGROUP_ID = 26 };

/* per id */
enum { K_NONE, K_UPTR, K_PVAR_PTR }; /* a pointer into uniform memory / into a Private or Function variable */
enum { I_UBUILTIN = 1, I_CAND = 2, I_VAR = 4, I_PVAR_OTHER = 8 };

typedef struct {
    uint32_t header, merge, cond; /* cond: the condition or selector id (0: none) */
    int parent;
    uint8_t loop, uniform, ret;
    size_t from, to;
} construct;

typedef struct {
    uint32_t label;
    int cons;     /* innermost construct the block is in (-1: none) */
    size_t term;  /* its terminator's offset */
} block;

struct vkb_uniform {
    construct *c;
    unsigned nc;
    uint32_t bound;
    int32_t *hdr; /* label -> the construct it heads, -1 */
};

typedef struct {
    uint8_t *uv;    /* value uniform */
    uint8_t *kind;  /* K_* of a pointer */
    uint8_t *info;  /* I_* */
    uint8_t *var_u; /* a Private / Function variable: every store uniform */
    uint32_t *root; /* a K_PVAR_PTR pointer: its variable */
    int32_t *mrg;   /* label -> the construct it is the merge block of, -1 */
    int32_t *blk;   /* label -> its block index, -1 */
} state;

static int grow(void **v, unsigned *cap, unsigned n, size_t sz)
{
    if (n < *cap) return 1;
    unsigned c = *cap ? *cap * 2 : 32;
    void *p = realloc(*v, (size_t)c * sz);
    if (!p) return 0;
    *v = p, *cap = c;
    return 1;
}

/* The operations whose result is uniform when their (id) operands are: the first operand word
 * and how many (0: all to the end) */
static int candidate(uint32_t op, uint32_t len, unsigned *first, unsigned *count)
{
    *first = 2, *count = 0;
    if ((op >= 109 && op <= 152) || (op >= 154 && op <= 158) || (op >= 164 && op <= 191) || (op >= 194 && op <= 205)) return 1;
    switch (op) {
    case OP_COMPOSITE_CONSTRUCT:
    case OP_COPY_OBJECT:
    case OP_TRANSPOSE:
    case OP_VECTOR_INSERT_DYNAMIC:
    case OP_VECTOR_EXTRACT_DYNAMIC:
    case OP_ACCESS_CHAIN:
    case OP_IN_BOUNDS_ACCESS_CHAIN:
    case OP_PTR_ACCESS_CHAIN:
    case OP_IN_BOUNDS_PTR_ACCESS_CHAIN:
        return 1;
    case OP_COMPOSITE_EXTRACT:
        *count = 1;
        return 1;
    case OP_COMPOSITE_INSERT:
    case OP_VECTOR_SHUFFLE:
        *count = 2;
        return 1;
    case OP_EXT_INST: /* set, instruction (literal), then the operands */
        *first = 4;
        return 1;
    case OP_LOAD:
        *count = 1;
        return 1;
    case OP_PHI:
        return 1;
    case OP_IMAGE_FETCH: /* a sampled image's texel (read only): the same where image and coordinate (and the
                          * operands after them) are; the operand mask word counts too, which can only lower it */
        return 1;
    default:
        return 0;
    }
    (void)len;
}

static int depth_uniform(const construct *c, int k)
{
    for (; k >= 0; k = c[k].parent)
        if (!c[k].uniform) return 0;
    return 1;
}

/* Every construct from k up to (not including) stop is uniform */
static int uniform_up_to(const construct *c, int k, int stop)
{
    for (; k >= 0 && k != stop; k = c[k].parent)
        if (!c[k].uniform) return 0;
    return 1;
}

static int in_construct(const construct *c, int k, int target)
{
    for (; k >= 0; k = c[k].parent)
        if (k == target) return 1;
    return 0;
}

vkb_uniform *vkb_spv_uniform(const uint32_t *code, size_t n, size_t start, size_t end, uint32_t bound)
{
    return vkb_spv_uniform_given(code, n, start, end, bound, NULL);
}

vkb_uniform *vkb_spv_uniform_given(const uint32_t *code, size_t n, size_t start, size_t end, uint32_t bound, const uint8_t *given)
{
    vkb_uniform *u = calloc(1, sizeof(*u));
    state s = {0};
    block *b = NULL;
    unsigned nb = 0, bcap = 0, ccap = 0;
    int *stack = NULL;
    unsigned sn = 0, scap = 0;
    int ok = 0;
    if (!u) return NULL;
    u->bound = bound;
    s.uv = calloc(bound, 1), s.kind = calloc(bound, 1), s.info = calloc(bound, 1), s.var_u = calloc(bound, 1);
    s.root = calloc(bound, sizeof(*s.root));
    s.mrg = malloc(bound * sizeof(*s.mrg)), s.blk = malloc(bound * sizeof(*s.blk)), u->hdr = malloc(bound * sizeof(*u->hdr));
    if (!s.uv || !s.kind || !s.info || !s.var_u || !s.root || !s.mrg || !s.blk || !u->hdr) goto out;
    memset(s.mrg, 0xff, bound * sizeof(*s.mrg)), memset(s.blk, 0xff, bound * sizeof(*s.blk));
    memset(u->hdr, 0xff, bound * sizeof(*u->hdr));
    uint8_t *ptr_sc = calloc(bound, 1); /* a pointer type's storage class + 1 */
    if (!ptr_sc) goto out;

    /* 1: globals, and the other functions' writes to Private variables */
    int other_fn = 0;
    for (size_t i = 5; i < n;) {
        uint32_t op = code[i] & 0xffff, len = code[i] >> 16;
        const uint32_t *w = code + i + 1;
        if (!len || i + len > n) {
            free(ptr_sc);
            goto out;
        }
        if (op == OP_FUNCTION) other_fn = i != start;
        else if (op == OP_FUNCTION_END) other_fn = 0;
        else if (op == OP_DECORATE && len >= 4 && w[1] == 11 && w[0] < bound &&
                 (w[2] == BI_NUM_WORKGROUPS || w[2] == BI_WORKGROUP_SIZE || w[2] == BI_WORKGROUP_ID))
            s.info[w[0]] |= I_UBUILTIN;
        else if (op == OP_TYPE_POINTER && len >= 4 && w[0] < bound)
            ptr_sc[w[0]] = (uint8_t)(w[1] < 255 ? w[1] + 1 : 0);
        else if ((op >= 41 && op <= 46) || (op >= 48 && op <= 52)) { /* constants, spec constants */
            if (len >= 3 && w[1] < bound) s.uv[w[1]] = 1;
        } else if (op == OP_VARIABLE && !other_fn && i < start && len >= 4 && w[1] < bound) {
            uint32_t sc = w[2];
            if (sc == SC_UNIFORM || sc == SC_PUSH_CONSTANT || sc == SC_UNIFORM_CONSTANT || (sc == SC_INPUT && (s.info[w[1]] & I_UBUILTIN)))
                s.kind[w[1]] = K_UPTR, s.uv[w[1]] = 1;
            else if (sc == SC_PRIVATE)
                s.kind[w[1]] = K_PVAR_PTR, s.root[w[1]] = w[1], s.uv[w[1]] = 1, s.var_u[w[1]] = 1, s.info[w[1]] |= I_VAR;
        } else if (other_fn && len >= 3) {
            /* another function: its pointers into Private variables, and the variables it writes */
            if ((op == OP_ACCESS_CHAIN || op == OP_IN_BOUNDS_ACCESS_CHAIN || op == OP_PTR_ACCESS_CHAIN ||
                 op == OP_IN_BOUNDS_PTR_ACCESS_CHAIN || op == OP_COPY_OBJECT) &&
                len >= 4 && w[1] < bound && w[2] < bound && s.kind[w[2]] == K_PVAR_PTR)
                s.kind[w[1]] = K_PVAR_PTR, s.root[w[1]] = s.root[w[2]];
            else if (op == OP_STORE && w[0] < bound && s.kind[w[0]] == K_PVAR_PTR)
                s.info[s.root[w[0]]] |= I_PVAR_OTHER;
        }
        i += len;
    }
    free(ptr_sc);
    for (uint32_t id = 1; id < bound; id++)
        if (s.info[id] & I_PVAR_OTHER) s.var_u[id] = 0;
    /* the other functions' pointer records are not this one's */
    for (uint32_t id = 1; id < bound; id++)
        if (s.kind[id] == K_PVAR_PTR && !(s.info[id] & I_VAR)) s.kind[id] = K_NONE, s.root[id] = 0;

    /* 2: the function's constructs, blocks and candidate values */
    int cur_block = -1;
    for (size_t i = start; i < end;) {
        uint32_t op = code[i] & 0xffff, len = code[i] >> 16;
        const uint32_t *w = code + i + 1;
        if (op == OP_LABEL && len >= 2 && w[0] < bound) {
            while (sn && u->c[stack[sn - 1]].merge == w[0]) {
                u->c[stack[sn - 1]].to = i;
                sn--;
            }
            for (unsigned k = 0; k < sn; k++)
                if (u->c[stack[k]].merge == w[0]) goto out; /* a merge block out of order */
            if (!grow((void **)&b, &bcap, nb, sizeof(*b))) goto out;
            b[nb] = (block){w[0], sn ? stack[sn - 1] : -1, 0};
            s.blk[w[0]] = (int32_t)nb;
            cur_block = (int)nb++;
        } else if ((op == OP_SELECTION_MERGE || op == OP_LOOP_MERGE) && len >= 3 && cur_block >= 0 && w[0] < bound) {
            if (!grow((void **)&u->c, &ccap, u->nc, sizeof(*u->c)) || !grow((void **)&stack, &scap, sn, sizeof(*stack))) goto out;
            int k = (int)u->nc++;
            construct *c = &u->c[k];
            *c = (construct){b[cur_block].label, w[0], 0, sn ? stack[sn - 1] : -1, op == OP_LOOP_MERGE, 1, 0, 0, 0};
            uint32_t nop = code[i + len] & 0xffff;
            if (i + len < end && (nop == OP_BRANCH_CONDITIONAL || nop == OP_SWITCH)) c->cond = code[i + len + 1];
            if (c->loop) { /* the header block is inside its loop */
                c->from = 0;
                for (size_t j = start; j < i; j += code[j] >> 16) /* (its OpLabel: the block's start) */
                    if ((code[j] & 0xffff) == OP_LABEL && code[j + 1] == c->header) c->from = j;
                b[cur_block].cons = k;
            } else
                c->from = i;
            if (c->header < bound) u->hdr[c->header] = k;
            s.mrg[w[0]] = k;
            stack[sn++] = k;
        } else if (op >= OP_BRANCH && op <= OP_UNREACHABLE) {
            if (cur_block >= 0) b[cur_block].term = i;
            if (op == OP_KILL || op == OP_RETURN || op == OP_RETURN_VALUE || op == OP_UNREACHABLE)
                for (int k = sn ? stack[sn - 1] : -1; k >= 0; k = u->c[k].parent) u->c[k].ret = 1;
        } else if (op == OP_TERMINATE_INVOCATION) {
            if (cur_block >= 0) b[cur_block].term = i;
            for (int k = sn ? stack[sn - 1] : -1; k >= 0; k = u->c[k].parent) u->c[k].ret = 1;
        } else if (op == OP_VARIABLE && len >= 4 && w[1] < bound && w[2] == SC_FUNCTION) {
            s.kind[w[1]] = K_PVAR_PTR, s.root[w[1]] = w[1], s.uv[w[1]] = 1, s.var_u[w[1]] = 1, s.info[w[1]] |= I_VAR;
        } else if (given && len >= 3 && w[1] < bound && given[w[1]]) {
            s.uv[w[1]] = 1; /* uniform by the caller's word (never lowered) */
        } else {
            unsigned f, c;
            if (candidate(op, len, &f, &c) && len >= 3 && w[1] < bound) s.info[w[1]] |= I_CAND, s.uv[w[1]] = 1;
        }
        i += len;
    }
    if (sn) goto out; /* a construct still open */
    for (unsigned k = 0; k < u->nc; k++)
        if (u->c[k].loop && u->c[k].ret) u->c[k].uniform = 0;

    /* 3: lower until nothing changes */
    for (int round = 0, changed = 1; changed; round++) {
        if (round > 64) goto out;
        changed = 0;
        int blk = -1;
#define LOWER(x)                                                                                                       \
    do {                                                                                                               \
        if (x) x = 0, changed = 1;                                                                                     \
    } while (0)
        for (size_t i = start; i < end;) {
            uint32_t op = code[i] & 0xffff, len = code[i] >> 16;
            const uint32_t *w = code + i + 1;
            i += len;
            if (op == OP_LABEL) {
                blk = s.blk[w[0]];
                continue;
            }
            if (op == OP_STORE && len >= 3 && w[0] < bound && s.kind[w[0]] == K_PVAR_PTR) {
                uint32_t v = s.root[w[0]];
                int okst = s.uv[w[0]] && w[1] < bound && s.uv[w[1]] && blk >= 0 && depth_uniform(u->c, b[blk].cons);
                if (!okst) LOWER(s.var_u[v]);
                continue;
            }
            if ((op == OP_COPY_MEMORY || op == OP_COPY_MEMORY_SIZED) && len >= 3 && w[0] < bound && s.kind[w[0]] == K_PVAR_PTR) {
                LOWER(s.var_u[s.root[w[0]]]);
                continue;
            }
            if (op == OP_FUNCTION_CALL) { /* pointers handed to a call: what it writes is not known */
                for (uint32_t k = 3; k < len - 1; k++)
                    if (w[k] < bound && s.kind[w[k]] == K_PVAR_PTR) LOWER(s.var_u[s.root[w[k]]]);
                continue;
            }
            if (op >= 227 && op <= 242 && len >= 4) { /* atomics on a variable */
                uint32_t p = op == 228 ? w[0] : w[2];
                if (p < bound && s.kind[p] == K_PVAR_PTR) LOWER(s.var_u[s.root[p]]);
                continue;
            }
            if (len < 3 || w[1] >= bound || !(s.info[w[1]] & I_CAND) || !s.uv[w[1]]) continue;
            uint32_t r = w[1];
            unsigned f, c;
            candidate(op, len, &f, &c);
            int all = 1;
            if (op == OP_PHI) {
                for (uint32_t k = 2; k + 1 < len; k += 2) all &= w[k] < bound && s.uv[w[k]];
                if (all && len > 5) { /* more than one way in: where they meet must be uniform */
                    uint32_t lbl = blk >= 0 ? b[blk].label : 0;
                    int h = lbl ? u->hdr[lbl] : -1, m = lbl ? s.mrg[lbl] : -1;
                    int hl = h >= 0 && u->c[h].loop;
                    if (!hl && m < 0) all = 0;
                    if (hl && !u->c[h].uniform) all = 0;
                    if (m >= 0 && !u->c[m].uniform) all = 0;
                }
            } else {
                uint32_t last = c ? f + c : len - 1;
                for (uint32_t k = f; k < last && k < len - 1; k++) all &= w[k] < bound && s.uv[w[k]];
                if (op == OP_LOAD) {
                    uint32_t p = w[2];
                    if (p >= bound || s.kind[p] == K_NONE) all = 0;
                    else if (s.kind[p] == K_PVAR_PTR && !s.var_u[s.root[p]]) all = 0;
                }
            }
            /* pointers keep where they point */
            if ((op == OP_ACCESS_CHAIN || op == OP_IN_BOUNDS_ACCESS_CHAIN || op == OP_PTR_ACCESS_CHAIN ||
                 op == OP_IN_BOUNDS_PTR_ACCESS_CHAIN || op == OP_COPY_OBJECT) &&
                w[2] < bound && s.kind[w[2]] != K_NONE && s.kind[r] == K_NONE) {
                s.kind[r] = s.kind[w[2]], s.root[r] = s.root[w[2]];
                changed = 1;
            }
            if (!all) LOWER(s.uv[r]);
        }
        /* constructs */
        for (unsigned k = 0; k < u->nc; k++) {
            construct *c = &u->c[k];
            if (!c->uniform || c->loop) continue;
            if (!c->cond || c->cond >= bound || !s.uv[c->cond]) LOWER(c->uniform);
        }
        for (unsigned k = 0; k < u->nc; k++) {
            construct *c = &u->c[k];
            if (!c->uniform || !c->loop) continue;
            for (unsigned j = 0; j < nb && c->uniform; j++) {
                if (!b[j].term || !in_construct(u->c, b[j].cons, (int)k)) continue;
                size_t t = b[j].term;
                uint32_t op = code[t] & 0xffff, len = code[t] >> 16;
                const uint32_t *w = code + t + 1;
                int exits = 0;
                if (op == OP_BRANCH) exits = w[0] == c->merge || w[0] == c->header;
                else if (op == OP_BRANCH_CONDITIONAL && len >= 4) exits = w[1] == c->merge || w[2] == c->merge || w[1] == c->header || w[2] == c->header;
                else if (op == OP_SWITCH && len >= 3) {
                    exits = w[1] == c->merge || w[1] == c->header;
                    for (uint32_t q = 3; q < len - 1; q += 2) exits |= w[q] == c->merge || w[q] == c->header;
                }
                if (!exits) continue;
                if (!uniform_up_to(u->c, b[j].cons, (int)k)) LOWER(c->uniform);
                else if ((op == OP_BRANCH_CONDITIONAL || op == OP_SWITCH) && (w[0] >= bound || !s.uv[w[0]])) LOWER(c->uniform);
            }
        }
#undef LOWER
    }
    ok = 1;
out:
    free(b), free(stack);
    free(s.uv), free(s.kind), free(s.info), free(s.var_u), free(s.root), free(s.mrg), free(s.blk);
    if (!ok) {
        vkb_uniform_free(u);
        return NULL;
    }
    return u;
}

int vkb_uniform_construct(const vkb_uniform *u, uint32_t label, size_t *from, size_t *to)
{
    if (!u || label >= u->bound || u->hdr[label] < 0) return 0;
    const construct *c = &u->c[u->hdr[label]];
    if (from) *from = c->from;
    if (to) *to = c->to;
    return c->uniform;
}

void vkb_uniform_counts(const vkb_uniform *u, unsigned *constructs, unsigned *uniform)
{
    *constructs = u ? u->nc : 0, *uniform = 0;
    for (unsigned k = 0; u && k < u->nc; k++) *uniform += u->c[k].uniform;
}

void vkb_uniform_free(vkb_uniform *u)
{
    if (!u) return;
    free(u->c);
    free(u->hdr);
    free(u);
}
