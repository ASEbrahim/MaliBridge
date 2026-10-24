/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * MK-02: gl_Position.y negated in the last pre-rasterization stage (see spirv_flip.h). At each
 * site, for a block member (a loose output is the variable itself):
 *   %p  = OpAccessChain %_ptr_Output_v4float %out %uint_k
 *   %v  = OpLoad %v4float %p
 *   %y  = OpCompositeExtract %float %v 1
 *   %ny = OpFNegate %float %y                 (or, picked by ViewportIndex:
 *                                              %vi = OpLoad %int %viewport_index
 *                                              %sh = OpShiftRightLogical %uint %mask %vi
 *                                              %b  = OpBitwiseAnd %uint %sh %uint_1
 *                                              %c  = OpIEqual %bool %b %uint_1
 *                                              %s  = OpSelect %float %c %float_n1 %float_1
 *                                              %ny = OpFMul %float %y %s)
 *   %nv = OpCompositeInsert %v4float %ny %v 1
 *         OpStore %p %nv
 * and in a geometry shader, after the emit, OpStore %p %v.
 */
#include "spirv_flip.h"
#include "spirv_edit.h"

#include <stdlib.h>
#include <string.h>

enum {
    OP_TYPE_POINTER = 32, OP_FNEGATE = 127, OP_FMUL = 133, OP_SELECT = 169, OP_IEQUAL = 170, OP_SHR_LOGICAL = 194,
    OP_AND = 199, OP_EMIT_VERTEX = 218, OP_EMIT_STREAM_VERTEX = 220, OP_TYPE_STRUCT = 30,
    EXEC_MODE_XFB = 11, BUILTIN_VIEWPORT_INDEX = 10,
};

typedef struct {
    int ep;                 /* the OpEntryPoint's index */
    uint32_t fn;            /* its function */
    uint32_t pos_var, pos_member, member_type, vec_type, float_type;
    int in_block;
    uint32_t vp_var, vp_type;
    int xfb;
} target;

/* The entry point's name: *iface the first interface word, 1 when it is `name` */
static int ep_name(const spv_inst *in, const char *name, uint32_t *iface)
{
    if (in->n < 4) return 0;
    size_t cap = (size_t)(in->n - 3) * 4, len = strnlen((const char *)&in->w[2], cap);
    if (len == cap) return 0;
    *iface = 2 + (uint32_t)(len / 4 + 1);
    return !name || !strcmp((const char *)&in->w[2], name);
}

static int decorated_builtin(const spv_mod *m, uint32_t id, uint32_t builtin)
{
    for (uint32_t i = 0; i < m->count; i++) {
        const spv_inst *in = &m->ins[i];
        if (in->op == SpvOpDecorate && in->n >= 4 && in->w[0] == id && in->w[1] == SpvDecorationBuiltIn && in->w[2] == builtin)
            return 1;
    }
    return 0;
}

static int64_t member_builtin(const spv_mod *m, uint32_t st, uint32_t builtin)
{
    for (uint32_t i = 0; i < m->count; i++) {
        const spv_inst *in = &m->ins[i];
        if (in->op == SpvOpMemberDecorate && in->n >= 5 && in->w[0] == st && in->w[2] == SpvDecorationBuiltIn && in->w[3] == builtin)
            return in->w[1];
    }
    return -1;
}

static const spv_inst *def(const spv_mod *m, uint32_t id)
{
    int d = spv_find_def(m, id);
    return d < 0 ? NULL : &m->ins[d];
}

static int find_target(const spv_mod *m, uint32_t model, const char *entry, target *t, const char **why)
{
    memset(t, 0, sizeof(*t));
    t->ep = -1;
    uint32_t iface = 0;
    for (uint32_t i = 0; i < m->count && t->ep < 0; i++) {
        const spv_inst *in = &m->ins[i];
        if (in->op == SpvOpEntryPoint && in->w[0] == model && ep_name(in, entry, &iface)) t->ep = (int)i, t->fn = in->w[1];
    }
    if (t->ep < 0) {
        *why = "no such entry point";
        return -1;
    }
    const spv_inst *ep = &m->ins[t->ep];
    for (uint32_t i = 0; i < m->count; i++) {
        const spv_inst *in = &m->ins[i];
        if (in->op == SpvOpExecutionMode && in->n >= 3 && in->w[0] == t->fn && in->w[1] == EXEC_MODE_XFB) t->xfb = 1;
        if (in->op != SpvOpVariable || in->n < 4 || in->w[2] != SpvStorageClassOutput) continue;
        uint32_t var = in->w[1];
        int listed = 0;
        for (uint32_t k = iface; k + 1 < ep->n; k++)
            if (ep->w[k] == var) listed = 1;
        if (!listed) continue;
        const spv_inst *pt = def(m, in->w[0]);
        if (!pt || pt->op != OP_TYPE_POINTER) continue;
        uint32_t pointee = pt->w[2];
        if (decorated_builtin(m, var, SpvBuiltInPosition)) {
            t->pos_var = var, t->vec_type = pointee;
        } else if (decorated_builtin(m, var, BUILTIN_VIEWPORT_INDEX)) {
            t->vp_var = var, t->vp_type = pointee;
        } else {
            const spv_inst *st = def(m, pointee);
            if (!st || st->op != OP_TYPE_STRUCT) continue;
            int64_t k = member_builtin(m, pointee, SpvBuiltInPosition);
            if (k >= 0 && k + 1 < st->n) {
                t->pos_var = var, t->in_block = 1, t->pos_member = (uint32_t)k;
                t->member_type = t->vec_type = st->w[1 + k];
            }
        }
    }
    if (t->pos_var) {
        const spv_inst *v = def(m, t->vec_type);
        const spv_inst *f = v && v->op == SpvOpTypeVector ? def(m, v->w[1]) : NULL;
        if (!v || v->op != SpvOpTypeVector || v->w[2] != 4 || !f || f->op != SpvOpTypeFloat || f->w[1] != 32) {
            *why = "gl_Position is not a vec4 of 32-bit floats";
            return -1;
        }
        t->float_type = v->w[1];
    }
    if (t->vp_var) {
        const spv_inst *it = def(m, t->vp_type);
        if (!it || it->op != SpvOpTypeInt || it->w[1] != 32) t->vp_var = 0; /* not a scalar int: treated as not written */
    }
    return 0;
}

static void fill_info(const target *t, vkb_flip_info *info)
{
    if (!info) return;
    memset(info, 0, sizeof(*info));
    info->position = t->pos_var != 0;
    info->in_block = t->in_block;
    info->writes_vpindex = t->vp_var != 0;
    info->xfb = t->xfb;
}

int vkb_spv_flip_info(const uint32_t *code, size_t size, uint32_t model, const char *entry, vkb_flip_info *info, const char **why)
{
    spv_mod m;
    const char *w = "not SPIR-V";
    if (!why) why = &w;
    if (spv_parse(&m, code, size)) {
        spv_free(&m);
        *why = "not SPIR-V";
        return -1;
    }
    target t;
    int r = find_target(&m, model, entry, &t, why);
    if (!r) fill_info(&t, info);
    spv_free(&m);
    return r;
}

typedef struct {
    uint32_t ptr_type, idx;                            /* block member access */
    uint32_t uint_t, bool_t, mask_c, one_u, fneg1, fpos1; /* conditional */
    int conditional;
} ids;

static uint32_t emit(spv_mod *m, uint32_t *at, uint16_t op, const uint32_t *w, uint16_t n)
{
    spv_insert(m, (*at)++, op, w, n);
    return 0;
}

/* The prelude at `at`: returns the instructions inserted; *p_out / *v_out the pointer and the app's value. */
static uint32_t prelude(spv_mod *m, const target *t, const ids *c, uint32_t at, uint32_t *p_out, uint32_t *v_out)
{
    uint32_t start = at, p = t->pos_var;
    if (t->in_block) {
        p = spv_new_id(m);
        uint32_t w[4] = {c->ptr_type, p, t->pos_var, c->idx};
        emit(m, &at, SpvOpAccessChain, w, 4);
    }
    uint32_t v = spv_new_id(m), y = spv_new_id(m), ny = spv_new_id(m), nv = spv_new_id(m);
    emit(m, &at, SpvOpLoad, (uint32_t[]){t->vec_type, v, p}, 3);
    emit(m, &at, SpvOpCompositeExtract, (uint32_t[]){t->float_type, y, v, 1}, 4);
    if (!c->conditional) {
        emit(m, &at, OP_FNEGATE, (uint32_t[]){t->float_type, ny, y}, 3);
    } else {
        uint32_t vi = spv_new_id(m), sh = spv_new_id(m), b = spv_new_id(m), cc = spv_new_id(m), s = spv_new_id(m);
        emit(m, &at, SpvOpLoad, (uint32_t[]){t->vp_type, vi, t->vp_var}, 3);
        emit(m, &at, OP_SHR_LOGICAL, (uint32_t[]){c->uint_t, sh, c->mask_c, vi}, 4);
        emit(m, &at, OP_AND, (uint32_t[]){c->uint_t, b, sh, c->one_u}, 4);
        emit(m, &at, OP_IEQUAL, (uint32_t[]){c->bool_t, cc, b, c->one_u}, 4);
        emit(m, &at, OP_SELECT, (uint32_t[]){t->float_type, s, cc, c->fneg1, c->fpos1}, 5);
        emit(m, &at, OP_FMUL, (uint32_t[]){t->float_type, ny, y, s}, 4);
    }
    emit(m, &at, SpvOpCompositeInsert, (uint32_t[]){t->vec_type, nv, ny, v, 1}, 5);
    emit(m, &at, SpvOpStore, (uint32_t[]){p, nv}, 2);
    *p_out = p, *v_out = v;
    return at - start;
}

int vkb_spv_flip(const uint32_t *code, size_t size, uint32_t model, const char *entry, uint32_t mask, uint32_t count,
                 uint32_t **out, size_t *out_size, vkb_flip_info *info, const char **why)
{
    const char *w = NULL;
    if (!why) why = &w;
    if (info) memset(info, 0, sizeof(*info));
    if (model != SpvExecutionModelVertex && model != SpvExecutionModelTessellationEvaluation && model != SpvExecutionModelGeometry) {
        *why = "not a vertex, tessellation evaluation or geometry stage";
        return -1;
    }
    spv_mod m;
    if (spv_parse(&m, code, size)) {
        spv_free(&m);
        *why = "not SPIR-V";
        return -1;
    }
    target t;
    if (find_target(&m, model, entry, &t, why)) {
        spv_free(&m);
        return -1;
    }
    fill_info(&t, info);
    if (count > 32) count = 32;
    uint32_t all = count >= 32 ? 0xffffffffu : (1u << count) - 1;
    mask &= all;
    if (!t.vp_var) mask &= 1, all = 1; /* viewport 0 only */
    if (!t.pos_var || !mask) {
        spv_free(&m);
        return 0;
    }
    ids c = {0};
    c.conditional = mask != all;
    /* types and constants first: they go in before the first function and move the sites */
    if (t.in_block) {
        c.ptr_type = spv_type_pointer(&m, SpvStorageClassOutput, t.member_type);
        c.idx = spv_const_u32(&m, t.pos_member);
    }
    if (c.conditional) {
        c.uint_t = spv_type_int(&m, 32, 0);
        c.bool_t = spv_type_bool(&m);
        c.mask_c = spv_const_u32(&m, mask);
        c.one_u = spv_const_u32(&m, 1);
        c.fneg1 = spv_const_f32(&m, -1.0f);
        c.fpos1 = spv_const_f32(&m, 1.0f);
    }
    if (m.err) {
        spv_free(&m);
        *why = "out of memory";
        return -1;
    }
    /* the sites, from the last back (an insertion leaves the earlier ones where they are) */
    uint32_t *sites = malloc((m.count + 1) * sizeof(*sites)), ns = 0;
    if (!sites) {
        spv_free(&m);
        *why = "out of memory";
        return -1;
    }
    int in_fn = 0, gs = model == SpvExecutionModelGeometry;
    for (uint32_t i = 0; i < m.count; i++) {
        const spv_inst *in = &m.ins[i];
        if (in->op == SpvOpFunction) in_fn = in->n >= 3 && in->w[1] == t.fn ? 2 : 1;
        else if (in->op == SpvOpFunctionEnd) in_fn = 0;
        else if (gs ? (in_fn && (in->op == OP_EMIT_VERTEX || in->op == OP_EMIT_STREAM_VERTEX)) : (in_fn == 2 && in->op == SpvOpReturn))
            sites[ns++] = i;
    }
    if (!ns) {
        free(sites);
        spv_free(&m);
        *why = gs ? "no OpEmitVertex" : "no OpReturn in the entry point";
        return -1;
    }
    for (uint32_t k = ns; k-- > 0;) {
        uint32_t at = sites[k], p, v;
        if (gs) {
            uint32_t n = prelude(&m, &t, &c, at, &p, &v);
            spv_insert(&m, at + n + 1, SpvOpStore, (uint32_t[]){p, v}, 2); /* after the emit: the app's value back */
        } else {
            prelude(&m, &t, &c, at, &p, &v);
        }
    }
    free(sites);
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
    if (info) info->sites = ns, info->conditional = c.conditional;
    return 1;
}
