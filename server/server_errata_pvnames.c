/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * pv_names (PV-01, PowerVR DXT 25.3 on Android 17; the Pixel 10 family): a vertex shader whose
 * input and output variable share an OpName reads that input as zero. DXVK names a
 * passed-through attribute and its varying alike ("TEXCOORD"), so D3D11 games drew one flat colour
 * or garbage. Names are debug information: the fix drops every OpName and OpMemberName of a module
 * on its way to the driver (spirv_names.c), on vkCreateShaderModule and on the inline modules of
 * vkCreateGraphicsPipelines / vkCreateComputePipelines (maintenance5).
 *
 * On only where the startup probe pv_names saw the bug (a driver without it keeps its names);
 * VKBRIDGE_ERRATA=+pv_names forces it. The new code is a function of the old (the same bytes at
 * every launch), so the driver's pipeline cache keeps its entries; each rewritten module is
 * registered under the app's hash (vkb_errata_spv_origin), so the layers beneath, which name a
 * module by the hash of the code they see, keep the app's names in their logs and dumps.
 *
 * Logged: the first modules rewritten (a few, at info level), a device total at vkDestroyDevice.
 *
 * Imitation (host): the same hooks beneath the fix, with the bug itself (vkb_spv_names_imitate):
 * every store to an output variable that shares an OpName with an input stores zero. The fixed
 * code has no names, so with the fix above it the imitation finds nothing to break.
 */
#define _GNU_SOURCE
#include "server_errata.h"
#include "spirv_names.h"

#include <stdlib.h>
#include <string.h>

#define LOG_INFO_MAX 8
#define INLINE_MAX 64

typedef struct vkb_errata_pvnames {
    PFN_vkCreateShaderModule fix_mod, imi_mod;
    PFN_vkCreateGraphicsPipelines fix_gfx, imi_gfx;
    PFN_vkCreateComputePipelines fix_comp, imi_comp;
    unsigned long seen, stripped, bytes_saved, imitated, logged;
} vkb_errata_pvnames;

static vkb_errata_pvnames *last_state;

static vkb_errata_pvnames *state(void)
{
    vkb_errata_dev *e = vkb_errata_cur();
    return e && e->pvnames ? e->pvnames : __atomic_load_n(&last_state, __ATOMIC_ACQUIRE);
}

/* The fix (or the imitation) on one module: 1 with *out the new code to use (the caller frees it) */
static int rewrite(vkb_errata_pvnames *s, int imitate, const uint32_t *code, size_t size, uint32_t **out, size_t *out_size)
{
    if (!code || size < 20) return 0;
    if (imitate) {
        unsigned shared = 0;
        int r = vkb_spv_names_imitate(code, size, out, out_size, &shared);
        if (r == 1) __atomic_add_fetch(&s->imitated, 1, __ATOMIC_RELAXED);
        return r == 1;
    }
    __atomic_add_fetch(&s->seen, 1, __ATOMIC_RELAXED);
    int r = vkb_spv_strip_names(code, size, out, out_size);
    if (r != 1) return 0; /* no names, or not a module the pass can read: left as it is */
    __atomic_add_fetch(&s->stripped, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&s->bytes_saved, size - *out_size, __ATOMIC_RELAXED);
    if (vkb_errata_spv_origin(*out, *out_size, code, size)) {
        unsigned long n = __atomic_fetch_add(&s->logged, 1, __ATOMIC_RELAXED);
        if (n < LOG_INFO_MAX || vkb_log_level() >= VKB_LOG_DEBUG)
            vkb_log(n < LOG_INFO_MAX ? VKB_LOG_INFO : VKB_LOG_DEBUG, "errata: pv_names: module %zu -> %zu bytes, names dropped%s", size, *out_size,
                    n == LOG_INFO_MAX - 1 ? " (further modules at debug level)" : "");
    }
    return 1;
}

static VkResult module_call(int imitate, VkDevice d, const VkShaderModuleCreateInfo *ci, const VkAllocationCallbacks *a, VkShaderModule *out)
{
    vkb_errata_pvnames *s = state();
    PFN_vkCreateShaderModule next = imitate ? s->imi_mod : s->fix_mod;
    uint32_t *code = NULL;
    size_t size = 0;
    if (!rewrite(s, imitate, ci->pCode, ci->codeSize, &code, &size)) return next(d, ci, a, out);
    VkShaderModuleCreateInfo c = *ci;
    c.pCode = code;
    c.codeSize = size;
    VkResult r = next(d, &c, a, out);
    free(code);
    return r;
}

/* Inline modules of a batch's stages, rewritten in place for the call and put back after */
typedef struct {
    VkShaderModuleCreateInfo *mi;
    const uint32_t *code;
    size_t size;
    uint32_t *repl;
} inline_repl;

static unsigned stages_rewrite(vkb_errata_pvnames *s, int imitate, uint32_t n, const VkPipelineShaderStageCreateInfo *st, inline_repl *r, unsigned nr)
{
    for (uint32_t i = 0; i < n; i++)
        for (const VkBaseInStructure *b = st[i].pNext; b; b = b->pNext) {
            if (b->sType != VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO) continue;
            VkShaderModuleCreateInfo *mi = (VkShaderModuleCreateInfo *)b;
            uint32_t *code;
            size_t size;
            if (nr == INLINE_MAX) {
                VKB_WARN("errata: pv_names: more than %d inline modules in one call; the rest left as they are", INLINE_MAX);
                return nr;
            }
            if (!rewrite(s, imitate, mi->pCode, mi->codeSize, &code, &size)) continue;
            r[nr++] = (inline_repl){mi, mi->pCode, mi->codeSize, code};
            mi->pCode = code;
            mi->codeSize = size;
        }
    return nr;
}

static void stages_restore(inline_repl *r, unsigned nr)
{
    for (unsigned k = 0; k < nr; k++) {
        r[k].mi->pCode = r[k].code;
        r[k].mi->codeSize = r[k].size;
        free(r[k].repl);
    }
}

static VkResult gfx_call(int imitate, VkDevice d, VkPipelineCache c, uint32_t n, const VkGraphicsPipelineCreateInfo *ci,
                         const VkAllocationCallbacks *a, VkPipeline *out)
{
    vkb_errata_pvnames *s = state();
    inline_repl r[INLINE_MAX];
    unsigned nr = 0;
    for (uint32_t i = 0; i < n; i++) nr = stages_rewrite(s, imitate, ci[i].stageCount, ci[i].pStages, r, nr);
    VkResult res = (imitate ? s->imi_gfx : s->fix_gfx)(d, c, n, ci, a, out);
    stages_restore(r, nr);
    return res;
}

static VkResult comp_call(int imitate, VkDevice d, VkPipelineCache c, uint32_t n, const VkComputePipelineCreateInfo *ci,
                          const VkAllocationCallbacks *a, VkPipeline *out)
{
    vkb_errata_pvnames *s = state();
    inline_repl r[INLINE_MAX];
    unsigned nr = 0;
    for (uint32_t i = 0; i < n; i++) nr = stages_rewrite(s, imitate, 1, &ci[i].stage, r, nr);
    VkResult res = (imitate ? s->imi_comp : s->fix_comp)(d, c, n, ci, a, out);
    stages_restore(r, nr);
    return res;
}

static VKAPI_ATTR VkResult VKAPI_CALL pn_CreateShaderModule(VkDevice d, const VkShaderModuleCreateInfo *ci, const VkAllocationCallbacks *a,
                                                            VkShaderModule *out)
{
    return module_call(0, d, ci, a, out);
}

static VKAPI_ATTR VkResult VKAPI_CALL pni_CreateShaderModule(VkDevice d, const VkShaderModuleCreateInfo *ci, const VkAllocationCallbacks *a,
                                                             VkShaderModule *out)
{
    return module_call(1, d, ci, a, out);
}

static VKAPI_ATTR VkResult VKAPI_CALL pn_CreateGraphicsPipelines(VkDevice d, VkPipelineCache c, uint32_t n, const VkGraphicsPipelineCreateInfo *ci,
                                                                 const VkAllocationCallbacks *a, VkPipeline *out)
{
    return gfx_call(0, d, c, n, ci, a, out);
}

static VKAPI_ATTR VkResult VKAPI_CALL pni_CreateGraphicsPipelines(VkDevice d, VkPipelineCache c, uint32_t n, const VkGraphicsPipelineCreateInfo *ci,
                                                                  const VkAllocationCallbacks *a, VkPipeline *out)
{
    return gfx_call(1, d, c, n, ci, a, out);
}

static VKAPI_ATTR VkResult VKAPI_CALL pn_CreateComputePipelines(VkDevice d, VkPipelineCache c, uint32_t n, const VkComputePipelineCreateInfo *ci,
                                                                const VkAllocationCallbacks *a, VkPipeline *out)
{
    return comp_call(0, d, c, n, ci, a, out);
}

static VKAPI_ATTR VkResult VKAPI_CALL pni_CreateComputePipelines(VkDevice d, VkPipelineCache c, uint32_t n, const VkComputePipelineCreateInfo *ci,
                                                                 const VkAllocationCallbacks *a, VkPipeline *out)
{
    return comp_call(1, d, c, n, ci, a, out);
}

static vkb_errata_pvnames *get_state(vkb_errata_dev *e)
{
    if (!e->pvnames) e->pvnames = calloc(1, sizeof(*e->pvnames));
    return e->pvnames;
}

void vkb_errata_pvnames_install(vkb_srv_table *t, vkb_errata_dev *e)
{
    vkb_errata_pvnames *s = get_state(e);
    if (!s || !t->real.vkCreateShaderModule || !t->real.vkCreateGraphicsPipelines || !t->real.vkCreateComputePipelines) return;
    s->fix_mod = t->real.vkCreateShaderModule;
    s->fix_gfx = t->real.vkCreateGraphicsPipelines;
    s->fix_comp = t->real.vkCreateComputePipelines;
    if (!s->imi_mod) s->imi_mod = s->fix_mod, s->imi_gfx = s->fix_gfx, s->imi_comp = s->fix_comp;
    ERRATA_HOOK(t, vkCreateShaderModule, pn_CreateShaderModule);
    ERRATA_HOOK(t, vkCreateGraphicsPipelines, pn_CreateGraphicsPipelines);
    ERRATA_HOOK(t, vkCreateComputePipelines, pn_CreateComputePipelines);
    if (!e->probe) __atomic_store_n(&last_state, s, __ATOMIC_RELEASE);
}

void vkb_errata_pvnames_imitate(vkb_srv_table *t, vkb_errata_dev *e)
{
    vkb_errata_pvnames *s = get_state(e);
    if (!s || !t->real.vkCreateShaderModule || !t->real.vkCreateGraphicsPipelines || !t->real.vkCreateComputePipelines) return;
    s->imi_mod = t->real.vkCreateShaderModule;
    s->imi_gfx = t->real.vkCreateGraphicsPipelines;
    s->imi_comp = t->real.vkCreateComputePipelines;
    if (!s->fix_mod) s->fix_mod = s->imi_mod, s->fix_gfx = s->imi_gfx, s->fix_comp = s->imi_comp;
    ERRATA_HOOK(t, vkCreateShaderModule, pni_CreateShaderModule);
    ERRATA_HOOK(t, vkCreateGraphicsPipelines, pni_CreateGraphicsPipelines);
    ERRATA_HOOK(t, vkCreateComputePipelines, pni_CreateComputePipelines);
    if (!e->probe) __atomic_store_n(&last_state, s, __ATOMIC_RELEASE);
}

void vkb_errata_pvnames_destroy(vkb_srv_table *t, vkb_errata_dev *e)
{
    vkb_errata_pvnames *s = e->pvnames;
    if (!s) return;
    if (e->on >> VKB_ERRATUM_PV_NAMES & 1)
        VKB_INFO("errata: pv_names: device %u: names dropped from %lu of %lu module(s), %lu bytes less", t->id, s->stripped, s->seen, s->bytes_saved);
    if ((e->imitated >> VKB_ERRATUM_PV_NAMES & 1) && !e->probe)
        VKB_INFO("errata: pv_names: device %u: the imitation broke %lu module(s)", t->id, s->imitated);
    e->pvnames = NULL;
    /* the last one made stays: a server thread that finds no table falls back to it */
    if (e->probe || __atomic_load_n(&last_state, __ATOMIC_ACQUIRE) != s) free(s);
}
