/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * MK-45 (nan_position): see spirv_nanpos.h.
 */
#include "spirv_nanpos.h"
#include "spirv_edit.h"

#include <stdlib.h>
#include <string.h>

enum { OpSelect = 169, OpReturnValue = 254 };

int vkb_spv_nan_position(const uint32_t *code, size_t size, uint32_t **out, size_t *out_size, vkb_nanpos_stats *st, const char **why)
{
    vkb_nanpos_stats local;
    if (!st) st = &local;
    memset(st, 0, sizeof(*st));
    *why = NULL;
    if (!code || size < 20 || (size & 3) || code[0] != 0x07230203u) return 0;
    spv_mod m;
    if (spv_parse(&m, code, size) < 0) {
        *why = "cannot parse";
        return -1;
    }
    int rc = 0;
    /* the vertex entry point(s): execution model 0 */
    uint32_t entry[8];
    uint32_t nentry = 0;
    for (uint32_t i = 0; i < m.count; i++) {
        const spv_inst *in = &m.ins[i];
        if (in->op == SpvOpEntryPoint && in->n >= 3 && in->w[0] == SpvExecutionModelVertex && nentry < 8) entry[nentry++] = in->w[1];
    }
    if (!nentry) goto done;
    /* the Position output: a variable decorated BuiltIn Position, or member k of a struct so decorated */
    uint32_t pos_var = 0, pos_struct = 0, pos_member = 0;
    for (uint32_t i = 0; i < m.count; i++) {
        const spv_inst *in = &m.ins[i];
        if (in->op == SpvOpDecorate && in->n >= 4 && in->w[1] == SpvDecorationBuiltIn && in->w[2] == SpvBuiltInPosition) pos_var = in->w[0];
        if (in->op == SpvOpMemberDecorate && in->n >= 5 && in->w[2] == SpvDecorationBuiltIn && in->w[3] == SpvBuiltInPosition)
            pos_struct = in->w[0], pos_member = in->w[1];
    }
    if (!pos_var && pos_struct) {
        /* the Output variable whose pointee is that struct (a block of per-vertex outputs) */
        for (uint32_t i = 0; i < m.count && !pos_var; i++) {
            const spv_inst *in = &m.ins[i];
            if (in->op != SpvOpVariable || in->n < 4 || in->w[2] != SpvStorageClassOutput) continue;
            int pt = spv_find_def(&m, in->w[0]);
            if (pt >= 0 && m.ins[pt].op == SpvOpTypePointer && m.ins[pt].n >= 4 && m.ins[pt].w[2] == pos_struct) pos_var = in->w[1];
        }
        st->member = 1;
    }
    if (!pos_var) goto done;
    /* a plain variable must be an Output of a vec4 (DXVK's oPos; an Input decorated Position is a later stage's) */
    if (!st->member) {
        int vd = spv_find_def(&m, pos_var);
        if (vd < 0 || m.ins[vd].op != SpvOpVariable || m.ins[vd].n < 4 || m.ins[vd].w[2] != SpvStorageClassOutput) goto done;
    }

    uint32_t f32 = spv_type_float(&m, 32), v4f = spv_type_vector(&m, f32, 4);
    uint32_t b1 = spv_type_bool(&m), v4b = spv_type_vector(&m, b1, 4), u32 = spv_type_int(&m, 32, 0);
    uint32_t c2 = spv_const_f32(&m, 2.0f), c1 = spv_const_f32(&m, 1.0f), c0 = spv_const_f32(&m, 0.0f);
    uint32_t c23 = spv_const_u32(&m, 23), cff = spv_const_u32(&m, 0xff);
    uint32_t far = spv_new_id(&m);
    {
        uint32_t ops[6] = {v4f, far, c2, c2, c2, c1};
        spv_insert(&m, spv_types_end(&m), SpvOpConstantComposite, ops, 6);
    }
    uint32_t p_out_v4f = st->member ? spv_type_pointer(&m, SpvStorageClassOutput, v4f) : 0;
    uint32_t c_member = st->member ? spv_const_u32(&m, pos_member) : 0;
    if (!f32 || !v4f || !b1 || !v4b || !u32 || !c2 || !c1 || !c0 || !c23 || !cff || (st->member && (!p_out_v4f || !c_member))) {
        *why = "types or constants could not be added";
        rc = -1;
        goto done;
    }

    /* every return of each vertex entry point's function. The test is on the bits (scalar integer
     * ops, which no fast-math folds and bitwise_scalar leaves alone): a component whose exponent is
     * all ones (NaN or Inf), or a w of exactly zero - what this driver makes of a NaN matrix. */
    enum { OpBitcast = 124, OpShiftRightLogical = 194, OpBitwiseAnd = 199, OpIEqual = 170, OpLogicalOr = 166, OpFOrdEqual = 180 };
    for (uint32_t e = 0; e < nentry; e++) {
        int fi = -1;
        for (uint32_t i = 0; i < m.count; i++)
            if (m.ins[i].op == SpvOpFunction && m.ins[i].n >= 5 && m.ins[i].w[1] == entry[e]) {
                fi = (int)i;
                break;
            }
        if (fi < 0) continue;
        for (uint32_t i = (uint32_t)fi + 1; i < m.count && m.ins[i].op != SpvOpFunctionEnd; i++) {
            if (m.ins[i].op != SpvOpReturn && m.ins[i].op != OpReturnValue) continue;
            uint32_t at = i;
            uint32_t ptr = pos_var;
#define INS(opc, ...)                                                                   do {                                                                            uint32_t ops_[] = {__VA_ARGS__};                                            spv_insert(&m, at++, (opc), ops_, sizeof(ops_) / sizeof(ops_[0]));             } while (0)
            if (st->member) {
                ptr = spv_new_id(&m);
                INS(SpvOpAccessChain, p_out_v4f, ptr, pos_var, c_member);
            }
            uint32_t val = spv_new_id(&m);
            INS(SpvOpLoad, v4f, val, ptr);
            uint32_t bad = 0;
            for (uint32_t k = 0; k < 4; k++) {
                uint32_t comp = spv_new_id(&m), bits = spv_new_id(&m), sh = spv_new_id(&m), ex = spv_new_id(&m), isx = spv_new_id(&m);
                INS(SpvOpCompositeExtract, f32, comp, val, k);
                INS(OpBitcast, u32, bits, comp);
                INS(OpShiftRightLogical, u32, sh, bits, c23);
                INS(OpBitwiseAnd, u32, ex, sh, cff);
                INS(OpIEqual, b1, isx, ex, cff);
                if (k == 3) {
                    uint32_t z = spv_new_id(&m), both = spv_new_id(&m);
                    INS(OpFOrdEqual, b1, z, comp, c0);
                    INS(OpLogicalOr, b1, both, isx, z);
                    isx = both;
                }
                if (!bad) bad = isx;
                else {
                    uint32_t o = spv_new_id(&m);
                    INS(OpLogicalOr, b1, o, bad, isx);
                    bad = o;
                }
            }
            uint32_t bad4 = spv_new_id(&m), sel = spv_new_id(&m);
            INS(SpvOpCompositeConstruct, v4b, bad4, bad, bad, bad, bad);
            INS(OpSelect, v4f, sel, bad4, far, val);
            INS(SpvOpStore, ptr, sel);
#undef INS
            i = at; /* past the inserted code, at the return */
            st->guards++;
        }
    }
    if (!st->guards) goto done;
    *out_size = spv_write(&m, out);
    if (!*out_size) {
        *why = "cannot write";
        rc = -1;
        goto done;
    }
    rc = 1;
done:
    spv_free(&m);
    return rc;
}
