/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * Workgroup memory zeroed by the shader itself instead of through initializers. DXVK gives a
 * compute shader's shared memory a zero initializer (VK_KHR_zero_initialize_workgroup_memory, so
 * D3D's "groupshared starts at zero" holds), and Arm's driver turns that into something very slow:
 * Dark Souls III's tiled-lighting shader (32x32 threads, three 1024-element arrays) spent 6.2 ms of
 * a frame on the G720 with it, 0.24 ms zeroing them by hand. Here each such variable loses its
 * initializer and the entry point starts by zeroing it: an array's elements spread over the
 * workgroup's invocations (element i, i + size, ... unrolled, at most WG_UNROLL a variable), a
 * single value by invocation 0; then one workgroup barrier. Anything else (several entry points,
 * a workgroup size from specialization constants, huge arrays) leaves the module as it is.
 */
#include "spirv_wginit.h"
#include "spirv_edit.h"

#include <stdlib.h>
#include <string.h>

#define WG_UNROLL 64

enum {
    OP_CONSTANT_NULL = 46, OP_CONTROL_BARRIER = 224, OP_PHI = 245, OP_IEQUAL = 170, OP_ULESS_THAN = 176,
    STORAGE_WORKGROUP = 4, EXEC_LOCAL_SIZE = 17, EXEC_LOCAL_SIZE_ID = 38, BUILTIN_LOCAL_INVOCATION_INDEX = 29,
    BUILTIN_WORKGROUP_SIZE = 25, CAP_VULKAN_MEMORY_MODEL = 5345, SCOPE_WORKGROUP = 2,
    MEM_NON_PRIVATE_POINTER = 0x20,
};

typedef struct {
    uint32_t var, type, null_id; /* the variable, its pointee type, a null of what each store writes */
    uint32_t elem, len;          /* an array's element type and length; elem 0: a single value */
} wg_var;

/* The value of a 32-bit OpConstant, or -1 */
static int64_t const_value(const spv_mod *m, uint32_t id)
{
    int d = spv_find_def(m, id);
    if (d < 0 || m->ins[d].op != SpvOpConstant || m->ins[d].n != 4) return -1;
    return m->ins[d].w[2];
}

static uint32_t null_of(spv_mod *m, uint32_t type)
{
    uint32_t end = spv_types_end(m);
    for (uint32_t i = 0; i < end; i++)
        if (m->ins[i].op == OP_CONSTANT_NULL && m->ins[i].w[0] == type) return m->ins[i].w[1];
    uint32_t id = spv_new_id(m), ops[2] = {type, id};
    spv_insert(m, spv_types_end(m), OP_CONSTANT_NULL, ops, 2);
    return id;
}

/* The entry point's LocalInvocationIndex input, made if missing */
static uint32_t local_index_var(spv_mod *m)
{
    for (uint32_t i = 0; i < m->count; i++)
        if (m->ins[i].op == SpvOpDecorate && m->ins[i].n >= 4 && m->ins[i].w[1] == SpvDecorationBuiltIn &&
            m->ins[i].w[2] == BUILTIN_LOCAL_INVOCATION_INDEX)
            return m->ins[i].w[0];
    uint32_t ptr = spv_type_pointer(m, SpvStorageClassInput, spv_type_int(m, 32, 0));
    uint32_t var = spv_new_id(m), vops[3] = {ptr, var, SpvStorageClassInput};
    spv_insert(m, spv_types_end(m), SpvOpVariable, vops, 3);
    uint32_t dops[3] = {var, SpvDecorationBuiltIn, BUILTIN_LOCAL_INVOCATION_INDEX};
    spv_insert(m, spv_annotations_end(m), SpvOpDecorate, dops, 3);
    spv_add_interface(m, var);
    return var;
}

int vkb_spv_wg_init(const uint32_t *code, size_t size, uint32_t **out, size_t *out_size, vkb_wginit_stats *st, const char **why)
{
    memset(st, 0, sizeof(*st));
    *why = NULL;
    spv_mod m;
    if (spv_parse(&m, code, size) < 0) {
        *why = "not SPIR-V";
        return -1;
    }
    int r = 0;
    wg_var vars[32];
    unsigned nv = 0;
    /* compute, one entry point */
    int entry = -1, entries = 0;
    for (uint32_t i = 0; i < m.count; i++)
        if (m.ins[i].op == SpvOpEntryPoint) entries++, entry = (int)i;
    for (uint32_t i = 0; i < spv_types_end(&m); i++) {
        spv_inst *in = &m.ins[i];
        if (in->op == SpvOpVariable && in->n == 5 && in->w[2] == STORAGE_WORKGROUP) {
            if (nv == 32) {
                *why = "more than 32 initialized workgroup variables";
                r = -1;
                goto done;
            }
            vars[nv++].var = in->w[1];
        }
    }
    if (!nv) goto done;
    if (entries != 1 || m.ins[entry].w[0] != SpvExecutionModelGLCompute) {
        *why = entries != 1 ? "several entry points" : "not a compute shader";
        r = -1;
        goto done;
    }
    uint32_t main_fn = m.ins[entry].w[1];
    /* the workgroup size */
    int64_t wg = -1, wg_builtin = -1;
    for (uint32_t i = 0; i < m.count; i++) {
        spv_inst *in = &m.ins[i];
        if (in->op == SpvOpDecorate && in->n >= 4 && in->w[1] == SpvDecorationBuiltIn && in->w[2] == BUILTIN_WORKGROUP_SIZE) {
            /* takes precedence over LocalSize; usable when made of plain constants */
            int d = spv_find_def(&m, in->w[0]);
            int64_t x = -1, y = -1, z = -1;
            if (d >= 0 && m.ins[d].op == SpvOpConstantComposite && m.ins[d].n == 6)
                x = const_value(&m, m.ins[d].w[2]), y = const_value(&m, m.ins[d].w[3]), z = const_value(&m, m.ins[d].w[4]);
            if (x <= 0 || y <= 0 || z <= 0) {
                *why = "a WorkgroupSize from specialization constants";
                r = -1;
                goto done;
            }
            wg_builtin = x * y * z;
        }
        if (in->op == SpvOpExecutionMode && in->n >= 6 && in->w[0] == main_fn && in->w[1] == EXEC_LOCAL_SIZE)
            wg = (int64_t)in->w[2] * in->w[3] * in->w[4];
        if (in->op == SpvOpExecutionModeId && in->n >= 6 && in->w[0] == main_fn && in->w[1] == EXEC_LOCAL_SIZE_ID) {
            int64_t x = const_value(&m, in->w[2]), y = const_value(&m, in->w[3]), z = const_value(&m, in->w[4]);
            if (x > 0 && y > 0 && z > 0) wg = x * y * z;
        }
    }
    if (wg_builtin > 0) wg = wg_builtin;
    if (wg <= 0) {
        *why = "no fixed workgroup size";
        r = -1;
        goto done;
    }
    /* the variables' types: arrays spread over the invocations, the rest by invocation 0 */
    for (unsigned k = 0; k < nv; k++) {
        int d = spv_find_def(&m, vars[k].var);
        int p = d >= 0 ? spv_find_def(&m, m.ins[d].w[0]) : -1;
        if (p < 0 || m.ins[p].op != SpvOpTypePointer) {
            *why = "a variable of an unknown type";
            r = -1;
            goto done;
        }
        vars[k].type = m.ins[p].w[2];
        vars[k].elem = 0;
        int t = spv_find_def(&m, vars[k].type);
        if (t >= 0 && m.ins[t].op == SpvOpTypeArray) {
            int64_t len = const_value(&m, m.ins[t].w[2]);
            if (len <= 0) {
                *why = "an array of unknown length";
                r = -1;
                goto done;
            }
            if ((len + wg - 1) / wg > WG_UNROLL) {
                *why = "an array too large for the workgroup";
                r = -1;
                goto done;
            }
            vars[k].elem = m.ins[t].w[1];
            vars[k].len = (uint32_t)len;
        }
    }
    int vmm = spv_has_capability(&m, CAP_VULKAN_MEMORY_MODEL);
    uint32_t u32 = spv_type_int(&m, 32, 0), boolean = spv_type_bool(&m);
    uint32_t lidv = local_index_var(&m);
    for (unsigned k = 0; k < nv; k++) {
        vars[k].null_id = null_of(&m, vars[k].elem ? vars[k].elem : vars[k].type);
        int d = spv_find_def(&m, vars[k].var);
        m.ins[d].n--; /* the initializer goes */
    }
    uint32_t c_scope = spv_const_u32(&m, SCOPE_WORKGROUP), c_zero = spv_const_u32(&m, 0);
    uint32_t c_sem = spv_const_u32(&m, vmm ? 0x6108u : 0x108u); /* AcquireRelease | WorkgroupMemory (| MakeAvailable | MakeVisible) */
    uint32_t c_wg = spv_const_u32(&m, (uint32_t)wg);
    (void)c_wg;
    /* constants for the unrolled offsets and tails, made before code is placed */
    uint32_t koff[WG_UNROLL + 1], ptrs[32], tails[32];
    for (unsigned j = 0; j <= WG_UNROLL; j++) koff[j] = 0;
    for (unsigned k = 0; k < nv; k++) {
        ptrs[k] = vars[k].elem ? spv_type_pointer(&m, STORAGE_WORKGROUP, vars[k].elem) : 0;
        tails[k] = 0;
        if (!vars[k].elem) continue;
        uint32_t full = vars[k].len / (uint32_t)wg, rem = vars[k].len % (uint32_t)wg;
        for (uint32_t j = 1; j <= full && j <= WG_UNROLL; j++)
            if (!koff[j]) koff[j] = spv_const_u32(&m, j * (uint32_t)wg);
        if (rem) {
            tails[k] = spv_const_u32(&m, rem);
            if (!koff[full]) koff[full] = full ? spv_const_u32(&m, full * (uint32_t)wg) : 0;
        }
    }
    /* the entry function's first block: after its OpLabel and OpVariables */
    int f = -1;
    for (uint32_t i = 0; i < m.count; i++)
        if (m.ins[i].op == SpvOpFunction && m.ins[i].w[1] == main_fn) f = (int)i;
    uint32_t at = (uint32_t)f + 1;
    while (at < m.count && m.ins[at].op == SpvOpFunctionParameter) at++;
    if (at >= m.count || m.ins[at].op != SpvOpLabel) {
        *why = "no first block";
        r = -1;
        goto done;
    }
    uint32_t first_label = m.ins[at].w[0];
    at++;
    while (at < m.count && (m.ins[at].op == SpvOpVariable || m.ins[at].op == SpvOpLine || m.ins[at].op == SpvOpNoLine)) at++;
#define EMIT(op, ...)                                                   \
    do {                                                                \
        uint32_t o_[] = {__VA_ARGS__};                                  \
        spv_insert(&m, at++, op, o_, (uint16_t)(sizeof(o_) / 4));      \
    } while (0)
    uint32_t lid = spv_new_id(&m);
    EMIT(SpvOpLoad, u32, lid, lidv);
    uint32_t last_label = first_label;
    for (unsigned k = 0; k < nv; k++) {
        if (!vars[k].elem) continue;
        uint32_t full = vars[k].len / (uint32_t)wg;
        for (uint32_t j = 0; j < full; j++) {
            uint32_t idx = lid;
            if (j) {
                idx = spv_new_id(&m);
                EMIT(SpvOpIAdd, u32, idx, lid, koff[j]);
            }
            uint32_t p = spv_new_id(&m);
            EMIT(SpvOpAccessChain, ptrs[k], p, vars[k].var, idx);
            if (vmm) EMIT(SpvOpStore, p, vars[k].null_id, MEM_NON_PRIVATE_POINTER);
            else EMIT(SpvOpStore, p, vars[k].null_id);
            st->stores++;
        }
        if (tails[k]) {
            /* the last, partial round: only the invocations below the remainder */
            uint32_t c = spv_new_id(&m), t = spv_new_id(&m), mg = spv_new_id(&m);
            EMIT(OP_ULESS_THAN, boolean, c, lid, tails[k]);
            EMIT(SpvOpSelectionMerge, mg, 0);
            EMIT(SpvOpBranchConditional, c, t, mg);
            EMIT(SpvOpLabel, t);
            uint32_t idx = lid;
            if (full) {
                idx = spv_new_id(&m);
                EMIT(SpvOpIAdd, u32, idx, lid, koff[full]);
            }
            uint32_t p = spv_new_id(&m);
            EMIT(SpvOpAccessChain, ptrs[k], p, vars[k].var, idx);
            if (vmm) EMIT(SpvOpStore, p, vars[k].null_id, MEM_NON_PRIVATE_POINTER);
            else EMIT(SpvOpStore, p, vars[k].null_id);
            EMIT(SpvOpBranch, mg);
            EMIT(SpvOpLabel, mg);
            last_label = mg;
            st->stores++;
        }
        st->arrays++;
    }
    int singles = 0;
    for (unsigned k = 0; k < nv; k++) singles += !vars[k].elem;
    if (singles) {
        uint32_t c = spv_new_id(&m), t = spv_new_id(&m), mg = spv_new_id(&m);
        EMIT(OP_IEQUAL, boolean, c, lid, c_zero);
        EMIT(SpvOpSelectionMerge, mg, 0);
        EMIT(SpvOpBranchConditional, c, t, mg);
        EMIT(SpvOpLabel, t);
        for (unsigned k = 0; k < nv; k++) {
            if (vars[k].elem) continue;
            if (vmm) EMIT(SpvOpStore, vars[k].var, vars[k].null_id, MEM_NON_PRIVATE_POINTER);
            else EMIT(SpvOpStore, vars[k].var, vars[k].null_id);
            st->stores++;
            st->singles++;
        }
        EMIT(SpvOpBranch, mg);
        EMIT(SpvOpLabel, mg);
        last_label = mg;
    }
    EMIT(OP_CONTROL_BARRIER, c_scope, c_scope, c_sem);
#undef EMIT
    /* the original first block now ends in the last merge block: phis that named it name that one */
    if (last_label != first_label)
        for (uint32_t i = at; i < m.count; i++) {
            spv_inst *in = &m.ins[i];
            if (in->op != OP_PHI) continue;
            for (uint32_t a = 3; a + 1 < in->n; a += 2)
                if (in->w[a] == first_label) in->w[a] = last_label;
        }
    st->vars = nv;
    if (m.err) {
        *why = "out of memory";
        r = -1;
        goto done;
    }
    *out_size = spv_write(&m, out);
    r = *out_size ? 1 : -1;
    if (r < 0) *why = "could not write the module";
done:
    spv_free(&m);
    return r;
}
