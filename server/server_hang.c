/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * Evidence: GPU hang detector. With VKBRIDGE_HANG_DETECT=1 (<cache dir>/env) or <cache dir>/hang-detect
 * present, every submission is followed by an empty one with a fence - a fence's first scope covers
 * everything submitted earlier on the queue - and a watcher thread looks at the oldest unsignalled
 * fence of each queue. A submission still running after 2.5 s, or the device lost, is logged once
 * with the pipelines its command buffers bound and their stages' driver modules (the names
 * content hashes; with dump-spirv on, their SPIR-V is saved as dump-spirv/h-<hash>.spv). Hooks sit on the driver calls, so the bridge's own pipelines
 * (the BCn decoder's dispatches) are seen too. The Witcher 3 (D3D12, 2026-10-11): a job held the
 * whole GPU past Mali's watchdog, the device was lost and the game presented black frames; the
 * logs could not say which job. Off unless asked for; a debugging aid, not a feature.
 */
#define _GNU_SOURCE
#include "vkb_server.h"
#include "server_emu.h"
#include "vkb_emu_formats.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define HANG_AFTER_S 2.5 /* VKBRIDGE_HANG_AFTER_MS overrides (host tests use a tiny value) */
#define PIPE_BUCKETS 1024
#define CB_BUCKETS 512
#define CB_PIPES 48
#define RING 256
#define QUEUES 8
#define STAGES 5
#define ADDRS 65536
#define DEAD 8192

typedef struct hg_addr {
    VkDeviceAddress a;
    VkDeviceSize size;
    VkBuffer b;
} hg_addr;

typedef struct hg_pipe {
    VkPipeline p;
    int compute;
    uint32_t n;
    VkShaderStageFlagBits stage[STAGES];
    VkShaderModule module[STAGES];
    uint64_t hash[STAGES];
    struct hg_pipe *next;
} hg_pipe;

typedef struct hg_mod {
    VkShaderModule m;
    uint64_t hash;
    size_t size;
    struct hg_mod *next;
} hg_mod;

#define CB_DRAWS 8
#define PC_BYTES 160 /* push constants kept per draw (vkd3d-proton's root constants block is 132 B) */

/* Descriptor evidence: what the app's images, views and samplers are, and which of them every
 * slot of a descriptor set holds (sampled images and samplers; writes and copies followed). */
#define OBJ_BUCKETS 16384
typedef struct hg_obj {
    uint64_t key;
    int kind;              /* 1 image, 2 view, 3 sampler, 4 descriptor set (freed: 5), 6 memory */
    int dead;              /* destroyed or freed */
    double dead_t;
    union {
        struct { VkFormat format; VkExtent3D extent; uint32_t mips, layers, usage, flags, tiling, samples; VkDeviceMemory mem; VkDeviceSize off; } img;
        struct { VkImage image; VkFormat format; uint32_t type, base_mip, mips, base_layer, layers, aspect; } view;
        struct { uint32_t mag, min, mip, u, v, w, aniso, compare, border, unnorm; float max_aniso, lod_bias, min_lod, max_lod; } smp;
        struct { VkDescriptorPool pool; VkDescriptorSetLayout layout; uint32_t var, writes, copies_in; double t; } set;
        struct { VkDeviceSize size; uint32_t type; } mem;
    };
    struct hg_obj *next;
} hg_obj;

typedef struct hg_set {
    VkDescriptorSet set;
    uint32_t n;            /* slots of binding 0 tracked */
    uint64_t *slot;        /* the view or sampler written there */
    struct hg_set *next;
} hg_set;

typedef struct hg_draw {
    VkPipeline p;          /* the graphics pipeline bound at the draw */
    VkDescriptorSet sets[4]; /* graphics sets 0-3 bound at the draw */
    VkImageView rt[3];     /* colour attachments 0-1 and the depth attachment of the rendering it is in */
    uint32_t count, instances, indexed;
    VkExtent2D area;       /* the render area of the pass or rendering it is in */
    int32_t first, voff;   /* firstVertex/firstIndex, vertexOffset */
    uint32_t pc_size;      /* push constants as they were at the draw */
    uint8_t pc[PC_BYTES];
    uint64_t slot3[13];    /* what set 3 held at the slots root u32 80..128 select, when the draw was recorded */
} hg_draw;

enum { K_COPY_BUFFER, K_COPY_IMAGE, K_COPY_B2I, K_COPY_I2B, K_BLIT, K_CLEAR_COLOR, K_CLEAR_DS, K_CLEAR_ATT, K_BARRIER, K_BARRIER2, K_SET_EVENT, K_SET_EVENT2, K_WAIT_EVENTS, K_WAIT_EVENTS2, K_FILL, K_UPDATE, K_DRAW_INDIRECT, K_DRAW_INDEXED_INDIRECT, K_DISPATCH_INDIRECT, K_RESOLVE, K_COPY_BUFFER2, K_COPY_IMAGE2, K_COPY_B2I2, K_N };
static const char *const k_names[K_N] = {"copy_buffer", "copy_image", "copy_b2i", "copy_i2b", "blit", "clear_color", "clear_ds", "clear_att", "barrier", "barrier2", "set_event", "set_event2", "wait_events", "wait_events2", "fill", "update", "draw_indirect", "draw_indexed_indirect", "dispatch_indirect", "resolve", "copy_buffer2", "copy_image2", "copy_b2i2"};
#define EVENTS 8
#define COPIES 8
typedef struct hg_copy {
    int kind;
    uint64_t src, dst;
    uint32_t nreg;
    VkDeviceSize src_size;     /* buffer sources: memory requirements size */
    VkDeviceSize boff;
    uint32_t row, height;
    VkOffset3D off, soff;
    VkExtent3D ext;
    uint32_t mip, layer, nlayers, smip, slayer;
} hg_copy;
typedef struct hg_cb {
    VkCommandBuffer cb;
    uint32_t k[K_N];
    VkEvent waited[EVENTS];
    uint32_t nwaited;
    hg_copy copy[COPIES];
    uint32_t ncopy;
    uint32_t n, dispatches, draws, ndraws;
    VkPipeline pipes[CB_PIPES];
    VkPipeline gfx_bound;
    VkExtent2D area;
    uint32_t pc_size;
    uint8_t pc[PC_BYTES];
    VkDescriptorSet sets[4];
    VkImageView rt[3];
    hg_draw draw[CB_DRAWS];
    struct hg_cb *next;
} hg_cb;

#define WAITS 8
typedef struct hg_entry {
    uint32_t k[K_N];
    VkEvent waited[EVENTS];
    uint32_t nwaited;
    hg_copy copy[COPIES];
    uint32_t ncopy;
    VkFence fence;
    double t;
    uint32_t nwait, nsig;
    VkSemaphore wait[WAITS], sig[WAITS];
    uint64_t wait_v[WAITS], sig_v[WAITS];
    uint32_t seq, ncb, npipes, dispatches, draws, ndraws;
    VkPipeline pipes[CB_PIPES];
    hg_draw draw[CB_DRAWS];
    int told;
} hg_entry;

typedef struct hg_queue {
    VkQueue q;
    hg_entry ring[RING];
    uint32_t head, len, seq;
} hg_queue;

typedef struct vkb_hang {
    vkb_srv_table *t;
    VkDevice dev;
    pthread_mutex_t lock;
    pthread_t thread;
    volatile int stop, lost_told;
    double after;
    hg_pipe *pipes[PIPE_BUCKETS];
    hg_mod *mods[PIPE_BUCKETS];
    hg_cb *cbs[CB_BUCKETS];
    hg_queue queues[QUEUES];
    uint32_t nqueues;
    /* The calls hooked, as they were before (driver or crash guard). */
    PFN_vkCreateGraphicsPipelines gfx;
    PFN_vkCreateComputePipelines compute;
    PFN_vkDestroyPipeline destroy_pipe;
    PFN_vkCreateShaderModule create_mod;
    PFN_vkDestroyShaderModule destroy_mod;
    PFN_vkBeginCommandBuffer begin;
    PFN_vkCmdBindPipeline bind;
    PFN_vkCmdDispatch dispatch;
    PFN_vkCmdDraw draw;
    PFN_vkCmdDrawIndexed draw_indexed;
    PFN_vkCmdBeginRendering begin_rendering;
    PFN_vkCmdBeginRenderPass begin_pass;
    PFN_vkQueueSubmit submit;
    PFN_vkQueueSubmit2 submit2;
    PFN_vkBindImageMemory bind_img;
    PFN_vkBindImageMemory2 bind_img2;
    PFN_vkGetImageMemoryRequirements img_req;
    int overflows, misaligned;
    uint64_t skip[16];          /* VKBRIDGE_SKIP_SHADERS: draws with these modules are dropped (bisecting) */
    uint32_t nskip, skipped;
    /* Buffer device addresses handed out (live), and those of destroyed buffers (a ring). */
    hg_addr *live;
    uint32_t nlive;
    hg_addr dead[DEAD];
    uint32_t ndead, dead_next;
    VkDeviceAddress lo, hi;     /* the span every address so far fell in */
    int addr_told;
    PFN_vkGetBufferDeviceAddress bda;
    PFN_vkDestroyBuffer destroy_buf;
    PFN_vkCmdPushConstants push;
    PFN_vkGetBufferMemoryRequirements buf_req;
    /* Driver calls the watcher uses. */
    PFN_vkCreateFence create_fence;
    PFN_vkDestroyFence destroy_fence;
    PFN_vkResetFences reset_fences;
    PFN_vkGetFenceStatus fence_status;
    PFN_vkGetDeviceFaultInfoEXT fault_info; /* VK_EXT_device_fault, enabled with the detector when the driver has it */
    /* Descriptor evidence (hg_obj, hg_set). */
    hg_obj *objs[OBJ_BUCKETS];
    hg_set *sets[OBJ_BUCKETS];
    PFN_vkCreateImage create_img;
    PFN_vkCreateImageView create_view;
    PFN_vkCreateSampler create_smp;
    PFN_vkUpdateDescriptorSets update_ds;
    PFN_vkCmdBindDescriptorSets bind_ds;
    PFN_vkAllocateDescriptorSets alloc_ds;
    PFN_vkFreeDescriptorSets free_ds;
    PFN_vkResetDescriptorPool reset_pool;
    PFN_vkUpdateDescriptorSetWithTemplate update_tmpl;
    PFN_vkDestroyImage destroy_img;
    PFN_vkDestroyImageView destroy_view;
    PFN_vkDestroySampler destroy_smp;
    PFN_vkAllocateMemory alloc_mem;
    PFN_vkFreeMemory free_mem;
    PFN_vkGetSemaphoreCounterValue sem_value;
    PFN_vkGetEventStatus event_status;
    PFN_vkCmdCopyBuffer c_copy_buffer;
    PFN_vkCmdCopyImage c_copy_image;
    PFN_vkCmdCopyBufferToImage c_copy_b2i;
    PFN_vkCmdCopyImageToBuffer c_copy_i2b;
    PFN_vkCmdBlitImage c_blit;
    PFN_vkCmdClearColorImage c_clear_color;
    PFN_vkCmdClearDepthStencilImage c_clear_ds;
    PFN_vkCmdClearAttachments c_clear_att;
    PFN_vkCmdPipelineBarrier c_barrier;
    PFN_vkCmdPipelineBarrier2 c_barrier2;
    PFN_vkCmdSetEvent c_set_event;
    PFN_vkCmdSetEvent2 c_set_event2;
    PFN_vkCmdWaitEvents c_wait_events;
    PFN_vkCmdWaitEvents2 c_wait_events2;
    PFN_vkCmdFillBuffer c_fill;
    PFN_vkCmdUpdateBuffer c_update;
    PFN_vkCmdDrawIndirect c_draw_indirect;
    PFN_vkCmdDrawIndexedIndirect c_draw_indexed_indirect;
    PFN_vkCmdDispatchIndirect c_dispatch_indirect;
    PFN_vkCmdResolveImage c_resolve;
    PFN_vkCmdCopyBuffer2 c_copy_buffer2;
    PFN_vkCmdCopyImage2 c_copy_image2;
    PFN_vkCmdCopyBufferToImage2 c_copy_b2i2;
} vkb_hang;

static vkb_hang *hangs[4096];

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

static vkb_hang *last; /* any device's: the saved calls are the driver's, the same for every device */

/* The calling device's; a call without one (none seen so far) gets another device's state, whose
 * saved driver calls are still the right ones to forward to. */
static vkb_hang *cur(void)
{
    vkb_srv_table *t = vkb_srv_current_table();
    vkb_hang *h = t && t->id < 4096 ? hangs[t->id] : NULL;
    return h ? h : __atomic_load_n(&last, __ATOMIC_ACQUIRE);
}

/* Calls on a command buffer or queue carry no device; the current table is the caller's. */
static vkb_hang *for_cmd(void) { return cur(); }

static hg_pipe *find_pipe(vkb_hang *h, VkPipeline p)
{
    for (hg_pipe *e = h->pipes[hk((uint64_t)(uintptr_t)p, PIPE_BUCKETS)]; e; e = e->next)
        if (e->p == p) return e;
    return NULL;
}

/* FNV-1a over the SPIR-V: handles are reused once vkd3d destroys its modules, hashes are not. */
static uint64_t spv_hash(const uint32_t *code, size_t size)
{
    return vkb_spv_app_hash(code, size); /* FNV-1a; a module an erratum rewrote by the app's code */
}

/* Called with the lock held. */
static hg_mod *find_mod(vkb_hang *h, VkShaderModule m)
{
    for (hg_mod *e = h->mods[hk((uint64_t)(uintptr_t)m, PIPE_BUCKETS)]; e; e = e->next)
        if (e->m == m) return e;
    return NULL;
}

static VKAPI_ATTR VkResult VKAPI_CALL hg_CreateShaderModule(VkDevice d, const VkShaderModuleCreateInfo *ci, const VkAllocationCallbacks *a,
                                                           VkShaderModule *out)
{
    vkb_hang *h = cur();
    VkResult r = h->create_mod(d, ci, a, out);
    if (r != VK_SUCCESS || !ci->pCode) return r;
    uint64_t hash = spv_hash(ci->pCode, ci->codeSize);
    /* With dump-spirv on, each distinct module once more under its hash: dump-spirv/h-<hash>.spv. */
    const char *dir = vkb_pcache_dir();
    char path[600];
    snprintf(path, sizeof(path), "%s/dump-spirv/h-%016llx.spv", dir ? dir : "", (unsigned long long)hash);
    if (dir && vkb_emu_dumping() && access(path, F_OK) != 0) {
        FILE *f = fopen(path, "wb");
        if (f) {
            fwrite(ci->pCode, 1, ci->codeSize, f);
            fclose(f);
        }
    }
    hg_mod *e = calloc(1, sizeof(*e));
    if (!e) return r;
    e->m = *out;
    e->hash = hash;
    e->size = ci->codeSize;
    pthread_mutex_lock(&h->lock);
    hg_mod *old = find_mod(h, *out);
    if (old) {
        old->hash = hash;
        old->size = ci->codeSize;
        free(e);
    } else {
        unsigned b = hk((uint64_t)(uintptr_t)*out, PIPE_BUCKETS);
        e->next = h->mods[b];
        h->mods[b] = e;
    }
    pthread_mutex_unlock(&h->lock);
    return r;
}

static VKAPI_ATTR void VKAPI_CALL hg_DestroyShaderModule(VkDevice d, VkShaderModule m, const VkAllocationCallbacks *a)
{
    vkb_hang *h = cur();
    if (m) {
        pthread_mutex_lock(&h->lock);
        hg_mod **pp = &h->mods[hk((uint64_t)(uintptr_t)m, PIPE_BUCKETS)];
        while (*pp && (*pp)->m != m) pp = &(*pp)->next;
        if (*pp) {
            hg_mod *e = *pp;
            *pp = e->next;
            free(e);
        }
        pthread_mutex_unlock(&h->lock);
    }
    h->destroy_mod(d, m, a);
}

static void note_pipes(vkb_hang *h, uint32_t n, VkPipeline *out, int compute, const VkPipelineShaderStageCreateInfo *const *stages,
                       const uint32_t *nstages)
{
    pthread_mutex_lock(&h->lock);
    for (uint32_t i = 0; i < n; i++) {
        if (!out[i]) continue;
        hg_pipe *e = calloc(1, sizeof(*e));
        if (!e) break;
        e->p = out[i];
        e->compute = compute;
        for (uint32_t s = 0; s < nstages[i] && e->n < STAGES; s++) {
            e->stage[e->n] = stages[i][s].stage;
            e->module[e->n] = stages[i][s].module;
            hg_mod *m = find_mod(h, stages[i][s].module);
            e->hash[e->n++] = m ? m->hash : 0;
        }
        unsigned b = hk((uint64_t)(uintptr_t)out[i], PIPE_BUCKETS);
        e->next = h->pipes[b];
        h->pipes[b] = e;
    }
    pthread_mutex_unlock(&h->lock);
}

static VKAPI_ATTR VkResult VKAPI_CALL hg_CreateGraphicsPipelines(VkDevice d, VkPipelineCache c, uint32_t n,
                                                                const VkGraphicsPipelineCreateInfo *ci,
                                                                const VkAllocationCallbacks *a, VkPipeline *out)
{
    vkb_hang *h = cur();
    VkResult r = h->gfx(d, c, n, ci, a, out);
    const VkPipelineShaderStageCreateInfo *st[64];
    uint32_t ns[64];
    for (uint32_t i = 0; i < n && i < 64; i++) {
        st[i] = ci[i].pStages;
        ns[i] = ci[i].pStages ? ci[i].stageCount : 0;
    }
    note_pipes(h, n < 64 ? n : 64, out, 0, st, ns);
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL hg_CreateComputePipelines(VkDevice d, VkPipelineCache c, uint32_t n,
                                                               const VkComputePipelineCreateInfo *ci,
                                                               const VkAllocationCallbacks *a, VkPipeline *out)
{
    vkb_hang *h = cur();
    VkResult r = h->compute(d, c, n, ci, a, out);
    const VkPipelineShaderStageCreateInfo *st[64];
    uint32_t ns[64];
    for (uint32_t i = 0; i < n && i < 64; i++) {
        st[i] = &ci[i].stage;
        ns[i] = 1;
    }
    note_pipes(h, n < 64 ? n : 64, out, 1, st, ns);
    return r;
}

static VKAPI_ATTR void VKAPI_CALL hg_DestroyPipeline(VkDevice d, VkPipeline p, const VkAllocationCallbacks *a)
{
    vkb_hang *h = cur();
    if (p) {
        pthread_mutex_lock(&h->lock);
        hg_pipe **pp = &h->pipes[hk((uint64_t)(uintptr_t)p, PIPE_BUCKETS)];
        while (*pp && (*pp)->p != p) pp = &(*pp)->next;
        if (*pp) {
            hg_pipe *e = *pp;
            *pp = e->next;
            free(e);
        }
        pthread_mutex_unlock(&h->lock);
    }
    h->destroy_pipe(d, p, a);
}

/* Called with the lock held. */
static hg_cb *cb_get(vkb_hang *h, VkCommandBuffer cb, int create)
{
    unsigned b = hk((uint64_t)(uintptr_t)cb, CB_BUCKETS);
    for (hg_cb *e = h->cbs[b]; e; e = e->next)
        if (e->cb == cb) return e;
    if (!create) return NULL;
    hg_cb *e = calloc(1, sizeof(*e));
    if (!e) return NULL;
    e->cb = cb;
    e->next = h->cbs[b];
    h->cbs[b] = e;
    return e;
}

static VKAPI_ATTR VkResult VKAPI_CALL hg_BeginCommandBuffer(VkCommandBuffer cb, const VkCommandBufferBeginInfo *bi)
{
    vkb_hang *h = for_cmd();
    pthread_mutex_lock(&h->lock);
    hg_cb *e = cb_get(h, cb, 1);
    if (e) {
        e->n = e->dispatches = e->draws = e->ndraws = 0;
        e->gfx_bound = VK_NULL_HANDLE;
        e->area = (VkExtent2D){0, 0};
        e->pc_size = 0;
        memset(e->sets, 0, sizeof(e->sets));
        memset(e->k, 0, sizeof(e->k));
        e->nwaited = 0;
        e->ncopy = 0;
    }
    pthread_mutex_unlock(&h->lock);
    return h->begin(cb, bi);
}

static VKAPI_ATTR void VKAPI_CALL hg_CmdBindPipeline(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipeline p)
{
    vkb_hang *h = for_cmd();
    pthread_mutex_lock(&h->lock);
    hg_cb *e = cb_get(h, cb, 1);
    if (e) {
        uint32_t i = 0;
        while (i < e->n && e->pipes[i] != p) i++;
        if (i == e->n && e->n < CB_PIPES) e->pipes[e->n++] = p;
        if (bp == VK_PIPELINE_BIND_POINT_GRAPHICS) e->gfx_bound = p;
    }
    pthread_mutex_unlock(&h->lock);
    h->bind(cb, bp, p);
}

static VKAPI_ATTR void VKAPI_CALL hg_CmdDispatch(VkCommandBuffer cb, uint32_t x, uint32_t y, uint32_t z)
{
    vkb_hang *h = for_cmd();
    pthread_mutex_lock(&h->lock);
    hg_cb *e = cb_get(h, cb, 1);
    if (e) e->dispatches++;
    pthread_mutex_unlock(&h->lock);
    h->dispatch(cb, x, y, z);
}

/* Called with the lock held: whether the draw is to be dropped (VKBRIDGE_SKIP_SHADERS). */
static int skip_draw(vkb_hang *h, hg_cb *e)
{
    if (!h->nskip || !e || !e->gfx_bound) return 0;
    hg_pipe *p = find_pipe(h, e->gfx_bound);
    for (uint32_t s = 0; p && s < p->n; s++)
        for (uint32_t k = 0; k < h->nskip; k++)
            if (p->hash[s] == h->skip[k]) {
                if (h->skipped++ < 4) VKB_ERR("HANG bisect: draw with h-%016llx dropped", (unsigned long long)h->skip[k]);
                return 1;
            }
    return 0;
}

/* Called with the lock held: the first draws, and after that the largest ones. */
static int32_t h_first, h_voff; /* set with the lock held by the draw hooks, for note_draw */
static hg_set *set_get(vkb_hang *h, VkDescriptorSet set, uint32_t need);
static void note_draw(vkb_hang *h, hg_cb *e, uint32_t count, uint32_t instances, uint32_t indexed)
{
    e->draws++;
    hg_draw d = {e->gfx_bound, {0}, {0}, count, instances, indexed, e->area, h_first, h_voff, e->pc_size, {0}};
    memcpy(d.sets, e->sets, sizeof(d.sets));
    memcpy(d.rt, e->rt, sizeof(d.rt));
    memcpy(d.pc, e->pc, sizeof(d.pc));
    hg_set *s3 = e->sets[3] ? set_get(h, e->sets[3], 0) : NULL;
    for (uint32_t k = 0; s3 && k < 13; k++) {
        uint32_t idx;
        memcpy(&idx, d.pc + 80 + 4 * k, 4);
        d.slot3[k] = idx < s3->n ? s3->slot[idx] : 0;
    }
    uint64_t size = (uint64_t)count * instances;
    if (e->ndraws < CB_DRAWS) {
        e->draw[e->ndraws++] = d;
        return;
    }
    uint32_t small = 0;
    for (uint32_t i = 1; i < CB_DRAWS; i++)
        if ((uint64_t)e->draw[i].count * e->draw[i].instances < (uint64_t)e->draw[small].count * e->draw[small].instances) small = i;
    if (size > (uint64_t)e->draw[small].count * e->draw[small].instances) e->draw[small] = d;
}

static VKAPI_ATTR void VKAPI_CALL hg_CmdDraw(VkCommandBuffer cb, uint32_t vc, uint32_t ic, uint32_t fv, uint32_t fi)
{
    vkb_hang *h = for_cmd();
    pthread_mutex_lock(&h->lock);
    hg_cb *e = cb_get(h, cb, 1);
    h_first = (int32_t)fv, h_voff = 0;
    if (e) note_draw(h, e, vc, ic, 0);
    int skip = skip_draw(h, e);
    pthread_mutex_unlock(&h->lock);
    if (!skip) h->draw(cb, vc, ic, fv, fi);
}

static VKAPI_ATTR void VKAPI_CALL hg_CmdBeginRendering(VkCommandBuffer cb, const VkRenderingInfo *ri)
{
    vkb_hang *h = for_cmd();
    pthread_mutex_lock(&h->lock);
    hg_cb *e = cb_get(h, cb, 1);
    if (e && ri) {
        e->area = ri->renderArea.extent;
        memset(e->rt, 0, sizeof(e->rt));
        for (uint32_t i = 0; i < ri->colorAttachmentCount && i < 2; i++) e->rt[i] = ri->pColorAttachments[i].imageView;
        if (ri->pDepthAttachment) e->rt[2] = ri->pDepthAttachment->imageView;
    }
    pthread_mutex_unlock(&h->lock);
    h->begin_rendering(cb, ri);
}

static VKAPI_ATTR void VKAPI_CALL hg_CmdBeginRenderPass(VkCommandBuffer cb, const VkRenderPassBeginInfo *bi, VkSubpassContents c)
{
    vkb_hang *h = for_cmd();
    pthread_mutex_lock(&h->lock);
    hg_cb *e = cb_get(h, cb, 1);
    if (e && bi) e->area = bi->renderArea.extent;
    pthread_mutex_unlock(&h->lock);
    h->begin_pass(cb, bi, c);
}

static VKAPI_ATTR void VKAPI_CALL hg_CmdDrawIndexed(VkCommandBuffer cb, uint32_t ic, uint32_t inst, uint32_t fi, int32_t vo, uint32_t fin)
{
    vkb_hang *h = for_cmd();
    pthread_mutex_lock(&h->lock);
    hg_cb *e = cb_get(h, cb, 1);
    h_first = (int32_t)fi, h_voff = vo;
    if (e) note_draw(h, e, ic, inst, 1);
    int skip = skip_draw(h, e);
    pthread_mutex_unlock(&h->lock);
    if (!skip) h->draw_indexed(cb, ic, inst, fi, vo, fin);
}

/* Called with the lock held. */
static hg_queue *queue_get(vkb_hang *h, VkQueue q)
{
    for (uint32_t i = 0; i < h->nqueues; i++)
        if (h->queues[i].q == q) return &h->queues[i];
    if (h->nqueues == QUEUES) return NULL;
    hg_queue *e = &h->queues[h->nqueues++];
    memset(e, 0, sizeof(*e));
    e->q = q;
    return e;
}

static void add_cb(vkb_hang *h, hg_entry *en, VkCommandBuffer cb)
{
    en->ncb++;
    hg_cb *c = cb_get(h, cb, 0);
    if (!c) return;
    en->dispatches += c->dispatches;
    for (int i = 0; i < K_N; i++) en->k[i] += c->k[i];
    for (uint32_t i = 0; i < c->nwaited && en->nwaited < EVENTS; i++) en->waited[en->nwaited++] = c->waited[i];
    for (uint32_t i = 0; i < c->ncopy && en->ncopy < COPIES; i++) en->copy[en->ncopy++] = c->copy[i];
    en->draws += c->draws;
    for (uint32_t i = 0; i < c->ndraws && en->ndraws < CB_DRAWS; i++) en->draw[en->ndraws++] = c->draw[i];
    for (uint32_t i = 0; i < c->n && en->npipes < CB_PIPES; i++) {
        uint32_t k = 0;
        while (k < en->npipes && en->pipes[k] != c->pipes[i]) k++;
        if (k == en->npipes) en->pipes[en->npipes++] = c->pipes[i];
    }
}

/* After the app's submission: an empty one whose fence signals when everything before it is done. */
static void mark(vkb_hang *h, VkQueue q, uint32_t n1, const VkSubmitInfo *s1, uint32_t n2, const VkSubmitInfo2 *s2)
{
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence f;
    if (h->create_fence(h->dev, &fci, NULL, &f) != VK_SUCCESS) return;
    pthread_mutex_lock(&h->lock);
    hg_queue *hq = queue_get(h, q);
    if (!hq || hq->len == RING) {
        pthread_mutex_unlock(&h->lock);
        h->destroy_fence(h->dev, f, NULL);
        return;
    }
    hg_entry *en = &hq->ring[(hq->head + hq->len) % RING];
    memset(en, 0, sizeof(*en));
    en->fence = f;
    en->t = now_s();
    en->seq = ++hq->seq;
    for (uint32_t i = 0; i < n1; i++) {
        for (uint32_t c = 0; c < s1[i].commandBufferCount; c++) add_cb(h, en, s1[i].pCommandBuffers[c]);
        const VkTimelineSemaphoreSubmitInfo *ts = NULL;
        for (const VkBaseInStructure *p = s1[i].pNext; p; p = p->pNext)
            if (p->sType == VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO) ts = (const VkTimelineSemaphoreSubmitInfo *)p;
        for (uint32_t k = 0; k < s1[i].waitSemaphoreCount && en->nwait < WAITS; k++, en->nwait++) {
            en->wait[en->nwait] = s1[i].pWaitSemaphores[k];
            en->wait_v[en->nwait] = ts && k < ts->waitSemaphoreValueCount && ts->pWaitSemaphoreValues ? ts->pWaitSemaphoreValues[k] : 0;
        }
        for (uint32_t k = 0; k < s1[i].signalSemaphoreCount && en->nsig < WAITS; k++, en->nsig++) {
            en->sig[en->nsig] = s1[i].pSignalSemaphores[k];
            en->sig_v[en->nsig] = ts && k < ts->signalSemaphoreValueCount && ts->pSignalSemaphoreValues ? ts->pSignalSemaphoreValues[k] : 0;
        }
    }
    for (uint32_t i = 0; i < n2; i++) {
        for (uint32_t c = 0; c < s2[i].commandBufferInfoCount; c++) add_cb(h, en, s2[i].pCommandBufferInfos[c].commandBuffer);
        for (uint32_t k = 0; k < s2[i].waitSemaphoreInfoCount && en->nwait < WAITS; k++, en->nwait++) {
            en->wait[en->nwait] = s2[i].pWaitSemaphoreInfos[k].semaphore;
            en->wait_v[en->nwait] = s2[i].pWaitSemaphoreInfos[k].value;
        }
        for (uint32_t k = 0; k < s2[i].signalSemaphoreInfoCount && en->nsig < WAITS; k++, en->nsig++) {
            en->sig[en->nsig] = s2[i].pSignalSemaphoreInfos[k].semaphore;
            en->sig_v[en->nsig] = s2[i].pSignalSemaphoreInfos[k].value;
        }
    }
    hq->len++;
    /* Submitted under the lock so the ring's order is the queue's. */
    VkResult r = h->submit(q, 0, NULL, f);
    if (r != VK_SUCCESS) {
        hq->len--;
        h->destroy_fence(h->dev, f, NULL);
    }
    pthread_mutex_unlock(&h->lock);
}

static VKAPI_ATTR VkResult VKAPI_CALL hg_QueueSubmit(VkQueue q, uint32_t n, const VkSubmitInfo *s, VkFence f)
{
    vkb_hang *h = for_cmd();
    VkResult r = h->submit(q, n, s, f);
    if (r == VK_SUCCESS && n) mark(h, q, n, s, 0, NULL);
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL hg_QueueSubmit2(VkQueue q, uint32_t n, const VkSubmitInfo2 *s, VkFence f)
{
    vkb_hang *h = for_cmd();
    VkResult r = h->submit2(q, n, s, f);
    if (r == VK_SUCCESS && n) mark(h, q, 0, NULL, n, s);
    return r;
}

/* Evidence too: an image bound past the end of its memory (offset + the image's own size). The
 * GPU would read and write beyond the allocation; on Mali that stalls the job (a "hang"). BCn
 * images are decoded into larger ones, which a D3D12 heap sized for the compressed texture
 * would not hold. Logged for the first 32. */
static void check_bind(vkb_hang *h, VkImage img, VkDeviceMemory mem, VkDeviceSize off)
{
    if (!h->img_req || h->overflows >= 32) return;
    VkMemoryRequirements req;
    h->img_req(h->dev, img, &req);
    vkb_srv_mem *m = vkb_mem_find(h->t, mem, 0);
    if (req.alignment && (off & (req.alignment - 1)) && h->misaligned < 32) {
        int n = __atomic_add_fetch(&h->misaligned, 1, __ATOMIC_RELAXED);
        vkb_emu_image *r = h->t->emu ? vkb_emu_find_image(h->t->emu, img) : NULL;
        VKB_ERR("BIND MISALIGNED %d: image %llx (%ux%u, format %d) wants alignment %llu, bound at offset %llu of memory %llx", n,
                (unsigned long long)(uintptr_t)img, r ? r->extent.width : 0, r ? r->extent.height : 0, r ? r->client_format : 0,
                (unsigned long long)req.alignment, (unsigned long long)off, (unsigned long long)(uintptr_t)mem);
    }
    if (!m || off + req.size <= m->size) return;
    int n = __atomic_add_fetch(&h->overflows, 1, __ATOMIC_RELAXED);
    vkb_emu_image *r = h->t->emu ? vkb_emu_find_image(h->t->emu, img) : NULL;
    VKB_ERR("BIND OVERFLOW %d: image %llx%s (%ux%u, format %d, driver format %d) needs %llu bytes at offset %llu of memory %llx "
            "holding %llu: %llu bytes past its end",
            n, (unsigned long long)(uintptr_t)img,
            r && r->client_format != r->format ? (vkb_tx_block_bytes(r->format) ? " (BCn transcoded)" : " (BCn decoded)") : "", r ? r->extent.width : 0,
            r ? r->extent.height : 0, r ? r->client_format : 0, r ? r->format : 0, (unsigned long long)req.size,
            (unsigned long long)off, (unsigned long long)(uintptr_t)mem, (unsigned long long)m->size,
            (unsigned long long)(off + req.size - m->size));
}

static hg_obj *obj_get(vkb_hang *h, uint64_t key, int create);

static void note_bind(vkb_hang *h, VkImage img, VkDeviceMemory mem, VkDeviceSize off)
{
    pthread_mutex_lock(&h->lock);
    hg_obj *o = obj_get(h, (uint64_t)img, 0);
    if (o && o->kind == 1) {
        o->img.mem = mem;
        o->img.off = off;
    }
    pthread_mutex_unlock(&h->lock);
}

static VKAPI_ATTR VkResult VKAPI_CALL hg_BindImageMemory(VkDevice d, VkImage img, VkDeviceMemory mem, VkDeviceSize off)
{
    vkb_hang *h = cur();
    check_bind(h, img, mem, off);
    note_bind(h, img, mem, off);
    return h->bind_img(d, img, mem, off);
}

static VKAPI_ATTR VkResult VKAPI_CALL hg_BindImageMemory2(VkDevice d, uint32_t n, const VkBindImageMemoryInfo *bi)
{
    vkb_hang *h = cur();
    for (uint32_t i = 0; i < n; i++)
        if (bi[i].memory) check_bind(h, bi[i].image, bi[i].memory, bi[i].memoryOffset), note_bind(h, bi[i].image, bi[i].memory, bi[i].memoryOffset);
    return h->bind_img2(d, n, bi);
}

/* Evidence: the 64-bit values pushed to graphics stages that look like buffer device addresses are
 * checked against the buffers they were handed out for (vkd3d-proton passes root CBVs/SRVs/UAVs
 * that way). One into a destroyed buffer, or into no buffer at all, is a read the GPU faults on;
 * logged for the first 32. */
static VKAPI_ATTR VkDeviceAddress VKAPI_CALL hg_GetBufferDeviceAddress(VkDevice d, const VkBufferDeviceAddressInfo *ai)
{
    vkb_hang *h = cur();
    VkDeviceAddress a = h->bda(d, ai);
    if (!a || !h->live) return a;
    VkMemoryRequirements req;
    h->buf_req(d, ai->buffer, &req);
    pthread_mutex_lock(&h->lock);
    uint32_t i = 0;
    while (i < h->nlive && h->live[i].b != ai->buffer) i++;
    if (i == h->nlive && h->nlive < ADDRS) h->nlive++;
    if (i < h->nlive) h->live[i] = (hg_addr){a, req.size, ai->buffer};
    if (!h->lo || a < h->lo) h->lo = a;
    if (a + req.size > h->hi) h->hi = a + req.size;
    pthread_mutex_unlock(&h->lock);
    return a;
}

static VKAPI_ATTR void VKAPI_CALL hg_DestroyBuffer(VkDevice d, VkBuffer b, const VkAllocationCallbacks *al)
{
    vkb_hang *h = cur();
    if (b && h->live) {
        pthread_mutex_lock(&h->lock);
        for (uint32_t i = 0; i < h->nlive; i++)
            if (h->live[i].b == b) {
                h->dead[h->dead_next] = h->live[i];
                h->dead_next = (h->dead_next + 1) % DEAD;
                if (h->ndead < DEAD) h->ndead++;
                h->live[i] = h->live[--h->nlive];
                break;
            }
        pthread_mutex_unlock(&h->lock);
    }
    h->destroy_buf(d, b, al);
}

/* Called with the lock held. */
static void check_addr(vkb_hang *h, VkDeviceAddress v, uint32_t off)
{
    if (!h->lo || v < h->lo || v >= h->hi + (1ull << 30)) return; /* not an address of ours */
    for (uint32_t i = 0; i < h->nlive; i++)
        if (v >= h->live[i].a && v < h->live[i].a + h->live[i].size) return;
    if (h->addr_told >= 32) return;
    h->addr_told++;
    for (uint32_t i = 0; i < h->ndead; i++)
        if (v >= h->dead[i].a && v < h->dead[i].a + h->dead[i].size) {
            VKB_ERR("ADDRESS push constant (offset %u) %llx points into DESTROYED buffer %llx (%llx + %llu)", off, (unsigned long long)v,
                    (unsigned long long)(uintptr_t)h->dead[i].b, (unsigned long long)h->dead[i].a, (unsigned long long)h->dead[i].size);
            return;
        }
    VKB_ERR("ADDRESS push constant (offset %u) %llx is in no buffer handed out (span %llx..%llx, %u live)", off, (unsigned long long)v,
            (unsigned long long)h->lo, (unsigned long long)h->hi, h->nlive);
}

static VKAPI_ATTR void VKAPI_CALL hg_CmdPushConstants(VkCommandBuffer cb, VkPipelineLayout l, VkShaderStageFlags st, uint32_t off, uint32_t size,
                                                     const void *data)
{
    vkb_hang *h = for_cmd();
    if (st & VK_SHADER_STAGE_ALL_GRAPHICS) {
        pthread_mutex_lock(&h->lock);
        hg_cb *e = cb_get(h, cb, 1);
        if (e && off < PC_BYTES) {
            uint32_t n = size < PC_BYTES - off ? size : PC_BYTES - off;
            memcpy(e->pc + off, data, n);
            if (off + n > e->pc_size) e->pc_size = off + n;
        }
        pthread_mutex_unlock(&h->lock);
    }
    if (h->live && (st & (VK_SHADER_STAGE_ALL_GRAPHICS | VK_SHADER_STAGE_COMPUTE_BIT))) {
        pthread_mutex_lock(&h->lock);
        for (uint32_t o = (off + 7) & ~7u; o + 8 <= off + size; o += 8) {
            uint64_t v;
            memcpy(&v, (const uint8_t *)data + (o - off), 8);
            check_addr(h, v, o);
        }
        pthread_mutex_unlock(&h->lock);
    }
    h->push(cb, l, st, off, size, data);
}

/* Called with the lock held. */
static hg_obj *obj_get(vkb_hang *h, uint64_t key, int create)
{
    unsigned b = hk(key, OBJ_BUCKETS);
    for (hg_obj *o = h->objs[b]; o; o = o->next)
        if (o->key == key) {
            if (create) o->dead = 0; /* the handle made again */
            return o;
        }
    if (!create) return NULL;
    hg_obj *o = calloc(1, sizeof(*o));
    if (!o) return NULL;
    o->key = key;
    o->next = h->objs[b];
    h->objs[b] = o;
    return o;
}

/* Called with the lock held. */
static hg_set *set_get(vkb_hang *h, VkDescriptorSet set, uint32_t need)
{
    unsigned b = hk((uint64_t)set, OBJ_BUCKETS);
    hg_set *e = h->sets[b];
    while (e && e->set != set) e = e->next;
    if (!need) return e;
    if (!e) {
        if (!(e = calloc(1, sizeof(*e)))) return NULL;
        e->set = set;
        e->next = h->sets[b];
        h->sets[b] = e;
    }
    if (need > e->n) {
        uint32_t n = e->n ? e->n : 64;
        while (n < need) n *= 2;
        uint64_t *g = realloc(e->slot, (size_t)n * sizeof(*g));
        if (!g) return NULL;
        memset(g + e->n, 0, (size_t)(n - e->n) * sizeof(*g));
        e->slot = g;
        e->n = n;
    }
    return e;
}

static VKAPI_ATTR VkResult VKAPI_CALL hg_CreateImage(VkDevice d, const VkImageCreateInfo *ci, const VkAllocationCallbacks *a, VkImage *out)
{
    vkb_hang *h = cur();
    VkResult r = h->create_img(d, ci, a, out);
    if (r == VK_SUCCESS) {
        pthread_mutex_lock(&h->lock);
        hg_obj *o = obj_get(h, (uint64_t)*out, 1);
        if (o) {
            o->kind = 1;
            o->img.format = ci->format;
            o->img.extent = ci->extent;
            o->img.mips = ci->mipLevels;
            o->img.layers = ci->arrayLayers;
            o->img.usage = ci->usage;
            o->img.flags = ci->flags;
            o->img.tiling = ci->tiling;
            o->img.samples = ci->samples;
            o->img.mem = VK_NULL_HANDLE;
        }
        pthread_mutex_unlock(&h->lock);
    }
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL hg_CreateImageView(VkDevice d, const VkImageViewCreateInfo *ci, const VkAllocationCallbacks *a,
                                                        VkImageView *out)
{
    vkb_hang *h = cur();
    VkResult r = h->create_view(d, ci, a, out);
    if (r == VK_SUCCESS) {
        pthread_mutex_lock(&h->lock);
        hg_obj *o = obj_get(h, (uint64_t)*out, 1);
        if (o) {
            o->kind = 2;
            o->view.image = ci->image;
            o->view.format = ci->format;
            o->view.type = ci->viewType;
            o->view.base_mip = ci->subresourceRange.baseMipLevel;
            o->view.mips = ci->subresourceRange.levelCount;
            o->view.base_layer = ci->subresourceRange.baseArrayLayer;
            o->view.layers = ci->subresourceRange.layerCount;
            o->view.aspect = ci->subresourceRange.aspectMask;
        }
        pthread_mutex_unlock(&h->lock);
    }
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL hg_CreateSampler(VkDevice d, const VkSamplerCreateInfo *ci, const VkAllocationCallbacks *a, VkSampler *out)
{
    vkb_hang *h = cur();
    VkResult r = h->create_smp(d, ci, a, out);
    if (r == VK_SUCCESS) {
        pthread_mutex_lock(&h->lock);
        hg_obj *o = obj_get(h, (uint64_t)*out, 1);
        if (o) {
            o->kind = 3;
            o->smp.mag = ci->magFilter;
            o->smp.min = ci->minFilter;
            o->smp.mip = ci->mipmapMode;
            o->smp.u = ci->addressModeU;
            o->smp.v = ci->addressModeV;
            o->smp.w = ci->addressModeW;
            o->smp.aniso = ci->anisotropyEnable;
            o->smp.max_aniso = ci->maxAnisotropy;
            o->smp.compare = ci->compareEnable ? 1 + ci->compareOp : 0;
            o->smp.border = ci->borderColor;
            o->smp.unnorm = ci->unnormalizedCoordinates;
            o->smp.lod_bias = ci->mipLodBias;
            o->smp.min_lod = ci->minLod;
            o->smp.max_lod = ci->maxLod;
            for (const VkBaseInStructure *p = ci->pNext; p; p = p->pNext)
                if (p->sType == VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT) o->smp.border = 100 + o->smp.border;
        }
        pthread_mutex_unlock(&h->lock);
    }
    return r;
}

/* Binding 0 only (vkd3d-proton's bindless arrays); sampled images and samplers. */
static VKAPI_ATTR void VKAPI_CALL hg_UpdateDescriptorSets(VkDevice d, uint32_t nw, const VkWriteDescriptorSet *w, uint32_t nc,
                                                         const VkCopyDescriptorSet *c)
{
    vkb_hang *h = cur();
    pthread_mutex_lock(&h->lock);
    for (uint32_t i = 0; i < nw; i++) {
        hg_obj *so = obj_get(h, (uint64_t)w[i].dstSet, 0);
        if (so && so->kind == 4) so->set.writes++;
        if (w[i].dstBinding != 0 || !w[i].pImageInfo) continue;
        int smp = w[i].descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER;
        if (!smp && w[i].descriptorType != VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE) continue;
        hg_set *e = set_get(h, w[i].dstSet, w[i].dstArrayElement + w[i].descriptorCount);
        for (uint32_t k = 0; e && k < w[i].descriptorCount; k++)
            e->slot[w[i].dstArrayElement + k] = smp ? (uint64_t)w[i].pImageInfo[k].sampler : (uint64_t)w[i].pImageInfo[k].imageView;
    }
    for (uint32_t i = 0; i < nc; i++) {
        hg_obj *so = obj_get(h, (uint64_t)c[i].dstSet, 0);
        if (so && so->kind == 4) so->set.copies_in++;
        if (c[i].srcBinding != 0 || c[i].dstBinding != 0) continue;
        hg_set *src = set_get(h, c[i].srcSet, 0);
        if (!src) continue;
        hg_set *dst = set_get(h, c[i].dstSet, c[i].dstArrayElement + c[i].descriptorCount);
        src = set_get(h, c[i].srcSet, 0); /* the same set may have grown */
        for (uint32_t k = 0; dst && k < c[i].descriptorCount; k++)
            dst->slot[c[i].dstArrayElement + k] = c[i].srcArrayElement + k < src->n ? src->slot[c[i].srcArrayElement + k] : 0;
    }
    pthread_mutex_unlock(&h->lock);
    h->update_ds(d, nw, w, nc, c);
}

static VKAPI_ATTR VkResult VKAPI_CALL hg_AllocateDescriptorSets(VkDevice d, const VkDescriptorSetAllocateInfo *ai, VkDescriptorSet *out)
{
    vkb_hang *h = cur();
    VkResult r = h->alloc_ds(d, ai, out);
    if (r != VK_SUCCESS) return r;
    const VkDescriptorSetVariableDescriptorCountAllocateInfo *vc = NULL;
    for (const VkBaseInStructure *p = ai->pNext; p; p = p->pNext)
        if (p->sType == VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO)
            vc = (const VkDescriptorSetVariableDescriptorCountAllocateInfo *)p;
    pthread_mutex_lock(&h->lock);
    for (uint32_t i = 0; i < ai->descriptorSetCount; i++) {
        hg_obj *o = obj_get(h, (uint64_t)out[i], 1);
        if (!o) continue;
        memset(&o->set, 0, sizeof(o->set));
        o->kind = 4;
        o->set.pool = ai->descriptorPool;
        o->set.layout = ai->pSetLayouts[i];
        o->set.var = vc && i < vc->descriptorSetCount ? vc->pDescriptorCounts[i] : 0;
        o->set.t = now_s();
    }
    pthread_mutex_unlock(&h->lock);
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL hg_FreeDescriptorSets(VkDevice d, VkDescriptorPool pool, uint32_t n, const VkDescriptorSet *sets)
{
    vkb_hang *h = cur();
    pthread_mutex_lock(&h->lock);
    for (uint32_t i = 0; i < n; i++) {
        hg_obj *o = sets[i] ? obj_get(h, (uint64_t)sets[i], 0) : NULL;
        if (o && o->kind == 4) o->kind = 5, o->set.t = now_s();
    }
    pthread_mutex_unlock(&h->lock);
    return h->free_ds(d, pool, n, sets);
}

static VKAPI_ATTR VkResult VKAPI_CALL hg_ResetDescriptorPool(VkDevice d, VkDescriptorPool pool, VkDescriptorPoolResetFlags f)
{
    vkb_hang *h = cur();
    pthread_mutex_lock(&h->lock);
    for (unsigned b = 0; b < OBJ_BUCKETS; b++)
        for (hg_obj *o = h->objs[b]; o; o = o->next)
            if (o->kind == 4 && o->set.pool == pool) o->kind = 5, o->set.t = now_s();
    pthread_mutex_unlock(&h->lock);
    return h->reset_pool(d, pool, f);
}

static VKAPI_ATTR void VKAPI_CALL hg_UpdateDescriptorSetWithTemplate(VkDevice d, VkDescriptorSet set, VkDescriptorUpdateTemplate t, const void *data)
{
    vkb_hang *h = cur();
    pthread_mutex_lock(&h->lock);
    hg_obj *o = obj_get(h, (uint64_t)set, 0);
    if (o && o->kind == 4) o->set.writes++;
    pthread_mutex_unlock(&h->lock);
    h->update_tmpl(d, set, t, data);
}

/* Called with the lock held. */
static void describe_set(vkb_hang *h, uint32_t si, VkDescriptorSet set)
{
    if (!set) return;
    hg_obj *o = obj_get(h, (uint64_t)set, 0);
    hg_set *e = set_get(h, set, 0);
    if (!o || (o->kind != 4 && o->kind != 5))
        VKB_ERR("HANG       set %u = %llx: NEVER ALLOCATED (as far as seen)%s", si, (unsigned long long)set, e ? ", yet written" : "");
    else
        VKB_ERR("HANG       set %u = %llx: %s %.1f s ago, pool %llx layout %llx variable count %u, %u write(s), %u copy(ies) in, %u slots tracked", si,
                (unsigned long long)set, o->kind == 4 ? "allocated" : "FREED", now_s() - o->set.t, (unsigned long long)o->set.pool,
                (unsigned long long)o->set.layout, o->set.var, o->set.writes, o->set.copies_in, e ? e->n : 0);
}

static VKAPI_ATTR void VKAPI_CALL hg_CmdBindDescriptorSets(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout l, uint32_t first,
                                                          uint32_t n, const VkDescriptorSet *sets, uint32_t nd, const uint32_t *dyn)
{
    vkb_hang *h = for_cmd();
    if (bp == VK_PIPELINE_BIND_POINT_GRAPHICS) {
        pthread_mutex_lock(&h->lock);
        hg_cb *e = cb_get(h, cb, 1);
        for (uint32_t i = 0; e && i < n; i++)
            if (first + i < 4) e->sets[first + i] = sets[i];
        static int told;
        for (uint32_t i = 0; i < n; i++) {
            hg_obj *o = sets[i] ? obj_get(h, (uint64_t)sets[i], 0) : NULL;
            if (sets[i] && (!o || o->kind < 4) && told < 8) {
                told++;
                VKB_ERR("HANG bind of an unknown set: cb %p layout %llx first %u count %u ndyn %u; set[%u] = %llx (%s)", (void *)cb,
                        (unsigned long long)l, first, n, nd, i, (unsigned long long)sets[i], o ? "another object's handle" : "never seen");
                for (uint32_t k = 0; k < n; k++) VKB_ERR("HANG   set[%u] = %llx", k, (unsigned long long)sets[k]);
                void *bt[12];
                int nb = __builtin_return_address(0) ? 1 : 0;
                bt[0] = __builtin_return_address(0);
                for (int k = 0; k < nb; k++) VKB_ERR("HANG   called from %p (server base %p)", bt[k], (void *)vkb_hang_wanted);
            }
        }
        pthread_mutex_unlock(&h->lock);
    }
    h->bind_ds(cb, bp, l, first, n, sets, nd, dyn);
}

static void mark_dead(vkb_hang *h, uint64_t key)
{
    if (!key) return;
    pthread_mutex_lock(&h->lock);
    hg_obj *o = obj_get(h, key, 0);
    if (o) {
        o->dead = 1;
        o->dead_t = now_s();
    }
    pthread_mutex_unlock(&h->lock);
}

static VKAPI_ATTR void VKAPI_CALL hg_DestroyImage(VkDevice d, VkImage x, const VkAllocationCallbacks *a)
{
    vkb_hang *h = cur();
    mark_dead(h, (uint64_t)x);
    h->destroy_img(d, x, a);
}

static VKAPI_ATTR void VKAPI_CALL hg_DestroyImageView(VkDevice d, VkImageView x, const VkAllocationCallbacks *a)
{
    vkb_hang *h = cur();
    mark_dead(h, (uint64_t)x);
    h->destroy_view(d, x, a);
}

static VKAPI_ATTR void VKAPI_CALL hg_DestroySampler(VkDevice d, VkSampler x, const VkAllocationCallbacks *a)
{
    vkb_hang *h = cur();
    mark_dead(h, (uint64_t)x);
    h->destroy_smp(d, x, a);
}

static VKAPI_ATTR VkResult VKAPI_CALL hg_AllocateMemory(VkDevice d, const VkMemoryAllocateInfo *ai, const VkAllocationCallbacks *a, VkDeviceMemory *out)
{
    vkb_hang *h = cur();
    VkResult r = h->alloc_mem(d, ai, a, out);
    if (r == VK_SUCCESS) {
        pthread_mutex_lock(&h->lock);
        hg_obj *o = obj_get(h, (uint64_t)*out, 1);
        if (o) {
            o->kind = 6;
            o->mem.size = ai->allocationSize;
            o->mem.type = ai->memoryTypeIndex;
        }
        pthread_mutex_unlock(&h->lock);
    }
    return r;
}

static VKAPI_ATTR void VKAPI_CALL hg_FreeMemory(VkDevice d, VkDeviceMemory x, const VkAllocationCallbacks *a)
{
    vkb_hang *h = cur();
    mark_dead(h, (uint64_t)x);
    h->free_mem(d, x, a);
}

/* Called with the lock held: what slot <idx> of a bound set holds. */
static void describe_slot(vkb_hang *h, const char *what, uint32_t set_index, VkDescriptorSet set, uint32_t idx, uint32_t off)
{
    hg_set *e = set_get(h, set, 0);
    if (!e || idx >= e->n) return;
    uint64_t v = e->slot[idx];
    hg_obj *o = v ? obj_get(h, v, 0) : NULL;
    if (!v) {
        VKB_ERR("HANG       root u32 at %u = %u: set %u slot holds NOTHING written (%s)", off, idx, set_index, what);
    } else if (!o) {
        VKB_ERR("HANG       root u32 at %u = %u: set %u slot holds %llx, not seen created (destroyed and reused?)", off, idx, set_index,
                (unsigned long long)v);
    } else if (o->kind == 2) {
        hg_obj *im = obj_get(h, (uint64_t)o->view.image, 0);
        char ib[400] = "image not seen created";
        if (im && im->kind == 1) {
            hg_obj *mo = im->img.mem ? obj_get(h, (uint64_t)im->img.mem, 0) : NULL;
            char mb[120] = "no memory bound seen";
            if (mo && mo->kind == 6)
                snprintf(mb, sizeof(mb), "memory %llx + %llu of %llu type %u%s", (unsigned long long)im->img.mem, (unsigned long long)im->img.off,
                         (unsigned long long)mo->mem.size, mo->mem.type, mo->dead ? " FREED" : "");
            snprintf(ib, sizeof(ib), "image %llx%s fmt %d %ux%ux%u mips %u layers %u samples %u usage 0x%x flags 0x%x tiling %u, %s",
                     (unsigned long long)o->view.image, im->dead ? " DESTROYED" : "", im->img.format, im->img.extent.width, im->img.extent.height,
                     im->img.extent.depth, im->img.mips, im->img.layers, im->img.samples, im->img.usage, im->img.flags, im->img.tiling, mb);
        }
        VKB_ERR("HANG       root u32 at %u = %u: set %u view %llx%s fmt %d type %u mips %u+%u layers %u+%u aspect 0x%x; %s", off, idx, set_index,
                (unsigned long long)v, o->dead ? " DESTROYED" : "", o->view.format, o->view.type, o->view.base_mip, o->view.mips, o->view.base_layer,
                o->view.layers, o->view.aspect, ib);
    } else if (o->kind == 3) {
        VKB_ERR("HANG       root u32 at %u = %u: set %u sampler %llx%s filt %u/%u mip %u addr %u/%u/%u aniso %u(%.0f) cmp %u border %u unnorm %u "
                "lod %.2f..%.2f bias %.2f",
                off, idx, set_index, (unsigned long long)v, o->dead ? " DESTROYED" : "", o->smp.mag, o->smp.min, o->smp.mip, o->smp.u, o->smp.v, o->smp.w, o->smp.aniso,
                o->smp.max_aniso, o->smp.compare, o->smp.border, o->smp.unnorm, o->smp.min_lod, o->smp.max_lod, o->smp.lod_bias);
    }
}

static void count(VkCommandBuffer cb, int kind, uint32_t nev, const VkEvent *ev)
{
    vkb_hang *h = for_cmd();
    pthread_mutex_lock(&h->lock);
    hg_cb *e = cb_get(h, cb, 1);
    if (e) {
        e->k[kind]++;
        for (uint32_t i = 0; i < nev && e->nwaited < EVENTS; i++) e->waited[e->nwaited++] = ev[i];
    }
    pthread_mutex_unlock(&h->lock);
}
#define CNT(name, kind, params, args) \
    static VKAPI_ATTR void VKAPI_CALL hg_##name params { count(cb, kind, 0, NULL); for_cmd()->c_##name args; }
CNT(copy_buffer, K_COPY_BUFFER, (VkCommandBuffer cb, VkBuffer a, VkBuffer b, uint32_t n, const VkBufferCopy *r), (cb, a, b, n, r))
CNT(copy_image, K_COPY_IMAGE, (VkCommandBuffer cb, VkImage a, VkImageLayout al, VkImage b, VkImageLayout bl, uint32_t n, const VkImageCopy *r), (cb, a, al, b, bl, n, r))
CNT(copy_b2i, K_COPY_B2I, (VkCommandBuffer cb, VkBuffer a, VkImage b, VkImageLayout bl, uint32_t n, const VkBufferImageCopy *r), (cb, a, b, bl, n, r))
CNT(copy_i2b, K_COPY_I2B, (VkCommandBuffer cb, VkImage a, VkImageLayout al, VkBuffer b, uint32_t n, const VkBufferImageCopy *r), (cb, a, al, b, n, r))
CNT(blit, K_BLIT, (VkCommandBuffer cb, VkImage a, VkImageLayout al, VkImage b, VkImageLayout bl, uint32_t n, const VkImageBlit *r, VkFilter f), (cb, a, al, b, bl, n, r, f))
CNT(clear_color, K_CLEAR_COLOR, (VkCommandBuffer cb, VkImage a, VkImageLayout al, const VkClearColorValue *c, uint32_t n, const VkImageSubresourceRange *r), (cb, a, al, c, n, r))
CNT(clear_ds, K_CLEAR_DS, (VkCommandBuffer cb, VkImage a, VkImageLayout al, const VkClearDepthStencilValue *c, uint32_t n, const VkImageSubresourceRange *r), (cb, a, al, c, n, r))
CNT(clear_att, K_CLEAR_ATT, (VkCommandBuffer cb, uint32_t na, const VkClearAttachment *a, uint32_t nr, const VkClearRect *r), (cb, na, a, nr, r))
CNT(barrier, K_BARRIER, (VkCommandBuffer cb, VkPipelineStageFlags s, VkPipelineStageFlags d, VkDependencyFlags f, uint32_t nm, const VkMemoryBarrier *m, uint32_t nb, const VkBufferMemoryBarrier *b, uint32_t ni, const VkImageMemoryBarrier *i), (cb, s, d, f, nm, m, nb, b, ni, i))
CNT(barrier2, K_BARRIER2, (VkCommandBuffer cb, const VkDependencyInfo *di), (cb, di))
CNT(set_event, K_SET_EVENT, (VkCommandBuffer cb, VkEvent ev, VkPipelineStageFlags st), (cb, ev, st))
CNT(set_event2, K_SET_EVENT2, (VkCommandBuffer cb, VkEvent ev, const VkDependencyInfo *di), (cb, ev, di))
CNT(fill, K_FILL, (VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, VkDeviceSize sz, uint32_t v), (cb, b, o, sz, v))
CNT(update, K_UPDATE, (VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, VkDeviceSize sz, const void *d), (cb, b, o, sz, d))
CNT(draw_indirect, K_DRAW_INDIRECT, (VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, uint32_t n, uint32_t st), (cb, b, o, n, st))
CNT(draw_indexed_indirect, K_DRAW_INDEXED_INDIRECT, (VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, uint32_t n, uint32_t st), (cb, b, o, n, st))
CNT(dispatch_indirect, K_DISPATCH_INDIRECT, (VkCommandBuffer cb, VkBuffer b, VkDeviceSize o), (cb, b, o))
CNT(resolve, K_RESOLVE, (VkCommandBuffer cb, VkImage a, VkImageLayout al, VkImage b, VkImageLayout bl, uint32_t n, const VkImageResolve *r), (cb, a, al, b, bl, n, r))
CNT(copy_buffer2, K_COPY_BUFFER2, (VkCommandBuffer cb, const VkCopyBufferInfo2 *i), (cb, i))
static void note_copy(VkCommandBuffer cb, const hg_copy *c)
{
    vkb_hang *h = for_cmd();
    pthread_mutex_lock(&h->lock);
    hg_cb *e = cb_get(h, cb, 1);
    if (e && e->ncopy < COPIES) e->copy[e->ncopy++] = *c;
    pthread_mutex_unlock(&h->lock);
}
static VKAPI_ATTR void VKAPI_CALL hg_copy_image2(VkCommandBuffer cb, const VkCopyImageInfo2 *i)
{
    count(cb, K_COPY_IMAGE2, 0, NULL);
    hg_copy c = {K_COPY_IMAGE2, (uint64_t)i->srcImage, (uint64_t)i->dstImage, i->regionCount};
    if (i->regionCount) {
        const VkImageCopy2 *r = &i->pRegions[0];
        c.soff = r->srcOffset;
        c.off = r->dstOffset;
        c.ext = r->extent;
        c.smip = r->srcSubresource.mipLevel;
        c.slayer = r->srcSubresource.baseArrayLayer;
        c.mip = r->dstSubresource.mipLevel;
        c.layer = r->dstSubresource.baseArrayLayer;
        c.nlayers = r->dstSubresource.layerCount;
    }
    note_copy(cb, &c);
    for_cmd()->c_copy_image2(cb, i);
}
static VKAPI_ATTR void VKAPI_CALL hg_copy_b2i2(VkCommandBuffer cb, const VkCopyBufferToImageInfo2 *i)
{
    vkb_hang *h = for_cmd();
    count(cb, K_COPY_B2I2, 0, NULL);
    hg_copy c = {K_COPY_B2I2, (uint64_t)i->srcBuffer, (uint64_t)i->dstImage, i->regionCount};
    VkMemoryRequirements req = {0};
    if (h->buf_req) h->buf_req(h->dev, i->srcBuffer, &req);
    c.src_size = req.size;
    if (i->regionCount) {
        const VkBufferImageCopy2 *r = &i->pRegions[0];
        c.boff = r->bufferOffset;
        c.row = r->bufferRowLength;
        c.height = r->bufferImageHeight;
        c.off = r->imageOffset;
        c.ext = r->imageExtent;
        c.mip = r->imageSubresource.mipLevel;
        c.layer = r->imageSubresource.baseArrayLayer;
        c.nlayers = r->imageSubresource.layerCount;
    }
    note_copy(cb, &c);
    h->c_copy_b2i2(cb, i);
}
static VKAPI_ATTR void VKAPI_CALL hg_wait_events(VkCommandBuffer cb, uint32_t n, const VkEvent *ev, VkPipelineStageFlags s, VkPipelineStageFlags d,
                                                 uint32_t nm, const VkMemoryBarrier *m, uint32_t nb, const VkBufferMemoryBarrier *b, uint32_t ni,
                                                 const VkImageMemoryBarrier *i)
{
    count(cb, K_WAIT_EVENTS, n, ev);
    for_cmd()->c_wait_events(cb, n, ev, s, d, nm, m, nb, b, ni, i);
}
static VKAPI_ATTR void VKAPI_CALL hg_wait_events2(VkCommandBuffer cb, uint32_t n, const VkEvent *ev, const VkDependencyInfo *di)
{
    count(cb, K_WAIT_EVENTS2, n, ev);
    for_cmd()->c_wait_events2(cb, n, ev, di);
}

static const char *stage_name(VkShaderStageFlagBits s)
{
    switch (s) {
    case VK_SHADER_STAGE_VERTEX_BIT: return "vs";
    case VK_SHADER_STAGE_FRAGMENT_BIT: return "fs";
    case VK_SHADER_STAGE_COMPUTE_BIT: return "cs";
    case VK_SHADER_STAGE_GEOMETRY_BIT: return "gs";
    case VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT: return "tcs";
    case VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT: return "tes";
    default: return "?";
    }
}

/* Called with the lock held: the buffer an address falls in, if it is one of ours. */
static void describe_addr(vkb_hang *h, VkDeviceAddress v, uint32_t off)
{
    if (!h->lo || v < h->lo || v >= h->hi + (1ull << 30)) return;
    for (uint32_t i = 0; i < h->nlive; i++)
        if (v >= h->live[i].a && v < h->live[i].a + h->live[i].size) {
            VKB_ERR("HANG       offset %u: %llx = buffer %llx + %llu (size %llu, %llu B left)", off, (unsigned long long)v,
                    (unsigned long long)(uintptr_t)h->live[i].b, (unsigned long long)(v - h->live[i].a), (unsigned long long)h->live[i].size,
                    (unsigned long long)(h->live[i].a + h->live[i].size - v));
            return;
        }
    VKB_ERR("HANG       offset %u: %llx is in no live buffer", off, (unsigned long long)v);
}

/* Called with the lock held, once the device is lost: what the driver says faulted. */
static void tell_fault(vkb_hang *h)
{
    if (!h->fault_info) return;
    VkDeviceFaultCountsEXT counts = {VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT};
    VkResult r = h->fault_info(h->dev, &counts, NULL);
    if (r != VK_SUCCESS && r != VK_INCOMPLETE) {
        VKB_ERR("HANG fault info: vkGetDeviceFaultInfoEXT -> %d", r);
        return;
    }
    if (counts.addressInfoCount > 32) counts.addressInfoCount = 32;
    if (counts.vendorInfoCount > 32) counts.vendorInfoCount = 32;
    counts.vendorBinarySize = 0;
    VkDeviceFaultAddressInfoEXT addr[32];
    VkDeviceFaultVendorInfoEXT vend[32];
    VkDeviceFaultInfoEXT info = {VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT};
    info.pAddressInfos = counts.addressInfoCount ? addr : NULL;
    info.pVendorInfos = counts.vendorInfoCount ? vend : NULL;
    r = h->fault_info(h->dev, &counts, &info);
    VKB_ERR("HANG fault info (%d): \"%s\", %u address(es), %u vendor record(s)", r, info.description, counts.addressInfoCount,
            counts.vendorInfoCount);
    static const char *const kinds[] = {"none", "read", "write", "execute", "IP unknown", "IP invalid", "IP fault"};
    for (uint32_t i = 0; i < counts.addressInfoCount; i++) {
        VKB_ERR("HANG   fault address %llx (%s, precision %llu)", (unsigned long long)addr[i].reportedAddress,
                addr[i].addressType < 7 ? kinds[addr[i].addressType] : "?", (unsigned long long)addr[i].addressPrecision);
        describe_addr(h, addr[i].reportedAddress, 0);
        for (uint32_t k = 0; k < h->ndead; k++)
            if (addr[i].reportedAddress >= h->dead[k].a && addr[i].reportedAddress < h->dead[k].a + h->dead[k].size)
                VKB_ERR("HANG     in DESTROYED buffer %llx (%llx + %llu)", (unsigned long long)(uintptr_t)h->dead[k].b,
                        (unsigned long long)h->dead[k].a, (unsigned long long)h->dead[k].size);
    }
    for (uint32_t i = 0; i < counts.vendorInfoCount; i++)
        VKB_ERR("HANG   vendor: \"%s\" code %llx data %llx", vend[i].description, (unsigned long long)vend[i].vendorFaultCode,
                (unsigned long long)vend[i].vendorFaultData);
}

/* Called with the lock held. */
static void tell(vkb_hang *h, uint32_t qi, hg_entry *en, uint32_t behind, double age, const char *why)
{
    VKB_ERR("HANG device %u queue %u: submission #%u %s after %.1f s (%u command buffer(s), %u draw(s), %u dispatch(es), %u pipeline(s); "
            "%u submission(s) queued behind it)",
            h->t->id, qi, en->seq, why, age, en->ncb, en->draws, en->dispatches, en->npipes, behind);
    {
        char line[600];
        int len = 0;
        for (int i = 0; i < K_N && len < (int)sizeof(line) - 40; i++)
            if (en->k[i]) len += snprintf(line + len, sizeof(line) - (size_t)len, " %s=%u", k_names[i], en->k[i]);
        VKB_ERR("HANG   other commands:%s", len ? line : " none");
        for (uint32_t i = 0; i < en->ncopy; i++) {
            hg_copy *c = &en->copy[i];
            hg_obj *di = obj_get(h, c->dst, 0);
            char db[200] = "dst image not seen created";
            if (di && di->kind == 1)
                snprintf(db, sizeof(db), "dst image %llx%s fmt %d %ux%ux%u mips %u layers %u usage 0x%x flags 0x%x", (unsigned long long)c->dst,
                         di->dead ? " DESTROYED" : "", di->img.format, di->img.extent.width, di->img.extent.height, di->img.extent.depth,
                         di->img.mips, di->img.layers, di->img.usage, di->img.flags);
            if (c->kind == K_COPY_B2I2) {
                VKB_ERR("HANG   copy buffer %llx (size %llu) -> image: %u region(s), first: offset %llu row %u height %u at %d,%d,%d extent %ux%ux%u "
                        "mip %u layer %u+%u; %s",
                        (unsigned long long)c->src, (unsigned long long)c->src_size, c->nreg, (unsigned long long)c->boff, c->row, c->height,
                        c->off.x, c->off.y, c->off.z, c->ext.width, c->ext.height, c->ext.depth, c->mip, c->layer, c->nlayers, db);
            } else {
                hg_obj *si = obj_get(h, c->src, 0);
                VKB_ERR("HANG   copy image %llx (fmt %d %ux%u mips %u%s) mip %u layer %u at %d,%d,%d -> mip %u layer %u+%u at %d,%d,%d extent %ux%ux%u, "
                        "%u region(s); %s",
                        (unsigned long long)c->src, si && si->kind == 1 ? si->img.format : 0, si && si->kind == 1 ? si->img.extent.width : 0,
                        si && si->kind == 1 ? si->img.extent.height : 0, si && si->kind == 1 ? si->img.mips : 0, si && si->dead ? " DESTROYED" : "",
                        c->smip, c->slayer, c->soff.x, c->soff.y, c->soff.z, c->mip, c->layer, c->nlayers, c->off.x, c->off.y, c->off.z,
                        c->ext.width, c->ext.height, c->ext.depth, c->nreg, db);
            }
        }
        for (uint32_t i = 0; i < en->nwaited; i++)
            VKB_ERR("HANG   waits on event %llx", (unsigned long long)en->waited[i]);
    }
    for (uint32_t i = 0; i < en->nwait + en->nsig; i++) {
        int w = i < en->nwait;
        VkSemaphore sem = w ? en->wait[i] : en->sig[i - en->nwait];
        uint64_t want = w ? en->wait_v[i] : en->sig_v[i - en->nwait];
        /* Values asked for only: querying the semaphore from this thread crashed Mali's driver (a
         * binary or destroyed one, 2026-10-12). */
        VKB_ERR("HANG   %s semaphore %llx value %llu", w ? "waits on" : "signals", (unsigned long long)sem, (unsigned long long)want);
    }
    /* The other queues: what each one's oldest unfinished submission is. */
    for (uint32_t q = 0; q < h->nqueues; q++) {
        if (q == qi || !h->queues[q].len) continue;
        hg_entry *o = &h->queues[q].ring[h->queues[q].head];
        VKB_ERR("HANG   queue %u (%p): oldest unfinished submission #%u, %.1f s old, %u draw(s) %u dispatch(es), %u wait(s), %u queued", q,
                (void *)h->queues[q].q, o->seq, now_s() - o->t, o->draws, o->dispatches, o->nwait, h->queues[q].len);
        for (uint32_t i = 0; i < o->nwait; i++)
            VKB_ERR("HANG     waits on semaphore %llx value %llu", (unsigned long long)o->wait[i], (unsigned long long)o->wait_v[i]);
    }
    for (uint32_t i = 0; i < en->npipes; i++) {
        hg_pipe *p = find_pipe(h, en->pipes[i]);
        char line[400];
        int len = snprintf(line, sizeof(line), "HANG   pipeline %llx", (unsigned long long)(uintptr_t)en->pipes[i]);
        if (!p) {
            snprintf(line + len, sizeof(line) - (size_t)len, " (not seen created)");
        } else {
            len += snprintf(line + len, sizeof(line) - (size_t)len, " %s:", p->compute ? "compute" : "graphics");
            for (uint32_t s = 0; s < p->n && len < (int)sizeof(line) - 40; s++)
                len += snprintf(line + len, sizeof(line) - (size_t)len, " %s=h-%016llx", stage_name(p->stage[s]),
                                (unsigned long long)p->hash[s]);
        }
        VKB_ERR("%s", line);
    }
    for (uint32_t i = 0; i < en->ndraws; i++) {
        hg_draw *d = &en->draw[i];
        VKB_ERR("HANG   draw%s %u x %u instance(s) first %d vertex offset %d, pipeline %llx, render area %ux%u", d->indexed ? " indexed" : "",
                d->count, d->instances, d->first, d->voff, (unsigned long long)(uintptr_t)d->p, d->area.width, d->area.height);
        /* The root constants it ran with: every 8-byte value, and for addresses the buffer they fall in. */
        char line[600];
        int len = 0;
        for (uint32_t o = 0; o + 8 <= d->pc_size && len < (int)sizeof(line) - 80; o += 8) {
            uint64_t v;
            memcpy(&v, d->pc + o, 8);
            len += snprintf(line + len, sizeof(line) - (size_t)len, " %u:%llx", o, (unsigned long long)v);
        }
        VKB_ERR("HANG     push constants (%u B):%s", d->pc_size, line);
        for (uint32_t o = 0; o + 8 <= d->pc_size; o += 8) {
            uint64_t v;
            memcpy(&v, d->pc + o, 8);
            describe_addr(h, v, o);
        }
        for (int k = 0; k < 3; k++) {
            if (!d->rt[k]) continue;
            hg_obj *v = obj_get(h, (uint64_t)d->rt[k], 0);
            hg_obj *im = v && v->kind == 2 ? obj_get(h, (uint64_t)v->view.image, 0) : NULL;
            VKB_ERR("HANG     %s attachment: view %llx fmt %d -> image %llx %ux%u fmt %d usage 0x%x flags 0x%x", k == 2 ? "depth" : "colour",
                    (unsigned long long)d->rt[k], v ? v->view.format : 0, v ? (unsigned long long)v->view.image : 0ull,
                    im ? im->img.extent.width : 0, im ? im->img.extent.height : 0, im ? im->img.format : 0, im ? im->img.usage : 0,
                    im ? im->img.flags : 0);
        }
        VKB_ERR("HANG     sets bound: %llx %llx %llx %llx", (unsigned long long)d->sets[0], (unsigned long long)d->sets[1],
                (unsigned long long)d->sets[2], (unsigned long long)d->sets[3]);
        for (uint32_t si = 0; si < 4; si++) describe_set(h, si, d->sets[si]);
        /* Root constants as descriptor indices: what each bound set holds there. */
        for (uint32_t o = 80; o + 4 <= d->pc_size; o += 4) {
            uint32_t idx;
            memcpy(&idx, d->pc + o, 4);
            hg_set *s3 = d->sets[3] ? set_get(h, d->sets[3], 0) : NULL;
            if (s3 && idx < s3->n && o <= 128 && d->slot3[(o - 80) / 4] != s3->slot[idx])
                VKB_ERR("HANG       root u32 at %u = %u: set 3 slot REWRITTEN since the draw was recorded (%llx then, %llx now)", o, idx,
                        (unsigned long long)d->slot3[(o - 80) / 4], (unsigned long long)s3->slot[idx]);
            for (uint32_t si = 0; si < 4; si++)
                if (d->sets[si]) describe_slot(h, "texture/sampler", si, d->sets[si], idx, o);
        }
    }
}

static void *watch(void *arg)
{
    vkb_hang *h = arg;
    while (!h->stop) {
        usleep(300000);
        pthread_mutex_lock(&h->lock);
        for (uint32_t qi = 0; qi < h->nqueues; qi++) {
            hg_queue *hq = &h->queues[qi];
            while (hq->len) {
                hg_entry *en = &hq->ring[hq->head];
                VkResult r = h->fence_status(h->dev, en->fence);
                if (r == VK_SUCCESS) {
                    h->destroy_fence(h->dev, en->fence, NULL);
                    hq->head = (hq->head + 1) % RING;
                    hq->len--;
                    continue;
                }
                double age = now_s() - en->t;
                if (r == VK_ERROR_DEVICE_LOST) {
                    if (!h->lost_told) {
                        h->lost_told = 1;
                        tell(h, qi, en, hq->len - 1, age, "unfinished when the device was lost");
                        tell_fault(h);
                    }
                } else if (age > h->after && !en->told) {
                    en->told = 1;
                    tell(h, qi, en, hq->len - 1, age, "still running");
                }
                break;
            }
        }
        pthread_mutex_unlock(&h->lock);
    }
    return NULL;
}

int vkb_hang_wanted(void)
{
    const char *v = vkb_srv_env("VKBRIDGE_HANG_DETECT");
    if (v && *v && strcmp(v, "0")) return 1;
    const char *dir = vkb_pcache_dir();
    char path[600];
    snprintf(path, sizeof(path), "%s/hang-detect", dir ? dir : "");
    return dir && access(path, F_OK) == 0;
}

/* Hooks the driver-level table (t->real) and the dispatch table where it still points at the same
 * call, as the crash guard does: emulations that call the driver directly are seen too. */
#define HOOK(field, mine, saved)                                    \
    do {                                                            \
        h->saved = t->real.field;                                   \
        if (h->saved) {                                             \
            if (t->dt.field == h->saved) t->dt.field = mine;        \
            t->real.field = mine;                                   \
        }                                                           \
    } while (0)

void vkb_hang_device_init(vkb_srv_table *t)
{
    if (t->id >= 4096 || !vkb_hang_wanted()) return;
    if (!t->real.vkCreateFence || !t->real.vkGetFenceStatus || !t->real.vkDestroyFence || !t->real.vkQueueSubmit) return;
    vkb_hang *h = calloc(1, sizeof(*h));
    if (!h) return;
    h->t = t;
    h->dev = t->device;
    const char *ms = vkb_srv_env("VKBRIDGE_HANG_AFTER_MS");
    h->after = ms && atoi(ms) > 0 ? atoi(ms) / 1000.0 : HANG_AFTER_S;
    const char *sk = vkb_srv_env("VKBRIDGE_SKIP_SHADERS"); /* h-<hash>,h-<hash>,... */
    while (sk && *sk && h->nskip < 16) {
        while (*sk == ',' || *sk == ' ' || *sk == 'h' || *sk == '-') sk++;
        char *end;
        unsigned long long v = strtoull(sk, &end, 16);
        if (end == sk) break;
        h->skip[h->nskip++] = v;
        sk = end;
    }
    if (h->nskip) VKB_INFO("device %u: hang bisect drops draws using %u listed shader(s)", t->id, h->nskip);
    pthread_mutex_init(&h->lock, NULL);
    h->create_fence = t->real.vkCreateFence;
    h->destroy_fence = t->real.vkDestroyFence;
    h->reset_fences = t->real.vkResetFences;
    h->fence_status = t->real.vkGetFenceStatus;
    h->img_req = t->real.vkGetImageMemoryRequirements;
    h->buf_req = t->real.vkGetBufferMemoryRequirements;
    h->sem_value = t->real.vkGetSemaphoreCounterValue;
    h->event_status = t->real.vkGetEventStatus;
    PFN_vkGetDeviceProcAddr gdpa = (PFN_vkGetDeviceProcAddr)vkb_gipa(t->instance, "vkGetDeviceProcAddr");
    if (gdpa) h->fault_info = (PFN_vkGetDeviceFaultInfoEXT)gdpa(t->device, "vkGetDeviceFaultInfoEXT");
    if (h->buf_req) h->live = calloc(ADDRS, sizeof(*h->live));
    HOOK(vkCreateGraphicsPipelines, hg_CreateGraphicsPipelines, gfx);
    HOOK(vkCreateComputePipelines, hg_CreateComputePipelines, compute);
    HOOK(vkDestroyPipeline, hg_DestroyPipeline, destroy_pipe);
    HOOK(vkCreateShaderModule, hg_CreateShaderModule, create_mod);
    HOOK(vkDestroyShaderModule, hg_DestroyShaderModule, destroy_mod);
    HOOK(vkBeginCommandBuffer, hg_BeginCommandBuffer, begin);
    HOOK(vkCmdBindPipeline, hg_CmdBindPipeline, bind);
    HOOK(vkCmdDispatch, hg_CmdDispatch, dispatch);
    HOOK(vkCmdDraw, hg_CmdDraw, draw);
    HOOK(vkCmdDrawIndexed, hg_CmdDrawIndexed, draw_indexed);
    HOOK(vkCmdBeginRendering, hg_CmdBeginRendering, begin_rendering);
    HOOK(vkCmdBeginRenderPass, hg_CmdBeginRenderPass, begin_pass);
    HOOK(vkQueueSubmit, hg_QueueSubmit, submit);
    HOOK(vkQueueSubmit2, hg_QueueSubmit2, submit2);
    HOOK(vkBindImageMemory, hg_BindImageMemory, bind_img);
    HOOK(vkBindImageMemory2, hg_BindImageMemory2, bind_img2);
    HOOK(vkGetBufferDeviceAddress, hg_GetBufferDeviceAddress, bda);
    HOOK(vkDestroyBuffer, hg_DestroyBuffer, destroy_buf);
    HOOK(vkCmdPushConstants, hg_CmdPushConstants, push);
    HOOK(vkCreateImage, hg_CreateImage, create_img);
    HOOK(vkCreateImageView, hg_CreateImageView, create_view);
    HOOK(vkCreateSampler, hg_CreateSampler, create_smp);
    HOOK(vkUpdateDescriptorSets, hg_UpdateDescriptorSets, update_ds);
    HOOK(vkCmdBindDescriptorSets, hg_CmdBindDescriptorSets, bind_ds);
    HOOK(vkAllocateDescriptorSets, hg_AllocateDescriptorSets, alloc_ds);
    HOOK(vkFreeDescriptorSets, hg_FreeDescriptorSets, free_ds);
    HOOK(vkResetDescriptorPool, hg_ResetDescriptorPool, reset_pool);
    HOOK(vkUpdateDescriptorSetWithTemplate, hg_UpdateDescriptorSetWithTemplate, update_tmpl);
    HOOK(vkCmdCopyBuffer, hg_copy_buffer, c_copy_buffer);
    HOOK(vkCmdCopyImage, hg_copy_image, c_copy_image);
    HOOK(vkCmdCopyBufferToImage, hg_copy_b2i, c_copy_b2i);
    HOOK(vkCmdCopyImageToBuffer, hg_copy_i2b, c_copy_i2b);
    HOOK(vkCmdBlitImage, hg_blit, c_blit);
    HOOK(vkCmdClearColorImage, hg_clear_color, c_clear_color);
    HOOK(vkCmdClearDepthStencilImage, hg_clear_ds, c_clear_ds);
    HOOK(vkCmdClearAttachments, hg_clear_att, c_clear_att);
    HOOK(vkCmdPipelineBarrier, hg_barrier, c_barrier);
    HOOK(vkCmdPipelineBarrier2, hg_barrier2, c_barrier2);
    HOOK(vkCmdSetEvent, hg_set_event, c_set_event);
    HOOK(vkCmdSetEvent2, hg_set_event2, c_set_event2);
    HOOK(vkCmdWaitEvents, hg_wait_events, c_wait_events);
    HOOK(vkCmdWaitEvents2, hg_wait_events2, c_wait_events2);
    HOOK(vkCmdFillBuffer, hg_fill, c_fill);
    HOOK(vkCmdUpdateBuffer, hg_update, c_update);
    HOOK(vkCmdDrawIndirect, hg_draw_indirect, c_draw_indirect);
    HOOK(vkCmdDrawIndexedIndirect, hg_draw_indexed_indirect, c_draw_indexed_indirect);
    HOOK(vkCmdDispatchIndirect, hg_dispatch_indirect, c_dispatch_indirect);
    HOOK(vkCmdResolveImage, hg_resolve, c_resolve);
    HOOK(vkCmdCopyBuffer2, hg_copy_buffer2, c_copy_buffer2);
    HOOK(vkCmdCopyImage2, hg_copy_image2, c_copy_image2);
    HOOK(vkCmdCopyBufferToImage2, hg_copy_b2i2, c_copy_b2i2);
    HOOK(vkDestroyImage, hg_DestroyImage, destroy_img);
    HOOK(vkDestroyImageView, hg_DestroyImageView, destroy_view);
    HOOK(vkDestroySampler, hg_DestroySampler, destroy_smp);
    HOOK(vkAllocateMemory, hg_AllocateMemory, alloc_mem);
    HOOK(vkFreeMemory, hg_FreeMemory, free_mem);
    hangs[t->id] = h;
    __atomic_store_n(&last, h, __ATOMIC_RELEASE);
    if (pthread_create(&h->thread, NULL, watch, h)) h->thread = 0;
    VKB_INFO("device %u: GPU hang detector on (a submission running past %.3f s is logged with its pipelines)", t->id, h->after);
}

#define UNHOOK(field, mine, saved)                                  \
    do {                                                            \
        if (h->saved) {                                             \
            if (t->dt.field == mine) t->dt.field = h->saved;        \
            if (t->real.field == mine) t->real.field = h->saved;    \
        }                                                           \
    } while (0)

void vkb_hang_device_destroy(vkb_srv_table *t)
{
    if (t->id >= 4096 || !hangs[t->id]) return;
    vkb_hang *h = hangs[t->id];
    h->stop = 1;
    if (h->thread) pthread_join(h->thread, NULL);
    UNHOOK(vkCreateGraphicsPipelines, hg_CreateGraphicsPipelines, gfx);
    UNHOOK(vkCreateComputePipelines, hg_CreateComputePipelines, compute);
    UNHOOK(vkDestroyPipeline, hg_DestroyPipeline, destroy_pipe);
    UNHOOK(vkCreateShaderModule, hg_CreateShaderModule, create_mod);
    UNHOOK(vkDestroyShaderModule, hg_DestroyShaderModule, destroy_mod);
    UNHOOK(vkBeginCommandBuffer, hg_BeginCommandBuffer, begin);
    UNHOOK(vkCmdBindPipeline, hg_CmdBindPipeline, bind);
    UNHOOK(vkCmdDispatch, hg_CmdDispatch, dispatch);
    UNHOOK(vkCmdDraw, hg_CmdDraw, draw);
    UNHOOK(vkCmdDrawIndexed, hg_CmdDrawIndexed, draw_indexed);
    UNHOOK(vkCmdBeginRendering, hg_CmdBeginRendering, begin_rendering);
    UNHOOK(vkCmdBeginRenderPass, hg_CmdBeginRenderPass, begin_pass);
    UNHOOK(vkQueueSubmit, hg_QueueSubmit, submit);
    UNHOOK(vkQueueSubmit2, hg_QueueSubmit2, submit2);
    UNHOOK(vkBindImageMemory, hg_BindImageMemory, bind_img);
    UNHOOK(vkBindImageMemory2, hg_BindImageMemory2, bind_img2);
    UNHOOK(vkGetBufferDeviceAddress, hg_GetBufferDeviceAddress, bda);
    UNHOOK(vkDestroyBuffer, hg_DestroyBuffer, destroy_buf);
    UNHOOK(vkCmdPushConstants, hg_CmdPushConstants, push);
    UNHOOK(vkCreateImage, hg_CreateImage, create_img);
    UNHOOK(vkCreateImageView, hg_CreateImageView, create_view);
    UNHOOK(vkCreateSampler, hg_CreateSampler, create_smp);
    UNHOOK(vkUpdateDescriptorSets, hg_UpdateDescriptorSets, update_ds);
    UNHOOK(vkCmdBindDescriptorSets, hg_CmdBindDescriptorSets, bind_ds);
    UNHOOK(vkAllocateDescriptorSets, hg_AllocateDescriptorSets, alloc_ds);
    UNHOOK(vkFreeDescriptorSets, hg_FreeDescriptorSets, free_ds);
    UNHOOK(vkResetDescriptorPool, hg_ResetDescriptorPool, reset_pool);
    UNHOOK(vkUpdateDescriptorSetWithTemplate, hg_UpdateDescriptorSetWithTemplate, update_tmpl);
    UNHOOK(vkCmdCopyBuffer, hg_copy_buffer, c_copy_buffer);
    UNHOOK(vkCmdCopyImage, hg_copy_image, c_copy_image);
    UNHOOK(vkCmdCopyBufferToImage, hg_copy_b2i, c_copy_b2i);
    UNHOOK(vkCmdCopyImageToBuffer, hg_copy_i2b, c_copy_i2b);
    UNHOOK(vkCmdBlitImage, hg_blit, c_blit);
    UNHOOK(vkCmdClearColorImage, hg_clear_color, c_clear_color);
    UNHOOK(vkCmdClearDepthStencilImage, hg_clear_ds, c_clear_ds);
    UNHOOK(vkCmdClearAttachments, hg_clear_att, c_clear_att);
    UNHOOK(vkCmdPipelineBarrier, hg_barrier, c_barrier);
    UNHOOK(vkCmdPipelineBarrier2, hg_barrier2, c_barrier2);
    UNHOOK(vkCmdSetEvent, hg_set_event, c_set_event);
    UNHOOK(vkCmdSetEvent2, hg_set_event2, c_set_event2);
    UNHOOK(vkCmdWaitEvents, hg_wait_events, c_wait_events);
    UNHOOK(vkCmdWaitEvents2, hg_wait_events2, c_wait_events2);
    UNHOOK(vkCmdFillBuffer, hg_fill, c_fill);
    UNHOOK(vkCmdUpdateBuffer, hg_update, c_update);
    UNHOOK(vkCmdDrawIndirect, hg_draw_indirect, c_draw_indirect);
    UNHOOK(vkCmdDrawIndexedIndirect, hg_draw_indexed_indirect, c_draw_indexed_indirect);
    UNHOOK(vkCmdDispatchIndirect, hg_dispatch_indirect, c_dispatch_indirect);
    UNHOOK(vkCmdResolveImage, hg_resolve, c_resolve);
    UNHOOK(vkCmdCopyBuffer2, hg_copy_buffer2, c_copy_buffer2);
    UNHOOK(vkCmdCopyImage2, hg_copy_image2, c_copy_image2);
    UNHOOK(vkCmdCopyBufferToImage2, hg_copy_b2i2, c_copy_b2i2);
    UNHOOK(vkDestroyImage, hg_DestroyImage, destroy_img);
    UNHOOK(vkDestroyImageView, hg_DestroyImageView, destroy_view);
    UNHOOK(vkDestroySampler, hg_DestroySampler, destroy_smp);
    UNHOOK(vkAllocateMemory, hg_AllocateMemory, alloc_mem);
    UNHOOK(vkFreeMemory, hg_FreeMemory, free_mem);
    hangs[t->id] = NULL;
    if (__atomic_load_n(&last, __ATOMIC_ACQUIRE) == h) {
        vkb_hang *o = NULL;
        for (int i = 0; i < 4096 && !o; i++) o = hangs[i];
        __atomic_store_n(&last, o, __ATOMIC_RELEASE);
    }
    for (uint32_t qi = 0; qi < h->nqueues; qi++)
        for (uint32_t i = 0; i < h->queues[qi].len; i++)
            h->destroy_fence(h->dev, h->queues[qi].ring[(h->queues[qi].head + i) % RING].fence, NULL);
    for (unsigned b = 0; b < PIPE_BUCKETS; b++)
        for (hg_pipe *e = h->pipes[b], *n; e; e = n) {
            n = e->next;
            free(e);
        }
    for (unsigned b = 0; b < PIPE_BUCKETS; b++)
        for (hg_mod *e = h->mods[b], *n; e; e = n) {
            n = e->next;
            free(e);
        }
    for (unsigned b = 0; b < CB_BUCKETS; b++)
        for (hg_cb *e = h->cbs[b], *n; e; e = n) {
            n = e->next;
            free(e);
        }
    for (unsigned b = 0; b < OBJ_BUCKETS; b++) {
        for (hg_obj *e = h->objs[b], *n; e; e = n) {
            n = e->next;
            free(e);
        }
        for (hg_set *e = h->sets[b], *n; e; e = n) {
            n = e->next;
            free(e->slot);
            free(e);
        }
    }
    free(h->live);
    pthread_mutex_destroy(&h->lock);
    free(h);
}
