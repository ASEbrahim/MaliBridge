/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A geometry shader that only passes triangles through and sets the layer, folded into the vertex
 * shader. Unreal Engine 4 renders volume textures (the colour-grading LUT among them) with such a
 * pair: the vertex shader computes the slice into an ordinary varying, the geometry shader copies
 * every vertex unchanged and writes that varying to Layer (DXVK keeps the D3D11 GS). The
 * Mali-G925's compiler (Android 16) never finished compiling two of Stray's pipelines built this
 * way; without the geometry stage they are a plain vertex + fragment pipeline, and on Mali a
 * geometry stage is slow anyway. With shaderOutputLayer the vertex shader writes Layer itself.
 *
 * Recognized narrowly (anything else is left alone): triangles in, a triangle strip of 3 out, one
 * invocation; the body is straight-line code that, for vertex k = 0, 1, 2, loads inputs of vertex
 * k (OpSelect with a constant condition, as DXVK guards its vertex count, is looked through) and
 * stores them to outputs, then emits. Every output must be an input at the same location (or
 * Position from Position), the same for all three vertices, except one: Layer, from a scalar
 * input at location L. The vertex shader's output at location L then becomes BuiltIn Layer.
 */
#define _GNU_SOURCE
#include "spirv_gsfold.h"
#include "spirv_edit.h"

#include <stdlib.h>
#include <string.h>

enum {
    OP_SELECT = 169, OP_UGT = 172, OP_EMIT = 218, OP_END_PRIM = 219, OP_FUNCTION_CALL = 57,
    EM_INVOCATIONS = 0, EM_TRIANGLES = 22, EM_OUTPUT_VERTICES = 26, EM_OUTPUT_TRIANGLE_STRIP = 29,
    BI_LAYER = 9, DEC_STREAM = 29, DEC_XFB_BUFFER = 36, DEC_XFB_STRIDE = 37, DEC_FLAT = 14,
    CAP_SHADER_LAYER = 69, CAP_VIEWPORT_INDEX_LAYER_EXT = 5254,
};

#define MAXMAP 64
#define MAXREST 4

typedef struct copy {
    uint32_t out, in;            /* variables */
    uint32_t nrest, rest[MAXREST]; /* the same constant indices on both sides (after the vertex) */
} copy;

static int const_value(const spv_mod *m, uint32_t id, uint32_t *v)
{
    int d = spv_find_def(m, id);
    if (d < 0 || m->ins[d].op != SpvOpConstant || m->ins[d].n < 4) return 0;
    *v = m->ins[d].w[2];
    return 1;
}

/* The variable an id is decorated BuiltIn x as a whole, or one whose struct type has member 0 BuiltIn x. */
static int builtin_of(const spv_mod *m, uint32_t var)
{
    int64_t b = spv_get_decoration(m, var, SpvDecorationBuiltIn);
    if (b >= 0) return (int)b;
    int d = spv_find_def(m, var);
    if (d < 0 || m->ins[d].op != SpvOpVariable) return -1;
    int p = spv_find_def(m, m->ins[d].w[0]);
    if (p < 0 || m->ins[p].op != SpvOpTypePointer) return -1;
    uint32_t t = m->ins[p].w[2];
    for (int k = 0; k < 2; k++) { /* through one array (the input's vertices) */
        int td = spv_find_def(m, t);
        if (td < 0) return -1;
        if (m->ins[td].op == SpvOpTypeArray) {
            t = m->ins[td].w[1];
            continue;
        }
        if (m->ins[td].op != SpvOpTypeStruct) return -1;
        for (uint32_t i = 0; i < m->count; i++) {
            const spv_inst *in = &m->ins[i];
            if (in->op == SpvOpMemberDecorate && in->n >= 5 && in->w[0] == t && in->w[2] == SpvDecorationBuiltIn) return (int)in->w[3] + 1000;
        }
        return -1;
    }
    return -1;
}

int vkb_gs_layer_passthrough(const uint32_t *code, size_t size, uint32_t *layer_location)
{
    spv_mod m;
    if (spv_parse(&m, code, size) != 0) return 0;
    int ok = spv_execution_model(&m) == SpvExecutionModelGeometry;
    int tri = 0, strip = 0, nverts = 0, invocations = 1;
    for (uint32_t i = 0; ok && i < m.count; i++) {
        const spv_inst *in = &m.ins[i];
        if (in->op == SpvOpExecutionMode && in->n >= 3) {
            if (in->w[1] == EM_TRIANGLES) tri = 1;
            if (in->w[1] == EM_OUTPUT_TRIANGLE_STRIP) strip = 1;
            if (in->w[1] == EM_OUTPUT_VERTICES && in->n >= 4) nverts = (int)in->w[2];
            if (in->w[1] == EM_INVOCATIONS && in->n >= 4) invocations = (int)in->w[2];
        }
        if (in->op == SpvOpDecorate && in->n >= 3 && (in->w[1] == DEC_STREAM || in->w[1] == DEC_XFB_BUFFER || in->w[1] == DEC_XFB_STRIDE)) ok = 0;
        if (in->op == SpvOpMemberDecorate && in->n >= 4 && (in->w[2] == DEC_STREAM || in->w[2] == DEC_XFB_BUFFER)) ok = 0;
        if (in->op == OP_FUNCTION_CALL || in->op == SpvOpBranch || in->op == SpvOpBranchConditional) ok = 0;
    }
    ok = ok && tri && strip && nverts == 3 && invocations == 1;
    copy map[3][MAXMAP];
    uint32_t nmap[3] = {0, 0, 0};
    int vertex = 0;
    int in_func = 0;
    for (uint32_t i = 0; ok && i < m.count; i++) {
        const spv_inst *in = &m.ins[i];
        if (in->op == SpvOpFunction) {
            in_func = 1;
            continue;
        }
        if (!in_func) continue;
        switch (in->op) {
        case SpvOpLabel: case SpvOpAccessChain: case SpvOpInBoundsAccessChain: case SpvOpLoad: case OP_SELECT: case OP_UGT:
        case SpvOpReturn: case SpvOpFunctionEnd: case OP_END_PRIM: case SpvOpLine: case SpvOpNoLine:
            break;
        case OP_EMIT:
            vertex++;
            break;
        case SpvOpStore: {
            if (vertex > 2) {
                ok = 0;
                break;
            }
            /* destination: an output variable, or a constant access chain into one */
            uint32_t out = in->w[0], orest[MAXREST], norest = 0;
            int d = spv_find_def(&m, out);
            if (d >= 0 && (m.ins[d].op == SpvOpAccessChain || m.ins[d].op == SpvOpInBoundsAccessChain)) {
                const spv_inst *ac = &m.ins[d];
                out = ac->w[2];
                for (uint32_t k = 3; k < (uint32_t)ac->n - 1 && ok; k++) {
                    if (norest == MAXREST || !const_value(&m, ac->w[k], &orest[norest++])) ok = 0;
                }
            }
            /* value: through constant-condition selects to a load from an input of this vertex */
            uint32_t val = in->w[1];
            for (int guard = 0; guard < 4; guard++) {
                int vd = spv_find_def(&m, val);
                if (vd < 0 || m.ins[vd].op != OP_SELECT) break;
                int cd = spv_find_def(&m, m.ins[vd].w[2]);
                uint32_t a, b;
                if (cd < 0 || m.ins[cd].op != OP_UGT || !const_value(&m, m.ins[cd].w[2], &a) || !const_value(&m, m.ins[cd].w[3], &b)) {
                    ok = 0;
                    break;
                }
                val = a > b ? m.ins[vd].w[3] : m.ins[vd].w[4];
            }
            int ld = spv_find_def(&m, val);
            if (!ok || ld < 0 || m.ins[ld].op != SpvOpLoad) {
                ok = 0;
                break;
            }
            int ad = spv_find_def(&m, m.ins[ld].w[2]);
            if (ad < 0 || (m.ins[ad].op != SpvOpAccessChain && m.ins[ad].op != SpvOpInBoundsAccessChain) || m.ins[ad].n < 5) {
                ok = 0;
                break;
            }
            const spv_inst *ac = &m.ins[ad];
            uint32_t vtx, irest[MAXREST], nirest = 0;
            if (!const_value(&m, ac->w[3], &vtx) || (int)vtx != vertex) {
                ok = 0;
                break;
            }
            for (uint32_t k = 4; k < (uint32_t)ac->n - 1 && ok; k++)
                if (nirest == MAXREST || !const_value(&m, ac->w[k], &irest[nirest++])) ok = 0;
            if (!ok || nirest != norest || memcmp(irest, orest, norest * 4) || nmap[vertex] == MAXMAP) {
                ok = 0;
                break;
            }
            copy *c = &map[vertex][nmap[vertex]++];
            memset(c, 0, sizeof(*c));
            c->out = out;
            c->in = ac->w[2];
            c->nrest = norest;
            memcpy(c->rest, orest, norest * 4);
            break;
        }
        default:
            ok = 0;
        }
    }
    ok = ok && vertex == 3 && nmap[0] && nmap[0] == nmap[1] && nmap[0] == nmap[2];
    for (int v = 1; ok && v < 3; v++)
        for (uint32_t i = 0; i < nmap[0] && ok; i++) ok = !memcmp(&map[0][i], &map[v][i], sizeof(copy));
    /* What each copy is. */
    int64_t layer_loc = -1;
    for (uint32_t i = 0; ok && i < nmap[0]; i++) {
        const copy *c = &map[0][i];
        int ob = builtin_of(&m, c->out), ib = builtin_of(&m, c->in);
        int64_t ol = spv_get_decoration(&m, c->out, SpvDecorationLocation), il = spv_get_decoration(&m, c->in, SpvDecorationLocation);
        int64_t oc = spv_get_decoration(&m, c->out, SpvDecorationComponent), ic = spv_get_decoration(&m, c->in, SpvDecorationComponent);
        if (ob == BI_LAYER) {
            ok = il >= 0 && ib < 0 && c->nrest == 0 && (ic < 0 || ic == 0) && (layer_loc < 0 || layer_loc == il);
            layer_loc = il;
        } else if (ob == 1000 + SpvBuiltInPosition) {
            ok = ib == 1000 + SpvBuiltInPosition;
        } else if (ob < 0) {
            ok = ol >= 0 && ol == il && ib < 0 && (oc < 0 ? 0 : oc) == (ic < 0 ? 0 : ic);
        } else {
            ok = 0;
        }
    }
    /* No output may sit at the layer's location (the fragment shader would want it). */
    for (uint32_t i = 0; ok && i < nmap[0]; i++)
        if (builtin_of(&m, map[0][i].out) < 0 && spv_get_decoration(&m, map[0][i].out, SpvDecorationLocation) == layer_loc) ok = 0;
    ok = ok && layer_loc >= 0;
    if (ok) *layer_location = (uint32_t)layer_loc;
    spv_free(&m);
    return ok;
}

uint32_t *vkb_vs_write_layer(const uint32_t *code, size_t size, uint32_t location, size_t *out_size)
{
    spv_mod m;
    if (spv_parse(&m, code, size) != 0) return NULL;
    uint32_t var = 0;
    int ok = spv_execution_model(&m) == SpvExecutionModelVertex;
    for (uint32_t i = 0; ok && i < m.count; i++) {
        const spv_inst *in = &m.ins[i];
        if (in->op != SpvOpVariable || in->n < 4 || in->w[2] != SpvStorageClassOutput) continue;
        if (spv_get_decoration(&m, in->w[1], SpvDecorationLocation) != (int64_t)location) continue;
        if (var) ok = 0; /* two variables at one location (components): not this shape */
        var = in->w[1];
    }
    if (ok && var) {
        /* a 32-bit integer scalar */
        int d = spv_find_def(&m, var);
        int p = d >= 0 ? spv_find_def(&m, m.ins[d].w[0]) : -1;
        int t = p >= 0 ? spv_find_def(&m, m.ins[p].w[2]) : -1;
        ok = t >= 0 && m.ins[t].op == SpvOpTypeInt && m.ins[t].w[1] == 32;
    }
    uint32_t *out = NULL;
    if (ok && var) {
        spv_remove_decoration(&m, var, SpvDecorationLocation);
        spv_remove_decoration(&m, var, SpvDecorationComponent);
        spv_remove_decoration(&m, var, DEC_FLAT);
        uint32_t ops[3] = {var, SpvDecorationBuiltIn, BI_LAYER};
        spv_insert(&m, spv_annotations_end(&m), SpvOpDecorate, ops, 3);
        if (spv_version(&m) >= 0x00010500) {
            spv_add_capability(&m, CAP_SHADER_LAYER);
        } else {
            spv_add_capability(&m, CAP_VIEWPORT_INDEX_LAYER_EXT);
            static const char ext[] = "SPV_EXT_shader_viewport_index_layer";
            uint32_t w[10] = {0};
            memcpy(w, ext, sizeof(ext));
            int at = spv_find_op(&m, SpvOpExtInstImport, 0);
            if (at < 0) at = spv_find_op(&m, SpvOpMemoryModel, 0);
            if (at >= 0) spv_insert(&m, (uint32_t)at, SpvOpExtension, w, (uint16_t)((sizeof(ext) + 4) / 4));
        }
        if (!m.err) {
            size_t n = spv_write(&m, &out);
            if (!n) out = NULL;
            *out_size = n;
        }
    }
    spv_free(&m);
    return out;
}
