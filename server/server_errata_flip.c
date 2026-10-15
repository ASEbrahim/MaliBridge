/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * viewport_flip (MK-02, wgpu #10057: Mali-G715 r54p2 ignores a negative viewport height - the
 * maintenance1 y-flip DXVK and vkd3d-proton always use - so every frame comes out upside down).
 *
 * The fix sends the driver positive heights only and moves the flip into the shader: a viewport
 * (x, y, w, h < 0) becomes (x, y + h, w, -h), and the last pre-rasterization stage negates
 * gl_Position.y (spirv_flip.c). Every vertex then lands on the framebuffer position the negative
 * viewport would have given it (fb_y = y + h * (1 - y_ndc) / 2 both ways), so nothing else moves:
 * no front-face swap, no stencil swap; gl_FragCoord, gl_FrontFacing, derivatives, gl_PointCoord,
 * scissors and depth are as the app's. On a correct driver the fix is invisible - which is how
 * it is tested there (VKBRIDGE_ERRATA=+viewport_flip).
 *
 * Which viewports flip is a mask (bit i: ViewportIndex i is negative); a stage that does not write
 * ViewportIndex uses viewport 0 only. Each graphics pipeline is built for one mask:
 *   static viewports    the mask of its pViewportState: one pipeline, the right one;
 *   dynamic viewports   (VK_DYNAMIC_STATE_VIEWPORT, _WITH_COUNT) the app's handle is built for the
 *                       mask this device's draws used last (at first: negative, DXVK's), and a
 *                       draw whose viewports need another mask binds a variant built for it then,
 *                       once, from a copy of the create info kept for this (its SPIR-V shared
 *                       between pipelines; the render pass and layout kept alive until the
 *                       pipeline goes). DXVK and vkd3d never need a second one; an app that draws
 *                       one pipeline with both signs (Zink: window and FBO) gets two.
 * vkCmdSetViewport / vkCmdSetViewportWithCount pass negatives rewritten; before a draw the
 * variant for the current mask is bound, and the driver's viewports are made to match it: a
 * viewport it flips goes mirrored, (y + h, -h), any other as the app set it. A pipeline with no
 * variant for a mask (none could be built, or VKBRIDGE_FLIP_RETAIN_MB) draws with its own and
 * its mirrored viewports: then a positive viewport goes negative - still right on a correct
 * driver, on the G715 as wrong as without the fix. A command buffer's bind of the app's pipeline
 * is the app's handle as before; the variant's bind is the driver's alone.
 *
 * Graphics pipeline libraries (DXVK links them when the driver has VK_EXT_graphics_pipeline_library):
 * a pre-rasterization library is flipped as a whole pipeline is, for its mask; a pipeline linked
 * from it draws with that mask, and its variant for another is linked again from the library's
 * variant (the libraries a kept create info names live until it goes).
 *
 * Left as they are (logged once): a stage with a module identifier, a gl_Position that is not a
 * vec4. Transform feedback that captures gl_Position captures it flipped (TODO: capture from a
 * copy before the flip; logged).
 *
 * Cost: none when off. On: a SPIR-V pass on the last pre-rasterization stage of each pipeline
 * (a temporary module made from it), a mask check per draw, and for dynamic-viewport pipelines a
 * copy of the create info (VKBRIDGE_FLIP_RETAIN_MB caps the SPIR-V kept, 128 by default; past it
 * new pipelines keep their one mask). Logged: totals at vkDestroyDevice, each variant compiled at
 * draw time (the first 16 at info level).
 *
 * Imitation (host): vkCreateGraphicsPipelines, vkCmdSetViewport and vkCmdSetViewportWithCount
 * beneath the fix turn a negative viewport into the positive one over the same rows - the
 * picture upside down, as the G715 draws it. The fix sends only positive heights, so with it
 * above the imitation has nothing to change.
 *
 * Probe (to be wired in server_probe.c as viewport_flip; recorded raw, decided per mode):
 *   target  4x4 RGBA8, cleared to 0; one pipeline, VS from gl_VertexIndex, FS writes a colour
 *           per primitive and gl_FrontFacing; cull none; front face counter-clockwise;
 *   case a  viewport (0, 4, 4, -4); a triangle covering NDC y in [-1, 0] (x in [-1, 1]), wound
 *           counter-clockwise as seen with y up. Pass: rows 2-3 (the bottom) lit, rows 0-1 clear,
 *           and front-facing. The bug: rows 0-1 lit (facing then reads back-facing too).
 *   case b  two viewports, each flipped in its own half ((0, 4, 2, -4) and (2, 4, 2, -4)),
 *           ViewportIndex from the VS (when shaderOutputViewportIndex and multiViewport are
 *           there): the same triangle into each. Pass: both halves as in a. Catches a driver
 *           that flips viewport 0 only.
 *   Anything else is a fail with the rows' pattern in the detail. Limit 4 s; one tiny pipeline
 *   (two with case b). The gate is a failed probe alone, in both modes: the fix applied where
 *   it is not needed is invisible, but costs its compiles.
 */
#define _GNU_SOURCE
#include "server_errata.h"
#include "spirv_edit.h"
#include "spirv_flip.h"
#include "vkb_wire.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_VP 16
#define MAX_STAGES 8
#define MAX_VARIANTS 4
#define BUCKETS 1024
#define LOG_INFO_MAX 16
#define RETAIN_MB_DEFAULT 128

enum { DYN_NONE, DYN_VIEWPORT, DYN_WITH_COUNT };
enum { OBJ_RENDER_PASS = 1, OBJ_LAYOUT = 2, OBJ_PIPELINE = 3 };

/* SPIR-V kept for variants, shared by content */
typedef struct blob {
    uint64_t hash;
    size_t size;
    uint32_t *code;
    int refs;
    struct blob *next;
} blob;

typedef struct mod_rec {
    VkShaderModule h;
    blob *b;
    struct mod_rec *next;
} mod_rec;

/* a render pass, pipeline layout or pipeline library a kept create info names: destroyed when the
 * last record that names it goes */
typedef struct obj_ref {
    uint64_t h;
    int kind, refs, pending;
    struct obj_ref *next;
} obj_ref;

typedef struct variant {
    uint32_t key;
    VkPipeline p;
    int failed;
} variant;

typedef struct pipe_rec {
    VkPipeline h;              /* the app's handle */
    int refs;                  /* the map's, and each command buffer's that has it bound */
    uint8_t dyn;               /* DYN_* */
    uint8_t can_vary;          /* variants can be built (a create info is kept) */
    uint8_t writes_vp;         /* the flipped stage writes ViewportIndex */
    uint32_t vcount;           /* pViewportState->viewportCount (DYN_VIEWPORT) */
    variant var[MAX_VARIANTS]; /* var[0]: the app's handle */
    uint32_t nvar;
    /* the kept create info: encoded with inline modules emptied; mods[s] the stage's code */
    uint8_t *enc;
    size_t enc_len;
    blob *mods[MAX_STAGES];
    uint32_t nstages;
    int flip_stage;
    uint32_t flip_model;
    char entry[64];
    VkRenderPass rp;
    VkPipelineLayout layout;
    /* graphics pipeline library: a pre-rasterization library's record is flipped like a whole
     * pipeline; a pipeline linked from one takes its mask, and a variant of it is linked again
     * with the library's variant (the libraries it names kept alive for that) */
    uint8_t preraster_lib;
    struct pipe_rec *lib;      /* the pre-rasterization library linked (a ref) */
    uint64_t *libs;            /* every library it names (kept) */
    uint32_t nlibs;
    pthread_mutex_t lock;
    struct pipe_rec *next;
} pipe_rec;

typedef struct cb_rec {
    VkCommandBuffer cb;
    VkCommandPool pool;
    pipe_rec *pipe;          /* the bound graphics pipeline's record (a ref), NULL: none to vary */
    VkPipeline bound;        /* what the driver has bound */
    VkViewport vp[MAX_VP];   /* as the app set them */
    uint32_t neg;            /* vp[i].height < 0 */
    uint32_t rw;             /* the driver has vp[i] mirrored, (y + h, -h) */
    uint32_t count;          /* vkCmdSetViewportWithCount's */
    uint32_t nset;           /* viewports 0..nset-1 set */
    int with_count;          /* the app's last set was vkCmdSetViewportWithCount */
    struct cb_rec *next;
} cb_rec;

typedef struct vkb_errata_flip {
    VkDevice device;
    /* what the fix's wrappers call: t->real as it was when the fix was installed */
    PFN_vkCreateShaderModule mod;
    PFN_vkDestroyShaderModule dmod;
    PFN_vkCreateGraphicsPipelines gfx;
    PFN_vkDestroyPipeline dpipe;
    PFN_vkDestroyRenderPass drp;
    PFN_vkDestroyPipelineLayout dpl;
    PFN_vkAllocateCommandBuffers acb;
    PFN_vkFreeCommandBuffers fcb;
    PFN_vkBeginCommandBuffer bcb;
    PFN_vkDestroyCommandPool dpool;
    PFN_vkCmdBindPipeline bind;
    PFN_vkCmdSetViewport vp;
    PFN_vkCmdSetViewportWithCount vpc;
    PFN_vkCmdDraw draw;
    PFN_vkCmdDrawIndexed draw_idx;
    PFN_vkCmdDrawIndirect draw_ind;
    PFN_vkCmdDrawIndexedIndirect draw_idx_ind;
    PFN_vkCmdDrawIndirectCount draw_ind_cnt;
    PFN_vkCmdDrawIndexedIndirectCount draw_idx_ind_cnt;
    PFN_vkCmdDrawIndirectByteCountEXT draw_bytes;
    PFN_vkCmdDrawMultiEXT draw_multi;
    PFN_vkCmdDrawMultiIndexedEXT draw_multi_idx;
    /* what the imitation's call */
    PFN_vkCreateGraphicsPipelines imi_gfx;
    PFN_vkCmdSetViewport imi_vp;
    PFN_vkCmdSetViewportWithCount imi_vpc;
    int fix, imitate;

    pthread_mutex_t lock; /* the maps */
    blob *blobs[BUCKETS];
    mod_rec *mods[BUCKETS];
    pipe_rec *pipes[BUCKETS];
    obj_ref *objs[BUCKETS];
    cb_rec *cbs[BUCKETS];
    size_t retained, retain_cap;
    /* the masks draws used last: the guess for a new dynamic-viewport pipeline */
    uint32_t last_key0, last_key_vp;

    /* counts */
    unsigned long p_static, p_dynamic, p_linked, p_flipped, p_left, p_failed, variants, variant_failed, viewports, rebinds, reemits,
        imitated_vp, imitated_pipes, logged;
    uint64_t flip_us, variant_us;
} vkb_errata_flip;

static vkb_errata_flip *last_state;
static unsigned cb_gen; /* bumped when a cb record goes: the per-thread cache is checked against it */

static vkb_errata_flip *state(void)
{
    vkb_errata_dev *e = vkb_errata_cur();
    return e && e->flip ? e->flip : __atomic_load_n(&last_state, __ATOMIC_ACQUIRE);
}

static unsigned hb(uint64_t k)
{
    k ^= k >> 33;
    k *= 0xff51afd7ed558ccdull;
    k ^= k >> 33;
    return (unsigned)k & (BUCKETS - 1);
}

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static void add_ms(uint64_t *us, double ms)
{
    __atomic_add_fetch(us, (uint64_t)(ms * 1e3), __ATOMIC_RELAXED);
}

/* Only modules with a stage that can feed the rasterizer, or a fragment stage (a variant needs
 * every stage of its pipeline), are kept: not compute-only ones. */
static int graphics_module(const uint32_t *code, size_t size)
{
    if (size < 20 || (size & 3) || code[0] != 0x07230203u) return 0;
    for (size_t pos = 5, words = size / 4; pos < words;) {
        uint32_t n = code[pos] >> 16, op = code[pos] & 0xffff;
        if (!n || pos + n > words) return 0;
        if (op == SpvOpEntryPoint && n > 1 && code[pos + 1] != SpvExecutionModelGLCompute) return 1;
        if (op == SpvOpFunction) return 0;
        pos += n;
    }
    return 0;
}

static uint64_t fnv1a(const void *p, size_t n)
{
    uint64_t h = 0xcbf29ce484222325ull;
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 0x100000001b3ull;
    return h;
}

/* ------------------------------------------------------------------ the maps (s->lock held) */

static blob *blob_get_locked(vkb_errata_flip *s, const uint32_t *code, size_t size)
{
    uint64_t h = fnv1a(code, size);
    unsigned k = hb(h);
    for (blob *b = s->blobs[k]; b; b = b->next)
        if (b->hash == h && b->size == size && !memcmp(b->code, code, size)) {
            b->refs++;
            return b;
        }
    blob *b = calloc(1, sizeof(*b));
    if (!b || !(b->code = malloc(size))) {
        free(b);
        return NULL;
    }
    memcpy(b->code, code, size);
    b->hash = h, b->size = size, b->refs = 1;
    b->next = s->blobs[k];
    s->blobs[k] = b;
    s->retained += size;
    return b;
}

static void blob_put_locked(vkb_errata_flip *s, blob *b)
{
    if (!b || --b->refs > 0) return;
    for (blob **pp = &s->blobs[hb(b->hash)]; *pp; pp = &(*pp)->next)
        if (*pp == b) {
            *pp = b->next;
            break;
        }
    s->retained -= b->size;
    free(b->code);
    free(b);
}

static void obj_ref_locked(vkb_errata_flip *s, uint64_t h, int kind)
{
    if (!h) return;
    unsigned k = hb(h);
    for (obj_ref *o = s->objs[k]; o; o = o->next)
        if (o->h == h && o->kind == kind) {
            o->refs++;
            return;
        }
    obj_ref *o = calloc(1, sizeof(*o));
    if (!o) return;
    o->h = h, o->kind = kind, o->refs = 1;
    o->next = s->objs[k];
    s->objs[k] = o;
}

/* 1: the caller destroys it now (the last ref went and the app had destroyed it) */
static int obj_unref_locked(vkb_errata_flip *s, uint64_t h, int kind)
{
    if (!h) return 0;
    for (obj_ref **pp = &s->objs[hb(h)]; *pp; pp = &(*pp)->next) {
        obj_ref *o = *pp;
        if (o->h != h || o->kind != kind) continue;
        if (--o->refs > 0) return 0;
        int pending = o->pending;
        *pp = o->next;
        free(o);
        return pending;
    }
    return 0;
}

/* the app destroys an object: 1 when it is kept for now */
static int obj_defer(vkb_errata_flip *s, uint64_t h, int kind)
{
    int kept = 0;
    pthread_mutex_lock(&s->lock);
    for (obj_ref *o = s->objs[hb(h)]; o; o = o->next)
        if (o->h == h && o->kind == kind) o->pending = kept = 1;
    pthread_mutex_unlock(&s->lock);
    return kept;
}

static void obj_destroy(vkb_errata_flip *s, uint64_t h, int kind)
{
    if (kind == OBJ_RENDER_PASS) s->drp(s->device, (VkRenderPass)h, NULL);
    else if (kind == OBJ_LAYOUT) s->dpl(s->device, (VkPipelineLayout)h, NULL);
    else s->dpipe(s->device, (VkPipeline)h, NULL);
}

/* The last ref goes (the app destroyed the pipeline, no command buffer has it bound, no linked
 * pipeline names it): its variants and what it kept go too. */
static void pipe_put(vkb_errata_flip *s, pipe_rec *r)
{
    if (!r || __atomic_sub_fetch(&r->refs, 1, __ATOMIC_ACQ_REL) > 0) return;
    for (uint32_t k = 1; k < r->nvar; k++)
        if (r->var[k].p) s->dpipe(s->device, r->var[k].p, NULL);
    pthread_mutex_lock(&s->lock);
    for (uint32_t k = 0; k < r->nstages; k++) blob_put_locked(s, r->mods[k]);
    int drp = obj_unref_locked(s, (uint64_t)r->rp, OBJ_RENDER_PASS);
    int dpl = obj_unref_locked(s, (uint64_t)r->layout, OBJ_LAYOUT);
    uint32_t ndl = 0;
    for (uint32_t k = 0; k < r->nlibs; k++)
        if (obj_unref_locked(s, r->libs[k], OBJ_PIPELINE)) r->libs[ndl++] = r->libs[k];
    if (r->enc) s->retained -= r->enc_len;
    pthread_mutex_unlock(&s->lock);
    if (drp) obj_destroy(s, (uint64_t)r->rp, OBJ_RENDER_PASS);
    if (dpl) obj_destroy(s, (uint64_t)r->layout, OBJ_LAYOUT);
    for (uint32_t k = 0; k < ndl; k++) obj_destroy(s, r->libs[k], OBJ_PIPELINE);
    pipe_put(s, r->lib);
    free(r->libs);
    free(r->enc);
    pthread_mutex_destroy(&r->lock);
    free(r);
}

static pipe_rec *pipe_find_ref(vkb_errata_flip *s, VkPipeline p)
{
    pthread_mutex_lock(&s->lock);
    pipe_rec *r = s->pipes[hb((uint64_t)p)];
    while (r && r->h != p) r = r->next;
    if (r) __atomic_add_fetch(&r->refs, 1, __ATOMIC_ACQ_REL);
    pthread_mutex_unlock(&s->lock);
    return r;
}

/* The command buffer's record (made when `make`); a per-thread cache spares the lock on draws. */
static __thread struct {
    vkb_errata_flip *s;
    VkCommandBuffer cb;
    cb_rec *c;
    unsigned gen;
} tc;

static cb_rec *cb_find(vkb_errata_flip *s, VkCommandBuffer cb, int make)
{
    unsigned gen = __atomic_load_n(&cb_gen, __ATOMIC_ACQUIRE);
    if (tc.s == s && tc.cb == cb && tc.gen == gen && tc.c) return tc.c;
    pthread_mutex_lock(&s->lock);
    unsigned k = hb((uint64_t)(uintptr_t)cb);
    cb_rec *c = s->cbs[k];
    while (c && c->cb != cb) c = c->next;
    if (!c && make && (c = calloc(1, sizeof(*c)))) {
        c->cb = cb;
        c->next = s->cbs[k];
        s->cbs[k] = c;
    }
    pthread_mutex_unlock(&s->lock);
    if (c) tc.s = s, tc.cb = cb, tc.c = c, tc.gen = gen;
    return c;
}

static void cb_reset(vkb_errata_flip *s, cb_rec *c)
{
    pipe_rec *p = c->pipe;
    VkCommandBuffer cb = c->cb;
    VkCommandPool pool = c->pool;
    cb_rec *next = c->next;
    memset(c, 0, sizeof(*c));
    c->cb = cb, c->pool = pool, c->next = next;
    pipe_put(s, p);
}

/* removes the records of these command buffers (pool: all of the pool's when n == 0) */
static void cb_drop(vkb_errata_flip *s, VkCommandPool pool, uint32_t n, const VkCommandBuffer *cbs)
{
    cb_rec *gone = NULL;
    pthread_mutex_lock(&s->lock);
    if (n) {
        for (uint32_t i = 0; i < n; i++) {
            if (!cbs[i]) continue;
            for (cb_rec **pp = &s->cbs[hb((uint64_t)(uintptr_t)cbs[i])]; *pp; pp = &(*pp)->next)
                if ((*pp)->cb == cbs[i]) {
                    cb_rec *c = *pp;
                    *pp = c->next;
                    c->next = gone, gone = c;
                    break;
                }
        }
    } else {
        for (unsigned k = 0; k < BUCKETS; k++)
            for (cb_rec **pp = &s->cbs[k]; *pp;) {
                cb_rec *c = *pp;
                if (c->pool != pool) {
                    pp = &c->next;
                    continue;
                }
                *pp = c->next;
                c->next = gone, gone = c;
            }
    }
    __atomic_add_fetch(&cb_gen, 1, __ATOMIC_ACQ_REL);
    pthread_mutex_unlock(&s->lock);
    while (gone) {
        cb_rec *c = gone;
        gone = c->next;
        pipe_put(s, c->pipe);
        free(c);
    }
}

/* ------------------------------------------------------------------ helpers */

static void rewrite_vp(VkViewport *v)
{
    v->y += v->height;
    v->height = -v->height;
}

static int dyn_kind(const VkGraphicsPipelineCreateInfo *ci)
{
    int k = DYN_NONE;
    if (!ci->pDynamicState) return k;
    for (uint32_t i = 0; i < ci->pDynamicState->dynamicStateCount; i++) {
        VkDynamicState d = ci->pDynamicState->pDynamicStates[i];
        if (d == VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT) return DYN_WITH_COUNT;
        if (d == VK_DYNAMIC_STATE_VIEWPORT) k = DYN_VIEWPORT;
    }
    return k;
}

static const void *chain_find(const void *pNext, VkStructureType t)
{
    for (const VkBaseInStructure *b = pNext; b; b = b->pNext)
        if (b->sType == t) return b;
    return NULL;
}

static uint32_t model_of(VkShaderStageFlagBits st)
{
    switch (st) {
    case VK_SHADER_STAGE_VERTEX_BIT: return SpvExecutionModelVertex;
    case VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT: return SpvExecutionModelTessellationEvaluation;
    case VK_SHADER_STAGE_GEOMETRY_BIT: return SpvExecutionModelGeometry;
    default: return ~0u;
    }
}

/* The last pre-rasterization stage, or -1 */
static int last_stage(const VkGraphicsPipelineCreateInfo *ci)
{
    int best = -1, rank = 0;
    for (uint32_t i = 0; ci->pStages && i < ci->stageCount; i++) {
        int r = ci->pStages[i].stage == VK_SHADER_STAGE_GEOMETRY_BIT ? 3
              : ci->pStages[i].stage == VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT ? 2
              : ci->pStages[i].stage == VK_SHADER_STAGE_VERTEX_BIT ? 1 : 0;
        if (r > rank) rank = r, best = (int)i;
    }
    return best;
}

/* A stage's code: its module's (kept at vkCreateShaderModule) or its inline module's */
static const uint32_t *stage_code(vkb_errata_flip *s, const VkPipelineShaderStageCreateInfo *st, size_t *size,
                                  VkShaderModuleCreateInfo **inl)
{
    *inl = NULL;
    if (st->module) {
        pthread_mutex_lock(&s->lock);
        mod_rec *m = s->mods[hb((uint64_t)st->module)];
        while (m && m->h != st->module) m = m->next;
        const uint32_t *c = m ? m->b->code : NULL;
        *size = m ? m->b->size : 0;
        pthread_mutex_unlock(&s->lock);
        return c; /* the module lives for the call */
    }
    VkShaderModuleCreateInfo *mi = (VkShaderModuleCreateInfo *)chain_find(st->pNext, VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
    if (!mi) return NULL;
    *inl = mi;
    *size = mi->codeSize;
    return mi->pCode;
}

/* The mask a pipeline is built for, of the n viewports in use: bit i flips ViewportIndex i. A
 * stage that does not write ViewportIndex reaches viewport 0 only (0 or 1); every viewport in use
 * negative is KEY_ALL whatever their number, a plain negation (DXVK's 1 and 16 viewports alike). */
#define KEY_ALL 0xffffffffu
static uint32_t key_of(uint32_t neg, uint32_t n, int writes_vp)
{
    uint32_t used = n >= 32 ? 0xffffffffu : (1u << n) - 1;
    if (!writes_vp) return neg & used & 1;
    return n && (neg & used) == used ? KEY_ALL : neg & used;
}

static void log_variant(vkb_errata_flip *s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void log_variant(vkb_errata_flip *s, const char *fmt, ...)
{
    unsigned long n = __atomic_fetch_add(&s->logged, 1, __ATOMIC_RELAXED);
    int level = n < LOG_INFO_MAX ? VKB_LOG_INFO : VKB_LOG_DEBUG;
    if (level > vkb_log_level()) return;
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    vkb_log(level, "errata: viewport_flip: %s%s", msg, n == LOG_INFO_MAX - 1 ? " (further ones at debug level)" : "");
}

/* A temporary driver module with this stage's code flipped for `key`; VK_NULL_HANDLE when there
 * is nothing to flip (*fail: it could not be done). For an inline stage *inl_code gets the code. */
static VkShaderModule flipped_module(vkb_errata_flip *s, VkDevice d, const uint32_t *code, size_t size, uint32_t model,
                                     const char *entry, uint32_t key, int inline_stage, uint32_t **inl_code,
                                     size_t *inl_size, int *fail)
{
    *fail = 0;
    uint32_t *out = NULL;
    size_t out_size = 0;
    vkb_flip_info info;
    const char *why = NULL;
    double t0 = now_ms();
    int r = vkb_spv_flip(code, size, model, entry, key, 32, &out, &out_size, &info, &why);
    add_ms(&s->flip_us, now_ms() - t0);
    if (r <= 0) {
        if (r < 0) {
            *fail = 1;
            VKB_WARN("errata: viewport_flip: module h-%016llx (%s) left as it is: %s; with this driver its picture is upside down",
                     (unsigned long long)vkb_spv_app_hash(code, size), entry, why);
        }
        return VK_NULL_HANDLE;
    }
    if (info.xfb) VKB_ONCE(VKB_LOG_WARN, "errata: viewport_flip: a stage with transform feedback: a captured gl_Position is captured flipped (logged once)");
    vkb_errata_spv_origin(out, out_size, code, size);
    if (inline_stage) {
        *inl_code = out, *inl_size = out_size;
        return VK_NULL_HANDLE;
    }
    VkShaderModuleCreateInfo mci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, out_size, out};
    VkShaderModule m = VK_NULL_HANDLE;
    if (s->mod(d, &mci, NULL, &m) != VK_SUCCESS) {
        *fail = 1;
        m = VK_NULL_HANDLE;
    }
    free(out);
    return m;
}

/* ------------------------------------------------------------------ shader modules */

static VKAPI_ATTR VkResult VKAPI_CALL fl_CreateShaderModule(VkDevice d, const VkShaderModuleCreateInfo *ci, const VkAllocationCallbacks *a,
                                                            VkShaderModule *out)
{
    vkb_errata_flip *s = state();
    VkResult r = s->mod(d, ci, a, out);
    if (r != VK_SUCCESS || !ci->pCode || !graphics_module(ci->pCode, ci->codeSize)) return r;
    /* every stage's code is kept while the module lives: a variant may need any of them */
    mod_rec *m = calloc(1, sizeof(*m));
    if (!m) return r;
    m->h = *out;
    pthread_mutex_lock(&s->lock);
    m->b = blob_get_locked(s, ci->pCode, ci->codeSize);
    if (m->b) {
        unsigned k = hb((uint64_t)*out);
        m->next = s->mods[k];
        s->mods[k] = m;
    }
    pthread_mutex_unlock(&s->lock);
    if (!m->b) free(m);
    return r;
}

static VKAPI_ATTR void VKAPI_CALL fl_DestroyShaderModule(VkDevice d, VkShaderModule h, const VkAllocationCallbacks *a)
{
    vkb_errata_flip *s = state();
    if (h) {
        pthread_mutex_lock(&s->lock);
        for (mod_rec **pp = &s->mods[hb((uint64_t)h)]; *pp; pp = &(*pp)->next)
            if ((*pp)->h == h) {
                mod_rec *m = *pp;
                *pp = m->next;
                blob_put_locked(s, m->b);
                free(m);
                break;
            }
        pthread_mutex_unlock(&s->lock);
    }
    s->dmod(d, h, a);
}

/* ------------------------------------------------------------------ pipelines */

typedef struct plan {
    pipe_rec *lib;            /* linked from a pre-rasterization library with a record (a ref) */
    int preraster_lib;        /* this is a pre-rasterization library */
    int stage;                /* the flipped stage, -1: none */
    uint32_t model;
    int writes_vp, can, dyn;  /* can: a variant can be built */
    uint32_t key, vcount;
    VkShaderModule temp;
    VkShaderModuleCreateInfo *inl;
    const uint32_t *inl_code0;
    size_t inl_size0;
    uint32_t *inl_code;
    VkPipelineShaderStageCreateInfo *stages;
    VkPipelineViewportStateCreateInfo vps;
    VkViewport *vps_vp;
} plan;

/* What one create info needs; fills c (a copy of ci) with what the driver gets. */
static void plan_one(vkb_errata_flip *s, VkDevice d, const VkGraphicsPipelineCreateInfo *ci, VkGraphicsPipelineCreateInfo *c, plan *p)
{
    memset(p, 0, sizeof(*p));
    p->stage = -1;
    p->dyn = dyn_kind(ci);
    const VkPipelineCreateFlags2CreateInfo *f2 = chain_find(ci->pNext, VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO);
    int is_lib = (ci->flags & VK_PIPELINE_CREATE_LIBRARY_BIT_KHR) || (f2 && (f2->flags & VK_PIPELINE_CREATE_2_LIBRARY_BIT_KHR));
    const VkGraphicsPipelineLibraryCreateInfoEXT *gl = chain_find(ci->pNext, VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT);
    const VkPipelineLibraryCreateInfoKHR *links = chain_find(ci->pNext, VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR);
    for (uint32_t i = 0; links && i < links->libraryCount && !p->lib; i++) {
        pipe_rec *l = pipe_find_ref(s, links->pLibraries[i]);
        if (l && l->preraster_lib) p->lib = l;
        else pipe_put(s, l);
    }
    if (p->lib) {
        /* linked: the library was flipped for its mask, the pipeline draws with it */
        p->dyn = p->lib->dyn;
        p->vcount = p->lib->vcount;
        p->writes_vp = p->lib->writes_vp;
        p->key = p->lib->var[0].key;
        p->can = p->lib->can_vary;
        return;
    }
    if (is_lib && gl && !(gl->flags & VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT)) {
        p->dyn = DYN_NONE; /* vertex input, fragment shader or output: nothing here moves */
        return;
    }
    p->preraster_lib = is_lib;
    int st = last_stage(ci);
    if (st < 0) return;
    const VkPipelineShaderStageCreateInfo *sst = &ci->pStages[st];
    if (chain_find(sst->pNext, VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_MODULE_IDENTIFIER_CREATE_INFO_EXT)) {
        VKB_ONCE(VKB_LOG_WARN, "errata: viewport_flip: a stage given by module identifier is left as it is (logged once)");
        return;
    }
    size_t size = 0;
    VkShaderModuleCreateInfo *inl;
    const uint32_t *code = stage_code(s, sst, &size, &inl);
    const char *entry = sst->pName ? sst->pName : "main";
    vkb_flip_info info;
    if (!code || vkb_spv_flip_info(code, size, model_of(sst->stage), entry, &info, NULL) || !info.position) return;
    p->stage = st;
    p->model = model_of(sst->stage);
    p->writes_vp = info.writes_vpindex;
    p->can = 1;
    uint32_t neg = 0, n = 0;
    if (p->dyn == DYN_NONE) {
        const VkPipelineViewportStateCreateInfo *v = ci->pViewportState;
        if (!v || !v->pViewports) return;
        n = v->viewportCount < MAX_VP ? v->viewportCount : MAX_VP;
        for (uint32_t i = 0; i < n; i++)
            if (v->pViewports[i].height < 0) neg |= 1u << i;
        p->key = key_of(neg, n, p->writes_vp);
        if (!p->key) return;
        /* the rewritten viewports: those the shader flips */
        p->vps_vp = malloc(v->viewportCount * sizeof(VkViewport));
        if (!p->vps_vp) {
            p->key = 0;
            return;
        }
        memcpy(p->vps_vp, v->pViewports, v->viewportCount * sizeof(VkViewport));
        for (uint32_t i = 0; i < n; i++)
            if (p->key >> i & 1) rewrite_vp(&p->vps_vp[i]);
        p->vps = *v;
        p->vps.pViewports = p->vps_vp;
    } else {
        p->vcount = ci->pViewportState ? ci->pViewportState->viewportCount : 1;
        n = p->dyn == DYN_WITH_COUNT ? MAX_VP : p->vcount;
        p->key = key_of(p->writes_vp ? __atomic_load_n(&s->last_key_vp, __ATOMIC_RELAXED) : __atomic_load_n(&s->last_key0, __ATOMIC_RELAXED),
                        n, p->writes_vp);
        if (!p->key) return;
    }
    int fail;
    uint32_t *icode = NULL;
    size_t isize = 0;
    VkShaderModule m = flipped_module(s, d, code, size, p->model, entry, p->key, inl != NULL, &icode, &isize, &fail);
    if (fail || (!m && !icode)) {
        free(p->vps_vp);
        p->vps_vp = NULL;
        p->key = 0;
        p->can = 0;
        __atomic_add_fetch(&s->p_failed, 1, __ATOMIC_RELAXED);
        return;
    }
    p->stages = malloc(ci->stageCount * sizeof(*p->stages));
    if (!p->stages) {
        if (m) s->dmod(d, m, NULL);
        free(icode);
        free(p->vps_vp);
        p->vps_vp = NULL;
        p->key = 0;
        return;
    }
    memcpy(p->stages, ci->pStages, ci->stageCount * sizeof(*p->stages));
    if (m) {
        p->temp = m;
        p->stages[st].module = m;
    } else {
        /* an inline module: its code swapped for the call (and put back: it is a layer's) */
        p->inl = inl, p->inl_code0 = inl->pCode, p->inl_size0 = inl->codeSize, p->inl_code = icode;
        inl->pCode = icode;
        inl->codeSize = isize;
    }
    c->pStages = p->stages;
    if (p->vps_vp) c->pViewportState = &p->vps;
    __atomic_add_fetch(&s->p_flipped, 1, __ATOMIC_RELAXED);
}

static void plan_done(vkb_errata_flip *s, VkDevice d, plan *p)
{
    if (p->temp) s->dmod(d, p->temp, NULL);
    if (p->inl) {
        p->inl->pCode = p->inl_code0;
        p->inl->codeSize = p->inl_size0;
        free(p->inl_code);
    }
    free(p->stages);
    free(p->vps_vp);
}

/* A dynamic-viewport pipeline's record, with what a variant needs. */
static void record(vkb_errata_flip *s, const VkGraphicsPipelineCreateInfo *ci, plan *p, VkPipeline h)
{
    pipe_rec *r = calloc(1, sizeof(*r));
    if (!r) return;
    r->lib = p->lib; /* the plan's ref, now the record's */
    p->lib = NULL;
    r->h = h;
    r->refs = 1;
    r->dyn = (uint8_t)p->dyn;
    r->writes_vp = (uint8_t)p->writes_vp;
    r->vcount = p->vcount;
    r->var[0] = (variant){p->key, h, 0};
    r->nvar = 1;
    r->flip_stage = p->stage;
    r->flip_model = p->model;
    r->preraster_lib = (uint8_t)p->preraster_lib;
    pthread_mutex_init(&r->lock, NULL);
    int keep = p->can && ci->stageCount <= MAX_STAGES;
    if (keep && __atomic_load_n(&s->retained, __ATOMIC_RELAXED) >= s->retain_cap) {
        VKB_ONCE(VKB_LOG_WARN, "errata: viewport_flip: more than VKBRIDGE_FLIP_RETAIN_MB of SPIR-V kept for variants; new pipelines keep the "
                               "viewport signs they were made for (logged once)");
        keep = 0;
    }
    if (keep && p->stage >= 0) {
        const char *entry = ci->pStages[p->stage].pName ? ci->pStages[p->stage].pName : "main";
        snprintf(r->entry, sizeof(r->entry), "%s", entry);
        if (strlen(entry) >= sizeof(r->entry)) keep = 0;
    }
    if (keep) {
        /* each stage's code, shared by content; inline modules emptied for the encoding */
        VkShaderModuleCreateInfo *inl[MAX_STAGES] = {0};
        const uint32_t *code0[MAX_STAGES];
        size_t size0[MAX_STAGES];
        pthread_mutex_lock(&s->lock);
        for (uint32_t k = 0; k < ci->stageCount && keep; k++) {
            const VkPipelineShaderStageCreateInfo *st = &ci->pStages[k];
            const uint32_t *code = NULL;
            size_t size = 0;
            if (st->module) {
                mod_rec *m = s->mods[hb((uint64_t)st->module)];
                while (m && m->h != st->module) m = m->next;
                if (m) code = m->b->code, size = m->b->size;
            } else if ((inl[k] = (VkShaderModuleCreateInfo *)chain_find(st->pNext, VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO))) {
                code = inl[k]->pCode, size = inl[k]->codeSize;
            }
            if (!code || !(r->mods[k] = blob_get_locked(s, code, size))) keep = 0;
            r->nstages = k + 1;
        }
        pthread_mutex_unlock(&s->lock);
        if (keep) {
            for (uint32_t k = 0; k < ci->stageCount; k++)
                if (inl[k]) code0[k] = inl[k]->pCode, size0[k] = inl[k]->codeSize, inl[k]->pCode = NULL, inl[k]->codeSize = 0;
            vkb_enc e;
            vkb_enc_init(&e);
            vkb_enc_VkGraphicsPipelineCreateInfo(&e, ci);
            for (uint32_t k = 0; k < ci->stageCount; k++)
                if (inl[k]) inl[k]->pCode = code0[k], inl[k]->codeSize = size0[k];
            if (!e.oom && (r->enc = malloc(e.len))) {
                memcpy(r->enc, e.buf, e.len);
                r->enc_len = e.len;
            } else {
                keep = 0;
            }
            vkb_enc_free(&e);
        }
    }
    const VkPipelineLibraryCreateInfoKHR *links = chain_find(ci->pNext, VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR);
    if (keep && links && links->libraryCount && !(r->libs = malloc(links->libraryCount * sizeof(*r->libs)))) keep = 0;
    pthread_mutex_lock(&s->lock);
    if (keep) {
        r->can_vary = 1;
        r->rp = ci->renderPass;
        r->layout = ci->layout;
        obj_ref_locked(s, (uint64_t)r->rp, OBJ_RENDER_PASS);
        obj_ref_locked(s, (uint64_t)r->layout, OBJ_LAYOUT);
        for (uint32_t k = 0; links && k < links->libraryCount; k++) {
            r->libs[r->nlibs++] = (uint64_t)links->pLibraries[k];
            obj_ref_locked(s, (uint64_t)links->pLibraries[k], OBJ_PIPELINE);
        }
        s->retained += r->enc_len;
    } else {
        for (uint32_t k = 0; k < r->nstages; k++) blob_put_locked(s, r->mods[k]);
        memset(r->mods, 0, sizeof(r->mods));
        r->nstages = 0;
    }
    unsigned k = hb((uint64_t)h);
    r->next = s->pipes[k];
    s->pipes[k] = r;
    pthread_mutex_unlock(&s->lock);
}

static VKAPI_ATTR VkResult VKAPI_CALL fl_CreateGraphicsPipelines(VkDevice d, VkPipelineCache cache, uint32_t n,
                                                                 const VkGraphicsPipelineCreateInfo *ci, const VkAllocationCallbacks *a,
                                                                 VkPipeline *out)
{
    vkb_errata_flip *s = state();
    VkGraphicsPipelineCreateInfo *c = malloc(n * sizeof(*c));
    plan *p = calloc(n, sizeof(*p));
    if (!c || !p) {
        free(c);
        free(p);
        return s->gfx(d, cache, n, ci, a, out);
    }
    memcpy(c, ci, n * sizeof(*c));
    for (uint32_t i = 0; i < n; i++) plan_one(s, d, &ci[i], &c[i], &p[i]);
    VkResult r = s->gfx(d, cache, n, c, a, out);
    for (uint32_t i = 0; i < n; i++) {
        plan_done(s, d, &p[i]);
        if (out[i] && p[i].dyn == DYN_NONE) __atomic_add_fetch(&s->p_static, 1, __ATOMIC_RELAXED);
        if (out[i] && p[i].dyn != DYN_NONE) {
            __atomic_add_fetch(&s->p_dynamic, 1, __ATOMIC_RELAXED);
            if (p[i].lib) __atomic_add_fetch(&s->p_linked, 1, __ATOMIC_RELAXED);
            record(s, &ci[i], &p[i], out[i]);
        }
        pipe_put(s, p[i].lib);
    }
    free(c);
    free(p);
    return r;
}

static VKAPI_ATTR void VKAPI_CALL fl_DestroyPipeline(VkDevice d, VkPipeline h, const VkAllocationCallbacks *a)
{
    vkb_errata_flip *s = state();
    pipe_rec *r = NULL;
    if (h) {
        pthread_mutex_lock(&s->lock);
        for (pipe_rec **pp = &s->pipes[hb((uint64_t)h)]; *pp; pp = &(*pp)->next)
            if ((*pp)->h == h) {
                r = *pp;
                *pp = r->next;
                break;
            }
        pthread_mutex_unlock(&s->lock);
    }
    if (r) {
        /* no more variants of a pipeline the app is done with (a library's may still be linked) */
        if (!r->preraster_lib) {
            pthread_mutex_lock(&r->lock);
            r->can_vary = 0;
            pthread_mutex_unlock(&r->lock);
        }
        pipe_put(s, r); /* its variants go with the last ref */
    }
    if (h && obj_defer(s, (uint64_t)h, OBJ_PIPELINE)) return; /* a library a kept create info links */
    s->dpipe(d, h, a);
}

static VKAPI_ATTR void VKAPI_CALL fl_DestroyRenderPass(VkDevice d, VkRenderPass h, const VkAllocationCallbacks *a)
{
    vkb_errata_flip *s = state();
    if (h && obj_defer(s, (uint64_t)h, OBJ_RENDER_PASS)) return;
    s->drp(d, h, a);
}

static VKAPI_ATTR void VKAPI_CALL fl_DestroyPipelineLayout(VkDevice d, VkPipelineLayout h, const VkAllocationCallbacks *a)
{
    vkb_errata_flip *s = state();
    if (h && obj_defer(s, (uint64_t)h, OBJ_LAYOUT)) return;
    s->dpl(d, h, a);
}

static VkPipeline variant_get(vkb_errata_flip *s, pipe_rec *r, uint32_t key);

/* A variant of a dynamic-viewport pipeline for `key`, built from the kept create info. */
static VkPipeline build_variant(vkb_errata_flip *s, pipe_rec *r, uint32_t key)
{
    vkb_arena ar = {0};
    vkb_dec dec;
    vkb_dec_init(&dec, r->enc, r->enc_len, &ar, NULL, 0);
    VkGraphicsPipelineCreateInfo c;
    vkb_dec_VkGraphicsPipelineCreateInfo(&dec, &c);
    VkPipeline out = VK_NULL_HANDLE;
    VkShaderModule temps[MAX_STAGES] = {0};
    uint32_t *flipped = NULL;
    if (dec.err || c.stageCount != r->nstages) goto done;
    /* a pipeline of its own: no derivative, no early return, no feedback into the app's memory */
    c.flags &= ~(VkPipelineCreateFlags)(VK_PIPELINE_CREATE_DERIVATIVE_BIT | VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT |
                                        VK_PIPELINE_CREATE_EARLY_RETURN_ON_FAILURE_BIT);
    c.basePipelineHandle = VK_NULL_HANDLE;
    c.basePipelineIndex = -1;
    for (VkBaseOutStructure **pp = (VkBaseOutStructure **)&c.pNext; *pp;) {
        if ((*pp)->sType == VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO) {
            *pp = (*pp)->pNext;
            continue;
        }
        if ((*pp)->sType == VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO)
            ((VkPipelineCreateFlags2CreateInfo *)*pp)->flags &= ~(VkPipelineCreateFlags2)(
                VK_PIPELINE_CREATE_2_DERIVATIVE_BIT | VK_PIPELINE_CREATE_2_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT |
                VK_PIPELINE_CREATE_2_EARLY_RETURN_ON_FAILURE_BIT);
        pp = &(*pp)->pNext;
    }
    if (r->lib) {
        /* linked again, with the pre-rasterization library's variant for this mask */
        VkPipeline lv = key == r->lib->var[0].key ? r->lib->h : variant_get(s, r->lib, key);
        VkPipelineLibraryCreateInfoKHR *li = (VkPipelineLibraryCreateInfoKHR *)chain_find(c.pNext, VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR);
        VkPipeline *libs = li && li->libraryCount ? vkb_arena_alloc(&ar, li->libraryCount * sizeof(*libs)) : NULL;
        if (!lv || !libs) goto done;
        for (uint32_t k = 0; k < li->libraryCount; k++) libs[k] = li->pLibraries[k] == r->lib->h ? lv : li->pLibraries[k];
        li->pLibraries = libs;
    }
    VkPipelineShaderStageCreateInfo *st = (VkPipelineShaderStageCreateInfo *)c.pStages;
    for (uint32_t k = 0; k < c.stageCount; k++) {
        const uint32_t *code = r->mods[k]->code;
        size_t size = r->mods[k]->size;
        if ((int)k == r->flip_stage && key) {
            vkb_flip_info info;
            const char *why = NULL;
            double t0 = now_ms();
            int fr = vkb_spv_flip(code, size, r->flip_model, r->entry, key, 32, &flipped, &size, &info, &why);
            add_ms(&s->flip_us, now_ms() - t0);
            if (fr <= 0) goto done;
            vkb_errata_spv_origin(flipped, size, r->mods[k]->code, r->mods[k]->size);
            code = flipped;
        }
        VkShaderModuleCreateInfo *inl = (VkShaderModuleCreateInfo *)chain_find(st[k].pNext, VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
        if (st[k].module || !inl) {
            VkShaderModuleCreateInfo mci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, size, code};
            if (s->mod(s->device, &mci, NULL, &temps[k]) != VK_SUCCESS) goto done;
            st[k].module = temps[k];
        } else {
            inl->pCode = code;
            inl->codeSize = size;
        }
    }
    if (s->gfx(s->device, VK_NULL_HANDLE, 1, &c, NULL, &out) != VK_SUCCESS) out = VK_NULL_HANDLE;
done:
    for (uint32_t k = 0; k < MAX_STAGES; k++)
        if (temps[k]) s->dmod(s->device, temps[k], NULL);
    free(flipped);
    vkb_arena_free(&ar);
    return out;
}

static VkPipeline variant_get(vkb_errata_flip *s, pipe_rec *r, uint32_t key)
{
    pthread_mutex_lock(&r->lock);
    for (uint32_t k = 0; k < r->nvar; k++)
        if (r->var[k].key == key) {
            VkPipeline p = r->var[k].failed ? VK_NULL_HANDLE : r->var[k].p;
            pthread_mutex_unlock(&r->lock);
            return p;
        }
    VkPipeline p = VK_NULL_HANDLE;
    if (r->can_vary && r->nvar < MAX_VARIANTS) {
        double t0 = now_ms();
        p = build_variant(s, r, key);
        double ms = now_ms() - t0;
        add_ms(&s->variant_us, ms);
        r->var[r->nvar++] = (variant){key, p, !p};
        if (p) {
            __atomic_add_fetch(&s->variants, 1, __ATOMIC_RELAXED);
            log_variant(s, "pipeline %p: a variant for viewport mask 0x%x (the app's has 0x%x) compiled at draw time in %.1f ms", (void *)r->h,
                        key, r->var[0].key, ms);
        } else {
            __atomic_add_fetch(&s->variant_failed, 1, __ATOMIC_RELAXED);
            VKB_WARN("errata: viewport_flip: pipeline %p: the variant for viewport mask 0x%x could not be built; its draws keep mask 0x%x",
                     (void *)r->h, key, r->var[0].key);
        }
    } else if (r->can_vary) {
        VKB_ONCE(VKB_LOG_WARN, "errata: viewport_flip: a pipeline drawn with more than %d viewport masks; the rest use its own (logged once)",
                 MAX_VARIANTS);
    }
    pthread_mutex_unlock(&r->lock);
    return p;
}

/* ------------------------------------------------------------------ command buffers */

static VKAPI_ATTR VkResult VKAPI_CALL fl_AllocateCommandBuffers(VkDevice d, const VkCommandBufferAllocateInfo *ai, VkCommandBuffer *out)
{
    vkb_errata_flip *s = state();
    VkResult r = s->acb(d, ai, out);
    if (r != VK_SUCCESS) return r;
    for (uint32_t i = 0; i < ai->commandBufferCount; i++) {
        cb_rec *c = cb_find(s, out[i], 1);
        if (c) {
            cb_reset(s, c);
            c->pool = ai->commandPool;
        }
    }
    return r;
}

static VKAPI_ATTR void VKAPI_CALL fl_FreeCommandBuffers(VkDevice d, VkCommandPool pool, uint32_t n, const VkCommandBuffer *cbs)
{
    vkb_errata_flip *s = state();
    if (n) cb_drop(s, pool, n, cbs);
    s->fcb(d, pool, n, cbs);
}

static VKAPI_ATTR void VKAPI_CALL fl_DestroyCommandPool(VkDevice d, VkCommandPool pool, const VkAllocationCallbacks *a)
{
    vkb_errata_flip *s = state();
    if (pool) cb_drop(s, pool, 0, NULL);
    s->dpool(d, pool, a);
}

static VKAPI_ATTR VkResult VKAPI_CALL fl_BeginCommandBuffer(VkCommandBuffer cb, const VkCommandBufferBeginInfo *bi)
{
    vkb_errata_flip *s = state();
    cb_rec *c = cb_find(s, cb, 1);
    if (c) cb_reset(s, c);
    return s->bcb(cb, bi);
}

static VKAPI_ATTR void VKAPI_CALL fl_CmdBindPipeline(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipeline p)
{
    vkb_errata_flip *s = state();
    if (bp == VK_PIPELINE_BIND_POINT_GRAPHICS) {
        cb_rec *c = cb_find(s, cb, 1);
        if (c) {
            pipe_rec *old = c->pipe;
            c->pipe = pipe_find_ref(s, p);
            c->bound = p;
            pipe_put(s, old);
        }
    }
    s->bind(cb, bp, p);
}

/* the app's viewports first..first+n, negatives rewritten, into the record and to the driver */
static void set_viewports(vkb_errata_flip *s, VkCommandBuffer cb, int with_count, uint32_t first, uint32_t n, const VkViewport *v)
{
    cb_rec *c = cb_find(s, cb, 1);
    VkViewport tmp[MAX_VP], *w = n <= MAX_VP ? tmp : malloc(n * sizeof(*w));
    if (!w) {
        if (with_count) s->vpc(cb, n, v);
        else s->vp(cb, first, n, v);
        return;
    }
    memcpy(w, v, n * sizeof(*w));
    for (uint32_t i = 0; i < n; i++) {
        uint32_t k = first + i, bit = k < MAX_VP ? 1u << k : 0;
        if (c && bit) {
            c->vp[k] = v[i];
            c->neg = v[i].height < 0 ? c->neg | bit : c->neg & ~bit;
            c->rw &= ~bit;
        }
        if (v[i].height < 0 && bit) {
            rewrite_vp(&w[i]);
            if (c) c->rw |= bit;
            __atomic_add_fetch(&s->viewports, 1, __ATOMIC_RELAXED);
        }
    }
    if (c && with_count) c->count = n;
    if (c) {
        c->with_count = with_count;
        uint32_t top = first + n < MAX_VP ? first + n : MAX_VP;
        if (with_count) c->nset = top;
        else if (top > c->nset) c->nset = top;
    }
    if (with_count) s->vpc(cb, n, w);
    else s->vp(cb, first, n, w);
    if (w != tmp) free(w);
}

static VKAPI_ATTR void VKAPI_CALL fl_CmdSetViewport(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkViewport *v)
{
    set_viewports(state(), cb, 0, first, n, v);
}

static VKAPI_ATTR void VKAPI_CALL fl_CmdSetViewportWithCount(VkCommandBuffer cb, uint32_t n, const VkViewport *v)
{
    set_viewports(state(), cb, 1, 0, n, v);
}

/* Before a draw: the driver's viewports and the bound variant made to match the mask. */
static void prep(VkCommandBuffer cb)
{
    vkb_errata_flip *s = state();
    cb_rec *c = cb_find(s, cb, 0);
    if (!c) return;
    if (!c->pipe) {
        /* a pipeline without a record (static viewports, or made before): the app's viewports */
        if (c->rw && c->nset) {
            if (c->with_count) s->vpc(cb, c->nset, c->vp);
            else s->vp(cb, 0, c->nset, c->vp);
            c->rw = 0;
            __atomic_add_fetch(&s->reemits, 1, __ATOMIC_RELAXED);
        }
        return;
    }
    pipe_rec *p = c->pipe;
    uint32_t n = p->dyn == DYN_WITH_COUNT ? c->count : p->vcount;
    if (n > MAX_VP) n = MAX_VP;
    uint32_t used = (1u << n) - 1, key = key_of(c->neg, n, p->writes_vp);
    if (p->writes_vp) __atomic_store_n(&s->last_key_vp, key, __ATOMIC_RELAXED);
    else __atomic_store_n(&s->last_key0, key, __ATOMIC_RELAXED);
    VkPipeline v = key == p->var[0].key ? p->h : variant_get(s, p, key);
    if (!v) key = p->var[0].key, v = p->h; /* none for this mask: the pipeline's own */
    /* The driver gets mirrored exactly the viewports the bound variant flips: a negative one
     * becomes positive, and (with no variant for the mask) a positive one negative - right on a
     * correct driver either way, and on the G715 as wrong as without the fix. */
    if (!p->writes_vp) used &= 1;
    uint32_t want = key & used;
    if ((c->rw ^ want) & used) {
        VkViewport w[MAX_VP];
        for (uint32_t i = 0; i < n; i++) {
            w[i] = c->vp[i];
            if (want >> i & 1) rewrite_vp(&w[i]); /* (y + h, -h): its own inverse */
        }
        if (p->dyn == DYN_WITH_COUNT) s->vpc(cb, n, w);
        else s->vp(cb, 0, n, w);
        c->rw = (c->rw & ~used) | want;
        __atomic_add_fetch(&s->reemits, 1, __ATOMIC_RELAXED);
    }
    if (v != c->bound) {
        s->bind(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, v);
        c->bound = v;
        __atomic_add_fetch(&s->rebinds, 1, __ATOMIC_RELAXED);
    }
}

static VKAPI_ATTR void VKAPI_CALL fl_CmdDraw(VkCommandBuffer cb, uint32_t vc, uint32_t ic, uint32_t fv, uint32_t fi)
{
    prep(cb);
    state()->draw(cb, vc, ic, fv, fi);
}

static VKAPI_ATTR void VKAPI_CALL fl_CmdDrawIndexed(VkCommandBuffer cb, uint32_t xc, uint32_t ic, uint32_t fx, int32_t vo, uint32_t fi)
{
    prep(cb);
    state()->draw_idx(cb, xc, ic, fx, vo, fi);
}

static VKAPI_ATTR void VKAPI_CALL fl_CmdDrawIndirect(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, uint32_t n, uint32_t st)
{
    prep(cb);
    state()->draw_ind(cb, b, o, n, st);
}

static VKAPI_ATTR void VKAPI_CALL fl_CmdDrawIndexedIndirect(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, uint32_t n, uint32_t st)
{
    prep(cb);
    state()->draw_idx_ind(cb, b, o, n, st);
}

static VKAPI_ATTR void VKAPI_CALL fl_CmdDrawIndirectCount(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, VkBuffer cbuf, VkDeviceSize co,
                                                         uint32_t max, uint32_t st)
{
    prep(cb);
    state()->draw_ind_cnt(cb, b, o, cbuf, co, max, st);
}

static VKAPI_ATTR void VKAPI_CALL fl_CmdDrawIndexedIndirectCount(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, VkBuffer cbuf,
                                                                VkDeviceSize co, uint32_t max, uint32_t st)
{
    prep(cb);
    state()->draw_idx_ind_cnt(cb, b, o, cbuf, co, max, st);
}

static VKAPI_ATTR void VKAPI_CALL fl_CmdDrawIndirectByteCountEXT(VkCommandBuffer cb, uint32_t ic, uint32_t fi, VkBuffer b, VkDeviceSize o,
                                                                uint32_t off, uint32_t stride)
{
    prep(cb);
    state()->draw_bytes(cb, ic, fi, b, o, off, stride);
}

static VKAPI_ATTR void VKAPI_CALL fl_CmdDrawMultiEXT(VkCommandBuffer cb, uint32_t n, const VkMultiDrawInfoEXT *vi, uint32_t ic, uint32_t fi,
                                                    uint32_t st)
{
    prep(cb);
    state()->draw_multi(cb, n, vi, ic, fi, st);
}

static VKAPI_ATTR void VKAPI_CALL fl_CmdDrawMultiIndexedEXT(VkCommandBuffer cb, uint32_t n, const VkMultiDrawIndexedInfoEXT *ii, uint32_t ic,
                                                           uint32_t fi, uint32_t st, const int32_t *vo)
{
    prep(cb);
    state()->draw_multi_idx(cb, n, ii, ic, fi, st, vo);
}

/* ------------------------------------------------------------------ the imitation */

static VKAPI_ATTR VkResult VKAPI_CALL fli_CreateGraphicsPipelines(VkDevice d, VkPipelineCache cache, uint32_t n,
                                                                  const VkGraphicsPipelineCreateInfo *ci, const VkAllocationCallbacks *a,
                                                                  VkPipeline *out)
{
    vkb_errata_flip *s = state();
    VkGraphicsPipelineCreateInfo *c = malloc(n * sizeof(*c));
    VkPipelineViewportStateCreateInfo *vs = calloc(n, sizeof(*vs));
    VkViewport **vv = calloc(n, sizeof(*vv));
    if (!c || !vs || !vv) {
        free(c), free(vs), free(vv);
        return s->imi_gfx(d, cache, n, ci, a, out);
    }
    memcpy(c, ci, n * sizeof(*c));
    for (uint32_t i = 0; i < n; i++) {
        const VkPipelineViewportStateCreateInfo *v = ci[i].pViewportState;
        if (dyn_kind(&ci[i]) != DYN_NONE || !v || !v->pViewports) continue;
        int any = 0;
        for (uint32_t k = 0; k < v->viewportCount; k++) any |= v->pViewports[k].height < 0;
        if (!any || !(vv[i] = malloc(v->viewportCount * sizeof(VkViewport)))) continue;
        memcpy(vv[i], v->pViewports, v->viewportCount * sizeof(VkViewport));
        for (uint32_t k = 0; k < v->viewportCount; k++)
            if (vv[i][k].height < 0) rewrite_vp(&vv[i][k]); /* the same rows, not flipped */
        vs[i] = *v;
        vs[i].pViewports = vv[i];
        c[i].pViewportState = &vs[i];
        __atomic_add_fetch(&s->imitated_pipes, 1, __ATOMIC_RELAXED);
    }
    VkResult r = s->imi_gfx(d, cache, n, c, a, out);
    for (uint32_t i = 0; i < n; i++) free(vv[i]);
    free(c), free(vs), free(vv);
    return r;
}

static void imi_viewports(vkb_errata_flip *s, VkCommandBuffer cb, int with_count, uint32_t first, uint32_t n, const VkViewport *v)
{
    VkViewport tmp[MAX_VP], *w = n <= MAX_VP ? tmp : malloc(n * sizeof(*w));
    if (w) {
        memcpy(w, v, n * sizeof(*w));
        for (uint32_t i = 0; i < n; i++)
            if (w[i].height < 0) rewrite_vp(&w[i]), __atomic_add_fetch(&s->imitated_vp, 1, __ATOMIC_RELAXED);
        v = w;
    }
    if (with_count) s->imi_vpc(cb, n, v);
    else s->imi_vp(cb, first, n, v);
    if (w && w != tmp) free(w);
}

static VKAPI_ATTR void VKAPI_CALL fli_CmdSetViewport(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkViewport *v)
{
    imi_viewports(state(), cb, 0, first, n, v);
}

static VKAPI_ATTR void VKAPI_CALL fli_CmdSetViewportWithCount(VkCommandBuffer cb, uint32_t n, const VkViewport *v)
{
    imi_viewports(state(), cb, 1, 0, n, v);
}

/* ------------------------------------------------------------------ install */

static vkb_errata_flip *get_state(vkb_srv_table *t, vkb_errata_dev *e)
{
    if (!e->flip) {
        vkb_errata_flip *s = calloc(1, sizeof(*s));
        if (!s) return NULL;
        pthread_mutex_init(&s->lock, NULL);
        s->device = t->device;
        s->last_key0 = 1, s->last_key_vp = KEY_ALL; /* DXVK's and vkd3d-proton's: every viewport negative */
        const char *mb = vkb_srv_env("VKBRIDGE_FLIP_RETAIN_MB");
        s->retain_cap = (size_t)(mb && *mb ? atol(mb) : RETAIN_MB_DEFAULT) << 20;
        e->flip = s;
    }
    return e->flip;
}

void vkb_errata_flip_install(vkb_srv_table *t, vkb_errata_dev *e)
{
    vkb_dispatch *r = &t->real;
    if (!r->vkCreateGraphicsPipelines || !r->vkCmdSetViewport || !r->vkCmdBindPipeline || !r->vkCmdDraw) return;
    vkb_errata_flip *s = get_state(t, e);
    if (!s) return;
    s->fix = 1;
    s->mod = r->vkCreateShaderModule, s->dmod = r->vkDestroyShaderModule;
    s->gfx = r->vkCreateGraphicsPipelines, s->dpipe = r->vkDestroyPipeline;
    s->drp = r->vkDestroyRenderPass, s->dpl = r->vkDestroyPipelineLayout;
    s->acb = r->vkAllocateCommandBuffers, s->fcb = r->vkFreeCommandBuffers, s->bcb = r->vkBeginCommandBuffer;
    s->dpool = r->vkDestroyCommandPool;
    s->bind = r->vkCmdBindPipeline, s->vp = r->vkCmdSetViewport, s->vpc = r->vkCmdSetViewportWithCount;
    s->draw = r->vkCmdDraw, s->draw_idx = r->vkCmdDrawIndexed, s->draw_ind = r->vkCmdDrawIndirect;
    s->draw_idx_ind = r->vkCmdDrawIndexedIndirect, s->draw_ind_cnt = r->vkCmdDrawIndirectCount;
    s->draw_idx_ind_cnt = r->vkCmdDrawIndexedIndirectCount, s->draw_bytes = r->vkCmdDrawIndirectByteCountEXT;
    s->draw_multi = r->vkCmdDrawMultiEXT, s->draw_multi_idx = r->vkCmdDrawMultiIndexedEXT;
    ERRATA_HOOK(t, vkCreateShaderModule, fl_CreateShaderModule);
    ERRATA_HOOK(t, vkDestroyShaderModule, fl_DestroyShaderModule);
    ERRATA_HOOK(t, vkCreateGraphicsPipelines, fl_CreateGraphicsPipelines);
    ERRATA_HOOK(t, vkDestroyPipeline, fl_DestroyPipeline);
    ERRATA_HOOK(t, vkDestroyRenderPass, fl_DestroyRenderPass);
    ERRATA_HOOK(t, vkDestroyPipelineLayout, fl_DestroyPipelineLayout);
    ERRATA_HOOK(t, vkAllocateCommandBuffers, fl_AllocateCommandBuffers);
    ERRATA_HOOK(t, vkFreeCommandBuffers, fl_FreeCommandBuffers);
    ERRATA_HOOK(t, vkDestroyCommandPool, fl_DestroyCommandPool);
    ERRATA_HOOK(t, vkBeginCommandBuffer, fl_BeginCommandBuffer);
    ERRATA_HOOK(t, vkCmdBindPipeline, fl_CmdBindPipeline);
    ERRATA_HOOK(t, vkCmdSetViewport, fl_CmdSetViewport);
    ERRATA_HOOK(t, vkCmdSetViewportWithCount, fl_CmdSetViewportWithCount);
    ERRATA_HOOK(t, vkCmdDraw, fl_CmdDraw);
    ERRATA_HOOK(t, vkCmdDrawIndexed, fl_CmdDrawIndexed);
    ERRATA_HOOK(t, vkCmdDrawIndirect, fl_CmdDrawIndirect);
    ERRATA_HOOK(t, vkCmdDrawIndexedIndirect, fl_CmdDrawIndexedIndirect);
    ERRATA_HOOK(t, vkCmdDrawIndirectCount, fl_CmdDrawIndirectCount);
    ERRATA_HOOK(t, vkCmdDrawIndexedIndirectCount, fl_CmdDrawIndexedIndirectCount);
    ERRATA_HOOK(t, vkCmdDrawIndirectByteCountEXT, fl_CmdDrawIndirectByteCountEXT);
    ERRATA_HOOK(t, vkCmdDrawMultiEXT, fl_CmdDrawMultiEXT);
    ERRATA_HOOK(t, vkCmdDrawMultiIndexedEXT, fl_CmdDrawMultiIndexedEXT);
    __atomic_store_n(&last_state, s, __ATOMIC_RELEASE);
}

void vkb_errata_flip_imitate(vkb_srv_table *t, vkb_errata_dev *e)
{
    vkb_dispatch *r = &t->real;
    if (!r->vkCreateGraphicsPipelines || !r->vkCmdSetViewport) return;
    vkb_errata_flip *s = get_state(t, e);
    if (!s) return;
    s->imitate = 1;
    s->imi_gfx = r->vkCreateGraphicsPipelines;
    s->imi_vp = r->vkCmdSetViewport;
    s->imi_vpc = r->vkCmdSetViewportWithCount;
    ERRATA_HOOK(t, vkCreateGraphicsPipelines, fli_CreateGraphicsPipelines);
    ERRATA_HOOK(t, vkCmdSetViewport, fli_CmdSetViewport);
    ERRATA_HOOK(t, vkCmdSetViewportWithCount, fli_CmdSetViewportWithCount);
    __atomic_store_n(&last_state, s, __ATOMIC_RELEASE);
}

void vkb_errata_flip_destroy(vkb_srv_table *t, vkb_errata_dev *e)
{
    vkb_errata_flip *s = e->flip;
    if (!s) return;
    if (s->fix)
        VKB_INFO("errata: viewport_flip: device %u: %lu pipeline(s) made (%lu static viewports, %lu dynamic, %lu of them linked), "
                 "%lu with a flipped stage, %lu could not be flipped; %lu variant(s) compiled at draw time in %.1f ms (%lu failed), "
                 "SPIR-V pass %.1f ms; %lu viewport(s) rewritten, %lu re-sent before a draw, %lu variant bind(s)",
                 t->id, s->p_static + s->p_dynamic, s->p_static, s->p_dynamic, s->p_linked, s->p_flipped, s->p_failed, s->variants,
                 s->variant_us / 1e3, s->variant_failed, s->flip_us / 1e3, s->viewports, s->reemits, s->rebinds);
    if (s->imitate)
        VKB_INFO("errata: viewport_flip: device %u: the imitation turned %lu viewport(s) and %lu pipeline(s) upside down", t->id,
                 s->imitated_vp, s->imitated_pipes);
    /* what the app left: records (with their variants and what they keep), modules, blobs */
    for (unsigned k = 0; k < BUCKETS; k++)
        while (s->cbs[k]) {
            cb_rec *c = s->cbs[k];
            s->cbs[k] = c->next;
            pipe_put(s, c->pipe);
            free(c);
        }
    for (unsigned k = 0; k < BUCKETS; k++)
        while (s->pipes[k]) {
            pipe_rec *r = s->pipes[k];
            s->pipes[k] = r->next;
            pipe_put(s, r);
        }
    for (unsigned k = 0; k < BUCKETS; k++) {
        while (s->mods[k]) {
            mod_rec *m = s->mods[k];
            s->mods[k] = m->next;
            free(m);
        }
        while (s->objs[k]) {
            obj_ref *o = s->objs[k];
            s->objs[k] = o->next;
            if (o->pending) obj_destroy(s, o->h, o->kind);
            free(o);
        }
        while (s->blobs[k]) {
            blob *b = s->blobs[k];
            s->blobs[k] = b->next;
            free(b->code);
            free(b);
        }
    }
    __atomic_add_fetch(&cb_gen, 1, __ATOMIC_ACQ_REL);
    e->flip = NULL;
    if (__atomic_load_n(&last_state, __ATOMIC_ACQUIRE) != s) {
        pthread_mutex_destroy(&s->lock);
        free(s);
    }
}
