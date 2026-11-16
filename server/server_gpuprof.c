/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * Evidence: where the GPU's time goes. With VKBRIDGE_GPU_PROF=1 (<cache dir>/env, read at device
 * creation) every primary command buffer gets timestamps at its start, around each render pass and
 * at its end; when the command buffer is begun again (so its last run is over) the spans are read
 * back and summed by render pass shape - extent, attachment formats - and every 5 s the shapes
 * costing most GPU time are logged with their draw and primitive counts. Spans between passes
 * are split by what they hold (dispatches, uploads with the BCn decoder's work, copies, clears; see
 * OP_*) and summed by that, with their barrier counts. Passes also show which attachments lack
 * framebuffer compression (AFBC, asked through VK_EXT_image_compression_control, which the profiler
 * turns on) or have STORAGE usage, and a census of images made with and without AFBC by format
 * follows each summary. Bottom-of-pipe stamps
 * serialize the passes a little, so the total runs slightly over the unprofiled frame time.
 * The costliest passes' pipelines are logged with their stages' SPIR-V hashes (with dump-spirv on,
 * saved as dump-spirv/g-<hash>.spv). Subnautica (2026-10-12): GPU 98 % busy at 1300 MHz for 6 fps
 * on 1200 draws in 52 passes at 1280x720, and DXVK's HUD could not say which; this showed a
 * 160x128 pass taking 75 + 37 ms a frame, its draws DXVK's stream output geometry shaders (fixed by
 * the fold in server_emu_shader.c). VKBRIDGE_GPU_PROF=2 also splits passes by their first pipeline,
 * ends a run of dispatches where the compute pipeline changes (otherwise the run shares one span, billed to
 * its first pipeline whatever the others cost; one pipeline's dispatches back to back stay one span, so a
 * command buffer of hundreds of dispatches keeps within its QUERIES stamps) and saves every module's SPIR-V
 * under dump-spirv/ by that hash. A command buffer that runs out of stamps is measured up to there; the
 * report says how many did ("cut short").
 * Subnautica paused underwater after the fold (2026-10-12, ~23 ms GPU a frame at ~33 fps): the split
 * spans showed no bridge work of note (no uploads, 0.5 ms of the game's copies; the dispatches are
 * its ocean FFT), every attachment and decoded BC texture kept AFBC, and =2 named the costliest
 * draw a full-screen ray march (2.2 ms, screen-space reflections).
 * Reading and reporting (2026-11-08, after Lies of P's vkd3d-proton frames gave no line in 3.5 minutes): a finished command
 * buffer is read back by a timer thread (every 100 ms, results fetched without waiting: not ready yet is tried again, for 10 s),
 * or when it is begun again or freed, whichever comes first; the summary is printed by that thread every 5 s whether or not
 * anything was read (so silence means the profiler is off, and every way of being off logs one line saying why). vkd3d-proton
 * allocates and frees command buffers, passes its draws through vkCmdDrawIndirectCount / vkCmdDrawMultiEXT and its compute
 * through vkCmdDispatchBase: counted. A primary begun with a pInheritanceInfo is read as primary (the level is noted at
 * allocation). A recording that was never submitted is told from the one read before by its first and last stamps.
 * VKBRIDGE_GPU_PROF=counters adds Mali's hardware counters (server_hwcnt.c: why the GPU is busy -
 * which queue, which shader core unit, external memory), =counters-only reads those alone.
 * Off unless asked for.
 */
#define _GNU_SOURCE
#include "vkb_server.h"
#include "server_emu.h"

#include <stdio.h>
#include <unistd.h>

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define DROP_S 10.0 /* a finished command buffer whose stamps never come back (never submitted) is let go after this */
#define POLL_BUDGET 256 /* pending command buffers tried per 100 ms */
#define QUERIES 512
#define SEGS (QUERIES - 1)
#define CB_BUCKETS 512
#define VIEW_BUCKETS 4096
#define KEYS 512
#define REPORT_S 5.0

enum { SEG_BETWEEN, SEG_PASS };
/* What a span between passes was opened by: each change of kind stamps, so the transfers, the
 * dispatches and the BCn decoder (inside UPLOAD: it runs in the upload's place) get their own
 * spans; consecutive commands of one kind share one. */
enum { OP_NONE, OP_DISPATCH, OP_UPLOAD, OP_IMGCOPY, OP_BUFCOPY, OP_CLEAR, OP_COUNT };
static const char *const op_name[OP_COUNT] = {"idle/barriers", "dispatches", "buffer->image uploads", "image copies/blits",
                                              "buffer copies/fills", "clears"};

typedef struct gp_seg {
    uint8_t kind, ncolor, load, msaa, sub;
    uint8_t nocomp, storage; /* attachments (bit i color i, 0x80 depth) without AFBC / with STORAGE usage */
    uint32_t w, h;
    uint32_t ops, barriers;
    uint32_t fmt[2], dfmt;
    uint32_t draws, dispatches;
    uint32_t icalls;  /* indirect-count draw calls (vkCmdDrawIndirectCount, vkCmdDrawIndexedIndirectCount): not in draws */
    uint64_t imax;    /* the most draws those calls could make (their maxDrawCount) */
    uint64_t prims; /* vertices or indices x instances; indirect draws count 0 */
    VkPipeline bound;
    VkPipeline pipe[4]; /* the first pipelines drawn with, and their draws and prims */
    uint32_t pdraws[4];
    uint64_t pprims[4];
} gp_seg;

typedef struct gp_cb {
    VkCommandBuffer cb;
    VkQueryPool pool;
    uint32_t n; /* stamps written */
    int on, secondary;
    int lvl_known, lvl_secondary; /* from vkAllocateCommandBuffers */
    VkCommandPool cmdpool;
    int pending;      /* ended, not yet read */
    double ended_at;
    int have_last;    /* the first and last stamps of the run read last: a read equal to them is that run again (the pool is reset only when the buffer runs) */
    uint64_t last_t0, last_t1;
    struct gp_cb *pnext; /* the pending list */
    int cut; /* ran out of stamps: what came after is in no span */
    VkPipeline bound, cbound; /* graphics, compute */
    gp_seg seg[SEGS];
    struct gp_cb *next;
} gp_cb;

typedef struct gp_view {
    VkImageView v;
    uint32_t fmt, samples;
    uint8_t nocomp, storage; /* the image: made without framebuffer compression (AFBC), with STORAGE usage */
    struct gp_view *next;
} gp_view;

typedef struct gp_img {
    VkImage i;
    uint32_t samples;
    uint8_t nocomp, storage;
    struct gp_img *next;
} gp_img;

typedef struct gp_pipe {
    VkPipeline p;
    char desc[200]; /* stages with their SPIR-V hashes (dump-spirv/g-<hash>.spv), topology, discard */
    struct gp_pipe *next;
} gp_pipe;

typedef struct gp_mod {
    VkShaderModule m;
    uint64_t hash;
    struct gp_mod *next;
} gp_mod;

typedef struct gp_key {
    gp_seg k; /* shape fields only */
    gp_seg worst; /* the costliest instance, with its pipelines */
    uint64_t worst_ns;
    uint64_t ns, prims, imax;
    uint32_t count, draws, dispatches, ops, barriers, icalls;
} gp_key;

typedef struct vkb_gprof {
    vkb_srv_table *t;
    VkDevice dev;
    pthread_mutex_t lock;
    double period_ns;
    uint64_t mask;
    gp_cb *cbs[CB_BUCKETS];
    gp_view *views[VIEW_BUCKETS];
    gp_img *imgs[VIEW_BUCKETS];
    gp_pipe *pipes[VIEW_BUCKETS];
    gp_mod *mods[VIEW_BUCKETS];
    gp_key keys[KEYS];
    uint32_t nkeys, cbs_read, cbs_dropped, cbs_cut;
    uint32_t cbs_begun, cbs_ended, cbs_freed, cbs_secondary; /* since the last summary */
    uint32_t cbs_pending_n;
    gp_cb *pending;
    double period_s;
    pthread_t thread;
    int thread_on, stop;
    uint32_t fallback; /* hook calls that found no table of the thread and used the last profiled device */
    uint32_t made[256][2], made_new; /* images made by core format: with AFBC, without */
    uint64_t total_ns;
    double since;
    PFN_vkCreateQueryPool create_pool;
    PFN_vkDestroyQueryPool destroy_pool;
    PFN_vkGetQueryPoolResults results;
    PFN_vkCmdResetQueryPool reset;
    PFN_vkCmdWriteTimestamp stamp;
    PFN_vkBeginCommandBuffer begin;
    PFN_vkEndCommandBuffer end;
    PFN_vkCmdBeginRendering begin_rendering;
    PFN_vkCmdEndRendering end_rendering;
    PFN_vkCmdBeginRenderPass begin_pass;
    PFN_vkCmdBeginRenderPass2 begin_pass2;
    PFN_vkCmdEndRenderPass end_pass;
    PFN_vkCmdEndRenderPass2 end_pass2;
    PFN_vkCmdBindPipeline bind;
    PFN_vkCmdExecuteCommands execute;
    PFN_vkCmdDraw draw;
    PFN_vkCmdDrawIndexed draw_indexed;
    PFN_vkCmdDrawIndirect draw_indirect;
    PFN_vkCmdDrawIndexedIndirect draw_indexed_indirect;
    PFN_vkCmdDispatch dispatch;
    PFN_vkCmdDispatchIndirect dispatch_indirect;
    PFN_vkCmdDispatchBase dispatch_base;
    PFN_vkCmdDrawIndirectCount draw_indirect_count;
    PFN_vkCmdDrawIndexedIndirectCount draw_indexed_indirect_count;
    PFN_vkCmdDrawMultiEXT draw_multi;
    PFN_vkCmdDrawMultiIndexedEXT draw_multi_indexed;
    PFN_vkCmdDrawIndirectByteCountEXT draw_bytecount;
    PFN_vkAllocateCommandBuffers alloc_cbs;
    PFN_vkFreeCommandBuffers free_cbs;
    PFN_vkDestroyCommandPool destroy_cmdpool;
    PFN_vkCreateGraphicsPipelines gfx;
    PFN_vkCreateComputePipelines comp;
    PFN_vkCreateShaderModule create_mod;
    PFN_vkCreateImageView create_view;
    PFN_vkDestroyImageView destroy_view;
    PFN_vkCreateImage create_img;
    PFN_vkDestroyImage destroy_img;
    PFN_vkGetImageSubresourceLayout2KHR layout2; /* with VK_EXT_image_compression_control: AFBC or not */
    struct {
        PFN_vkCmdCopyBufferToImage vkCmdCopyBufferToImage;
        PFN_vkCmdCopyBufferToImage2 vkCmdCopyBufferToImage2;
        PFN_vkCmdCopyImage vkCmdCopyImage;
        PFN_vkCmdCopyImage2 vkCmdCopyImage2;
        PFN_vkCmdBlitImage vkCmdBlitImage;
        PFN_vkCmdBlitImage2 vkCmdBlitImage2;
        PFN_vkCmdResolveImage vkCmdResolveImage;
        PFN_vkCmdResolveImage2 vkCmdResolveImage2;
        PFN_vkCmdCopyImageToBuffer vkCmdCopyImageToBuffer;
        PFN_vkCmdCopyImageToBuffer2 vkCmdCopyImageToBuffer2;
        PFN_vkCmdCopyBuffer vkCmdCopyBuffer;
        PFN_vkCmdCopyBuffer2 vkCmdCopyBuffer2;
        PFN_vkCmdFillBuffer vkCmdFillBuffer;
        PFN_vkCmdUpdateBuffer vkCmdUpdateBuffer;
        PFN_vkCmdClearColorImage vkCmdClearColorImage;
        PFN_vkCmdClearDepthStencilImage vkCmdClearDepthStencilImage;
        PFN_vkCmdPipelineBarrier vkCmdPipelineBarrier;
        PFN_vkCmdPipelineBarrier2 vkCmdPipelineBarrier2;
    } o;
} vkb_gprof;

static vkb_gprof *profs[4096];
static int by_pipe; /* VKBRIDGE_GPU_PROF=2: passes are told apart by their first pipeline too */
static vkb_gprof *last;

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static unsigned hk(uint64_t k, unsigned buckets)
{
    k ^= k >> 31;
    k *= 0x9e3779b97f4a7c15ull;
    return (unsigned)(k >> 40) & (buckets - 1);
}

static vkb_gprof *cur(void)
{
    vkb_srv_table *t = vkb_srv_current_table();
    vkb_gprof *g = t && t->id < 4096 ? profs[t->id] : NULL;
    if (g) return g;
    g = __atomic_load_n(&last, __ATOMIC_ACQUIRE);
    if (g) __atomic_fetch_add(&g->fallback, 1, __ATOMIC_RELAXED);
    return g;
}

/* Called with the lock held. */
static gp_cb *cb_get(vkb_gprof *g, VkCommandBuffer cb, int create)
{
    unsigned b = hk((uint64_t)(uintptr_t)cb, CB_BUCKETS);
    for (gp_cb *e = g->cbs[b]; e; e = e->next)
        if (e->cb == cb) return e;
    if (!create) return NULL;
    gp_cb *e = calloc(1, sizeof(*e));
    if (!e) return NULL;
    e->cb = cb;
    e->next = g->cbs[b];
    g->cbs[b] = e;
    return e;
}

static gp_view *view_get(vkb_gprof *g, VkImageView v)
{
    for (gp_view *e = g->views[hk((uint64_t)v, VIEW_BUCKETS)]; e; e = e->next)
        if (e->v == v) return e;
    return NULL;
}

static int same_shape(const gp_seg *a, const gp_seg *b)
{
    return a->kind == b->kind && a->w == b->w && a->h == b->h && a->ncolor == b->ncolor && a->fmt[0] == b->fmt[0] &&
           a->fmt[1] == b->fmt[1] && a->dfmt == b->dfmt && a->load == b->load && a->msaa == b->msaa && a->sub == b->sub &&
           a->nocomp == b->nocomp && a->storage == b->storage && (!by_pipe || a->pipe[0] == b->pipe[0]);
}

static int by_ns(const void *a, const void *b)
{
    uint64_t x = ((const gp_key *)a)->ns, y = ((const gp_key *)b)->ns;
    return x < y ? 1 : x > y ? -1 : 0;
}

static void report(vkb_gprof *g, double now)
{
    double span = now - g->since;
    qsort(g->keys, g->nkeys, sizeof(g->keys[0]), by_ns);
    char cut[64] = "";
    if (g->cbs_cut) snprintf(cut, sizeof(cut), ", %u cut short at %d stamps", g->cbs_cut, QUERIES);
    VKB_INFO("gpu prof %.1fs: %.1f ms GPU/s in %u command buffers (%u unread%s), %u pass shapes", span,
             g->total_ns / 1e6 / span, g->cbs_read, g->cbs_dropped, cut, g->nkeys);
    VKB_INFO("  gpu prof device %u: %u primary begun, %u ended, %u freed, %u secondary begun, %u waiting to be read, %u calls without a device table",
             g->t->id, g->cbs_begun, g->cbs_ended, g->cbs_freed, g->cbs_secondary, g->cbs_pending_n,
             __atomic_exchange_n(&g->fallback, 0, __ATOMIC_RELAXED));
    for (uint32_t i = 0; i < g->nkeys && i < (by_pipe ? 24u : 14u); i++) {
        const gp_key *k = &g->keys[i];
        if (k->k.kind == SEG_BETWEEN)
            VKB_INFO("  %6.1f ms/s %5.1f%% between passes, %s x%u: %.0f us each, %u commands, %u dispatches, %u barriers",
                     k->ns / 1e6 / span, 100.0 * k->ns / (g->total_ns ? g->total_ns : 1), op_name[k->k.sub], k->count,
                     k->ns / 1e3 / k->count, k->ops, k->dispatches, k->barriers);
        else
            VKB_INFO("  %6.1f ms/s %5.1f%% pass %ux%u c%u fmt %u,%u d%u msaa %u load 0x%x nocomp 0x%x storage 0x%x x%u: %.0f us each, "
                     "%u draws, %llu prims",
                     k->ns / 1e6 / span, 100.0 * k->ns / (g->total_ns ? g->total_ns : 1), k->k.w, k->k.h, k->k.ncolor,
                     k->k.fmt[0], k->k.fmt[1], k->k.dfmt, k->k.msaa, k->k.load, k->k.nocomp, k->k.storage, k->count,
                     k->ns / 1e3 / k->count, k->draws, (unsigned long long)k->prims);
        if (k->icalls)
            VKB_INFO("      %u indirect-count draw calls (up to %llu draws, not in the draw count above)", k->icalls, (unsigned long long)k->imax);
        if (k->k.kind == SEG_PASS && k->barriers) VKB_INFO("      %u barriers inside", k->barriers);
        if ((k->k.kind == SEG_PASS || k->dispatches) && i < (by_pipe ? 12u : 4u))
            for (int j = 0; j < (by_pipe ? 1 : 4) && k->worst.pipe[j]; j++)
            {
                gp_pipe *pp = g->pipes[hk((uint64_t)k->worst.pipe[j], VIEW_BUCKETS)];
                while (pp && pp->p != k->worst.pipe[j]) pp = pp->next;
                VKB_INFO("      worst %.0f us: pipeline %p %u draws/dispatches %llu prims/workgroups: %s", k->worst_ns / 1e3, (void *)k->worst.pipe[j],
                         k->worst.pdraws[j], (unsigned long long)k->worst.pprims[j], pp ? pp->desc : "not seen created");
            }
    }
    if (g->made_new) {
        char line[600];
        int len = 0;
        for (int f = 0; f < 256 && len < (int)sizeof(line) - 24; f++)
            if (g->made[f][0] || g->made[f][1]) len += snprintf(line + len, sizeof(line) - (size_t)len, " %d:%u/%u", f, g->made[f][0], g->made[f][0] + g->made[f][1]);
        VKB_INFO("  optimal-tiling images made so far, format:with AFBC/all:%s", line);
        g->made_new = 0;
    }
    g->nkeys = 0;
    g->total_ns = 0;
    g->cbs_read = g->cbs_dropped = g->cbs_cut = 0;
    g->cbs_begun = g->cbs_ended = g->cbs_freed = g->cbs_secondary = 0;
    g->since = now;
}

static void unpend(vkb_gprof *g, gp_cb *e)
{
    if (!e->pending) return;
    e->pending = 0;
    for (gp_cb **pp = &g->pending; *pp; pp = &(*pp)->pnext)
        if (*pp == e) {
            *pp = e->pnext;
            break;
        }
    e->pnext = NULL;
    if (g->cbs_pending_n) g->cbs_pending_n--;
}

/* Called with the lock held. Reads a finished command buffer's stamps without waiting. final: it is being begun again or freed,
 * so what is not readable now never will be (dropped); otherwise not readable is tried again until DROP_S old. */
static void consume(vkb_gprof *g, gp_cb *e, int final)
{
    if (!e->pending) return;
    if (e->n < 2) {
        unpend(g, e);
        return;
    }
    uint64_t ts[QUERIES];
    VkResult r = g->results(g->dev, e->pool, 0, e->n, sizeof(uint64_t) * e->n, ts, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
    int stale = r == VK_SUCCESS && e->have_last && ts[0] == e->last_t0 && ts[e->n - 1] == e->last_t1;
    if (r != VK_SUCCESS || stale) {
        if (!final && now_s() - e->ended_at < DROP_S) return;
        unpend(g, e);
        g->cbs_dropped++; /* recorded, never submitted (or its results lost with the pool) */
        return;
    }
    e->have_last = 1;
    e->last_t0 = ts[0];
    e->last_t1 = ts[e->n - 1];
    unpend(g, e);
    g->cbs_read++;
    if (e->cut) g->cbs_cut++;
    for (uint32_t i = 0; i + 1 < e->n; i++) {
        uint64_t d = ((ts[i + 1] - ts[i]) & g->mask);
        uint64_t ns = (uint64_t)(d * g->period_ns);
        if (ns > 2000000000ull) continue; /* wrapped or bogus */
        const gp_seg *s = &e->seg[i];
        if (s->kind == SEG_BETWEEN && !s->ops && !s->dispatches && ns < 2000) continue;
        g->total_ns += ns;
        gp_key *k = NULL;
        for (uint32_t j = 0; j < g->nkeys; j++)
            if (same_shape(&g->keys[j].k, s)) {
                k = &g->keys[j];
                break;
            }
        if (!k && g->nkeys < KEYS) {
            k = &g->keys[g->nkeys++];
            memset(k, 0, sizeof(*k));
            k->k = *s;
        }
        if (!k) continue;
        if (ns > k->worst_ns) {
            k->worst_ns = ns;
            k->worst = *s;
        }
        k->ns += ns;
        k->count++;
        k->draws += s->draws;
        k->dispatches += s->dispatches;
        k->prims += s->prims;
        k->icalls += s->icalls;
        k->imax += s->imax;
        k->ops += s->ops;
        k->barriers += s->barriers;
    }
}

/* Called with the lock held: the entry and its pool go (the command buffer is freed). */
static void cb_remove(vkb_gprof *g, gp_cb *e)
{
    consume(g, e, 1);
    unpend(g, e);
    unsigned b = hk((uint64_t)(uintptr_t)e->cb, CB_BUCKETS);
    for (gp_cb **pp = &g->cbs[b]; *pp; pp = &(*pp)->next)
        if (*pp == e) {
            *pp = e->next;
            break;
        }
    if (g->destroy_pool && e->pool) g->destroy_pool(g->dev, e->pool, NULL);
    free(e);
}

/* The timer thread: finished command buffers read (without waiting), the summary every period_s whether or not anything came. */
static void *gp_thread(void *arg)
{
    vkb_gprof *g = arg;
    while (!__atomic_load_n(&g->stop, __ATOMIC_ACQUIRE)) {
        usleep(100000);
        pthread_mutex_lock(&g->lock);
        gp_cb *e = g->pending;
        for (int budget = POLL_BUDGET; e && budget > 0; budget--) {
            gp_cb *next = e->pnext;
            consume(g, e, 0);
            e = next;
        }
        double now = now_s();
        if (now - g->since >= g->period_s) report(g, now);
        pthread_mutex_unlock(&g->lock);
    }
    return NULL;
}

/* Closes the open span with a stamp and opens the next. Outside render passes only. */
static gp_seg *next_seg(vkb_gprof *g, gp_cb *e, VkCommandBuffer cb)
{
    if (e->secondary) return NULL;
    if (!e->on || e->n >= QUERIES) {
        if (e->on) e->cut = 1;
        e->on = 0;
        return NULL;
    }
    g->stamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, e->pool, e->n);
    e->n++;
    if (e->n > SEGS) return NULL;
    gp_seg *s = &e->seg[e->n - 1];
    memset(s, 0, sizeof(*s));
    s->bound = e->bound;
    return s;
}

static gp_seg *open_seg(gp_cb *e)
{
    return e && e->on && e->n >= 1 && e->n <= SEGS ? &e->seg[e->n - 1] : NULL;
}

static VKAPI_ATTR VkResult VKAPI_CALL gp_BeginCommandBuffer(VkCommandBuffer cb, const VkCommandBufferBeginInfo *bi)
{
    vkb_gprof *g = cur();
    VkResult r = g->begin(cb, bi);
    if (r != VK_SUCCESS) return r;
    pthread_mutex_lock(&g->lock);
    gp_cb *e = cb_get(g, cb, 1);
    /* The level is what vkAllocateCommandBuffers said; a pInheritanceInfo on a primary is ignored by the driver and passed by some apps. */
    int sec = e ? (e->lvl_known ? e->lvl_secondary : (bi && bi->pInheritanceInfo && (bi->flags & VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT))) : 0;
    if (e && sec) {
        /* Secondary (DXVK records its render passes' draws in them): counts only, added to the
         * primary's open span when executed. */
        consume(g, e, 1);
        g->cbs_secondary++;
        e->secondary = 1;
        e->n = 1;
        e->on = 1;
        e->bound = VK_NULL_HANDLE;
        memset(&e->seg[0], 0, sizeof(e->seg[0]));
    } else if (e) {
        VkQueryPoolCreateInfo qi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, NULL, 0, VK_QUERY_TYPE_TIMESTAMP, QUERIES, 0};
        consume(g, e, 1);
        if (e->secondary) e->have_last = 0;
        e->secondary = 0;
        e->cut = 0;
        e->on = 0;
        e->n = 0;
        g->cbs_begun++;
        if (e->pool || g->create_pool(g->dev, &qi, NULL, &e->pool) == VK_SUCCESS) {
            g->reset(cb, e->pool, 0, QUERIES);
            e->on = 1;
            e->bound = VK_NULL_HANDLE;
            next_seg(g, e, cb);
        }
    }
    pthread_mutex_unlock(&g->lock);
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL gp_EndCommandBuffer(VkCommandBuffer cb)
{
    vkb_gprof *g = cur();
    pthread_mutex_lock(&g->lock);
    gp_cb *e = cb_get(g, cb, 0);
    if (e && e->on && !e->secondary) {
        next_seg(g, e, cb);
        e->on = 0;
        g->cbs_ended++;
        if (e->n >= 2 && !e->pending) {
            e->pending = 1;
            e->ended_at = now_s();
            e->pnext = g->pending;
            g->pending = e;
            g->cbs_pending_n++;
        }
    }
    pthread_mutex_unlock(&g->lock);
    return g->end(cb);
}

static VKAPI_ATTR VkResult VKAPI_CALL gp_AllocateCommandBuffers(VkDevice d, const VkCommandBufferAllocateInfo *ai, VkCommandBuffer *out)
{
    vkb_gprof *g = cur();
    VkResult r = g->alloc_cbs(d, ai, out);
    if (r != VK_SUCCESS || !ai) return r;
    pthread_mutex_lock(&g->lock);
    for (uint32_t i = 0; i < ai->commandBufferCount; i++) {
        gp_cb *e = cb_get(g, out[i], 1);
        if (!e) continue;
        unpend(g, e); /* a handle of a freed or destroyed buffer coming back */
        e->have_last = 0;
        e->on = 0;
        e->n = 0;
        e->secondary = 0;
        e->lvl_known = 1;
        e->lvl_secondary = ai->level == VK_COMMAND_BUFFER_LEVEL_SECONDARY;
        e->cmdpool = ai->commandPool;
    }
    pthread_mutex_unlock(&g->lock);
    return r;
}

static VKAPI_ATTR void VKAPI_CALL gp_FreeCommandBuffers(VkDevice d, VkCommandPool pool, uint32_t n, const VkCommandBuffer *cbs)
{
    vkb_gprof *g = cur();
    pthread_mutex_lock(&g->lock);
    for (uint32_t i = 0; cbs && i < n; i++) {
        gp_cb *e = cb_get(g, cbs[i], 0);
        if (e) {
            g->cbs_freed++;
            cb_remove(g, e);
        }
    }
    pthread_mutex_unlock(&g->lock);
    g->free_cbs(d, pool, n, cbs);
}

/* Its command buffers are freed with it. */
static VKAPI_ATTR void VKAPI_CALL gp_DestroyCommandPool(VkDevice d, VkCommandPool pool, const VkAllocationCallbacks *a)
{
    vkb_gprof *g = cur();
    pthread_mutex_lock(&g->lock);
    for (int b = 0; b < CB_BUCKETS; b++)
        for (gp_cb *e = g->cbs[b], *nx; e; e = nx) {
            nx = e->next;
            if (e->cmdpool == pool && pool != VK_NULL_HANDLE) {
                g->cbs_freed++;
                cb_remove(g, e);
            }
        }
    pthread_mutex_unlock(&g->lock);
    g->destroy_cmdpool(d, pool, a);
}

static void pass_begin(vkb_gprof *g, VkCommandBuffer cb, const VkRenderingInfo *ri, const VkRenderPassBeginInfo *bi)
{
    pthread_mutex_lock(&g->lock);
    gp_cb *e = cb_get(g, cb, 0);
    gp_seg *s = e ? next_seg(g, e, cb) : NULL;
    if (s) {
        s->kind = SEG_PASS;
        if (ri) {
            s->w = ri->renderArea.extent.width;
            s->h = ri->renderArea.extent.height;
            s->ncolor = (uint8_t)ri->colorAttachmentCount;
            for (uint32_t i = 0; i < ri->colorAttachmentCount; i++) {
                const VkRenderingAttachmentInfo *a = &ri->pColorAttachments[i];
                gp_view *v = a->imageView ? view_get(g, a->imageView) : NULL;
                if (i < 2) s->fmt[i] = v ? v->fmt : 0;
                if (v && v->samples > s->msaa) s->msaa = (uint8_t)v->samples;
                if (a->imageView && a->loadOp == VK_ATTACHMENT_LOAD_OP_LOAD && i < 7) s->load |= 1u << i;
                if (v && v->nocomp && i < 7) s->nocomp |= 1u << i;
                if (v && v->storage && i < 7) s->storage |= 1u << i;
            }
            const VkRenderingAttachmentInfo *d = ri->pDepthAttachment;
            if (d && d->imageView) {
                gp_view *v = view_get(g, d->imageView);
                s->dfmt = v ? v->fmt : 1;
                if (d->loadOp == VK_ATTACHMENT_LOAD_OP_LOAD) s->load |= 0x80;
                if (v && v->nocomp) s->nocomp |= 0x80;
                if (v && v->storage) s->storage |= 0x80;
            }
        } else if (bi) {
            s->w = bi->renderArea.extent.width;
            s->h = bi->renderArea.extent.height;
        }
    }
    pthread_mutex_unlock(&g->lock);
}

static void pass_end(vkb_gprof *g, VkCommandBuffer cb)
{
    pthread_mutex_lock(&g->lock);
    gp_cb *e = cb_get(g, cb, 0);
    if (e) next_seg(g, e, cb);
    pthread_mutex_unlock(&g->lock);
}

static VKAPI_ATTR void VKAPI_CALL gp_CmdBeginRendering(VkCommandBuffer cb, const VkRenderingInfo *ri)
{
    vkb_gprof *g = cur();
    if (!(ri && (ri->flags & VK_RENDERING_RESUMING_BIT))) pass_begin(g, cb, ri, NULL);
    g->begin_rendering(cb, ri);
}

static VKAPI_ATTR void VKAPI_CALL gp_CmdEndRendering(VkCommandBuffer cb)
{
    vkb_gprof *g = cur();
    g->end_rendering(cb);
    pass_end(g, cb);
}

static VKAPI_ATTR void VKAPI_CALL gp_CmdBeginRenderPass(VkCommandBuffer cb, const VkRenderPassBeginInfo *bi, VkSubpassContents c)
{
    vkb_gprof *g = cur();
    pass_begin(g, cb, NULL, bi);
    g->begin_pass(cb, bi, c);
}

static VKAPI_ATTR void VKAPI_CALL gp_CmdBeginRenderPass2(VkCommandBuffer cb, const VkRenderPassBeginInfo *bi, const VkSubpassBeginInfo *si)
{
    vkb_gprof *g = cur();
    pass_begin(g, cb, NULL, bi);
    g->begin_pass2(cb, bi, si);
}

static VKAPI_ATTR void VKAPI_CALL gp_CmdEndRenderPass(VkCommandBuffer cb)
{
    vkb_gprof *g = cur();
    g->end_pass(cb);
    pass_end(g, cb);
}

static VKAPI_ATTR void VKAPI_CALL gp_CmdEndRenderPass2(VkCommandBuffer cb, const VkSubpassEndInfo *ei)
{
    vkb_gprof *g = cur();
    g->end_pass2(cb, ei);
    pass_end(g, cb);
}

static void add_pipe(gp_seg *s, VkPipeline p, uint32_t draws, uint64_t prims)
{
    for (int j = 0; j < 4; j++) {
        if (s->pipe[j] && s->pipe[j] != p) continue;
        s->pipe[j] = p;
        s->pdraws[j] += draws;
        s->pprims[j] += prims;
        return;
    }
}

static void count2(vkb_gprof *g, VkCommandBuffer cb, uint32_t draws, uint32_t dispatches, uint64_t prims, uint32_t icalls, uint64_t imax)
{
    pthread_mutex_lock(&g->lock);
    gp_seg *s = open_seg(cb_get(g, cb, 0));
    if (s) {
        s->icalls += icalls;
        s->imax += imax;
        if (icalls) add_pipe(s, s->bound, icalls, 0);
        s->draws += draws;
        s->dispatches += dispatches;
        s->prims += prims;
        if (draws) add_pipe(s, s->bound, draws, prims);
        else if (dispatches) add_pipe(s, cb_get(g, cb, 0)->cbound, dispatches, prims);
    }
    pthread_mutex_unlock(&g->lock);
}

static void count(vkb_gprof *g, VkCommandBuffer cb, uint32_t draws, uint32_t dispatches, uint64_t prims)
{
    count2(g, cb, draws, dispatches, prims, 0, 0);
}

static VKAPI_ATTR void VKAPI_CALL gp_CmdExecuteCommands(VkCommandBuffer cb, uint32_t n, const VkCommandBuffer *cbs)
{
    vkb_gprof *g = cur();
    pthread_mutex_lock(&g->lock);
    gp_seg *s = open_seg(cb_get(g, cb, 0));
    for (uint32_t i = 0; s && i < n; i++) {
        gp_cb *x = cb_get(g, cbs[i], 0);
        if (!x || !x->secondary) continue;
        const gp_seg *y = &x->seg[0];
        s->draws += y->draws;
        s->dispatches += y->dispatches;
        s->prims += y->prims;
        s->icalls += y->icalls;
        s->imax += y->imax;
        for (int j = 0; j < 4 && y->pipe[j]; j++) add_pipe(s, y->pipe[j], y->pdraws[j], y->pprims[j]);
    }
    pthread_mutex_unlock(&g->lock);
    g->execute(cb, n, cbs);
}

static VKAPI_ATTR void VKAPI_CALL gp_CmdBindPipeline(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipeline p)
{
    vkb_gprof *g = cur();
    if (bp == VK_PIPELINE_BIND_POINT_GRAPHICS) {
        pthread_mutex_lock(&g->lock);
        gp_cb *e = cb_get(g, cb, 0);
        if (e) e->bound = p;
        gp_seg *s = open_seg(e);
        if (s) s->bound = p;
        pthread_mutex_unlock(&g->lock);
    } else if (bp == VK_PIPELINE_BIND_POINT_COMPUTE) {
        pthread_mutex_lock(&g->lock);
        gp_cb *e = cb_get(g, cb, 0);
        if (e) e->cbound = p;
        pthread_mutex_unlock(&g->lock);
    }
    g->bind(cb, bp, p);
}

static VKAPI_ATTR void VKAPI_CALL gp_CmdDraw(VkCommandBuffer cb, uint32_t vc, uint32_t ic, uint32_t fv, uint32_t fi)
{
    vkb_gprof *g = cur();
    count(g, cb, 1, 0, (uint64_t)vc * ic);
    g->draw(cb, vc, ic, fv, fi);
}

static VKAPI_ATTR void VKAPI_CALL gp_CmdDrawIndexed(VkCommandBuffer cb, uint32_t ic, uint32_t inst, uint32_t fi, int32_t vo, uint32_t fin)
{
    vkb_gprof *g = cur();
    count(g, cb, 1, 0, (uint64_t)ic * inst);
    g->draw_indexed(cb, ic, inst, fi, vo, fin);
}

static VKAPI_ATTR void VKAPI_CALL gp_CmdDrawIndirect(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, uint32_t n, uint32_t st)
{
    vkb_gprof *g = cur();
    count(g, cb, n, 0, 0);
    g->draw_indirect(cb, b, o, n, st);
}

static VKAPI_ATTR void VKAPI_CALL gp_CmdDrawIndexedIndirect(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, uint32_t n, uint32_t st)
{
    vkb_gprof *g = cur();
    count(g, cb, n, 0, 0);
    g->draw_indexed_indirect(cb, b, o, n, st);
}

/* vkd3d-proton's draws: ExecuteIndirect and its multi-draw and stream-out forms. The count is read by the GPU: the calls are counted,
 * with the most draws they could make. */
static VKAPI_ATTR void VKAPI_CALL gp_CmdDrawIndirectCount(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, VkBuffer cbuf, VkDeviceSize co, uint32_t max, uint32_t st)
{
    vkb_gprof *g = cur();
    count2(g, cb, 0, 0, 0, 1, max);
    g->draw_indirect_count(cb, b, o, cbuf, co, max, st);
}

static VKAPI_ATTR void VKAPI_CALL gp_CmdDrawIndexedIndirectCount(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, VkBuffer cbuf, VkDeviceSize co, uint32_t max,
                                                                 uint32_t st)
{
    vkb_gprof *g = cur();
    count2(g, cb, 0, 0, 0, 1, max);
    g->draw_indexed_indirect_count(cb, b, o, cbuf, co, max, st);
}

static VKAPI_ATTR void VKAPI_CALL gp_CmdDrawMultiEXT(VkCommandBuffer cb, uint32_t n, const VkMultiDrawInfoEXT *v, uint32_t inst, uint32_t fi, uint32_t st)
{
    vkb_gprof *g = cur();
    uint64_t prims = 0;
    for (uint32_t i = 0; v && i < n; i++) prims += (uint64_t)((const VkMultiDrawInfoEXT *)((const char *)v + (size_t)i * (st ? st : sizeof(*v))))->vertexCount * inst;
    count(g, cb, n, 0, prims);
    g->draw_multi(cb, n, v, inst, fi, st);
}

static VKAPI_ATTR void VKAPI_CALL gp_CmdDrawMultiIndexedEXT(VkCommandBuffer cb, uint32_t n, const VkMultiDrawIndexedInfoEXT *v, uint32_t inst, uint32_t fi,
                                                            uint32_t st, const int32_t *vo)
{
    vkb_gprof *g = cur();
    uint64_t prims = 0;
    for (uint32_t i = 0; v && i < n; i++) prims += (uint64_t)((const VkMultiDrawIndexedInfoEXT *)((const char *)v + (size_t)i * (st ? st : sizeof(*v))))->indexCount * inst;
    count(g, cb, n, 0, prims);
    g->draw_multi_indexed(cb, n, v, inst, fi, st, vo);
}

static VKAPI_ATTR void VKAPI_CALL gp_CmdDrawIndirectByteCountEXT(VkCommandBuffer cb, uint32_t inst, uint32_t fi, VkBuffer cb2, VkDeviceSize co, uint32_t cbo,
                                                                 uint32_t vs)
{
    vkb_gprof *g = cur();
    count(g, cb, 1, 0, 0);
    g->draw_bytecount(cb, inst, fi, cb2, co, cbo, vs);
}

/* A command outside render passes: a new span when its kind differs from the open one's, and
 * under =2 a dispatch with another compute pipeline than the open run's (its pipe[0]). */
static void op(vkb_gprof *g, VkCommandBuffer cb, int kind)
{
    pthread_mutex_lock(&g->lock);
    gp_cb *e = cb_get(g, cb, 0);
    gp_seg *s = open_seg(e);
    int other_pipe = by_pipe && kind == OP_DISPATCH && s && s->sub == OP_DISPATCH && s->dispatches && s->pipe[0] != e->cbound;
    if (s && s->kind == SEG_BETWEEN && (s->sub != kind || other_pipe)) {
        s = next_seg(g, e, cb);
        if (s) s->sub = (uint8_t)kind;
    }
    if (s) s->ops++;
    pthread_mutex_unlock(&g->lock);
}

#define OP_HOOK(name, kind, params, args)                     \
    static VKAPI_ATTR void VKAPI_CALL gp_##name params        \
    {                                                         \
        vkb_gprof *g = cur();                                 \
        op(g, cb, kind);                                      \
        g->o.name args;                                       \
    }
OP_HOOK(vkCmdCopyBufferToImage, OP_UPLOAD, (VkCommandBuffer cb, VkBuffer s, VkImage d, VkImageLayout l, uint32_t n, const VkBufferImageCopy *r),
        (cb, s, d, l, n, r))
OP_HOOK(vkCmdCopyBufferToImage2, OP_UPLOAD, (VkCommandBuffer cb, const VkCopyBufferToImageInfo2 *i), (cb, i))
OP_HOOK(vkCmdCopyImage, OP_IMGCOPY,
        (VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl, uint32_t n, const VkImageCopy *r),
        (cb, s, sl, d, dl, n, r))
OP_HOOK(vkCmdCopyImage2, OP_IMGCOPY, (VkCommandBuffer cb, const VkCopyImageInfo2 *i), (cb, i))
OP_HOOK(vkCmdBlitImage, OP_IMGCOPY,
        (VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl, uint32_t n, const VkImageBlit *r, VkFilter f),
        (cb, s, sl, d, dl, n, r, f))
OP_HOOK(vkCmdBlitImage2, OP_IMGCOPY, (VkCommandBuffer cb, const VkBlitImageInfo2 *i), (cb, i))
OP_HOOK(vkCmdResolveImage, OP_IMGCOPY,
        (VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl, uint32_t n, const VkImageResolve *r),
        (cb, s, sl, d, dl, n, r))
OP_HOOK(vkCmdResolveImage2, OP_IMGCOPY, (VkCommandBuffer cb, const VkResolveImageInfo2 *i), (cb, i))
OP_HOOK(vkCmdCopyImageToBuffer, OP_IMGCOPY, (VkCommandBuffer cb, VkImage s, VkImageLayout l, VkBuffer d, uint32_t n, const VkBufferImageCopy *r),
        (cb, s, l, d, n, r))
OP_HOOK(vkCmdCopyImageToBuffer2, OP_IMGCOPY, (VkCommandBuffer cb, const VkCopyImageToBufferInfo2 *i), (cb, i))
OP_HOOK(vkCmdCopyBuffer, OP_BUFCOPY, (VkCommandBuffer cb, VkBuffer s, VkBuffer d, uint32_t n, const VkBufferCopy *r), (cb, s, d, n, r))
OP_HOOK(vkCmdCopyBuffer2, OP_BUFCOPY, (VkCommandBuffer cb, const VkCopyBufferInfo2 *i), (cb, i))
OP_HOOK(vkCmdFillBuffer, OP_BUFCOPY, (VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, VkDeviceSize sz, uint32_t v), (cb, b, o, sz, v))
OP_HOOK(vkCmdUpdateBuffer, OP_BUFCOPY, (VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, VkDeviceSize sz, const void *p), (cb, b, o, sz, p))
OP_HOOK(vkCmdClearColorImage, OP_CLEAR,
        (VkCommandBuffer cb, VkImage i, VkImageLayout l, const VkClearColorValue *c, uint32_t n, const VkImageSubresourceRange *r),
        (cb, i, l, c, n, r))
OP_HOOK(vkCmdClearDepthStencilImage, OP_CLEAR,
        (VkCommandBuffer cb, VkImage i, VkImageLayout l, const VkClearDepthStencilValue *c, uint32_t n, const VkImageSubresourceRange *r),
        (cb, i, l, c, n, r))

static void barrier(vkb_gprof *g, VkCommandBuffer cb)
{
    pthread_mutex_lock(&g->lock);
    gp_seg *s = open_seg(cb_get(g, cb, 0));
    if (s) s->barriers++;
    pthread_mutex_unlock(&g->lock);
}

static VKAPI_ATTR void VKAPI_CALL gp_vkCmdPipelineBarrier(VkCommandBuffer cb, VkPipelineStageFlags ss, VkPipelineStageFlags ds, VkDependencyFlags f,
                                                          uint32_t nm, const VkMemoryBarrier *m, uint32_t nb, const VkBufferMemoryBarrier *b,
                                                          uint32_t ni, const VkImageMemoryBarrier *im)
{
    vkb_gprof *g = cur();
    barrier(g, cb);
    g->o.vkCmdPipelineBarrier(cb, ss, ds, f, nm, m, nb, b, ni, im);
}

static VKAPI_ATTR void VKAPI_CALL gp_vkCmdPipelineBarrier2(VkCommandBuffer cb, const VkDependencyInfo *d)
{
    vkb_gprof *g = cur();
    barrier(g, cb);
    g->o.vkCmdPipelineBarrier2(cb, d);
}

static VKAPI_ATTR void VKAPI_CALL gp_CmdDispatch(VkCommandBuffer cb, uint32_t x, uint32_t y, uint32_t z)
{
    vkb_gprof *g = cur();
    op(g, cb, OP_DISPATCH);
    count(g, cb, 0, 1, (uint64_t)x * y * z); /* workgroups, as the dispatches' "prims" */
    g->dispatch(cb, x, y, z);
}

static VKAPI_ATTR void VKAPI_CALL gp_CmdDispatchIndirect(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o)
{
    vkb_gprof *g = cur();
    op(g, cb, OP_DISPATCH);
    count(g, cb, 0, 1, 0);
    g->dispatch_indirect(cb, b, o);
}

static VKAPI_ATTR void VKAPI_CALL gp_CmdDispatchBase(VkCommandBuffer cb, uint32_t bx, uint32_t by, uint32_t bz, uint32_t x, uint32_t y, uint32_t z)
{
    vkb_gprof *g = cur();
    op(g, cb, OP_DISPATCH);
    count(g, cb, 0, 1, (uint64_t)x * y * z);
    g->dispatch_base(cb, bx, by, bz, x, y, z);
}

static uint64_t spv_hash(const uint32_t *code, size_t size)
{
    uint64_t h = 1469598103934665603ull;
    const uint8_t *b = (const uint8_t *)code;
    for (size_t i = 0; i < size; i++) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}

/* With dump-spirv on, each distinct module once under its hash. */
static uint64_t note_code(const uint32_t *code, size_t size)
{
    uint64_t h = spv_hash(code, size);
    if (vkb_emu_dumping() || by_pipe) {
        char path[600];
        snprintf(path, sizeof(path), "%s/dump-spirv/g-%016llx.spv", vkb_pcache_dir(), (unsigned long long)h);
        if (access(path, F_OK) != 0) {
            FILE *f = fopen(path, "wb");
            if (f) {
                fwrite(code, 1, size, f);
                fclose(f);
            }
        }
    }
    return h;
}

/* On the driver table: what reaches the driver, emulation rewrites included. */
static VKAPI_ATTR VkResult VKAPI_CALL gp_CreateShaderModule(VkDevice d, const VkShaderModuleCreateInfo *ci, const VkAllocationCallbacks *a,
                                                            VkShaderModule *out)
{
    vkb_gprof *g = cur();
    VkResult r = g->create_mod(d, ci, a, out);
    if (r == VK_SUCCESS && ci && ci->pCode) {
        gp_mod *m = calloc(1, sizeof(*m));
        if (m) {
            m->m = *out;
            m->hash = note_code(ci->pCode, ci->codeSize);
            pthread_mutex_lock(&g->lock);
            unsigned b = hk((uint64_t)*out, VIEW_BUCKETS);
            m->next = g->mods[b];
            g->mods[b] = m;
            pthread_mutex_unlock(&g->lock);
        }
    }
    return r;
}

static uint64_t stage_hash(vkb_gprof *g, const VkPipelineShaderStageCreateInfo *st)
{
    if (st->module) {
        /* Handles are reused once a module is destroyed: the newest entry is the live one. */
        for (gp_mod *m = g->mods[hk((uint64_t)st->module, VIEW_BUCKETS)]; m; m = m->next)
            if (m->m == st->module) return m->hash;
        return 0;
    }
    for (const VkBaseInStructure *b = st->pNext; b; b = b->pNext)
        if (b->sType == VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO) {
            const VkShaderModuleCreateInfo *mi = (const VkShaderModuleCreateInfo *)b;
            return mi->pCode ? note_code(mi->pCode, mi->codeSize) : 0;
        }
    return 1;
}

static VKAPI_ATTR VkResult VKAPI_CALL gp_CreateGraphicsPipelines(VkDevice d, VkPipelineCache c, uint32_t n, const VkGraphicsPipelineCreateInfo *ci,
                                                                 const VkAllocationCallbacks *a, VkPipeline *out)
{
    vkb_gprof *g = cur();
    VkResult r = g->gfx(d, c, n, ci, a, out);
    if (r < 0) return r;
    pthread_mutex_lock(&g->lock);
    for (uint32_t i = 0; i < n; i++) {
        if (!out[i]) continue;
        gp_pipe *pp = calloc(1, sizeof(*pp));
        if (!pp) break;
        pp->p = out[i];
        int len = 0;
        for (uint32_t j = 0; j < ci[i].stageCount && len < (int)sizeof(pp->desc) - 40; j++)
            len += snprintf(pp->desc + len, sizeof(pp->desc) - (size_t)len, "%s0x%x:%016llx", j ? " " : "", ci[i].pStages[j].stage,
                            (unsigned long long)stage_hash(g, &ci[i].pStages[j]));
        const VkPipelineInputAssemblyStateCreateInfo *ia = ci[i].pInputAssemblyState;
        const VkPipelineRasterizationStateCreateInfo *rs = ci[i].pRasterizationState;
        const VkPipelineVertexInputStateCreateInfo *vi = ci[i].pVertexInputState;
        if (len < (int)sizeof(pp->desc) - 40)
            snprintf(pp->desc + len, sizeof(pp->desc) - (size_t)len, " | topo %d discard %d vb %u va %u flags 0x%x", ia ? (int)ia->topology : -1,
                     rs ? (int)rs->rasterizerDiscardEnable : -1, vi ? vi->vertexBindingDescriptionCount : 0,
                     vi ? vi->vertexAttributeDescriptionCount : 0, ci[i].flags);
        unsigned b = hk((uint64_t)out[i], VIEW_BUCKETS);
        pp->next = g->pipes[b];
        g->pipes[b] = pp;
    }
    pthread_mutex_unlock(&g->lock);
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL gp_CreateComputePipelines(VkDevice d, VkPipelineCache c, uint32_t n, const VkComputePipelineCreateInfo *ci,
                                                                const VkAllocationCallbacks *a, VkPipeline *out)
{
    vkb_gprof *g = cur();
    VkResult r = g->comp(d, c, n, ci, a, out);
    if (r < 0) return r;
    pthread_mutex_lock(&g->lock);
    for (uint32_t i = 0; i < n; i++) {
        if (!out[i]) continue;
        gp_pipe *pp = calloc(1, sizeof(*pp));
        if (!pp) break;
        pp->p = out[i];
        snprintf(pp->desc, sizeof(pp->desc), "0x20:%016llx | compute", (unsigned long long)stage_hash(g, &ci[i].stage));
        unsigned b = hk((uint64_t)out[i], VIEW_BUCKETS);
        pp->next = g->pipes[b];
        g->pipes[b] = pp;
    }
    pthread_mutex_unlock(&g->lock);
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL gp_CreateImageView(VkDevice d, const VkImageViewCreateInfo *ci, const VkAllocationCallbacks *a, VkImageView *out)
{
    vkb_gprof *g = cur();
    VkResult r = g->create_view(d, ci, a, out);
    if (r == VK_SUCCESS && ci) {
        gp_view *v = calloc(1, sizeof(*v));
        if (v) {
            v->v = *out;
            v->fmt = (uint32_t)ci->format;
            pthread_mutex_lock(&g->lock);
            for (gp_img *im = g->imgs[hk((uint64_t)ci->image, VIEW_BUCKETS)]; im; im = im->next)
                if (im->i == ci->image) {
                    v->samples = im->samples;
                    v->nocomp = im->nocomp;
                    v->storage = im->storage;
                    break;
                }
            unsigned b = hk((uint64_t)*out, VIEW_BUCKETS);
            v->next = g->views[b];
            g->views[b] = v;
            pthread_mutex_unlock(&g->lock);
        }
    }
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL gp_CreateImage(VkDevice d, const VkImageCreateInfo *ci, const VkAllocationCallbacks *a, VkImage *out)
{
    vkb_gprof *g = cur();
    VkResult r = g->create_img(d, ci, a, out);
    if (r != VK_SUCCESS || !ci) return r;
    int nocomp = -1;
    if (g->layout2 && ci->tiling == VK_IMAGE_TILING_OPTIMAL) {
        VkImageCompressionPropertiesEXT cp = {VK_STRUCTURE_TYPE_IMAGE_COMPRESSION_PROPERTIES_EXT, NULL, 0, 0};
        VkSubresourceLayout2KHR l = {VK_STRUCTURE_TYPE_SUBRESOURCE_LAYOUT_2_KHR, &cp, {0}};
        VkImageAspectFlags asp = (ci->usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
        VkImageSubresource2KHR sr = {VK_STRUCTURE_TYPE_IMAGE_SUBRESOURCE_2_KHR, NULL, {asp, 0, 0}};
        g->layout2(d, *out, &sr, &l);
        nocomp = cp.imageCompressionFlags == VK_IMAGE_COMPRESSION_DISABLED_EXT;
        if (ci->format < 256) {
            pthread_mutex_lock(&g->lock);
            g->made[ci->format][nocomp]++;
            g->made_new = 1;
            pthread_mutex_unlock(&g->lock);
        }
    }
    if (!(ci->usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT))) return r;
    gp_img *im = calloc(1, sizeof(*im));
    if (!im) return r;
    im->i = *out;
    im->samples = ci->samples > 1 ? (uint32_t)ci->samples : 0;
    im->storage = (ci->usage & VK_IMAGE_USAGE_STORAGE_BIT) != 0;
    im->nocomp = nocomp == 1;
    pthread_mutex_lock(&g->lock);
    unsigned b = hk((uint64_t)*out, VIEW_BUCKETS);
    im->next = g->imgs[b];
    g->imgs[b] = im;
    pthread_mutex_unlock(&g->lock);
    return r;
}

static VKAPI_ATTR void VKAPI_CALL gp_DestroyImage(VkDevice d, VkImage x, const VkAllocationCallbacks *a)
{
    vkb_gprof *g = cur();
    if (x) {
        pthread_mutex_lock(&g->lock);
        for (gp_img **p = &g->imgs[hk((uint64_t)x, VIEW_BUCKETS)]; *p; p = &(*p)->next)
            if ((*p)->i == x) {
                gp_img *dead = *p;
                *p = dead->next;
                free(dead);
                break;
            }
        pthread_mutex_unlock(&g->lock);
    }
    g->destroy_img(d, x, a);
}

static VKAPI_ATTR void VKAPI_CALL gp_DestroyImageView(VkDevice d, VkImageView x, const VkAllocationCallbacks *a)
{
    vkb_gprof *g = cur();
    if (x) {
        pthread_mutex_lock(&g->lock);
        for (gp_view **p = &g->views[hk((uint64_t)x, VIEW_BUCKETS)]; *p; p = &(*p)->next)
            if ((*p)->v == x) {
                gp_view *dead = *p;
                *p = dead->next;
                free(dead);
                break;
            }
        pthread_mutex_unlock(&g->lock);
    }
    g->destroy_view(d, x, a);
}

/* The dispatch table only: what the client's commands reach, emulation wrappers included (with the
 * divisor emulation on, draws reach the driver table through another table's calls). The bridge's
 * own work (BCn decoder dispatches) shows in the spans, not the counts. */
#define HOOK(field, mine, saved)                                    \
    do {                                                            \
        g->saved = t->dt.field;                                     \
        if (g->saved) t->dt.field = mine;                           \
    } while (0)

#define UNHOOK(field, mine, saved)                                  \
    do {                                                            \
        if (g->saved && t->dt.field == mine) t->dt.field = g->saved; \
    } while (0)

int vkb_gprof_wanted(void)
{
    const char *v = vkb_srv_env("VKBRIDGE_GPU_PROF");
    return v && *v && strcmp(v, "0");
}

void vkb_gprof_device_init(vkb_srv_table *t)
{
    if (!vkb_gprof_wanted()) return;
    if (t->id >= 4096) {
        VKB_INFO("device %u: GPU profiler off (device id beyond the 4096 it tracks)", t->id);
        return;
    }
    const char *mode = vkb_srv_env("VKBRIDGE_GPU_PROF");
    by_pipe = !strcmp(mode, "2");
    if (strstr(mode, "counters")) { /* Mali's hardware counters too (server_hwcnt.c), or alone */
        vkb_hwcnt_start();
        if (!strcmp(mode, "counters-only")) {
            VKB_INFO("device %u: GPU profiler off (VKBRIDGE_GPU_PROF=counters-only reads the hardware counters alone)", t->id);
            return;
        }
    }
    char missing[400] = "";
#define NEED(name)                                                                                   \
    do {                                                                                             \
        if (!t->real.name) snprintf(missing + strlen(missing), sizeof(missing) - strlen(missing), " %s", #name); \
    } while (0)
    NEED(vkCreateQueryPool); NEED(vkGetQueryPoolResults); NEED(vkCmdWriteTimestamp); NEED(vkCmdResetQueryPool);
    NEED(vkBeginCommandBuffer); NEED(vkEndCommandBuffer); NEED(vkCreateImageView); NEED(vkDestroyImageView);
    NEED(vkCmdDraw); NEED(vkCmdDrawIndexed); NEED(vkCmdDrawIndirect); NEED(vkCmdDrawIndexedIndirect);
    NEED(vkCmdDispatch); NEED(vkCmdDispatchIndirect); NEED(vkCmdBindPipeline); NEED(vkCmdExecuteCommands);
#undef NEED
    if (*missing) {
        VKB_INFO("device %u: GPU profiler off (the driver lacks:%s)", t->id, missing);
        return;
    }
    PFN_vkGetPhysicalDeviceProperties props = (PFN_vkGetPhysicalDeviceProperties)vkb_gipa(t->instance, "vkGetPhysicalDeviceProperties");
    PFN_vkGetPhysicalDeviceQueueFamilyProperties qfp =
        (PFN_vkGetPhysicalDeviceQueueFamilyProperties)vkb_gipa(t->instance, "vkGetPhysicalDeviceQueueFamilyProperties");
    if (!props || !qfp) {
        VKB_INFO("device %u: GPU profiler off (vkGetPhysicalDeviceProperties or QueueFamilyProperties not found)", t->id);
        return;
    }
    VkPhysicalDeviceProperties p;
    props(t->physical, &p);
    VkQueueFamilyProperties q[16];
    uint32_t nq = 16;
    qfp(t->physical, &nq, q);
    uint32_t bits = 64;
    for (uint32_t i = 0; i < nq; i++)
        if (q[i].timestampValidBits < bits) bits = q[i].timestampValidBits;
    if (!bits) {
        /* A stamp on a family without timestamps is invalid; the profiler does not track families. */
        VKB_INFO("device %u: GPU profiler off (a queue family has no timestamps)", t->id);
        return;
    }
    vkb_gprof *g = calloc(1, sizeof(*g));
    if (!g) {
        VKB_INFO("device %u: GPU profiler off (out of memory)", t->id);
        return;
    }
    g->t = t;
    g->dev = t->device;
    g->period_ns = p.limits.timestampPeriod;
    g->mask = bits >= 64 ? ~0ull : (1ull << bits) - 1;
    g->since = now_s();
    g->period_s = REPORT_S;
    {
        const char *ps = vkb_srv_env("VKBRIDGE_TEST_GPROF_S"); /* host tests */
        if (ps && atof(ps) > 0) g->period_s = atof(ps);
    }
    pthread_mutex_init(&g->lock, NULL);
    g->create_pool = t->real.vkCreateQueryPool;
    g->destroy_pool = t->real.vkDestroyQueryPool;
    g->results = t->real.vkGetQueryPoolResults;
    g->reset = t->real.vkCmdResetQueryPool;
    g->stamp = t->real.vkCmdWriteTimestamp;
    HOOK(vkBeginCommandBuffer, gp_BeginCommandBuffer, begin);
    HOOK(vkEndCommandBuffer, gp_EndCommandBuffer, end);
    HOOK(vkCmdBeginRendering, gp_CmdBeginRendering, begin_rendering);
    HOOK(vkCmdEndRendering, gp_CmdEndRendering, end_rendering);
    HOOK(vkCmdBeginRenderPass, gp_CmdBeginRenderPass, begin_pass);
    HOOK(vkCmdBeginRenderPass2, gp_CmdBeginRenderPass2, begin_pass2);
    HOOK(vkCmdEndRenderPass, gp_CmdEndRenderPass, end_pass);
    HOOK(vkCmdEndRenderPass2, gp_CmdEndRenderPass2, end_pass2);
    HOOK(vkCmdBindPipeline, gp_CmdBindPipeline, bind);
    HOOK(vkCmdExecuteCommands, gp_CmdExecuteCommands, execute);
    HOOK(vkCmdDraw, gp_CmdDraw, draw);
    HOOK(vkCmdDrawIndexed, gp_CmdDrawIndexed, draw_indexed);
    HOOK(vkCmdDrawIndirect, gp_CmdDrawIndirect, draw_indirect);
    HOOK(vkCmdDrawIndexedIndirect, gp_CmdDrawIndexedIndirect, draw_indexed_indirect);
    HOOK(vkCmdDispatch, gp_CmdDispatch, dispatch);
    HOOK(vkCmdDispatchIndirect, gp_CmdDispatchIndirect, dispatch_indirect);
    HOOK(vkCmdDispatchBase, gp_CmdDispatchBase, dispatch_base);
    HOOK(vkCmdDrawIndirectCount, gp_CmdDrawIndirectCount, draw_indirect_count);
    HOOK(vkCmdDrawIndexedIndirectCount, gp_CmdDrawIndexedIndirectCount, draw_indexed_indirect_count);
    HOOK(vkCmdDrawMultiEXT, gp_CmdDrawMultiEXT, draw_multi);
    HOOK(vkCmdDrawMultiIndexedEXT, gp_CmdDrawMultiIndexedEXT, draw_multi_indexed);
    HOOK(vkCmdDrawIndirectByteCountEXT, gp_CmdDrawIndirectByteCountEXT, draw_bytecount);
    HOOK(vkAllocateCommandBuffers, gp_AllocateCommandBuffers, alloc_cbs);
    HOOK(vkFreeCommandBuffers, gp_FreeCommandBuffers, free_cbs);
    HOOK(vkDestroyCommandPool, gp_DestroyCommandPool, destroy_cmdpool);
    HOOK(vkCreateImageView, gp_CreateImageView, create_view);
    g->create_mod = t->real.vkCreateShaderModule;
    if (g->create_mod) {
        if (t->dt.vkCreateShaderModule == g->create_mod) t->dt.vkCreateShaderModule = gp_CreateShaderModule;
        t->real.vkCreateShaderModule = gp_CreateShaderModule;
    }
    g->gfx = t->real.vkCreateGraphicsPipelines;
    if (g->gfx) {
        if (t->dt.vkCreateGraphicsPipelines == g->gfx) t->dt.vkCreateGraphicsPipelines = gp_CreateGraphicsPipelines;
        t->real.vkCreateGraphicsPipelines = gp_CreateGraphicsPipelines;
    }
    g->comp = t->real.vkCreateComputePipelines;
    if (g->comp) {
        if (t->dt.vkCreateComputePipelines == g->comp) t->dt.vkCreateComputePipelines = gp_CreateComputePipelines;
        t->real.vkCreateComputePipelines = gp_CreateComputePipelines;
    }
    if (by_pipe) { /* the costliest pipelines' SPIR-V, saved by hash */
        vkb_cache_subdir("dump-spirv");
    }
    HOOK(vkDestroyImageView, gp_DestroyImageView, destroy_view);
    HOOK(vkCreateImage, gp_CreateImage, create_img);
    HOOK(vkDestroyImage, gp_DestroyImage, destroy_img);
    { /* NULL unless server_instance.c enabled VK_EXT_image_compression_control for the profiler */
        PFN_vkGetDeviceProcAddr gdpa = (PFN_vkGetDeviceProcAddr)vkb_gipa(t->instance, "vkGetDeviceProcAddr");
        if (gdpa) g->layout2 = (PFN_vkGetImageSubresourceLayout2KHR)gdpa(t->device, "vkGetImageSubresourceLayout2EXT");
    }
    HOOK(vkCmdCopyBufferToImage, gp_vkCmdCopyBufferToImage, o.vkCmdCopyBufferToImage);
    HOOK(vkCmdCopyBufferToImage2, gp_vkCmdCopyBufferToImage2, o.vkCmdCopyBufferToImage2);
    HOOK(vkCmdCopyImage, gp_vkCmdCopyImage, o.vkCmdCopyImage);
    HOOK(vkCmdCopyImage2, gp_vkCmdCopyImage2, o.vkCmdCopyImage2);
    HOOK(vkCmdBlitImage, gp_vkCmdBlitImage, o.vkCmdBlitImage);
    HOOK(vkCmdBlitImage2, gp_vkCmdBlitImage2, o.vkCmdBlitImage2);
    HOOK(vkCmdResolveImage, gp_vkCmdResolveImage, o.vkCmdResolveImage);
    HOOK(vkCmdResolveImage2, gp_vkCmdResolveImage2, o.vkCmdResolveImage2);
    HOOK(vkCmdCopyImageToBuffer, gp_vkCmdCopyImageToBuffer, o.vkCmdCopyImageToBuffer);
    HOOK(vkCmdCopyImageToBuffer2, gp_vkCmdCopyImageToBuffer2, o.vkCmdCopyImageToBuffer2);
    HOOK(vkCmdCopyBuffer, gp_vkCmdCopyBuffer, o.vkCmdCopyBuffer);
    HOOK(vkCmdCopyBuffer2, gp_vkCmdCopyBuffer2, o.vkCmdCopyBuffer2);
    HOOK(vkCmdFillBuffer, gp_vkCmdFillBuffer, o.vkCmdFillBuffer);
    HOOK(vkCmdUpdateBuffer, gp_vkCmdUpdateBuffer, o.vkCmdUpdateBuffer);
    HOOK(vkCmdClearColorImage, gp_vkCmdClearColorImage, o.vkCmdClearColorImage);
    HOOK(vkCmdClearDepthStencilImage, gp_vkCmdClearDepthStencilImage, o.vkCmdClearDepthStencilImage);
    HOOK(vkCmdPipelineBarrier, gp_vkCmdPipelineBarrier, o.vkCmdPipelineBarrier);
    HOOK(vkCmdPipelineBarrier2, gp_vkCmdPipelineBarrier2, o.vkCmdPipelineBarrier2);
    profs[t->id] = g;
    __atomic_store_n(&last, g, __ATOMIC_RELEASE);
    if (pthread_create(&g->thread, NULL, gp_thread, g) == 0) g->thread_on = 1;
    VKB_INFO("device %u: GPU profiler on (timestamp period %.2f ns, %u bits; summary every %.0f s%s%s)", t->id, g->period_ns, bits, g->period_s,
             g->thread_on ? "" : "; NO timer thread: read only when a command buffer is begun again or freed",
             g->alloc_cbs && g->free_cbs ? "" : "; command buffer levels unknown");
}

void vkb_gprof_device_destroy(vkb_srv_table *t)
{
    if (t->id >= 4096 || !profs[t->id]) return;
    vkb_gprof *g = profs[t->id];
    UNHOOK(vkBeginCommandBuffer, gp_BeginCommandBuffer, begin);
    UNHOOK(vkEndCommandBuffer, gp_EndCommandBuffer, end);
    UNHOOK(vkCmdBeginRendering, gp_CmdBeginRendering, begin_rendering);
    UNHOOK(vkCmdEndRendering, gp_CmdEndRendering, end_rendering);
    UNHOOK(vkCmdBeginRenderPass, gp_CmdBeginRenderPass, begin_pass);
    UNHOOK(vkCmdBeginRenderPass2, gp_CmdBeginRenderPass2, begin_pass2);
    UNHOOK(vkCmdEndRenderPass, gp_CmdEndRenderPass, end_pass);
    UNHOOK(vkCmdEndRenderPass2, gp_CmdEndRenderPass2, end_pass2);
    UNHOOK(vkCmdBindPipeline, gp_CmdBindPipeline, bind);
    UNHOOK(vkCmdExecuteCommands, gp_CmdExecuteCommands, execute);
    UNHOOK(vkCmdDraw, gp_CmdDraw, draw);
    UNHOOK(vkCmdDrawIndexed, gp_CmdDrawIndexed, draw_indexed);
    UNHOOK(vkCmdDrawIndirect, gp_CmdDrawIndirect, draw_indirect);
    UNHOOK(vkCmdDrawIndexedIndirect, gp_CmdDrawIndexedIndirect, draw_indexed_indirect);
    UNHOOK(vkCmdDispatch, gp_CmdDispatch, dispatch);
    UNHOOK(vkCmdDispatchIndirect, gp_CmdDispatchIndirect, dispatch_indirect);
    UNHOOK(vkCmdDispatchBase, gp_CmdDispatchBase, dispatch_base);
    UNHOOK(vkCmdDrawIndirectCount, gp_CmdDrawIndirectCount, draw_indirect_count);
    UNHOOK(vkCmdDrawIndexedIndirectCount, gp_CmdDrawIndexedIndirectCount, draw_indexed_indirect_count);
    UNHOOK(vkCmdDrawMultiEXT, gp_CmdDrawMultiEXT, draw_multi);
    UNHOOK(vkCmdDrawMultiIndexedEXT, gp_CmdDrawMultiIndexedEXT, draw_multi_indexed);
    UNHOOK(vkCmdDrawIndirectByteCountEXT, gp_CmdDrawIndirectByteCountEXT, draw_bytecount);
    UNHOOK(vkAllocateCommandBuffers, gp_AllocateCommandBuffers, alloc_cbs);
    UNHOOK(vkFreeCommandBuffers, gp_FreeCommandBuffers, free_cbs);
    UNHOOK(vkDestroyCommandPool, gp_DestroyCommandPool, destroy_cmdpool);
    UNHOOK(vkCreateImageView, gp_CreateImageView, create_view);
    if (g->create_mod) {
        if (t->dt.vkCreateShaderModule == gp_CreateShaderModule) t->dt.vkCreateShaderModule = g->create_mod;
        if (t->real.vkCreateShaderModule == gp_CreateShaderModule) t->real.vkCreateShaderModule = g->create_mod;
    }
    if (g->gfx) {
        if (t->dt.vkCreateGraphicsPipelines == gp_CreateGraphicsPipelines) t->dt.vkCreateGraphicsPipelines = g->gfx;
        if (t->real.vkCreateGraphicsPipelines == gp_CreateGraphicsPipelines) t->real.vkCreateGraphicsPipelines = g->gfx;
    }
    if (g->comp) {
        if (t->dt.vkCreateComputePipelines == gp_CreateComputePipelines) t->dt.vkCreateComputePipelines = g->comp;
        if (t->real.vkCreateComputePipelines == gp_CreateComputePipelines) t->real.vkCreateComputePipelines = g->comp;
    }
    UNHOOK(vkDestroyImageView, gp_DestroyImageView, destroy_view);
    UNHOOK(vkCreateImage, gp_CreateImage, create_img);
    UNHOOK(vkDestroyImage, gp_DestroyImage, destroy_img);
    UNHOOK(vkCmdCopyBufferToImage, gp_vkCmdCopyBufferToImage, o.vkCmdCopyBufferToImage);
    UNHOOK(vkCmdCopyBufferToImage2, gp_vkCmdCopyBufferToImage2, o.vkCmdCopyBufferToImage2);
    UNHOOK(vkCmdCopyImage, gp_vkCmdCopyImage, o.vkCmdCopyImage);
    UNHOOK(vkCmdCopyImage2, gp_vkCmdCopyImage2, o.vkCmdCopyImage2);
    UNHOOK(vkCmdBlitImage, gp_vkCmdBlitImage, o.vkCmdBlitImage);
    UNHOOK(vkCmdBlitImage2, gp_vkCmdBlitImage2, o.vkCmdBlitImage2);
    UNHOOK(vkCmdResolveImage, gp_vkCmdResolveImage, o.vkCmdResolveImage);
    UNHOOK(vkCmdResolveImage2, gp_vkCmdResolveImage2, o.vkCmdResolveImage2);
    UNHOOK(vkCmdCopyImageToBuffer, gp_vkCmdCopyImageToBuffer, o.vkCmdCopyImageToBuffer);
    UNHOOK(vkCmdCopyImageToBuffer2, gp_vkCmdCopyImageToBuffer2, o.vkCmdCopyImageToBuffer2);
    UNHOOK(vkCmdCopyBuffer, gp_vkCmdCopyBuffer, o.vkCmdCopyBuffer);
    UNHOOK(vkCmdCopyBuffer2, gp_vkCmdCopyBuffer2, o.vkCmdCopyBuffer2);
    UNHOOK(vkCmdFillBuffer, gp_vkCmdFillBuffer, o.vkCmdFillBuffer);
    UNHOOK(vkCmdUpdateBuffer, gp_vkCmdUpdateBuffer, o.vkCmdUpdateBuffer);
    UNHOOK(vkCmdClearColorImage, gp_vkCmdClearColorImage, o.vkCmdClearColorImage);
    UNHOOK(vkCmdClearDepthStencilImage, gp_vkCmdClearDepthStencilImage, o.vkCmdClearDepthStencilImage);
    UNHOOK(vkCmdPipelineBarrier, gp_vkCmdPipelineBarrier, o.vkCmdPipelineBarrier);
    UNHOOK(vkCmdPipelineBarrier2, gp_vkCmdPipelineBarrier2, o.vkCmdPipelineBarrier2);
    profs[t->id] = NULL;
    if (g->thread_on) { /* before the entries go */
        __atomic_store_n(&g->stop, 1, __ATOMIC_RELEASE);
        pthread_join(g->thread, NULL);
        g->thread_on = 0;
    }
    if (__atomic_load_n(&last, __ATOMIC_ACQUIRE) == g) {
        vkb_gprof *other = NULL;
        for (int i = 0; i < 4096 && !other; i++) other = profs[i];
        __atomic_store_n(&last, other, __ATOMIC_RELEASE);
    }
    for (int b = 0; b < CB_BUCKETS; b++)
        for (gp_cb *e = g->cbs[b], *n; e; e = n) {
            n = e->next;
            if (g->destroy_pool && e->pool) g->destroy_pool(g->dev, e->pool, NULL);
            free(e);
        }
    for (int b = 0; b < VIEW_BUCKETS; b++) {
        for (gp_view *e = g->views[b], *n; e; e = n) {
            n = e->next;
            free(e);
        }
        for (gp_img *e = g->imgs[b], *n; e; e = n) {
            n = e->next;
            free(e);
        }
        for (gp_pipe *e = g->pipes[b], *n; e; e = n) {
            n = e->next;
            free(e);
        }
        for (gp_mod *e = g->mods[b], *n; e; e = n) {
            n = e->next;
            free(e);
        }
    }
    /* Another thread may still be inside a hook holding g (the hang detector leaks for the same
     * reason); the state is small next to the device. */
}
