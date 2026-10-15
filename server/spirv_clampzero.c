/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * MK-32: min/max/clamp with a constant zero limit on a constant-selected value, given a zero the
 * driver cannot fold (see spirv_clampzero.h). One scan of the words finds the min/max/clamp ops
 * with a zero limit (most modules end there, untouched); then what each value is made of is
 * worked out to a fixed point (a load is constant-selected only once every store to its variable
 * is), and the module is edited with spirv_edit.c:
 *     %zv = OpVariable %_ptr_Private_float Private                (global, one per scalar type)
 *     main:  OpStore %zv %float_0                                  (top of every entry point)
 *     each function using it:  %z = OpLoad %float %zv  [%z2 = OpCompositeConstruct %v2float %z %z]
 *     %r = OpExtInst %float %glsl NClamp %x %z %float_1            (was %float_0)
 * %r keeps its id (and so its decorations and uses).
 */
#include "spirv_clampzero.h"
#include "spirv_edit.h"

#include <stdlib.h>
#include <string.h>

#include "spirv_opclass.inc"

enum {
    OP_EXT_INST_IMPORT = 11, OP_EXT_INST = 12, OP_ENTRY_POINT = 15, OP_TYPE_INT = 21, OP_TYPE_FLOAT = 22,
    OP_TYPE_VECTOR = 23, OP_TYPE_POINTER = 32, OP_CONSTANT_TRUE = 41, OP_CONSTANT_FALSE = 42, OP_CONSTANT = 43,
    OP_CONSTANT_COMPOSITE = 44, OP_CONSTANT_NULL = 46, OP_SPEC_TRUE = 48, OP_SPEC_FALSE = 49, OP_SPEC_CONSTANT = 50,
    OP_SPEC_COMPOSITE = 51, OP_SPEC_OP = 52, OP_FUNCTION = 54, OP_FUNCTION_END = 56, OP_VARIABLE = 59, OP_LOAD = 61,
    OP_STORE = 62, OP_ACCESS_CHAIN = 65, OP_IN_BOUNDS_ACCESS_CHAIN = 66, OP_PTR_ACCESS_CHAIN = 67,
    OP_IN_BOUNDS_PTR_ACCESS_CHAIN = 70, OP_VECTOR_SHUFFLE = 79, OP_COMPOSITE_CONSTRUCT = 80, OP_COMPOSITE_EXTRACT = 81,
    OP_COMPOSITE_INSERT = 82, OP_COPY_OBJECT = 83, OP_BITCAST = 124, OP_SELECT = 169, OP_BIT_FIELD_INSERT = 201,
    OP_PHI = 245, OP_LABEL = 248, OP_LINE = 8, OP_NO_LINE = 317,
};
enum { SC_OUTPUT = 3, SC_PRIVATE = 6, SC_FUNCTION = 7 };
enum { EM_TESS_CONTROL = 1 };

/* GLSL.std.450 */
enum { G_FMIN = 37, G_UMIN = 38, G_SMIN = 39, G_FMAX = 40, G_UMAX = 41, G_SMAX = 42, G_FCLAMP = 43, G_UCLAMP = 44,
       G_SCLAMP = 45, G_NMIN = 79, G_NMAX = 80, G_NCLAMP = 81 };

#define MAX_BOUND (1u << 22)
#define MAX_LANES 16
#define MAX_TYPES 16 /* scalar types given an opaque zero in one module */
#define FIXPOINT_MAX 32

/* What a value is made of */
enum { K_NONE, K_CONST, K_SELECT, K_PHI, K_MEMORY };
#define COND(k) ((k) >= K_SELECT)

enum { F_ZERO = 1, F_SPEC_ZERO = 2, F_ENTRY = 4, F_GLSL = 8, F_PTR_TYPE = 16, F_NONSEM = 32 };
/* variables whose loads may be constant-selected */
enum { V_CAND = 1, V_OK = 2, V_BAD = 4, V_HAS = 8 };

typedef struct {
    uint32_t type;  /* result type; OpTypeVector: its component type */
    uint32_t root;  /* a pointer into a Function/Private/Output variable: the variable */
    uint16_t op;
    uint8_t kind;   /* K_* */
    uint8_t flags;  /* F_* */
    uint8_t lanes;  /* OpTypeVector */
    uint8_t width;  /* OpTypeInt / OpTypeFloat */
    uint8_t vflags; /* V_*: an OpVariable */
} id_info;

/* A pattern op */
typedef struct {
    uint32_t ins;   /* instruction ordinal */
    uint32_t func;  /* index in funcs */
    uint8_t slot[2]; /* operand indices (in w[]) holding the zero, 0: none */
} hit;

typedef struct {
    uint32_t ins;   /* OpFunction ordinal */
    uint32_t at;    /* where the opaque zeros go: after the first block's OpVariables */
    int entry, used;
} func;

static int is_clamp(uint32_t g)
{
    return g == G_FCLAMP || g == G_UCLAMP || g == G_SCLAMP || g == G_NCLAMP;
}

static int is_minmax(uint32_t g)
{
    return (g >= G_FMIN && g <= G_SMAX) || g == G_NMIN || g == G_NMAX;
}

/* min of the same family, for the imitation's clamp(x, 0, hi) -> min(x, hi) */
static uint32_t clamp_min(uint32_t g)
{
    return g == G_FCLAMP ? G_FMIN : g == G_UCLAMP ? G_UMIN : g == G_SCLAMP ? G_SMIN : G_NMIN;
}

static uint32_t clamp_max(uint32_t g)
{
    return g == G_FCLAMP ? G_FMAX : g == G_UCLAMP ? G_UMAX : g == G_SCLAMP ? G_SMAX : G_NMAX;
}

static uint8_t kind_of(const id_info *ii, uint32_t bound, uint32_t id)
{
    return id && id < bound ? ii[id].kind : K_NONE;
}

static uint32_t root_of(const id_info *ii, uint32_t bound, uint32_t id)
{
    return id && id < bound ? ii[id].root : 0;
}

/* The kind of a value op over ids[0..n): K_NONE if any is unknown, else the first condition-made
 * one's, else K_CONST */
static uint8_t over(const id_info *ii, uint32_t bound, const uint32_t *ids, uint32_t n)
{
    uint8_t k = K_CONST;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t x = kind_of(ii, bound, ids[i]);
        if (x == K_NONE) return K_NONE;
        if (COND(x) && !COND(k)) k = x;
    }
    return n ? k : K_NONE;
}

/* Ops whose operands after the result id are all value ids */
static int value_op(uint32_t op)
{
    return (op >= 109 && op <= 124) /* conversions, bitcast */ || (op >= 126 && op <= 148) /* arithmetic */ ||
           op == OP_COPY_OBJECT || op == OP_COMPOSITE_CONSTRUCT || (op >= 164 && op <= 200) /* logic, compare, bitwise */;
}

/* The kind an instruction in function code gives its result (K_NONE: nothing known) */
static uint8_t classify(const id_info *ii, uint32_t bound, uint32_t op, const uint32_t *w, uint32_t nw)
{
    if (nw < 3) return K_NONE;
    switch (op) {
    case OP_SELECT: {
        if (nw < 5) return K_NONE;
        uint8_t k = over(ii, bound, w + 3, 2);
        if (k == K_NONE) return K_NONE;
        if (COND(k)) return k;
        return kind_of(ii, bound, w[2]) == K_CONST ? K_CONST : K_SELECT;
    }
    case OP_PHI:
        for (uint32_t i = 2; i < nw; i += 2)
            if (kind_of(ii, bound, w[i]) == K_NONE) return K_NONE;
        return K_PHI;
    case OP_COMPOSITE_EXTRACT: return over(ii, bound, w + 2, 1);
    case OP_VECTOR_SHUFFLE: case OP_COMPOSITE_INSERT: return nw >= 4 ? over(ii, bound, w + 2, 2) : K_NONE;
    case OP_EXT_INST:
        if (nw < 4 || w[2] >= bound || !(ii[w[2]].flags & F_GLSL)) return K_NONE;
        return over(ii, bound, w + 4, nw - 4);
    default:
        return value_op(op) ? over(ii, bound, w + 2, nw - 2) : K_NONE;
    }
}

/* The zero operand of a pattern op: 1 for a constant zero, 2 for a spec constant defaulting to 0 */
static int zero_at(const id_info *ii, uint32_t bound, uint32_t id)
{
    if (!id || id >= bound) return 0;
    return ii[id].flags & F_ZERO ? 1 : ii[id].flags & F_SPEC_ZERO ? 2 : 0;
}

/* An OpConstant of scalar type t with all value words zero, or a new one (64-bit: two words) */
static uint32_t zero_const(spv_mod *m, uint32_t t, uint32_t width)
{
    uint32_t nv = width > 32 ? 2 : 1;
    for (uint32_t i = 0; i < m->count; i++) {
        const spv_inst *in = &m->ins[i];
        if (in->op != OP_CONSTANT || in->n != 3 + nv || in->w[0] != t) continue;
        if (!in->w[2] && (nv == 1 || !in->w[3])) return in->w[1];
    }
    uint32_t id = spv_new_id(m), ops[4] = {t, id, 0, 0};
    spv_insert(m, spv_types_end(m), OP_CONSTANT, ops, (uint16_t)(2 + nv));
    return id;
}

static void set_inst(spv_mod *m, uint32_t at, uint16_t op, const uint32_t *ops, uint16_t nops)
{
    spv_inst *in = &m->ins[at];
    uint32_t *nw = malloc((size_t)(nops ? nops : 1) * 4);
    if (!nw) {
        m->err = 1;
        return;
    }
    memcpy(nw, ops, (size_t)nops * 4);
    free(in->w);
    in->w = nw;
    in->op = op;
    in->n = (uint16_t)(nops + 1);
}

int vkb_spv_clamp_zero(const uint32_t *code, size_t size, unsigned flags, uint32_t **out, size_t *out_size,
                       vkb_clampzero_stats *st, const char **why)
{
    vkb_clampzero_stats local;
    if (!st) st = &local;
    memset(st, 0, sizeof(*st));
    *why = NULL;
    size_t n = size / 4;
    if (n < 5 || (size & 3) || code[0] != 0x07230203u) return 0;
    uint32_t bound = code[3];
    if (!bound || bound > MAX_BOUND) {
        *why = "id bound out of range";
        return -1;
    }
    int imitate = flags & VKB_CLAMP_ZERO_IMITATE, all = (flags & VKB_CLAMP_ZERO_ALL) && !imitate;

    id_info *ii = calloc(bound, sizeof(*ii));
    hit *hits = NULL;
    func *funcs = NULL;
    uint32_t nhits = 0, nfuncs = 0, cap_hits = 0, cap_funcs = 0;
    spv_mod m;
    int have_mod = 0, rc = 0;
    if (!ii) {
        *why = "out of memory";
        return -1;
    }

    /* Pass 1: types, constants, variables and pointers into them; the min/max/clamp ops with a zero. */
    int in_func = 0, has_tcs = 0, cand_ops = 0;
    size_t fstart = 0;
    uint32_t ord = 0;
    for (size_t i = 5; i < n; ord++) {
        uint32_t op = code[i] & 0xffff, len = code[i] >> 16;
        if (!len || i + len > n) {
            *why = "malformed instruction stream";
            rc = -1;
            goto done;
        }
        const uint32_t *w = code + i + 1;
        uint32_t nw = len - 1;
        int cls = spv_op_class(op);
        uint32_t rid = cls == 1 && nw >= 1 ? w[0] : cls == 2 && nw >= 2 ? w[1] : 0;
        if (rid >= bound) {
            *why = "id past the bound";
            rc = -1;
            goto done;
        }
        if (op == OP_FUNCTION) {
            if (!fstart) fstart = i;
            in_func = 1;
        }
        if (op == OP_FUNCTION_END) in_func = 0;
        if (op == OP_ENTRY_POINT && nw >= 2) {
            if (w[0] == EM_TESS_CONTROL) has_tcs = 1;
            if (w[1] < bound) ii[w[1]].flags |= F_ENTRY;
        }
        if (rid) {
            id_info *x = &ii[rid];
            x->op = (uint16_t)op;
            if (cls == 2) x->type = w[0];
            switch (op) {
            case OP_EXT_INST_IMPORT:
                if (nw >= 5 && !memcmp(w + 1, "GLSL.std.450", 13)) x->flags |= F_GLSL;
                if (nw >= 4 && !memcmp(w + 1, "NonSemantic.", 12)) x->flags |= F_NONSEM;
                break;
            case OP_TYPE_INT: case OP_TYPE_FLOAT: x->width = (uint8_t)(nw >= 2 && w[1] <= 64 ? w[1] : 0); break;
            case OP_TYPE_VECTOR:
                if (nw >= 3) x->type = w[1], x->lanes = (uint8_t)(w[2] <= MAX_LANES ? w[2] : 0);
                break;
            case OP_TYPE_POINTER: x->flags |= F_PTR_TYPE; break;
            case OP_CONSTANT: {
                int z = 1;
                for (uint32_t k = 2; k < nw; k++) z &= !w[k];
                x->kind = K_CONST;
                if (z) x->flags |= F_ZERO;
                break;
            }
            case OP_CONSTANT_NULL: x->kind = K_CONST, x->flags |= F_ZERO; break;
            case OP_CONSTANT_COMPOSITE: {
                int z = nw > 2;
                for (uint32_t k = 2; k < nw; k++) z &= w[k] < bound && (ii[w[k]].flags & F_ZERO);
                x->kind = K_CONST;
                if (z) x->flags |= F_ZERO;
                break;
            }
            case OP_SPEC_CONSTANT:
                x->kind = K_CONST;
                if (nw >= 3 && !w[2] && (nw < 4 || !w[3])) x->flags |= F_SPEC_ZERO;
                break;
            case OP_CONSTANT_TRUE: case OP_CONSTANT_FALSE: case OP_SPEC_TRUE: case OP_SPEC_FALSE: case OP_SPEC_COMPOSITE:
            case OP_SPEC_OP:
                x->kind = K_CONST;
                break;
            case OP_VARIABLE:
                if (nw >= 3 && (w[2] == SC_FUNCTION || w[2] == SC_PRIVATE || (w[2] == SC_OUTPUT && !has_tcs))) {
                    x->root = rid;
                    x->vflags = V_CAND;
                    if (nw >= 4) {
                        if (kind_of(ii, bound, w[3]) == K_CONST) x->vflags |= V_HAS;
                        else x->vflags = 0;
                    }
                }
                break;
            case OP_ACCESS_CHAIN: case OP_IN_BOUNDS_ACCESS_CHAIN: case OP_PTR_ACCESS_CHAIN: case OP_IN_BOUNDS_PTR_ACCESS_CHAIN:
                if (in_func && nw >= 3) x->root = root_of(ii, bound, w[2]);
                break;
            case OP_COPY_OBJECT:
                if (in_func && nw >= 3) x->root = root_of(ii, bound, w[2]);
                break;
            }
        }
        if (op == OP_EXT_INST && in_func && nw >= 6 && w[2] < bound && (ii[w[2]].flags & F_GLSL) &&
            (is_clamp(w[3]) ? nw >= 7 : is_minmax(w[3]))) {
            int zc = 0, zs = 0;
            for (uint32_t k = is_clamp(w[3]) ? 5 : 4; k < (is_clamp(w[3]) ? 7u : 6u); k++) {
                int z = zero_at(ii, bound, w[k]);
                zc += z == 1, zs += z == 2;
            }
            if (zc) st->zero_ops++, cand_ops = 1;
            else if (zs) st->spec_zero++;
        }
        i += len;
    }
    if (!cand_ops) goto done;

    /* Pass 2: which variables only ever hold constant-selected values. A pointer into one used
     * other than to load, store to or index it takes it out (any operand word: conservative). */
    if (!all) {
        for (size_t i = fstart; i < n;) {
            uint32_t op = code[i] & 0xffff, len = code[i] >> 16;
            const uint32_t *w = code + i + 1;
            uint32_t nw = len - 1, from = 0, to = nw;
            switch (op) {
            case OP_LOAD: to = 0; break;                     /* the pointer loaded from; memory operands */
            case OP_STORE: from = 1, to = nw >= 2 ? 2 : 0; break; /* the object stored (a pointer escapes) */
            case OP_ACCESS_CHAIN: case OP_IN_BOUNDS_ACCESS_CHAIN: case OP_PTR_ACCESS_CHAIN: case OP_IN_BOUNDS_PTR_ACCESS_CHAIN:
                from = 3;
                break;
            case OP_VARIABLE: from = 3; break;               /* an initializer pointing in */
            case OP_COPY_OBJECT: if (nw >= 3 && root_of(ii, bound, w[1])) to = 0; break;
            case OP_COMPOSITE_EXTRACT: from = 2, to = nw >= 3 ? 3 : 0; break;
            case OP_VECTOR_SHUFFLE: case OP_COMPOSITE_INSERT: from = 2, to = nw >= 4 ? 4 : 0; break;
            case OP_EXT_INST:
                /* GLSL.std.450 operands from w[4]; non-semantic ones (debug info naming a variable) are no use */
                if (nw >= 3 && w[2] < bound && (ii[w[2]].flags & F_NONSEM)) to = 0;
                else from = 4;
                break;
            case OP_FUNCTION: case OP_FUNCTION_END: case OP_LABEL: case OP_LINE: case OP_NO_LINE: to = 0; break;
            default: {
                int cls = spv_op_class(op);
                from = cls == 2 ? 2 : cls == 1 ? 1 : 0; /* not the result type and id */
            }
            }
            for (uint32_t k = from; k < to; k++) {
                uint32_t r = root_of(ii, bound, w[k]);
                if (r) ii[r].vflags &= ~V_CAND;
            }
            i += len;
        }

        /* The fixed point: values only gain a kind, variables only become OK. */
        for (int iter = 0; iter < FIXPOINT_MAX; iter++) {
            int changed = 0;
            for (uint32_t v = 1; v < bound; v++)
                if (ii[v].vflags & V_CAND) ii[v].vflags &= ~V_BAD;
            for (size_t i = fstart; i < n;) {
                uint32_t op = code[i] & 0xffff, len = code[i] >> 16;
                const uint32_t *w = code + i + 1;
                uint32_t nw = len - 1;
                if (op == OP_STORE && nw >= 2) {
                    uint32_t r = root_of(ii, bound, w[0]);
                    if (r && (ii[r].vflags & V_CAND)) {
                        if (kind_of(ii, bound, w[1]) == K_NONE) ii[r].vflags |= V_BAD;
                        ii[r].vflags |= V_HAS;
                    }
                } else if (op == OP_LOAD && nw >= 3) {
                    uint32_t r = root_of(ii, bound, w[2]);
                    if (r && (ii[r].vflags & V_OK) && w[1] < bound && ii[w[1]].kind == K_NONE &&
                        !(w[0] < bound && (ii[w[0]].flags & F_PTR_TYPE)))
                        ii[w[1]].kind = K_MEMORY, changed = 1;
                } else if (spv_op_class(op) == 2 && nw >= 2 && w[1] < bound && ii[w[1]].kind == K_NONE) {
                    uint8_t k = classify(ii, bound, op, w, nw);
                    if (k != K_NONE) ii[w[1]].kind = k, changed = 1;
                }
                i += len;
            }
            for (uint32_t v = 1; v < bound; v++) {
                uint8_t f = ii[v].vflags;
                if (!(f & V_CAND)) continue;
                int ok = (f & V_HAS) && !(f & V_BAD);
                if (ok != !!(f & V_OK)) {
                    ii[v].vflags = ok ? f | V_OK : f & ~V_OK;
                    changed = 1;
                }
            }
            if (!changed) break;
        }
    }

    /* Pass 3: the pattern ops, by instruction ordinal; the functions they are in. */
    ord = 0;
    int cur_func = -1, want_label = 0, in_vars = 0;
    for (size_t i = 5; i < n; ord++) {
        uint32_t op = code[i] & 0xffff, len = code[i] >> 16;
        const uint32_t *w = code + i + 1;
        uint32_t nw = len - 1;
        if (in_vars) {
            if (op == OP_VARIABLE) funcs[cur_func].at = ord + 1;
            else if (op != OP_LINE && op != OP_NO_LINE && op != OP_EXT_INST) in_vars = 0;
        }
        if (op == OP_FUNCTION && nw >= 2) {
            if (nfuncs == cap_funcs) {
                cap_funcs = cap_funcs ? cap_funcs * 2 : 16;
                func *nf = realloc(funcs, cap_funcs * sizeof(*nf));
                if (!nf) {
                    *why = "out of memory";
                    rc = -1;
                    goto done;
                }
                funcs = nf;
            }
            cur_func = (int)nfuncs++;
            funcs[cur_func] = (func){ord, 0, w[1] < bound && (ii[w[1]].flags & F_ENTRY), 0};
            want_label = 1;
        } else if (op == OP_LABEL && want_label) {
            funcs[cur_func].at = ord + 1;
            want_label = 0, in_vars = 1;
        } else if (op == OP_EXT_INST && cur_func >= 0 && nw >= 6 && w[2] < bound && (ii[w[2]].flags & F_GLSL) &&
                   (is_clamp(w[3]) ? nw >= 7 : is_minmax(w[3]))) {
            int clamp = is_clamp(w[3]);
            hit h = {ord, (uint32_t)cur_func, {0, 0}};
            uint8_t k = K_NONE;
            if (clamp) {
                for (int s = 0; s < 2; s++)
                    if (zero_at(ii, bound, w[5 + s]) == 1) h.slot[s] = (uint8_t)(5 + s);
                if (h.slot[0] || h.slot[1]) k = kind_of(ii, bound, w[4]);
            } else {
                int z0 = zero_at(ii, bound, w[4]) == 1, z1 = zero_at(ii, bound, w[5]) == 1;
                if (z0 && !z1) h.slot[0] = 4, k = kind_of(ii, bound, w[5]);
                else if (z1 && !z0) h.slot[0] = 5, k = kind_of(ii, bound, w[4]);
                else if (z0 && z1) h.slot[0] = 4, h.slot[1] = 5, k = K_CONST;
            }
            int take = (h.slot[0] || h.slot[1]) && (COND(k) || all);
            if (take && imitate && !clamp && h.slot[1]) take = 0; /* min(0, 0): nothing to lose */
            if (take) {
                if (nhits == cap_hits) {
                    cap_hits = cap_hits ? cap_hits * 2 : 32;
                    hit *nh = realloc(hits, cap_hits * sizeof(*nh));
                    if (!nh) {
                        *why = "out of memory";
                        rc = -1;
                        goto done;
                    }
                    hits = nh;
                }
                hits[nhits++] = h;
                st->ops++;
                if (clamp) st->clamps++;
                else st->minmax++;
                if (!COND(k)) st->by_all++;
                else if (k == K_SELECT) st->by_select++;
                else if (k == K_PHI) st->by_phi++;
                else st->by_memory++;
            }
        }
        i += len;
    }
    if (!nhits) goto done;

    if (spv_parse(&m, code, size)) {
        *why = "malformed module";
        rc = -1;
        goto done;
    }
    have_mod = 1;

    if (imitate) {
        /* the zero limit lost */
        for (uint32_t h = 0; h < nhits; h++) {
            spv_inst *in = &m.ins[hits[h].ins];
            uint32_t g = in->w[3], ops[7];
            memcpy(ops, in->w, 4 * 4);
            if (is_clamp(g) && hits[h].slot[0] && !hits[h].slot[1]) { /* clamp(x, 0, hi) -> min(x, hi) */
                ops[3] = clamp_min(g), ops[4] = in->w[4], ops[5] = in->w[6];
                set_inst(&m, hits[h].ins, OP_EXT_INST, ops, 6);
            } else if (is_clamp(g) && hits[h].slot[1] && !hits[h].slot[0]) { /* clamp(x, lo, 0) -> max(x, lo) */
                ops[3] = clamp_max(g), ops[4] = in->w[4], ops[5] = in->w[5];
                set_inst(&m, hits[h].ins, OP_EXT_INST, ops, 6);
            } else { /* clamp(x, 0, 0), max(x, 0), min(x, 0) -> x */
                uint32_t x = is_clamp(g) ? in->w[4] : in->w[hits[h].slot[0] == 4 ? 5 : 4];
                ops[2] = x;
                set_inst(&m, hits[h].ins, OP_COPY_OBJECT, ops, 3);
            }
        }
    } else {
        /* The scalar types needed, in order of first use; per function which (bit per type). */
        uint32_t types[MAX_TYPES], ntypes = 0;
        uint32_t *fused = calloc(nfuncs ? nfuncs : 1, sizeof(*fused));
        if (!fused) {
            *why = "out of memory";
            rc = -1;
            goto done;
        }
        for (uint32_t h = 0; h < nhits && !rc; h++) {
            const spv_inst *in = &m.ins[hits[h].ins];
            for (int s = 0; s < 2; s++) {
                if (!hits[h].slot[s]) continue;
                uint32_t t = ii[in->w[hits[h].slot[s]]].type, sc = t;
                if (t < bound && ii[t].op == OP_TYPE_VECTOR) {
                    if (!ii[t].lanes) {
                        *why = "a vector type with too many lanes";
                        rc = -1;
                        break;
                    }
                    sc = ii[t].type;
                }
                if (sc >= bound || (ii[sc].op != OP_TYPE_FLOAT && ii[sc].op != OP_TYPE_INT) || !ii[sc].width) {
                    *why = "a zero limit of a type that is not a number";
                    rc = -1;
                    break;
                }
                uint32_t k = 0;
                while (k < ntypes && types[k] != sc) k++;
                if (k == ntypes) {
                    if (ntypes == MAX_TYPES) {
                        *why = "too many scalar types";
                        rc = -1;
                        break;
                    }
                    types[ntypes++] = sc;
                }
                fused[hits[h].func] |= 1u << k;
            }
        }
        if (rc) {
            free(fused);
            goto done;
        }

        /* Globals: before the first function, so every function's ordinal moves by the same delta. */
        uint32_t before = m.count;
        uint32_t zc[MAX_TYPES], var[MAX_TYPES] = {0}, u32 = 0, u0 = 0;
        int bfi[MAX_TYPES];
        for (uint32_t k = 0; k < ntypes; k++) {
            uint32_t t = types[k], width = ii[t].width;
            zc[k] = zero_const(&m, t, width);
            bfi[k] = (flags & VKB_CLAMP_ZERO_BFI) && width == 32;
            if (bfi[k]) {
                if (!u32) u32 = spv_type_int(&m, 32, 0), u0 = spv_const_u32(&m, 0);
                continue;
            }
            uint32_t pt = spv_type_pointer(&m, SC_PRIVATE, t);
            var[k] = spv_new_id(&m);
            uint32_t ops[3] = {pt, var[k], SC_PRIVATE};
            spv_insert(&m, spv_types_end(&m), OP_VARIABLE, ops, 3);
            if (spv_version(&m) >= 0x10400) spv_add_interface(&m, var[k]);
        }
        uint32_t delta = m.count - before;
        for (uint32_t f = 0; f < nfuncs; f++) funcs[f].ins += delta, funcs[f].at += delta;
        for (uint32_t h = 0; h < nhits; h++) hits[h].ins += delta;

        /* Each function, the last first (its insertions move nothing before it): its pattern ops'
         * operands, then its opaque zeros at the top. */
        uint32_t h_end = nhits;
        for (int f = (int)nfuncs - 1; f >= 0 && !m.err; f--) {
            uint32_t h_begin = h_end;
            while (h_begin > 0 && hits[h_begin - 1].func == (uint32_t)f) h_begin--;
            int any_private = 0;
            for (uint32_t k = 0; k < ntypes; k++) any_private |= var[k] != 0;
            if (h_begin == h_end && !(funcs[f].entry && any_private)) continue;
            uint32_t z[MAX_TYPES] = {0};
            uint32_t buf[3 + MAX_LANES];
            uint32_t at = funcs[f].at;
            /* ids first, in order: scalar zeros, then the vectors as the ops ask for them */
            for (uint32_t k = 0; k < ntypes; k++)
                if (fused[f] >> k & 1) z[k] = spv_new_id(&m);
            if (h_begin < h_end) st->functions++;
            /* vectors: one per vector type in this function */
            uint32_t vt[MAX_TYPES * 2], vid[MAX_TYPES * 2], nv = 0;
            for (uint32_t h = h_begin; h < h_end; h++) {
                spv_inst *in = &m.ins[hits[h].ins];
                for (int s = 0; s < 2; s++) {
                    if (!hits[h].slot[s]) continue;
                    uint32_t t = ii[in->w[hits[h].slot[s]]].type, sc = t, k = 0;
                    if (ii[t].op == OP_TYPE_VECTOR) sc = ii[t].type;
                    while (types[k] != sc) k++;
                    uint32_t rep = z[k];
                    if (sc != t) {
                        uint32_t j = 0;
                        while (j < nv && vt[j] != t) j++;
                        if (j == nv) {
                            if (nv == MAX_TYPES * 2) {
                                m.err = 1;
                                break;
                            }
                            vt[nv] = t, vid[nv] = spv_new_id(&m), nv++;
                        }
                        rep = vid[j];
                    }
                    in->w[hits[h].slot[s]] = rep;
                }
            }
            /* the code at the top of the function */
            if (funcs[f].entry)
                for (uint32_t k = 0; k < ntypes; k++)
                    if (var[k]) {
                        uint32_t ops[2] = {var[k], zc[k]};
                        spv_insert(&m, at++, OP_STORE, ops, 2);
                    }
            for (uint32_t k = 0; k < ntypes; k++) {
                if (!z[k]) continue;
                if (!bfi[k]) {
                    uint32_t ops[3] = {types[k], z[k], var[k]};
                    spv_insert(&m, at++, OP_LOAD, ops, 3);
                } else if (ii[types[k]].op == OP_TYPE_INT) {
                    uint32_t ops[6] = {types[k], z[k], zc[k], zc[k], u0, u0};
                    spv_insert(&m, at++, OP_BIT_FIELD_INSERT, ops, 6);
                } else {
                    uint32_t c = spv_new_id(&m), b = spv_new_id(&m);
                    uint32_t o1[3] = {u32, c, zc[k]}, o2[6] = {u32, b, c, u0, u0, u0}, o3[3] = {types[k], z[k], b};
                    spv_insert(&m, at++, OP_BITCAST, o1, 3);
                    spv_insert(&m, at++, OP_BIT_FIELD_INSERT, o2, 6);
                    spv_insert(&m, at++, OP_BITCAST, o3, 3);
                }
            }
            for (uint32_t j = 0; j < nv; j++) {
                uint32_t sc = ii[vt[j]].type, k = 0;
                while (types[k] != sc) k++;
                buf[0] = vt[j], buf[1] = vid[j];
                for (uint32_t l = 0; l < ii[vt[j]].lanes; l++) buf[2 + l] = z[k];
                spv_insert(&m, at++, OP_COMPOSITE_CONSTRUCT, buf, (uint16_t)(2 + ii[vt[j]].lanes));
            }
            h_end = h_begin;
        }
        free(fused);
    }
    if (m.err) {
        *why = "out of memory";
        rc = -1;
        goto done;
    }
    if (m.header[3] > MAX_BOUND) {
        *why = "the new ids pass the bound limit";
        rc = -1;
        goto done;
    }
    *out_size = spv_write(&m, out);
    if (!*out_size) {
        *why = "out of memory";
        rc = -1;
        goto done;
    }
    rc = 1;
done:
    if (have_mod) spv_free(&m);
    if (rc != 1) {
        if (rc < 0) {
            unsigned z = st->zero_ops, s = st->spec_zero;
            memset(st, 0, sizeof(*st));
            st->zero_ops = z, st->spec_zero = s;
        }
    }
    free(ii);
    free(hits);
    free(funcs);
    return rc;
}
