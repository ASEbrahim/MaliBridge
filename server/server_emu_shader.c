/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * Shader-side emulations, done when a graphics pipeline is created (both ends of the vertex ->
 * fragment interface are known then):
 *
 *  clip distance (shaderClipDistance missing): every stage's ClipDistance - loose or in a
 *    gl_PerVertex block, output or gl_in[] input, capability declared or not (spirv_clip.c) -
 *    becomes an ordinary varying at one free location; the fragment shader reads the last
 *    pre-rasterization stage's and discards the fragment when any distance is negative. For
 *    planar clip distances this is the same set of pixels the hardware clipper would keep.
 *  cull distance (shaderCullDistance missing): the output becomes an unread varying - nothing is
 *    culled, which costs fragments but never drops geometry that should be visible.
 *  point size in geometry/tessellation (shaderTessellationAndGeometryPointSize missing): same.
 *
 * The per-vertex fold (server_spvfix.c) sits above this layer, so geometry/tessellation modules
 * arrive here with their clip/cull distances already in blocks; both forms are handled.
 *
 * Shader modules are kept as SPIR-V so a pipeline can build rewritten variants; the driver's own
 * module is created from a variant that needs none of the missing features.
 */
#define _GNU_SOURCE
#include "server_emu.h"
#include "spirv_clip.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* VKBRIDGE_CLIP_DISCARD=0 (a measurement switch): the clip distances still become ordinary
 * varyings, but no fragment shader discards on them, so nothing is clipped. A discard in a fragment
 * shader costs Mali its early depth/stencil update and Forward Pixel Kill; this shows what the
 * emulation's discard costs a game whose every world shader writes ClipDistance (Dark Souls III). */
static int clip_discard_wanted(void)
{
    const char *v = vkb_srv_env("VKBRIDGE_CLIP_DISCARD");
    if (v && !strcmp(v, "0")) {
        VKB_ONCE(VKB_LOG_WARN, "emulation: clip distances not applied (VKBRIDGE_CLIP_DISCARD=0, a measurement switch)");
        return 0;
    }
    return 1;
}


typedef struct shader_rec {
    VkShaderModule module;
    uint32_t *code;
    size_t size;
    struct shader_rec *next;
} shader_rec;

typedef struct shader_state {
    pthread_mutex_t lock;
    shader_rec *mods[1024];
} shader_state;

#define SHADER_EMU (VKB_EMU_CLIP_DISTANCE | VKB_EMU_CULL_DISTANCE | VKB_EMU_POINT_SIZE)
/* Emulations that need the shader modules' SPIR-V kept. */
#define SHADER_STORE (SHADER_EMU | VKB_EMU_DIVISOR | VKB_EMU_DUAL_SRC | VKB_EMU_BGR10_VTX | VKB_EMU_UF11_VTX)

static unsigned mh(VkShaderModule m)
{
    uint64_t k = (uint64_t)m;
    k ^= k >> 31;
    k *= 0x94d049bb133111ebull;
    return (unsigned)(k >> 54);
}

static shader_state *state(vkb_emu_device *e) { return e->shaders; }

static const shader_rec *find_module(vkb_emu_device *e, VkShaderModule m)
{
    shader_state *s = state(e);
    pthread_mutex_lock(&s->lock);
    shader_rec *r = s->mods[mh(m)];
    while (r && r->module != m) r = r->next;
    pthread_mutex_unlock(&s->lock);
    return r;
}

/* ------------------------------------------------------------------ rewrites */

/* Finds a standalone Output/Input variable decorated with a builtin; 0 if none. *in_block: a block member has it. */
static uint32_t builtin_var(const spv_mod *m, uint32_t builtin, uint32_t storage, int *in_block)
{
    *in_block = 0;
    for (uint32_t i = 0; i < m->count; i++) {
        const spv_inst *in = &m->ins[i];
        if (in->op == SpvOpDecorate && in->w[1] == SpvDecorationBuiltIn && in->w[2] == builtin) {
            int d = spv_find_def(m, in->w[0]);
            if (d >= 0 && m->ins[d].op == SpvOpVariable && m->ins[d].w[2] == storage) return in->w[0];
        }
        if (in->op == SpvOpMemberDecorate && in->w[2] == SpvDecorationBuiltIn && in->w[3] == builtin) *in_block = 1;
    }
    return 0;
}

/* The emulations a module needs, as spirv_clip.h's VKB_SPV_* bits. */
static unsigned clip_what(uint32_t flags)
{
    return (flags & VKB_EMU_CLIP_DISTANCE ? VKB_SPV_CLIP : 0) | (flags & VKB_EMU_CULL_DISTANCE ? VKB_SPV_CULL : 0) |
           (flags & VKB_EMU_POINT_SIZE ? VKB_SPV_POINT_SIZE : 0);
}

static void clip_left(const vkb_spv_clip_info *info)
{
    VKB_ONCE(VKB_LOG_WARN, "emulation: a clip/cull distance built-in could not be demoted (%s; logged once)", info->why);
}

/* ------------------------------------------------------------------ module store */

/* Evidence mode: when <cache dir>/dump-spirv exists, every module the driver gets is saved there
 * and vkCreateGraphicsPipelines logs its stages' modules (tablet run 8: Mali's compiler crashed
 * inside that call, and what it had been given was gone with the process). */
int vkb_emu_dumping(void)
{
    static int dumping = -1;
    if (dumping < 0) {
        const char *dir = vkb_pcache_dir();
        char path[600];
        snprintf(path, sizeof(path), "%s/dump-spirv", dir ? dir : "");
        dumping = dir && access(path, F_OK) == 0;
        if (dumping) VKB_INFO("SPIR-V evidence mode: driver modules saved under %s", path);
    }
    return dumping;
}

/* VKBRIDGE_SPIRV_SUBST=1 at this end too: the driver's module (after every rewrite above) is replaced by
 * <pipeline cache dir>/spv-subst/<16 hex digits>.spv named by the FNV-1a hash of the code as the profiler's
 * dump-spirv/g-<hash>.spv names it, so a dumped module can be edited and put back without a rebuild. */
static uint32_t *subst_driver_code(const uint32_t *code, size_t size, size_t *out_size)
{
    const char *on = vkb_srv_env("VKBRIDGE_SPIRV_SUBST"), *dir = vkb_pcache_dir();
    if (!on || strcmp(on, "1") != 0 || !dir || !code || size < 20) return NULL;
    uint64_t h = 1469598103934665603ull;
    const uint8_t *b = (const uint8_t *)code;
    for (size_t i = 0; i < size; i++) h = (h ^ b[i]) * 1099511628211ull;
    char path[700];
    snprintf(path, sizeof(path), "%s/spv-subst/%016llx.spv", dir, (unsigned long long)h);
    FILE *in = fopen(path, "rb");
    if (!in) return NULL;
    uint32_t *out = NULL;
    long n = fseek(in, 0, SEEK_END) == 0 ? ftell(in) : -1;
    if (n >= 20 && n < (64l << 20) && !(n & 3) && fseek(in, 0, SEEK_SET) == 0 && (out = malloc((size_t)n)) &&
        fread(out, 1, (size_t)n, in) == (size_t)n && out[0] == 0x07230203u) {
        *out_size = (size_t)n;
        VKB_INFO("SPIR-V subst (driver module): %016llx (%zu bytes) replaced by spv-subst/%016llx.spv (%ld bytes)",
                 (unsigned long long)h, size, (unsigned long long)h, n);
    } else {
        free(out);
        out = NULL;
    }
    fclose(in);
    return out;
}

VkResult vkb_emu_driver_module(VkDevice device, const VkShaderModuleCreateInfo *ci, VkShaderModule *out)
{
    VkShaderModuleCreateInfo sci = *ci;
    size_t subsz = 0;
    uint32_t *sub = subst_driver_code(ci->pCode, ci->codeSize, &subsz);
    if (sub) {
        sci.pCode = sub;
        sci.codeSize = subsz;
        ci = &sci;
    }
    VkResult r = vkb_emu_real()->vkCreateShaderModule(device, ci, NULL, out);
    free(sub);
    if (r == VK_SUCCESS && vkb_emu_dumping()) {
        static int seq;
        char path[600];
        snprintf(path, sizeof(path), "%s/dump-spirv/%d-%llx.spv", vkb_pcache_dir(), __atomic_fetch_add(&seq, 1, __ATOMIC_RELAXED),
                 (unsigned long long)(uintptr_t)*out);
        FILE *f = fopen(path, "wb");
        if (f) {
            fwrite(ci->pCode, 1, ci->codeSize, f);
            fclose(f);
        }
        VKB_INFO("driver module %llx (%zu bytes) saved as %s", (unsigned long long)(uintptr_t)*out, ci->codeSize, path);
    }
    return r;
}

static VkResult make_module(VkDevice device, const spv_mod *m, VkShaderModule *out)
{
    uint32_t *code;
    size_t size = spv_write(m, &code);
    if (!size) return VK_ERROR_OUT_OF_HOST_MEMORY;
    const char *dump = vkb_srv_env_global("VKBRIDGE_DUMP_SPIRV");
    if (dump && *dump) {
        static int seq;
        char path[512];
        snprintf(path, sizeof(path), "%s/vkbridge-%d-%d.spv", dump, (int)getpid(), __atomic_fetch_add(&seq, 1, __ATOMIC_RELAXED));
        FILE *f = fopen(path, "wb");
        if (f) {
            fwrite(code, 1, size, f);
            fclose(f);
        }
    }
    VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, size, code};
    VkResult r = vkb_emu_driver_module(device, &ci, out);
    free(code);
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL emu_CreateShaderModule(VkDevice device, const VkShaderModuleCreateInfo *ci,
                                                            const VkAllocationCallbacks *pAllocator, VkShaderModule *pModule)
{
    vkb_emu_device *e = vkb_emu_cur();
    spv_mod m;
    VkResult r;
    int needs = 0;
    if (spv_parse(&m, ci->pCode, ci->codeSize) == 0) needs = vkb_spv_clip_needed(&m, clip_what(e->flags));
    if (needs) {
        /* The driver's module must not ask for what it lacks; pipelines use rewritten variants. */
        vkb_spv_clip_info info;
        if (vkb_spv_clip_demote(&m, clip_what(e->flags), (uint32_t)(vkb_spv_clip_max_location(&m) + 1), NULL, &info) < 0)
            clip_left(&info);
        r = make_module(device, &m, pModule);
    } else {
        r = vkb_emu_driver_module(device, ci, pModule);
    }
    spv_free(&m);
    if (r != VK_SUCCESS) return r;
    shader_rec *rec = calloc(1, sizeof(*rec));
    rec->module = *pModule;
    rec->size = ci->codeSize;
    rec->code = malloc(ci->codeSize);
    memcpy(rec->code, ci->pCode, ci->codeSize);
    shader_state *s = state(e);
    pthread_mutex_lock(&s->lock);
    unsigned h = mh(*pModule);
    rec->next = s->mods[h];
    s->mods[h] = rec;
    pthread_mutex_unlock(&s->lock);
    return r;
}

static VKAPI_ATTR void VKAPI_CALL emu_DestroyShaderModule(VkDevice device, VkShaderModule module, const VkAllocationCallbacks *pAllocator)
{
    vkb_emu_device *e = vkb_emu_cur();
    if (module) {
        shader_state *s = state(e);
        pthread_mutex_lock(&s->lock);
        shader_rec **pp = &s->mods[mh(module)];
        while (*pp && (*pp)->module != module) pp = &(*pp)->next;
        shader_rec *r = *pp;
        if (r) *pp = r->next;
        pthread_mutex_unlock(&s->lock);
        if (r) {
            free(r->code);
            free(r);
        }
    }
    vkb_emu_real()->vkDestroyShaderModule(device, module, pAllocator);
}

/* Stages rewritten during this vkCreateGraphicsPipelines call (set_stage_module): their new
 * module's SPIR-V, so a later rewrite of the same stage builds on it. Freed when the call ends
 * (vkb_emu_shader_call_end). */
#define REWRITTEN_MAX 64
static __thread struct {
    VkShaderModule module;
    uint32_t *code;
    size_t size;
} rewritten[REWRITTEN_MAX];
static __thread int nrewritten;

void vkb_emu_shader_call_end(void)
{
    for (int i = 0; i < nrewritten; i++) free(rewritten[i].code);
    nrewritten = 0;
}

/* The SPIR-V of a stage: its module's (kept here, or rewritten in this call) or the inline
 * VkShaderModuleCreateInfo's. */
static int stage_code(vkb_emu_device *e, const VkPipelineShaderStageCreateInfo *st, const uint32_t **code, size_t *size)
{
    for (int i = 0; st->module && i < nrewritten; i++)
        if (rewritten[i].module == st->module) {
            *code = rewritten[i].code;
            *size = rewritten[i].size;
            return 1;
        }
    if (st->module) {
        const shader_rec *r = find_module(e, st->module);
        if (!r) return 0;
        *code = r->code;
        *size = r->size;
        return 1;
    }
    for (const VkBaseInStructure *b = st->pNext; b; b = b->pNext) {
        if (b->sType == VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO) {
            *code = ((const VkShaderModuleCreateInfo *)b)->pCode;
            *size = ((const VkShaderModuleCreateInfo *)b)->codeSize;
            return 1;
        }
    }
    return 0;
}

/* Replace a stage's shader by a rewritten module (recorded for destruction after the call). */
static void set_stage_module(VkDevice device, VkPipelineShaderStageCreateInfo *st, const spv_mod *m, VkShaderModule *mods,
                             uint32_t *nmods, uint32_t cap)
{
    VkShaderModule mod;
    if (*nmods >= cap || make_module(device, m, &mod) != VK_SUCCESS) return;
    mods[(*nmods)++] = mod;
    st->module = mod;
    if (nrewritten < REWRITTEN_MAX) {
        uint32_t *code;
        size_t size = spv_write(m, &code);
        if (size) rewritten[nrewritten].module = mod, rewritten[nrewritten].code = code, rewritten[nrewritten++].size = size;
    }
    vkb_emu_chain_take(st, VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
}

/* Every stage's clip/cull built-ins become Location variables at one base past every location the
 * pipeline's stages use, so each stage's demoted inputs meet the previous stage's demoted outputs
 * (TCS/TES/GS gl_in[] included) and the fragment shader's discard reads the last one's. */
void vkb_emu_shader_pipeline(vkb_emu_device *e, VkDevice device, VkGraphicsPipelineCreateInfo *ci, VkShaderModule *mods,
                             uint32_t *nmods, uint32_t cap)
{
    if (!(e->flags & SHADER_EMU) || !ci->pStages) return;
    static const VkShaderStageFlagBits order[5] = {VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT,
                                                   VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT, VK_SHADER_STAGE_GEOMETRY_BIT,
                                                   VK_SHADER_STAGE_FRAGMENT_BIT};
    unsigned what = clip_what(e->flags);
    VkPipelineShaderStageCreateInfo *st[5] = {0};
    spv_mod sm[5];
    int have[5] = {0}, needs = 0, loc = -1;
    for (uint32_t s = 0; s < ci->stageCount; s++)
        for (int k = 0; k < 5; k++)
            if (ci->pStages[s].stage == order[k]) st[k] = (VkPipelineShaderStageCreateInfo *)&ci->pStages[s];
    for (int k = 0; k < 5; k++) {
        const uint32_t *code;
        size_t size;
        if (!st[k] || !stage_code(e, st[k], &code, &size)) continue;
        if (spv_parse(&sm[k], code, size) != 0) {
            spv_free(&sm[k]);
            continue;
        }
        have[k] = 1;
        needs |= vkb_spv_clip_needed(&sm[k], what);
    }
    for (int k = 0; needs && k < 5; k++) {
        int l = have[k] ? vkb_spv_clip_max_location(&sm[k]) : -1;
        if (l > loc) loc = l;
    }
    if (needs) {
        uint32_t base = (uint32_t)(loc + 1), prev[3] = {0}, clip = 0;
        int approx = 0, first = 1;
        for (int k = 0; k < 5; k++) {
            if (!have[k]) continue;
            vkb_spv_clip_info info;
            memset(&info, 0, sizeof(info));
            int changed = vkb_spv_clip_needed(&sm[k], what);
            /* A partial rewrite still goes in: it is no worse than the driver's module, and its
             * locations agree with the other stages'. */
            if (changed && vkb_spv_clip_demote(&sm[k], what, base, first ? NULL : prev, &info) < 0) clip_left(&info);
            for (int b = 0; b < 3; b++) approx |= info.in[b] && info.in[b] != prev[b];
            if (k < 4) {
                memcpy(prev, info.out, sizeof(prev));
                clip = info.out[0];
                first = 0;
            } else if (clip && clip_discard_wanted() && vkb_spv_clip_discard(&sm[k], base, clip, info.clip_in_var) == 0) {
                changed = 1;
            }
            if (changed) set_stage_module(device, st[k], &sm[k], mods, nmods, cap);
        }
        if (clip && !have[4])
            VKB_ONCE(VKB_LOG_WARN, "emulation: clip distances in a pipeline without a fragment shader are not applied (logged once)");
        if (approx)
            VKB_ONCE(VKB_LOG_WARN, "emulation: clip/cull distance inputs differ from the previous stage's outputs; clipping approximated (logged once)");
        VKB_DBG("emulation: pipeline with %u clip distance(s) rewritten at location %u", clip, base);
    }
    for (int k = 0; k < 5; k++)
        if (have[k]) spv_free(&sm[k]);
}

/* ------------------------------------------------------------------ stream output fold */

/* DXVK's D3D11 stream output is a geometry shader that only passes a point through: one point in,
 * its inputs copied (component by component) to outputs decorated XfbBuffer/XfbStride/Offset, one
 * point out, rasterizer discard on. Mali runs geometry shaders slowly - Subnautica (2026-10-12):
 * Unity's GPU skinning, ~20 such draws and ~40k points a frame, took 75-105 ms of a 135 ms frame,
 * GPU 98 % busy at 1300 MHz, 6-8 fps; folded, 28-35 fps with the skinned hands drawn as before.
 * The vertex shader can capture the same values itself: its
 * outputs take the decorations of the GS outputs they feed and the GS stage goes. Anything but
 * that exact shape (streams, more vertices, arithmetic, built-ins, a rasterized stream) is left
 * alone. */

#define XFB_CAPS 64

typedef struct xfb_cap {
    uint32_t loc, comp;            /* the GS input (= VS output) location and component */
    uint32_t buf, stride, offset;  /* where the GS output went */
} xfb_cap;

static int const_u32(const spv_mod *m, uint32_t id, uint32_t *v)
{
    int d = spv_find_def(m, id);
    if (d < 0 || m->ins[d].op != SpvOpConstant || m->ins[d].n != 4) return 0;
    *v = m->ins[d].w[2];
    return 1;
}

/* Components of a 32-bit scalar or vector type; 0 for anything else. */
static uint32_t comps32(const spv_mod *m, uint32_t type)
{
    int d = spv_find_def(m, type);
    if (d < 0) return 0;
    const spv_inst *t = &m->ins[d];
    if ((t->op == SpvOpTypeFloat || t->op == SpvOpTypeInt) && t->w[1] == 32) return 1;
    if (t->op == SpvOpTypeVector && comps32(m, t->w[1]) == 1) return t->w[2];
    return 0;
}

/* The pointee type of a variable of this storage class, or 0. */
static uint32_t var_type(const spv_mod *m, uint32_t var, uint32_t storage)
{
    int d = spv_find_def(m, var);
    if (d < 0 || m->ins[d].op != SpvOpVariable || m->ins[d].w[2] != storage) return 0;
    int p = spv_find_def(m, m->ins[d].w[0]);
    return p >= 0 && m->ins[p].op == SpvOpTypePointer ? m->ins[p].w[2] : 0;
}

/* The captures of a pass-through stream output GS; -1 when it is anything else. */
static int xfb_gs_captures(const spv_mod *g, xfb_cap *cap)
{
    if (spv_execution_model(g) != SpvExecutionModelGeometry || spv_has_capability(g, 54 /* GeometryStreams */)) return -1;
    int xfb = 0, in_points = 0, out_points = 0, verts = 0;
    for (uint32_t i = 0; i < g->count; i++) {
        const spv_inst *in = &g->ins[i];
        if (in->op == SpvOpEntryPoint && i != (uint32_t)spv_find_op(g, SpvOpEntryPoint, 0)) return -1;
        if (in->op != SpvOpExecutionMode) continue;
        switch (in->w[1]) {
        case 11: xfb = 1; break;                                  /* Xfb */
        case 19: in_points = 1; break;                            /* InputPoints */
        case 27: out_points = 1; break;                           /* OutputPoints */
        case 26: verts = in->n > 3 ? (int)in->w[2] : 0; break;    /* OutputVertices */
        case 0: if (in->n <= 3 || in->w[2] != 1) return -1; break; /* Invocations */
        default: return -1;
        }
    }
    if (!xfb || !in_points || !out_points || verts != 1) return -1;
    /* r = value id -> (input location, first component, count) */
    typedef struct { uint32_t id, loc, comp, n; } val;
    val vals[XFB_CAPS];
    int nvals = 0, ncap = 0, emits = 0, funcs = 0;
    for (uint32_t i = (uint32_t)spv_types_end(g); i < g->count; i++) {
        const spv_inst *in = &g->ins[i];
        switch (in->op) {
        case SpvOpFunction: if (++funcs > 1) return -1; break;
        case SpvOpLabel: case SpvOpFunctionEnd: case SpvOpReturn: case SpvOpLine: case SpvOpNoLine:
        case SpvOpAccessChain: case SpvOpInBoundsAccessChain: break; /* chains are read through their loads */
        case 218: emits++; break;                                     /* OpEmitVertex */
        case 219: break;                                              /* OpEndPrimitive */
        case SpvOpLoad: {
            /* %r = OpLoad %t %chain; %chain = AccessChain %input %vertex0 [%k] */
            int c = spv_find_def(g, in->w[2]);
            if (c < 0 || (g->ins[c].op != SpvOpAccessChain && g->ins[c].op != SpvOpInBoundsAccessChain)) return -1;
            const spv_inst *ch = &g->ins[c];
            uint32_t var = ch->w[2], v0, k = 0, nidx = ch->n - 4u;
            uint32_t at = var_type(g, var, SpvStorageClassInput);
            int ad = at ? spv_find_def(g, at) : -1;
            if (ad < 0 || g->ins[ad].op != SpvOpTypeArray || nidx < 1 || nidx > 2 || !const_u32(g, ch->w[3], &v0) || v0 != 0) return -1;
            uint32_t elem = g->ins[ad].w[1], ne = comps32(g, elem);
            int64_t loc = spv_get_decoration(g, var, SpvDecorationLocation), cb = spv_get_decoration(g, var, SpvDecorationComponent);
            if (!ne || loc < 0 || spv_get_decoration(g, var, SpvDecorationBuiltIn) >= 0) return -1;
            if (nidx == 2 && (!const_u32(g, ch->w[4], &k) || k >= ne)) return -1;
            if (nvals >= XFB_CAPS) return -1;
            vals[nvals++] = (val){in->w[1], (uint32_t)loc, (uint32_t)(cb < 0 ? 0 : cb) + k, nidx == 2 ? 1 : ne};
            break;
        }
        case SpvOpStore: {
            uint32_t ot = var_type(g, in->w[0], SpvStorageClassOutput), n = ot ? comps32(g, ot) : 0;
            int64_t buf = spv_get_decoration(g, in->w[0], 36 /* XfbBuffer */), stride = spv_get_decoration(g, in->w[0], 37 /* XfbStride */),
                    off = spv_get_decoration(g, in->w[0], 35 /* Offset */), stream = spv_get_decoration(g, in->w[0], 29 /* Stream */);
            if (!n || (stream > 0)) return -1;
            if (buf < 0 || stride < 0 || off < 0) break; /* not captured; nothing is rasterized either */
            int v = -1;
            for (int j = 0; j < nvals; j++)
                if (vals[j].id == in->w[1]) v = j;
            if (v < 0 || vals[v].n != n || ncap + (int)n > XFB_CAPS) return -1;
            for (uint32_t j = 0; j < n; j++)
                cap[ncap++] = (xfb_cap){vals[v].loc, vals[v].comp + j, (uint32_t)buf, (uint32_t)stride, (uint32_t)off + 4 * j};
            break;
        }
        default: return -1;
        }
    }
    return emits == 1 && ncap ? ncap : -1;
}

/* Gives the VS outputs the captures' decorations; 0 when every capture found its output. */
static int xfb_decorate_vs(spv_mod *v, const xfb_cap *cap, int ncap)
{
    if (spv_execution_model(v) != SpvExecutionModelVertex || spv_has_capability(v, 53 /* TransformFeedback */)) return -1;
    int found = 0;
    uint32_t adds[XFB_CAPS][3], nadd = 0;
    for (uint32_t i = 0; i < v->count; i++) {
        const spv_inst *in = &v->ins[i];
        if (in->op != SpvOpVariable || in->w[2] != SpvStorageClassOutput) continue;
        uint32_t var = in->w[1], t = var_type(v, var, SpvStorageClassOutput), n = t ? comps32(v, t) : 0;
        int64_t loc = spv_get_decoration(v, var, SpvDecorationLocation), cb = spv_get_decoration(v, var, SpvDecorationComponent);
        if (!n || loc < 0) continue;
        uint32_t c0 = (uint32_t)(cb < 0 ? 0 : cb);
        int first = -1, hits = 0;
        for (uint32_t j = 0; j < n; j++)
            for (int k = 0; k < ncap; k++) {
                if (cap[k].loc != (uint32_t)loc || cap[k].comp != c0 + j) continue;
                if (first < 0 && j == 0) first = k;
                if (first < 0) return -1; /* a captured component without the first one */
                if (cap[k].buf != cap[first].buf || cap[k].stride != cap[first].stride || cap[k].offset != cap[first].offset + 4 * j)
                    return -1;
                hits++;
            }
        if (!hits) continue;
        if (hits != (int)n || nadd + 3 > XFB_CAPS) return -1; /* part of a vector captured: not expressible */
        adds[nadd][0] = var, adds[nadd][1] = 36, adds[nadd++][2] = cap[first].buf;
        adds[nadd][0] = var, adds[nadd][1] = 37, adds[nadd++][2] = cap[first].stride;
        adds[nadd][0] = var, adds[nadd][1] = 35, adds[nadd++][2] = cap[first].offset;
        found += hits;
    }
    if (found != ncap) return -1;
    for (uint32_t i = 0; i < nadd; i++) spv_insert(v, spv_annotations_end(v), SpvOpDecorate, adds[i], 3);
    spv_add_capability(v, 53);
    int ep = spv_find_op(v, SpvOpEntryPoint, 0);
    uint32_t mode[2] = {v->ins[ep].w[1], 11 /* Xfb */};
    int at = spv_find_op(v, SpvOpExecutionMode, 0);
    spv_insert(v, at >= 0 ? (uint32_t)at : (uint32_t)ep + 1, SpvOpExecutionMode, mode, 2);
    return 0;
}

void vkb_emu_shader_xfb_fold(vkb_emu_device *e, VkDevice device, VkGraphicsPipelineCreateInfo *ci, VkShaderModule *mods, uint32_t *nmods,
                             uint32_t cap)
{
    if (!e->shaders || !ci->pStages || ci->stageCount < 2) return;
    const char *off = vkb_srv_env("VKBRIDGE_NO_XFB_FOLD");
    if (off && *off && strcmp(off, "0")) return;
    int vs = -1, gs = -1, other = 0;
    for (uint32_t s = 0; s < ci->stageCount; s++) {
        VkShaderStageFlagBits st = ci->pStages[s].stage;
        if (st == VK_SHADER_STAGE_VERTEX_BIT) vs = (int)s;
        else if (st == VK_SHADER_STAGE_GEOMETRY_BIT) gs = (int)s;
        else other = 1;
    }
    /* Only a stream nothing rasterizes: rasterizer discard fixed on (without it the GS's points,
     * not the VS's, would be drawn). */
    const VkPipelineRasterizationStateCreateInfo *rs = ci->pRasterizationState;
    int discard = rs && rs->rasterizerDiscardEnable;
    if (discard && ci->pDynamicState)
        for (uint32_t i = 0; i < ci->pDynamicState->dynamicStateCount; i++)
            if (ci->pDynamicState->pDynamicStates[i] == VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE) discard = 0;
    if (vs < 0 || gs < 0 || !discard) return;
    if (other) {
        for (uint32_t s = 0; s < ci->stageCount; s++)
            if (ci->pStages[s].stage != VK_SHADER_STAGE_VERTEX_BIT && ci->pStages[s].stage != VK_SHADER_STAGE_GEOMETRY_BIT &&
                ci->pStages[s].stage != VK_SHADER_STAGE_FRAGMENT_BIT)
                return;
    }
    const uint32_t *gcode, *vcode;
    size_t gsize, vsize;
    VkPipelineShaderStageCreateInfo *st = (VkPipelineShaderStageCreateInfo *)ci->pStages;
    if (!stage_code(e, &st[gs], &gcode, &gsize) || !stage_code(e, &st[vs], &vcode, &vsize)) return;
    spv_mod g, v;
    xfb_cap caps[XFB_CAPS];
    int ncap = -1, ok = 0;
    if (spv_parse(&g, gcode, gsize) == 0) ncap = xfb_gs_captures(&g, caps);
    spv_free(&g);
    if (ncap <= 0) return;
    if (spv_parse(&v, vcode, vsize) == 0 && !vkb_spv_clip_needed(&v, clip_what(e->flags)) && xfb_decorate_vs(&v, caps, ncap) == 0) {
        uint32_t before = *nmods;
        set_stage_module(device, &st[vs], &v, mods, nmods, cap);
        ok = *nmods > before;
    }
    spv_free(&v);
    if (!ok) {
        VKB_ONCE(VKB_LOG_INFO, "emulation: a stream output geometry shader could not be folded into its vertex shader (logged once)");
        return;
    }
    /* The GS goes, and with discard on the fragment shader too: nothing reaches it, and its inputs
     * need not match the vertex shader's outputs. */
    uint32_t keep = 0;
    for (uint32_t s = 0; s < ci->stageCount; s++)
        if (st[s].stage == VK_SHADER_STAGE_VERTEX_BIT) st[keep++] = st[s];
    ci->stageCount = keep;
    VKB_ONCE(VKB_LOG_INFO, "emulation: stream output geometry shaders folded into their vertex shaders (%d components; logged once)", ncap);
}

/* A vertex shader that reads BaseInstance as 0 (divisor emulation splits draws, which moves
 * BaseInstance; DXVK derives SV_InstanceID from it). VK_NULL_HANDLE when the shader never reads it. */
VkShaderModule vkb_emu_shader_zero_base_instance(vkb_emu_device *e, VkDevice device, const VkPipelineShaderStageCreateInfo *vs)
{
    const uint32_t *code;
    size_t size;
    if (!e->shaders || !stage_code(e, vs, &code, &size)) return VK_NULL_HANDLE;
    spv_mod m;
    if (spv_parse(&m, code, size) != 0) return VK_NULL_HANDLE;
    int in_block;
    uint32_t var = builtin_var(&m, SpvBuiltInBaseInstance, SpvStorageClassInput, &in_block);
    VkShaderModule out = VK_NULL_HANDLE;
    if (var) {
        int d = spv_find_def(&m, var);
        int p = d >= 0 ? spv_find_def(&m, m.ins[d].w[0]) : -1;
        uint32_t type = p >= 0 ? m.ins[p].w[2] : 0;
        int td = spv_find_def(&m, type);
        uint32_t zero = 0;
        if (td >= 0 && m.ins[td].op == SpvOpTypeInt) {
            zero = spv_new_id(&m);
            uint32_t ops[3] = {type, zero, 0};
            spv_insert(&m, spv_types_end(&m), SpvOpConstant, ops, 3);
        }
        int changed = 0;
        for (uint32_t i = 0; zero && i < m.count; i++) {
            spv_inst *in = &m.ins[i];
            if (in->op == SpvOpLoad && in->w[2] == var) {
                /* %r = OpLoad %int %BaseInstance  ->  %r = OpCopyObject %int %zero */
                in->op = 83; /* OpCopyObject */
                in->w[2] = zero;
                in->n = 4;
                changed = 1;
            }
        }
        if (changed) make_module(device, &m, &out);
    }
    spv_free(&m);
    return out;
}

/* Dual-source blending without the feature (server_emu_compat.c): the fragment shader's second
 * source - an output decorated Index 1 - moves to a location no attachment takes, so it is
 * written nowhere and the driver does not see Index. */
void vkb_emu_shader_drop_second_output(vkb_emu_device *e, VkDevice device, VkGraphicsPipelineCreateInfo *ci, VkShaderModule *mods,
                                       uint32_t *nmods, uint32_t cap)
{
    if (!e->shaders || !ci->pStages) return;
    for (uint32_t s = 0; s < ci->stageCount; s++) {
        VkPipelineShaderStageCreateInfo *st = (VkPipelineShaderStageCreateInfo *)&ci->pStages[s];
        if (st->stage != VK_SHADER_STAGE_FRAGMENT_BIT) continue;
        const uint32_t *code;
        size_t size;
        if (!stage_code(e, st, &code, &size)) return;
        spv_mod m;
        if (spv_parse(&m, code, size) != 0) return;
        int changed = 0;
        for (uint32_t i = 0; i < m.count; i++) {
            spv_inst *in = &m.ins[i];
            if (in->op != SpvOpDecorate || in->n < 4 || in->w[1] != 32 /* Index */ || in->w[2] == 0) continue;
            uint32_t var = in->w[0];
            spv_remove(&m, i);
            i--;
            int loc = spv_max_location(&m, SpvStorageClassOutput) + 1;
            if (loc < 1) loc = 1;
            if (loc > 7) loc = 7;
            spv_remove_decoration(&m, var, SpvDecorationLocation);
            uint32_t d[3] = {var, SpvDecorationLocation, (uint32_t)loc};
            spv_insert(&m, spv_annotations_end(&m), SpvOpDecorate, d, 3);
            changed = 1;
        }
        if (changed) set_stage_module(device, st, &m, mods, nmods, cap);
        spv_free(&m);
        return;
    }
}

/* ------------------------------------------------------------------ BGRA 10-10-10-2 vertex fetch */

/* VKB_EMU_BGR10_VTX (server_emu.c): an attribute in A2R10G10B10_SNORM/USCALED/SSCALED_PACK32, which
 * the driver cannot fetch, becomes its A2B10G10R10 twin - the same 32 bits read with red and blue the other
 * way round - and the vertex shader puts them back: each whole load of that location gets a zyx(w)
 * shuffle (the load's result is renamed and the shuffle takes its id, so every use stays), and an
 * access chain into it picks component 2 for 0 and 0 for 2. A read this does not handle (a variable
 * of fewer than three components, a dynamic index, the variable passed on whole) is logged once and
 * the attribute fetched swapped. */
#define SpvOpVectorShuffle 79

/* Whether operand word k of in can name a variable: the packed-format rewrites below look for every
 * use of an input variable by its id, and a literal equal to that id is none (OpTypeFloat 32 names
 * no variable %32; a test's shader had its colour input there, and both rewrites gave up). Debug
 * and decoration instructions, types and constants never name one; nor does OpExtInst's
 * instruction number. */
static int names_var(const spv_inst *in, uint32_t k)
{
    switch (in->op) {
    case 3: case 4: case 5: case 6: case 7: case 8: case 10: case 11: case 14: case 16: case 17: /* debug, mode, capability */
    case SpvOpDecorate: case 72: case 317: case 330: case 331:                                    /* decorations, OpNoLine, ... */
        return 0;
    default:
        break;
    }
    if (in->op >= 19 && in->op <= 39) return 0; /* types */
    if ((in->op >= 41 && in->op <= 46) || (in->op >= 48 && in->op <= 52)) return 0; /* constants, spec constants */
    if (in->op == SpvOpExtInst && k == 3) return 0;
    return 1;
}

void vkb_emu_shader_bgr10(vkb_emu_device *e, VkDevice device, VkGraphicsPipelineCreateInfo *ci, VkShaderModule *mods,
                          uint32_t *nmods, uint32_t cap)
{
    if (!(e->flags & VKB_EMU_BGR10_VTX) || !e->shaders || !ci->pVertexInputState || !ci->pStages) return;
    VkPipelineVertexInputStateCreateInfo *vi = (VkPipelineVertexInputStateCreateInfo *)ci->pVertexInputState;
    uint64_t locs = 0;
    for (uint32_t a = 0; a < vi->vertexAttributeDescriptionCount; a++) {
        VkVertexInputAttributeDescription *ad = (VkVertexInputAttributeDescription *)&vi->pVertexAttributeDescriptions[a];
        VkFormat rgba = vkb_bgr10_rgba(ad->format);
        if (rgba == VK_FORMAT_UNDEFINED || ad->location >= 64) continue;
        ad->format = rgba;
        locs |= 1ull << ad->location;
    }
    if (!locs) return;
    VkPipelineShaderStageCreateInfo *st = NULL;
    for (uint32_t i = 0; i < ci->stageCount; i++)
        if (ci->pStages[i].stage == VK_SHADER_STAGE_VERTEX_BIT) st = (VkPipelineShaderStageCreateInfo *)&ci->pStages[i];
    const uint32_t *code;
    size_t size;
    spv_mod m;
    if (!st || !stage_code(e, st, &code, &size) || spv_parse(&m, code, size) != 0) {
        VKB_ONCE(VKB_LOG_WARN, "emulation: BGRA 10-10-10-2 vertex input with a vertex shader not at hand: red and blue swapped (logged once)");
        return;
    }
    uint32_t vars[64];
    int nv = 0, bad = 0, changed = 0;
    /* made before the walk: a new constant goes into the types section and moves what follows */
    uint32_t c0 = spv_const_u32(&m, 0), c2 = spv_const_u32(&m, 2);
    for (uint32_t i = 0; i < m.count && nv < 64; i++) {
        const spv_inst *in = &m.ins[i];
        if (in->op != SpvOpDecorate || in->n < 4 || in->w[1] != SpvDecorationLocation || in->w[2] >= 64 || !(locs >> in->w[2] & 1)) continue;
        uint32_t var = in->w[0];
        int64_t comp = spv_get_decoration(&m, var, SpvDecorationComponent);
        if (var_type(&m, var, SpvStorageClassInput) && comp <= 0) vars[nv++] = var;
    }
    for (int v = 0; v < nv; v++) {
        uint32_t var = vars[v], n = comps32(&m, var_type(&m, var, SpvStorageClassInput));
        for (uint32_t i = 0; i < m.count; i++) {
            spv_inst *in = &m.ins[i];
            if (in->op == SpvOpLoad && in->n >= 4 && in->w[2] == var) {
                if (n < 3) {
                    bad = 1;
                    continue;
                }
                uint32_t type = in->w[0], old = in->w[1], renamed = spv_new_id(&m);
                in->w[1] = renamed;
                uint32_t ops[8] = {type, old, renamed, renamed, 2, 1, 0, 3};
                spv_insert(&m, i + 1, SpvOpVectorShuffle, ops, (uint16_t)(4 + n));
                i++;
                changed = 1;
            } else if ((in->op == SpvOpAccessChain || in->op == SpvOpInBoundsAccessChain) && in->n >= 5 && in->w[2] == var) {
                uint32_t idx;
                if (!const_u32(&m, in->w[3], &idx)) {
                    bad = 1;
                    continue;
                }
                if (idx == 0 || idx == 2) {
                    in->w[3] = idx == 0 ? c2 : c0;
                    changed = 1;
                }
            } else if (in->op != SpvOpDecorate && in->op != SpvOpName && in->op != SpvOpEntryPoint && in->op != SpvOpVariable) {
                for (uint32_t k = 0; k + 1 < in->n; k++)
                    if (in->w[k] == var && !(in->op == SpvOpLoad && k == 2) && names_var(in, k)) bad = 1;
            }
        }
    }
    if (bad) VKB_ONCE(VKB_LOG_WARN, "emulation: BGRA 10-10-10-2 vertex input read in a way not handled: red and blue may be swapped (logged once)");
    if (changed) set_stage_module(device, st, &m, mods, nmods, cap);
    VKB_ONCE(VKB_LOG_INFO, "emulation: BGRA 10-10-10-2 vertex input fetched as RGBA, red and blue swapped back in the vertex shader");
    spv_free(&m);
}

/* ------------------------------------------------------------------ B10G11R11 float vertex fetch */

/* VKB_EMU_UF11_VTX (server_emu.c): an attribute in B10G11R11_UFLOAT_PACK32, which the driver cannot
 * fetch, is read as R32_UINT and unpacked in the vertex shader. Each packed float is a half float
 * without its sign and its low mantissa bits (the same 5-bit exponent and bias, infinity and NaN
 * included): red and green shifted into place make one UnpackHalf2x16, blue another. The input
 * becomes a private variable of the type the shader gave it, filled at the start of the entry point
 * from a new uint input at the same location, so every read of it stays as it was. A shader this does
 * not handle (an input that is not 32-bit float, or used other than loaded or indexed) keeps the
 * format, which the driver then refuses as it did before (logged once). */
#define SpvOpShiftRightLogical 194
#define SpvOpShiftLeftLogical 196
#define SpvOpBitwiseOr 197
#define SpvOpBitwiseAnd 199
#define GLSLstd450UnpackHalf2x16 62

/* The interface ids of an OpEntryPoint start after its name string */
static uint32_t entry_iface_at(const spv_inst *ep)
{
    uint32_t k = 2;
    while (k + 1 < ep->n) {
        uint32_t w = ep->w[k++];
        if (!(w & 0xff000000u) || !(w & 0xff0000u) || !(w & 0xff00u) || !(w & 0xffu)) break;
    }
    return k;
}

/* Whether the input variable is read only by whole loads and access chains (whose results are
 * loaded), with a 32-bit float scalar or vector type: what the rewrite below handles. */
static int uf11_var_ok(const spv_mod *m, uint32_t var)
{
    uint32_t t = var_type(m, var, SpvStorageClassInput);
    int d = t ? spv_find_def(m, t) : -1;
    if (d < 0) return 0;
    const spv_inst *ty = &m->ins[d];
    if (ty->op == SpvOpTypeVector) {
        if (ty->w[2] > 4) return 0;
        d = spv_find_def(m, ty->w[1]);
        if (d < 0) return 0;
        ty = &m->ins[d];
    }
    if (ty->op != SpvOpTypeFloat || ty->w[1] != 32) return 0;
    for (uint32_t i = 0; i < m->count; i++) {
        const spv_inst *in = &m->ins[i];
        if (in->op == SpvOpDecorate || in->op == SpvOpName || in->op == SpvOpEntryPoint || in->op == SpvOpVariable) continue;
        int access = in->op == SpvOpAccessChain || in->op == SpvOpInBoundsAccessChain;
        for (uint32_t k = 0; k + 1 < in->n; k++)
            if (in->w[k] == var && !(in->op == SpvOpLoad && k == 2) && !(access && k == 2) && names_var(in, k)) return 0;
    }
    return 1;
}

static void uf11_rewrite(spv_mod *m, uint32_t var, uint32_t loc)
{
    uint32_t vt = var_type(m, var, SpvStorageClassInput), n = comps32(m, vt);
    uint32_t u32 = spv_type_int(m, 32, 0), f32 = spv_type_float(m, 32), v2 = spv_type_vector(m, f32, 2);
    uint32_t glsl = spv_glsl_std450(m);
    uint32_t c4 = spv_const_u32(m, 4), c5 = spv_const_u32(m, 5), c11 = spv_const_u32(m, 11), c20 = spv_const_u32(m, 20),
             c22 = spv_const_u32(m, 22), c7ff = spv_const_u32(m, 0x7ff), one = spv_const_f32(m, 1.0f);
    uint32_t priv = spv_type_pointer(m, SpvStorageClassPrivate, vt), in_ptr = spv_type_pointer(m, SpvStorageClassInput, u32);
    /* access chains into it point at Private now */
    for (uint32_t i = 0; i < m->count; i++) {
        spv_inst *in = &m->ins[i];
        if ((in->op != SpvOpAccessChain && in->op != SpvOpInBoundsAccessChain) || in->n < 4 || in->w[2] != var) continue;
        int p = spv_find_def(m, in->w[0]);
        if (p < 0 || m->ins[p].op != SpvOpTypePointer) continue;
        uint32_t result = in->w[1], pt = spv_type_pointer(m, SpvStorageClassPrivate, m->ins[p].w[2]);
        m->ins[spv_find_def(m, result)].w[0] = pt; /* the type's insert may have moved it */
    }
    /* declared again after its new pointer type, which went to the end of the globals */
    spv_remove(m, (uint32_t)spv_find_def(m, var));
    uint32_t pops[3] = {priv, var, SpvStorageClassPrivate};
    spv_insert(m, spv_types_end(m), SpvOpVariable, pops, 3);
    uint32_t input = spv_new_id(m), vops[3] = {in_ptr, input, SpvStorageClassInput};
    spv_insert(m, spv_types_end(m), SpvOpVariable, vops, 3);
    spv_remove_decoration(m, var, SpvDecorationLocation);
    uint32_t dec[3] = {input, SpvDecorationLocation, loc};
    spv_insert(m, spv_annotations_end(m), SpvOpDecorate, dec, 3);
    /* the entry point's interface: the new input in its place (the private one too from SPIR-V 1.4) */
    for (uint32_t i = 0; i < m->count; i++) {
        spv_inst *ep = &m->ins[i];
        if (ep->op != SpvOpEntryPoint) continue;
        for (uint32_t k = entry_iface_at(ep); k + 1 < ep->n; k++)
            if (ep->w[k] == var) ep->w[k] = input;
    }
    if (spv_version(m) >= 0x00010400) spv_add_interface(m, var);

    int ep = spv_find_op(m, SpvOpEntryPoint, 0);
    uint32_t fn = m->ins[ep].w[1];
    int fi = -1;
    for (uint32_t i = 0; i < m->count; i++)
        if (m->ins[i].op == SpvOpFunction && m->ins[i].w[1] == fn) fi = (int)i;
    int li = fi < 0 ? -1 : spv_find_op(m, SpvOpLabel, (uint32_t)fi);
    if (li < 0) {
        m->err = 1;
        return;
    }
    uint32_t at = (uint32_t)li + 1;
    while (at < m->count && (m->ins[at].op == SpvOpVariable || m->ins[at].op == SpvOpLine || m->ins[at].op == SpvOpNoLine)) at++;
#define EMIT(op, ...)                                                                    \
    do {                                                                                 \
        const uint32_t o_[] = {__VA_ARGS__};                                             \
        at = spv_insert(m, at, op, o_, (uint16_t)(sizeof(o_) / sizeof(o_[0]))) + 1;      \
    } while (0)
    uint32_t u = spv_new_id(m), r0 = spv_new_id(m), r = spv_new_id(m), g0 = spv_new_id(m), g1 = spv_new_id(m), g = spv_new_id(m),
             rg = spv_new_id(m), b0 = spv_new_id(m), b = spv_new_id(m), hrg = spv_new_id(m), hb = spv_new_id(m);
    EMIT(SpvOpLoad, u32, u, input);
    EMIT(SpvOpBitwiseAnd, u32, r0, u, c7ff);
    EMIT(SpvOpShiftLeftLogical, u32, r, r0, c4);         /* red: half bits 4-14 */
    EMIT(SpvOpShiftRightLogical, u32, g0, u, c11);
    EMIT(SpvOpBitwiseAnd, u32, g1, g0, c7ff);
    EMIT(SpvOpShiftLeftLogical, u32, g, g1, c20);        /* green: the high half's bits 4-14 */
    EMIT(SpvOpBitwiseOr, u32, rg, r, g);
    EMIT(SpvOpShiftRightLogical, u32, b0, u, c22);
    EMIT(SpvOpShiftLeftLogical, u32, b, b0, c5);         /* blue: half bits 5-14 */
    EMIT(SpvOpExtInst, v2, hrg, glsl, GLSLstd450UnpackHalf2x16, rg);
    EMIT(SpvOpExtInst, v2, hb, glsl, GLSLstd450UnpackHalf2x16, b);
    uint32_t c[4] = {spv_new_id(m), spv_new_id(m), spv_new_id(m), one};
    EMIT(SpvOpCompositeExtract, f32, c[0], hrg, 0);
    if (n >= 2) EMIT(SpvOpCompositeExtract, f32, c[1], hrg, 1);
    if (n >= 3) EMIT(SpvOpCompositeExtract, f32, c[2], hb, 0);
    uint32_t val = c[0];
    if (n >= 2) {
        uint32_t ops[6] = {vt, val = spv_new_id(m), c[0], c[1], c[2], c[3]};
        at = spv_insert(m, at, SpvOpCompositeConstruct, ops, (uint16_t)(2 + n)) + 1;
    }
    EMIT(SpvOpStore, var, val);
#undef EMIT
}

void vkb_emu_shader_uf11(vkb_emu_device *e, VkDevice device, VkGraphicsPipelineCreateInfo *ci, VkShaderModule *mods,
                         uint32_t *nmods, uint32_t cap)
{
    if (!(e->flags & VKB_EMU_UF11_VTX) || !e->shaders || !ci->pVertexInputState || !ci->pStages) return;
    VkPipelineVertexInputStateCreateInfo *vi = (VkPipelineVertexInputStateCreateInfo *)ci->pVertexInputState;
    uint64_t locs = 0;
    for (uint32_t a = 0; a < vi->vertexAttributeDescriptionCount; a++)
        if (vi->pVertexAttributeDescriptions[a].format == VK_FORMAT_B10G11R11_UFLOAT_PACK32 && vi->pVertexAttributeDescriptions[a].location < 64)
            locs |= 1ull << vi->pVertexAttributeDescriptions[a].location;
    if (!locs) return;
    VkPipelineShaderStageCreateInfo *st = NULL;
    for (uint32_t i = 0; i < ci->stageCount; i++)
        if (ci->pStages[i].stage == VK_SHADER_STAGE_VERTEX_BIT) st = (VkPipelineShaderStageCreateInfo *)&ci->pStages[i];
    const uint32_t *code;
    size_t size;
    spv_mod m;
    if (!st || !stage_code(e, st, &code, &size) || spv_parse(&m, code, size) != 0) {
        VKB_ONCE(VKB_LOG_WARN, "emulation: B10G11R11 float vertex input with a vertex shader not at hand: left to the driver (logged once)");
        return;
    }
    uint32_t vars[64], vlocs[64];
    int nv = 0, bad = 0;
    for (uint32_t i = 0; i < m.count && nv < 64; i++) {
        const spv_inst *in = &m.ins[i];
        if (in->op != SpvOpDecorate || in->n < 4 || in->w[1] != SpvDecorationLocation || in->w[2] >= 64 || !(locs >> in->w[2] & 1)) continue;
        uint32_t var = in->w[0];
        if (!var_type(&m, var, SpvStorageClassInput)) continue;
        if (spv_get_decoration(&m, var, SpvDecorationComponent) > 0 || !uf11_var_ok(&m, var)) bad = 1;
        vlocs[nv] = in->w[2];
        vars[nv++] = var;
    }
    if (!bad)
        for (int v = 0; v < nv && !m.err; v++) uf11_rewrite(&m, vars[v], vlocs[v]);
    if (bad || m.err) {
        VKB_ONCE(VKB_LOG_WARN, "emulation: B10G11R11 float vertex input read in a way not handled: left to the driver (logged once)");
        spv_free(&m);
        return;
    }
    /* the shader reads the attribute as a uint now; one it does not read is fetched as one all the same */
    for (uint32_t a = 0; a < vi->vertexAttributeDescriptionCount; a++) {
        VkVertexInputAttributeDescription *ad = (VkVertexInputAttributeDescription *)&vi->pVertexAttributeDescriptions[a];
        if (ad->format == VK_FORMAT_B10G11R11_UFLOAT_PACK32) ad->format = VK_FORMAT_R32_UINT;
    }
    if (nv) set_stage_module(device, st, &m, mods, nmods, cap);
    VKB_ONCE(VKB_LOG_INFO, "emulation: B10G11R11 float vertex input fetched as R32_UINT, unpacked in the vertex shader");
    spv_free(&m);
}

void vkb_emu_shader_install(vkb_srv_table *dev)
{
    vkb_emu_device *e = dev->emu;
    if (!(e->flags & SHADER_STORE)) return;
    shader_state *s = calloc(1, sizeof(*s));
    pthread_mutex_init(&s->lock, NULL);
    e->shaders = s;
    dev->dt.vkCreateShaderModule = emu_CreateShaderModule;
    dev->dt.vkDestroyShaderModule = emu_DestroyShaderModule;
}

void vkb_emu_shader_uninstall(vkb_srv_table *dev)
{
    shader_state *s = dev->emu->shaders;
    if (!s) return;
    for (int i = 0; i < 1024; i++) {
        while (s->mods[i]) {
            shader_rec *r = s->mods[i];
            s->mods[i] = r->next;
            free(r->code);
            free(r);
        }
    }
    pthread_mutex_destroy(&s->lock);
    free(s);
    dev->emu->shaders = NULL;
}
