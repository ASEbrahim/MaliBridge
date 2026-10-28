/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * Asynchronous graphics pipeline compiles (VKBRIDGE_ASYNC_COMPILE=1; off by default).
 *
 * Mali has no VK_EXT_graphics_pipeline_library, so DXVK 2.x and 3.x compile each new pipeline in
 * full on their submission thread at the first draw that needs it (DXVK 1.10-async and the
 * "gplasync" builds skip such draws instead), and Mali's compiler takes tens to hundreds of
 * milliseconds a pipeline: a frame-time spike each time (Dark Souls Remastered on the Tab S10+,
 * "very unstable framerate"; UE4 hitching). With this layer a vkCreateGraphicsPipelines that has
 * not finished within VKBRIDGE_ASYNC_COMPILE_WAIT_MS (2) returns a stand-in handle; a worker
 * thread (VKBRIDGE_ASYNC_COMPILE_THREADS, 2) goes on compiling it through every layer beneath
 * (pipeline cache, emulations, errata, the compile watchdog), and the draws recorded while the
 * stand-in is bound are dropped from the command buffer until the real pipeline is there. A
 * pipeline in the cache finishes inside the wait and its real handle goes back as before.
 * The idea is DXVK-async's (zlib-licensed, by Sporif and others); this is written for the bridge.
 *
 * Only pipelines whose skipped draws leave nothing behind but a frame without them are made in
 * the background; every other one compiles synchronously as before:
 *   - the pipeline writes no colour (no colour attachment, no fragment stage, every write mask
 *     zero, rasterizer discard): depth-only and stencil passes - shadow maps, depth pre-passes,
 *     occlusion - would be wrong for longer than a frame;
 *   - a stage stores to a storage buffer or image, uses a writing atomic or transform feedback
 *     (spirv_effects.c; a module the scan did not see counts as such);
 *   - no vertex input (no binding, the vertex input not dynamic): full-screen passes, and among
 *     them DXVK's own copy, blit, resolve and clear pipelines, whose results stay - unless
 *     VKBRIDGE_ASYNC_COMPILE=2, which lets them skip too (closer to DXVK-async; also D3D11
 *     draws that build their vertices from SV_VertexID);
 *   - flags or structures this layer does not know (pipeline libraries, creation feedback,
 *     anything else: only dynamic rendering, robustness and plain flags2 pass), a batch of more
 *     than one, or more than ASYNC_QUEUE_MAX pipelines already waiting.
 * Compute pipelines are never touched.
 *
 * The objects a queued compile names are kept for it: a vkDestroyShaderModule, PipelineLayout or
 * RenderPass of one still in use is done when the compile ends (DXVK destroys its modules right
 * after creating the pipeline). A derivative compiled in the background loses its base (allowed:
 * the base is only a hint). The app's pipeline cache is not used in the background: the
 * persistent cache (server_pcache.c) takes those pipelines.
 *
 * The draw skipping is decided as the command buffer is replayed into the driver (its
 * vkEndCommandBuffer, server_conn.c), on that thread: a vkCmdBindPipeline of a stand-in that is
 * not ready is not passed down and the draws after it are dropped, until the next graphics bind.
 * A stand-in that is ready is bound as its real pipeline. A compile that fails leaves its draws
 * skipped for good (DXVK skips a failed pipeline's draws too).
 *
 * VKBRIDGE_ASYNC_COMPILE_IMITATE_MS=<ms> (tests, measurements): every graphics compile through
 * this layer waits that long first, synchronous ones included, with the layer off too - a slow
 * compiler for the host suite (host/asyncpipe_test.sh) and an A/B on a fast one.
 */
#define _GNU_SOURCE
#include "vkb_server.h"
#include "spirv_effects.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define ASYNC_QUEUE_MAX 1024 /* per device: past it, compiles are synchronous again */
#define ASYNC_THREADS_DEFAULT 2
#define ASYNC_THREADS_MAX 8
#define ASYNC_WAIT_MS_DEFAULT 2.0
#define ASYNC_LOG_PERIOD_S 30
#define PROXY_BUCKETS 1024
#define OBJ_BUCKETS 1024
#define REFS_MAX 8 /* the modules of up to 6 stages, the layout, the render pass */

enum { ST_PENDING, ST_READY, ST_FAILED };
enum { OBJ_MODULE, OBJ_LAYOUT, OBJ_RENDER_PASS };
enum {
    WHY_BATCH, WHY_FLAGS, WHY_CHAIN, WHY_STAGES, WHY_NO_COLOR, WHY_DISCARD, WHY_NO_VERTEX_INPUT, WHY_EFFECTS,
    WHY_MODULE, WHY_QUEUE, WHY_COUNT
};
static const char *const why_names[WHY_COUNT] = {
    "batch", "flags", "structures", "stages", "no colour output", "rasterizer discard", "no vertex input", "stores or xfb",
    "unknown module", "queue full",
};

typedef struct vkb_async {
    vkb_srv_table *t;
    int on;            /* background compiles; 0: the imitation's delay alone */
    int fullscreen;    /* VKBRIDGE_ASYNC_COMPILE=2: pipelines without vertex input too */
    double imitate_ms; /* VKBRIDGE_ASYNC_COMPILE_IMITATE_MS */
    double wait_ms;    /* VKBRIDGE_ASYNC_COMPILE_WAIT_MS */
    /* what this layer calls: the next one down */
    PFN_vkCreateGraphicsPipelines gfx;
    PFN_vkDestroyPipeline destroy_pipe;
    PFN_vkCreateShaderModule create_mod;
    PFN_vkDestroyShaderModule destroy_mod;
    PFN_vkDestroyPipelineLayout destroy_layout;
    PFN_vkDestroyRenderPass destroy_rp;
    PFN_vkBeginCommandBuffer begin;
    PFN_vkCmdBindPipeline bind;
    PFN_vkCmdDraw draw;
    PFN_vkCmdDrawIndexed draw_indexed;
    PFN_vkCmdDrawIndirect draw_indirect;
    PFN_vkCmdDrawIndexedIndirect draw_indexed_indirect;
    PFN_vkCmdDrawIndirectCount draw_indirect_count;
    PFN_vkCmdDrawIndexedIndirectCount draw_indexed_indirect_count;
    PFN_vkCmdDrawMultiEXT draw_multi;
    PFN_vkCmdDrawMultiIndexedEXT draw_multi_indexed;
    PFN_vkCmdDrawIndirectByteCountEXT draw_byte_count;
    /* under lk */
    int queued, running;
    struct vkb_async *next;
    /* statistics (atomic) */
    uint64_t n_background, n_quick, n_failed, n_sync[WHY_COUNT], draws_skipped, binds_skipped, compile_us_total;
    uint64_t compile_us_max, create_us_max, queue_max;
    uint64_t logged_background, logged_skipped;
    uint32_t chain_stype; /* the first structure that kept a pipeline synchronous (the summary names it) */
    double last_log;
} vkb_async;

/* A pipeline compiled in the background; while the client holds it as a stand-in (exposed), its
 * address is the handle the client has. */
typedef struct apipe {
    vkb_async *a;
    int state;     /* ST_* (lk) */
    int exposed;   /* the client has the stand-in (lk); 0: the creating thread takes the result */
    int destroyed; /* the client destroyed the stand-in while it compiled (lk) */
    VkPipeline real;
    uint8_t *blob; /* the encoded create info */
    size_t blob_len;
    uint32_t nrefs;
    struct objref *refs[REFS_MAX];
    double t_queued;
    struct apipe *qnext, *hnext;
} apipe;

/* An object a queued compile names (and each shader module's scan result). */
typedef struct objref {
    uint64_t h;
    int type;
    vkb_async *a;
    uint32_t fx;  /* modules: SPV_FX_* */
    int refs;     /* queued or running compiles naming it */
    int dead;     /* the client destroyed it: done when refs reaches 0 */
    struct objref *next;
} objref;

extern __thread const char *vkb_srv_current_cmd; /* server_conn.c: named in the crash line */

static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t work_cv = PTHREAD_COND_INITIALIZER;
static pthread_cond_t done_cv; /* CLOCK_MONOTONIC, made once (init_once) */
static pthread_once_t once = PTHREAD_ONCE_INIT;
static apipe *q_head, *q_tail;
static apipe *proxies[PROXY_BUCKETS];
static int nproxies; /* atomic reads: binds look nothing up while it is 0 */
static objref *objs[OBJ_BUCKETS];
static vkb_async *devs;
static int nworkers, want_workers;

/* The command buffer whose draws this thread drops (its bound pipeline is a stand-in not ready). */
static __thread VkCommandBuffer tls_skip;

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static void init_once(void)
{
    pthread_condattr_t ca;
    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    pthread_cond_init(&done_cv, &ca);
    pthread_condattr_destroy(&ca);
}

static void stat_max(uint64_t *m, uint64_t v)
{
    uint64_t o = __atomic_load_n(m, __ATOMIC_RELAXED);
    while (v > o && !__atomic_compare_exchange_n(m, &o, v, 1, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
}

static vkb_async *cur(void)
{
    vkb_srv_table *t = vkb_srv_current_table();
    if (t && t->async) return t->async;
    pthread_mutex_lock(&lk);
    vkb_async *a = devs; /* a thread without a table: the driver's functions are the same for every device */
    pthread_mutex_unlock(&lk);
    return a;
}

static void imitate(const vkb_async *a)
{
    if (a->imitate_ms > 0) usleep((useconds_t)(a->imitate_ms * 1000));
}

/* ------------------------------------------------------------------ handle maps (lk held) */

static unsigned hbucket(uint64_t h, unsigned n) { return (unsigned)((h >> 4) ^ (h >> 20)) & (n - 1); }

static apipe *proxy_find(VkPipeline p)
{
    for (apipe *x = proxies[hbucket((uint64_t)(uintptr_t)p, PROXY_BUCKETS)]; x; x = x->hnext)
        if ((VkPipeline)(uintptr_t)x == p) return x;
    return NULL;
}

static void proxy_add(apipe *x)
{
    unsigned b = hbucket((uint64_t)(uintptr_t)x, PROXY_BUCKETS);
    x->hnext = proxies[b];
    __atomic_store_n(&proxies[b], x, __ATOMIC_RELEASE);
    __atomic_add_fetch(&nproxies, 1, __ATOMIC_RELEASE);
}

static void proxy_remove(apipe *x)
{
    for (apipe **pp = &proxies[hbucket((uint64_t)(uintptr_t)x, PROXY_BUCKETS)]; *pp; pp = &(*pp)->hnext)
        if (*pp == x) {
            __atomic_store_n(pp, x->hnext, __ATOMIC_RELEASE);
            __atomic_sub_fetch(&nproxies, 1, __ATOMIC_RELEASE);
            return;
        }
}

static objref **obj_slot(int type, uint64_t h)
{
    objref **pp = &objs[hbucket(h ^ (uint64_t)type, OBJ_BUCKETS)];
    while (*pp && !((*pp)->h == h && (*pp)->type == type)) pp = &(*pp)->next;
    return pp;
}

static objref *obj_get(vkb_async *a, int type, uint64_t h)
{
    objref **pp = obj_slot(type, h);
    if (*pp) return *pp;
    objref *o = calloc(1, sizeof(*o));
    if (!o) return NULL;
    o->h = h;
    o->type = type;
    o->a = a;
    o->fx = SPV_FX_STORE | SPV_FX_XFB; /* until a scan says otherwise */
    *pp = o;
    return o;
}

static void obj_unlink(objref *o)
{
    objref **pp = obj_slot(o->type, o->h);
    if (*pp == o) *pp = o->next;
}

/* The client's destroy of an object that may be in use by a queued compile. */
static int defer_destroy(int type, uint64_t h)
{
    if (!h) return 0;
    pthread_mutex_lock(&lk);
    objref **pp = obj_slot(type, h), *o = *pp;
    int deferred = o && o->refs > 0;
    if (deferred) o->dead = 1;
    else if (o) {
        *pp = o->next;
        free(o);
    }
    pthread_mutex_unlock(&lk);
    return deferred;
}

static void destroy_obj(vkb_async *a, int type, uint64_t h)
{
    VkDevice d = a->t->device;
    if (type == OBJ_MODULE) a->destroy_mod(d, (VkShaderModule)(uintptr_t)h, NULL);
    else if (type == OBJ_LAYOUT) a->destroy_layout(d, (VkPipelineLayout)(uintptr_t)h, NULL);
    else a->destroy_rp(d, (VkRenderPass)(uintptr_t)h, NULL);
}

/* A compile is done with its objects: those the client destroyed meanwhile go now. */
static void release_refs(apipe *x)
{
    objref *gone[REFS_MAX];
    uint32_t ng = 0;
    pthread_mutex_lock(&lk);
    for (uint32_t i = 0; i < x->nrefs; i++) {
        objref *o = x->refs[i];
        if (--o->refs == 0 && o->dead) {
            obj_unlink(o);
            gone[ng++] = o;
        }
    }
    x->nrefs = 0;
    pthread_mutex_unlock(&lk);
    for (uint32_t i = 0; i < ng; i++) {
        destroy_obj(x->a, gone[i]->type, gone[i]->h);
        free(gone[i]);
    }
}

/* ------------------------------------------------------------------ which pipelines */

static int dynamic(const VkGraphicsPipelineCreateInfo *ci, VkDynamicState s)
{
    const VkPipelineDynamicStateCreateInfo *d = ci->pDynamicState;
    for (uint32_t i = 0; d && i < d->dynamicStateCount; i++)
        if (d->pDynamicStates[i] == s) return 1;
    return 0;
}

#define FLAGS_OK (VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT | VK_PIPELINE_CREATE_ALLOW_DERIVATIVES_BIT | VK_PIPELINE_CREATE_DERIVATIVE_BIT)

/* The scan of the stages whose SPIR-V comes inline (VkShaderModuleCreateInfo in the stage's
 * chain, maintenance5), before lk is taken; 0 for the others. */
static void scan_inline(const VkGraphicsPipelineCreateInfo *ci, uint32_t fx[6])
{
    for (uint32_t i = 0; i < ci->stageCount && i < 6; i++) {
        fx[i] = 0;
        if (ci->pStages[i].module) continue;
        for (const VkBaseInStructure *s = ci->pStages[i].pNext; s; s = s->pNext)
            if (s->sType == VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO) {
                const VkShaderModuleCreateInfo *m = (const void *)s;
                fx[i] = spv_effects(m->pCode, m->codeSize);
            }
    }
}

/* Why ci must compile synchronously (WHY_*), or -1 when its draws may be skipped meanwhile; fx:
 * scan_inline's. Takes a reference on each object it names (refs, *nrefs) when it returns -1; lk
 * held. */
static int classify(vkb_async *a, const VkGraphicsPipelineCreateInfo *ci, const uint32_t inline_fx[6], objref **refs, uint32_t *nrefs)
{
    if (ci->flags & ~(VkPipelineCreateFlags)FLAGS_OK) return WHY_FLAGS;
    for (const VkBaseInStructure *s = ci->pNext; s; s = s->pNext) {
        if (s->sType == VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO) {
            const VkPipelineCreateFlags2CreateInfo *f = (const void *)s;
            if (f->flags & ~(VkPipelineCreateFlags2)(VK_PIPELINE_CREATE_2_DISABLE_OPTIMIZATION_BIT | VK_PIPELINE_CREATE_2_ALLOW_DERIVATIVES_BIT))
                return WHY_FLAGS;
        } else if (s->sType != VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO && s->sType != VK_STRUCTURE_TYPE_PIPELINE_ROBUSTNESS_CREATE_INFO) {
            if (!a->chain_stype) a->chain_stype = s->sType;
            return WHY_CHAIN;
        }
    }
    /* colour output */
    const VkPipelineRasterizationStateCreateInfo *rs = ci->pRasterizationState;
    if (!rs || rs->rasterizerDiscardEnable || dynamic(ci, VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE)) return WHY_DISCARD;
    for (const VkBaseInStructure *s = rs->pNext; s; s = s->pNext)
        if (s->sType == VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_STREAM_CREATE_INFO_EXT) return WHY_EFFECTS;
    const VkPipelineColorBlendStateCreateInfo *cb = ci->pColorBlendState;
    int color = 0;
    for (uint32_t i = 0; cb && cb->pAttachments && i < cb->attachmentCount; i++) color |= cb->pAttachments[i].colorWriteMask != 0;
    if (cb && cb->attachmentCount && dynamic(ci, VK_DYNAMIC_STATE_COLOR_WRITE_MASK_EXT)) color = 1;
    for (const VkBaseInStructure *s = ci->pNext; s; s = s->pNext)
        if (s->sType == VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO) {
            const VkPipelineRenderingCreateInfo *r = (const void *)s;
            int any = 0;
            for (uint32_t i = 0; r->pColorAttachmentFormats && i < r->colorAttachmentCount; i++)
                any |= r->pColorAttachmentFormats[i] != VK_FORMAT_UNDEFINED;
            if (!any) color = 0;
        }
    if (!color) return WHY_NO_COLOR;
    /* vertex input */
    const VkPipelineVertexInputStateCreateInfo *vi = ci->pVertexInputState;
    if (!a->fullscreen && !dynamic(ci, VK_DYNAMIC_STATE_VERTEX_INPUT_EXT) && (!vi || !vi->vertexBindingDescriptionCount))
        return WHY_NO_VERTEX_INPUT;
    /* stages: a vertex and a fragment stage, each without side effects */
    uint32_t seen = 0, fx = 0, n = 0;
    objref *r[REFS_MAX];
    if (ci->stageCount > 6) return WHY_STAGES;
    for (uint32_t i = 0; i < ci->stageCount; i++) {
        const VkPipelineShaderStageCreateInfo *st = &ci->pStages[i];
        seen |= st->stage;
        const VkShaderModuleCreateInfo *inl = NULL;
        for (const VkBaseInStructure *s = st->pNext; s; s = s->pNext) {
            if (s->sType == VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO) inl = (const void *)s;
            else if (s->sType != VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO &&
                     s->sType != VK_STRUCTURE_TYPE_PIPELINE_ROBUSTNESS_CREATE_INFO && s->sType != VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT) {
                if (!a->chain_stype) a->chain_stype = s->sType;
                return WHY_CHAIN;
            }
        }
        if (st->module) {
            objref *o = *obj_slot(OBJ_MODULE, (uint64_t)(uintptr_t)st->module);
            if (!o || o->dead || o->a != a) return WHY_MODULE;
            fx |= o->fx;
            r[n++] = o;
        } else if (inl) {
            fx |= inline_fx[i];
        } else {
            return WHY_MODULE;
        }
    }
    if (!(seen & VK_SHADER_STAGE_VERTEX_BIT) || !(seen & VK_SHADER_STAGE_FRAGMENT_BIT) ||
        (seen & ~(VkShaderStageFlags)VK_SHADER_STAGE_ALL_GRAPHICS))
        return WHY_STAGES;
    if (fx) return WHY_EFFECTS;
    if (ci->layout) {
        objref *o = obj_get(a, OBJ_LAYOUT, (uint64_t)(uintptr_t)ci->layout);
        if (!o) return WHY_QUEUE;
        r[n++] = o;
    }
    if (ci->renderPass) {
        objref *o = obj_get(a, OBJ_RENDER_PASS, (uint64_t)(uintptr_t)ci->renderPass);
        if (!o) return WHY_QUEUE;
        r[n++] = o;
    }
    for (uint32_t i = 0; i < n; i++) {
        r[i]->refs++;
        refs[i] = r[i];
    }
    *nrefs = n;
    return -1;
}

/* ------------------------------------------------------------------ workers */

static void log_summary(vkb_async *a, const char *when)
{
    char sync[320] = "";
    size_t o = 0;
    uint64_t nsync = 0;
    for (int i = 0; i < WHY_COUNT; i++) {
        uint64_t v = __atomic_load_n(&a->n_sync[i], __ATOMIC_RELAXED);
        nsync += v;
        if (v && o < sizeof(sync))
            o += snprintf(sync + o, sizeof(sync) - o, "%s%s %llu", o ? ", " : "", why_names[i], (unsigned long long)v);
        if (v && i == WHY_CHAIN && a->chain_stype && o < sizeof(sync))
            o += snprintf(sync + o, sizeof(sync) - o, " (first: structure type %u)", a->chain_stype);
    }
    uint64_t bg = __atomic_load_n(&a->n_background, __ATOMIC_RELAXED);
    VKB_INFO("async compile, device %u%s: %llu pipelines in the background (%llu failed, longest %.0f ms, mean %.0f ms, queue up to %llu), "
             "%llu ready within the %.0f ms wait, %llu draws skipped (%llu binds); %llu synchronous%s%s%s (longest call %.0f ms)",
             a->t->id, when, (unsigned long long)bg, (unsigned long long)a->n_failed, a->compile_us_max / 1000.0,
             bg ? a->compile_us_total / 1000.0 / bg : 0.0, (unsigned long long)a->queue_max, (unsigned long long)a->n_quick, a->wait_ms,
             (unsigned long long)a->draws_skipped, (unsigned long long)a->binds_skipped, (unsigned long long)nsync, *sync ? " (" : "", sync,
             *sync ? ")" : "", a->create_us_max / 1000.0);
}

static void compile_job(apipe *x, vkb_arena *arena)
{
    vkb_async *a = x->a;
    vkb_srv_set_current_table(a->t); /* the layers beneath find their device by it */
    vkb_srv_current_cmd = "vkCreateGraphicsPipelines (in the background)";
    double t0 = now_s();
    vkb_dec d;
    vkb_dec_init(&d, x->blob, x->blob_len, arena, NULL, 0);
    VkGraphicsPipelineCreateInfo ci;
    memset(&ci, 0, sizeof(ci));
    vkb_dec_VkGraphicsPipelineCreateInfo(&d, &ci);
    VkPipeline p = VK_NULL_HANDLE;
    VkResult r = VK_ERROR_INITIALIZATION_FAILED;
    if (!d.err) {
        imitate(a);
        r = a->gfx(a->t->device, VK_NULL_HANDLE, 1, &ci, NULL, &p);
    } else {
        VKB_ERR("async compile: the queued create info did not decode");
    }
    vkb_arena_reset(arena);
    vkb_srv_current_cmd = NULL;
    uint64_t us = (uint64_t)((now_s() - t0) * 1e6);
    __atomic_add_fetch(&a->compile_us_total, us, __ATOMIC_RELAXED);
    stat_max(&a->compile_us_max, us);
    free(x->blob);
    x->blob = NULL;
    release_refs(x);
    if (r != VK_SUCCESS) {
        p = VK_NULL_HANDLE;
        __atomic_add_fetch(&a->n_failed, 1, __ATOMIC_RELAXED);
        VKB_WARN("async compile, device %u: a pipeline failed in the background (%d); its draws stay skipped", a->t->id, r);
    }
    pthread_mutex_lock(&lk);
    x->real = p;
    x->state = p ? ST_READY : ST_FAILED;
    int drop = x->exposed && x->destroyed; /* the client let go of it meanwhile */
    pthread_mutex_unlock(&lk);
    if (drop) {
        if (p) a->destroy_pipe(a->t->device, p, NULL);
        free(x);
    }
    double now = now_s();
    pthread_mutex_lock(&lk);
    uint64_t bg = __atomic_load_n(&a->n_background, __ATOMIC_RELAXED), sk = __atomic_load_n(&a->draws_skipped, __ATOMIC_RELAXED);
    int say = now - a->last_log > ASYNC_LOG_PERIOD_S && (bg != a->logged_background || sk != a->logged_skipped);
    if (say) {
        a->last_log = now;
        a->logged_background = bg;
        a->logged_skipped = sk;
    }
    pthread_mutex_unlock(&lk);
    if (say) log_summary(a, " so far");
    vkb_srv_set_current_table(NULL);
}

static void *worker(void *arg)
{
    (void)arg;
    vkb_arena arena = {0};
    pthread_mutex_lock(&lk);
    for (;;) {
        while (!q_head) pthread_cond_wait(&work_cv, &lk);
        apipe *x = q_head;
        q_head = x->qnext;
        if (!q_head) q_tail = NULL;
        vkb_async *a = x->a;
        a->queued--;
        a->running++;
        pthread_mutex_unlock(&lk);
        compile_job(x, &arena); /* x may be gone after it */
        pthread_mutex_lock(&lk);
        a->running--;
        pthread_cond_broadcast(&done_cv);
    }
    return NULL;
}

static void start_workers(void) /* lk held */
{
    while (nworkers < want_workers) {
        pthread_t th;
        pthread_attr_t at;
        pthread_attr_init(&at);
        pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        pthread_attr_setstacksize(&at, 4u << 20); /* as the connection threads: the compiler runs on it */
        int e = pthread_create(&th, &at, worker, NULL);
        pthread_attr_destroy(&at);
        if (e) {
            VKB_WARN("async compile: no worker thread (%s)", strerror(e));
            break;
        }
        nworkers++;
    }
}

/* ------------------------------------------------------------------ the wrappers */

static VkResult sync_create(vkb_async *a, VkDevice device, VkPipelineCache cache, uint32_t n, const VkGraphicsPipelineCreateInfo *ci,
                            const VkAllocationCallbacks *al, VkPipeline *out, int why)
{
    /* A base that is a stand-in: its real pipeline when there is one, else no base (only a hint). */
    VkGraphicsPipelineCreateInfo *copy = NULL;
    for (uint32_t i = 0; i < n && ci && __atomic_load_n(&nproxies, __ATOMIC_ACQUIRE); i++) {
        if (!ci[i].basePipelineHandle) continue;
        pthread_mutex_lock(&lk);
        apipe *x = proxy_find(ci[i].basePipelineHandle);
        VkPipeline real = x && x->state == ST_READY ? x->real : VK_NULL_HANDLE;
        pthread_mutex_unlock(&lk);
        if (!x) continue;
        if (!copy) {
            if (!(copy = malloc(n * sizeof(*ci)))) return VK_ERROR_OUT_OF_HOST_MEMORY;
            memcpy(copy, ci, n * sizeof(*ci));
        }
        copy[i].basePipelineHandle = real;
        if (!real) copy[i].flags &= ~(VkPipelineCreateFlags)VK_PIPELINE_CREATE_DERIVATIVE_BIT;
    }
    if (copy) ci = copy;
    if (why >= 0) {
        __atomic_add_fetch(&a->n_sync[why], n, __ATOMIC_RELAXED);
        VKB_DBG("async compile, device %u: %u pipeline(s) synchronous (%s)", a->t->id, n, why_names[why]);
    }
    double t0 = now_s();
    imitate(a);
    VkResult r = a->gfx(device, cache, n, ci, al, out);
    stat_max(&a->create_us_max, (uint64_t)((now_s() - t0) * 1e6));
    free(copy);
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL as_CreateGraphicsPipelines(VkDevice device, VkPipelineCache cache, uint32_t n,
                                                                 const VkGraphicsPipelineCreateInfo *ci, const VkAllocationCallbacks *al,
                                                                 VkPipeline *out)
{
    vkb_async *a = cur();
    if (!a->on) return sync_create(a, device, cache, n, ci, al, out, -1);
    if (n != 1 || !ci || !out) return sync_create(a, device, cache, n, ci, al, out, WHY_BATCH);
    apipe *x = calloc(1, sizeof(*x));
    if (!x) return sync_create(a, device, cache, n, ci, al, out, WHY_QUEUE);
    uint32_t inline_fx[6];
    if (ci->stageCount <= 6) scan_inline(ci, inline_fx);
    pthread_mutex_lock(&lk);
    int why = a->queued >= ASYNC_QUEUE_MAX ? WHY_QUEUE : classify(a, ci, inline_fx, x->refs, &x->nrefs);
    pthread_mutex_unlock(&lk);
    if (why < 0) {
        /* the copy the worker decodes: no base pipeline (a hint; the base may be a stand-in) */
        VkGraphicsPipelineCreateInfo c = *ci;
        c.flags &= ~(VkPipelineCreateFlags)VK_PIPELINE_CREATE_DERIVATIVE_BIT;
        c.basePipelineHandle = VK_NULL_HANDLE;
        c.basePipelineIndex = -1;
        vkb_enc e;
        vkb_enc_init(&e);
        vkb_enc_VkGraphicsPipelineCreateInfo(&e, &c);
        if (e.oom || !e.buf) {
            vkb_enc_free(&e);
            release_refs(x);
            why = WHY_QUEUE;
        } else {
            x->blob = e.buf;
            x->blob_len = e.len;
        }
    }
    if (why >= 0) {
        free(x);
        return sync_create(a, device, cache, n, ci, al, out, why);
    }
    double t0 = now_s();
    x->a = a;
    x->t_queued = t0;
    pthread_mutex_lock(&lk);
    start_workers();
    if (q_tail) q_tail->qnext = x;
    else q_head = x;
    q_tail = x;
    a->queued++;
    stat_max(&a->queue_max, (uint64_t)a->queued);
    pthread_cond_signal(&work_cv);
    /* a pipeline the cache has is done within the wait: its real handle goes back as before */
    struct timespec dl;
    clock_gettime(CLOCK_MONOTONIC, &dl);
    long ns = (long)(a->wait_ms * 1e6);
    dl.tv_sec += ns / 1000000000L;
    dl.tv_nsec += ns % 1000000000L;
    if (dl.tv_nsec >= 1000000000L) dl.tv_sec++, dl.tv_nsec -= 1000000000L;
    while (x->state == ST_PENDING && ns > 0)
        if (pthread_cond_timedwait(&done_cv, &lk, &dl) == ETIMEDOUT) break;
    int st = x->state;
    VkPipeline real = x->real;
    if (st == ST_PENDING) {
        x->exposed = 1;
        proxy_add(x);
    }
    pthread_mutex_unlock(&lk);
    stat_max(&a->create_us_max, (uint64_t)((now_s() - t0) * 1e6));
    if (st == ST_PENDING) {
        __atomic_add_fetch(&a->n_background, 1, __ATOMIC_RELAXED);
        *out = (VkPipeline)(uintptr_t)x;
        return VK_SUCCESS;
    }
    /* done in time (the worker leaves an unexposed one to this thread) */
    __atomic_add_fetch(&a->n_quick, 1, __ATOMIC_RELAXED);
    free(x);
    *out = real;
    return st == ST_READY ? VK_SUCCESS : VK_ERROR_INITIALIZATION_FAILED;
}

static VKAPI_ATTR void VKAPI_CALL as_DestroyPipeline(VkDevice device, VkPipeline p, const VkAllocationCallbacks *al)
{
    vkb_async *a = cur();
    if (p && __atomic_load_n(&nproxies, __ATOMIC_ACQUIRE)) {
        pthread_mutex_lock(&lk);
        apipe *x = proxy_find(p);
        int pending = 0;
        VkPipeline real = VK_NULL_HANDLE;
        if (x) {
            proxy_remove(x);
            pending = x->state == ST_PENDING;
            real = x->real;
            if (pending) x->destroyed = 1; /* the worker destroys it when done */
        }
        pthread_mutex_unlock(&lk);
        if (x) {
            if (!pending) {
                if (real) a->destroy_pipe(device, real, al);
                free(x);
            }
            return;
        }
    }
    a->destroy_pipe(device, p, al);
}

static VKAPI_ATTR VkResult VKAPI_CALL as_CreateShaderModule(VkDevice device, const VkShaderModuleCreateInfo *ci, const VkAllocationCallbacks *al,
                                                            VkShaderModule *out)
{
    vkb_async *a = cur();
    VkResult r = a->create_mod(device, ci, al, out);
    if (r != VK_SUCCESS || !*out) return r;
    uint32_t fx = spv_effects(ci->pCode, ci->codeSize);
    pthread_mutex_lock(&lk);
    objref **pp = obj_slot(OBJ_MODULE, (uint64_t)(uintptr_t)*out);
    if (*pp && (*pp)->refs == 0) { /* a stale entry for a reused handle */
        objref *o = *pp;
        *pp = o->next;
        free(o);
    }
    objref *o = *pp ? NULL : obj_get(a, OBJ_MODULE, (uint64_t)(uintptr_t)*out);
    if (o) o->fx = fx;
    pthread_mutex_unlock(&lk);
    return r;
}

static VKAPI_ATTR void VKAPI_CALL as_DestroyShaderModule(VkDevice device, VkShaderModule m, const VkAllocationCallbacks *al)
{
    vkb_async *a = cur();
    if (!defer_destroy(OBJ_MODULE, (uint64_t)(uintptr_t)m)) a->destroy_mod(device, m, al);
}

static VKAPI_ATTR void VKAPI_CALL as_DestroyPipelineLayout(VkDevice device, VkPipelineLayout l, const VkAllocationCallbacks *al)
{
    vkb_async *a = cur();
    if (!defer_destroy(OBJ_LAYOUT, (uint64_t)(uintptr_t)l)) a->destroy_layout(device, l, al);
}

static VKAPI_ATTR void VKAPI_CALL as_DestroyRenderPass(VkDevice device, VkRenderPass rp, const VkAllocationCallbacks *al)
{
    vkb_async *a = cur();
    if (!defer_destroy(OBJ_RENDER_PASS, (uint64_t)(uintptr_t)rp)) a->destroy_rp(device, rp, al);
}

static VKAPI_ATTR VkResult VKAPI_CALL as_BeginCommandBuffer(VkCommandBuffer cb, const VkCommandBufferBeginInfo *bi)
{
    if (tls_skip == cb) tls_skip = VK_NULL_HANDLE;
    return cur()->begin(cb, bi);
}

static VKAPI_ATTR void VKAPI_CALL as_CmdBindPipeline(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipeline p)
{
    vkb_async *a = cur();
    if (bp == VK_PIPELINE_BIND_POINT_GRAPHICS) {
        if (__atomic_load_n(&nproxies, __ATOMIC_ACQUIRE)) {
            unsigned b = hbucket((uint64_t)(uintptr_t)p, PROXY_BUCKETS);
            if (__atomic_load_n(&proxies[b], __ATOMIC_ACQUIRE)) {
                pthread_mutex_lock(&lk);
                apipe *x = proxy_find(p);
                VkPipeline real = x && x->state == ST_READY ? x->real : VK_NULL_HANDLE;
                pthread_mutex_unlock(&lk);
                if (x && !real) { /* still compiling (or failed): its draws are dropped */
                    tls_skip = cb;
                    __atomic_add_fetch(&a->binds_skipped, 1, __ATOMIC_RELAXED);
                    return;
                }
                if (x) p = real;
            }
        }
        if (tls_skip == cb) tls_skip = VK_NULL_HANDLE;
    }
    a->bind(cb, bp, p);
}

#define SKIP(cb)                                                                    \
    do {                                                                            \
        if (tls_skip && tls_skip == (cb)) {                                         \
            __atomic_add_fetch(&a->draws_skipped, 1, __ATOMIC_RELAXED);             \
            return;                                                                 \
        }                                                                           \
    } while (0)

static VKAPI_ATTR void VKAPI_CALL as_CmdDraw(VkCommandBuffer cb, uint32_t vc, uint32_t ic, uint32_t fv, uint32_t fi)
{
    vkb_async *a = cur();
    SKIP(cb);
    a->draw(cb, vc, ic, fv, fi);
}

static VKAPI_ATTR void VKAPI_CALL as_CmdDrawIndexed(VkCommandBuffer cb, uint32_t xc, uint32_t ic, uint32_t fx, int32_t vo, uint32_t fi)
{
    vkb_async *a = cur();
    SKIP(cb);
    a->draw_indexed(cb, xc, ic, fx, vo, fi);
}

static VKAPI_ATTR void VKAPI_CALL as_CmdDrawIndirect(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, uint32_t n, uint32_t s)
{
    vkb_async *a = cur();
    SKIP(cb);
    a->draw_indirect(cb, b, o, n, s);
}

static VKAPI_ATTR void VKAPI_CALL as_CmdDrawIndexedIndirect(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, uint32_t n, uint32_t s)
{
    vkb_async *a = cur();
    SKIP(cb);
    a->draw_indexed_indirect(cb, b, o, n, s);
}

static VKAPI_ATTR void VKAPI_CALL as_CmdDrawIndirectCount(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, VkBuffer cbuf, VkDeviceSize co,
                                                         uint32_t m, uint32_t s)
{
    vkb_async *a = cur();
    SKIP(cb);
    a->draw_indirect_count(cb, b, o, cbuf, co, m, s);
}

static VKAPI_ATTR void VKAPI_CALL as_CmdDrawIndexedIndirectCount(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, VkBuffer cbuf,
                                                                VkDeviceSize co, uint32_t m, uint32_t s)
{
    vkb_async *a = cur();
    SKIP(cb);
    a->draw_indexed_indirect_count(cb, b, o, cbuf, co, m, s);
}

static VKAPI_ATTR void VKAPI_CALL as_CmdDrawMultiEXT(VkCommandBuffer cb, uint32_t n, const VkMultiDrawInfoEXT *d, uint32_t ic, uint32_t fi,
                                                    uint32_t s)
{
    vkb_async *a = cur();
    SKIP(cb);
    a->draw_multi(cb, n, d, ic, fi, s);
}

static VKAPI_ATTR void VKAPI_CALL as_CmdDrawMultiIndexedEXT(VkCommandBuffer cb, uint32_t n, const VkMultiDrawIndexedInfoEXT *d, uint32_t ic,
                                                           uint32_t fi, uint32_t s, const int32_t *vo)
{
    vkb_async *a = cur();
    SKIP(cb);
    a->draw_multi_indexed(cb, n, d, ic, fi, s, vo);
}

static VKAPI_ATTR void VKAPI_CALL as_CmdDrawIndirectByteCountEXT(VkCommandBuffer cb, uint32_t ic, uint32_t fi, VkBuffer b, VkDeviceSize o,
                                                                uint32_t co, uint32_t vs)
{
    vkb_async *a = cur();
    SKIP(cb);
    a->draw_byte_count(cb, ic, fi, b, o, co, vs);
}

/* ------------------------------------------------------------------ install */

#define WRAP(field, saved, fn)              \
    do {                                    \
        if (dt->field) {                    \
            a->saved = dt->field;           \
            dt->field = fn;                 \
        }                                   \
    } while (0)
#define UNWRAP(field, saved, fn)                              \
    do {                                                      \
        if (dt->field == fn) dt->field = a->saved;            \
    } while (0)

void vkb_async_device_init(vkb_srv_table *t)
{
    const char *on_s = vkb_srv_env("VKBRIDGE_ASYNC_COMPILE");
    const char *im_s = vkb_srv_env("VKBRIDGE_ASYNC_COMPILE_IMITATE_MS");
    const char *w_s = vkb_srv_env("VKBRIDGE_ASYNC_COMPILE_WAIT_MS");
    const char *n_s = vkb_srv_env("VKBRIDGE_ASYNC_COMPILE_THREADS");
    int on = on_s && *on_s ? *on_s != '0' : (vkb_mode() == VKB_MODE_PERFORMANCE);
    double im = im_s ? atof(im_s) : 0;
    vkb_dispatch *dt = &t->dt;
    if ((!on && im <= 0) || !dt->vkCreateGraphicsPipelines || !dt->vkDestroyPipeline || !dt->vkCmdBindPipeline) return;
    pthread_once(&once, init_once);
    vkb_async *a = calloc(1, sizeof(*a));
    if (!a) return;
    a->t = t;
    a->on = on;
    a->fullscreen = on && on_s && atoi(on_s) >= 2;
    a->imitate_ms = im > 0 ? im : 0;
    a->wait_ms = w_s && *w_s ? atof(w_s) : ASYNC_WAIT_MS_DEFAULT;
    if (a->wait_ms < 0) a->wait_ms = 0;
    a->last_log = now_s();
    int threads = n_s && *n_s ? atoi(n_s) : ASYNC_THREADS_DEFAULT;
    threads = threads < 1 ? 1 : threads > ASYNC_THREADS_MAX ? ASYNC_THREADS_MAX : threads;
    WRAP(vkCreateGraphicsPipelines, gfx, as_CreateGraphicsPipelines);
    if (on) {
        WRAP(vkDestroyPipeline, destroy_pipe, as_DestroyPipeline);
        if (dt->vkCreateShaderModule && dt->vkDestroyShaderModule && dt->vkDestroyPipelineLayout && dt->vkDestroyRenderPass &&
            dt->vkBeginCommandBuffer) {
            WRAP(vkCreateShaderModule, create_mod, as_CreateShaderModule);
            WRAP(vkDestroyShaderModule, destroy_mod, as_DestroyShaderModule);
            WRAP(vkDestroyPipelineLayout, destroy_layout, as_DestroyPipelineLayout);
            WRAP(vkDestroyRenderPass, destroy_rp, as_DestroyRenderPass);
            WRAP(vkBeginCommandBuffer, begin, as_BeginCommandBuffer);
        } else {
            a->on = 0; /* cannot keep the objects a compile names: nothing in the background */
        }
        WRAP(vkCmdBindPipeline, bind, as_CmdBindPipeline);
        WRAP(vkCmdDraw, draw, as_CmdDraw);
        WRAP(vkCmdDrawIndexed, draw_indexed, as_CmdDrawIndexed);
        WRAP(vkCmdDrawIndirect, draw_indirect, as_CmdDrawIndirect);
        WRAP(vkCmdDrawIndexedIndirect, draw_indexed_indirect, as_CmdDrawIndexedIndirect);
        WRAP(vkCmdDrawIndirectCount, draw_indirect_count, as_CmdDrawIndirectCount);
        WRAP(vkCmdDrawIndexedIndirectCount, draw_indexed_indirect_count, as_CmdDrawIndexedIndirectCount);
        WRAP(vkCmdDrawMultiEXT, draw_multi, as_CmdDrawMultiEXT);
        WRAP(vkCmdDrawMultiIndexedEXT, draw_multi_indexed, as_CmdDrawMultiIndexedEXT);
        WRAP(vkCmdDrawIndirectByteCountEXT, draw_byte_count, as_CmdDrawIndirectByteCountEXT);
    }
    pthread_mutex_lock(&lk);
    if (threads > want_workers) want_workers = threads; /* one pool for every device; started at the first compile */
    a->next = devs;
    devs = a;
    pthread_mutex_unlock(&lk);
    t->async = a;
    if (a->on)
        VKB_INFO("device %u: asynchronous pipeline compile on (VKBRIDGE_ASYNC_COMPILE=%d): %d worker thread(s), %.1f ms wait%s%s", t->id,
                 a->fullscreen ? 2 : 1, want_workers, a->wait_ms, a->fullscreen ? "; pipelines without vertex input too" : "",
                 a->imitate_ms > 0 ? "; compiles slowed down (VKBRIDGE_ASYNC_COMPILE_IMITATE_MS)" : "");
    else
        VKB_INFO("device %u: graphics pipeline compiles slowed down by %.0f ms (VKBRIDGE_ASYNC_COMPILE_IMITATE_MS)%s", t->id, a->imitate_ms,
                 on ? "; asynchronous compile not possible here" : "");
}

void vkb_async_device_destroy(vkb_srv_table *t)
{
    vkb_async *a = t->async;
    if (!a) return;
    /* queued compiles of this device are dropped, running ones waited for */
    apipe *dropped = NULL;
    pthread_mutex_lock(&lk);
    for (apipe **pp = &q_head; *pp;) {
        apipe *x = *pp;
        if (x->a != a) {
            pp = &x->qnext;
            continue;
        }
        *pp = x->qnext;
        a->queued--;
        x->qnext = dropped;
        dropped = x;
    }
    q_tail = NULL;
    for (apipe *x = q_head; x; x = x->qnext) q_tail = x;
    while (a->running) pthread_cond_wait(&done_cv, &lk);
    pthread_mutex_unlock(&lk);
    while (dropped) {
        apipe *x = dropped;
        dropped = x->qnext;
        free(x->blob);
        x->blob = NULL;
        release_refs(x); /* the objects the client destroyed meanwhile go now */
        pthread_mutex_lock(&lk);
        x->state = ST_FAILED;
        int mine = x->exposed; /* an unexposed one belongs to a create still waiting (an app racing its own vkDestroyDevice) */
        if (mine && !x->destroyed) proxy_remove(x);
        pthread_cond_broadcast(&done_cv);
        pthread_mutex_unlock(&lk);
        if (mine) free(x);
    }
    /* stand-ins the app did not destroy (the driver frees their pipelines with the device), and the
     * device's object records */
    pthread_mutex_lock(&lk);
    for (int b = 0; b < PROXY_BUCKETS; b++)
        for (apipe **pp = &proxies[b]; *pp;) {
            apipe *x = *pp;
            if (x->a != a) {
                pp = &x->hnext;
                continue;
            }
            *pp = x->hnext;
            __atomic_sub_fetch(&nproxies, 1, __ATOMIC_RELEASE);
            free(x);
        }
    for (int b = 0; b < OBJ_BUCKETS; b++)
        for (objref **pp = &objs[b]; *pp;) {
            objref *o = *pp;
            if (o->a != a) {
                pp = &o->next;
                continue;
            }
            *pp = o->next;
            free(o);
        }
    for (vkb_async **pp = &devs; *pp; pp = &(*pp)->next)
        if (*pp == a) {
            *pp = a->next;
            break;
        }
    pthread_mutex_unlock(&lk);
    uint64_t nsync = 0;
    for (int i = 0; i < WHY_COUNT; i++) nsync += a->n_sync[i];
    if (a->on && (a->n_background || a->n_quick || a->draws_skipped || nsync)) log_summary(a, "");
    vkb_dispatch *dt = &t->dt;
    UNWRAP(vkCreateGraphicsPipelines, gfx, as_CreateGraphicsPipelines);
    UNWRAP(vkDestroyPipeline, destroy_pipe, as_DestroyPipeline);
    UNWRAP(vkCreateShaderModule, create_mod, as_CreateShaderModule);
    UNWRAP(vkDestroyShaderModule, destroy_mod, as_DestroyShaderModule);
    UNWRAP(vkDestroyPipelineLayout, destroy_layout, as_DestroyPipelineLayout);
    UNWRAP(vkDestroyRenderPass, destroy_rp, as_DestroyRenderPass);
    UNWRAP(vkBeginCommandBuffer, begin, as_BeginCommandBuffer);
    UNWRAP(vkCmdBindPipeline, bind, as_CmdBindPipeline);
    UNWRAP(vkCmdDraw, draw, as_CmdDraw);
    UNWRAP(vkCmdDrawIndexed, draw_indexed, as_CmdDrawIndexed);
    UNWRAP(vkCmdDrawIndirect, draw_indirect, as_CmdDrawIndirect);
    UNWRAP(vkCmdDrawIndexedIndirect, draw_indexed_indirect, as_CmdDrawIndexedIndirect);
    UNWRAP(vkCmdDrawIndirectCount, draw_indirect_count, as_CmdDrawIndirectCount);
    UNWRAP(vkCmdDrawIndexedIndirectCount, draw_indexed_indirect_count, as_CmdDrawIndexedIndirectCount);
    UNWRAP(vkCmdDrawMultiEXT, draw_multi, as_CmdDrawMultiEXT);
    UNWRAP(vkCmdDrawMultiIndexedEXT, draw_multi_indexed, as_CmdDrawMultiIndexedEXT);
    UNWRAP(vkCmdDrawIndirectByteCountEXT, draw_byte_count, as_CmdDrawIndirectByteCountEXT);
    t->async = NULL;
    free(a);
}
