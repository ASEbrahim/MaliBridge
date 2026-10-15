/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * clamp_zero (MK-32, Arm erratum 3779215; r38p1-r49p0 and r50p0-r51p0, fixed in r49p1 and
 * r52p0): a min/max/clamp with a constant zero limit on a value selected at run time from
 * constants is miscompiled - the clamp becomes a min or a max (Godot's fullscreen-triangle vertex
 * shaders, Godot PRs #92817 and #109994). The fix rewrites every module that holds the pattern
 * (spirv_clampzero.c: the zero becomes a load of a Private variable set to 0 at the top of each
 * entry point) on its way to the driver: vkCreateShaderModule, and the inline modules of
 * vkCreateGraphicsPipelines and vkCreateComputePipelines. Installed driver-near like every
 * erratum, chained with bitwise_scalar's wrappers of the same functions (this one above it).
 *
 * Only modules with the pattern change (22 of 17543 in the Fossilize corpus), and the new code is
 * a function of the old, so the driver's pipeline cache and the shader warm-up keep their entries
 * for every other module. Rewritten modules are named by the app's code (vkb_errata_spv_origin):
 * the compile guard's stuck-pipelines and stuck-spirv, the hang detector's h-<hash> names and the
 * shader discard list do not change with the fix.
 *
 * Scope. Forced on (VKBRIDGE_ERRATA=+clamp_zero) it rewrites every min/max/clamp with a zero
 * limit, whatever its value - the bisect kit's 14-opaquezero, byte for byte in shape: the
 * candidate fix for the Mali-G925 r49p1-beta compiler never finishing UE4's colour-grading LUT
 * (Stray), whose 54 and 57 zero-limit ops take no constant-selected value, so the narrow pattern
 * leaves those shaders alone. That wide form touches most modules (78% of the corpus): an A/B
 * switch, not a default. VKBRIDGE_CLAMP_ZERO_ALL=1 gives the wide form however the erratum came
 * on, =0 the narrow one even when forced. VKBRIDGE_CLAMP_ZERO_BFI=1 takes the zero from
 * bitcast(OpBitFieldInsert(bitcast(0), 0, 0, 0)) instead of the Private variable (the kit's 14a).
 * For the A/B on a device that already gave the LUT its stand-in, the compile guard's memory has
 * to go too: delete <cache dir>/stuck-pipelines and set VKBRIDGE_PROBE_APPLY=0 (the startup
 * probe's "LUT stand-in at once"), or the fixed code never reaches the driver.
 *
 * Logged: each distinct rewritten module once ("clamp_zero: module h-<app hash>: N op(s) ...";
 * the first 64 per device at info level, the rest at debug), a module the pass cannot rewrite
 * with the reason, and a device total at vkDestroyDevice.
 *
 * Imitation (host): the same hooks beneath the fix, with the pass in VKB_CLAMP_ZERO_IMITATE mode -
 * each pattern op loses its zero limit (clamp(x, 0, hi) -> min(x, hi), max(x, 0) -> x). The fixed
 * code holds no constant zero limit, so with the fix above it the imitation has nothing to break.
 */
#define _GNU_SOURCE
#include "server_errata.h"
#include "spirv_clampzero.h"

#include <stdlib.h>
#include <string.h>

#define LOG_INFO_MAX 64
#define INLINE_MAX 64

typedef struct vkb_errata_clampzero {
    unsigned flags;
    PFN_vkCreateShaderModule fix_mod, imi_mod;
    PFN_vkCreateGraphicsPipelines fix_gfx, imi_gfx;
    PFN_vkCreateComputePipelines fix_comp, imi_comp;
    unsigned long seen, rewritten, ops, failed, imitated, logged;
} vkb_errata_clampzero;

static vkb_errata_clampzero *last_state;

static uint64_t fnv1a(const uint32_t *code, size_t size)
{
    uint64_t h = 0xcbf29ce484222325ull;
    const uint8_t *b = (const uint8_t *)code;
    for (size_t i = 0; i < size; i++) h = (h ^ b[i]) * 0x100000001b3ull;
    return h;
}

static vkb_errata_clampzero *state(void)
{
    vkb_errata_dev *e = vkb_errata_cur();
    return e && e->clampzero ? e->clampzero : __atomic_load_n(&last_state, __ATOMIC_ACQUIRE);
}

/* The fix (or the imitation) on one module: 1 with *out the new code to use (the caller frees it). */
static int rewrite(vkb_errata_clampzero *s, int imitate, const uint32_t *code, size_t size, uint32_t **out, size_t *out_size)
{
    if (!code || size < 20) return 0;
    vkb_clampzero_stats st;
    const char *why;
    int r = vkb_spv_clamp_zero(code, size, imitate ? VKB_CLAMP_ZERO_IMITATE : s->flags, out, out_size, &st, &why);
    if (imitate) {
        if (r == 1) __atomic_add_fetch(&s->imitated, 1, __ATOMIC_RELAXED);
        return r == 1;
    }
    __atomic_add_fetch(&s->seen, 1, __ATOMIC_RELAXED);
    if (r < 0) {
        __atomic_add_fetch(&s->failed, 1, __ATOMIC_RELAXED);
        VKB_WARN("errata: clamp_zero: module h-%016llx left as it is (%s); Mali may miscompile it",
                 (unsigned long long)vkb_spv_app_hash(code, size), why);
        return 0;
    }
    if (r == 0) return 0;
    __atomic_add_fetch(&s->rewritten, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&s->ops, st.ops, __ATOMIC_RELAXED);
    uint64_t app = vkb_spv_app_hash(code, size);
    if (vkb_errata_spv_origin(*out, *out_size, code, size)) {
        unsigned long n = __atomic_fetch_add(&s->logged, 1, __ATOMIC_RELAXED);
        if (n < LOG_INFO_MAX || vkb_log_level() >= VKB_LOG_DEBUG)
            vkb_log(n < LOG_INFO_MAX ? VKB_LOG_INFO : VKB_LOG_DEBUG,
                    "errata: clamp_zero: module h-%016llx: %u op(s) given an opaque zero (clamp %u, min/max %u; value by select %u, "
                    "phi %u, memory %u%s; %s), %zu -> %zu bytes, driver code h-%016llx%s",
                    (unsigned long long)app, st.ops, st.clamps, st.minmax, st.by_select, st.by_phi, st.by_memory,
                    st.by_all ? ", every zero limit" : "", s->flags & VKB_CLAMP_ZERO_BFI ? "BitFieldInsert" : "Private variable",
                    size, *out_size, (unsigned long long)fnv1a(*out, *out_size),
                    n == LOG_INFO_MAX - 1 ? " (further modules at debug level)" : "");
    }
    return 1;
}

static VkResult module_call(int imitate, VkDevice d, const VkShaderModuleCreateInfo *ci, const VkAllocationCallbacks *a, VkShaderModule *out)
{
    vkb_errata_clampzero *s = state();
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

/* Inline modules of a batch's stages, rewritten in place for the call (and put back after: the
 * create infos may be a layer's own). */
typedef struct {
    VkShaderModuleCreateInfo *mi;
    const uint32_t *code;
    size_t size;
    uint32_t *repl;
} inline_repl;

static unsigned stages_rewrite(vkb_errata_clampzero *s, int imitate, uint32_t n, const VkPipelineShaderStageCreateInfo *st, inline_repl *r,
                               unsigned nr)
{
    for (uint32_t i = 0; i < n; i++)
        for (const VkBaseInStructure *b = st[i].pNext; b; b = b->pNext) {
            if (b->sType != VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO) continue;
            VkShaderModuleCreateInfo *mi = (VkShaderModuleCreateInfo *)b;
            uint32_t *code;
            size_t size;
            if (nr == INLINE_MAX) {
                VKB_WARN("errata: clamp_zero: more than %d inline modules in one call; the rest left as they are", INLINE_MAX);
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
    vkb_errata_clampzero *s = state();
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
    vkb_errata_clampzero *s = state();
    inline_repl r[INLINE_MAX];
    unsigned nr = 0;
    for (uint32_t i = 0; i < n; i++) nr = stages_rewrite(s, imitate, 1, &ci[i].stage, r, nr);
    VkResult res = (imitate ? s->imi_comp : s->fix_comp)(d, c, n, ci, a, out);
    stages_restore(r, nr);
    return res;
}

static VKAPI_ATTR VkResult VKAPI_CALL cz_CreateShaderModule(VkDevice d, const VkShaderModuleCreateInfo *ci, const VkAllocationCallbacks *a,
                                                            VkShaderModule *out)
{
    return module_call(0, d, ci, a, out);
}

static VKAPI_ATTR VkResult VKAPI_CALL czi_CreateShaderModule(VkDevice d, const VkShaderModuleCreateInfo *ci, const VkAllocationCallbacks *a,
                                                             VkShaderModule *out)
{
    return module_call(1, d, ci, a, out);
}

static VKAPI_ATTR VkResult VKAPI_CALL cz_CreateGraphicsPipelines(VkDevice d, VkPipelineCache c, uint32_t n, const VkGraphicsPipelineCreateInfo *ci,
                                                                 const VkAllocationCallbacks *a, VkPipeline *out)
{
    return gfx_call(0, d, c, n, ci, a, out);
}

static VKAPI_ATTR VkResult VKAPI_CALL czi_CreateGraphicsPipelines(VkDevice d, VkPipelineCache c, uint32_t n, const VkGraphicsPipelineCreateInfo *ci,
                                                                  const VkAllocationCallbacks *a, VkPipeline *out)
{
    return gfx_call(1, d, c, n, ci, a, out);
}

static VKAPI_ATTR VkResult VKAPI_CALL cz_CreateComputePipelines(VkDevice d, VkPipelineCache c, uint32_t n, const VkComputePipelineCreateInfo *ci,
                                                                const VkAllocationCallbacks *a, VkPipeline *out)
{
    return comp_call(0, d, c, n, ci, a, out);
}

static VKAPI_ATTR VkResult VKAPI_CALL czi_CreateComputePipelines(VkDevice d, VkPipelineCache c, uint32_t n, const VkComputePipelineCreateInfo *ci,
                                                                 const VkAllocationCallbacks *a, VkPipeline *out)
{
    return comp_call(1, d, c, n, ci, a, out);
}

static int env_flag(const char *name)
{
    const char *v = vkb_srv_env(name);
    return !v || !*v ? -1 : strcmp(v, "0") != 0;
}

static vkb_errata_clampzero *get_state(vkb_errata_dev *e)
{
    if (!e->clampzero) e->clampzero = calloc(1, sizeof(*e->clampzero));
    return e->clampzero;
}

void vkb_errata_clampzero_install(vkb_srv_table *t, vkb_errata_dev *e)
{
    vkb_errata_clampzero *s = get_state(e);
    if (!s || !t->real.vkCreateShaderModule || !t->real.vkCreateGraphicsPipelines || !t->real.vkCreateComputePipelines) return;
    int forced = e->forced >> VKB_ERRATUM_CLAMP_ZERO & 1, all = env_flag("VKBRIDGE_CLAMP_ZERO_ALL");
    s->flags = 0;
    if (all == 1 || (all < 0 && forced)) s->flags |= VKB_CLAMP_ZERO_ALL;
    if (env_flag("VKBRIDGE_CLAMP_ZERO_BFI") == 1) s->flags |= VKB_CLAMP_ZERO_BFI;
    if (!e->probe)
        VKB_INFO("errata: clamp_zero gives %s an opaque zero (%s)%s",
                 s->flags & VKB_CLAMP_ZERO_ALL ? "every min/max/clamp with a constant zero limit" : "min/max/clamp of constant-selected values",
                 s->flags & VKB_CLAMP_ZERO_BFI ? "OpBitFieldInsert, VKBRIDGE_CLAMP_ZERO_BFI" : "a Private variable",
                 all == 1 ? " - VKBRIDGE_CLAMP_ZERO_ALL" : all < 0 && forced ? " - forced on, the wide form (VKBRIDGE_CLAMP_ZERO_ALL=0: the narrow one)" : "");
    s->fix_mod = t->real.vkCreateShaderModule;
    s->fix_gfx = t->real.vkCreateGraphicsPipelines;
    s->fix_comp = t->real.vkCreateComputePipelines;
    if (!s->imi_mod) s->imi_mod = s->fix_mod, s->imi_gfx = s->fix_gfx, s->imi_comp = s->fix_comp;
    ERRATA_HOOK(t, vkCreateShaderModule, cz_CreateShaderModule);
    ERRATA_HOOK(t, vkCreateGraphicsPipelines, cz_CreateGraphicsPipelines);
    ERRATA_HOOK(t, vkCreateComputePipelines, cz_CreateComputePipelines);
    if (!e->probe) __atomic_store_n(&last_state, s, __ATOMIC_RELEASE);
}

void vkb_errata_clampzero_imitate(vkb_srv_table *t, vkb_errata_dev *e)
{
    vkb_errata_clampzero *s = get_state(e);
    if (!s || !t->real.vkCreateShaderModule || !t->real.vkCreateGraphicsPipelines || !t->real.vkCreateComputePipelines) return;
    s->imi_mod = t->real.vkCreateShaderModule;
    s->imi_gfx = t->real.vkCreateGraphicsPipelines;
    s->imi_comp = t->real.vkCreateComputePipelines;
    if (!s->fix_mod) s->fix_mod = s->imi_mod, s->fix_gfx = s->imi_gfx, s->fix_comp = s->imi_comp;
    ERRATA_HOOK(t, vkCreateShaderModule, czi_CreateShaderModule);
    ERRATA_HOOK(t, vkCreateGraphicsPipelines, czi_CreateGraphicsPipelines);
    ERRATA_HOOK(t, vkCreateComputePipelines, czi_CreateComputePipelines);
    if (!e->probe) __atomic_store_n(&last_state, s, __ATOMIC_RELEASE);
}

void vkb_errata_clampzero_destroy(vkb_srv_table *t, vkb_errata_dev *e)
{
    vkb_errata_clampzero *s = e->clampzero;
    if (!s) return;
    if (e->on >> VKB_ERRATUM_CLAMP_ZERO & 1)
        VKB_INFO("errata: clamp_zero: device %u: %lu of %lu module(s) rewritten (%lu op(s) given an opaque zero), %lu left as they were",
                 t->id, s->rewritten, s->seen, s->ops, s->failed);
    if ((e->imitated >> VKB_ERRATUM_CLAMP_ZERO & 1) && !e->probe)
        VKB_INFO("errata: clamp_zero: device %u: the imitation broke %lu module(s)", t->id, s->imitated);
    e->clampzero = NULL;
    /* the last one made stays: a server thread that finds no table falls back to it */
    if (e->probe || __atomic_load_n(&last_state, __ATOMIC_ACQUIRE) != s) free(s);
}
