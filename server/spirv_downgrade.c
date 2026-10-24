/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * SPIR-V 1.5 / 1.6 modules lowered to 1.4 (or 1.6 to 1.5): see spirv_downgrade.h.
 */
#include "spirv_downgrade.h"
#include "spirv_edit.h"

#include <stdlib.h>
#include <string.h>

enum {
    OP_GROUP_BROADCAST = 337, OP_GROUP_SHUFFLE = 345, OP_CONSTANT_NULL = 46, OP_SPEC_CONSTANT = 50, OP_SPEC_CONSTANT_OP = 52,
    CAP_SHUFFLE = 65, CAP_LAYER = 69, CAP_VIEWPORT = 70, CAP_VIEWPORT_LAYER_EXT = 5254,
};

/* What a version took from an extension: capabilities lo..hi, or (cap 0) the opcode `op`. */
static const struct {
    uint32_t since;
    uint32_t lo, hi, op;
    const char *ext;
} promoted[] = {
    {VKB_SPV_1_5, 4448, 4450, 0, "SPV_KHR_8bit_storage"},
    {VKB_SPV_1_5, 5301, 5312, 0, "SPV_EXT_descriptor_indexing"},
    {VKB_SPV_1_5, 5345, 5346, 0, "SPV_KHR_vulkan_memory_model"},
    {VKB_SPV_1_5, 5347, 5347, 0, "SPV_KHR_physical_storage_buffer"},
    {VKB_SPV_1_5, CAP_LAYER, CAP_VIEWPORT, 0, "SPV_EXT_shader_viewport_index_layer"},
    {VKB_SPV_1_6, 5379, 5379, 0, "SPV_EXT_demote_to_helper_invocation"},
    {VKB_SPV_1_6, 6016, 6019, 0, "SPV_KHR_integer_dot_product"},
    {VKB_SPV_1_6, 0, 0, SpvOpTerminateInvocation, "SPV_KHR_terminate_invocation"},
};
#define NPROMOTED (sizeof(promoted) / sizeof(promoted[0]))

static int has_extension(const spv_mod *m, const char *name)
{
    size_t len = strlen(name);
    for (uint32_t i = 0; i < m->count; i++) {
        const spv_inst *in = &m->ins[i];
        if (in->op != SpvOpExtension || (size_t)(in->n - 1) * 4 <= len) continue;
        if (!memcmp(in->w, name, len) && ((const char *)in->w)[len] == 0) return 1;
    }
    return 0;
}

/* After the capabilities (and any extensions already there): where OpExtension goes. */
static uint32_t extension_slot(const spv_mod *m)
{
    uint32_t i = 0;
    while (i < m->count && (m->ins[i].op == SpvOpCapability || m->ins[i].op == SpvOpExtension)) i++;
    return i;
}

static void add_extension(spv_mod *m, const char *name)
{
    uint32_t words[32];
    size_t len = strlen(name), nw = len / 4 + 1;
    if (nw > 32) return;
    memset(words, 0, nw * 4);
    memcpy(words, name, len);
    spv_insert(m, extension_slot(m), SpvOpExtension, words, (uint16_t)nw);
}

static int is_constant(const spv_mod *m, uint32_t id)
{
    int d = spv_find_def(m, id);
    if (d < 0) return 0;
    uint16_t op = m->ins[d].op;
    return op == SpvOpConstant || op == OP_CONSTANT_NULL || op == OP_SPEC_CONSTANT || op == OP_SPEC_CONSTANT_OP;
}

int vkb_spv_downgrade(const uint32_t *code, size_t size, uint32_t target, uint32_t **out, size_t *out_size,
                      vkb_downgrade_stats *st, const char **why)
{
    vkb_downgrade_stats local;
    if (!st) st = &local;
    memset(st, 0, sizeof(*st));
    *why = NULL;
    if (size < 20 || (size & 3) || code[0] != 0x07230203u) {
        *why = "not SPIR-V";
        return -1;
    }
    st->from = code[1];
    if (code[1] <= target) return 0;
    if (target < VKB_SPV_1_4) {
        *why = "no lowering below SPIR-V 1.4 (the driver lacks VK_KHR_spirv_1_4)";
        return -1;
    }
    if (code[1] > VKB_SPV_1_6) {
        *why = "a SPIR-V version above 1.6";
        return -1;
    }
    spv_mod m;
    if (spv_parse(&m, code, size) < 0) {
        spv_free(&m);
        *why = "malformed module";
        return -1;
    }
    /* What the module uses, by the version it came in. */
    int need[NPROMOTED];
    memset(need, 0, sizeof(need));
    int layer_caps = 0;
    for (uint32_t i = 0; i < m.count; i++) {
        const spv_inst *in = &m.ins[i];
        for (size_t p = 0; p < NPROMOTED; p++) {
            if (promoted[p].since <= target || promoted[p].since > code[1]) continue;
            if (promoted[p].op ? in->op == promoted[p].op
                               : in->op == SpvOpCapability && in->n >= 2 && in->w[0] >= promoted[p].lo && in->w[0] <= promoted[p].hi)
                need[p] = 1;
        }
        if (target < VKB_SPV_1_5 && in->op == SpvOpCapability && in->n >= 2 && (in->w[0] == CAP_LAYER || in->w[0] == CAP_VIEWPORT))
            layer_caps++;
    }
    /* 1.5's dynamic-id broadcast: a shuffle (same operands: scope, value, id). */
    if (target < VKB_SPV_1_5) {
        for (uint32_t i = 0; i < m.count; i++) {
            spv_inst *in = &m.ins[i];
            if (in->op != OP_GROUP_BROADCAST || in->n < 6 || is_constant(&m, in->w[4])) continue;
            in->op = OP_GROUP_SHUFFLE;
            st->broadcasts++;
        }
        if (st->broadcasts) spv_add_capability(&m, CAP_SHUFFLE);
    }
    if (layer_caps) {
        for (uint32_t i = 0; i < m.count;) {
            if (m.ins[i].op == SpvOpCapability && m.ins[i].n >= 2 && (m.ins[i].w[0] == CAP_LAYER || m.ins[i].w[0] == CAP_VIEWPORT))
                spv_remove(&m, i);
            else
                i++;
        }
        spv_add_capability(&m, CAP_VIEWPORT_LAYER_EXT);
        st->layer_caps = (unsigned)layer_caps;
    }
    for (size_t p = 0; p < NPROMOTED; p++) {
        if (!need[p] || has_extension(&m, promoted[p].ext)) continue;
        add_extension(&m, promoted[p].ext);
        st->extensions++;
    }
    m.header[1] = target;
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
