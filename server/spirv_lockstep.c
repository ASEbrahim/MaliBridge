/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * lockstep: strided loops of a 32-thread compute shader given a workgroup-uniform trip count
 * (see spirv_lockstep.h).
 *
 * 1  one pass over the module: the GLCompute entry point and its local size, the
 *    LocalInvocationId variable and the values that are its x, 32-bit unsigned type and constants,
 *    Workgroup pointers, where each id is defined, the OpIAdds;
 * 2  each loop (OpLoopMerge) of the entry point's function, in layout order up to its merge
 *    block: the induction phi, the Workgroup accesses, the one way out and its test;
 * 3  the new code: a constant 0 (when the module has none), each new phi after the old one, its
 *    increment after the old one's, the exit test's operand changed.
 */
#include "spirv_lockstep.h"

#include <stdlib.h>
#include <string.h>

enum {
    OP_DECORATE = 71, OP_ENTRY_POINT = 15, OP_EXECUTION_MODE = 16, OP_EXECUTION_MODE_ID = 331, OP_TYPE_INT = 21,
    OP_TYPE_POINTER = 32, OP_CONSTANT = 43, OP_FUNCTION = 54, OP_FUNCTION_END = 56, OP_VARIABLE = 59, OP_LOAD = 61,
    OP_STORE = 62, OP_COPY_MEMORY = 63, OP_COPY_MEMORY_SIZED = 64, OP_ACCESS_CHAIN = 65, OP_IN_BOUNDS_ACCESS_CHAIN = 66,
    OP_PTR_ACCESS_CHAIN = 67, OP_IN_BOUNDS_PTR_ACCESS_CHAIN = 70, OP_COMPOSITE_EXTRACT = 81, OP_COPY_OBJECT = 83, OP_IADD = 128,
    OP_UGREATER_THAN_EQUAL = 174, OP_ULESS_THAN_EQUAL = 178, OP_ATOMIC_LOAD = 227, OP_ATOMIC_STORE = 228,
    OP_ATOMIC_FLAG_CLEAR = 319, OP_PHI = 245, OP_LOOP_MERGE = 246, OP_LABEL = 248, OP_BRANCH = 249,
    OP_BRANCH_CONDITIONAL = 250, OP_SWITCH = 251, OP_KILL = 252, OP_RETURN = 253, OP_RETURN_VALUE = 254, OP_UNREACHABLE = 255,
    OP_TERMINATE_INVOCATION = 4416,
};
enum { EXEC_GLCOMPUTE = 5, MODE_LOCAL_SIZE = 17, MODE_LOCAL_SIZE_ID = 38, BI_LOCAL_INVOCATION_ID = 27, SC_WORKGROUP = 4 };
enum { F_UINT_TYPE = 1, F_CONST = 2, F_LIID = 4, F_LIID_X_PTR = 8, F_LIID_VEC = 16, F_TID = 32, F_WG_TYPE = 64, F_WG = 128 };

#define MAX_BOUND (1u << 22)
#define MAX_LOOPS 32

typedef struct {
    uint8_t flags;
    uint32_t value; /* F_CONST */
    size_t def;     /* offset of the defining instruction (0: none / global) */
} idi;

typedef struct {
    size_t phi_at, add_at, cmp_operand; /* where the new phi and its increment go, the test's operand word */
    uint32_t pre_label, back_label;      /* the phi's incoming blocks: from before the loop, from the back edge */
    uint32_t phi_new, add_new;
} edit;

static int wg_access(const idi *ii, uint32_t bound, uint32_t op, const uint32_t *w, uint32_t len)
{
#define WG(k) ((k) < len - 1 && w[k] < bound && (ii[w[k]].flags & F_WG))
    switch (op) {
    case OP_LOAD:
    case OP_ATOMIC_LOAD:
        return WG(2);
    case OP_STORE:
    case OP_ATOMIC_STORE:
    case OP_ATOMIC_FLAG_CLEAR:
        return WG(0);
    case OP_COPY_MEMORY:
    case OP_COPY_MEMORY_SIZED:
        return WG(0) || WG(1);
    default:
        return op >= 229 && op <= 242 ? WG(2) : (op == 318 || op == 5614 || op == 5615 || op == 6035) ? WG(2) : 0;
    }
#undef WG
}

static int is_exit_op(uint32_t op)
{
    return op == OP_RETURN || op == OP_RETURN_VALUE || op == OP_KILL || op == OP_UNREACHABLE || op == OP_TERMINATE_INVOCATION;
}

int vkb_spv_lockstep_loops(const uint32_t *code, size_t size, uint32_t **out, size_t *out_size, unsigned *loops, const char **why)
{
    *out = NULL, *out_size = 0, *loops = 0, *why = NULL;
    size_t n = size / 4;
    if (n < 5 || (size & 3) || code[0] != 0x07230203u) return 0;
    uint32_t bound = code[3];
    if (!bound || bound > MAX_BOUND) {
        *why = "id bound out of range";
        return -1;
    }
    idi *ii = calloc(bound, sizeof(*ii));
    if (!ii) {
        *why = "out of memory";
        return -1;
    }
    int rc = 0;
    uint32_t entry = 0, ls[3] = {0, 0, 0}, ls_id[3] = {0, 0, 0}, uint_t = 0, zero = 0;
    int nentries = 0;
    size_t first_fn = 0;
    edit ed[MAX_LOOPS];
    unsigned ne = 0;

    /* 1 */
    for (size_t i = 5; i < n;) {
        uint32_t op = code[i] & 0xffff, len = code[i] >> 16;
        if (!len || i + len > n) {
            *why = "malformed instruction stream";
            rc = -1;
            goto out;
        }
        const uint32_t *w = code + i + 1;
        uint32_t nw = len - 1;
        switch (op) {
        case OP_ENTRY_POINT:
            if (nw >= 2 && w[0] == EXEC_GLCOMPUTE) entry = w[1], nentries++;
            break;
        case OP_EXECUTION_MODE:
            if (nw >= 5 && w[1] == MODE_LOCAL_SIZE) ls[0] = w[2], ls[1] = w[3], ls[2] = w[4];
            break;
        case OP_EXECUTION_MODE_ID:
            if (nw >= 5 && w[1] == MODE_LOCAL_SIZE_ID) ls_id[0] = w[2], ls_id[1] = w[3], ls_id[2] = w[4];
            break;
        case OP_DECORATE:
            if (nw >= 3 && w[1] == 11 && w[2] == BI_LOCAL_INVOCATION_ID && w[0] < bound) ii[w[0]].flags |= F_LIID;
            break;
        case OP_TYPE_INT:
            if (nw >= 3 && w[1] == 32 && w[2] == 0 && w[0] < bound) {
                ii[w[0]].flags |= F_UINT_TYPE;
                if (!uint_t) uint_t = w[0];
            }
            break;
        case OP_TYPE_POINTER:
            if (nw >= 3 && w[1] == SC_WORKGROUP && w[0] < bound) ii[w[0]].flags |= F_WG_TYPE;
            break;
        case OP_CONSTANT:
            if (!first_fn && nw == 3 && w[0] < bound && w[1] < bound && (ii[w[0]].flags & F_UINT_TYPE)) {
                ii[w[1]].flags |= F_CONST, ii[w[1]].value = w[2];
                if (!zero && w[2] == 0 && w[0] == uint_t) zero = w[1];
            }
            break;
        case OP_FUNCTION:
            if (!first_fn) first_fn = i;
            break;
        default:
            break;
        }
        /* Workgroup pointers; LocalInvocationId.x */
        switch (op) {
        case OP_VARIABLE:
        case OP_ACCESS_CHAIN:
        case OP_IN_BOUNDS_ACCESS_CHAIN:
        case OP_PTR_ACCESS_CHAIN:
        case OP_IN_BOUNDS_PTR_ACCESS_CHAIN:
        case OP_COPY_OBJECT:
        case OP_PHI:
            if (nw >= 2 && w[0] < bound && w[1] < bound && (ii[w[0]].flags & F_WG_TYPE)) ii[w[1]].flags |= F_WG;
            break;
        default:
            break;
        }
        if ((op == OP_ACCESS_CHAIN || op == OP_IN_BOUNDS_ACCESS_CHAIN) && nw == 4 && w[1] < bound && w[2] < bound && w[3] < bound &&
            (ii[w[2]].flags & F_LIID) && (ii[w[3]].flags & F_CONST) && ii[w[3]].value == 0)
            ii[w[1]].flags |= F_LIID_X_PTR;
        if (op == OP_LOAD && nw >= 3 && w[1] < bound && w[2] < bound) {
            if (ii[w[2]].flags & F_LIID_X_PTR) ii[w[1]].flags |= F_TID;
            if (ii[w[2]].flags & F_LIID) ii[w[1]].flags |= F_LIID_VEC;
        }
        if (op == OP_COMPOSITE_EXTRACT && nw == 4 && w[1] < bound && w[2] < bound && (ii[w[2]].flags & F_LIID_VEC) && w[3] == 0)
            ii[w[1]].flags |= F_TID;
        /* where each value of the functions is defined: an instruction's w[1] is its result, or (in
         * one without a result) an id it uses, which comes after the id's definition in a
         * structured function's layout: the first is the definition. Branches and merges name
         * blocks further on, and are left out. */
        if (first_fn && nw >= 2 && w[1] < bound && !ii[w[1]].def && op != OP_LOOP_MERGE && op != OP_BRANCH &&
            op != OP_BRANCH_CONDITIONAL && op != OP_SWITCH && op != OP_LABEL && op != OP_FUNCTION_END)
            ii[w[1]].def = i;
        if (op == OP_LABEL && nw >= 1 && w[0] < bound) ii[w[0]].def = i;
        i += len;
    }
    for (int k = 0; k < 3; k++)
        if (ls_id[k] && ls_id[k] < bound && (ii[ls_id[k]].flags & F_CONST)) ls[k] = ii[ls_id[k]].value;
    if (nentries != 1 || !first_fn || !uint_t || ls[0] < 2 || ls[1] != 1 || ls[2] != 1) goto out;
    uint32_t W = ls[0];

    /* 2: the entry point's function */
    size_t fstart = 0, fend = 0;
    for (size_t i = first_fn; i < n; i += code[i] >> 16) {
        uint32_t op = code[i] & 0xffff;
        if (op == OP_FUNCTION && !fstart && code[i + 2] == entry) fstart = i;
        else if (op == OP_FUNCTION_END && fstart) {
            fend = i;
            break;
        }
    }
    if (!fstart) goto out;
    size_t label_at = 0;
    for (size_t i = fstart; i < fend; i += code[i] >> 16) {
        uint32_t op = code[i] & 0xffff, len = code[i] >> 16;
        const uint32_t *w = code + i + 1;
        if (op == OP_LABEL) label_at = i;
        if (op != OP_LOOP_MERGE || len < 3) continue;
        uint32_t merge = w[0];
        if (merge >= bound || !ii[merge].def || ii[merge].def <= i) continue;
        size_t lend = ii[merge].def;
        /* the induction phi: i = OpPhi(tid, pre; i + W, back) in the header */
        size_t phi_at = 0, add_at = 0;
        uint32_t phi = 0, pre = 0, back = 0;
        for (size_t k = label_at + 2; k < i; k += code[k] >> 16) {
            uint32_t o = code[k] & 0xffff, l = code[k] >> 16;
            const uint32_t *p = code + k + 1;
            if (o != OP_PHI || l != 7 || p[0] != uint_t) continue;
            for (int s = 0; s < 2; s++) {
                uint32_t t = p[2 + 2 * s], x = p[4 - 2 * s];
                if (t >= bound || x >= bound || !(ii[t].flags & F_TID) || !ii[x].def) continue;
                const uint32_t *a = code + ii[x].def + 1;
                if ((code[ii[x].def] & 0xffff) != OP_IADD || (code[ii[x].def] >> 16) != 5) continue;
                uint32_t other = a[2] == p[1] ? a[3] : a[3] == p[1] ? a[2] : 0;
                if (!other || other >= bound || !(ii[other].flags & F_CONST) || ii[other].value != W) continue;
                phi = p[1], phi_at = k, add_at = ii[x].def, pre = p[3 + 2 * s], back = p[5 - 2 * s];
            }
            if (phi) break;
        }
        if (!phi) continue;
        /* the loop's body (layout order, header to merge): its Workgroup accesses, its ways out */
        size_t last_wg = 0, test_at = 0, cmp_operand = 0;
        int bad = 0, exits = 0;
        uint32_t blk = 0, exit_blk = 0;
        size_t blk_at = 0;
        for (size_t k = label_at; k < lend && !bad; k += code[k] >> 16) {
            uint32_t o = code[k] & 0xffff, l = code[k] >> 16;
            const uint32_t *p = code + k + 1;
            if (o == OP_LABEL) blk = p[0], blk_at = k;
            if (wg_access(ii, bound, o, p, l)) last_wg = k;
            if (is_exit_op(o)) bad = 1;
            if (o == OP_BRANCH && p[0] == merge) {
                /* a block that is only the way out */
                if (k != blk_at + 2 || exit_blk) bad = 1;
                exit_blk = blk, exits++;
            }
            if (o == OP_BRANCH_CONDITIONAL && l >= 4 && (p[1] == merge || p[2] == merge)) bad = 1;
            if (o == OP_SWITCH)
                for (uint32_t q = 1; q < l - 1; q += 2)
                    if (p[q] == merge) bad = 1;
        }
        if (bad || exits != 1 || !last_wg) continue;
        /* the test that leads there: OpBranchConditional c exit_blk other, c = (n <= i) or (i >= n) */
        int ntests = 0;
        for (size_t k = label_at; k < lend; k += code[k] >> 16) {
            uint32_t o = code[k] & 0xffff, l = code[k] >> 16;
            const uint32_t *p = code + k + 1;
            if (o == OP_BRANCH && p[0] == exit_blk) bad = 1;
            if (o == OP_SWITCH)
                for (uint32_t q = 1; q < l - 1; q += 2)
                    if (p[q] == exit_blk) bad = 1;
            if (o != OP_BRANCH_CONDITIONAL || l < 4 || (p[1] != exit_blk && p[2] != exit_blk)) continue;
            ntests++;
            if (p[1] != exit_blk || p[0] >= bound || !ii[p[0]].def) {
                bad = 1;
                continue;
            }
            size_t c = ii[p[0]].def;
            uint32_t co = code[c] & 0xffff;
            const uint32_t *cw = code + c + 1;
            if ((code[c] >> 16) != 5) {
                bad = 1;
                continue;
            }
            /* the test is used by the branch alone (it changes for every invocation but 0) */
            unsigned uses = 0;
            for (size_t u = fstart; u < fend; u += code[u] >> 16)
                for (uint32_t q = 1; q < (code[u] >> 16); q++) uses += code[u + q] == p[0] && u != c;
            if (uses != 1) {
                bad = 1;
                continue;
            }
            uint32_t nv = 0;
            if (co == OP_ULESS_THAN_EQUAL && cw[3] == phi) nv = cw[2], cmp_operand = c + 4;
            else if (co == OP_UGREATER_THAN_EQUAL && cw[2] == phi) nv = cw[3], cmp_operand = c + 3;
            /* n: from before the loop */
            if (!nv || nv >= bound || !((ii[nv].flags & F_CONST) || (ii[nv].def && ii[nv].def < label_at))) bad = 1;
            test_at = k;
        }
        if (bad || ntests != 1 || test_at < last_wg) continue;
        if (ne == MAX_LOOPS) break;
        ed[ne++] = (edit){phi_at, add_at, cmp_operand, pre, back, 0, 0};
    }
    if (!ne) goto out;

    /* 3 */
    uint32_t next = bound;
    uint32_t pre_c[4];
    unsigned npre = 0;
    if (!zero) {
        zero = next++;
        pre_c[npre++] = (4u << 16) | OP_CONSTANT, pre_c[npre++] = uint_t, pre_c[npre++] = zero, pre_c[npre++] = 0;
    }
    for (unsigned k = 0; k < ne; k++) {
        ed[k].phi_new = next++;
        ed[k].add_new = next++;
    }
    if (next > MAX_BOUND) {
        *why = "id bound out of range";
        rc = -1;
        goto out;
    }
    uint32_t *o = malloc((n + npre + 12 * ne) * 4);
    if (!o) {
        *why = "out of memory";
        rc = -1;
        goto out;
    }
    memcpy(o, code, first_fn * 4);
    size_t p = first_fn;
    memcpy(o + p, pre_c, npre * 4);
    p += npre;
    for (size_t i = first_fn; i < n;) {
        uint32_t len = code[i] >> 16;
        size_t at = p;
        memcpy(o + p, code + i, len * 4);
        p += len;
        for (unsigned k = 0; k < ne; k++) {
            if (ed[k].cmp_operand >= i && ed[k].cmp_operand < i + len) o[at + (ed[k].cmp_operand - i)] = ed[k].phi_new;
            if (ed[k].phi_at == i) {
                o[p++] = (7u << 16) | OP_PHI, o[p++] = uint_t, o[p++] = ed[k].phi_new;
                o[p++] = zero, o[p++] = ed[k].pre_label, o[p++] = ed[k].add_new, o[p++] = ed[k].back_label;
            }
            if (ed[k].add_at == i) {
                const uint32_t *a = code + i + 1;
                uint32_t c = (ii[a[2]].flags & F_CONST) && ii[a[2]].value == W ? a[2] : a[3];
                o[p++] = (5u << 16) | OP_IADD, o[p++] = uint_t, o[p++] = ed[k].add_new, o[p++] = ed[k].phi_new, o[p++] = c;
            }
        }
        i += len;
    }
    o[3] = next;
    *out = o;
    *out_size = p * 4;
    *loops = ne;
    rc = 1;
out:
    free(ii);
    return rc;
}
