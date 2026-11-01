/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * Workgroup variable initializers done by one invocation (spirv_wgzero.h says why).
 */
#include "spirv_wgzero.h"
#include "spirv_edit.h"

#include <stdlib.h>
#include <string.h>

enum {
    OP_IEQUAL = 170,
    OP_CONTROL_BARRIER = 224,
    SC_WORKGROUP = 4,
    BI_LOCAL_INVOCATION_INDEX = 29,
    MEMORY_MODEL_VULKAN = 3,
    SCOPE_WORKGROUP = 2,
    SEM_ACQREL_WORKGROUP = 0x108,
    SEM_AVAILABLE_VISIBLE = 0x6000,
    MEMORY_NON_PRIVATE_POINTER = 0x20,
};

static int in_interface(const spv_mod *m, uint32_t var)
{
    int ep = spv_find_op(m, SpvOpEntryPoint, 0);
    if (ep < 0) return 0;
    const spv_inst *in = &m->ins[ep];
    uint32_t k = 2;
    while (k < (uint32_t)in->n - 1u) {
        uint32_t wd = in->w[k++];
        if (!(wd & 0xff) || !(wd & 0xff00) || !(wd & 0xff0000) || !(wd & 0xff000000)) break; /* the name's last word */
    }
    for (; k < (uint32_t)in->n - 1u; k++)
        if (in->w[k] == var) return 1;
    return 0;
}

static uint32_t local_index_var(spv_mod *m)
{
    for (uint32_t i = 0; i < m->count; i++) {
        const spv_inst *in = &m->ins[i];
        if (in->op == SpvOpDecorate && in->n >= 4 && in->w[1] == SpvDecorationBuiltIn && in->w[2] == BI_LOCAL_INVOCATION_INDEX) {
            uint32_t var = in->w[0];
            if (!in_interface(m, var)) spv_add_interface(m, var);
            return var;
        }
    }
    uint32_t var = spv_new_id(m), deco[3] = {var, SpvDecorationBuiltIn, BI_LOCAL_INVOCATION_INDEX};
    spv_insert(m, spv_annotations_end(m), SpvOpDecorate, deco, 3);
    uint32_t ptr = spv_type_pointer(m, SpvStorageClassInput, spv_type_int(m, 32, 0));
    uint32_t def[3] = {ptr, var, SpvStorageClassInput};
    spv_insert(m, spv_types_end(m), SpvOpVariable, def, 3);
    spv_add_interface(m, var);
    return var;
}

int vkb_spv_wg_zero(const uint32_t *code, size_t size, uint32_t **out, size_t *out_size, unsigned *vars, const char **why)
{
    *why = NULL;
    *vars = 0;
    spv_mod m;
    if (spv_parse(&m, code, size) != 0) return 0;
    int rc = 0;
    int ep = spv_find_op(&m, SpvOpEntryPoint, 0);
    if (spv_execution_model(&m) != SpvExecutionModelGLCompute || ep < 0) goto done;

    uint32_t nvar = 0;
    for (uint32_t i = 0; i < m.count; i++) {
        const spv_inst *in = &m.ins[i];
        if (in->op == SpvOpFunction) break;
        if (in->op == SpvOpVariable && in->n == 5 && in->w[2] == SC_WORKGROUP) nvar++;
    }
    if (!nvar) goto done;
    rc = -1;
    if (spv_find_op(&m, SpvOpEntryPoint, (uint32_t)ep + 1) >= 0) {
        *why = "more than one entry point";
        goto done;
    }
    uint32_t *var = malloc(nvar * sizeof(uint32_t)), *init = malloc(nvar * sizeof(uint32_t));
    if (!var || !init) {
        free(var);
        free(init);
        *why = "out of memory";
        goto done;
    }
    nvar = 0;
    for (uint32_t i = 0; i < m.count; i++) {
        spv_inst *in = &m.ins[i];
        if (in->op == SpvOpFunction) break;
        if (in->op == SpvOpVariable && in->n == 5 && in->w[2] == SC_WORKGROUP) {
            var[nvar] = in->w[1];
            init[nvar++] = in->w[3];
            in->n = 4;
        }
    }

    int vulkan_mm = 0;
    int mm = spv_find_op(&m, SpvOpMemoryModel, 0);
    if (mm >= 0 && m.ins[mm].n >= 3) vulkan_mm = m.ins[mm].w[1] == MEMORY_MODEL_VULKAN;
    uint32_t entry_fn = m.ins[spv_find_op(&m, SpvOpEntryPoint, 0)].w[1];
    uint32_t lii = local_index_var(&m);
    uint32_t t_u32 = spv_type_int(&m, 32, 0), t_bool = spv_type_bool(&m);
    uint32_t c0 = spv_const_u32(&m, 0), scope = spv_const_u32(&m, SCOPE_WORKGROUP);
    uint32_t sem = spv_const_u32(&m, SEM_ACQREL_WORKGROUP | (vulkan_mm ? SEM_AVAILABLE_VISIBLE : 0));

    int fn = -1;
    for (uint32_t i = 0; i < m.count; i++)
        if (m.ins[i].op == SpvOpFunction && m.ins[i].n >= 3 && m.ins[i].w[1] == entry_fn) {
            fn = (int)i;
            break;
        }
    uint32_t at = (uint32_t)fn + 1;
    while (fn >= 0 && at < m.count && m.ins[at].op == SpvOpFunctionParameter) at++;
    if (fn < 0 || at >= m.count || m.ins[at].op != SpvOpLabel) {
        free(var);
        free(init);
        *why = "no entry function body";
        goto done;
    }
    uint32_t old_label = m.ins[at].w[0], first = at + 1, body = first;
    while (body < m.count && (m.ins[body].op == SpvOpVariable || m.ins[body].op == SpvOpLine || m.ins[body].op == SpvOpNoLine)) body++;

    /* The entry block's OpVariables move into the new entry block, ahead of the old label. */
    uint32_t new_label = spv_new_id(&m), zero_label = spv_new_id(&m), idx = spv_new_id(&m), is0 = spv_new_id(&m);
    uint32_t pos = at;
    spv_insert(&m, pos++, SpvOpLabel, &new_label, 1);
    first++, body++;
    for (uint32_t k = first; k < body; k++) {
        spv_inst mv = m.ins[k];
        uint32_t ops[16];
        uint16_t n = mv.n - 1 < 16 ? (uint16_t)(mv.n - 1) : 16;
        memcpy(ops, mv.w, n * 4u);
        spv_remove(&m, k);
        spv_insert(&m, pos++, mv.op, ops, n);
    }
    uint32_t load[3] = {t_u32, idx, lii};
    spv_insert(&m, pos++, SpvOpLoad, load, 3);
    uint32_t cmp[4] = {t_bool, is0, idx, c0};
    spv_insert(&m, pos++, OP_IEQUAL, cmp, 4);
    uint32_t merge[2] = {old_label, 0};
    spv_insert(&m, pos++, SpvOpSelectionMerge, merge, 2);
    uint32_t branch[3] = {is0, zero_label, old_label};
    spv_insert(&m, pos++, SpvOpBranchConditional, branch, 3);
    spv_insert(&m, pos++, SpvOpLabel, &zero_label, 1);
    for (uint32_t k = 0; k < nvar; k++) {
        uint32_t st[3] = {var[k], init[k], MEMORY_NON_PRIVATE_POINTER};
        spv_insert(&m, pos++, SpvOpStore, st, vulkan_mm ? 3 : 2);
    }
    spv_insert(&m, pos++, SpvOpBranch, &old_label, 1);
    pos++; /* the old label */
    uint32_t bar[3] = {scope, scope, sem};
    spv_insert(&m, pos, OP_CONTROL_BARRIER, bar, 3);
    free(var);
    free(init);
    if (m.err) {
        *why = "out of memory";
        goto done;
    }
    size_t bytes = spv_write(&m, out);
    if (!bytes) {
        *why = "out of memory";
        goto done;
    }
    *out_size = bytes;
    *vars = nvar;
    rc = 1;
done:
    spv_free(&m);
    return rc;
}
