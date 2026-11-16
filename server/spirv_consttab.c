/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * PV-10: constant arrays indexed at run time moved from Function to Private storage (see spirv_consttab.h).
 *     before:  %t = OpVariable %_ptr_Function__arr_uint_246 Function          (in a function's first block)
 *              OpStore %t %table                                              (%table: OpConstantComposite)
 *              %p = OpAccessChain %_ptr_Function_uint %t %i ; %v = OpLoad %uint %p
 *     after:   %t = OpVariable %_ptr_Private__arr_uint_246 Private            (global)
 *              OpStore %t %table
 *              %p = OpAccessChain %_ptr_Private_uint %t %i ; %v = OpLoad %uint %p
 * A variable qualifies when every use of it, or of an access chain into it, is one of: OpStore into the
 * variable itself of an OpConstantComposite / OpConstantNull, OpLoad, OpAccessChain / OpInBoundsAccessChain
 * (whose result is followed the same way), OpName, OpDecorate. Any other use - a store of something else,
 * a store into an element, a function argument, a copy, a pointer select - leaves it where it is.
 */
#include "spirv_consttab.h"
#include "spirv_edit.h"

#include <stdlib.h>
#include <string.h>

#define MAX_BOUND (1u << 22)

enum { T_NONE = 0, T_VAR = 1, T_CHAIN = 2 };

static int is_const_value(const spv_mod *m, uint32_t id)
{
    int d = spv_find_def(m, id);
    return d >= 0 && (m->ins[d].op == SpvOpConstantComposite || m->ins[d].op == 46 /* OpConstantNull */);
}

/* The array length of an OpTypeArray (its length constant's value), 0 when not a plain constant */
static unsigned array_len(const spv_mod *m, uint32_t type)
{
    int d = spv_find_def(m, type);
    if (d < 0 || m->ins[d].op != SpvOpTypeArray) return 0;
    int c = spv_find_def(m, m->ins[d].w[2]);
    unsigned n = c >= 0 && m->ins[c].op == SpvOpConstant && m->ins[c].n >= 4 ? m->ins[c].w[2] : 0;
    unsigned inner = array_len(m, m->ins[d].w[1]);
    return inner ? n * inner : n;
}

int vkb_spv_consttab(const uint32_t *code, size_t size, uint32_t **out, size_t *out_size, vkb_consttab_stats *st,
                     const char **why)
{
    vkb_consttab_stats dummy;
    if (!st) st = &dummy;
    memset(st, 0, sizeof(*st));
    const char *nowhy;
    if (!why) why = &nowhy;
    *why = NULL;
    *out = NULL;
    *out_size = 0;
    if (size < 20 || (size & 3) || code[0] != 0x07230203u) {
        *why = "not a SPIR-V module";
        return -1;
    }
    uint32_t bound = code[3];
    if (!bound || bound > MAX_BOUND) {
        *why = "id bound out of range";
        return -1;
    }
    /* A quick look first: no Function variable of array type, nothing to do (most modules) */
    spv_mod m;
    if (spv_parse(&m, code, size) || m.err) {
        spv_free(&m);
        *why = "cannot parse";
        return -1;
    }
    uint8_t *kind = calloc(bound, 1);  /* T_VAR / T_CHAIN */
    uint32_t *root = calloc(bound, sizeof(uint32_t)); /* the variable a tracked id is (into) */
    uint8_t *bad = calloc(bound, 1);   /* per variable: disqualified */
    uint8_t *stored = calloc(bound, 1); /* per variable: has a constant store */
    if (!kind || !root || !bad || !stored) {
        free(kind), free(root), free(bad), free(stored);
        spv_free(&m);
        *why = "out of memory";
        return -1;
    }
    unsigned cand = 0;
    for (uint32_t i = 0; i < m.count; i++) {
        const spv_inst *in = &m.ins[i];
        if (in->op != SpvOpVariable || in->n < 4 || in->w[2] != SpvStorageClassFunction || in->w[1] >= bound) continue;
        int pt = spv_find_def(&m, in->w[0]);
        if (pt < 0 || m.ins[pt].op != SpvOpTypePointer) continue;
        int at = spv_find_def(&m, m.ins[pt].w[2]);
        if (at < 0 || m.ins[at].op != SpvOpTypeArray) continue;
        if (in->n > 4) continue; /* an initializer: Function variables with one are rare; left as they are */
        kind[in->w[1]] = T_VAR, root[in->w[1]] = in->w[1];
        cand++;
    }
    if (!cand) {
        free(kind), free(root), free(bad), free(stored);
        spv_free(&m);
        return 0;
    }
    /* Follow the uses in order (an access chain is defined before its uses, in the same function) */
    for (uint32_t i = 0; i < m.count; i++) {
        const spv_inst *in = &m.ins[i];
        uint32_t nops = in->n - 1u;
        switch (in->op) {
        case SpvOpVariable:
            continue; /* the definitions themselves */
        case SpvOpName:
        case SpvOpDecorate:
            continue;
        case SpvOpStore:
            if (nops >= 2 && in->w[0] < bound && kind[in->w[0]]) {
                uint32_t r = root[in->w[0]];
                if (kind[in->w[0]] == T_VAR && is_const_value(&m, in->w[1])) stored[r] = 1;
                else bad[r] = 1;
            }
            if (nops >= 2 && in->w[1] < bound && kind[in->w[1]]) bad[root[in->w[1]]] = 1; /* a pointer stored */
            continue;
        case SpvOpLoad:
            /* w[0] type, w[1] result, w[2] pointer: a read is fine */
            for (uint32_t k = 3; k < nops; k++)
                if (in->w[k] < bound && kind[in->w[k]]) bad[root[in->w[k]]] = 1;
            continue;
        case SpvOpAccessChain:
        case SpvOpInBoundsAccessChain:
            if (nops >= 3 && in->w[2] < bound && kind[in->w[2]] && in->w[1] < bound) {
                kind[in->w[1]] = T_CHAIN, root[in->w[1]] = root[in->w[2]];
                for (uint32_t k = 3; k < nops; k++) /* an index that is itself a tracked pointer: not a table */
                    if (in->w[k] < bound && kind[in->w[k]]) bad[root[in->w[k]]] = 1;
                continue;
            }
            break;
        default:
            break;
        }
        /* any other instruction naming a tracked id (an id or a literal of the same value: either way the
         * variable stays where it is) */
        for (uint32_t k = 0; k < nops; k++)
            if (in->w[k] < bound && kind[in->w[k]]) bad[root[in->w[k]]] = 1;
    }
    unsigned moving = 0;
    for (uint32_t id = 0; id < bound; id++)
        if (kind[id] == T_VAR) {
            if (bad[id] || !stored[id]) st->skipped++;
            else moving++;
        }
    if (!moving) {
        free(kind), free(root), free(bad), free(stored);
        spv_free(&m);
        return 0;
    }
    /* Collect first (edits shift and may move the instructions), then edit by id */
    uint32_t *moved = calloc(moving, sizeof(uint32_t)), *marr = calloc(moving, sizeof(uint32_t));
    uint32_t nchain = 0, nm = 0;
    for (uint32_t i = 0; i < m.count; i++)
        if ((m.ins[i].op == SpvOpAccessChain || m.ins[i].op == SpvOpInBoundsAccessChain) && m.ins[i].n >= 4 &&
            m.ins[i].w[1] < bound && kind[m.ins[i].w[1]] == T_CHAIN && !bad[root[m.ins[i].w[1]]] && stored[root[m.ins[i].w[1]]])
            nchain++;
    uint32_t *cid = calloc(nchain ? nchain : 1, sizeof(uint32_t)), *cpt = calloc(nchain ? nchain : 1, sizeof(uint32_t));
    if (!moved || !marr || !cid || !cpt) {
        free(moved), free(marr), free(cid), free(cpt);
        free(kind), free(root), free(bad), free(stored);
        spv_free(&m);
        *why = "out of memory";
        return -1;
    }
    nchain = 0;
    for (uint32_t i = 0; i < m.count; i++) {
        const spv_inst *in = &m.ins[i];
        if ((in->op == SpvOpAccessChain || in->op == SpvOpInBoundsAccessChain) && in->n >= 4 && in->w[1] < bound &&
            kind[in->w[1]] == T_CHAIN && !bad[root[in->w[1]]] && stored[root[in->w[1]]]) {
            int pt = spv_find_def(&m, in->w[0]);
            if (pt < 0 || m.ins[pt].op != SpvOpTypePointer) continue;
            cid[nchain] = in->w[1], cpt[nchain++] = m.ins[pt].w[2];
        }
        if (in->op == SpvOpVariable && in->n == 4 && in->w[1] < bound && kind[in->w[1]] == T_VAR && !bad[in->w[1]] &&
            stored[in->w[1]] && in->w[2] == SpvStorageClassFunction && nm < moving) {
            int pt = spv_find_def(&m, in->w[0]);
            moved[nm] = in->w[1], marr[nm++] = pt >= 0 ? m.ins[pt].w[2] : 0;
        }
    }
    for (uint32_t v = 0; v < nm; v++) {
        int d = spv_find_def(&m, moved[v]);
        if (d >= 0) spv_remove(&m, (uint32_t)d);
    }
    for (uint32_t v = 0; v < nm; v++) {
        uint32_t ops[3] = {spv_type_pointer(&m, SpvStorageClassPrivate, marr[v]), moved[v], SpvStorageClassPrivate};
        spv_insert(&m, spv_types_end(&m), SpvOpVariable, ops, 3);
        st->vars++;
        st->words += array_len(&m, marr[v]);
    }
    for (uint32_t c = 0; c < nchain; c++) {
        uint32_t np = spv_type_pointer(&m, SpvStorageClassPrivate, cpt[c]);
        int d = spv_find_def(&m, cid[c]);
        if (d >= 0) m.ins[d].w[0] = np, st->chains++;
    }
    free(marr), free(cid), free(cpt);
    /* SPIR-V 1.4+: every global a shader uses is in its entry point's interface */
    if (moved && spv_version(&m) >= 0x00010400u) {
        for (uint32_t i = 0; i < m.count; i++) {
            if (m.ins[i].op != SpvOpEntryPoint) continue;
            uint32_t n = m.ins[i].n - 1u;
            uint32_t *ops = malloc((n + nm) * sizeof(uint32_t));
            if (!ops) break;
            memcpy(ops, m.ins[i].w, n * sizeof(uint32_t));
            memcpy(ops + n, moved, nm * sizeof(uint32_t));
            spv_remove(&m, i);
            spv_insert(&m, i, SpvOpEntryPoint, ops, (uint16_t)(n + nm));
            free(ops);
        }
    }
    free(moved);
    free(kind), free(root), free(bad), free(stored);
    if (m.err) {
        spv_free(&m);
        *why = "edit failed";
        memset(st, 0, sizeof(*st));
        return -1;
    }
    *out_size = spv_write(&m, out);
    spv_free(&m);
    if (!*out_size) {
        *why = "write failed";
        return -1;
    }
    return 1;
}
