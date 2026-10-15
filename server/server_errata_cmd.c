/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Driver errata on command-buffer state (server_errata.c decides which are on):
 *
 * dyn_reemit (MK-04; Arm errata 3809538, 3893905, 3893903): after vkCmdBindPipeline the next draw
 *   ignores dynamic state set before the bind (depth test/write/compare, stencil test/op/masks/
 *   reference, cull mode, vertex strides; on r48-r53 the EDS3 sample mask, line mode and
 *   conservative mode), or a pipeline with one of those three static picks up the dynamic value
 *   set before it was bound. The fix keeps, per command buffer, the last value the app set for
 *   each of those states, records each graphics pipeline's dynamic-state list at creation, and
 *   after every graphics vkCmdBindPipeline sends again the values that the pipeline declares
 *   dynamic and that this command buffer has set (vertex strides: the vertex buffer bindings that
 *   have one, one binding per call). Nothing else is sent: a pipeline without dynamic state costs
 *   a lookup. For 3893903, when the decision is not "always" alone (a probe, the driver's range,
 *   a forced switch, an imitation), binding a pipeline whose sample mask, line mode or
 *   conservative mode is static sends that static value too - only for a state this command
 *   buffer has set dynamically (so its EDS3 feature is on, and only then can a stale value leak).
 *   That last part is outside the API's rules for a static state, which is why a decision of
 *   "always" (safe mode on a driver not in range) leaves it out. A state a static pipeline
 *   overrides counts as unset until the app sets it again; vkCmdExecuteCommands leaves the
 *   primary's state undefined, so it is forgotten there too; vkBeginCommandBuffer starts afresh.
 *   Imitation (host): beneath the fix, vkCmdBindPipeline puts every state its pipeline declares
 *   dynamic back to the pipeline's own create-info values (strides included: the bound buffers
 *   are bound again with the pipeline's strides), and a pipeline with a static sample mask, line
 *   mode or conservative mode gets the app's last dynamic value of it.
 *
 * vb_split (MK-05; Arm erratum 3786496, r42-r47, ANGLE gates every driver below r48):
 *   vkCmdBindVertexBuffers2 with more than one binding and pStrides applies the strides by array
 *   index, not by binding number. The fix (option A) makes each such call one call per binding
 *   (firstBinding = b, bindingCount = 1): exactly equivalent, a few calls more. Below r48 the
 *   server also hides VK_EXT_vertex_input_dynamic_state (vkb_errata_hidden_ext), whose
 *   vkCmdSetVertexInputEXT sets strides another way. If the probe finds single-binding calls
 *   wrong as well, the split cannot help: option B, not built, would take
 *   VERTEX_INPUT_BINDING_STRIDE out of the pipelines' dynamic lists and bake the strides into
 *   pipeline variants keyed by the bound strides (the divisor emulation's variant machinery,
 *   server_emu_divisor.c, already keys variants by g->stride[k]).
 *   Imitation (host): a call with more than one binding and pStrides writes pStrides[i] into the
 *   stride of binding i, not first + i (binding i keeps its buffer; the call's own bindings past
 *   the end keep their old stride). VKBRIDGE_ERRATA_IMITATE_VB=single: single-binding calls too,
 *   the shape the split does not fix (the probe's option B case).
 *
 * The wrappers chain: each install takes t->real as it finds it (its next one down), so the
 * vb_split wrapper of vkCmdBindVertexBuffers2 calls dyn_reemit's, which calls the imitation's.
 * The lifecycle wrappers (command buffers and pipelines) are installed once, by the first of these
 * to be installed, beneath the rest. dyn_reemit's own re-sent bindings go beneath vb_split and are
 * one binding per call already.
 */
#define _GNU_SOURCE
#include "server_errata.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define CB_BUCKETS 1024
#define PL_BUCKETS 4096
#define NVB 32 /* vertex buffer bindings tracked */

/* The states kept (S_*), per stencil face where the API sets faces apart */
enum {
    S_DTEST = 1u << 0, S_DWRITE = 1u << 1, S_DCMP = 1u << 2, S_STEST = 1u << 3,
    S_SOP_F = 1u << 4, S_SOP_B = 1u << 5, S_SCM_F = 1u << 6, S_SCM_B = 1u << 7,
    S_SWM_F = 1u << 8, S_SWM_B = 1u << 9, S_SREF_F = 1u << 10, S_SREF_B = 1u << 11,
    S_CULL = 1u << 12, S_SMASK = 1u << 13, S_LINE = 1u << 14, S_CONS = 1u << 15,
    S_STRIDE = 1u << 16, /* a pipeline's dynamic list only; the values are the vertex bindings */
};
#define S_DS (S_DTEST | S_DWRITE | S_DCMP | S_STEST | S_SOP_F | S_SOP_B | S_SCM_F | S_SCM_B | S_SWM_F | S_SWM_B | S_SREF_F | S_SREF_B)
#define S_RS (S_CULL | S_LINE | S_CONS)
#define S_EDS3 (S_SMASK | S_LINE | S_CONS)
#define S_ALL (S_DS | S_RS | S_SMASK | S_STRIDE)
enum { H_DS = 1, H_RS = 2, H_MS = 4, H_VI = 8 }; /* create-info sections a pipeline has */

typedef struct {
    VkBool32 dtest, dwrite, stest;
    VkCompareOp dcmp;
    VkCullModeFlags cull;
    struct {
        VkStencilOp fail, pass, dfail;
        VkCompareOp cmp;
        uint32_t cmask, wmask, ref;
    } st[2]; /* front, back */
    VkSampleCountFlagBits samples;
    VkSampleMask smask[2];
    VkLineRasterizationModeEXT line;
    VkConservativeRasterizationModeEXT cons;
} dstate;

typedef struct {
    VkBuffer buf;
    VkDeviceSize off, size, stride;
} vbind;

typedef struct cbs {
    VkCommandBuffer cb;
    VkCommandPool pool;
    struct cbs *next;
    /* the app's values, as the fix sees them */
    uint32_t set;  /* S_*: set and still valid */
    uint32_t ever; /* S_EDS3 set dynamically since the begin */
    dstate v;
    uint32_t vb_buf, vb_stride; /* bindings with a buffer / a dynamic stride */
    vbind vb[NVB];
    /* what reached the driver, as the imitations see it (host) */
    uint32_t d_ever, d_buf, d_stride;
    dstate dv;
    vbind dvb[NVB];
} cbs;

typedef struct pinfo {
    VkPipeline p;
    struct pinfo *next;
    uint32_t dyn, has;
    dstate v; /* the create info's values */
    uint32_t vi_bind;
    VkDeviceSize vi_stride[NVB];
} pinfo;

typedef struct vkb_errata_cmd {
    vkb_errata_dev *owner;
    pthread_mutex_t lock, plock;
    uint64_t gen; /* changes when a command buffer's state goes (the per-thread cache) */
    cbs *cb[CB_BUCKETS];
    pinfo *pl[PL_BUCKETS];
    int life_on, imi_vb_on, imi_vb_bug, imi_vb_single;
    vkb_dispatch life, imi_vb, imi_dyn, fix_dyn, fix_vb; /* each wrapper set's next one down */
    uint64_t binds, binds_resent, resent, resent_vb, resent_static, nopipe;
    uint64_t split, split_into;
    uint64_t imi_resets, imi_stale, imi_vb_wrong;
} vkb_errata_cmd;

static uint64_t gen_base;
static vkb_errata_cmd *last_cmd; /* for a thread without a table (vkb_errata_cur's last device) */
static __thread struct {
    vkb_errata_cmd *c;
    VkCommandBuffer cb;
    cbs *s;
    uint64_t gen;
} tc;

#define ADD(x, n) __atomic_add_fetch(&(x), (n), __ATOMIC_RELAXED)

static vkb_errata_cmd *C(void)
{
    vkb_errata_dev *e = vkb_errata_cur();
    return e && e->cmd ? e->cmd : __atomic_load_n(&last_cmd, __ATOMIC_ACQUIRE);
}

static unsigned hsh(uint64_t k, unsigned buckets)
{
    k ^= k >> 30;
    k *= 0xbf58476d1ce4e5b9ull;
    k ^= k >> 27;
    return (unsigned)(k % buckets);
}

static void cbs_reset(cbs *s)
{
    VkCommandBuffer cb = s->cb;
    VkCommandPool pool = s->pool;
    cbs *next = s->next;
    memset(s, 0, sizeof(*s));
    s->cb = cb, s->pool = pool, s->next = next;
}

/* The state of cb (made when create and missing) */
static cbs *cb_find(vkb_errata_cmd *c, VkCommandBuffer cb, int create)
{
    uint64_t g = __atomic_load_n(&c->gen, __ATOMIC_ACQUIRE);
    if (tc.c == c && tc.cb == cb && tc.gen == g) return tc.s;
    unsigned h = hsh((uint64_t)(uintptr_t)cb, CB_BUCKETS);
    pthread_mutex_lock(&c->lock);
    cbs *s = c->cb[h];
    while (s && s->cb != cb) s = s->next;
    if (!s && create && (s = calloc(1, sizeof(*s)))) {
        s->cb = cb;
        s->next = c->cb[h];
        c->cb[h] = s;
    }
    pthread_mutex_unlock(&c->lock);
    if (s) tc.c = c, tc.cb = cb, tc.s = s, tc.gen = g;
    return s;
}

/* Under c->lock */
static void cb_drop_locked(vkb_errata_cmd *c, int (*match)(const cbs *, const void *), const void *arg)
{
    for (unsigned h = 0; h < CB_BUCKETS; h++)
        for (cbs **pp = &c->cb[h]; *pp;) {
            cbs *s = *pp;
            if (match(s, arg)) {
                *pp = s->next;
                free(s);
            } else
                pp = &s->next;
        }
    __atomic_add_fetch(&c->gen, 1, __ATOMIC_RELEASE);
}

static int pinfo_get(vkb_errata_cmd *c, VkPipeline p, pinfo *out)
{
    unsigned h = hsh((uint64_t)(uintptr_t)p, PL_BUCKETS);
    pthread_mutex_lock(&c->plock);
    pinfo *i = c->pl[h];
    while (i && i->p != p) i = i->next;
    if (i) *out = *i;
    pthread_mutex_unlock(&c->plock);
    return i != NULL;
}

static uint32_t dyn_bit(VkDynamicState d)
{
    switch (d) {
    case VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE: return S_DTEST;
    case VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE: return S_DWRITE;
    case VK_DYNAMIC_STATE_DEPTH_COMPARE_OP: return S_DCMP;
    case VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE: return S_STEST;
    case VK_DYNAMIC_STATE_STENCIL_OP: return S_SOP_F | S_SOP_B;
    case VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK: return S_SCM_F | S_SCM_B;
    case VK_DYNAMIC_STATE_STENCIL_WRITE_MASK: return S_SWM_F | S_SWM_B;
    case VK_DYNAMIC_STATE_STENCIL_REFERENCE: return S_SREF_F | S_SREF_B;
    case VK_DYNAMIC_STATE_CULL_MODE: return S_CULL;
    case VK_DYNAMIC_STATE_SAMPLE_MASK_EXT: return S_SMASK;
    case VK_DYNAMIC_STATE_LINE_RASTERIZATION_MODE_EXT: return S_LINE;
    case VK_DYNAMIC_STATE_CONSERVATIVE_RASTERIZATION_MODE_EXT: return S_CONS;
    case VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE: return S_STRIDE;
    default: return 0;
    }
}

static const void *chain_find(const void *pnext, VkStructureType type)
{
    for (const VkBaseInStructure *b = pnext; b; b = b->pNext)
        if (b->sType == type) return b;
    return NULL;
}

/* A graphics pipeline's record: its dynamic list and the values its create info holds, from the
 * parts it has (graphics pipeline libraries: its own parts, the rest from its libraries). */
static void pinfo_fill(vkb_errata_cmd *c, const VkGraphicsPipelineCreateInfo *ci, pinfo *pi)
{
    memset(pi, 0, sizeof(*pi));
    pi->v.line = VK_LINE_RASTERIZATION_MODE_DEFAULT_EXT;
    pi->v.cons = VK_CONSERVATIVE_RASTERIZATION_MODE_DISABLED_EXT;
    const VkGraphicsPipelineLibraryCreateInfoEXT *gl = chain_find(ci->pNext, VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT);
    const VkPipelineLibraryCreateInfoKHR *li = chain_find(ci->pNext, VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR);
    VkGraphicsPipelineLibraryFlagsEXT parts =
        VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT | VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT |
        VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT | VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT;
    if (gl)
        parts = gl->flags;
    else if ((ci->flags & VK_PIPELINE_CREATE_LIBRARY_BIT_KHR) || (li && li->libraryCount))
        parts = 0;
    if (parts && ci->pDynamicState)
        for (uint32_t k = 0; k < ci->pDynamicState->dynamicStateCount; k++) pi->dyn |= dyn_bit(ci->pDynamicState->pDynamicStates[k]);
    const VkPipelineDepthStencilStateCreateInfo *ds = ci->pDepthStencilState;
    if ((parts & VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT) && ds) {
        pi->has |= H_DS;
        pi->v.dtest = ds->depthTestEnable, pi->v.dwrite = ds->depthWriteEnable, pi->v.dcmp = ds->depthCompareOp;
        pi->v.stest = ds->stencilTestEnable;
        const VkStencilOpState *f[2] = {&ds->front, &ds->back};
        for (int i = 0; i < 2; i++) {
            pi->v.st[i].fail = f[i]->failOp, pi->v.st[i].pass = f[i]->passOp, pi->v.st[i].dfail = f[i]->depthFailOp;
            pi->v.st[i].cmp = f[i]->compareOp, pi->v.st[i].cmask = f[i]->compareMask, pi->v.st[i].wmask = f[i]->writeMask;
            pi->v.st[i].ref = f[i]->reference;
        }
    }
    const VkPipelineRasterizationStateCreateInfo *rs = ci->pRasterizationState;
    if ((parts & VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT) && rs) {
        pi->has |= H_RS;
        pi->v.cull = rs->cullMode;
        const VkPipelineRasterizationLineStateCreateInfo *ls = chain_find(rs->pNext, VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_LINE_STATE_CREATE_INFO);
        if (ls) pi->v.line = (VkLineRasterizationModeEXT)ls->lineRasterizationMode;
        const VkPipelineRasterizationConservativeStateCreateInfoEXT *cs =
            chain_find(rs->pNext, VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_CONSERVATIVE_STATE_CREATE_INFO_EXT);
        if (cs) pi->v.cons = cs->conservativeRasterizationMode;
    }
    const VkPipelineMultisampleStateCreateInfo *ms = ci->pMultisampleState;
    if ((parts & VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT) && ms) {
        pi->has |= H_MS;
        pi->v.samples = ms->rasterizationSamples ? ms->rasterizationSamples : VK_SAMPLE_COUNT_1_BIT;
        pi->v.smask[0] = pi->v.smask[1] = ~0u;
        if (ms->pSampleMask) {
            pi->v.smask[0] = ms->pSampleMask[0];
            if (pi->v.samples > 32) pi->v.smask[1] = ms->pSampleMask[1];
        }
    }
    const VkPipelineVertexInputStateCreateInfo *vi = ci->pVertexInputState;
    if ((parts & VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT) && vi) {
        pi->has |= H_VI;
        for (uint32_t k = 0; k < vi->vertexBindingDescriptionCount; k++) {
            uint32_t b = vi->pVertexBindingDescriptions[k].binding;
            if (b < NVB) pi->vi_bind |= 1u << b, pi->vi_stride[b] = vi->pVertexBindingDescriptions[k].stride;
        }
    }
    for (uint32_t l = 0; li && l < li->libraryCount; l++) {
        pinfo lp;
        if (!pinfo_get(c, li->pLibraries[l], &lp)) continue;
        pi->dyn |= lp.dyn;
        if ((lp.has & H_DS) && !(pi->has & H_DS)) memcpy(&pi->v.dtest, &lp.v.dtest, sizeof(VkBool32) * 3), pi->v.dcmp = lp.v.dcmp,
                                                    memcpy(pi->v.st, lp.v.st, sizeof(pi->v.st));
        if ((lp.has & H_RS) && !(pi->has & H_RS)) pi->v.cull = lp.v.cull, pi->v.line = lp.v.line, pi->v.cons = lp.v.cons;
        if ((lp.has & H_MS) && !(pi->has & H_MS)) pi->v.samples = lp.v.samples, memcpy(pi->v.smask, lp.v.smask, sizeof(pi->v.smask));
        if ((lp.has & H_VI) && !(pi->has & H_VI)) pi->vi_bind = lp.vi_bind, memcpy(pi->vi_stride, lp.vi_stride, sizeof(pi->vi_stride));
        pi->has |= lp.has;
    }
}

/* Sends the values of v for the states in m; the number of calls */
static unsigned emit(const vkb_dispatch *n, VkCommandBuffer cb, const dstate *v, uint32_t m)
{
    unsigned calls = 0;
#define E(bit, fn, ...) \
    if ((m & (bit)) && n->fn) n->fn(cb, __VA_ARGS__), calls++
    E(S_DTEST, vkCmdSetDepthTestEnable, v->dtest);
    E(S_DWRITE, vkCmdSetDepthWriteEnable, v->dwrite);
    E(S_DCMP, vkCmdSetDepthCompareOp, v->dcmp);
    E(S_STEST, vkCmdSetStencilTestEnable, v->stest);
    E(S_CULL, vkCmdSetCullMode, v->cull);
    E(S_SMASK, vkCmdSetSampleMaskEXT, v->samples, v->smask);
    E(S_LINE, vkCmdSetLineRasterizationModeEXT, v->line);
    E(S_CONS, vkCmdSetConservativeRasterizationModeEXT, v->cons);
#undef E
    /* stencil: both faces in one call where they agree */
#define F2(fb, bb, same, fn, ...)                                                                        \
    do {                                                                                                 \
        int f_ = !!(m & (fb)), b_ = !!(m & (bb));                                                        \
        if (!n->fn || !(f_ | b_)) break;                                                                 \
        if (f_ && b_ && (same)) {                                                                        \
            int i = 0;                                                                                   \
            n->fn(cb, VK_STENCIL_FACE_FRONT_AND_BACK, __VA_ARGS__), calls++;                             \
            break;                                                                                       \
        }                                                                                                \
        for (int i = 0; i < 2; i++)                                                                      \
            if (i ? b_ : f_) n->fn(cb, i ? VK_STENCIL_FACE_BACK_BIT : VK_STENCIL_FACE_FRONT_BIT, __VA_ARGS__), calls++; \
    } while (0)
    const __typeof__(v->st[0]) *s0 = &v->st[0], *s1 = &v->st[1];
    F2(S_SOP_F, S_SOP_B, s0->fail == s1->fail && s0->pass == s1->pass && s0->dfail == s1->dfail && s0->cmp == s1->cmp,
       vkCmdSetStencilOp, v->st[i].fail, v->st[i].pass, v->st[i].dfail, v->st[i].cmp);
    F2(S_SCM_F, S_SCM_B, s0->cmask == s1->cmask, vkCmdSetStencilCompareMask, v->st[i].cmask);
    F2(S_SWM_F, S_SWM_B, s0->wmask == s1->wmask, vkCmdSetStencilWriteMask, v->st[i].wmask);
    F2(S_SREF_F, S_SREF_B, s0->ref == s1->ref, vkCmdSetStencilReference, v->st[i].ref);
#undef F2
    return calls;
}

static unsigned rebind(const vkb_dispatch *n, VkCommandBuffer cb, const vbind *vb, uint32_t which)
{
    unsigned calls = 0;
    for (uint32_t b = 0; which && n->vkCmdBindVertexBuffers2; b++, which >>= 1)
        if (which & 1) n->vkCmdBindVertexBuffers2(cb, b, 1, &vb[b].buf, &vb[b].off, &vb[b].size, &vb[b].stride), calls++;
    return calls;
}

static uint32_t face_bits(VkStencilFaceFlags f, uint32_t front)
{
    return ((f & VK_STENCIL_FACE_FRONT_BIT) ? front : 0) | ((f & VK_STENCIL_FACE_BACK_BIT) ? front << 1 : 0);
}

/* ---- lifecycle: command buffers and pipelines (once, beneath the rest) */

static VKAPI_ATTR VkResult VKAPI_CALL l_AllocateCommandBuffers(VkDevice d, const VkCommandBufferAllocateInfo *ai, VkCommandBuffer *out)
{
    vkb_errata_cmd *c = C();
    VkResult r = c->life.vkAllocateCommandBuffers(d, ai, out);
    if (r == VK_SUCCESS)
        for (uint32_t i = 0; i < ai->commandBufferCount; i++) {
            cbs *s = cb_find(c, out[i], 1);
            if (s) cbs_reset(s), s->pool = ai->commandPool;
        }
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL l_BeginCommandBuffer(VkCommandBuffer cb, const VkCommandBufferBeginInfo *bi)
{
    vkb_errata_cmd *c = C();
    cbs *s = cb_find(c, cb, 0);
    if (s) cbs_reset(s);
    return c->life.vkBeginCommandBuffer(cb, bi);
}

static int match_cbs(const cbs *s, const void *arg)
{
    const VkCommandBuffer *l = ((const void *const *)arg)[0];
    uint32_t n = *(const uint32_t *)((const void *const *)arg)[1];
    for (uint32_t i = 0; i < n; i++)
        if (l[i] == s->cb) return 1;
    return 0;
}

static int match_pool(const cbs *s, const void *arg) { return s->pool == *(const VkCommandPool *)arg; }

static VKAPI_ATTR void VKAPI_CALL l_FreeCommandBuffers(VkDevice d, VkCommandPool pool, uint32_t n, const VkCommandBuffer *l)
{
    vkb_errata_cmd *c = C();
    const void *arg[2] = {l, &n};
    pthread_mutex_lock(&c->lock);
    cb_drop_locked(c, match_cbs, arg);
    pthread_mutex_unlock(&c->lock);
    c->life.vkFreeCommandBuffers(d, pool, n, l);
}

static VKAPI_ATTR void VKAPI_CALL l_DestroyCommandPool(VkDevice d, VkCommandPool pool, const VkAllocationCallbacks *a)
{
    vkb_errata_cmd *c = C();
    if (pool) {
        pthread_mutex_lock(&c->lock);
        cb_drop_locked(c, match_pool, &pool);
        pthread_mutex_unlock(&c->lock);
    }
    c->life.vkDestroyCommandPool(d, pool, a);
}

static VKAPI_ATTR VkResult VKAPI_CALL l_CreateGraphicsPipelines(VkDevice d, VkPipelineCache pc, uint32_t n, const VkGraphicsPipelineCreateInfo *ci,
                                                               const VkAllocationCallbacks *a, VkPipeline *out)
{
    vkb_errata_cmd *c = C();
    VkResult r = c->life.vkCreateGraphicsPipelines(d, pc, n, ci, a, out);
    for (uint32_t i = 0; i < n; i++) {
        if (!out[i]) continue;
        pinfo *pi = malloc(sizeof(*pi));
        if (!pi) continue;
        pinfo_fill(c, &ci[i], pi);
        pi->p = out[i];
        unsigned h = hsh((uint64_t)(uintptr_t)out[i], PL_BUCKETS);
        pthread_mutex_lock(&c->plock);
        pi->next = c->pl[h];
        c->pl[h] = pi;
        pthread_mutex_unlock(&c->plock);
    }
    return r;
}

static VKAPI_ATTR void VKAPI_CALL l_DestroyPipeline(VkDevice d, VkPipeline p, const VkAllocationCallbacks *a)
{
    vkb_errata_cmd *c = C();
    if (p) {
        unsigned h = hsh((uint64_t)(uintptr_t)p, PL_BUCKETS);
        pthread_mutex_lock(&c->plock);
        for (pinfo **pp = &c->pl[h]; *pp; pp = &(*pp)->next)
            if ((*pp)->p == p) {
                pinfo *x = *pp;
                *pp = x->next;
                free(x);
                break;
            }
        pthread_mutex_unlock(&c->plock);
    }
    c->life.vkDestroyPipeline(d, p, a);
}

/* ---- the imitations (host builds; beneath the fixes) */

static VKAPI_ATTR void VKAPI_CALL i_CmdBindVertexBuffers(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkBuffer *b, const VkDeviceSize *o)
{
    vkb_errata_cmd *c = C();
    cbs *s = cb_find(c, cb, 1);
    for (uint32_t j = 0; s && j < n; j++)
        if (first + j < NVB) {
            vbind *v = &s->dvb[first + j];
            v->buf = b[j], v->off = o[j], v->size = VK_WHOLE_SIZE;
            s->d_buf |= 1u << (first + j);
        }
    c->imi_vb.vkCmdBindVertexBuffers(cb, first, n, b, o);
}

static VKAPI_ATTR void VKAPI_CALL i_CmdBindVertexBuffers2(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkBuffer *b, const VkDeviceSize *o,
                                                          const VkDeviceSize *sz, const VkDeviceSize *st)
{
    vkb_errata_cmd *c = C();
    cbs *s = cb_find(c, cb, 1);
    if (!s) {
        c->imi_vb.vkCmdBindVertexBuffers2(cb, first, n, b, o, sz, st);
        return;
    }
    int bug = c->imi_vb_bug && st && (n > 1 || c->imi_vb_single) && first + n <= NVB;
    for (uint32_t j = 0; j < n; j++)
        if (first + j < NVB) {
            vbind *v = &s->dvb[first + j];
            v->buf = b[j], v->off = o[j], v->size = sz ? sz[j] : VK_WHOLE_SIZE;
            s->d_buf |= 1u << (first + j);
            if (st && !bug) v->stride = st[j], s->d_stride |= 1u << (first + j);
        }
    if (!bug) {
        c->imi_vb.vkCmdBindVertexBuffers2(cb, first, n, b, o, sz, st);
        return;
    }
    /* the erratum: pStrides[i] goes to binding i */
    for (uint32_t i = 0; i < n; i++) s->dvb[i].stride = st[i], s->d_stride |= 1u << i;
    for (uint32_t k = 0; k < NVB; k++)
        if ((k < n || (k >= first && k < first + n)) && (s->d_buf >> k & 1)) {
            vbind v = s->dvb[k];
            if (!(s->d_stride >> k & 1)) v.stride = 0;
            c->imi_vb.vkCmdBindVertexBuffers2(cb, k, 1, &v.buf, &v.off, &v.size, &v.stride);
        }
    ADD(c->imi_vb_wrong, 1);
}

static VKAPI_ATTR void VKAPI_CALL i_CmdSetSampleMaskEXT(VkCommandBuffer cb, VkSampleCountFlagBits samples, const VkSampleMask *m)
{
    vkb_errata_cmd *c = C();
    cbs *s = cb_find(c, cb, 1);
    if (s) {
        s->dv.samples = samples, s->dv.smask[0] = m[0], s->dv.smask[1] = samples > 32 ? m[1] : ~0u;
        s->d_ever |= S_SMASK;
    }
    c->imi_dyn.vkCmdSetSampleMaskEXT(cb, samples, m);
}

static VKAPI_ATTR void VKAPI_CALL i_CmdSetLineRasterizationModeEXT(VkCommandBuffer cb, VkLineRasterizationModeEXT mode)
{
    vkb_errata_cmd *c = C();
    cbs *s = cb_find(c, cb, 1);
    if (s) s->dv.line = mode, s->d_ever |= S_LINE;
    c->imi_dyn.vkCmdSetLineRasterizationModeEXT(cb, mode);
}

static VKAPI_ATTR void VKAPI_CALL i_CmdSetConservativeRasterizationModeEXT(VkCommandBuffer cb, VkConservativeRasterizationModeEXT mode)
{
    vkb_errata_cmd *c = C();
    cbs *s = cb_find(c, cb, 1);
    if (s) s->dv.cons = mode, s->d_ever |= S_CONS;
    c->imi_dyn.vkCmdSetConservativeRasterizationModeEXT(cb, mode);
}

static VKAPI_ATTR void VKAPI_CALL i_CmdBindPipeline(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipeline p)
{
    vkb_errata_cmd *c = C();
    c->imi_dyn.vkCmdBindPipeline(cb, bp, p);
    pinfo pi;
    if (bp != VK_PIPELINE_BIND_POINT_GRAPHICS || !pinfo_get(c, p, &pi)) return;
    cbs *s = cb_find(c, cb, 1);
    if (!s) return;
    /* the dynamic states go back to the pipeline's own (ignored) values */
    uint32_t m = pi.dyn & ~S_STRIDE;
    if (!(pi.has & H_DS)) m &= ~S_DS;
    if (!(pi.has & H_RS)) m &= ~S_RS;
    if (!(pi.has & H_MS)) m &= ~S_SMASK;
    unsigned n = emit(&c->imi_dyn, cb, &pi.v, m);
    if ((pi.dyn & S_STRIDE) && (pi.has & H_VI) && s->d_buf) {
        for (uint32_t b = 0; b < NVB; b++)
            if (s->d_buf >> b & 1) s->dvb[b].stride = (pi.vi_bind >> b & 1) ? pi.vi_stride[b] : 0, s->d_stride |= 1u << b;
        n += rebind(&c->imi_dyn, cb, s->dvb, s->d_buf);
    }
    if (n) ADD(c->imi_resets, 1);
    /* a static sample mask / line mode / conservative mode picks up the last dynamic value */
    uint32_t st = S_EDS3 & ~pi.dyn & s->d_ever;
    if (st && emit(&c->imi_dyn, cb, &s->dv, st)) ADD(c->imi_stale, 1);
}

/* ---- dyn_reemit (MK-04) */

static cbs *fx_state(vkb_errata_cmd **cp, VkCommandBuffer cb)
{
    *cp = C();
    return cb_find(*cp, cb, 1);
}

#define FX_SET(name, bits, assign, args, call)                                                           \
    static VKAPI_ATTR void VKAPI_CALL f_##name args                                                      \
    {                                                                                                    \
        vkb_errata_cmd *c;                                                                               \
        cbs *s = fx_state(&c, cb);                                                                       \
        if (s) {                                                                                         \
            assign;                                                                                      \
            s->set |= (bits);                                                                            \
            s->ever |= (bits) & S_EDS3;                                                                  \
        }                                                                                                \
        c->fix_dyn.name call;                                                                            \
    }

FX_SET(vkCmdSetDepthTestEnable, S_DTEST, s->v.dtest = v, (VkCommandBuffer cb, VkBool32 v), (cb, v))
FX_SET(vkCmdSetDepthWriteEnable, S_DWRITE, s->v.dwrite = v, (VkCommandBuffer cb, VkBool32 v), (cb, v))
FX_SET(vkCmdSetDepthCompareOp, S_DCMP, s->v.dcmp = v, (VkCommandBuffer cb, VkCompareOp v), (cb, v))
FX_SET(vkCmdSetStencilTestEnable, S_STEST, s->v.stest = v, (VkCommandBuffer cb, VkBool32 v), (cb, v))
FX_SET(vkCmdSetCullMode, S_CULL, s->v.cull = v, (VkCommandBuffer cb, VkCullModeFlags v), (cb, v))
FX_SET(vkCmdSetLineRasterizationModeEXT, S_LINE, s->v.line = v, (VkCommandBuffer cb, VkLineRasterizationModeEXT v), (cb, v))
FX_SET(vkCmdSetConservativeRasterizationModeEXT, S_CONS, s->v.cons = v, (VkCommandBuffer cb, VkConservativeRasterizationModeEXT v), (cb, v))
FX_SET(vkCmdSetSampleMaskEXT, S_SMASK, (s->v.samples = n, s->v.smask[0] = m[0], s->v.smask[1] = n > 32 ? m[1] : ~0u),
       (VkCommandBuffer cb, VkSampleCountFlagBits n, const VkSampleMask *m), (cb, n, m))
#define FACES(f, x)                                                                                      \
    for (int i = 0; i < 2; i++)                                                                          \
        if ((f) & (i ? VK_STENCIL_FACE_BACK_BIT : VK_STENCIL_FACE_FRONT_BIT)) x
FX_SET(vkCmdSetStencilOp, face_bits(f, S_SOP_F),
       FACES(f, (s->v.st[i].fail = fa, s->v.st[i].pass = pa, s->v.st[i].dfail = df, s->v.st[i].cmp = cmp)),
       (VkCommandBuffer cb, VkStencilFaceFlags f, VkStencilOp fa, VkStencilOp pa, VkStencilOp df, VkCompareOp cmp), (cb, f, fa, pa, df, cmp))
FX_SET(vkCmdSetStencilCompareMask, face_bits(f, S_SCM_F), FACES(f, s->v.st[i].cmask = v), (VkCommandBuffer cb, VkStencilFaceFlags f, uint32_t v),
       (cb, f, v))
FX_SET(vkCmdSetStencilWriteMask, face_bits(f, S_SWM_F), FACES(f, s->v.st[i].wmask = v), (VkCommandBuffer cb, VkStencilFaceFlags f, uint32_t v),
       (cb, f, v))
FX_SET(vkCmdSetStencilReference, face_bits(f, S_SREF_F), FACES(f, s->v.st[i].ref = v), (VkCommandBuffer cb, VkStencilFaceFlags f, uint32_t v),
       (cb, f, v))

static VKAPI_ATTR void VKAPI_CALL f_CmdBindVertexBuffers(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkBuffer *b, const VkDeviceSize *o)
{
    vkb_errata_cmd *c;
    cbs *s = fx_state(&c, cb);
    for (uint32_t j = 0; s && j < n; j++)
        if (first + j < NVB) {
            vbind *v = &s->vb[first + j];
            v->buf = b[j], v->off = o[j], v->size = VK_WHOLE_SIZE;
            s->vb_buf |= 1u << (first + j);
        }
    c->fix_dyn.vkCmdBindVertexBuffers(cb, first, n, b, o);
}

static VKAPI_ATTR void VKAPI_CALL f_CmdBindVertexBuffers2(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkBuffer *b, const VkDeviceSize *o,
                                                          const VkDeviceSize *sz, const VkDeviceSize *st)
{
    vkb_errata_cmd *c;
    cbs *s = fx_state(&c, cb);
    for (uint32_t j = 0; s && j < n; j++)
        if (first + j < NVB) {
            vbind *v = &s->vb[first + j];
            v->buf = b[j], v->off = o[j], v->size = sz ? sz[j] : VK_WHOLE_SIZE;
            s->vb_buf |= 1u << (first + j);
            if (st) v->stride = st[j], s->vb_stride |= 1u << (first + j);
        }
    c->fix_dyn.vkCmdBindVertexBuffers2(cb, first, n, b, o, sz, st);
}

static VKAPI_ATTR void VKAPI_CALL f_CmdExecuteCommands(VkCommandBuffer cb, uint32_t n, const VkCommandBuffer *l)
{
    vkb_errata_cmd *c;
    cbs *s = fx_state(&c, cb);
    c->fix_dyn.vkCmdExecuteCommands(cb, n, l);
    if (s) s->set = 0, s->vb_buf = s->vb_stride = 0;
}

static VKAPI_ATTR void VKAPI_CALL f_CmdBindPipeline(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipeline p)
{
    vkb_errata_cmd *c = C();
    c->fix_dyn.vkCmdBindPipeline(cb, bp, p);
    if (bp != VK_PIPELINE_BIND_POINT_GRAPHICS) return;
    ADD(c->binds, 1);
    cbs *s = cb_find(c, cb, 0);
    if (!s || !(s->set | s->ever | s->vb_stride)) return;
    pinfo pi;
    if (!pinfo_get(c, p, &pi)) {
        ADD(c->nopipe, 1);
        return;
    }
    unsigned n = emit(&c->fix_dyn, cb, &s->v, pi.dyn & s->set), nv = 0, ns = 0;
    if (pi.dyn & S_STRIDE)
        nv = rebind(&c->fix_dyn, cb, s->vb, s->vb_stride & s->vb_buf);
    else
        s->vb_stride = 0; /* static strides: the dynamic ones are gone */
    vkb_errata_dev *e = c->owner;
    if ((e->targeted | e->imitated) >> VKB_ERRATUM_DYN_REEMIT & 1) {
        uint32_t st = S_EDS3 & ~pi.dyn & s->ever;
        if (!(pi.has & H_MS)) st &= ~S_SMASK;
        if (!(pi.has & H_RS)) st &= ~(S_LINE | S_CONS);
        if (st) ns = emit(&c->fix_dyn, cb, &pi.v, st);
    }
    s->set &= pi.dyn; /* what the pipeline has static the app must set again */
    if (n | nv | ns) {
        ADD(c->binds_resent, 1);
        ADD(c->resent, n);
        ADD(c->resent_vb, nv);
        ADD(c->resent_static, ns);
    }
}

/* ---- vb_split (MK-05) */

static VKAPI_ATTR void VKAPI_CALL f_CmdBindVertexBuffers2Split(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkBuffer *b,
                                                               const VkDeviceSize *o, const VkDeviceSize *sz, const VkDeviceSize *st)
{
    vkb_errata_cmd *c = C();
    if (n <= 1 || !st) {
        c->fix_vb.vkCmdBindVertexBuffers2(cb, first, n, b, o, sz, st);
        return;
    }
    for (uint32_t j = 0; j < n; j++) c->fix_vb.vkCmdBindVertexBuffers2(cb, first + j, 1, &b[j], &o[j], sz ? &sz[j] : NULL, &st[j]);
    ADD(c->split, 1);
    ADD(c->split_into, n);
}

/* ---- installs */

static vkb_errata_cmd *cmd_get(vkb_srv_table *t, vkb_errata_dev *e)
{
    if (e->cmd) return e->cmd;
    vkb_errata_cmd *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    pthread_mutex_init(&c->lock, NULL);
    pthread_mutex_init(&c->plock, NULL);
    c->owner = e;
    c->gen = __atomic_add_fetch(&gen_base, 1ull << 32, __ATOMIC_RELAXED);
    e->cmd = c;
    if (!e->probe) __atomic_store_n(&last_cmd, c, __ATOMIC_RELEASE);
    return c;
}

static void life_install(vkb_srv_table *t, vkb_errata_cmd *c)
{
    if (c->life_on) return;
    c->life_on = 1;
    c->life = t->real;
    ERRATA_HOOK(t, vkAllocateCommandBuffers, l_AllocateCommandBuffers);
    ERRATA_HOOK(t, vkBeginCommandBuffer, l_BeginCommandBuffer);
    ERRATA_HOOK(t, vkFreeCommandBuffers, l_FreeCommandBuffers);
    ERRATA_HOOK(t, vkDestroyCommandPool, l_DestroyCommandPool);
    ERRATA_HOOK(t, vkCreateGraphicsPipelines, l_CreateGraphicsPipelines);
    ERRATA_HOOK(t, vkDestroyPipeline, l_DestroyPipeline);
}

static void imi_vb_install(vkb_srv_table *t, vkb_errata_cmd *c)
{
    if (c->imi_vb_on) return;
    c->imi_vb_on = 1;
    c->imi_vb = t->real;
    ERRATA_HOOK(t, vkCmdBindVertexBuffers, i_CmdBindVertexBuffers);
    ERRATA_HOOK(t, vkCmdBindVertexBuffers2, i_CmdBindVertexBuffers2);
}

void vkb_errata_dyn_imitate(vkb_srv_table *t, vkb_errata_dev *e)
{
    vkb_errata_cmd *c = cmd_get(t, e);
    if (!c) return;
    life_install(t, c);
    c->imi_dyn = t->real;
    ERRATA_HOOK(t, vkCmdBindPipeline, i_CmdBindPipeline);
    ERRATA_HOOK(t, vkCmdSetSampleMaskEXT, i_CmdSetSampleMaskEXT);
    ERRATA_HOOK(t, vkCmdSetLineRasterizationModeEXT, i_CmdSetLineRasterizationModeEXT);
    ERRATA_HOOK(t, vkCmdSetConservativeRasterizationModeEXT, i_CmdSetConservativeRasterizationModeEXT);
    imi_vb_install(t, c); /* the bound buffers, to bind again with the pipeline's strides */
}

void vkb_errata_vb_imitate(vkb_srv_table *t, vkb_errata_dev *e)
{
    vkb_errata_cmd *c = cmd_get(t, e);
    if (!c) return;
    life_install(t, c);
    imi_vb_install(t, c);
    c->imi_vb_bug = 1;
    const char *shape = vkb_srv_env("VKBRIDGE_ERRATA_IMITATE_VB");
    c->imi_vb_single = shape && !strcmp(shape, "single");
    if (c->imi_vb_single && !e->probe) VKB_WARN("errata: vb_split imitated on single-binding calls too (VKBRIDGE_ERRATA_IMITATE_VB=single)");
}

void vkb_errata_dyn_install(vkb_srv_table *t, vkb_errata_dev *e)
{
    vkb_errata_cmd *c = cmd_get(t, e);
    if (!c) return;
    life_install(t, c);
    c->fix_dyn = t->real;
    ERRATA_HOOK(t, vkCmdBindPipeline, f_CmdBindPipeline);
    ERRATA_HOOK(t, vkCmdBindVertexBuffers, f_CmdBindVertexBuffers);
    ERRATA_HOOK(t, vkCmdBindVertexBuffers2, f_CmdBindVertexBuffers2);
    ERRATA_HOOK(t, vkCmdExecuteCommands, f_CmdExecuteCommands);
    ERRATA_HOOK(t, vkCmdSetDepthTestEnable, f_vkCmdSetDepthTestEnable);
    ERRATA_HOOK(t, vkCmdSetDepthWriteEnable, f_vkCmdSetDepthWriteEnable);
    ERRATA_HOOK(t, vkCmdSetDepthCompareOp, f_vkCmdSetDepthCompareOp);
    ERRATA_HOOK(t, vkCmdSetStencilTestEnable, f_vkCmdSetStencilTestEnable);
    ERRATA_HOOK(t, vkCmdSetStencilOp, f_vkCmdSetStencilOp);
    ERRATA_HOOK(t, vkCmdSetStencilCompareMask, f_vkCmdSetStencilCompareMask);
    ERRATA_HOOK(t, vkCmdSetStencilWriteMask, f_vkCmdSetStencilWriteMask);
    ERRATA_HOOK(t, vkCmdSetStencilReference, f_vkCmdSetStencilReference);
    ERRATA_HOOK(t, vkCmdSetCullMode, f_vkCmdSetCullMode);
    ERRATA_HOOK(t, vkCmdSetSampleMaskEXT, f_vkCmdSetSampleMaskEXT);
    ERRATA_HOOK(t, vkCmdSetLineRasterizationModeEXT, f_vkCmdSetLineRasterizationModeEXT);
    ERRATA_HOOK(t, vkCmdSetConservativeRasterizationModeEXT, f_vkCmdSetConservativeRasterizationModeEXT);
}

void vkb_errata_vb_install(vkb_srv_table *t, vkb_errata_dev *e)
{
    vkb_errata_cmd *c = cmd_get(t, e);
    if (!c) return;
    c->fix_vb = t->real;
    ERRATA_HOOK(t, vkCmdBindVertexBuffers2, f_CmdBindVertexBuffers2Split);
}

void vkb_errata_cmd_destroy(vkb_srv_table *t, vkb_errata_dev *e)
{
    vkb_errata_cmd *c = e->cmd;
    if (!c) return;
    if (!e->probe && ((e->on | e->imitated) >> VKB_ERRATUM_DYN_REEMIT & 1) && (c->binds || c->imi_resets || c->imi_stale))
        VKB_INFO("errata: dyn_reemit: %llu graphics bind(s), %llu with state sent again: %llu value(s), %llu vertex buffer "
                 "binding(s), %llu static value(s) (3893903); %llu of an unknown pipeline; imitated: %llu bind(s) reset, %llu "
                 "stale static value(s)",
                 (unsigned long long)c->binds, (unsigned long long)c->binds_resent, (unsigned long long)c->resent,
                 (unsigned long long)c->resent_vb, (unsigned long long)c->resent_static, (unsigned long long)c->nopipe,
                 (unsigned long long)c->imi_resets, (unsigned long long)c->imi_stale);
    if (!e->probe && ((e->on | e->imitated) >> VKB_ERRATUM_VB_SPLIT & 1) && (c->split || c->imi_vb_wrong))
        VKB_INFO("errata: vb_split: %llu vkCmdBindVertexBuffers2 call(s) split into %llu; %llu imitated wrong",
                 (unsigned long long)c->split, (unsigned long long)c->split_into, (unsigned long long)c->imi_vb_wrong);
    for (unsigned h = 0; h < CB_BUCKETS; h++)
        for (cbs *s = c->cb[h], *n; s; s = n) n = s->next, free(s);
    for (unsigned h = 0; h < PL_BUCKETS; h++)
        for (pinfo *p = c->pl[h], *n; p; p = n) n = p->next, free(p);
    pthread_mutex_destroy(&c->lock);
    pthread_mutex_destroy(&c->plock);
    tc.c = NULL;
    vkb_errata_cmd *exp = c;
    __atomic_compare_exchange_n(&last_cmd, &exp, NULL, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    free(c);
    e->cmd = NULL;
}
