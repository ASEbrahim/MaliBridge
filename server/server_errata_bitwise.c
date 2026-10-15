/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * bitwise_scalar (MK-03, Arm erratum 3843237, r18p0-r53p0): an integer vector bitwise op (or
 * shift) whose operand is a lane swizzle or a vector built lane by lane is miscompiled. The fix
 * rewrites every module that holds the pattern (spirv_bitwise.c: one scalar op per lane, then an
 * OpCompositeConstruct) on its way to the driver: vkCreateShaderModule, and the inline modules
 * (VkShaderModuleCreateInfo in a stage's pNext) of vkCreateGraphicsPipelines and
 * vkCreateComputePipelines. Installed driver-near like every erratum, so the bridge's own shaders
 * are covered too.
 *
 * Only modules with the pattern change, and the new code is a function of the old (content-keyed,
 * the same bytes every launch), so the driver's pipeline cache and the shader warm-up keep their
 * entries for every other module; a module that changes misses once, when the fix first turns on.
 *
 * Hashes stay the app's: the layers beneath (the compile guard's stuck-pipelines and stuck-spirv,
 * the hang detector's h-<hash> names, the shader discard list) name a module by the FNV-1a hash
 * of the SPIR-V they see, which for a rewritten module is the new code. Each rewrite is
 * registered (new code's hash -> the app's), and vkb_spv_app_hash() - what those layers hash
 * with - maps it back: a stuck-pipelines line written with the fix on matches with it off (a mode
 * switch), and the names in the logs are those of the app's modules. A saved .spv under such a
 * name is the code the driver got.
 *
 * Logged: each distinct rewritten module once ("bitwise_scalar: module h-<app hash>: N op(s)
 * scalarised ..."; the first 64 per device at info level, the rest at debug), a module the pass
 * cannot rewrite with the reason, and a device total at vkDestroyDevice.
 * VKBRIDGE_BITWISE_ALL=1 rewrites every integer vector bitwise op, not only the pattern (the
 * plan's wider scope; for measuring).
 *
 * Imitation (host): the same hooks beneath the fix, with the pass in VKB_BITWISE_IMITATE mode -
 * each pattern operand replaced by the splat of its lane 0, the lane swizzle lost. The fixed code
 * holds no pattern op, so with the fix above it the imitation has nothing to break.
 */
#define _GNU_SOURCE
#include "server_errata.h"
#include "spirv_bitwise.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define LOG_INFO_MAX 64
#define ORIGIN_SLOTS 16384 /* power of two */
#define INLINE_MAX 64

typedef struct vkb_errata_bitwise {
    unsigned flags;
    /* what each set of wrappers calls: the functions t->real held when it was installed */
    PFN_vkCreateShaderModule fix_mod, imi_mod;
    PFN_vkCreateGraphicsPipelines fix_gfx, imi_gfx;
    PFN_vkCreateComputePipelines fix_comp, imi_comp;
    unsigned long seen, rewritten, ops, failed, imitated, logged;
} vkb_errata_bitwise;

static vkb_errata_bitwise *last_state;

/* The rewrites' origins: new code's hash -> the app's */
typedef struct {
    uint64_t out, in;
} origin;
static origin origins[ORIGIN_SLOTS];
static unsigned norigins;
static pthread_mutex_t origin_lock = PTHREAD_MUTEX_INITIALIZER;

static uint64_t fnv1a(const uint32_t *code, size_t size)
{
    uint64_t h = 0xcbf29ce484222325ull;
    const uint8_t *b = (const uint8_t *)code;
    for (size_t i = 0; i < size; i++) h = (h ^ b[i]) * 0x100000001b3ull;
    return h;
}

/* Registers out -> in; returns 1 when out was new (a module not seen before). A full table drops
 * the slot it lands on (bounded; a dropped one only gets the new code's hash). */
static int origin_add(uint64_t out, uint64_t in)
{
    unsigned b = (unsigned)(out ^ out >> 29) & (ORIGIN_SLOTS - 1);
    int added = 1;
    pthread_mutex_lock(&origin_lock);
    for (unsigned i = 0; i < 32; i++) {
        origin *o = &origins[(b + i) & (ORIGIN_SLOTS - 1)];
        if (o->out == out) {
            added = 0;
            break;
        }
        if (!o->out || i == 31) {
            if (!o->out) __atomic_store_n(&norigins, norigins + 1, __ATOMIC_RELEASE);
            o->out = out, o->in = in;
            break;
        }
    }
    pthread_mutex_unlock(&origin_lock);
    return added;
}

uint64_t vkb_spv_app_hash(const uint32_t *code, size_t size)
{
    uint64_t h = fnv1a(code, size);
    if (!__atomic_load_n(&norigins, __ATOMIC_ACQUIRE)) return h;
    unsigned b = (unsigned)(h ^ h >> 29) & (ORIGIN_SLOTS - 1);
    uint64_t r = h;
    pthread_mutex_lock(&origin_lock);
    for (unsigned i = 0; i < 32; i++) {
        origin *o = &origins[(b + i) & (ORIGIN_SLOTS - 1)];
        if (!o->out) break;
        if (o->out == h) {
            r = o->in;
            break;
        }
    }
    pthread_mutex_unlock(&origin_lock);
    return r;
}

int vkb_errata_spv_origin(const uint32_t *out, size_t out_size, const uint32_t *in, size_t in_size)
{
    return origin_add(fnv1a(out, out_size), vkb_spv_app_hash(in, in_size));
}

static vkb_errata_bitwise *state(void)
{
    vkb_errata_dev *e = vkb_errata_cur();
    return e && e->bitwise ? e->bitwise : __atomic_load_n(&last_state, __ATOMIC_ACQUIRE);
}

/* The fix (or the imitation) on one module: 1 with *out the new code to use (the caller frees it). */
static int rewrite(vkb_errata_bitwise *s, int imitate, const uint32_t *code, size_t size, uint32_t **out, size_t *out_size)
{
    if (!code || size < 20) return 0;
    vkb_bitwise_stats st;
    const char *why;
    int r = vkb_spv_bitwise(code, size, imitate ? VKB_BITWISE_IMITATE : s->flags, out, out_size, &st, &why);
    if (imitate) {
        if (r == 1) __atomic_add_fetch(&s->imitated, 1, __ATOMIC_RELAXED);
        return r == 1;
    }
    __atomic_add_fetch(&s->seen, 1, __ATOMIC_RELAXED);
    uint64_t in = fnv1a(code, size);
    if (r < 0) {
        __atomic_add_fetch(&s->failed, 1, __ATOMIC_RELAXED);
        VKB_WARN("errata: bitwise_scalar: module h-%016llx left as it is (%s); Mali may miscompile it", (unsigned long long)in, why);
        return 0;
    }
    if (r == 0) return 0;
    __atomic_add_fetch(&s->rewritten, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&s->ops, st.ops, __ATOMIC_RELAXED);
    uint64_t oh = fnv1a(*out, *out_size);
    if (origin_add(oh, in)) {
        unsigned long n = __atomic_fetch_add(&s->logged, 1, __ATOMIC_RELAXED);
        if (n < LOG_INFO_MAX || vkb_log_level() >= VKB_LOG_DEBUG)
            vkb_log(n < LOG_INFO_MAX ? VKB_LOG_INFO : VKB_LOG_DEBUG,
                    "errata: bitwise_scalar: module h-%016llx: %u op(s) scalarised (and/or/xor %u, not %u, shift %u; operand: "
                    "shuffle %u, construct %u, constant %u, cascade %u%s), %zu -> %zu bytes, driver code h-%016llx%s",
                    (unsigned long long)in, st.ops, st.and_or_xor, st.not_, st.shifts, st.by_shuffle, st.by_construct, st.by_constant,
                    st.by_cascade, st.by_all ? ", VKBRIDGE_BITWISE_ALL" : "", size, *out_size, (unsigned long long)oh,
                    n == LOG_INFO_MAX - 1 ? " (further modules at debug level)" : "");
    }
    return 1;
}

static VkResult module_call(int imitate, VkDevice d, const VkShaderModuleCreateInfo *ci, const VkAllocationCallbacks *a, VkShaderModule *out)
{
    vkb_errata_bitwise *s = state();
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

static unsigned stages_rewrite(vkb_errata_bitwise *s, int imitate, uint32_t n, const VkPipelineShaderStageCreateInfo *st, inline_repl *r,
                               unsigned nr)
{
    for (uint32_t i = 0; i < n; i++)
        for (const VkBaseInStructure *b = st[i].pNext; b; b = b->pNext) {
            if (b->sType != VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO) continue;
            VkShaderModuleCreateInfo *mi = (VkShaderModuleCreateInfo *)b;
            uint32_t *code;
            size_t size;
            if (nr == INLINE_MAX) {
                VKB_WARN("errata: bitwise_scalar: more than %d inline modules in one call; the rest left as they are", INLINE_MAX);
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
    vkb_errata_bitwise *s = state();
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
    vkb_errata_bitwise *s = state();
    inline_repl r[INLINE_MAX];
    unsigned nr = 0;
    for (uint32_t i = 0; i < n; i++) nr = stages_rewrite(s, imitate, 1, &ci[i].stage, r, nr);
    VkResult res = (imitate ? s->imi_comp : s->fix_comp)(d, c, n, ci, a, out);
    stages_restore(r, nr);
    return res;
}

static VKAPI_ATTR VkResult VKAPI_CALL bw_CreateShaderModule(VkDevice d, const VkShaderModuleCreateInfo *ci, const VkAllocationCallbacks *a,
                                                            VkShaderModule *out)
{
    return module_call(0, d, ci, a, out);
}

static VKAPI_ATTR VkResult VKAPI_CALL bwi_CreateShaderModule(VkDevice d, const VkShaderModuleCreateInfo *ci, const VkAllocationCallbacks *a,
                                                             VkShaderModule *out)
{
    return module_call(1, d, ci, a, out);
}

static VKAPI_ATTR VkResult VKAPI_CALL bw_CreateGraphicsPipelines(VkDevice d, VkPipelineCache c, uint32_t n, const VkGraphicsPipelineCreateInfo *ci,
                                                                 const VkAllocationCallbacks *a, VkPipeline *out)
{
    return gfx_call(0, d, c, n, ci, a, out);
}

static VKAPI_ATTR VkResult VKAPI_CALL bwi_CreateGraphicsPipelines(VkDevice d, VkPipelineCache c, uint32_t n, const VkGraphicsPipelineCreateInfo *ci,
                                                                  const VkAllocationCallbacks *a, VkPipeline *out)
{
    return gfx_call(1, d, c, n, ci, a, out);
}

static VKAPI_ATTR VkResult VKAPI_CALL bw_CreateComputePipelines(VkDevice d, VkPipelineCache c, uint32_t n, const VkComputePipelineCreateInfo *ci,
                                                                const VkAllocationCallbacks *a, VkPipeline *out)
{
    return comp_call(0, d, c, n, ci, a, out);
}

static VKAPI_ATTR VkResult VKAPI_CALL bwi_CreateComputePipelines(VkDevice d, VkPipelineCache c, uint32_t n, const VkComputePipelineCreateInfo *ci,
                                                                 const VkAllocationCallbacks *a, VkPipeline *out)
{
    return comp_call(1, d, c, n, ci, a, out);
}

static vkb_errata_bitwise *get_state(vkb_errata_dev *e)
{
    if (!e->bitwise) {
        e->bitwise = calloc(1, sizeof(*e->bitwise));
        if (!e->bitwise) return NULL;
        const char *all = vkb_srv_env("VKBRIDGE_BITWISE_ALL");
        if (all && *all && strcmp(all, "0")) {
            e->bitwise->flags |= VKB_BITWISE_ALL;
            VKB_INFO("errata: bitwise_scalar rewrites every integer vector bitwise op (VKBRIDGE_BITWISE_ALL)");
        }
    }
    return e->bitwise;
}

void vkb_errata_bitwise_install(vkb_srv_table *t, vkb_errata_dev *e)
{
    vkb_errata_bitwise *s = get_state(e);
    if (!s || !t->real.vkCreateShaderModule || !t->real.vkCreateGraphicsPipelines || !t->real.vkCreateComputePipelines) return;
    s->fix_mod = t->real.vkCreateShaderModule;
    s->fix_gfx = t->real.vkCreateGraphicsPipelines;
    s->fix_comp = t->real.vkCreateComputePipelines;
    if (!s->imi_mod) s->imi_mod = s->fix_mod, s->imi_gfx = s->fix_gfx, s->imi_comp = s->fix_comp;
    ERRATA_HOOK(t, vkCreateShaderModule, bw_CreateShaderModule);
    ERRATA_HOOK(t, vkCreateGraphicsPipelines, bw_CreateGraphicsPipelines);
    ERRATA_HOOK(t, vkCreateComputePipelines, bw_CreateComputePipelines);
    if (!e->probe) __atomic_store_n(&last_state, s, __ATOMIC_RELEASE);
}

void vkb_errata_bitwise_imitate(vkb_srv_table *t, vkb_errata_dev *e)
{
    vkb_errata_bitwise *s = get_state(e);
    if (!s || !t->real.vkCreateShaderModule || !t->real.vkCreateGraphicsPipelines || !t->real.vkCreateComputePipelines) return;
    s->imi_mod = t->real.vkCreateShaderModule;
    s->imi_gfx = t->real.vkCreateGraphicsPipelines;
    s->imi_comp = t->real.vkCreateComputePipelines;
    if (!s->fix_mod) s->fix_mod = s->imi_mod, s->fix_gfx = s->imi_gfx, s->fix_comp = s->imi_comp;
    ERRATA_HOOK(t, vkCreateShaderModule, bwi_CreateShaderModule);
    ERRATA_HOOK(t, vkCreateGraphicsPipelines, bwi_CreateGraphicsPipelines);
    ERRATA_HOOK(t, vkCreateComputePipelines, bwi_CreateComputePipelines);
    if (!e->probe) __atomic_store_n(&last_state, s, __ATOMIC_RELEASE);
}

void vkb_errata_bitwise_destroy(vkb_srv_table *t, vkb_errata_dev *e)
{
    vkb_errata_bitwise *s = e->bitwise;
    if (!s) return;
    if (e->on >> VKB_ERRATUM_BITWISE_SCALAR & 1)
        VKB_INFO("errata: bitwise_scalar: device %u: %lu of %lu module(s) rewritten (%lu op(s) scalarised), %lu left as they were",
                 t->id, s->rewritten, s->seen, s->ops, s->failed);
    if ((e->imitated >> VKB_ERRATUM_BITWISE_SCALAR & 1) && !e->probe)
        VKB_INFO("errata: bitwise_scalar: device %u: the imitation broke %lu module(s)", t->id, s->imitated);
    e->bitwise = NULL;
    /* the last one made stays: a server thread that finds no table falls back to it */
    if (e->probe || __atomic_load_n(&last_state, __ATOMIC_ACQUIRE) != s) free(s);
}
