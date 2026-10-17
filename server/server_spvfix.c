/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * SPIR-V fixes for Mali's shader compiler, applied to every module the app hands the driver.
 *
 * DXVK (1.7.3 and later) guards texture reads with OpSelect(cond, texel, vec4(0)) where cond is an
 * OpConstantComposite of OpSpecConstantTrue/False. Mali's compiler folds that composite as if it
 * were a plain constant - taking the spec constants at their default - and every texture reads
 * black. Declaring the composite OpSpecConstantComposite (same operands, same length) keeps it
 * specializable and the fold away. Found by leegao (bionic-vulkan-wrapper issue #93,
 * "OpCompositeConstant" write-up, 2025-08-10); Vortek patches the same pattern.
 *
 * The code is patched in place: it lives in the server's receive buffer (vkCreateShaderModule's
 * pCode, or a VkShaderModuleCreateInfo chained to a pipeline stage), so nothing is copied.
 * On for ARM GPUs; VKBRIDGE_SPIRV_FIX=1/0 forces it (host tests).
 *
 * Also here, for the same GPUs: Mali (r44) fails to build a compute pipeline whose layout holds
 * immutable YCbCr samplers (VK_ERROR_INITIALIZATION_FAILED), and gamescope's compositing layout
 * has 16 of them. In compute bindings they are replaced by one plain linear sampler: RGB content
 * composites as before; only NV12 surfaces (video) lose their YCbCr conversion.
 *
 * And: geometry and tessellation shaders get their loose per-vertex built-ins folded into
 * gl_PerVertex blocks (spirv_pervertex.c); loose ones crash Mali's compiler (tablet run 8, Steam's
 * first Zink pipeline). Those modules are rewritten into new code, not in place.
 *
 * And, with its own switch: an optimization barrier after every left shift by a constant
 * (spirv_shlbarrier.c, Lee Gao's MaliOptimizationBarrierPass): Mali's compiler folds such a shift
 * into the memory accesses around it and can turn them speculative - GPU faults, a lost device.
 * On for ARM GPUs (as in bionic-vulkan-wrapper, where it is on by default for Arm and Qualcomm);
 * VKBRIDGE_SHL_BARRIER=1/0 forces it (on a host GPU it alone installs these wrappers, without the
 * fixes above). A rewritten module is named by the app's code (vkb_errata_spv_origin), so the hang
 * detector's dumps and stuck-pipeline lists keep their names.
 */
#define _GNU_SOURCE
#include "vkb_server.h"
#include "server_errata.h"
#include "spirv_pervertex.h"
#include "spirv_shlbarrier.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OP_SPEC_CONSTANT_TRUE 48
#define OP_SPEC_CONSTANT_FALSE 49
#define OP_SPEC_CONSTANT 50
#define OP_SPEC_CONSTANT_COMPOSITE 51
#define OP_SPEC_CONSTANT_OP 52
#define OP_CONSTANT_COMPOSITE 44
#define OP_FUNCTION 54

/* The last modules created, kept so a pipeline the driver refuses can be dumped for a look
 * (<pipeline cache dir>/failed-<n>.spv, pulled with run-as): the driver says nothing about why. */
#define KEEP 64
typedef struct kept_module {
    VkShaderModule module;
    uint32_t *code;
    size_t size;
} kept_module;

typedef struct vkb_spvfix {
    PFN_vkCreateShaderModule create_module;
    PFN_vkCreateGraphicsPipelines create_gfx;
    PFN_vkCreateComputePipelines create_compute;
    unsigned long patched; /* composites changed, for the log */
    pthread_mutex_t lock;
    kept_module kept[KEEP];
    unsigned next_kept, dumped;
    PFN_vkCreateSampler create_sampler;
    PFN_vkDestroySampler destroy_sampler;
    PFN_vkCreateDescriptorSetLayout create_dsl;
    VkSampler ycbcr[64]; /* samplers created with a YCbCr conversion */
    unsigned nycbcr;
    VkSampler plain;     /* their stand-in in compute layouts */
    int fixes;           /* VKBRIDGE_SPIRV_FIX: the composites, per-vertex blocks, YCbCr samplers */
    int shl;             /* VKBRIDGE_SHL_BARRIER: barriers after constant left shifts */
    unsigned long shl_modules, shl_barriers, shl_seen;
} vkb_spvfix;

static vkb_spvfix *cur(void)
{
    vkb_srv_table *t = vkb_srv_current_table();
    return t ? t->spvfix : NULL;
}

/* Patches one module in place; returns the number of composites changed. */
static unsigned fix_module(uint32_t *w, size_t bytes)
{
    size_t n = bytes / 4;
    if (n < 5 || w[0] != 0x07230203u) return 0;
    uint32_t bound = w[3];
    if (!bound || bound > (1u << 24)) return 0;
    uint8_t *spec = calloc(bound, 1);
    if (!spec) return 0;
    unsigned changed = 0;
    for (size_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len || i + len > n) break;
        if (op == OP_FUNCTION) break; /* constants all come before the first function */
        if ((op == OP_SPEC_CONSTANT_TRUE || op == OP_SPEC_CONSTANT_FALSE || op == OP_SPEC_CONSTANT ||
             op == OP_SPEC_CONSTANT_COMPOSITE || op == OP_SPEC_CONSTANT_OP) && len >= 3 && w[i + 2] < bound) {
            spec[w[i + 2]] = 1;
        } else if (op == OP_CONSTANT_COMPOSITE && len >= 3 && w[i + 2] < bound) {
            int any = 0;
            for (uint32_t k = 3; k < len; k++) any |= w[i + k] < bound && spec[w[i + k]];
            if (any) {
                /* A composite of spec constants is itself one: declared so, it is no longer folded. */
                w[i] = (len << 16) | OP_SPEC_CONSTANT_COMPOSITE;
                spec[w[i + 2]] = 1;
                changed++;
            }
        }
        i += len;
    }
    free(spec);
    return changed;
}

static void note(vkb_spvfix *f, unsigned changed)
{
    if (!changed) return;
    unsigned long before = __atomic_fetch_add(&f->patched, changed, __ATOMIC_RELAXED);
    if (!before) VKB_INFO("SPIR-V fix: spec-constant composites redeclared for Mali's compiler (first in this device)");
}

static void fix_stages(vkb_spvfix *f, uint32_t n, const VkPipelineShaderStageCreateInfo *st)
{
    for (uint32_t i = 0; i < n; i++)
        for (const VkBaseInStructure *b = st[i].pNext; b; b = b->pNext)
            if (b->sType == VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO) {
                const VkShaderModuleCreateInfo *mi = (const void *)b;
                note(f, fix_module((uint32_t *)mi->pCode, mi->codeSize));
            }
}

static void dump_code(vkb_spvfix *f, const uint32_t *code, size_t size, const char *why)
{
    const char *dir = vkb_pcache_dir();
    if (!dir || f->dumped >= 16) return;
    char path[600];
    snprintf(path, sizeof(path), "%s/failed-%u.spv", dir, f->dumped++);
    FILE *o = fopen(path, "wb");
    if (!o && errno == ENOENT && vkb_cache_dir_ensure()) o = fopen(path, "wb"); /* the cache directory was removed */
    if (!o) return;
    fwrite(code, 1, size, o);
    fclose(o);
    VKB_ERR("%s: shader saved to %s", why, path);
}

/* The per-vertex block rewrite of one module's code; NULL when it is not needed (or not possible). */
static uint32_t *wrap_pervertex(const uint32_t *code, size_t size, size_t *out_size)
{
    uint32_t *out = NULL;
    const char *why = NULL;
    if (vkb_spv_wrap_pervertex(code, size, &out, out_size, &why)) {
        static int told;
        if (!__atomic_fetch_add(&told, 1, __ATOMIC_RELAXED))
            VKB_INFO("SPIR-V fix: per-vertex built-ins of geometry/tessellation shaders folded into gl_PerVertex blocks");
        return out;
    }
    if (why) VKB_WARN("SPIR-V fix: a geometry/tessellation shader kept its loose built-ins (%s); Mali may crash on it", why);
    return NULL;
}

/* The shl_barrier rewrite of one module's code; NULL when nothing changes. */
static uint32_t *shl_barrier(vkb_spvfix *f, const uint32_t *code, size_t size, size_t *out_size)
{
    if (!f->shl) return NULL;
    __atomic_add_fetch(&f->shl_seen, 1, __ATOMIC_RELAXED);
    uint32_t *out = NULL;
    vkb_shlbarrier_stats st;
    const char *why = NULL;
    int r = vkb_spv_shl_barrier(code, size, &out, out_size, &st, &why);
    if (r < 0) {
        VKB_ONCE(VKB_LOG_WARN, "SPIR-V fix: shl_barrier: a module left as it is (%s)", why);
        return NULL;
    }
    if (r == 0) return NULL;
    unsigned long before = __atomic_fetch_add(&f->shl_modules, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&f->shl_barriers, st.scalar + st.vector, __ATOMIC_RELAXED);
    if (!before)
        VKB_INFO("SPIR-V fix: shl_barrier: optimization barriers after constant left shifts (first module: %u scalar, %u vector)",
                 st.scalar, st.vector);
    return out;
}

/* The rewrites of one module's code (per-vertex blocks when pervertex, then shl_barrier): new
 * code (malloc'd, named by the app's code) or NULL when nothing changes. */
static uint32_t *rewrite(vkb_spvfix *f, const uint32_t *code, size_t size, int pervertex, size_t *out_size)
{
    size_t s1 = 0, s2 = 0;
    uint32_t *c1 = f->fixes && pervertex ? wrap_pervertex(code, size, &s1) : NULL;
    uint32_t *c2 = shl_barrier(f, c1 ? c1 : code, c1 ? s1 : size, &s2);
    if (c2) {
        free(c1);
        c1 = c2, s1 = s2;
    }
    if (c1) {
        vkb_errata_spv_origin(c1, s1, code, size);
        *out_size = s1;
    }
    return c1;
}

/* New code a pipeline call uses in place of a stage's inline module; freed after the call */
typedef struct {
    uint32_t **p;
    unsigned n, cap;
} repl_list;

static void repl_add(repl_list *l, uint32_t *code)
{
    if (l->n == l->cap) {
        unsigned cap = l->cap ? l->cap * 2 : 16;
        uint32_t **p = realloc(l->p, cap * sizeof(*p));
        if (!p) return; /* (never: the new code stays in use, leaked) */
        l->p = p, l->cap = cap;
    }
    l->p[l->n++] = code;
}

static void repl_free(repl_list *l)
{
    for (unsigned k = 0; k < l->n; k++) free(l->p[k]);
    free(l->p);
}

/* A stage's inline modules rewritten (the create infos are the server's own copies: changed in place) */
static void rewrite_stage(vkb_spvfix *f, const VkPipelineShaderStageCreateInfo *st, repl_list *l)
{
    int pervertex = (st->stage & (VK_SHADER_STAGE_GEOMETRY_BIT | VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT |
                                  VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT)) != 0;
    if (!f->shl && !(f->fixes && pervertex)) return;
    for (VkBaseInStructure *b = (VkBaseInStructure *)st->pNext; b; b = (VkBaseInStructure *)b->pNext) {
        if (b->sType != VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO) continue;
        VkShaderModuleCreateInfo *mi = (VkShaderModuleCreateInfo *)b;
        size_t sz = 0;
        uint32_t *code = rewrite(f, mi->pCode, mi->codeSize, pervertex, &sz);
        if (!code) continue;
        repl_add(l, code);
        mi->pCode = code;
        mi->codeSize = sz;
    }
}

static VkResult VKAPI_CALL fx_CreateShaderModule(VkDevice device, const VkShaderModuleCreateInfo *in_ci, const VkAllocationCallbacks *a,
                                                 VkShaderModule *out)
{
    vkb_spvfix *f = cur();
    if (f->fixes) note(f, fix_module((uint32_t *)in_ci->pCode, in_ci->codeSize));
    VkShaderModuleCreateInfo wrapped;
    const VkShaderModuleCreateInfo *ci = in_ci;
    size_t wsize = 0;
    uint32_t *wcode = rewrite(f, in_ci->pCode, in_ci->codeSize, 1, &wsize);
    if (wcode) {
        wrapped = *in_ci;
        wrapped.pCode = wcode;
        wrapped.codeSize = wsize;
        ci = &wrapped;
    }
    VkResult r = f->create_module(device, ci, a, out);
    if (r != VK_SUCCESS) {
        dump_code(f, ci->pCode, ci->codeSize, "vkCreateShaderModule failed");
        free(wcode);
        return r;
    }
    uint32_t *copy = malloc(ci->codeSize);
    if (copy) {
        memcpy(copy, ci->pCode, ci->codeSize);
        pthread_mutex_lock(&f->lock);
        kept_module *k = &f->kept[f->next_kept++ % KEEP];
        free(k->code);
        *k = (kept_module){*out, copy, ci->codeSize};
        pthread_mutex_unlock(&f->lock);
    }
    free(wcode);
    return r;
}

static void dump_stage(vkb_spvfix *f, const VkPipelineShaderStageCreateInfo *st, VkResult r)
{
    char why[96];
    snprintf(why, sizeof(why), "pipeline creation failed (%d), stage 0x%x", r, st->stage);
    for (const VkBaseInStructure *b = st->pNext; b; b = b->pNext)
        if (b->sType == VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO) {
            const VkShaderModuleCreateInfo *mi = (const void *)b;
            dump_code(f, mi->pCode, mi->codeSize, why);
            return;
        }
    pthread_mutex_lock(&f->lock);
    for (int i = 0; i < KEEP; i++)
        if (f->kept[i].module == st->module && f->kept[i].code) {
            dump_code(f, f->kept[i].code, f->kept[i].size, why);
            break;
        }
    pthread_mutex_unlock(&f->lock);
}

static VkResult VKAPI_CALL fx_CreateGraphicsPipelines(VkDevice device, VkPipelineCache cache, uint32_t n,
                                                      const VkGraphicsPipelineCreateInfo *ci, const VkAllocationCallbacks *a,
                                                      VkPipeline *out)
{
    vkb_spvfix *f = cur();
    /* Inline modules (in the request's buffer) that need a rewrite (per-vertex blocks, shl_barrier)
     * get new code for the call. */
    repl_list repl = {0};
    for (uint32_t i = 0; i < n; i++) {
        if (f->fixes) fix_stages(f, ci[i].stageCount, ci[i].pStages);
        for (uint32_t s = 0; s < ci[i].stageCount; s++) rewrite_stage(f, &ci[i].pStages[s], &repl);
    }
    VkResult r = f->create_gfx(device, cache, n, ci, a, out);
    if (r < 0)
        for (uint32_t i = 0; i < n; i++)
            for (uint32_t s = 0; s < ci[i].stageCount; s++) dump_stage(f, &ci[i].pStages[s], r);
    repl_free(&repl);
    return r;
}

static VkResult VKAPI_CALL fx_CreateComputePipelines(VkDevice device, VkPipelineCache cache, uint32_t n,
                                                     const VkComputePipelineCreateInfo *ci, const VkAllocationCallbacks *a,
                                                     VkPipeline *out)
{
    vkb_spvfix *f = cur();
    repl_list repl = {0};
    for (uint32_t i = 0; i < n; i++) {
        if (f->fixes) fix_stages(f, 1, &ci[i].stage);
        rewrite_stage(f, &ci[i].stage, &repl);
    }
    VkResult r = f->create_compute(device, cache, n, ci, a, out);
    if (r < 0)
        for (uint32_t i = 0; i < n; i++) dump_stage(f, &ci[i].stage, r);
    repl_free(&repl);
    return r;
}

static VkResult VKAPI_CALL fx_CreateSampler(VkDevice device, const VkSamplerCreateInfo *ci, const VkAllocationCallbacks *a, VkSampler *out)
{
    vkb_spvfix *f = cur();
    VkResult r = f->create_sampler(device, ci, a, out);
    if (r != VK_SUCCESS) return r;
    for (const VkBaseInStructure *b = ci->pNext; b; b = b->pNext)
        if (b->sType == VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO) {
            pthread_mutex_lock(&f->lock);
            if (f->nycbcr < sizeof(f->ycbcr) / sizeof(f->ycbcr[0])) f->ycbcr[f->nycbcr++] = *out;
            if (!f->plain) {
                VkSamplerCreateInfo pi = *ci;
                pi.pNext = NULL;
                if (f->create_sampler(device, &pi, NULL, &f->plain) != VK_SUCCESS) f->plain = VK_NULL_HANDLE;
            }
            pthread_mutex_unlock(&f->lock);
        }
    return r;
}

static void VKAPI_CALL fx_DestroySampler(VkDevice device, VkSampler s, const VkAllocationCallbacks *a)
{
    vkb_spvfix *f = cur();
    pthread_mutex_lock(&f->lock);
    for (unsigned i = 0; i < f->nycbcr; i++)
        if (f->ycbcr[i] == s) f->ycbcr[i] = f->ycbcr[--f->nycbcr];
    pthread_mutex_unlock(&f->lock);
    f->destroy_sampler(device, s, a);
}

static int is_ycbcr(vkb_spvfix *f, VkSampler s)
{
    for (unsigned i = 0; i < f->nycbcr; i++)
        if (f->ycbcr[i] == s) return 1;
    return 0;
}

static VkResult VKAPI_CALL fx_CreateDescriptorSetLayout(VkDevice device, const VkDescriptorSetLayoutCreateInfo *ci,
                                                        const VkAllocationCallbacks *a, VkDescriptorSetLayout *out)
{
    vkb_spvfix *f = cur();
    pthread_mutex_lock(&f->lock);
    int any = 0;
    for (uint32_t i = 0; i < ci->bindingCount && f->nycbcr && f->plain; i++) {
        const VkDescriptorSetLayoutBinding *b = &ci->pBindings[i];
        if (!(b->stageFlags & VK_SHADER_STAGE_COMPUTE_BIT) || !b->pImmutableSamplers) continue;
        for (uint32_t k = 0; k < b->descriptorCount; k++)
            if (is_ycbcr(f, b->pImmutableSamplers[k])) {
                /* The binding array lives in the request's buffer: swapped in place. */
                ((VkSampler *)b->pImmutableSamplers)[k] = f->plain;
                any = 1;
            }
    }
    pthread_mutex_unlock(&f->lock);
    static int told;
    if (any && !__atomic_fetch_add(&told, 1, __ATOMIC_RELAXED))
        VKB_WARN("compute layout: immutable YCbCr samplers replaced (Mali cannot build such pipelines); NV12 surfaces lose YCbCr conversion");
    return f->create_dsl(device, ci, a, out);
}

void vkb_spvfix_device_init(vkb_srv_table *t)
{
    VkPhysicalDeviceProperties p;
    t->real.vkGetPhysicalDeviceProperties(t->physical, &p);
    const char *env = vkb_srv_env("VKBRIDGE_SPIRV_FIX");
    /* ARM, or a GPU the startup probe saw fold DXVK's guard at the defaults (server_probe.c) */
    int on = env ? strcmp(env, "0") != 0 : p.vendorID == 0x13B5 || vkb_probe_flag(VKB_PROBE_FLAG_SPIRV_FIX);
    /* shl_barrier: ARM GPUs (a no-op the compiler cannot fold; the cost is one ALU op per constant
     * shift and, once, the driver's pipeline cache entries of the modules it changes) */
    const char *shl_env = vkb_srv_env("VKBRIDGE_SHL_BARRIER");
    int shl = shl_env && *shl_env ? strcmp(shl_env, "0") != 0 : p.vendorID == 0x13B5;
    if ((!on && !shl) || !t->dt.vkCreateShaderModule) return;
    vkb_spvfix *f = calloc(1, sizeof(*f));
    if (!f) return;
    f->fixes = on;
    f->shl = shl;
    pthread_mutex_init(&f->lock, NULL);
    vkb_dispatch *dt = &t->dt;
    f->create_module = dt->vkCreateShaderModule;
    f->create_gfx = dt->vkCreateGraphicsPipelines;
    f->create_compute = dt->vkCreateComputePipelines;
    dt->vkCreateShaderModule = fx_CreateShaderModule;
    dt->vkCreateGraphicsPipelines = fx_CreateGraphicsPipelines;
    dt->vkCreateComputePipelines = fx_CreateComputePipelines;
    f->create_sampler = dt->vkCreateSampler;
    f->destroy_sampler = dt->vkDestroySampler;
    f->create_dsl = dt->vkCreateDescriptorSetLayout;
    if (on) {
        dt->vkCreateSampler = fx_CreateSampler;
        dt->vkDestroySampler = fx_DestroySampler;
        dt->vkCreateDescriptorSetLayout = fx_CreateDescriptorSetLayout;
    }
    t->spvfix = f;
    VKB_INFO("device %u: Mali SPIR-V fixes %s, shl_barrier %s%s", t->id, on ? "on" : "off", shl ? "on" : "off",
             shl_env && *shl_env ? " (VKBRIDGE_SHL_BARRIER)" : "");
}

void vkb_spvfix_device_destroy(vkb_srv_table *t)
{
    vkb_spvfix *f = t->spvfix;
    if (!f) return;
    t->dt.vkCreateShaderModule = f->create_module;
    t->dt.vkCreateGraphicsPipelines = f->create_gfx;
    t->dt.vkCreateComputePipelines = f->create_compute;
    t->dt.vkCreateSampler = f->create_sampler;
    t->dt.vkDestroySampler = f->destroy_sampler;
    t->dt.vkCreateDescriptorSetLayout = f->create_dsl;
    if (f->plain) t->real.vkDestroySampler(t->device, f->plain, NULL);
    if (f->patched) VKB_INFO("device %u: SPIR-V fix changed %lu composites", t->id, f->patched);
    if (f->shl)
        VKB_INFO("device %u: shl_barrier: %lu of %lu module(s) rewritten, %lu barrier(s)", t->id, f->shl_modules, f->shl_seen,
                 f->shl_barriers);
    for (int i = 0; i < KEEP; i++) free(f->kept[i].code);
    pthread_mutex_destroy(&f->lock);
    free(f);
    t->spvfix = NULL;
}
