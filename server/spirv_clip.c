/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * Clip/cull distance (and geometry/tessellation point size) built-ins turned into ordinary
 * Location variables, for a driver without shaderClipDistance/shaderCullDistance (Mali-G720,
 * r44p1); server_emu_shader.c decides where and builds the fragment discard on top.
 *
 * A built-in comes as a loose variable (Zink, DXVK) or as a member of an interface block - the
 * vertex shader's gl_PerVertex output (glslang, Unity's HLSLcc), and after the per-vertex fold
 * (spirv_pervertex.c) every geometry/tessellation interface, arrayed gl_in[]/gl_out[] included.
 * Loose: BuiltIn becomes Location. Member: it moves out to its own Location variable, the block's
 * variable gets a struct without it (later members renumbered in decorations and access chains),
 * whole loads and stores of the block go member by member. Mali's compiler, handed ULTRAKILL's
 * Unity vertex shader with ClipDistance in gl_PerVertex and no ClipDistance capability, read
 * null at 0x40 and took the session down (tests/corpus/ultrakill_unity_vs_clipblock.spv).
 *
 * Anything of an unexpected shape stops that built-in only (the caller is told why); the rest of
 * the module is still rewritten.
 */
#include "spirv_clip.h"

#include <stdlib.h>
#include <string.h>

#define MAXM 16  /* members of a block */
#define MAXA 32  /* vertices of an arrayed interface */
#define MAXMEM 4 /* memory operand words of a whole load/store */

static const uint32_t builtins[3] = {SpvBuiltInClipDistance, SpvBuiltInCullDistance, SpvBuiltInPointSize};

static int per_vertex_io(int model, uint32_t storage)
{
    if (storage == SpvStorageClassInput)
        return model == SpvExecutionModelTessellationControl || model == SpvExecutionModelTessellationEvaluation ||
               model == SpvExecutionModelGeometry;
    return model == SpvExecutionModelTessellationControl;
}

static int point_size_stage(int model)
{
    return model == SpvExecutionModelTessellationControl || model == SpvExecutionModelTessellationEvaluation ||
           model == SpvExecutionModelGeometry;
}

static int has_point_size_cap(const spv_mod *m)
{
    return spv_has_capability(m, SpvCapabilityGeometryPointSize) || spv_has_capability(m, SpvCapabilityTessellationPointSize);
}

static uint32_t def_op(const spv_mod *m, uint32_t id)
{
    int d = spv_find_def(m, id);
    return d < 0 ? 0 : m->ins[d].op;
}

static uint32_t var_pointee(const spv_mod *m, uint32_t var)
{
    int d = spv_find_def(m, var);
    if (d < 0 || m->ins[d].op != SpvOpVariable) return 0;
    int p = spv_find_def(m, m->ins[d].w[0]);
    return p < 0 || m->ins[p].op != SpvOpTypePointer ? 0 : m->ins[p].w[2];
}

/* The struct of a block variable (one array level unwrapped for per-vertex ones), or 0. */
static uint32_t var_struct(const spv_mod *m, uint32_t var, int arrayed)
{
    uint32_t t = var_pointee(m, var);
    int d = spv_find_def(m, t);
    if (d >= 0 && arrayed && m->ins[d].op == SpvOpTypeArray) d = spv_find_def(m, m->ins[d].w[1]);
    return d >= 0 && m->ins[d].op == SpvOpTypeStruct ? m->ins[d].w[0] : 0;
}

/* Built-in decorations of `b` anywhere (variables or struct members). */
static int builtin_left(const spv_mod *m, uint32_t b)
{
    for (uint32_t i = 0; i < m->count; i++) {
        const spv_inst *in = &m->ins[i];
        if (in->op == SpvOpDecorate && in->n >= 4 && in->w[1] == SpvDecorationBuiltIn && in->w[2] == b) return 1;
        if (in->op == SpvOpMemberDecorate && in->n >= 5 && in->w[2] == SpvDecorationBuiltIn && in->w[3] == b) return 1;
    }
    return 0;
}

int vkb_spv_clip_needed(const spv_mod *m, unsigned what)
{
    if ((what & VKB_SPV_CLIP) && (spv_has_capability(m, SpvCapabilityClipDistance) || builtin_left(m, SpvBuiltInClipDistance)))
        return 1;
    if ((what & VKB_SPV_CULL) && (spv_has_capability(m, SpvCapabilityCullDistance) || builtin_left(m, SpvBuiltInCullDistance)))
        return 1;
    return (what & VKB_SPV_POINT_SIZE) && point_size_stage(spv_execution_model(m)) && has_point_size_cap(m);
}

int vkb_spv_clip_max_location(const spv_mod *m)
{
    int model = spv_execution_model(m);
    int i = spv_max_location_io(m, SpvStorageClassInput, per_vertex_io(model, SpvStorageClassInput));
    int o = spv_max_location_io(m, SpvStorageClassOutput, per_vertex_io(model, SpvStorageClassOutput));
    return i > o ? i : o;
}

static void decorate(spv_mod *m, uint32_t id, uint32_t dec, uint32_t value)
{
    uint32_t ops[3] = {id, dec, value};
    spv_insert(m, spv_annotations_end(m), SpvOpDecorate, ops, 3);
}

/* Locations taken by one vertex's value of type t (float[n] -> n, scalar -> 1). */
static uint32_t slots(const spv_mod *m, uint32_t t)
{
    uint32_t n = spv_array_length(m, t);
    return n ? n : 1;
}

/* Leading operand words of a function-body instruction that can hold ids; the rest are literals. */
static uint32_t id_words(const spv_inst *in)
{
    switch (in->op) {
    case SpvOpCompositeExtract: return 3;
    case SpvOpCompositeInsert: return 4;
    case 79 /* VectorShuffle */: return 4;
    case SpvOpExtInst: return 3;
    case SpvOpLoad: return 3;
    case SpvOpStore: return 2;
    case 63 /* CopyMemory */: return 2;
    case 251 /* Switch */: return 2;
    case 246 /* LoopMerge */: return 2;
    case SpvOpSelectionMerge: return 1;
    case SpvOpBranchConditional: return 3;
    default: return UINT16_MAX;
    }
}

static int const_u32(const spv_mod *m, uint32_t id, uint32_t *v)
{
    int d = spv_find_def(m, id);
    if (d < 0 || m->ins[d].op != SpvOpConstant || m->ins[d].n != 4) return 0;
    *v = m->ins[d].w[2];
    return 1;
}

/* Every use of the block variable is one demote_member rewrites: access chains that reach a
 * member, whole loads and stores; no decoration of its own but names. */
static const char *member_uses_ok(const spv_mod *m, uint32_t var, int arrayed)
{
    int in_body = 0;
    for (uint32_t i = 0; i < m->count; i++) {
        const spv_inst *in = &m->ins[i];
        if (in->op == SpvOpFunction) in_body = 1;
        if (!in_body) {
            if ((in->op == SpvOpDecorate || in->op == SpvOpDecorateId || in->op == SpvOpDecorateString) && in->w[0] == var)
                return "a decorated block variable";
            if (in->op == SpvOpVariable && in->w[1] == var && in->n > 4) return "a block variable with an initializer";
            if (in->op == SpvOpVariable && in->n > 4 && in->w[3] == var) return "a block variable named by a global";
            continue;
        }
        uint32_t lim = id_words(in), words = (uint32_t)in->n - 1;
        if (lim > words) lim = words;
        for (uint32_t k = 0; k < lim; k++) {
            if (in->w[k] != var) continue;
            if ((in->op == SpvOpAccessChain || in->op == SpvOpInBoundsAccessChain) && k == 2) {
                uint32_t c;
                if (words < (arrayed ? 5u : 4u)) return "an access chain to a whole block";
                if (!const_u32(m, in->w[arrayed ? 4 : 3], &c)) return "a block member index that is not a constant";
                continue;
            }
            if (in->op == SpvOpLoad && k == 2 && words <= 3 + MAXMEM) continue;
            if (in->op == SpvOpStore && k == 0 && words <= 2 + MAXMEM) continue;
            return "a use of a block the rewrite does not handle";
        }
    }
    return NULL;
}

/* Input/Output variables whose type holds struct st. */
static int interface_vars_of(const spv_mod *m, uint32_t st)
{
    int n = 0;
    for (uint32_t i = 0; i < m->count; i++) {
        const spv_inst *in = &m->ins[i];
        if (in->op != SpvOpVariable || (in->w[2] != SpvStorageClassInput && in->w[2] != SpvStorageClassOutput)) continue;
        if (var_struct(m, in->w[1], 0) == st || var_struct(m, in->w[1], 1) == st) n++;
    }
    return n;
}

static void remove_interface(spv_mod *m, uint32_t var)
{
    for (uint32_t i = 0; i < m->count; i++) {
        spv_inst *in = &m->ins[i];
        if (in->op != SpvOpEntryPoint) continue;
        /* model, function, name (literal words up to the one holding its terminating zero), interface */
        uint32_t k = 2;
        while (k < (uint32_t)in->n - 1) {
            uint32_t word = in->w[k++];
            if (!(word & 0xff000000u) || !(word & 0x00ff0000u) || !(word & 0x0000ff00u) || !(word & 0xffu)) break;
        }
        uint32_t o = k;
        for (; k < (uint32_t)in->n - 1; k++)
            if (in->w[k] != var) in->w[o++] = in->w[k];
        in->n = (uint16_t)(o + 1);
    }
}

/* Any function-body instruction naming the variable. */
static int var_used(const spv_mod *m, uint32_t var)
{
    int in_body = 0;
    for (uint32_t i = 0; i < m->count; i++) {
        const spv_inst *in = &m->ins[i];
        if (in->op == SpvOpFunction) in_body = 1;
        if (!in_body) continue;
        uint32_t lim = id_words(in), words = (uint32_t)in->n - 1;
        if (lim > words) lim = words;
        for (uint32_t k = 0; k < lim; k++)
            if (in->w[k] == var) return 1;
    }
    return 0;
}

/* A variable nothing reads or writes goes, with its decorations, names and interface entry. */
static void drop_var(spv_mod *m, uint32_t var)
{
    for (uint32_t i = 0; i < m->count;) {
        const spv_inst *in = &m->ins[i];
        int named = in->op == SpvOpDecorate || in->op == SpvOpDecorateId || in->op == SpvOpDecorateString || in->op == SpvOpName;
        if ((named && in->w[0] == var) || (in->op == SpvOpVariable && in->w[1] == var)) spv_remove(m, i);
        else i++;
    }
    remove_interface(m, var);
}

/* A loose built-in variable: BuiltIn -> Location. Returns its slot count, 0 when it was unused
 * and dropped, -1 if of a shape not taken. */
static int demote_loose(spv_mod *m, uint32_t var, int arrayed, uint32_t loc)
{
    uint32_t t = var_pointee(m, var);
    if (arrayed) {
        int a = spv_find_def(m, t);
        if (a < 0 || m->ins[a].op != SpvOpTypeArray) return -1;
        t = m->ins[a].w[1];
    }
    if (!var_used(m, var)) {
        drop_var(m, var);
        return 0;
    }
    spv_remove_decoration(m, var, SpvDecorationBuiltIn);
    decorate(m, var, SpvDecorationLocation, loc);
    return (int)slots(m, t);
}

/* Member k is read or written: an access chain through it, or a whole load/store of the block. */
static int member_used(const spv_mod *m, uint32_t var, int arrayed, uint32_t k)
{
    int in_body = 0;
    for (uint32_t i = 0; i < m->count; i++) {
        const spv_inst *in = &m->ins[i];
        if (in->op == SpvOpFunction) in_body = 1;
        if (!in_body) continue;
        uint32_t c;
        if ((in->op == SpvOpAccessChain || in->op == SpvOpInBoundsAccessChain) && in->w[2] == var &&
            const_u32(m, in->w[arrayed ? 4 : 3], &c) && c == k)
            return 1;
        if ((in->op == SpvOpLoad && in->w[2] == var) || (in->op == SpvOpStore && in->w[0] == var)) return 1;
    }
    return 0;
}

/* Member k of block variable `var` moves out to a new Location variable (returned in *nvar);
 * one never accessed is only removed (glslang's gl_PerVertex declares ClipDistance and
 * CullDistance in about every vertex shader, without the capability, and writes neither: a
 * varying nobody writes must not feed the discard). Returns its slot count, 0 when it was
 * removed, -1 when the shape is not taken (*why set, module untouched). */
static int demote_member(spv_mod *m, uint32_t var, uint32_t storage, int arrayed, uint32_t k, uint32_t loc, uint32_t *nvar,
                         const char **why)
{
    uint32_t outer = var_pointee(m, var), st = var_struct(m, var, arrayed), len_c = 0, len = 1;
    if (!st || (arrayed ? def_op(m, outer) != SpvOpTypeArray : outer != st)) {
        *why = "a block of an unexpected shape";
        return -1;
    }
    if (arrayed) {
        len_c = m->ins[spv_find_def(m, outer)].w[2];
        len = spv_array_length(m, outer);
        if (!len || len > MAXA) {
            *why = "a per-vertex block array of an unexpected length";
            return -1;
        }
    }
    const spv_inst *sd = &m->ins[spv_find_def(m, st)];
    uint32_t nm = (uint32_t)sd->n - 2, mt[MAXM];
    if (nm > MAXM || k >= nm) {
        *why = "a block of an unexpected shape";
        return -1;
    }
    memcpy(mt, &sd->w[1], nm * 4);
    if (interface_vars_of(m, st) != 1) {
        *why = "a block type shared by several variables";
        return -1;
    }
    if ((*why = member_uses_ok(m, var, arrayed))) return -1;
    int used = member_used(m, var, arrayed, k);

    /* Types and constants first (they shift the body). */
    int empty = nm == 1;
    uint32_t st2 = 0, ptr2 = 0;
    if (!empty) {
        uint32_t sops[MAXM];
        st2 = spv_new_id(m);
        sops[0] = st2;
        for (uint32_t j = 0, o = 1; j < nm; j++)
            if (j != k) sops[o++] = mt[j];
        spv_insert(m, spv_types_end(m), SpvOpTypeStruct, sops, (uint16_t)nm);
        ptr2 = spv_type_pointer(m, storage, arrayed ? spv_type_array(m, st2, len_c) : st2);
    }
    uint32_t et = mt[k];
    uint32_t nptr = spv_type_pointer(m, storage, arrayed ? spv_type_array(m, et, len_c) : et);
    uint32_t mptr[MAXM], idx[MAXM], vert[MAXA];
    for (uint32_t j = 0; j < nm; j++) {
        mptr[j] = spv_type_pointer(m, storage, mt[j]);
        idx[j] = spv_const_u32(m, j);
    }
    for (uint32_t v = 0; v < len; v++) vert[v] = spv_const_u32(m, v);
    uint32_t nv = 0;
    if (used) {
        nv = spv_new_id(m);
        uint32_t vops[3] = {nptr, nv, storage};
        spv_insert(m, spv_types_end(m), SpvOpVariable, vops, 3);
    }
    /* The block variable is retyped, after its new type (ids are defined before use). */
    int vd = spv_find_def(m, var);
    spv_remove(m, (uint32_t)vd);
    if (!empty) {
        uint32_t ops[3] = {ptr2, var, storage};
        spv_insert(m, spv_types_end(m), SpvOpVariable, ops, 3);
    }

    /* Decorations and names: the block's go to the new struct (member k dropped, later ones
     * renumbered); the old struct stays a plain one for values of whole loads. */
    for (uint32_t i = 0; i < m->count;) {
        spv_inst *in = &m->ins[i];
        int member = in->op == SpvOpMemberDecorate || in->op == SpvOpMemberDecorateString || in->op == SpvOpMemberName;
        int whole = in->op == SpvOpDecorate || in->op == SpvOpDecorateId || in->op == SpvOpDecorateString;
        if ((member || whole) && in->w[0] == st) {
            if (empty || (member && in->w[1] == k)) {
                spv_remove(m, i);
                continue;
            }
            in->w[0] = st2;
            if (member && in->w[1] > k) in->w[1]--;
        } else if (empty && (in->op == SpvOpName || whole) && in->w[0] == var) {
            spv_remove(m, i);
            continue;
        }
        i++;
    }
    if (empty) remove_interface(m, var);
    if (nv) {
        decorate(m, nv, SpvDecorationLocation, loc);
        spv_add_interface(m, nv);
    }

    /* Uses. */
    int in_body = 0;
    for (uint32_t i = 0; i < m->count; i++) {
        spv_inst *in = &m->ins[i];
        if (in->op == SpvOpFunction) in_body = 1;
        if (!in_body) continue;
        if ((in->op == SpvOpAccessChain || in->op == SpvOpInBoundsAccessChain) && in->w[2] == var) {
            /* [type, result, base, (vertex,) member, rest...] */
            uint32_t mpos = arrayed ? 4 : 3, words = (uint32_t)in->n - 1, c = 0;
            const_u32(m, in->w[mpos], &c);
            if (c != k) {
                in->w[mpos] = idx[c > k ? c - 1 : c];
                continue;
            }
            in->w[2] = nv;
            memmove(&in->w[mpos], &in->w[mpos + 1], (words - mpos - 1) * 4);
            in->n--;
            continue;
        }
        int load = in->op == SpvOpLoad && in->w[2] == var, store = in->op == SpvOpStore && in->w[0] == var;
        if (!load && !store) continue;
        /* Whole block: member by member (and vertex by vertex). load [type, result, ptr, mem...],
         * store [ptr, object, mem...]. */
        uint32_t head = load ? 3 : 2, nt = (uint32_t)in->n - 1 - head, tail[MAXMEM];
        uint32_t type = in->w[0], id = in->w[1];
        memcpy(tail, &in->w[head], nt * 4);
        uint32_t at = i, vals[MAXA], ops[8 + MAXM + MAXA];
        for (uint32_t v = 0; v < len; v++) {
            uint32_t src = id, elem[MAXM];
            if (store && arrayed) {
                src = spv_new_id(m);
                uint32_t ex[4] = {st, src, id, v};
                spv_insert(m, at++, SpvOpCompositeExtract, ex, 4);
            }
            for (uint32_t j = 0; j < nm; j++) {
                uint32_t p = spv_new_id(m), o = 0;
                elem[j] = spv_new_id(m);
                ops[o++] = mptr[j];
                ops[o++] = p;
                ops[o++] = j == k ? nv : var;
                if (arrayed) ops[o++] = vert[v];
                if (j != k) ops[o++] = idx[j > k ? j - 1 : j];
                spv_insert(m, at++, SpvOpAccessChain, ops, (uint16_t)o);
                o = 0;
                if (store) {
                    uint32_t ex[4] = {mt[j], elem[j], src, j};
                    spv_insert(m, at++, SpvOpCompositeExtract, ex, 4);
                    ops[o++] = p;
                    ops[o++] = elem[j];
                } else {
                    ops[o++] = mt[j];
                    ops[o++] = elem[j];
                    ops[o++] = p;
                }
                memcpy(&ops[o], tail, nt * 4);
                spv_insert(m, at++, load ? SpvOpLoad : SpvOpStore, ops, (uint16_t)(o + nt));
            }
            if (load) {
                vals[v] = arrayed ? spv_new_id(m) : id;
                ops[0] = arrayed ? st : type;
                ops[1] = vals[v];
                memcpy(&ops[2], elem, nm * 4);
                spv_insert(m, at++, SpvOpCompositeConstruct, ops, (uint16_t)(2 + nm));
            }
        }
        if (load && arrayed) {
            ops[0] = type;
            ops[1] = id;
            memcpy(&ops[2], vals, len * 4);
            spv_insert(m, at++, SpvOpCompositeConstruct, ops, (uint16_t)(2 + len));
        }
        spv_remove(m, at); /* the original load/store */
        i = at - 1;
    }
    *nvar = nv;
    return used ? (int)slots(m, et) : 0;
}

/* A struct type no interface variable uses any more but still marked as a built-in block:
 * plain struct (the driver sees no built-in in it). */
static void strip_dead_blocks(spv_mod *m, uint32_t b)
{
    for (uint32_t i = 0; i < m->count; i++) {
        const spv_inst *in = &m->ins[i];
        if (in->op != SpvOpMemberDecorate || in->n < 5 || in->w[2] != SpvDecorationBuiltIn || in->w[3] != b) continue;
        uint32_t st = in->w[0];
        if (interface_vars_of(m, st)) continue;
        for (uint32_t j = 0; j < m->count;) {
            const spv_inst *d = &m->ins[j];
            if ((d->op == SpvOpMemberDecorate || d->op == SpvOpDecorate) && d->w[0] == st) spv_remove(m, j);
            else j++;
        }
        i = (uint32_t)-1;
    }
}

/* The next variable of storage s carrying built-in b: loose (*k = UINT32_MAX) or as block member *k. */
static uint32_t find_builtin(const spv_mod *m, uint32_t b, uint32_t s, int arrayed, uint32_t *k)
{
    for (uint32_t i = 0; i < m->count; i++) {
        const spv_inst *in = &m->ins[i];
        if (in->op != SpvOpVariable || in->w[2] != s) continue;
        uint32_t var = in->w[1];
        if (spv_get_decoration(m, var, SpvDecorationBuiltIn) == (int64_t)b) {
            *k = UINT32_MAX;
            return var;
        }
        uint32_t st = var_struct(m, var, arrayed);
        for (uint32_t j = 0; st && j < m->count; j++) {
            const spv_inst *d = &m->ins[j];
            if (d->op == SpvOpMemberDecorate && d->n >= 5 && d->w[0] == st && d->w[2] == SpvDecorationBuiltIn && d->w[3] == b) {
                *k = d->w[1];
                return var;
            }
        }
    }
    return 0;
}

int vkb_spv_clip_demote(spv_mod *m, unsigned what, uint32_t loc, const uint32_t *prev_out, vkb_spv_clip_info *info)
{
    memset(info, 0, sizeof(*info));
    int model = spv_execution_model(m);
    const uint32_t storages[2] = {SpvStorageClassOutput, SpvStorageClassInput};
    int left = 0;
    for (int s = 0; s < 2; s++) {
        uint32_t cur = loc, *count = s == 0 ? info->out : info->in;
        int arrayed = per_vertex_io(model, storages[s]);
        for (int b = 0; b < 3; b++) {
            if (!(what & (1u << b))) continue;
            if (b == 2 && !(point_size_stage(model) && has_point_size_cap(m))) continue;
            uint32_t start = cur;
            for (int guard = 0; guard < 8; guard++) {
                uint32_t k, var = find_builtin(m, builtins[b], storages[s], arrayed, &k), nvar = var;
                if (!var) break;
                const char *why = "a built-in variable of an unexpected shape";
                int n = k == UINT32_MAX ? demote_loose(m, var, arrayed, cur)
                                        : demote_member(m, var, storages[s], arrayed, k, cur, &nvar, &why);
                if (n < 0) {
                    info->why = why;
                    left = 1;
                    break;
                }
                count[b] += (uint32_t)n;
                cur += (uint32_t)n;
                if (b == 0 && s == 1 && n) info->clip_in_var = nvar;
            }
            /* Inputs: the next built-in starts where the previous stage's did. */
            if (s == 1 && prev_out && start + prev_out[b] > cur) cur = start + prev_out[b];
        }
    }
    static const uint32_t caps[3][2] = {{SpvCapabilityClipDistance, 0}, {SpvCapabilityCullDistance, 0},
                                        {SpvCapabilityGeometryPointSize, SpvCapabilityTessellationPointSize}};
    for (int b = 0; b < 3; b++) {
        if (!(what & (1u << b)) || (b == 2 && !point_size_stage(model))) continue;
        strip_dead_blocks(m, builtins[b]);
        if (builtin_left(m, builtins[b])) {
            if (b < 2) left = 1; /* point size may stay where the capability was absent: ignored there */
            if (b < 2 && !info->why) info->why = "a built-in the rewrite cannot reach";
            if (b < 2 || has_point_size_cap(m)) continue;
        }
        spv_remove_capability(m, caps[b][0]);
        if (caps[b][1]) spv_remove_capability(m, caps[b][1]);
    }
    return left || m->err ? -1 : 0;
}

int vkb_spv_clip_discard(spv_mod *m, uint32_t loc, uint32_t n, uint32_t var)
{
    uint32_t f32 = spv_type_float(m, 32);
    uint32_t pf = spv_type_pointer(m, SpvStorageClassInput, f32);
    uint32_t tbool = spv_type_bool(m);
    uint32_t zero = spv_const_f32(m, 0.0f);
    uint32_t idx[16];
    if (n > 16) n = 16;
    for (uint32_t i = 0; i < n; i++) idx[i] = spv_const_u32(m, i);
    int scalar = 0;
    if (var) {
        /* The shader's own gl_ClipDistance input, demoted: read it (its length is the stage's). */
        uint32_t len = spv_array_length(m, var_pointee(m, var));
        scalar = !len;
        if (n > (len ? len : 1)) n = len ? len : 1;
    } else {
        uint32_t arr = spv_type_array(m, f32, spv_const_u32(m, n));
        uint32_t parr = spv_type_pointer(m, SpvStorageClassInput, arr);
        var = spv_new_id(m);
        uint32_t vops[3] = {parr, var, SpvStorageClassInput};
        spv_insert(m, spv_types_end(m), SpvOpVariable, vops, 3);
        decorate(m, var, SpvDecorationLocation, loc);
        spv_add_interface(m, var);
    }

    int ep = spv_find_op(m, SpvOpEntryPoint, 0);
    if (ep < 0) return -1;
    uint32_t fn = m->ins[ep].w[1];
    int fi = -1;
    for (uint32_t i = 0; i < m->count; i++)
        if (m->ins[i].op == SpvOpFunction && m->ins[i].w[1] == fn) fi = (int)i;
    if (fi < 0) return -1;
    int li = spv_find_op(m, SpvOpLabel, (uint32_t)fi);
    if (li < 0) return -1;
    uint32_t first_label = m->ins[li].w[0];
    uint32_t at = (uint32_t)li + 1;
    while (at < m->count && (m->ins[at].op == SpvOpVariable || m->ins[at].op == SpvOpLine || m->ins[at].op == SpvOpNoLine)) at++;

    uint32_t any = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t p = spv_new_id(m), v = spv_new_id(m), lt = spv_new_id(m);
        if (scalar) {
            uint32_t ld[3] = {f32, v, var};
            at = spv_insert(m, at, SpvOpLoad, ld, 3) + 1;
        } else {
            uint32_t ac[4] = {pf, p, var, idx[i]};
            at = spv_insert(m, at, SpvOpAccessChain, ac, 4) + 1;
            uint32_t ld[3] = {f32, v, p};
            at = spv_insert(m, at, SpvOpLoad, ld, 3) + 1;
        }
        uint32_t cmp[4] = {tbool, lt, v, zero};
        at = spv_insert(m, at, SpvOpFOrdLessThan, cmp, 4) + 1;
        if (any) {
            uint32_t o = spv_new_id(m), orr[4] = {tbool, o, any, lt};
            at = spv_insert(m, at, SpvOpLogicalOr, orr, 4) + 1;
            any = o;
        } else {
            any = lt;
        }
    }
    uint32_t kill = spv_new_id(m), merge = spv_new_id(m);
    uint32_t sm[2] = {merge, 0};
    at = spv_insert(m, at, SpvOpSelectionMerge, sm, 2) + 1;
    uint32_t bc[3] = {any, kill, merge};
    at = spv_insert(m, at, SpvOpBranchConditional, bc, 3) + 1;
    at = spv_insert(m, at, SpvOpLabel, &kill, 1) + 1;
    at = spv_insert(m, at, spv_version(m) >= 0x00010600 ? SpvOpTerminateInvocation : SpvOpKill, NULL, 0) + 1;
    at = spv_insert(m, at, SpvOpLabel, &merge, 1) + 1;
    /* Phis that named the old entry block as a predecessor now come from the merge block. */
    for (uint32_t i = at; i < m->count; i++) {
        spv_inst *in = &m->ins[i];
        if (in->op != 245 /* OpPhi */) continue;
        for (uint32_t k = 3; k < (uint32_t)in->n - 1; k += 2)
            if (in->w[k] == first_label) in->w[k] = merge;
    }
    return m->err ? -1 : 0;
}
