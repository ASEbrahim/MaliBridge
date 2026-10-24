/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * shm_barrier: barriers between shared-memory writes and reads in compute shaders (see
 * spirv_shmbarrier.h).
 *
 * 1  one pass over the module: the GLCompute entry points, Workgroup pointer types and the values
 *    of that type, 32-bit integer types and constants, and per function whether it reads or writes
 *    Workgroup memory (calls followed to a fixed point);
 * 2  each entry point's function walked in order, with a stack of the merge blocks of the open
 *    constructs: where barriers go (input offsets);
 * 3  the new code: the two constants it needs (when the module has none) right before the first
 *    OpFunction, each barrier before the instruction it was put at.
 */
#include "spirv_shmbarrier.h"
#include "spirv_uniform.h"

#include <stdlib.h>
#include <string.h>

enum {
    OP_CAPABILITY = 17, OP_DECORATE = 71, OP_ENTRY_POINT = 15, OP_TYPE_INT = 21, OP_TYPE_POINTER = 32, OP_CONSTANT = 43, OP_FUNCTION = 54,
    OP_FUNCTION_PARAMETER = 55, OP_FUNCTION_END = 56, OP_FUNCTION_CALL = 57, OP_VARIABLE = 59, OP_LOAD = 61,
    OP_STORE = 62, OP_COPY_MEMORY = 63, OP_COPY_MEMORY_SIZED = 64, OP_ACCESS_CHAIN = 65, OP_IN_BOUNDS_ACCESS_CHAIN = 66,
    OP_PTR_ACCESS_CHAIN = 67, OP_IN_BOUNDS_PTR_ACCESS_CHAIN = 70, OP_COPY_OBJECT = 83, OP_SELECT = 169,
    OP_CONTROL_BARRIER = 224, OP_MEMORY_BARRIER = 225, OP_ATOMIC_LOAD = 227, OP_ATOMIC_STORE = 228,
    OP_ATOMIC_EXCHANGE = 229, OP_ATOMIC_XOR = 242, OP_PHI = 245, OP_LOOP_MERGE = 246, OP_SELECTION_MERGE = 247,
    OP_LABEL = 248, OP_BRANCH = 249, OP_BRANCH_CONDITIONAL = 250, OP_SWITCH = 251, OP_KILL = 252, OP_RETURN = 253,
    OP_RETURN_VALUE = 254, OP_UNREACHABLE = 255, OP_ATOMIC_FLAG_TEST_AND_SET = 318, OP_ATOMIC_FLAG_CLEAR = 319,
    OP_TERMINATE_INVOCATION = 4416, OP_ATOMIC_FMIN = 5614, OP_ATOMIC_FMAX = 5615, OP_ATOMIC_FADD = 6035,
};

enum { SC_WORKGROUP = 4, EXEC_GLCOMPUTE = 5, SCOPE_WORKGROUP = 2, SEM_ACQREL_WG = 0x8 | 0x100, SEM_WG_MEMORY = 0x100 };

#define MAX_BOUND (1u << 22)
#define NONE ((size_t)-1)

enum { F_WG_PTR_TYPE = 1, F_WG_PTR = 2, F_INT32_TYPE = 4, F_CONST32 = 8, F_UINT32_TYPE = 16, F_LOOP_HEADER = 32, F_ENTRY = 64 };
enum { A_READ = 1, A_WRITE = 2 };

typedef struct {
    uint8_t flags;
    uint8_t access; /* a function: A_* it makes, itself or through its calls */
    uint32_t value; /* F_CONST32: its value */
} id_info;

typedef struct {
    size_t pos;   /* the input offset the barrier goes before */
    uint8_t kind; /* 1 control, 2 memory */
} insert;

typedef struct {
    insert *v;
    size_t n, cap;
} inserts;

static int add_insert(inserts *l, size_t pos, uint8_t kind)
{
    for (size_t i = l->n; i-- > 0;)
        if (l->v[i].pos == pos) {
            if (kind < l->v[i].kind) l->v[i].kind = kind; /* a control barrier covers a memory one */
            return 1;
        }
    if (l->n == l->cap) {
        size_t cap = l->cap ? l->cap * 2 : 16;
        insert *v = realloc(l->v, cap * sizeof(*v));
        if (!v) return 0;
        l->v = v, l->cap = cap;
    }
    l->v[l->n++] = (insert){pos, kind};
    return 1;
}

static int cmp_insert(const void *a, const void *b)
{
    const insert *x = a, *y = b;
    return x->pos < y->pos ? -1 : x->pos > y->pos;
}

/* What an instruction does to Workgroup memory (A_*) */
static unsigned access_of(const id_info *ii, uint32_t bound, uint32_t op, const uint32_t *w, uint32_t len)
{
#define WG(k) ((k) < len - 1 && w[k] < bound && (ii[w[k]].flags & F_WG_PTR))
    switch (op) {
    case OP_LOAD:
    case OP_ATOMIC_LOAD:
        return WG(2) ? A_READ : 0;
    case OP_STORE:
    case OP_ATOMIC_STORE:
    case OP_ATOMIC_FLAG_CLEAR:
        return WG(0) ? A_WRITE : 0;
    case OP_COPY_MEMORY:
    case OP_COPY_MEMORY_SIZED:
        return (WG(0) ? A_WRITE : 0) | (WG(1) ? A_READ : 0);
    case OP_ATOMIC_FLAG_TEST_AND_SET:
    case OP_ATOMIC_FMIN:
    case OP_ATOMIC_FMAX:
    case OP_ATOMIC_FADD:
        return WG(2) ? A_READ | A_WRITE : 0;
    case OP_FUNCTION_CALL:
        return len >= 4 && w[2] < bound ? ii[w[2]].access : 0;
    default:
        if (op >= OP_ATOMIC_EXCHANGE && op <= OP_ATOMIC_XOR) return WG(2) ? A_READ | A_WRITE : 0;
        return 0;
    }
#undef WG
}

/* A barrier's effect: bit 0 it synchronizes execution across the workgroup, bit 1 it orders
 * Workgroup memory. Operands that are not known constants count as both. */
static unsigned barrier_of(const id_info *ii, uint32_t bound, uint32_t op, const uint32_t *w, uint32_t len)
{
#define CV(k, v) ((w[k] < bound && (ii[w[k]].flags & F_CONST32)) ? (v = ii[w[k]].value, 1) : 0)
    uint32_t v = 0;
    if (op == OP_CONTROL_BARRIER && len == 4) {
        unsigned r = 0;
        if (!CV(0, v) || v <= SCOPE_WORKGROUP) r |= 1;
        if (!CV(2, v) || (v & SEM_WG_MEMORY)) r |= 2;
        return r;
    }
    if (op == OP_MEMORY_BARRIER && len == 3) return !CV(1, v) || (v & SEM_WG_MEMORY) ? 2 : 0;
    return 0;
#undef CV
}

/* Pending accesses since the last barrier: for the execution barrier and for the memory one */
enum { P_XR = 1, P_XW = 2, P_MR = 4, P_MW = 8, P_X = P_XR | P_XW, P_M = P_MR | P_MW };

/* An access (A_*) that conflicts with the pending ones of a kind (P_X or P_M): a read after a
 * write, or a write after a read */
static int conflict(unsigned a, unsigned pend, unsigned kind)
{
    unsigned r = kind == P_X ? P_XR : P_MR, wr = kind == P_X ? P_XW : P_MW;
    return ((a & A_READ) && (pend & wr)) || ((a & A_WRITE) && (pend & r));
}

static unsigned pend_of(unsigned a)
{
    return (a & A_READ ? P_XR | P_MR : 0) | (a & A_WRITE ? P_XW | P_MW : 0);
}

/* The accesses (A_*) made in code[from, to) */
static unsigned access_in(const uint32_t *code, size_t from, size_t to, const id_info *ii, uint32_t bound)
{
    unsigned a = 0;
    for (size_t i = from; i < to; i += code[i] >> 16) a |= access_of(ii, bound, code[i] & 0xffff, code + i + 1, code[i] >> 16);
    return a;
}

/* One open construct of the walk (the function's top level is region 0) */
typedef struct {
    uint32_t merge;  /* its merge block (0 until a loop's OpLoopMerge) */
    int eff;         /* every invocation of the workgroup that gets here gets here together */
    size_t entry;    /* where a control barrier before it goes: before this construct, or (when
                        that is not allowed) before the one around it that has such a place */
    size_t owner;    /* the region that place is the entry of */
    int done;        /* a barrier was put at this region's own entry */
    unsigned far;    /* P_* pending from before `entry` */
    unsigned near;   /* P_* pending from after `entry`, before this construct */
    unsigned cur;    /* P_* pending from inside it */
} region;

typedef struct {
    region *v;
    size_t n, cap;
} regions;

/* A construct begins, headed by `hdr`; entry: where a barrier before it would go (in the region
 * outside), or NONE */
static int push(regions *rs, const uint32_t *code, uint32_t hdr, int loop, size_t entry, int diverged, const vkb_uniform *u,
                const id_info *ii, uint32_t bound)
{
    if (rs->n == rs->cap) {
        region *nv = realloc(rs->v, rs->cap * 2 * sizeof(*nv));
        if (!nv) return 0;
        rs->v = nv, rs->cap *= 2;
    }
    region *pr = &rs->v[rs->n - 1];
    size_t from = 0, to = 0;
    int cu = u && vkb_uniform_construct(u, hdr, &from, &to);
    int ok_out = pr->eff && !diverged && entry != NONE; /* a barrier is allowed right before the construct */
    unsigned all = pr->far | pr->near | pr->cur;
    region nr = {0, pr->eff && !diverged && cu, NONE, rs->n, 0, 0, all, 0};
    if (ok_out)
        nr.entry = entry, nr.far = all, nr.near = 0;
    else if (pr->entry != NONE && !rs->v[pr->owner].done) /* the place before the construct around it */
        nr.entry = pr->entry, nr.owner = pr->owner, nr.far = pr->far, nr.near = pr->near | pr->cur;
    pr->far = pr->near = pr->cur = 0;
    /* a loop (level 2): what one iteration does is pending at the start of the next */
    if (loop && u && to > from) nr.cur = pend_of(access_in(code, from, to, ii, bound));
    rs->v[rs->n++] = nr;
    return 1;
}

/* The walk of one entry point's function [start, end): the barriers it needs go into *l. u: the
 * function's uniformity (level 2), or NULL (level 1: only the top level counts as uniform). 0,
 * or -1 with *why. */
static int walk(const uint32_t *code, size_t start, size_t end, const id_info *ii, uint32_t bound, const vkb_uniform *u,
                inserts *l, vkb_shmbarrier_stats *st, const char **why)
{
    regions rs = {malloc(16 * sizeof(region)), 1, 16};
    int diverged = 0;          /* an invocation may have left early, inside a construct not all took */
    size_t prev_branch = NONE; /* the previous block's OpBranch and its target, when it was in the */
    uint32_t prev_target = 0;  /* region the next block starts in */
    size_t prev_depth = 0;
    uint32_t label = 0;        /* the current block's */
    int rc = 0;
    if (!rs.v) goto oom;
    rs.v[0] = (region){0, 1, NONE, 0, 0, 0, 0, 0};
    for (size_t i = start; i < end;) {
        uint32_t op = code[i] & 0xffff, len = code[i] >> 16;
        const uint32_t *w = code + i + 1;
        i += len;
        size_t at = i - len;
        if (op == OP_LABEL && len >= 2) {
            label = w[0];
            while (rs.n > 1 && rs.v[rs.n - 1].merge == label) { /* constructs end: what they left pending goes up */
                region *c = &rs.v[rs.n - 1], *pr = &rs.v[rs.n - 2];
                if (c->entry != NONE && c->owner < rs.n - 1) /* the same place as the region around it */
                    pr->far |= c->far, pr->near |= c->near, pr->cur |= c->cur;
                else /* its own entry (or none): all of it came after the region around it began */
                    pr->cur |= c->far | c->near | c->cur;
                rs.n--;
            }
            for (size_t k = 1; k < rs.n; k++)
                if (rs.v[k].merge == label) {
                    *why = "a merge block out of structured order";
                    rc = -1;
                    goto done;
                }
            if (label < bound && (ii[label].flags & F_LOOP_HEADER)) { /* a loop begins: its header is inside it */
                size_t entry = prev_branch != NONE && prev_target == label && prev_depth == rs.n ? prev_branch : NONE;
                if (!push(&rs, code, label, 1, entry, diverged, u, ii, bound)) goto oom;
            }
            prev_branch = NONE;
            continue;
        }
        region *r = &rs.v[rs.n - 1];
        if (op == OP_SELECTION_MERGE && len >= 3) {
            /* the selection begins after this instruction; a barrier before it goes right here */
            if (!push(&rs, code, label, 0, at, diverged, u, ii, bound)) goto oom;
            rs.v[rs.n - 1].merge = w[0];
        } else if (op == OP_LOOP_MERGE && len >= 4) {
            r->merge = w[0];
        } else if (op == OP_BRANCH_CONDITIONAL || op == OP_SWITCH) {
            if (rs.n == 1) { /* outside every construct, a conditional branch needs a merge */
                *why = "a conditional branch at the top level without a merge";
                rc = -1;
                goto done;
            }
        } else if (op == OP_BRANCH && len >= 2) {
            prev_branch = at, prev_target = w[0], prev_depth = rs.n;
        } else if (op == OP_RETURN || op == OP_RETURN_VALUE || op == OP_KILL || op == OP_UNREACHABLE ||
                   op == OP_TERMINATE_INVOCATION) {
            if (!r->eff) diverged = 1; /* some invocations may be gone from here on */
        } else if (op == OP_FUNCTION_END) {
            if (rs.n > 1) {
                *why = "a construct still open at the end of the function";
                rc = -1;
                goto done;
            }
        } else if (op == OP_CONTROL_BARRIER || op == OP_MEMORY_BARRIER) {
            unsigned b = barrier_of(ii, bound, op, w, len), keep = (b & 1 ? 0 : P_X) | (b & 2 ? 0 : P_M);
            r->far &= keep, r->near &= keep, r->cur &= keep;
        } else {
            unsigned a = access_of(ii, bound, op, w, len);
            if (!a) continue;
            if (rs.n > 1 && conflict(a, r->far, P_X) && r->entry != NONE && !rs.v[r->owner].done) {
                if (!add_insert(l, r->entry, 1)) goto oom;
                st->entry++;
                rs.v[r->owner].done = 1;
                for (size_t k = r->owner; k < rs.n; k++) rs.v[k].far = 0; /* all of them were before that place */
            }
            unsigned pend = r->far | r->near | r->cur;
            if (r->eff && !diverged && conflict(a, pend, P_X)) {
                if (!add_insert(l, at, 1)) goto oom;
                if (rs.n > 1) st->inside++;
                else st->control++;
                r->far = r->near = r->cur = 0;
            } else if (conflict(a, pend, P_M)) {
                if (!add_insert(l, at, 2)) goto oom;
                st->memory++;
                r->far &= ~P_M, r->near &= ~P_M, r->cur &= ~P_M;
            }
            r->cur |= pend_of(a);
        }
    }
    goto done;
oom:
    *why = "out of memory";
    rc = -1;
done:
    free(rs.v);
    return rc;
}

int vkb_spv_shm_barrier(const uint32_t *code, size_t size, int level, uint32_t **out, size_t *out_size, vkb_shmbarrier_stats *st,
                        const char **why)
{
    vkb_shmbarrier_stats local;
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
    typedef struct {
        uint32_t caller, callee;
    } call;
    call *calls = NULL;
    size_t ncalls = 0, ccap = 0;
    inserts l = {0};
    int rc = 0;
    uint32_t *o = NULL;

    /* 1 */
    size_t first_fn = 0;
    uint32_t fn = 0, int32_t_id = 0, uint32_t_id = 0;
    int has_wg = 0, has_entry = 0;
    for (size_t i = 5; i < n;) {
        uint32_t op = code[i] & 0xffff, len = code[i] >> 16;
        if (!len || i + len > n) {
            *why = "malformed instruction stream";
            rc = -1;
            goto out;
        }
        const uint32_t *w = code + i + 1;
        switch (op) {
        case OP_CAPABILITY: /* GroupNonUniform* (61-68, 69 rotate is 6026), SubgroupBallotKHR, SubgroupVoteKHR */
            if (len >= 2 && ((w[0] >= 61 && w[0] <= 68) || w[0] == 4423 || w[0] == 4431 || w[0] == 6026)) st->subgroup = 1;
            break;
        case OP_DECORATE: /* BuiltIn SubgroupSize, NumSubgroups, SubgroupId, SubgroupLocalInvocationId */
            if (len >= 4 && w[1] == 11 && (w[2] == 36 || w[2] == 38 || w[2] == 40 || w[2] == 41 || (w[2] >= 4416 && w[2] <= 4420))) st->subgroup = 1;
            break;
        case OP_ENTRY_POINT:
            if (len >= 3 && w[0] == EXEC_GLCOMPUTE && w[1] < bound) ii[w[1]].flags |= F_ENTRY, has_entry = 1;
            break;
        case OP_TYPE_INT:
            if (len >= 4 && w[0] < bound && w[1] == 32) {
                ii[w[0]].flags |= F_INT32_TYPE | (w[2] ? 0 : F_UINT32_TYPE);
                if (!int32_t_id) int32_t_id = w[0];
                if (!w[2] && !uint32_t_id) uint32_t_id = w[0];
            }
            break;
        case OP_TYPE_POINTER:
            if (len >= 4 && w[0] < bound && w[1] == SC_WORKGROUP) ii[w[0]].flags |= F_WG_PTR_TYPE;
            break;
        case OP_CONSTANT:
            if (!fn && len == 4 && w[0] < bound && w[1] < bound && (ii[w[0]].flags & F_INT32_TYPE))
                ii[w[1]].flags |= F_CONST32, ii[w[1]].value = w[2];
            break;
        case OP_FUNCTION:
            if (len >= 5 && w[1] < bound) fn = w[1];
            if (!first_fn) first_fn = i;
            break;
        case OP_FUNCTION_END:
            fn = 0;
            break;
        case OP_VARIABLE:
            if (len >= 4 && !fn && w[2] == SC_WORKGROUP) has_wg = 1;
            /* fall through */
        case OP_FUNCTION_PARAMETER:
        case OP_ACCESS_CHAIN:
        case OP_IN_BOUNDS_ACCESS_CHAIN:
        case OP_PTR_ACCESS_CHAIN:
        case OP_IN_BOUNDS_PTR_ACCESS_CHAIN:
        case OP_COPY_OBJECT:
        case OP_SELECT:
        case OP_PHI:
            if (len >= 3 && w[0] < bound && w[1] < bound && (ii[w[0]].flags & F_WG_PTR_TYPE)) ii[w[1]].flags |= F_WG_PTR;
            break;
        case OP_FUNCTION_CALL:
            if (fn && len >= 4 && w[2] < bound) {
                if (ncalls == ccap) {
                    size_t cap = ccap ? ccap * 2 : 16;
                    call *c = realloc(calls, cap * sizeof(*c));
                    if (!c) {
                        *why = "out of memory";
                        rc = -1;
                        goto out;
                    }
                    calls = c, ccap = cap;
                }
                calls[ncalls++] = (call){fn, w[2]};
            }
            break;
        case OP_LOOP_MERGE:
            break;
        default:
            break;
        }
        if (fn && op != OP_FUNCTION_CALL) ii[fn].access |= (uint8_t)access_of(ii, bound, op, w, len);
        i += len;
    }
    if (!has_entry || !has_wg || !first_fn) goto out;
    st->shared = 1;
    /* calls: what a callee does, its callers do (to a fixed point; call graphs have no cycles) */
    for (int changed = 1, rounds = 0; changed && rounds < 64; rounds++) {
        changed = 0;
        for (size_t k = 0; k < ncalls; k++) {
            uint8_t a = ii[calls[k].caller].access | ii[calls[k].callee].access;
            if (a != ii[calls[k].caller].access) ii[calls[k].caller].access = a, changed = 1;
        }
    }

    /* 2: each GLCompute entry point's function (the loop headers first: a loop header block is
     * inside its loop, which is known only at its OpLoopMerge) */
    for (size_t i = first_fn; i < n;) {
        uint32_t op = code[i] & 0xffff, len = code[i] >> 16;
        if (op != OP_FUNCTION || len < 5 || code[i + 2] >= bound || !(ii[code[i + 2]].flags & F_ENTRY)) {
            i += len;
            continue;
        }
        size_t start = i, end = i;
        uint32_t label = 0;
        for (; end < n; end += code[end] >> 16) {
            uint32_t o2 = code[end] & 0xffff;
            if (o2 == OP_LABEL) label = code[end + 1];
            else if (o2 == OP_LOOP_MERGE && label < bound) ii[label].flags |= F_LOOP_HEADER;
            else if (o2 == OP_FUNCTION_END) {
                end += code[end] >> 16;
                break;
            }
        }
        st->entries++;
        if ((ii[code[i + 2]].access & (A_READ | A_WRITE)) == (A_READ | A_WRITE)) {
            vkb_uniform *u = level >= 2 ? vkb_spv_uniform(code, n, start, end, bound) : NULL;
            if (level >= 2 && !u) st->no_uniform++;
            if (u) {
                unsigned nc, nu;
                vkb_uniform_counts(u, &nc, &nu);
                st->constructs += nc, st->uniform += nu;
            }
            int wr = walk(code, start, end, ii, bound, u, &l, st, why);
            vkb_uniform_free(u);
            if (wr < 0) {
                rc = -1;
                goto out;
            }
        }
        i = end;
    }
    if (!l.n) goto out;
    qsort(l.v, l.n, sizeof(*l.v), cmp_insert);

    /* 3: the constants (existing ones when there are), then the code */
    uint32_t next = bound, c_scope = 0, c_sem = 0;
    for (uint32_t id = 1; id < bound && (!c_scope || !c_sem); id++)
        if (ii[id].flags & F_CONST32) {
            if (!c_scope && ii[id].value == SCOPE_WORKGROUP) c_scope = id;
            if (!c_sem && ii[id].value == SEM_ACQREL_WG) c_sem = id;
        }
    uint32_t pre[12];
    unsigned npre = 0;
    uint32_t t = uint32_t_id ? uint32_t_id : int32_t_id;
    if ((!c_scope || !c_sem) && !t) {
        t = next++;
        pre[npre++] = (4u << 16) | OP_TYPE_INT, pre[npre++] = t, pre[npre++] = 32, pre[npre++] = 0;
    }
    if (!c_scope) {
        c_scope = next++;
        pre[npre++] = (4u << 16) | OP_CONSTANT, pre[npre++] = t, pre[npre++] = c_scope, pre[npre++] = SCOPE_WORKGROUP;
    }
    if (!c_sem) {
        c_sem = next++;
        pre[npre++] = (4u << 16) | OP_CONSTANT, pre[npre++] = t, pre[npre++] = c_sem, pre[npre++] = SEM_ACQREL_WG;
    }
    if (next > MAX_BOUND) {
        *why = "id bound out of range";
        rc = -1;
        goto out;
    }
    o = malloc((n + npre + 4 * l.n) * 4);
    if (!o) {
        *why = "out of memory";
        rc = -1;
        goto out;
    }
    memcpy(o, code, first_fn * 4);
    size_t p = first_fn;
    memcpy(o + p, pre, npre * 4);
    p += npre;
    size_t k = 0;
    for (size_t i = first_fn; i < n;) {
        uint32_t len = code[i] >> 16;
        for (; k < l.n && l.v[k].pos == i; k++) {
            if (l.v[k].kind == 1) {
                o[p++] = (4u << 16) | OP_CONTROL_BARRIER;
                o[p++] = c_scope;
                o[p++] = c_scope;
                o[p++] = c_sem;
            } else {
                o[p++] = (3u << 16) | OP_MEMORY_BARRIER;
                o[p++] = c_scope;
                o[p++] = c_sem;
            }
        }
        memcpy(o + p, code + i, len * 4);
        p += len;
        i += len;
    }
    o[3] = next;
    *out = o;
    *out_size = p * 4;
    o = NULL;
    rc = 1;
out:
    if (rc < 0) memset(st, 0, sizeof(*st));
    free(o);
    free(l.v);
    free(calls);
    free(ii);
    return rc;
}
