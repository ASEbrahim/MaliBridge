/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * split_varying (MK-47): fragment inputs split by Component decorations at a Location the stage
 * before writes as one whole vector, merged into one input of that vector (spirv_splitvary.h).
 */
#include "spirv_splitvary.h"
#include "spirv_edit.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    OP_TYPE_POINTER = 32, OP_TYPE_VECTOR = 23, OP_TYPE_FLOAT = 22, OP_TYPE_INT = 21, OP_VARIABLE = 59, OP_DECORATE = 71,
    OP_ENTRY_POINT = 15, OP_CONSTANT_NULL = 46, OP_COPY_OBJECT = 83, OP_VECTOR_SHUFFLE = 79, OP_DECORATE_ID = 332,
    OP_DECORATE_STRING = 5632, OP_FUNCTION = 54,
};
enum { DEC_RELAXED = 0, DEC_NOPERSP = 13, DEC_FLAT = 14, DEC_CENTROID = 16, DEC_SAMPLE = 17 };

#define SCAN_MAX 512

typedef struct { uint32_t id, a, b; } triple;

static int find3(const triple *t, unsigned n, uint32_t id)
{
    for (unsigned i = 0; i < n; i++)
        if (t[i].id == id) return (int)i;
    return -1;
}

/* A 32-bit scalar or vector type: its component count and kind, or 0 */
static int scan_type(const triple *vec, unsigned nvec, const triple *scal, unsigned nscal, uint32_t type, uint8_t *width,
                     uint8_t *ctype)
{
    uint32_t n = 1, comp = type;
    int v = find3(vec, nvec, type);
    if (v >= 0) comp = vec[v].a, n = vec[v].b;
    int s = find3(scal, nscal, comp);
    if (s < 0 || n < 1 || n > 4) return 0;
    /* scal: a = kind (VKB_SV_*), b = width in bits */
    if (scal[s].b != 32) return 0;
    *width = (uint8_t)n;
    *ctype = (uint8_t)scal[s].a;
    return 1;
}

int vkb_spv_sv_scan(const uint32_t *code, size_t size, vkb_sv_info *info)
{
    memset(info, 0, sizeof(*info));
    info->model = -1;
    if (!code || size < 20 || (size & 3) || code[0] != 0x07230203u) return -1;
    triple loc[SCAN_MAX], comp[SCAN_MAX], var[SCAN_MAX], ptr[SCAN_MAX], vec[SCAN_MAX], scal[64];
    unsigned nloc = 0, ncomp = 0, nvar = 0, nptr = 0, nvec = 0, nscal = 0, entries = 0, over = 0;
    int model = -1;
    size_t words = size / 4;
    for (size_t pos = 5; pos < words;) {
        uint32_t n = code[pos] >> 16, op = code[pos] & 0xffff;
        if (n == 0 || pos + n > words) return -1;
        const uint32_t *w = code + pos + 1;
        switch (op) {
        case OP_ENTRY_POINT:
            if (n >= 3) model = (int)w[0];
            entries++;
            break;
        case OP_DECORATE:
            if (n >= 4 && w[1] == SpvDecorationLocation) {
                if (nloc < SCAN_MAX) loc[nloc++] = (triple){w[0], w[2], 0};
                else over = 1;
            } else if (n >= 4 && w[1] == SpvDecorationComponent) {
                if (ncomp < SCAN_MAX) comp[ncomp++] = (triple){w[0], w[2], 0};
                else over = 1;
            }
            break;
        case OP_TYPE_POINTER:
            if (n >= 4 && (w[1] == SpvStorageClassInput || w[1] == SpvStorageClassOutput)) {
                if (nptr < SCAN_MAX) ptr[nptr++] = (triple){w[0], w[1], w[2]};
                else over = 1;
            }
            break;
        case OP_TYPE_VECTOR:
            if (n >= 4) {
                if (nvec < SCAN_MAX) vec[nvec++] = (triple){w[0], w[1], w[2]};
                else over = 1;
            }
            break;
        case OP_TYPE_FLOAT:
        case OP_TYPE_INT:
            if (n >= 3) {
                uint32_t kind = op == OP_TYPE_FLOAT ? VKB_SV_FLOAT : (n >= 4 && w[2]) ? VKB_SV_SINT : VKB_SV_UINT;
                if (nscal < 64) scal[nscal++] = (triple){w[0], kind, w[1]};
                else over = 1;
            }
            break;
        case OP_VARIABLE:
            if (n >= 4 && (w[2] == SpvStorageClassInput || w[2] == SpvStorageClassOutput)) {
                if (nvar < SCAN_MAX) var[nvar++] = (triple){w[1], w[0], w[2]};
                else over = 1;
            }
            break;
        case OP_FUNCTION:
            pos = words; /* the interface is all declared before the first function */
            continue;
        }
        pos += n;
    }
    if (entries != 1 || over) return 0;
    info->model = model;
    int pre = model == SpvExecutionModelVertex || model == SpvExecutionModelTessellationEvaluation || model == SpvExecutionModelGeometry;
    int frag = model == SpvExecutionModelFragment;
    if (!pre && !frag) return 0;
    for (unsigned i = 0; i < nvar; i++) {
        uint32_t storage = var[i].b;
        if (pre ? storage != SpvStorageClassOutput : storage != SpvStorageClassInput) continue;
        int l = find3(loc, nloc, var[i].id);
        if (l < 0 || loc[l].a >= VKB_SV_LOCATIONS) continue;
        uint32_t L = loc[l].a;
        int c = find3(comp, ncomp, var[i].id);
        if (frag) {
            if (c >= 0 && comp[c].a > 0) info->split |= 1u << L;
            continue;
        }
        if (c >= 0) continue; /* a pre-rasterization output with a Component: not whole */
        int p = find3(ptr, nptr, var[i].a);
        uint8_t width, ctype;
        if (p < 0 || !scan_type(vec, nvec, scal, nscal, ptr[p].b, &width, &ctype)) continue;
        info->whole |= 1u << L;
        info->width[L] = width;
        info->ctype[L] = ctype;
    }
    /* a Location that some other output also uses (a component of it) is not one whole vector */
    if (pre)
        for (unsigned i = 0; i < nvar; i++) {
            if (var[i].b != SpvStorageClassOutput) continue;
            int l = find3(loc, nloc, var[i].id), c = find3(comp, ncomp, var[i].id);
            if (l >= 0 && c >= 0 && loc[l].a < VKB_SV_LOCATIONS) info->whole &= ~(1u << loc[l].a);
        }
    return 0;
}

/* ---- the rewrite ---- */

typedef struct piece {
    uint32_t id, type, loc, comp, n;
    uint8_t ctype;
    unsigned interp; /* bits: Flat, NoPerspective, Centroid, Sample */
    int bad;
    char why[96]; /* the first rule it breaks */
} piece;

static void set_bad(piece *p, const char *fmt, ...)
{
    if (!p->bad) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(p->why, sizeof(p->why), fmt, ap);
        va_end(ap);
    }
    p->bad = 1;
}

static const char *kind_name(uint8_t k)
{
    return k == VKB_SV_FLOAT ? "float" : k == VKB_SV_SINT ? "int" : k == VKB_SV_UINT ? "uint" : "?";
}

static int type_of_ptr(const spv_mod *m, uint32_t ptr_type, uint32_t *pointee)
{
    int p = spv_find_def(m, ptr_type);
    if (p < 0 || m->ins[p].op != SpvOpTypePointer || m->ins[p].n < 4) return 0;
    *pointee = m->ins[p].w[2];
    return 1;
}

/* 32-bit int/float scalar or vector: count and kind */
static int shape(const spv_mod *m, uint32_t type, uint32_t *n, uint8_t *ctype)
{
    int d = spv_find_def(m, type);
    if (d < 0) return 0;
    uint32_t comp = type;
    *n = 1;
    if (m->ins[d].op == SpvOpTypeVector) {
        comp = m->ins[d].w[1];
        *n = m->ins[d].w[2];
        d = spv_find_def(m, comp);
        if (d < 0) return 0;
    }
    const spv_inst *s = &m->ins[d];
    if (s->op == SpvOpTypeFloat && s->n >= 3 && s->w[1] == 32) *ctype = VKB_SV_FLOAT;
    else if (s->op == SpvOpTypeInt && s->n >= 4 && s->w[1] == 32) *ctype = s->w[2] ? VKB_SV_SINT : VKB_SV_UINT;
    else return 0;
    return *n >= 1 && *n <= 4;
}

/* The operand words [from, to) of an instruction that can name a variable; 0: none can (its id operands are
 * types, labels or functions, the rest literals). */
static int id_words(const spv_inst *in, uint32_t *from, uint32_t *to)
{
    uint32_t n = in->n > 0 ? (uint32_t)in->n - 1 : 0;
    switch (in->op) {
    case SpvOpMemberDecorate: /* struct type, member, decoration, literals */
    case SpvOpSelectionMerge: case 246 /* OpLoopMerge */: case SpvOpBranch: case SpvOpBranchConditional:
    case SpvOpFunction: case SpvOpLabel: case SpvOpReturn: case SpvOpFunctionEnd: case SpvOpKill: case 255 /* OpUnreachable */:
        return 0;
    case SpvOpDecorateString: /* target, decoration, string words */
        *from = 0, *to = n < 1 ? n : 1;
        return 1;
    case SpvOpExtInst: /* type, result, set, instruction number (literal), operand ids */
        *from = 0, *to = n;
        return 1;
    case SpvOpCompositeExtract: /* type, result, composite, literal indices */
        *from = 0, *to = n < 3 ? n : 3;
        return 1;
    case SpvOpCompositeInsert: /* type, result, object, composite, literal indices */
    case OP_VECTOR_SHUFFLE:   /* type, result, vector 1, vector 2, literal components */
        *from = 0, *to = n < 4 ? n : 4;
        return 1;
    case SpvOpStore:      /* pointer, object, memory operands */
    case 63 /* OpCopyMemory */: /* target, source, memory operands */
        *from = 0, *to = n < 2 ? n : 2;
        return 1;
    case 251 /* OpSwitch */: /* selector, default, literal/label pairs */
        *from = 0, *to = n < 1 ? n : 1;
        return 1;
    case SpvOpVariable: /* in a function: type, result, storage class (literal), initializer */
        *from = 0, *to = n;
        return 1;
    default:
        *from = 0, *to = n;
        return 1;
    }
}

/* Literal words inside [from, to) of id_words(): the instruction number of OpExtInst, the storage class of
 * OpVariable, the image-operand mask of the image instructions */
static int literal_word(const spv_inst *in, uint32_t j)
{
    switch (in->op) {
    case SpvOpExtInst: return j == 3;
    case SpvOpVariable: return j == 2;
    case 87: case 88: case 91:
    case 92: case 95: case 98:
        return j == 4;
    case 89: case 90: case 93:
    case 94: case 96: case 97:
        return j == 5;
    case 99: return j == 3;
    default: return 0;
    }
}

static int is_global_decl(uint16_t op)
{
    return (op >= 19 && op <= 52) || op == SpvOpUndef || op == 73 /* TypeForwardPointer */;
}

int vkb_spv_split_varying(const uint32_t *code, size_t size, const vkb_sv_info *prev, unsigned flags, uint32_t **out,
                          size_t *out_size, vkb_splitvary_stats *st, const char **why)
{
    vkb_splitvary_stats dummy;
    if (!st) st = &dummy;
    memset(st, 0, sizeof(*st));
    *out = NULL;
    *out_size = 0;
    *why = "";
    vkb_sv_info self;
    if (!prev || !prev->whole || vkb_spv_sv_scan(code, size, &self) < 0 || self.model != SpvExecutionModelFragment) return 0;
    uint32_t cand = self.split & prev->whole;
    if (!cand) return 0;
    spv_mod m;
    if (spv_parse(&m, code, size) < 0) {
        spv_free(&m);
        *why = "not SPIR-V it can read";
        return -1;
    }
    piece pc[64];
    unsigned np = 0;
    uint32_t types_end = spv_types_end(&m);
    /* the pieces: every Input variable at a candidate Location */
    for (uint32_t i = 0; i < types_end; i++) {
        const spv_inst *in = &m.ins[i];
        if (in->op != SpvOpVariable || in->n < 4 || in->w[2] != SpvStorageClassInput) continue;
        int64_t L = spv_get_decoration(&m, in->w[1], SpvDecorationLocation);
        if (L < 0 || L >= VKB_SV_LOCATIONS || !(cand >> L & 1)) continue;
        if (np == 64) {
            spv_free(&m);
            *why = "more than 64 inputs";
            return -1;
        }
        piece *p = &pc[np++];
        memset(p, 0, sizeof(*p));
        p->id = in->w[1];
        p->loc = (uint32_t)L;
        int64_t c = spv_get_decoration(&m, p->id, SpvDecorationComponent);
        p->comp = c < 0 ? 0 : (uint32_t)c;
        if (!type_of_ptr(&m, in->w[0], &p->type) || !shape(&m, p->type, &p->n, &p->ctype))
            set_bad(p, "type: not a 32-bit int/float scalar or vector (id %u)", p->id);
        else if (p->ctype != prev->ctype[L])
            set_bad(p, "type: %s%u here, the stage before writes %s%u", kind_name(p->ctype), p->n, kind_name(prev->ctype[L]),
                    prev->width[L]);
        else if (p->comp + p->n > prev->width[L])
            set_bad(p, "type: reads components %u-%u, the stage before writes %u", p->comp, p->comp + p->n - 1, prev->width[L]);
    }
    /* decorations and uses */
    for (uint32_t i = 0; i < m.count; i++) {
        const spv_inst *in = &m.ins[i];
        switch (in->op) { /* literals only, or names (an OpName of a piece goes with it) */
        case SpvOpName: case SpvOpMemberName: case SpvOpEntryPoint: case SpvOpSource: case SpvOpSourceExtension: case SpvOpSourceContinued:
        case SpvOpString: case SpvOpModuleProcessed: case SpvOpExtension: case SpvOpExtInstImport: case SpvOpCapability:
        case SpvOpMemoryModel: case SpvOpExecutionMode: case SpvOpLine: case SpvOpNoLine:
            continue;
        }
        if (in->op == SpvOpDecorate) {
            for (unsigned k = 0; k < np; k++) {
                if (in->w[0] != pc[k].id) continue;
                switch (in->w[1]) {
                case SpvDecorationLocation: case SpvDecorationComponent: case DEC_RELAXED: break;
                case DEC_FLAT: pc[k].interp |= 1; break;
                case DEC_NOPERSP: pc[k].interp |= 2; break;
                case DEC_CENTROID: pc[k].interp |= 4; break;
                case DEC_SAMPLE: pc[k].interp |= 8; break;
                default: set_bad(&pc[k], "decoration %u", in->w[1]);
                }
            }
            continue;
        }
        if (i < types_end && is_global_decl(in->op)) continue; /* types, constants: no variable operands */
        if (in->op == SpvOpVariable && i < types_end) {
            /* an initializer naming a piece */
            for (unsigned k = 0; k < np; k++)
                if (in->n >= 5 && in->w[3] == pc[k].id) set_bad(&pc[k], "use: the initializer of variable %u", in->w[1]);
            continue;
        }
        if (in->op == SpvOpLoad && in->n >= 4) {
            for (unsigned k = 0; k < np; k++)
                if (in->w[0] == pc[k].id || in->w[1] == pc[k].id) set_bad(&pc[k], "use: OpLoad word %u", in->w[0] == pc[k].id ? 1u : 2u);
            continue;
        }
        /* every other instruction: a piece named in an id operand is a use the rewrite does not follow. Literal
         * operands are numbers, not ids: a literal equal to a piece's id is no use of it (a player log) */
        uint32_t from = 0, to = in->n > 0 ? (uint32_t)in->n - 1 : 0;
        if (!id_words(in, &from, &to)) continue;
        for (uint32_t j = from; j < to; j++)
            for (unsigned k = 0; k < np; k++)
                if (in->w[j] == pc[k].id && !literal_word(in, j))
                    set_bad(&pc[k], "use: op %u word %u (instruction %u)", in->op, j + 1, i);
    }
    /* per Location: all pieces good, one interpolation, at least one above component 0 */
    uint32_t merge = 0;
    for (uint32_t L = 0; L < VKB_SV_LOCATIONS; L++) {
        if (!(cand >> L & 1)) continue;
        int ok = 1, any = 0, high = 0;
        unsigned interp = 0;
        uint32_t used = 0;
        char lw[112] = "";
        for (unsigned k = 0; k < np; k++) {
            if (pc[k].loc != L) continue;
            uint32_t bits = ((1u << pc[k].n) - 1) << pc[k].comp;
            if (ok && pc[k].bad) snprintf(lw, sizeof(lw), "component %u: %s", pc[k].comp, pc[k].why);
            else if (ok && (used & bits)) snprintf(lw, sizeof(lw), "component %u: overlaps another input", pc[k].comp);
            else if (ok && any && pc[k].interp != interp)
                snprintf(lw, sizeof(lw), "component %u: interpolation 0x%x, another input 0x%x (flat 1, noperspective 2, centroid 4, sample 8)",
                         pc[k].comp, pc[k].interp, interp);
            if (pc[k].bad || (used & bits) || (any && pc[k].interp != interp)) ok = 0;
            used |= bits;
            interp = pc[k].interp;
            any = 1;
            high |= pc[k].comp > 0;
        }
        if (ok && any && high) merge |= 1u << L;
        else {
            if (!st->skipped)
                snprintf(st->why, sizeof(st->why), "location %u (the stage before writes %s%u): %s", L, kind_name(prev->ctype[L]),
                         prev->width[L], lw[0] ? lw : "no input above component 0 left");
            st->skipped |= 1u << L;
        }
    }
    if (!merge) {
        spv_free(&m);
        if (st->skipped) *why = st == &dummy ? "a split input the merge does not take" : st->why;
        return st->skipped ? -1 : 0;
    }
    int imitate = (flags & VKB_SPLITVARY_IMITATE) != 0;
    uint32_t whole_var[VKB_SV_LOCATIONS] = {0}, whole_type[VKB_SV_LOCATIONS] = {0};
    if (!imitate)
        for (uint32_t L = 0; L < VKB_SV_LOCATIONS; L++) {
            if (!(merge >> L & 1)) continue;
            uint32_t ct = prev->ctype[L] == VKB_SV_FLOAT ? spv_type_float(&m, 32) : spv_type_int(&m, 32, prev->ctype[L] == VKB_SV_SINT);
            uint32_t t = prev->width[L] > 1 ? spv_type_vector(&m, ct, prev->width[L]) : ct;
            uint32_t pt = spv_type_pointer(&m, SpvStorageClassInput, t);
            uint32_t v = spv_new_id(&m), ops[3] = {pt, v, SpvStorageClassInput};
            spv_insert(&m, spv_types_end(&m), SpvOpVariable, ops, 3);
            uint32_t at = spv_annotations_end(&m);
            uint32_t d[3] = {v, SpvDecorationLocation, L};
            spv_insert(&m, at++, SpvOpDecorate, d, 3);
            unsigned interp = 0;
            for (unsigned k = 0; k < np; k++)
                if (pc[k].loc == L) interp = pc[k].interp;
            static const uint32_t dec[4] = {DEC_FLAT, DEC_NOPERSP, DEC_CENTROID, DEC_SAMPLE};
            for (int b = 0; b < 4; b++)
                if (interp >> b & 1) {
                    uint32_t dd[2] = {v, dec[b]};
                    spv_insert(&m, at++, SpvOpDecorate, dd, 2);
                }
            whole_var[L] = v, whole_type[L] = t;
            st->locations |= 1u << L;
        }
    /* loads */
    for (uint32_t i = 0; i < m.count; i++) {
        spv_inst *in = &m.ins[i];
        if (in->op != SpvOpLoad || in->n < 4) continue;
        int k = -1;
        for (unsigned j = 0; j < np; j++)
            if (in->w[2] == pc[j].id && (merge >> pc[j].loc & 1)) k = (int)j;
        if (k < 0) continue;
        const piece *p = &pc[k];
        uint32_t type = in->w[0], res = in->w[1];
        if (imitate) {
            if (!p->comp) continue;
            uint32_t null = 0;
            for (uint32_t j = 0; j < m.count && !null; j++)
                if (m.ins[j].op == OP_CONSTANT_NULL && m.ins[j].w[0] == type) null = m.ins[j].w[1];
            if (!null) {
                null = spv_new_id(&m);
                uint32_t ops[2] = {type, null};
                spv_insert(&m, spv_types_end(&m), OP_CONSTANT_NULL, ops, 2);
                i++; /* the load moved down by one */
            }
            uint32_t ops[3] = {type, res, null};
            spv_remove(&m, i);
            spv_insert(&m, i, OP_COPY_OBJECT, ops, 3);
            st->loads++;
            st->locations |= 1u << p->loc;
            continue;
        }
        uint32_t L = p->loc, tmp = spv_new_id(&m);
        uint32_t ld[3] = {whole_type[L], tmp, whole_var[L]};
        spv_remove(&m, i);
        spv_insert(&m, i, SpvOpLoad, ld, 3);
        if (p->n == prev->width[L]) {
            uint32_t ops[3] = {type, res, tmp};
            spv_insert(&m, i + 1, OP_COPY_OBJECT, ops, 3);
        } else if (p->n == 1) {
            uint32_t ops[4] = {type, res, tmp, p->comp};
            spv_insert(&m, i + 1, SpvOpCompositeExtract, ops, 4);
        } else {
            uint32_t ops[8] = {type, res, tmp, tmp};
            for (uint32_t c = 0; c < p->n; c++) ops[4 + c] = p->comp + c;
            spv_insert(&m, i + 1, OP_VECTOR_SHUFFLE, ops, (uint16_t)(4 + p->n));
        }
        i++;
        st->loads++;
    }
    if (!imitate) {
        /* the pieces go: their variables, decorations, names and entry-point operands */
        for (uint32_t i = 0; i < m.count;) {
            spv_inst *in = &m.ins[i];
            int drop = 0;
            for (unsigned k = 0; k < np && !drop; k++) {
                if (!(merge >> pc[k].loc & 1)) continue;
                uint32_t id = pc[k].id;
                if ((in->op == SpvOpVariable && in->n >= 3 && in->w[1] == id) || (in->op == SpvOpDecorate && in->w[0] == id) ||
                    (in->op == SpvOpName && in->w[0] == id))
                    drop = 1;
            }
            if (drop) {
                spv_remove(&m, i);
                continue;
            }
            if (in->op == SpvOpEntryPoint) {
                /* operands: model, id, name words (nul-terminated), interface ids */
                uint32_t j = 2;
                while (j < (uint32_t)in->n - 1 && memchr(&in->w[j], 0, 4) == NULL) j++;
                j++; /* past the word with the nul */
                uint32_t o = j;
                for (uint32_t r = j; r < (uint32_t)in->n - 1; r++) {
                    int gone = 0;
                    for (unsigned k = 0; k < np; k++)
                        if ((merge >> pc[k].loc & 1) && in->w[r] == pc[k].id) gone = 1;
                    if (!gone) in->w[o++] = in->w[r];
                }
                in->n = (uint16_t)(o + 1);
            }
            i++;
        }
        for (uint32_t L = 0; L < VKB_SV_LOCATIONS; L++)
            if (whole_var[L]) spv_add_interface(&m, whole_var[L]);
        for (unsigned k = 0; k < np; k++)
            if (merge >> pc[k].loc & 1) st->pieces++;
    }
    if (m.err) {
        spv_free(&m);
        *why = "out of memory";
        return -1;
    }
    *out_size = spv_write(&m, out);
    spv_free(&m);
    if (!*out_size) {
        *why = "out of memory";
        return -1;
    }
    return 1;
}
