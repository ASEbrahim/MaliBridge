/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * wave32: 32-wide subgroups emulated in compute shaders (see spirv_wave32.h).
 *
 * 1  one pass over the module: types, constants, built-in decorations, the entry point and its
 *    workgroup size, the shared memory it has, the subgroup built-in variables (each gets a
 *    Private stand-in) and LocalInvocationIndex (found or added);
 * 2  the entry point's function: which values the analysis may take as uniform (the 32-lane
 *    SubgroupSize and NumSubgroups; with a 32-invocation workgroup also the virtual subgroup's
 *    ballots, votes, broadcasts and reductions), then its uniformity (spirv_uniform.h);
 * 3  the functions written again: at the start of the entry point the lane values and the
 *    stand-ins' values, each subgroup operation replaced by its shared-memory form (where the
 *    workgroup is together, checked with a walk of the structured constructs as shm_barrier's),
 *    the result given the old id with OpCopyObject, loads of the built-ins from the stand-ins;
 * 4  the module: the new import, decoration and interface entries in their sections, the new
 *    types, constants and variables before the first function.
 */
#include "spirv_wave32.h"
#include "spirv_uniform.h"

#include <stdlib.h>
#include <string.h>

enum {
    OP_SOURCE_CONTINUED = 2, OP_SOURCE = 3, OP_SOURCE_EXTENSION = 4, OP_NAME = 5, OP_MEMBER_NAME = 6, OP_STRING = 7, OP_LINE = 8,
    OP_EXTENSION = 10, OP_EXT_INST_IMPORT = 11, OP_EXT_INST = 12, OP_MEMORY_MODEL = 14, OP_ENTRY_POINT = 15, OP_EXECUTION_MODE = 16,
    OP_CAPABILITY = 17, OP_TYPE_BOOL = 20, OP_TYPE_INT = 21, OP_TYPE_FLOAT = 22, OP_TYPE_VECTOR = 23, OP_TYPE_MATRIX = 24,
    OP_TYPE_ARRAY = 28, OP_TYPE_STRUCT = 30, OP_TYPE_POINTER = 32, OP_CONSTANT_TRUE = 41, OP_CONSTANT_FALSE = 42, OP_CONSTANT = 43,
    OP_CONSTANT_COMPOSITE = 44, OP_SPEC_CONSTANT = 50, OP_SPEC_CONSTANT_COMPOSITE = 51, OP_FUNCTION = 54, OP_FUNCTION_END = 56,
    OP_VARIABLE = 59, OP_LOAD = 61, OP_STORE = 62, OP_ACCESS_CHAIN = 65, OP_IN_BOUNDS_ACCESS_CHAIN = 66, OP_DECORATE = 71,
    OP_MEMBER_DECORATE = 72, OP_DECORATION_GROUP = 73, OP_GROUP_DECORATE = 74, OP_GROUP_MEMBER_DECORATE = 75,
    OP_VECTOR_EXTRACT_DYNAMIC = 77, OP_VECTOR_SHUFFLE = 79, OP_COMPOSITE_CONSTRUCT = 80, OP_COMPOSITE_EXTRACT = 81,
    OP_COMPOSITE_INSERT = 82, OP_COPY_OBJECT = 83, OP_BITCAST = 124, OP_IADD = 128, OP_FADD = 129, OP_ISUB = 130, OP_IMUL = 132,
    OP_FMUL = 133, OP_ALL = 155, OP_LOGICAL_EQUAL = 164, OP_LOGICAL_NOT_EQUAL = 165, OP_LOGICAL_OR = 166, OP_LOGICAL_AND = 167,
    OP_SELECT = 169, OP_IEQUAL = 170, OP_INOT_EQUAL = 171, OP_UGREATER_THAN_EQUAL = 174, OP_FORD_EQUAL = 180,
    OP_SHIFT_RIGHT_LOGICAL = 194, OP_SHIFT_LEFT_LOGICAL = 196, OP_BITWISE_OR = 197, OP_BITWISE_XOR = 198, OP_BITWISE_AND = 199,
    OP_NOT = 200, OP_BIT_COUNT = 205, OP_CONTROL_BARRIER = 224, OP_LOOP_MERGE = 246, OP_SELECTION_MERGE = 247, OP_LABEL = 248,
    OP_SWITCH = 251, OP_KILL = 252, OP_RETURN = 253, OP_RETURN_VALUE = 254, OP_UNREACHABLE = 255, OP_NO_LINE = 317,
    OP_MODULE_PROCESSED = 330, OP_EXECUTION_MODE_ID = 331, OP_DECORATE_ID = 332, OP_TERMINATE_INVOCATION = 4416,
    OP_DECORATE_STRING = 5632, OP_MEMBER_DECORATE_STRING = 5633,
    /* subgroup operations */
    SG_ELECT = 333, SG_ALL = 334, SG_ANY = 335, SG_ALL_EQUAL = 336, SG_BROADCAST = 337, SG_BROADCAST_FIRST = 338, SG_BALLOT = 339,
    SG_INVERSE_BALLOT = 340, SG_BIT_EXTRACT = 341, SG_BIT_COUNT = 342, SG_FIND_LSB = 343, SG_FIND_MSB = 344, SG_SHUFFLE = 345,
    SG_SHUFFLE_XOR = 346, SG_SHUFFLE_UP = 347, SG_SHUFFLE_DOWN = 348, SG_ARITH_FIRST = 349, SG_ARITH_LAST = 364,
    SG_QUAD_BROADCAST = 365, SG_QUAD_SWAP = 366, SG_ROTATE = 4431,
};

enum { BI_WORKGROUP_SIZE = 25, BI_LOCAL_INDEX = 29, BI_SUBGROUP_SIZE = 36, BI_NUM_SUBGROUPS = 38, BI_SUBGROUP_ID = 40,
       BI_SUBGROUP_LOCAL_ID = 41, BI_EQ_MASK = 4416, BI_LT_MASK = 4420 };
enum { SC_INPUT = 1, SC_WORKGROUP = 4, SC_PRIVATE = 6 };
enum { EXEC_GLCOMPUTE = 5, MODE_LOCAL_SIZE = 17, MODE_LOCAL_SIZE_ID = 38, SCOPE_WORKGROUP = 2, SCOPE_SUBGROUP = 3,
       SEM_ACQREL_WG = 0x8 | 0x100, MEM_AVAIL_NONPRIV = 0x8 | 0x20, MEM_VIS_NONPRIV = 0x10 | 0x20 };
enum { GLSL_FMIN = 37, GLSL_UMIN = 38, GLSL_SMIN = 39, GLSL_FMAX = 40, GLSL_UMAX = 41, GLSL_SMAX = 42, GLSL_FIND_ILSB = 73,
       GLSL_FIND_UMSB = 75 };
enum { GOP_REDUCE = 0, GOP_INCLUSIVE = 1, GOP_EXCLUSIVE = 2, GOP_CLUSTERED = 3 };

#define MAGIC 0x07230203u
#define MAX_BOUND (1u << 22)

static int is_sg_builtin(uint32_t b)
{
    return b == BI_SUBGROUP_SIZE || b == BI_NUM_SUBGROUPS || b == BI_SUBGROUP_ID || b == BI_SUBGROUP_LOCAL_ID ||
           (b >= BI_EQ_MASK && b <= BI_LT_MASK);
}

/* GroupNonUniform* and the older subgroup extensions' operations (KHR ballot/vote, quad
 * any/all, NV partitioned, INTEL shuffles) */
static int is_sg_op(uint32_t op)
{
    return (op >= SG_ELECT && op <= SG_QUAD_SWAP) || op == SG_ROTATE || op == 4421 || op == 4422 || (op >= 4428 && op <= 4430) ||
           op == 4432 || op == 5110 || op == 5111 || op == 5296 || (op >= 5571 && op <= 5578);
}

int vkb_spv_subgroup_class(const uint32_t *code, size_t size, int *compute)
{
    if (compute) *compute = 0;
    size_t n = size / 4;
    if (n < 5 || (size & 3) || code[0] != MAGIC) return VKB_SG_NONE;
    uint32_t bound = code[3];
    if (!bound || bound > MAX_BOUND) return VKB_SG_NONE;
    uint8_t *f = calloc(bound, 1); /* 1 a subgroup built-in variable, 2 SubgroupSize's, 4 a value read from it */
    if (!f) return VKB_SG_FIXED;
    int ops = 0, real = 0, adapts = 0;
    for (size_t i = 5; i < n;) {
        uint32_t op = code[i] & 0xffff, len = code[i] >> 16;
        if (!len || i + len > n) break;
        const uint32_t *w = code + i + 1;
        if (op == OP_ENTRY_POINT && len >= 3 && w[0] == EXEC_GLCOMPUTE && compute) *compute = 1;
        else if (op == OP_DECORATE && len >= 4 && w[1] == 11 && w[0] < bound && is_sg_builtin(w[2]))
            f[w[0]] |= 1 | (w[2] == BI_SUBGROUP_SIZE ? 2 : 0);
        else if ((op == OP_LOAD || op == OP_ACCESS_CHAIN || op == OP_IN_BOUNDS_ACCESS_CHAIN) && len >= 4 && w[2] < bound && (f[w[2]] & 1)) {
            real = 1;
            if (op == OP_LOAD && (f[w[2]] & 2) && w[1] < bound) f[w[1]] |= 4;
        } else if (is_sg_op(op)) {
            ops = 1;
            if (op != SG_ELECT && op != SG_BROADCAST_FIRST) real = 1;
        } else if (op >= OP_LOGICAL_EQUAL && op <= 191 && len >= 5) {
            if ((w[2] < bound && (f[w[2]] & 4)) || (w[3] < bound && (f[w[3]] & 4))) adapts = 1;
        } else if (op == OP_SWITCH && len >= 3 && w[0] < bound && (f[w[0]] & 4))
            adapts = 1;
        i += len;
    }
    free(f);
    if (!ops && !real) return VKB_SG_NONE;
    if (!real) return VKB_SG_ANY_SIZE;
    return adapts ? VKB_SG_ADAPTS : VKB_SG_FIXED;
}

/* ---- the rewrite ---- */

typedef struct {
    uint32_t *v;
    size_t n, cap;
    int oom;
} wbuf;

static void wput(wbuf *b, const uint32_t *w, size_t k)
{
    if (b->oom) return;
    if (b->n + k > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        while (cap < b->n + k) cap *= 2;
        uint32_t *v = realloc(b->v, cap * 4);
        if (!v) {
            b->oom = 1;
            return;
        }
        b->v = v, b->cap = cap;
    }
    memcpy(b->v + b->n, w, k * 4);
    b->n += k;
}

static void winst(wbuf *b, uint32_t op, const uint32_t *ops, unsigned k)
{
    uint32_t h = ((uint32_t)(k + 1) << 16) | op;
    wput(b, &h, 1);
    wput(b, ops, k);
}

enum { K_U32 = 1, K_I32, K_F32, K_BOOL };
enum { F_BI = 1, F_BIPTR = 2, F_LOOP_HEADER = 4, F_GIVEN = 8 };

typedef struct {
    uint32_t var, priv, builtin, pointee;
} bivar;

typedef struct {
    uint32_t merge;
    int eff;
} region;

typedef struct {
    const uint32_t *code;
    size_t n;
    uint32_t bound, next;
    size_t *def;     /* id -> offset of its defining instruction (0: none) */
    uint32_t *rtype; /* id -> its result type */
    uint8_t *fl;
    size_t types_from, first_fn;
    wbuf pre, out;
    uint32_t t_u32, t_bool, glsl;
    int glsl_new, vmm;
    uint32_t lii, lane, base, eqx, ltx, lex, sgid; /* the entry point's lane values */
    uint32_t tmp, wg;
    unsigned comps, ops, barriers, kept;
    const char *why;
} ctx;

static uint32_t nid(ctx *c)
{
    return c->next++;
}

/* The instruction defining an input id (NULL for new ones and for none) */
static const uint32_t *defn(const ctx *c, uint32_t id, uint32_t *op, uint32_t *len)
{
    if (!id || id >= c->bound || !c->def[id]) return NULL;
    const uint32_t *w = c->code + c->def[id];
    *op = w[0] & 0xffff, *len = w[0] >> 16;
    return w + 1;
}

/* A type or constant already declared (in the input or among the new ones): types match on
 * their operands, constants (type != 0) on their type and value words */
static uint32_t find_decl(const ctx *c, uint32_t op, uint32_t type, const uint32_t *ops, unsigned k)
{
    for (int pass = 0; pass < 2; pass++) {
        const uint32_t *v = pass ? c->pre.v : c->code;
        size_t from = pass ? 0 : c->types_from, to = pass ? c->pre.n : c->first_fn;
        for (size_t i = from; i < to && v;) {
            uint32_t o = v[i] & 0xffff, len = v[i] >> 16;
            if (!len) break;
            if (o == op) {
                if (!type && len == k + 2 && !memcmp(v + i + 2, ops, k * 4)) return v[i + 1];
                if (type && len == k + 3 && v[i + 1] == type && !memcmp(v + i + 3, ops, k * 4)) return v[i + 2];
            }
            i += len;
        }
    }
    return 0;
}

static uint32_t get_type(ctx *c, uint32_t op, const uint32_t *ops, unsigned k)
{
    uint32_t id = find_decl(c, op, 0, ops, k);
    if (id) return id;
    uint32_t w[8];
    w[0] = id = nid(c);
    if (k) memcpy(w + 1, ops, k * 4);
    winst(&c->pre, op, w, k + 1);
    return id;
}

static uint32_t get_const(ctx *c, uint32_t op, uint32_t type, const uint32_t *ops, unsigned k)
{
    uint32_t id = find_decl(c, op, type, ops, k);
    if (id) return id;
    uint32_t w[8];
    w[0] = type, w[1] = id = nid(c);
    if (k) memcpy(w + 2, ops, k * 4);
    winst(&c->pre, op, w, k + 2);
    return id;
}

static uint32_t u(ctx *c, uint32_t v)
{
    return get_const(c, OP_CONSTANT, c->t_u32, &v, 1);
}

/* A scalar or vector type: its kind (K_*, 0 when not 32-bit int/float/bool), scalar type and
 * component count */
static int shape(const ctx *c, uint32_t t, uint32_t *scalar, unsigned *k)
{
    uint32_t op, len;
    const uint32_t *w = defn(c, t, &op, &len);
    *k = 1, *scalar = t;
    if (t == c->t_u32) return K_U32;
    if (t == c->t_bool) return K_BOOL;
    if (!w) return 0;
    if (op == OP_TYPE_VECTOR && len == 4 && w[2] >= 2 && w[2] <= 4) {
        unsigned k2;
        int kind = shape(c, w[1], scalar, &k2);
        *k = w[2];
        return k2 == 1 ? kind : 0;
    }
    if (op == OP_TYPE_INT && len == 4 && w[1] == 32) return w[2] ? K_I32 : K_U32;
    if (op == OP_TYPE_FLOAT && len >= 3 && w[1] == 32) return K_F32;
    if (op == OP_TYPE_BOOL) return K_BOOL;
    return 0;
}

static uint32_t konst(ctx *c, uint32_t scalar, int kind, uint32_t v)
{
    if (kind == K_BOOL) return get_const(c, v ? OP_CONSTANT_TRUE : OP_CONSTANT_FALSE, scalar, NULL, 0);
    return get_const(c, OP_CONSTANT, scalar, &v, 1);
}

/* the value v in every component of t */
static uint32_t splat(ctx *c, uint32_t t, uint32_t v)
{
    uint32_t s;
    unsigned k;
    int kind = shape(c, t, &s, &k);
    uint32_t x = konst(c, s, kind, v);
    if (k == 1) return x;
    uint32_t xs[4] = {x, x, x, x};
    return get_const(c, OP_CONSTANT_COMPOSITE, t, xs, k);
}

static uint32_t e(ctx *c, uint32_t op, uint32_t t, const uint32_t *a, unsigned k)
{
    uint32_t w[8];
    w[0] = t, w[1] = nid(c);
    memcpy(w + 2, a, k * 4);
    winst(&c->out, op, w, k + 2);
    return w[1];
}
#define E1(op, t, a) e(c, op, t, (const uint32_t[]){a}, 1)
#define E2(op, t, a, b) e(c, op, t, (const uint32_t[]){a, b}, 2)
#define E3(op, t, a, b, d) e(c, op, t, (const uint32_t[]){a, b, d}, 3)

static uint32_t glsl(ctx *c)
{
    if (!c->glsl) c->glsl = nid(c), c->glsl_new = 1;
    return c->glsl;
}

static void barrier(ctx *c)
{
    uint32_t s = u(c, SCOPE_WORKGROUP), m = u(c, SEM_ACQREL_WG);
    winst(&c->out, OP_CONTROL_BARRIER, (const uint32_t[]){s, s, m}, 3);
    c->barriers++;
}

static uint32_t to_u32(ctx *c, int kind, uint32_t x)
{
    if (kind == K_U32) return x;
    if (kind == K_BOOL) return E3(OP_SELECT, c->t_u32, x, u(c, 1), u(c, 0));
    return E1(OP_BITCAST, c->t_u32, x);
}

static uint32_t from_u32(ctx *c, int kind, uint32_t scalar, uint32_t x)
{
    if (kind == K_U32) return x;
    if (kind == K_BOOL) return E2(OP_INOT_EQUAL, scalar, x, u(c, 0));
    return E1(OP_BITCAST, scalar, x);
}

/* An index operand as uint32 (0: not a 32-bit integer) */
static uint32_t index_u32(ctx *c, uint32_t id)
{
    uint32_t s;
    unsigned k;
    int kind = id < c->bound ? shape(c, c->rtype[id], &s, &k) : 0;
    if ((kind != K_U32 && kind != K_I32) || k != 1) return 0;
    return to_u32(c, kind, id);
}

static uint32_t extract(ctx *c, uint32_t scalar, uint32_t v, uint32_t i)
{
    uint32_t w[4] = {scalar, nid(c), v, i};
    winst(&c->out, OP_COMPOSITE_EXTRACT, w, 4);
    return w[1];
}

static uint32_t construct(ctx *c, uint32_t t, const uint32_t *x, unsigned k)
{
    return e(c, OP_COMPOSITE_CONSTRUCT, t, x, k);
}

/* tmp[i] (a pointer) */
static uint32_t slot(ctx *c, uint32_t i)
{
    uint32_t p = get_type(c, OP_TYPE_POINTER, (const uint32_t[]){SC_WORKGROUP, c->t_u32}, 2);
    return E2(OP_ACCESS_CHAIN, p, c->tmp, i);
}

/* Every invocation hands over v (of type t), and gets the one of lane src (a uint32) of its
 * virtual subgroup */
static uint32_t exchange(ctx *c, uint32_t t, uint32_t v, uint32_t src)
{
    uint32_t s, y[4];
    unsigned k;
    int kind = shape(c, t, &s, &k);
    if (k > c->comps) c->comps = k;
    barrier(c);
    for (unsigned i = 0; i < k; i++) {
        uint32_t x = to_u32(c, kind, k > 1 ? extract(c, s, v, i) : v);
        uint32_t p = slot(c, i ? E2(OP_IADD, c->t_u32, c->lii, u(c, i * c->wg)) : c->lii);
        if (c->vmm) winst(&c->out, OP_STORE, (const uint32_t[]){p, x, MEM_AVAIL_NONPRIV, u(c, SCOPE_WORKGROUP)}, 4);
        else winst(&c->out, OP_STORE, (const uint32_t[]){p, x}, 2);
    }
    barrier(c);
    uint32_t a = E2(OP_IADD, c->t_u32, c->base, E2(OP_BITWISE_AND, c->t_u32, src, u(c, 31)));
    for (unsigned i = 0; i < k; i++) {
        uint32_t p = slot(c, i ? E2(OP_IADD, c->t_u32, a, u(c, i * c->wg)) : a), l;
        if (c->vmm) l = e(c, OP_LOAD, c->t_u32, (const uint32_t[]){p, MEM_VIS_NONPRIV, u(c, SCOPE_WORKGROUP)}, 3);
        else l = E1(OP_LOAD, c->t_u32, p);
        y[i] = from_u32(c, kind, s, l);
    }
    return k > 1 ? construct(c, t, y, k) : y[0];
}

/* cond ? a : b for values of type t (cond a bool) */
static uint32_t pick(ctx *c, uint32_t t, uint32_t cond, uint32_t a, uint32_t b)
{
    uint32_t s;
    unsigned k;
    shape(c, t, &s, &k);
    if (k > 1) {
        uint32_t bv = get_type(c, OP_TYPE_VECTOR, (const uint32_t[]){c->t_bool, k}, 2), cs[4] = {cond, cond, cond, cond};
        cond = construct(c, bv, cs, k);
    }
    return E3(OP_SELECT, t, cond, a, b);
}

/* The group arithmetic operations: the operation, GLSL.std.450 or core, the value kinds, the
 * identity (for exclusive scans) */
typedef struct {
    uint16_t op;
    uint8_t ext, kinds; /* kinds: 1 integers, 2 floats, 4 bools */
    uint32_t ident;
} arith;

static const arith ariths[SG_ARITH_LAST - SG_ARITH_FIRST + 1] = {
    {OP_IADD, 0, 1, 0},          {OP_FADD, 0, 2, 0},          {OP_IMUL, 0, 1, 1},         {OP_FMUL, 0, 2, 0x3f800000},
    {GLSL_SMIN, 1, 1, 0x7fffffff}, {GLSL_UMIN, 1, 1, 0xffffffff}, {GLSL_FMIN, 1, 2, 0x7f800000}, {GLSL_SMAX, 1, 1, 0x80000000},
    {GLSL_UMAX, 1, 1, 0},        {GLSL_FMAX, 1, 2, 0xff800000}, {OP_BITWISE_AND, 0, 1, 0xffffffff}, {OP_BITWISE_OR, 0, 1, 0},
    {OP_BITWISE_XOR, 0, 1, 0},   {OP_LOGICAL_AND, 0, 4, 1},   {OP_LOGICAL_OR, 0, 4, 0},   {OP_LOGICAL_NOT_EQUAL, 0, 4, 0},
};

static uint32_t combine(ctx *c, const arith *a, uint32_t t, uint32_t x, uint32_t y)
{
    if (a->ext) return e(c, OP_EXT_INST, t, (const uint32_t[]){glsl(c), a->op, x, y}, 4);
    return E2(a->op, t, x, y);
}

/* over lanes l ^ 1, l ^ 2, ... below `cluster`: every lane gets the cluster's result */
static uint32_t reduce(ctx *c, const arith *a, uint32_t t, uint32_t v, unsigned cluster)
{
    for (unsigned d = 1; d < cluster; d <<= 1) v = combine(c, a, t, v, exchange(c, t, v, E2(OP_BITWISE_XOR, c->t_u32, c->lane, u(c, d))));
    return v;
}

static uint32_t inclusive(ctx *c, const arith *a, uint32_t t, uint32_t v)
{
    for (unsigned d = 1; d < 32; d <<= 1) {
        uint32_t o = exchange(c, t, v, E2(OP_ISUB, c->t_u32, c->lane, u(c, d)));
        v = pick(c, t, E2(OP_UGREATER_THAN_EQUAL, c->t_bool, c->lane, u(c, d)), combine(c, a, t, v, o), v);
    }
    return v;
}

static const arith or_op = {OP_BITWISE_OR, 0, 1, 0}, and_op = {OP_BITWISE_AND, 0, 1, 0xffffffff};

/* all lanes' bools: 1 when any (or all) of them is true */
static uint32_t vote(ctx *c, uint32_t pred, int all)
{
    uint32_t bit = E3(OP_SELECT, c->t_u32, pred, u(c, 1), u(c, 0));
    return E2(OP_INOT_EQUAL, c->t_bool, reduce(c, all ? &and_op : &or_op, c->t_u32, bit, 32), u(c, 0));
}

static int fail(ctx *c, const char *why)
{
    if (!c->why) c->why = why;
    return -1;
}

static int needs_together(uint32_t op)
{
    return !(op == SG_INVERSE_BALLOT || op == SG_BIT_EXTRACT || op == SG_BIT_COUNT || op == SG_FIND_LSB || op == SG_FIND_MSB);
}

/* One subgroup operation, w[0] its result type, w[1] its id, w[2] the scope */
static int lower(ctx *c, uint32_t op, const uint32_t *w, uint32_t len)
{
    if (len < 4) return fail(c, "a malformed subgroup operation");
    uint32_t rt = w[0], rid = w[1], sop, slen;
    const uint32_t *sw = defn(c, w[2], &sop, &slen);
    if (!sw || sop != OP_CONSTANT || slen != 4 || sw[2] != SCOPE_SUBGROUP) return fail(c, "a subgroup operation not at Subgroup scope");
    uint32_t rs, vs;
    unsigned rk, vk;
    int rkind = shape(c, rt, &rs, &rk), vkind = 0;
    uint32_t vt = 0, r = 0;
    /* the value operand (where the operation has one) */
    unsigned vat = op == SG_BIT_COUNT || (op >= SG_ARITH_FIRST && op <= SG_ARITH_LAST) ? 4 : 3;
    if (op != SG_ELECT) {
        if (len < vat + 2 || w[vat] >= c->bound) return fail(c, "a malformed subgroup operation");
        vt = c->rtype[w[vat]];
        vkind = shape(c, vt, &vs, &vk);
        if (!vkind) return fail(c, "a subgroup operation on a type that is not 32-bit");
    }
    if (!rkind && op != SG_BALLOT) return fail(c, "a subgroup operation on a type that is not 32-bit");
    c->ops++;
    switch (op) {
    case SG_ELECT:
        r = E2(OP_IEQUAL, c->t_bool, c->lane, u(c, 0));
        break;
    case SG_ALL:
    case SG_ANY:
        r = vote(c, w[3], op == SG_ALL);
        break;
    case SG_ALL_EQUAL: {
        uint32_t first = exchange(c, vt, w[3], u(c, 0));
        uint32_t cmp = vkind == K_F32 ? OP_FORD_EQUAL : vkind == K_BOOL ? OP_LOGICAL_EQUAL : OP_IEQUAL;
        uint32_t bt = vk > 1 ? get_type(c, OP_TYPE_VECTOR, (const uint32_t[]){c->t_bool, vk}, 2) : c->t_bool;
        uint32_t eq = E2(cmp, bt, w[3], first);
        if (vk > 1) eq = E1(OP_ALL, c->t_bool, eq);
        r = vote(c, eq, 1);
        break;
    }
    case SG_BROADCAST:
    case SG_SHUFFLE:
    case SG_SHUFFLE_XOR:
    case SG_SHUFFLE_UP:
    case SG_SHUFFLE_DOWN:
    case SG_QUAD_BROADCAST:
    case SG_QUAD_SWAP:
    case SG_ROTATE: {
        if (len < 6) return fail(c, "a malformed subgroup operation");
        if (op == SG_ROTATE && len > 6) return fail(c, "a clustered rotate");
        uint32_t x = index_u32(c, w[4]), src;
        if (!x) return fail(c, "a subgroup lane index that is not a 32-bit integer");
        switch (op) {
        case SG_SHUFFLE_XOR: src = E2(OP_BITWISE_XOR, c->t_u32, c->lane, x); break;
        case SG_SHUFFLE_UP: src = E2(OP_ISUB, c->t_u32, c->lane, x); break;
        case SG_SHUFFLE_DOWN:
        case SG_ROTATE: src = E2(OP_IADD, c->t_u32, c->lane, x); break;
        case SG_QUAD_BROADCAST: src = E2(OP_BITWISE_OR, c->t_u32, E2(OP_BITWISE_AND, c->t_u32, c->lane, u(c, ~3u)), x); break;
        case SG_QUAD_SWAP: src = E2(OP_BITWISE_XOR, c->t_u32, c->lane, E2(OP_IADD, c->t_u32, x, u(c, 1))); break;
        default: src = x;
        }
        r = exchange(c, vt, w[3], src);
        break;
    }
    case SG_BROADCAST_FIRST:
        r = exchange(c, vt, w[3], u(c, 0));
        break;
    case SG_BALLOT: {
        uint32_t bs;
        unsigned bk;
        int bkind = shape(c, rt, &bs, &bk);
        if (vkind != K_BOOL || vk != 1 || (bkind != K_U32 && bkind != K_I32) || bk != 4) return fail(c, "a ballot of an unusual type");
        uint32_t bit = E3(OP_SELECT, c->t_u32, w[3], c->eqx, u(c, 0));
        uint32_t x = from_u32(c, bkind, bs, reduce(c, &or_op, c->t_u32, bit, 32)), z = konst(c, bs, bkind, 0);
        r = construct(c, rt, (const uint32_t[]){x, z, z, z}, 4);
        break;
    }
    case SG_INVERSE_BALLOT:
    case SG_BIT_EXTRACT:
    case SG_BIT_COUNT:
    case SG_FIND_LSB:
    case SG_FIND_MSB: {
        if ((vkind != K_U32 && vkind != K_I32) || vk != 4) return fail(c, "a ballot of an unusual type");
        uint32_t x = to_u32(c, vkind, extract(c, vs, w[vat], 0));
        if (op == SG_INVERSE_BALLOT) {
            uint32_t b = E2(OP_BITWISE_AND, c->t_u32, E2(OP_SHIFT_RIGHT_LOGICAL, c->t_u32, x, c->lane), u(c, 1));
            r = E2(OP_INOT_EQUAL, rt, b, u(c, 0));
        } else if (op == SG_BIT_EXTRACT) {
            uint32_t i = index_u32(c, w[4]);
            if (!i) return fail(c, "a ballot bit index that is not a 32-bit integer");
            uint32_t word = to_u32(c, vkind, E2(OP_VECTOR_EXTRACT_DYNAMIC, vs, w[3], E2(OP_SHIFT_RIGHT_LOGICAL, c->t_u32, i, u(c, 5))));
            uint32_t b = E2(OP_SHIFT_RIGHT_LOGICAL, c->t_u32, word, E2(OP_BITWISE_AND, c->t_u32, i, u(c, 31)));
            r = E2(OP_INOT_EQUAL, rt, E2(OP_BITWISE_AND, c->t_u32, b, u(c, 1)), u(c, 0));
        } else if (op == SG_BIT_COUNT) {
            if (w[3] == GOP_INCLUSIVE) x = E2(OP_BITWISE_AND, c->t_u32, x, c->lex);
            else if (w[3] == GOP_EXCLUSIVE) x = E2(OP_BITWISE_AND, c->t_u32, x, c->ltx);
            else if (w[3] != GOP_REDUCE) return fail(c, "a ballot bit count with a group operation other than reduce or scans");
            r = E1(OP_BIT_COUNT, rt, x);
        } else
            r = e(c, OP_EXT_INST, rt, (const uint32_t[]){glsl(c), op == SG_FIND_LSB ? GLSL_FIND_ILSB : GLSL_FIND_UMSB, x}, 3);
        break;
    }
    default: {
        if (op < SG_ARITH_FIRST || op > SG_ARITH_LAST) return fail(c, "a subgroup operation this pass does not do");
        const arith *a = &ariths[op - SG_ARITH_FIRST];
        int kbit = vkind == K_F32 ? 2 : vkind == K_BOOL ? 4 : 1;
        if (!(a->kinds & kbit)) return fail(c, "a group operation on a type it does not take");
        uint32_t v = w[4];
        if (w[3] == GOP_REDUCE) r = reduce(c, a, vt, v, 32);
        else if (w[3] == GOP_CLUSTERED) {
            uint32_t cop, clen;
            const uint32_t *cw = len >= 7 ? defn(c, w[5], &cop, &clen) : NULL;
            if (!cw || cop != OP_CONSTANT || clen != 4 || !cw[2] || (cw[2] & (cw[2] - 1))) return fail(c, "a clustered operation without a constant power-of-two size");
            r = reduce(c, a, vt, v, cw[2] < 32 ? cw[2] : 32);
        } else if (w[3] == GOP_INCLUSIVE) r = inclusive(c, a, vt, v);
        else if (w[3] == GOP_EXCLUSIVE) {
            uint32_t in = inclusive(c, a, vt, v);
            uint32_t prev = exchange(c, vt, in, E2(OP_ISUB, c->t_u32, c->lane, u(c, 1)));
            r = pick(c, vt, E2(OP_INOT_EQUAL, c->t_bool, c->lane, u(c, 0)), prev, splat(c, vt, a->ident));
        } else
            return fail(c, "a partitioned group operation");
    }
    }
    winst(&c->out, OP_COPY_OBJECT, (const uint32_t[]){rt, rid, r}, 3);
    return 0;
}

/* Bytes of a Workgroup variable's type (-1: not known) */
static long size_of(const ctx *c, uint32_t t, int depth)
{
    uint32_t op, len;
    const uint32_t *w = defn(c, t, &op, &len);
    if (!w || depth > 16) return -1;
    switch (op) {
    case OP_TYPE_BOOL: return 4;
    case OP_TYPE_INT:
    case OP_TYPE_FLOAT: return len >= 3 ? (long)(w[1] / 8) : -1;
    case OP_TYPE_VECTOR:
    case OP_TYPE_MATRIX: {
        long s = len >= 4 ? size_of(c, w[1], depth + 1) : -1;
        return s < 0 ? -1 : s * (w[2] == 3 ? 4 : w[2]);
    }
    case OP_TYPE_ARRAY: {
        uint32_t cop, clen;
        const uint32_t *cw = len >= 4 ? defn(c, w[2], &cop, &clen) : NULL;
        long s = len >= 4 ? size_of(c, w[1], depth + 1) : -1;
        if (!cw || (cop != OP_CONSTANT && cop != OP_SPEC_CONSTANT) || clen < 4 || s < 0) return -1;
        return s * (long)cw[2];
    }
    case OP_TYPE_STRUCT: {
        long sum = 0;
        for (uint32_t k = 1; k < len; k++) {
            long s = size_of(c, w[k], depth + 1);
            if (s < 0) return -1;
            sum += (s + 3) & ~3l;
        }
        return sum;
    }
    default: return -1;
    }
}

/* Words of a literal string starting at w (at most max) */
static unsigned str_words(const uint32_t *w, unsigned max)
{
    for (unsigned k = 0; k < max; k++)
        if (!(w[k] & 0xff000000u) || !(w[k] & 0xff0000u) || !(w[k] & 0xff00u) || !(w[k] & 0xffu)) return k + 1;
    return max;
}

/* Does an instruction (other than a load or access chain) use one of the stand-in pointers? */
static int uses_flagged(const ctx *c, uint32_t op, const uint32_t *w, uint32_t len)
{
    unsigned from = 0, to = len - 1;
    switch (op) {
    case OP_LINE:
    case OP_NO_LINE:
    case OP_LOOP_MERGE:
    case OP_SELECTION_MERGE:
    case OP_LABEL:
        return 0;
    case OP_COMPOSITE_EXTRACT: from = 2, to = 3 < to ? 3 : to; break;
    case OP_COMPOSITE_INSERT:
    case OP_VECTOR_SHUFFLE: from = 2, to = 4 < to ? 4 : to; break;
    case OP_STORE: to = 2 < to ? 2 : to; break;
    case OP_SWITCH: to = 2 < to ? 2 : to; break;
    case OP_EXT_INST: from = 4; break;
    default: break;
    }
    for (unsigned k = from; k < to; k++)
        if (w[k] < c->bound && (c->fl[w[k]] & (F_BI | F_BIPTR))) return 1;
    return 0;
}

int vkb_spv_wave32(const uint32_t *code, size_t size, unsigned shared_limit, uint32_t **out, size_t *out_size, vkb_wave32_stats *st,
                   const char **why)
{
    vkb_wave32_stats local;
    if (!st) st = &local;
    memset(st, 0, sizeof(*st));
    *out = NULL, *out_size = 0, *why = NULL;
    size_t n = size / 4;
    st->cls = vkb_spv_subgroup_class(code, size, &st->compute);
    if (!st->compute || st->cls != VKB_SG_FIXED) return 0;
    uint32_t bound = code[3];
    ctx cx = {0}, *c = &cx;
    c->code = code, c->n = n, c->bound = c->next = bound;
    c->def = calloc(bound, sizeof(*c->def));
    c->rtype = calloc(bound, sizeof(*c->rtype));
    c->fl = calloc(bound, 1);
    uint8_t *given = calloc(bound, 1);
    uint16_t *bi = calloc(bound, sizeof(*bi)); /* BuiltIn + 1 */
    bivar *bv = NULL;
    unsigned nbv = 0;
    region *rs = NULL;
    vkb_uniform *un = NULL;
    int rc = -1;
    if (!c->def || !c->rtype || !c->fl || !given || !bi) {
        fail(c, "out of memory");
        goto done;
    }

    /* 1 */
    size_t ep_at = 0, mm_at = 0, ann_at = 0, entry_start = 0, entry_end = 0;
    uint32_t entry = 0, lsize[3] = {0, 0, 0}, lsize_id[3] = {0, 0, 0}, wgsize_id = 0, lii_var = 0, liptr_u32 = 0;
    long shared = 0;
    int eps = 0, shared_unknown = 0;
    for (size_t i = 5; i < n;) {
        uint32_t op = code[i] & 0xffff, len = code[i] >> 16;
        if (!len || i + len > n) {
            fail(c, "malformed instruction stream");
            goto done;
        }
        const uint32_t *w = code + i + 1;
        int pre = op == OP_CAPABILITY || op == OP_EXTENSION || op == OP_EXT_INST_IMPORT || op == OP_MEMORY_MODEL ||
                  op == OP_ENTRY_POINT || op == OP_EXECUTION_MODE || op == OP_EXECUTION_MODE_ID || op == OP_STRING ||
                  op == OP_SOURCE_EXTENSION || op == OP_SOURCE || op == OP_SOURCE_CONTINUED || op == OP_NAME ||
                  op == OP_MEMBER_NAME || op == OP_MODULE_PROCESSED || op == OP_DECORATE || op == OP_MEMBER_DECORATE ||
                  op == OP_DECORATION_GROUP || op == OP_GROUP_DECORATE || op == OP_GROUP_MEMBER_DECORATE ||
                  op == OP_DECORATE_ID || op == OP_DECORATE_STRING || op == OP_MEMBER_DECORATE_STRING;
        if (!pre && !ann_at) ann_at = c->types_from = i;
        if (op == OP_FUNCTION && !c->first_fn) c->first_fn = i;
        switch (op) {
        case OP_EXT_INST_IMPORT:
            if (len >= 3 && !memcmp(w + 1, "GLSL.std.450", 13)) c->glsl = w[0];
            break;
        case OP_MEMORY_MODEL:
            mm_at = i;
            c->vmm = len >= 3 && w[1] == 3;
            break;
        case OP_ENTRY_POINT:
            eps++;
            if (len >= 3 && w[0] == EXEC_GLCOMPUTE) entry = w[1], ep_at = i;
            break;
        case OP_EXECUTION_MODE:
            if (len >= 6 && w[0] == entry && w[1] == MODE_LOCAL_SIZE) lsize[0] = w[2], lsize[1] = w[3], lsize[2] = w[4];
            break;
        case OP_EXECUTION_MODE_ID: /* (its constants come later: read after this pass) */
            if (len >= 6 && w[0] == entry && w[1] == MODE_LOCAL_SIZE_ID) lsize_id[0] = w[2], lsize_id[1] = w[3], lsize_id[2] = w[4];
            break;
        case OP_DECORATE:
            if (len >= 4 && w[1] == 11 && w[0] < bound) {
                bi[w[0]] = (uint16_t)(w[2] + 1 > 0xffff ? 0xffff : w[2] + 1);
                if (w[2] == BI_WORKGROUP_SIZE) wgsize_id = w[0];
            }
            break;
        default:
            break;
        }
        /* result ids and types (the first definition wins: an instruction without a result that
         * this does not know uses ids defined before it) */
        if ((op >= 19 && op <= 38) || op == OP_EXT_INST_IMPORT || op == OP_LABEL || op == OP_DECORATION_GROUP || op == OP_STRING) {
            if (len >= 2 && w[0] < bound && !c->def[w[0]]) c->def[w[0]] = i;
        } else if (!pre && len >= 3 && w[0] < bound && w[1] < bound && !c->def[w[1]] && op != OP_STORE && op != 63 && op != 64 &&
                   op != OP_LINE && op != OP_NO_LINE && op != OP_FUNCTION_END && op != OP_CONTROL_BARRIER && op != 225 &&
                   op != OP_SELECTION_MERGE && op != OP_LOOP_MERGE && !(op >= 249 && op <= 255) && op != 228 && op != 99 &&
                   op != 39) {
            c->def[w[1]] = i, c->rtype[w[1]] = w[0];
        }
        if (op == OP_TYPE_INT && len == 4 && w[1] == 32 && !w[2] && !c->t_u32) c->t_u32 = w[0];
        if (op == OP_TYPE_BOOL && len == 2 && !c->t_bool) c->t_bool = w[0];
        if (op == OP_VARIABLE && !c->first_fn && len >= 4) {
            uint32_t pop, plen;
            const uint32_t *pw = defn(c, w[0], &pop, &plen);
            uint32_t pointee = pw && pop == OP_TYPE_POINTER && plen >= 4 ? pw[2] : 0;
            if (w[2] == SC_WORKGROUP) {
                long s = size_of(c, pointee, 0);
                if (s < 0) shared_unknown = 1;
                else shared += s;
            } else if (w[2] == SC_INPUT && bi[w[1]] && is_sg_builtin(bi[w[1]] - 1u)) {
                bivar *nb = realloc(bv, (nbv + 1) * sizeof(*bv));
                if (!nb) {
                    fail(c, "out of memory");
                    goto done;
                }
                bv = nb;
                bv[nbv++] = (bivar){w[1], 0, bi[w[1]] - 1u, pointee};
                c->fl[w[1]] |= F_BI;
            } else if (w[2] == SC_INPUT && bi[w[1]] == BI_LOCAL_INDEX + 1 && pointee && pointee == c->t_u32) {
                lii_var = w[1], liptr_u32 = w[0];
            }
        }
        i += len;
    }
    if (eps != 1 || !entry) {
        fail(c, eps > 1 ? "more than one entry point" : "no GLCompute entry point");
        goto done;
    }
    for (int k = 0; k < 3 && lsize_id[0]; k++) { /* LocalSizeId: plain constants only */
        uint32_t cop, clen;
        const uint32_t *cw = defn(c, lsize_id[k], &cop, &clen);
        if (!cw || cop != OP_CONSTANT || clen != 4) {
            fail(c, "the workgroup size from specialization constants");
            goto done;
        }
        lsize[k] = cw[2];
    }
    if (wgsize_id) { /* a WorkgroupSize constant overrides LocalSize; a specialized one is not known here */
        size_t at = c->def[wgsize_id];
        uint32_t wop = at ? code[at] & 0xffff : 0;
        if (wop == OP_SPEC_CONSTANT_COMPOSITE) {
            fail(c, "the workgroup size from specialization constants");
            goto done;
        }
        if (wop == OP_CONSTANT_COMPOSITE && (code[at] >> 16) == 6)
            for (int k = 0; k < 3; k++) {
                uint32_t cop, clen;
                const uint32_t *cw = defn(c, code[at + 3 + k], &cop, &clen);
                if (cw && cop == OP_CONSTANT && clen == 4) lsize[k] = cw[2];
            }
    }
    uint64_t wg = (uint64_t)lsize[0] * lsize[1] * lsize[2];
    st->invocations = (unsigned)(wg > 0xffffffffu ? 0xffffffffu : wg);
    if (!wg || wg % 32 || wg > 4096) {
        fail(c, "a workgroup size that is not a multiple of 32");
        goto done;
    }
    c->wg = (uint32_t)wg;
    if (shared_unknown) {
        fail(c, "shared memory of a type whose size is not known");
        goto done;
    }
    if (!c->first_fn || !ann_at) {
        fail(c, "no function");
        goto done;
    }
    for (unsigned k = 0; k < nbv; k++) {
        uint32_t s;
        unsigned kk;
        int kind = shape(c, bv[k].pointee, &s, &kk);
        int mask = bv[k].builtin >= BI_EQ_MASK;
        if ((kind != K_U32 && kind != K_I32) || kk != (mask ? 4u : 1u)) {
            fail(c, "a subgroup built-in of an unusual type");
            goto done;
        }
    }
    /* the entry point's function, its loop headers, the values taken as uniform */
    for (size_t i = c->first_fn; i < n; i += code[i] >> 16)
        if ((code[i] & 0xffff) == OP_FUNCTION && code[i + 2] == entry) {
            entry_start = i;
            break;
        }
    if (!entry_start) {
        fail(c, "the entry point's function not found");
        goto done;
    }
    {
        uint32_t label = 0;
        for (size_t i = entry_start; i < n; i += code[i] >> 16) {
            uint32_t op = code[i] & 0xffff, len = code[i] >> 16;
            const uint32_t *w = code + i + 1;
            if (op == OP_LABEL) label = w[0];
            else if (op == OP_LOOP_MERGE && label < bound) c->fl[label] |= F_LOOP_HEADER;
            else if (op == OP_FUNCTION_END) {
                entry_end = i + len;
                break;
            } else if (op == OP_LOAD && len >= 4 && w[2] < bound && (c->fl[w[2]] & F_BI) && w[1] < bound) {
                uint32_t b = bi[w[2]] - 1u;
                if (b == BI_SUBGROUP_SIZE || b == BI_NUM_SUBGROUPS || (b == BI_SUBGROUP_ID && c->wg == 32)) given[w[1]] = 1;
            } else if (c->wg == 32 && len >= 4 && w[1] < bound &&
                       (op == SG_BALLOT || op == SG_ALL || op == SG_ANY || op == SG_ALL_EQUAL || op == SG_BROADCAST ||
                        op == SG_BROADCAST_FIRST || (op >= SG_ARITH_FIRST && op <= SG_ARITH_LAST && len >= 6 && w[3] == GOP_REDUCE)))
                given[w[1]] = 1;
        }
    }
    un = vkb_spv_uniform_given(code, n, entry_start, entry_end, bound, given);
    if (!un) {
        fail(c, "control flow not structured as expected");
        goto done;
    }
    if (!c->t_u32) c->t_u32 = get_type(c, OP_TYPE_INT, (const uint32_t[]){32, 0}, 2);
    if (!c->t_bool) c->t_bool = get_type(c, OP_TYPE_BOOL, NULL, 0);
    for (unsigned k = 0; k < nbv; k++) bv[k].priv = nid(c);
    int lii_new = !lii_var;
    if (lii_new) lii_var = nid(c);
    c->tmp = nid(c);

    /* 3 */
    size_t rn = 1, rcap = 16;
    rs = malloc(rcap * sizeof(*rs));
    if (!rs) {
        fail(c, "out of memory");
        goto done;
    }
    int in_entry = 0, prologue = 0, diverged = 0;
    uint32_t label = 0;
    for (size_t i = c->first_fn; i < n && !c->why;) {
        uint32_t op = code[i] & 0xffff, len = code[i] >> 16;
        const uint32_t *w = code + i + 1;
        if (op == OP_FUNCTION) {
            in_entry = i == entry_start;
            prologue = in_entry;
            rn = 1, rs[0] = (region){0, 1}, diverged = 0;
        }
        /* the entry point's lane values, after the first block's variables */
        if (prologue == 2 && op != OP_VARIABLE && op != OP_LINE && op != OP_NO_LINE) {
            prologue = 0;
            if (lii_new) liptr_u32 = get_type(c, OP_TYPE_POINTER, (const uint32_t[]){SC_INPUT, c->t_u32}, 2);
            c->lii = E1(OP_LOAD, c->t_u32, lii_var);
            c->lane = E2(OP_BITWISE_AND, c->t_u32, c->lii, u(c, 31));
            c->base = E2(OP_BITWISE_AND, c->t_u32, c->lii, u(c, ~31u));
            c->eqx = E2(OP_SHIFT_LEFT_LOGICAL, c->t_u32, u(c, 1), c->lane);
            c->ltx = E2(OP_ISUB, c->t_u32, c->eqx, u(c, 1));
            c->lex = E2(OP_BITWISE_OR, c->t_u32, c->ltx, c->eqx);
            c->sgid = E2(OP_SHIFT_RIGHT_LOGICAL, c->t_u32, c->lii, u(c, 5));
            for (unsigned k = 0; k < nbv; k++) {
                uint32_t s, v = 0;
                unsigned kk;
                int kind = shape(c, bv[k].pointee, &s, &kk);
                switch (bv[k].builtin) {
                case BI_SUBGROUP_SIZE: v = u(c, 32); break;
                case BI_NUM_SUBGROUPS: v = u(c, c->wg / 32); break;
                case BI_SUBGROUP_ID: v = c->sgid; break;
                case BI_SUBGROUP_LOCAL_ID: v = c->lane; break;
                case BI_EQ_MASK: v = c->eqx; break;
                case BI_EQ_MASK + 1: v = E1(OP_NOT, c->t_u32, c->ltx); break; /* Ge */
                case BI_EQ_MASK + 2: v = E1(OP_NOT, c->t_u32, c->lex); break; /* Gt */
                case BI_EQ_MASK + 3: v = c->lex; break;
                default: v = c->ltx; break;
                }
                v = from_u32(c, kind, s, v);
                if (kk == 4) {
                    uint32_t z = konst(c, s, kind, 0);
                    v = construct(c, bv[k].pointee, (const uint32_t[]){v, z, z, z}, 4);
                }
                winst(&c->out, OP_STORE, (const uint32_t[]){bv[k].priv, v}, 2);
            }
            st->builtins = nbv;
        }
        if (prologue == 1 && op == OP_LABEL) prologue = 2;
        i += len;
        if (in_entry) { /* where the workgroup is together */
            if (op == OP_LABEL && len >= 2) {
                label = w[0];
                while (rn > 1 && rs[rn - 1].merge == label) rn--;
                if (label < bound && (c->fl[label] & F_LOOP_HEADER)) {
                    if (rn == rcap) {
                        region *nr = realloc(rs, rcap * 2 * sizeof(*rs));
                        if (!nr) { fail(c, "out of memory"); break; }
                        rs = nr, rcap *= 2;
                    }
                    rs[rn] = (region){0, rs[rn - 1].eff && !diverged && vkb_uniform_construct(un, label, NULL, NULL)};
                    rn++;
                }
            } else if (op == OP_SELECTION_MERGE && len >= 3) {
                if (rn == rcap) {
                    region *nr = realloc(rs, rcap * 2 * sizeof(*rs));
                    if (!nr) { fail(c, "out of memory"); break; }
                    rs = nr, rcap *= 2;
                }
                rs[rn] = (region){w[0], rs[rn - 1].eff && !diverged && vkb_uniform_construct(un, label, NULL, NULL)};
                rn++;
            } else if (op == OP_LOOP_MERGE && len >= 3) {
                rs[rn - 1].merge = w[0];
            } else if (op == OP_RETURN || op == OP_RETURN_VALUE || op == OP_KILL || op == OP_UNREACHABLE || op == OP_TERMINATE_INVOCATION) {
                if (!rs[rn - 1].eff) diverged = 1;
            }
        }
        if (is_sg_op(op)) {
            int together = in_entry && rs[rn - 1].eff && !diverged;
            if (op == SG_BROADCAST_FIRST && !together) {
                /* the first active lane's value: any lane's when it is uniform (vkd3d-proton's
                 * descriptor indices), and a waterfall loop works at any size; kept as it is */
                c->kept++;
                wput(&c->out, code + i - len, len);
            } else if (!in_entry) fail(c, "a subgroup operation in a function the entry point calls");
            else if (needs_together(op) && !together) fail(c, "a subgroup operation where the workgroup may not be together");
            else lower(c, op, w, len);
            continue;
        }
        if (op == OP_LOAD && len >= 4 && w[2] < bound && (c->fl[w[2]] & (F_BI | F_BIPTR))) {
            uint32_t p = w[2];
            for (unsigned k = 0; k < nbv; k++)
                if (bv[k].var == p) p = bv[k].priv;
            winst(&c->out, OP_LOAD, (const uint32_t[]){w[0], w[1], p}, 3); /* (no memory operands: Private memory) */
            continue;
        }
        if ((op == OP_ACCESS_CHAIN || op == OP_IN_BOUNDS_ACCESS_CHAIN) && len >= 4 && w[2] < bound && (c->fl[w[2]] & (F_BI | F_BIPTR))) {
            uint32_t pop, plen, p = w[2];
            const uint32_t *pw = defn(c, w[0], &pop, &plen);
            if (!pw || pop != OP_TYPE_POINTER || plen < 4) {
                fail(c, "an access chain of an unusual type");
                break;
            }
            for (unsigned k = 0; k < nbv; k++)
                if (bv[k].var == p) p = bv[k].priv;
            uint32_t nw[16];
            if (len - 1 > 16) {
                fail(c, "an access chain of an unusual type");
                break;
            }
            memcpy(nw, w, (len - 1) * 4);
            nw[0] = get_type(c, OP_TYPE_POINTER, (const uint32_t[]){SC_PRIVATE, pw[2]}, 2);
            nw[2] = p;
            winst(&c->out, op, nw, len - 1);
            if (w[1] < bound) c->fl[w[1]] |= F_BIPTR;
            continue;
        }
        if (uses_flagged(c, op, w, len)) {
            fail(c, "a subgroup built-in used other than by loads");
            break;
        }
        wput(&c->out, code + i - len, len);
    }
    if (c->why) goto done;
    if (c->out.oom || c->pre.oom) {
        fail(c, "out of memory");
        goto done;
    }

    /* the stand-ins, LocalInvocationIndex, the exchange array */
    for (unsigned k = 0; k < nbv; k++) {
        uint32_t p = get_type(c, OP_TYPE_POINTER, (const uint32_t[]){SC_PRIVATE, bv[k].pointee}, 2);
        winst(&c->pre, OP_VARIABLE, (const uint32_t[]){p, bv[k].priv, SC_PRIVATE}, 3);
    }
    if (lii_new) {
        if (!liptr_u32) liptr_u32 = get_type(c, OP_TYPE_POINTER, (const uint32_t[]){SC_INPUT, c->t_u32}, 2);
        winst(&c->pre, OP_VARIABLE, (const uint32_t[]){liptr_u32, lii_var, SC_INPUT}, 3);
    }
    st->shared = c->comps * c->wg * 4;
    if (c->comps) {
        if (shared + st->shared > (long)shared_limit) {
            fail(c, "not enough shared memory left for the exchanges");
            goto done;
        }
        uint32_t lenc = u(c, c->comps * c->wg), arr = nid(c), parr = nid(c);
        winst(&c->pre, OP_TYPE_ARRAY, (const uint32_t[]){arr, c->t_u32, lenc}, 3);
        winst(&c->pre, OP_TYPE_POINTER, (const uint32_t[]){parr, SC_WORKGROUP, arr}, 3);
        winst(&c->pre, OP_VARIABLE, (const uint32_t[]){parr, c->tmp, SC_WORKGROUP}, 3);
    }
    if (c->next > MAX_BOUND || c->pre.oom) {
        fail(c, c->pre.oom ? "out of memory" : "id bound out of range");
        goto done;
    }

    /* 4 */
    {
        wbuf o = {0};
        uint32_t hdr[5] = {code[0], code[1], code[2], c->next, code[4]};
        wput(&o, hdr, 5);
        int new_iface = code[1] >= 0x00010400; /* every global the entry point uses, from SPIR-V 1.4 */
        for (size_t i = 5; i < c->first_fn;) {
            uint32_t op = code[i] & 0xffff, len = code[i] >> 16;
            if (i == mm_at && c->glsl_new) {
                uint32_t iw[5] = {c->glsl};
                memcpy(iw + 1, "GLSL.std.450\0\0\0", 16);
                winst(&o, OP_EXT_INST_IMPORT, iw, 5);
            }
            if (i == ann_at && lii_new) winst(&o, OP_DECORATE, (const uint32_t[]){lii_var, 11, BI_LOCAL_INDEX}, 3);
            if (i == ep_at) {
                unsigned name = str_words(code + i + 3, len - 3);
                int listed = 0;
                for (unsigned k = 3 + name; k < len; k++) listed |= code[i + k] == lii_var;
                uint32_t extra[256];
                unsigned ne = 0;
                if (!listed) extra[ne++] = lii_var;
                if (new_iface && c->comps) extra[ne++] = c->tmp;
                for (unsigned k = 0; new_iface && k < nbv && ne < 256; k++) extra[ne++] = bv[k].priv;
                uint32_t h = ((len + ne) << 16) | op;
                wput(&o, &h, 1);
                wput(&o, code + i + 1, len - 1);
                wput(&o, extra, ne);
            } else
                wput(&o, code + i, len);
            i += len;
        }
        wput(&o, c->pre.v, c->pre.n);
        wput(&o, c->out.v, c->out.n);
        if (o.oom) {
            free(o.v);
            fail(c, "out of memory");
            goto done;
        }
        *out = o.v;
        *out_size = o.n * 4;
    }
    st->ops = c->ops, st->barriers = c->barriers, st->kept = c->kept;
    rc = 1;
done:
    if (rc < 0) {
        *why = c->why ? c->why : "not possible";
        st->ops = st->builtins = st->barriers = st->shared = st->kept = 0;
    }
    vkb_uniform_free(un);
    free(rs);
    free(bv);
    free(bi);
    free(given);
    free(c->pre.v);
    free(c->out.v);
    free(c->def);
    free(c->rtype);
    free(c->fl);
    return rc;
}
