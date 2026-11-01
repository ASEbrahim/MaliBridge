/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim
 *
 * Fence, event and timeline semaphore status answered from shared memory, without a round trip.
 *
 * A status poll (vkGetFenceStatus, vkWaitForFences with a zero timeout, vkGetEventStatus) went to
 * the server and waited there for every request the process had queued before it: on the S10+,
 * DS3's render thread spent ~1 ms a frame in vkGetEventStatus and vkcube made 2 fence round trips
 * a frame. Like the query mirror (query.c), the GPU now writes the answer into a small buffer in
 * shared, verified-coherent memory, one u32 per fence or event, and the client reads it there.
 *
 * Fences. A submit with a fence gets two batches appended:
 *     { signal S = n }  { wait S >= n at TRANSFER; a command buffer: vkCmdFillBuffer(slot, 1); barrier to HOST }
 * S is the slot's own timeline semaphore (the server turns timelineSemaphore on wherever the
 * driver has it: server_instance.c), so the fill runs after everything submitted before it (a
 * semaphore signal's first scope) without a pipeline barrier holding up later work. Not a binary
 * one: a binary wait whose signal sits in a batch held back by a wait-before-signal blocks the
 * submit inside Mesa's drivers (the host's RADV), and so the whole stream of the app's thread.
 * No timeline semaphores (a Vulkan 1.1 device): no fence mirror. The host
 * clears the slot before the submit. A slot reading 1 means the fence's work is done; the fence
 * itself is signalled right after (the same submit's fence signal): the server holds a vkResetFences
 * or vkDestroyFence of a fence it has not yet seen signalled until it is (server_syncfb.c).
 * Answered here: SUCCESS once the slot reads 1 (or the fence was created signalled or seen
 * signalled), NOT_READY while its mirrored submit is pending or after a reset. Once in 50 ms a
 * not-ready answer is asked of the server instead, so a lost device still reaches the app and a
 * signal this file did not see (a bind sparse, an acquire) is found. A fence imported, exported
 * or given to anything else is asked of the server until its next reset.
 *
 * Events. As queries: a primary command buffer that sets or resets events (its secondaries' too)
 * gets, at vkEndCommandBuffer, a barrier from all commands, a fill of each event's final state, and
 * a barrier to the host. The slot is therefore written after the event; reading 0 while the
 * event is set is a stale answer, never a wrong one. vkSetEvent / vkResetEvent from the host
 * write the slot themselves. A command buffer whose queue family cannot fill buffers unmirrors its
 * events until the next host set or reset. The 50 ms rule applies to a reset answer too.
 *
 * Timeline semaphores. gamescope polled vkGetSemaphoreCounterValue ~580 times a second (93% of its
 * round trips), dxvk-cs once a frame. For each mirrored semaphore a submit signals, one batch more:
 * wait S >= v at TRANSFER, copy v from a staging word into the semaphore's value word, then the
 * copy entry's done word (4 entries in flight per semaphore; a signal finding all 4 busy is not
 * mirrored). The value read is the largest of that, the host's vkSignalSemaphore and the server's
 * last answer: never ahead of the GPU, never backwards. The server is asked only once the value has
 * not moved for 50 ms (a skipped signal, a lost device). Exported or imported ones: the server.
 *
 * Only where it answers. The batches above cost the GPU and the driver on every submit, so each fence and timeline
 * semaphore keeps them only while they pay (sm_gate): over a window of 8 (fence) / 32 (semaphore) mirrored submits, at
 * least half of the app's questions about it (status, waits, value reads) must have been answered here. Otherwise it
 * is not mirrored for 64 submits, then tried again for a window; every failed try doubles the pause, up to 4096.
 * DXVK signals a fence and its submission timeline with every submit and waits on them right after submitting:
 * in Portal on the Galaxy Tab S10+ (~5 submits a frame) the mirror answered none of the fence waits and ~20% of the
 * semaphore waits, and its batches cost ~6% at the same GPU clock and pushed the device into a power state with the
 * GPU capped near 600 MHz instead of ~1150 (37-48 fps instead of 57-59). Balatro's semaphore waits come after the work
 * is done and are all answered here (92 fps with the mirror, 88 without): it stays mirrored, its fences (1% answered)
 * do not. A semaphore mirrored again after signals it did not copy is answered by the server until its value is past
 * them (sm_sem.unmirrored), so a local answer is never older than a signal this file did not see.
 *
 * VKBRIDGE_SYNC_MIRROR=0 (guest, or the server's switch: a game's preset, <cache dir>/env) turns
 * this off: every status goes to the server as before.
 */
#define _GNU_SOURCE
#include "vkb_client.h"
#include "gen/vkb_client_gen.h"

#include <sched.h>
#include <string.h>
#include <time.h>

#define SM_SLOTS 8192u
#define SM_TS 2048u              /* timeline semaphores mirrored */
#define SM_TK 4u                 /* their signals in flight each */
#define SM_TVAL_OFF ((VkDeviceSize)SM_SLOTS * 4)
#define SM_TSTAGE_OFF (SM_TVAL_OFF + (VkDeviceSize)SM_TS * 8)
#define SM_TDONE_OFF (SM_TSTAGE_OFF + (VkDeviceSize)SM_TS * SM_TK * 8)
#define SM_BUF_SIZE (SM_TDONE_OFF + (VkDeviceSize)SM_TS * SM_TK * 4)
#define SM_MAX_TSIG 16u          /* timeline signals mirrored per submit */
#define SM_RECHECK_NS (50ull * 1000 * 1000)
#define SM_WIN_FENCE 8u          /* a fence's mirrored submits judged together */
#define SM_WIN_SEM 32u           /* a timeline semaphore's mirrored signals judged together */
#define SM_COOL_MIN 64u          /* submits / signals without mirroring after a window that did not pay, doubled each time */
#define SM_COOL_MAX 4096u

/* Whether an object's submits are worth mirroring: it is mirrored while at least half of the app's questions about
 * it (status, waits, value reads) are answered here. A window that answered less, or was never asked, turns it off
 * for SM_COOL_MIN submits, then it is tried again for a window; each failed try doubles the pause (to SM_COOL_MAX). */
typedef struct sm_gate {
    uint8_t on;              /* mirrored (atomic) */
    uint16_t win;            /* mirrored submits in this window (submit path) */
    uint32_t asks, hits;     /* questions while mirrored, and how many were answered here (atomic) */
    uint32_t cool, pause;    /* submits left without mirroring; the current pause (submit path) */
} sm_gate;

static void gate_init(sm_gate *g)
{
    g->on = 1;               /* new objects start mirrored, as before */
}

/* A submit or signal of the object: 1 when it is mirrored. */
static int gate_submit(sm_gate *g, uint32_t window)
{
    if (!__atomic_load_n(&g->on, __ATOMIC_ACQUIRE)) {
        if (g->cool) {
            g->cool--;
            return 0;
        }
        g->win = 0; /* a new try */
        __atomic_store_n(&g->asks, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&g->hits, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&g->on, 1, __ATOMIC_RELEASE);
    }
    if (++g->win > window) {
        uint32_t a = __atomic_exchange_n(&g->asks, 0, __ATOMIC_RELAXED), h = __atomic_exchange_n(&g->hits, 0, __ATOMIC_RELAXED);
        g->win = 1;
        if (!a || h * 2 < a) {
            g->pause = g->pause ? (g->pause * 2 > SM_COOL_MAX ? SM_COOL_MAX : g->pause * 2) : SM_COOL_MIN;
            g->cool = g->pause;
            __atomic_store_n(&g->on, 0, __ATOMIC_RELEASE);
            return 0;
        }
        g->pause = 0;
    }
    return 1;
}

static void gate_ask(sm_gate *g, int hit)
{
    if (!__atomic_load_n(&g->on, __ATOMIC_ACQUIRE)) return;
    __atomic_add_fetch(&g->asks, 1, __ATOMIC_RELAXED);
    if (hit) __atomic_add_fetch(&g->hits, 1, __ATOMIC_RELAXED);
}

enum { F_UNKNOWN = 0, F_UNSIGNALED, F_PENDING, F_SIGNALED };

typedef struct sm_fence {
    uint32_t slot;
    uint8_t state;           /* F_* (atomic) */
    sm_gate gate;            /* whether its submits are mirrored */
    uint64_t wire_ns;        /* last time the server was asked */
} sm_fence;

typedef struct sm_event {
    uint32_t slot;
    uint8_t mirrored;        /* atomic */
    uint64_t wire_ns;
} sm_event;

/* A timeline semaphore: its value is the largest of what the GPU copied into tval[ts] after the
 * semaphore reached it, what the host signalled, and what the server said. */
typedef struct sm_sem {
    uint32_t ts;
    uint8_t unknown;         /* imported, or signalled where this file does not see it: the server's answer */
    sm_gate gate;            /* whether its signals are mirrored */
    uint8_t knext;
    uint64_t unmirrored;     /* largest value a submit signalled without a copy entry while not mirrored (atomic) */
    uint64_t seen;           /* largest value known (atomic) */
    uint64_t wire_ns, change_ns;
    uint64_t vk[SM_TK];      /* value each copy entry was given; 0: never used */
} sm_sem;

typedef struct sm_fam {
    VkCommandPool pool;
    VkCommandBuffer *cbs;    /* [SM_SLOTS], recorded on first use */
    VkCommandBuffer *tcbs;   /* [SM_TS * SM_TK] */
    int failed;
} sm_fam;

typedef struct vkb_sm_dev {
    pthread_mutex_t lock;
    int disabled;
    int ready;               /* buffer made */
    VkBuffer buf;
    VkDeviceMemory mem;
    uint32_t *slots;
    uint32_t *freelist, nfree, next;
    VkSemaphore *sems;       /* [SM_SLOTS] timeline semaphores, made on first use */
    uint64_t *sem_vals;      /* [SM_SLOTS] their last value signalled */
    int timeline;            /* the device can have them */
    vkb_map fences, events, tsems;
    uint64_t *tval, *tstage;
    uint32_t *tdone;
    uint32_t *tfree, ntfree, tnext;
    uint64_t local_sem, wire_sem;
    uint32_t nfam;
    sm_fam *fam;
    VkQueueFlags *fam_flags;
    uint64_t local_fence, local_event, wire_fence, wire_event; /* counts, for the log at the end */
    uint64_t submits, extended, extra;                         /* submits seen, given batches, batches added */
} vkb_sm_dev;

typedef struct vkb_evop {
    VkEvent ev;
    uint32_t val;
} vkb_evop;

static uint64_t sm_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int sm_enabled(void)
{
    static int v = -1;
    if (v < 0) v = vkb_sync_mirror_wanted();
    return v;
}

void vkb_sm_init_device(vkb_device *d)
{
    vkb_sm_dev *sm = calloc(1, sizeof(*sm));
    if (!sm) return;
    pthread_mutex_init(&sm->lock, NULL);
    vkb_map_init(&sm->fences);
    vkb_map_init(&sm->events);
    vkb_map_init(&sm->tsems);
    sm->disabled = !sm_enabled() || d->pd->info.strategy == VKB_MEM_NONE || !d->pd->info.coherent_types;
    d->sm = sm;
}

static void map_free_all(vkb_map *m)
{
    for (uint32_t i = 0; i < m->cap; i++)
        if (m->keys[i] && m->keys[i] != ~(uint64_t)0) free(m->vals[i]);
    vkb_map_destroy(m);
}

void vkb_sm_destroy_device(vkb_device *d)
{
    extern VKAPI_ATTR void VKAPI_CALL vkb_ep_vkDestroyBuffer(VkDevice, VkBuffer, const VkAllocationCallbacks *);
    extern VKAPI_ATTR void VKAPI_CALL vkb_ep_vkFreeMemory(VkDevice, VkDeviceMemory, const VkAllocationCallbacks *);
    vkb_sm_dev *sm = d->sm;
    if (!sm) return;
    VkDevice device = (VkDevice)d;
    if (sm->submits)
        VKB_INFO("sync mirror: %llu submits, %llu of them with mirror batches (%llu batches added)", (unsigned long long)sm->submits,
                 (unsigned long long)sm->extended, (unsigned long long)sm->extra);
    if (sm->local_fence + sm->local_event + sm->local_sem)
        VKB_INFO("sync mirror: %llu fence, %llu event and %llu semaphore answers from shared memory, %llu, %llu and %llu from the server",
                 (unsigned long long)sm->local_fence, (unsigned long long)sm->local_event, (unsigned long long)sm->local_sem,
                 (unsigned long long)sm->wire_fence, (unsigned long long)sm->wire_event, (unsigned long long)sm->wire_sem);
    /* The app has waited for its device to be idle before destroying it: none of this is in use. */
    for (uint32_t f = 0; f < sm->nfam; f++)
        if (sm->fam[f].pool) vkb_ep_vkDestroyCommandPool(device, sm->fam[f].pool, NULL); /* its command buffers with it */
    for (uint32_t f = 0; f < sm->nfam; f++) free(sm->fam[f].cbs), free(sm->fam[f].tcbs);
    if (sm->sems)
        for (uint32_t i = 0; i < SM_SLOTS; i++)
            if (sm->sems[i]) vkb_wire_vkDestroySemaphore(device, sm->sems[i], NULL);
    if (sm->buf) vkb_ep_vkDestroyBuffer(device, sm->buf, NULL);
    if (sm->mem) vkb_ep_vkFreeMemory(device, sm->mem, NULL);
    free(sm->sems);
    free(sm->sem_vals);
    free(sm->fam);
    free(sm->fam_flags);
    free(sm->freelist);
    free(sm->tfree);
    map_free_all(&sm->fences);
    map_free_all(&sm->events);
    map_free_all(&sm->tsems);
    pthread_mutex_destroy(&sm->lock);
    free(sm);
    d->sm = NULL;
}

/* Under sm->lock: the shared buffer and the queue families, once. 0 when off. */
static int sm_ready_locked(vkb_device *d, vkb_sm_dev *sm)
{
    extern VKAPI_ATTR VkResult VKAPI_CALL vkb_ep_vkCreateBuffer(VkDevice, const VkBufferCreateInfo *, const VkAllocationCallbacks *, VkBuffer *);
    extern VKAPI_ATTR void VKAPI_CALL vkb_ep_vkGetBufferMemoryRequirements(VkDevice, VkBuffer, VkMemoryRequirements *);
    extern VKAPI_ATTR VkResult VKAPI_CALL vkb_ep_vkAllocateMemory(VkDevice, const VkMemoryAllocateInfo *, const VkAllocationCallbacks *,
                                                                  VkDeviceMemory *);
    if (sm->disabled) return 0;
    if (sm->ready) return 1;
    VkDevice device = (VkDevice)d;
    vkb_physdev *pd = d->pd;
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, SM_BUF_SIZE, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                              VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
    uint32_t type = UINT32_MAX;
    if (vkb_ep_vkCreateBuffer(device, &bci, NULL, &sm->buf) != VK_SUCCESS) goto fail;
    VkMemoryRequirements req;
    vkb_ep_vkGetBufferMemoryRequirements(device, sm->buf, &req);
    for (uint32_t i = 0; i < pd->client_mem.memoryTypeCount && type == UINT32_MAX; i++) {
        if (!(req.memoryTypeBits & (1u << i)) || !pd->type_map[i].shared) continue;
        if (!(pd->info.coherent_types & (1u << pd->type_map[i].server_type))) continue;
        if (pd->client_mem.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) type = i;
    }
    if (type == UINT32_MAX) goto fail;
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size, type};
    if (vkb_ep_vkAllocateMemory(device, &ai, NULL, &sm->mem) != VK_SUCCESS) goto fail;
    if (vkb_wire_vkBindBufferMemory(device, sm->buf, sm->mem, 0) != VK_SUCCESS) goto fail;
    vkb_memory *vm = vkb_map_get(&d->memories, (uint64_t)sm->mem);
    if (!vm || !vm->ptr) goto fail;
    sm->slots = vm->ptr;
    memset(sm->slots, 0, (size_t)SM_BUF_SIZE);
    sm->tval = (uint64_t *)((uint8_t *)vm->ptr + SM_TVAL_OFF);
    sm->tstage = (uint64_t *)((uint8_t *)vm->ptr + SM_TSTAGE_OFF);
    sm->tdone = (uint32_t *)((uint8_t *)vm->ptr + SM_TDONE_OFF);
    sm->freelist = malloc(SM_SLOTS * sizeof(*sm->freelist));
    sm->tfree = malloc(SM_TS * sizeof(*sm->tfree));
    sm->sems = calloc(SM_SLOTS, sizeof(*sm->sems));
    sm->sem_vals = calloc(SM_SLOTS, sizeof(*sm->sem_vals));
    sm->timeline = d->pd->props.apiVersion >= VK_API_VERSION_1_2; /* 1.2 GPUs all have them; the server turns them on, as core or as VK_KHR_timeline_semaphore */
    uint32_t n = 0;
    vkb_wire_vkGetPhysicalDeviceQueueFamilyProperties((VkPhysicalDevice)pd, &n, NULL);
    VkQueueFamilyProperties *p = calloc(n ? n : 1, sizeof(*p));
    sm->fam = calloc(n ? n : 1, sizeof(*sm->fam));
    sm->fam_flags = calloc(n ? n : 1, sizeof(*sm->fam_flags));
    if (!sm->freelist || !sm->tfree || !sm->sems || !sm->sem_vals || !p || !sm->fam || !sm->fam_flags) {
        free(p);
        goto fail;
    }
    vkb_wire_vkGetPhysicalDeviceQueueFamilyProperties((VkPhysicalDevice)pd, &n, p);
    for (uint32_t i = 0; i < n; i++) sm->fam_flags[i] = p[i].queueFlags;
    sm->nfam = n;
    free(p);
    sm->ready = 1;
    VKB_INFO("sync mirror: fence, event and timeline semaphore status from shared memory (memory type %u; VKBRIDGE_SYNC_MIRROR=0 turns it off)", type);
    return 1;
fail:
    VKB_INFO("sync mirror: no coherent shared buffer; fence and event status stay on the wire");
    sm->disabled = 1;
    return 0;
}

/* Under sm->lock. UINT32_MAX when none is left. */
static uint32_t slot_take(vkb_sm_dev *sm)
{
    if (sm->nfree) return sm->freelist[--sm->nfree];
    if (sm->next < SM_SLOTS) return sm->next++;
    VKB_ONCE(VKB_LOG_INFO, "sync mirror: more than %u fences and events at once; the rest are asked of the server", SM_SLOTS);
    return UINT32_MAX;
}

static void slot_give(vkb_sm_dev *sm, uint32_t slot)
{
    if (slot < SM_SLOTS && sm->nfree < SM_SLOTS) sm->freelist[sm->nfree++] = slot;
}

static int fam_can_fill(vkb_sm_dev *sm, uint32_t fam)
{
    return fam < sm->nfam && (sm->fam_flags[fam] & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT));
}

/* ------------------------------------------------------------------ fences */

void vkb_sm_fence_created(vkb_device *d, const VkFenceCreateInfo *ci, VkFence fence)
{
    vkb_sm_dev *sm = d->sm;
    if (!sm || sm->disabled || !fence) return;
    for (const VkBaseInStructure *p = ci->pNext; p; p = p->pNext)
        if (p->sType == VK_STRUCTURE_TYPE_EXPORT_FENCE_CREATE_INFO) return; /* an export resets it behind our back */
    pthread_mutex_lock(&sm->lock);
    uint32_t slot = sm_ready_locked(d, sm) ? slot_take(sm) : UINT32_MAX;
    pthread_mutex_unlock(&sm->lock);
    if (slot == UINT32_MAX) return;
    sm_fence *f = calloc(1, sizeof(*f));
    if (!f) {
        pthread_mutex_lock(&sm->lock);
        slot_give(sm, slot);
        pthread_mutex_unlock(&sm->lock);
        return;
    }
    f->slot = slot;
    f->state = (ci->flags & VK_FENCE_CREATE_SIGNALED_BIT) ? F_SIGNALED : F_UNSIGNALED;
    gate_init(&f->gate);
    vkb_map_put(&sm->fences, (uint64_t)fence, f);
}

void vkb_sm_fence_destroyed(vkb_device *d, VkFence fence)
{
    vkb_sm_dev *sm = d->sm;
    if (!sm || !fence) return;
    sm_fence *f = vkb_map_remove(&sm->fences, (uint64_t)fence);
    if (!f) return;
    /* A slot whose fill may still come is not handed out again. */
    int busy = __atomic_load_n(&f->state, __ATOMIC_ACQUIRE) == F_PENDING && !__atomic_load_n(&sm->slots[f->slot], __ATOMIC_ACQUIRE);
    pthread_mutex_lock(&sm->lock);
    if (!busy) slot_give(sm, f->slot);
    pthread_mutex_unlock(&sm->lock);
    free(f);
}

static sm_fence *fence_of(vkb_device *d, VkFence fence)
{
    return d->sm && fence ? vkb_map_get(&d->sm->fences, (uint64_t)fence) : NULL;
}

/* Something this file does not follow may signal or unsignal the fence: the server's answer until its next reset. */
void vkb_sm_fence_unknown(vkb_device *d, VkFence fence)
{
    sm_fence *f = fence_of(d, fence);
    if (f) __atomic_store_n(&f->state, F_UNKNOWN, __ATOMIC_RELEASE);
}

void vkb_sm_fences_reset(vkb_device *d, uint32_t n, const VkFence *fences)
{
    for (uint32_t i = 0; i < n; i++) {
        sm_fence *f = fence_of(d, fences[i]);
        if (f) __atomic_store_n(&f->state, F_UNSIGNALED, __ATOMIC_RELEASE);
    }
}

/* The feedback command buffer and semaphore of a slot on a queue family. Under sm->lock. */
static int slot_cb_locked(vkb_device *d, vkb_sm_dev *sm, uint32_t fam, uint32_t slot, VkCommandBuffer *cb, VkSemaphore *sem, uint64_t *val)
{
    extern VKAPI_ATTR VkResult VKAPI_CALL vkb_ep_vkCreateCommandPool(VkDevice, const VkCommandPoolCreateInfo *, const VkAllocationCallbacks *,
                                                                     VkCommandPool *);
    extern VKAPI_ATTR VkResult VKAPI_CALL vkb_ep_vkAllocateCommandBuffers(VkDevice, const VkCommandBufferAllocateInfo *, VkCommandBuffer *);
    extern VKAPI_ATTR VkResult VKAPI_CALL vkb_ep_vkBeginCommandBuffer(VkCommandBuffer, const VkCommandBufferBeginInfo *);
    extern VKAPI_ATTR VkResult VKAPI_CALL vkb_ep_vkEndCommandBuffer(VkCommandBuffer);
    VkDevice device = (VkDevice)d;
    if (!sm->timeline || !fam_can_fill(sm, fam)) return 0;
    sm_fam *sf = &sm->fam[fam];
    if (sf->failed) return 0;
    if (!sf->pool) {
        VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, 0, fam};
        sf->cbs = calloc(SM_SLOTS, sizeof(*sf->cbs));
        if (!sf->cbs || vkb_ep_vkCreateCommandPool(device, &pci, NULL, &sf->pool) != VK_SUCCESS) {
            sf->pool = VK_NULL_HANDLE;
            sf->failed = 1;
            return 0;
        }
    }
    if (!sm->sems[slot]) {
        VkSemaphoreTypeCreateInfo ti = {VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, NULL, VK_SEMAPHORE_TYPE_TIMELINE, 0};
        VkSemaphoreCreateInfo sci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &ti};
        if (vkb_wire_vkCreateSemaphore(device, &sci, NULL, &sm->sems[slot]) != VK_SUCCESS) {
            sm->sems[slot] = VK_NULL_HANDLE;
            sm->timeline = 0;
            VKB_INFO("sync mirror: no timeline semaphore; fence status stays on the wire");
            return 0;
        }
        sm->sem_vals[slot] = 0;
    }
    if (!sf->cbs[slot]) {
        VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, sf->pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
        VkCommandBuffer c = VK_NULL_HANDLE;
        if (vkb_ep_vkAllocateCommandBuffers(device, &cai, &c) != VK_SUCCESS) return 0;
        /* Simultaneous use: an app that resets and resubmits its fence the moment the slot reads 1
         * may resubmit this while the GPU is still finishing it. */
        VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT, NULL};
        vkb_ep_vkBeginCommandBuffer(c, &bi);
        vkb_wire_vkCmdFillBuffer(c, sm->buf, (VkDeviceSize)slot * 4, 4, 1);
        VkMemoryBarrier host = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
        vkb_wire_vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, NULL, 0, NULL);
        if (vkb_ep_vkEndCommandBuffer(c) != VK_SUCCESS) return 0;
        sf->cbs[slot] = c;
    }
    *cb = sf->cbs[slot];
    *sem = sm->sems[slot];
    *val = ++sm->sem_vals[slot];
    return 1;
}

/* Only chains this file knows to be harmless next to two plain batches appended. */
static int chain_plain(const void *pnext)
{
    for (const VkBaseInStructure *p = pnext; p; p = p->pNext)
        if (p->sType != VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO && p->sType != VK_STRUCTURE_TYPE_PERFORMANCE_QUERY_SUBMIT_INFO_KHR)
            return 0;
    return 1;
}

/* Under nothing. 1 with the fence's slot cleared and its batches ready, 0: submit it as it is. */
static int fence_prepare(VkQueue queue, VkFence fence, sm_fence **pf, VkCommandBuffer *cb, VkSemaphore *sem, uint64_t *val)
{
    vkb_queue *q = (vkb_queue *)queue;
    vkb_device *d = q->device;
    sm_fence *f = fence_of(d, fence);
    if (!f) return 0;
    if (__atomic_load_n(&f->state, __ATOMIC_ACQUIRE) != F_UNSIGNALED || !gate_submit(&f->gate, SM_WIN_FENCE)) {
        __atomic_store_n(&f->state, F_UNKNOWN, __ATOMIC_RELEASE);
        return 0;
    }
    vkb_sm_dev *sm = d->sm;
    pthread_mutex_lock(&sm->lock);
    int ok = slot_cb_locked(d, sm, q->family, f->slot, cb, sem, val);
    pthread_mutex_unlock(&sm->lock);
    if (!ok) {
        __atomic_store_n(&f->state, F_UNKNOWN, __ATOMIC_RELEASE);
        return 0;
    }
    __atomic_store_n(&sm->slots[f->slot], 0, __ATOMIC_RELEASE);
    __atomic_store_n(&f->wire_ns, sm_now(), __ATOMIC_RELAXED); /* not asked of the server before 50 ms pending */
    __atomic_store_n(&f->state, F_PENDING, __ATOMIC_RELEASE);
    *pf = f;
    return 1;
}

/* ------------------------------------------------------------------ timeline semaphores */

static sm_sem *sem_of(vkb_device *d, VkSemaphore s)
{
    return d->sm && s ? vkb_map_get(&d->sm->tsems, (uint64_t)s) : NULL;
}

/* The copy command buffer of entry k of mirrored semaphore ts, on a queue family: stage -> tval,
 * then the entry's done word. Under sm->lock. */
static VkCommandBuffer tl_cb_locked(vkb_device *d, vkb_sm_dev *sm, uint32_t fam, uint32_t e)
{
    extern VKAPI_ATTR VkResult VKAPI_CALL vkb_ep_vkCreateCommandPool(VkDevice, const VkCommandPoolCreateInfo *, const VkAllocationCallbacks *,
                                                                     VkCommandPool *);
    extern VKAPI_ATTR VkResult VKAPI_CALL vkb_ep_vkAllocateCommandBuffers(VkDevice, const VkCommandBufferAllocateInfo *, VkCommandBuffer *);
    extern VKAPI_ATTR VkResult VKAPI_CALL vkb_ep_vkBeginCommandBuffer(VkCommandBuffer, const VkCommandBufferBeginInfo *);
    extern VKAPI_ATTR VkResult VKAPI_CALL vkb_ep_vkEndCommandBuffer(VkCommandBuffer);
    VkDevice device = (VkDevice)d;
    if (!fam_can_fill(sm, fam)) return VK_NULL_HANDLE;
    sm_fam *sf = &sm->fam[fam];
    if (sf->failed) return VK_NULL_HANDLE;
    if (!sf->pool) {
        VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, 0, fam};
        sf->cbs = calloc(SM_SLOTS, sizeof(*sf->cbs));
        if (!sf->cbs || vkb_ep_vkCreateCommandPool(device, &pci, NULL, &sf->pool) != VK_SUCCESS) {
            sf->pool = VK_NULL_HANDLE;
            sf->failed = 1;
            return VK_NULL_HANDLE;
        }
    }
    if (!sf->tcbs && !(sf->tcbs = calloc(SM_TS * SM_TK, sizeof(*sf->tcbs)))) return VK_NULL_HANDLE;
    if (!sf->tcbs[e]) {
        VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, sf->pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
        VkCommandBuffer c = VK_NULL_HANDLE;
        if (vkb_ep_vkAllocateCommandBuffers(device, &cai, &c) != VK_SUCCESS) return VK_NULL_HANDLE;
        VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT, NULL};
        vkb_ep_vkBeginCommandBuffer(c, &bi);
        VkBufferCopy copy = {SM_TSTAGE_OFF + (VkDeviceSize)e * 8, SM_TVAL_OFF + (VkDeviceSize)(e / SM_TK) * 8, 8};
        vkb_wire_vkCmdCopyBuffer(c, sm->buf, sm->buf, 1, &copy);
        VkMemoryBarrier xx = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT};
        vkb_wire_vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &xx, 0, NULL, 0, NULL);
        vkb_wire_vkCmdFillBuffer(c, sm->buf, SM_TDONE_OFF + (VkDeviceSize)e * 4, 4, 1);
        VkMemoryBarrier host = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
        vkb_wire_vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, NULL, 0, NULL);
        if (vkb_ep_vkEndCommandBuffer(c) != VK_SUCCESS) return VK_NULL_HANDLE;
        sf->tcbs[e] = c;
    }
    return sf->tcbs[e];
}

typedef struct tl_sig {
    VkSemaphore sem;
    uint64_t val;
    VkCommandBuffer cb;
} tl_sig;

static void tl_note(tl_sig *sig, uint32_t *n, VkSemaphore s, uint64_t v)
{
    for (uint32_t i = 0; i < *n; i++)
        if (sig[i].sem == s) {
            if (v > sig[i].val) sig[i].val = v;
            return;
        }
    if (*n < SM_MAX_TSIG) sig[(*n)++] = (tl_sig){s, v, VK_NULL_HANDLE};
}

/* For each mirrored timeline semaphore the submit signals: a copy entry staged with its value. A
 * semaphore whose entries are all still in flight is not mirrored this time (its value is then
 * asked of the server once it stops moving). Returns how many are ready, packed at the front. */
static uint32_t tl_prepare(vkb_queue *q, tl_sig *sig, uint32_t n)
{
    vkb_device *d = q->device;
    vkb_sm_dev *sm = d->sm;
    uint32_t k = 0;
    if (!n) return 0;
    pthread_mutex_lock(&sm->lock);
    for (uint32_t i = 0; i < n; i++) {
        sm_sem *ss = sem_of(d, sig[i].sem);
        if (!ss || ss->unknown) continue;
        if (!gate_submit(&ss->gate, SM_WIN_SEM)) {
            /* Not mirrored: no copy batch. Its value is the server's until the GPU is known to be past this signal. */
            uint64_t cur = __atomic_load_n(&ss->unmirrored, __ATOMIC_RELAXED);
            while (sig[i].val > cur && !__atomic_compare_exchange_n(&ss->unmirrored, &cur, sig[i].val, 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {}
            continue;
        }
        uint32_t e = ss->ts * SM_TK + ss->knext;
        if (ss->vk[ss->knext] && !__atomic_load_n(&sm->tdone[e], __ATOMIC_ACQUIRE)) continue;
        VkCommandBuffer cb = tl_cb_locked(d, sm, q->family, e);
        if (!cb) continue;
        __atomic_store_n(&sm->tdone[e], 0, __ATOMIC_RELEASE);
        __atomic_store_n(&sm->tstage[e], sig[i].val, __ATOMIC_RELEASE);
        ss->vk[ss->knext] = sig[i].val;
        ss->knext = (uint8_t)((ss->knext + 1) % SM_TK);
        sig[k] = sig[i];
        sig[k++].cb = cb;
    }
    pthread_mutex_unlock(&sm->lock);
    return k;
}

/* Only chains this file knows to be harmless next to batches appended. */
static int chain_plain2(const void *pnext)
{
    for (const VkBaseInStructure *p = pnext; p; p = p->pNext)
        if (p->sType != VK_STRUCTURE_TYPE_PERFORMANCE_QUERY_SUBMIT_INFO_KHR) return 0;
    return 1;
}

VkResult vkb_sm_queue_submit(VkQueue queue, uint32_t n, const VkSubmitInfo *si, VkFence fence)
{
    vkb_queue *q = (vkb_queue *)queue;
    vkb_device *d = q->device;
    if (!d->sm || d->sm->disabled || !d->sm->ready) return vkb_wire_vkQueueSubmit(queue, n, si, fence);
    __atomic_add_fetch(&d->sm->submits, 1, __ATOMIC_RELAXED);
    int plain = n <= 64; /* the appended batches go on the stack */
    for (uint32_t i = 0; i < n && plain; i++) plain = chain_plain(si[i].pNext);
    tl_sig sig[SM_MAX_TSIG];
    uint32_t nsig = 0;
    for (uint32_t i = 0; i < n && plain; i++)
        for (const VkBaseInStructure *p = si[i].pNext; p; p = p->pNext)
            if (p->sType == VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO) {
                const VkTimelineSemaphoreSubmitInfo *t = (const VkTimelineSemaphoreSubmitInfo *)p;
                for (uint32_t j = 0; j < si[i].signalSemaphoreCount && j < t->signalSemaphoreValueCount && t->pSignalSemaphoreValues; j++)
                    if (sem_of(d, si[i].pSignalSemaphores[j])) tl_note(sig, &nsig, si[i].pSignalSemaphores[j], t->pSignalSemaphoreValues[j]);
            }
    sm_fence *f = NULL;
    VkCommandBuffer fcb;
    VkSemaphore fsem;
    uint64_t fval;
    int fm = fence && plain && fence_prepare(queue, fence, &f, &fcb, &fsem, &fval);
    if (fence && !plain) vkb_sm_fence_unknown(d, fence);
    nsig = plain ? tl_prepare(q, sig, nsig) : 0;
    if (!fm && !nsig) return vkb_wire_vkQueueSubmit(queue, n, si, fence);
    VkSubmitInfo all[n + 3];
    if (n) memcpy(all, si, n * sizeof(*all));
    uint32_t m = n;
    VkSemaphore tw[SM_MAX_TSIG];
    uint64_t twv[SM_MAX_TSIG];
    VkPipelineStageFlags tst[SM_MAX_TSIG];
    VkCommandBuffer tcb[SM_MAX_TSIG];
    for (uint32_t i = 0; i < nsig; i++) tw[i] = sig[i].sem, twv[i] = sig[i].val, tst[i] = VK_PIPELINE_STAGE_TRANSFER_BIT, tcb[i] = sig[i].cb;
    VkTimelineSemaphoreSubmitInfo twait = {VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO, NULL, nsig, twv, 0, NULL};
    if (nsig) all[m++] = (VkSubmitInfo){VK_STRUCTURE_TYPE_SUBMIT_INFO, &twait, nsig, tw, tst, nsig, tcb, 0, NULL};
    static const VkPipelineStageFlags at = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkTimelineSemaphoreSubmitInfo fsig = {VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO, NULL, 0, NULL, 1, &fval};
    VkTimelineSemaphoreSubmitInfo fwait = {VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO, NULL, 1, &fval, 0, NULL};
    if (fm) {
        all[m++] = (VkSubmitInfo){VK_STRUCTURE_TYPE_SUBMIT_INFO, &fsig, 0, NULL, NULL, 0, NULL, 1, &fsem};
        all[m++] = (VkSubmitInfo){VK_STRUCTURE_TYPE_SUBMIT_INFO, &fwait, 1, &fsem, &at, 1, &fcb, 0, NULL};
    }
    __atomic_add_fetch(&d->sm->extended, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&d->sm->extra, m - n, __ATOMIC_RELAXED);
    VkResult r = vkb_wire_vkQueueSubmit(queue, m, all, fence);
    if (r != VK_SUCCESS && f) __atomic_store_n(&f->state, F_UNKNOWN, __ATOMIC_RELEASE);
    return r;
}

VkResult vkb_sm_queue_submit2(VkQueue queue, uint32_t n, const VkSubmitInfo2 *si, VkFence fence)
{
    vkb_queue *q = (vkb_queue *)queue;
    vkb_device *d = q->device;
    if (!d->sm || d->sm->disabled || !d->sm->ready) return vkb_wire_vkQueueSubmit2(queue, n, si, fence);
    __atomic_add_fetch(&d->sm->submits, 1, __ATOMIC_RELAXED);
    int plain = n <= 64;
    for (uint32_t i = 0; i < n && plain; i++) plain = chain_plain2(si[i].pNext) && !(si[i].flags & VK_SUBMIT_PROTECTED_BIT);
    tl_sig sig[SM_MAX_TSIG];
    uint32_t nsig = 0;
    for (uint32_t i = 0; i < n && plain; i++)
        for (uint32_t j = 0; j < si[i].signalSemaphoreInfoCount; j++) {
            const VkSemaphoreSubmitInfo *ss = &si[i].pSignalSemaphoreInfos[j];
            if (sem_of(d, ss->semaphore)) tl_note(sig, &nsig, ss->semaphore, ss->value);
        }
    sm_fence *f = NULL;
    VkCommandBuffer fcb;
    VkSemaphore fsem;
    uint64_t fval;
    int fm = fence && plain && fence_prepare(queue, fence, &f, &fcb, &fsem, &fval);
    if (fence && !plain) vkb_sm_fence_unknown(d, fence);
    nsig = plain ? tl_prepare(q, sig, nsig) : 0;
    if (!fm && !nsig) return vkb_wire_vkQueueSubmit2(queue, n, si, fence);
    VkSubmitInfo2 all[n + 3];
    if (n) memcpy(all, si, n * sizeof(*all));
    uint32_t m = n;
    VkSemaphoreSubmitInfo tw[SM_MAX_TSIG];
    VkCommandBufferSubmitInfo tcb[SM_MAX_TSIG];
    for (uint32_t i = 0; i < nsig; i++) {
        tw[i] = (VkSemaphoreSubmitInfo){VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, NULL, sig[i].sem, sig[i].val, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, 0};
        tcb[i] = (VkCommandBufferSubmitInfo){VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, NULL, sig[i].cb, 0};
    }
    if (nsig) all[m++] = (VkSubmitInfo2){VK_STRUCTURE_TYPE_SUBMIT_INFO_2, NULL, 0, nsig, tw, nsig, tcb, 0, NULL};
    VkSemaphoreSubmitInfo fs = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, NULL, fsem, fval, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0};
    VkSemaphoreSubmitInfo fw = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, NULL, fsem, fval, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, 0};
    VkCommandBufferSubmitInfo fci = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, NULL, fcb, 0};
    if (fm) {
        all[m++] = (VkSubmitInfo2){VK_STRUCTURE_TYPE_SUBMIT_INFO_2, NULL, 0, 0, NULL, 0, NULL, 1, &fs};
        all[m++] = (VkSubmitInfo2){VK_STRUCTURE_TYPE_SUBMIT_INFO_2, NULL, 0, 1, &fw, 1, &fci, 0, NULL};
    }
    __atomic_add_fetch(&d->sm->extended, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&d->sm->extra, m - n, __ATOMIC_RELAXED);
    VkResult r = vkb_wire_vkQueueSubmit2(queue, m, all, fence);
    if (r != VK_SUCCESS && f) __atomic_store_n(&f->state, F_UNKNOWN, __ATOMIC_RELEASE);
    return r;
}

void vkb_sm_sem_created(vkb_device *d, const VkSemaphoreCreateInfo *ci, VkSemaphore s)
{
    vkb_sm_dev *sm = d->sm;
    if (!sm || sm->disabled || !s) return;
    uint64_t init = 0;
    int timeline = 0;
    for (const VkBaseInStructure *p = ci->pNext; p; p = p->pNext) {
        if (p->sType == VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO) return; /* another process may signal it */
        if (p->sType == VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO) {
            const VkSemaphoreTypeCreateInfo *t = (const VkSemaphoreTypeCreateInfo *)p;
            timeline = t->semaphoreType == VK_SEMAPHORE_TYPE_TIMELINE;
            init = t->initialValue;
        }
    }
    if (!timeline) return;
    pthread_mutex_lock(&sm->lock);
    uint32_t ts = UINT32_MAX;
    if (sm_ready_locked(d, sm)) {
        if (sm->ntfree) ts = sm->tfree[--sm->ntfree];
        else if (sm->tnext < SM_TS) ts = sm->tnext++;
        else VKB_ONCE(VKB_LOG_INFO, "sync mirror: more than %u timeline semaphores at once; the rest are asked of the server", SM_TS);
    }
    pthread_mutex_unlock(&sm->lock);
    if (ts == UINT32_MAX) return;
    sm_sem *ss = calloc(1, sizeof(*ss));
    if (!ss) {
        pthread_mutex_lock(&sm->lock);
        sm->tfree[sm->ntfree++] = ts;
        pthread_mutex_unlock(&sm->lock);
        return;
    }
    ss->ts = ts;
    ss->seen = init;
    gate_init(&ss->gate);
    ss->change_ns = ss->wire_ns = sm_now();
    __atomic_store_n(&sm->tval[ts], 0, __ATOMIC_RELEASE);
    for (uint32_t k = 0; k < SM_TK; k++) __atomic_store_n(&sm->tdone[ts * SM_TK + k], 0, __ATOMIC_RELEASE);
    vkb_map_put(&sm->tsems, (uint64_t)s, ss);
}

/* Before the app's vkDestroySemaphore goes out: a copy batch waits on the semaphore right after the
 * app's own signal, and the app may destroy it the moment its signal is done. Waited for here
 * (it runs within microseconds of that signal; at most 100 ms for one whose signal never came). */
void vkb_sm_sem_settle(vkb_device *d, VkSemaphore s)
{
    sm_sem *ss = sem_of(d, s);
    if (!ss) return;
    vkb_sm_dev *sm = d->sm;
    uint64_t t0 = sm_now();
    for (uint32_t k = 0; k < SM_TK; k++)
        while (ss->vk[k] && !__atomic_load_n(&sm->tdone[ss->ts * SM_TK + k], __ATOMIC_ACQUIRE) && sm_now() - t0 < 100000000ull) {
            struct timespec ts = {0, 20000};
            nanosleep(&ts, NULL);
        }
}

/* The spec has every operation on it done: its copies too, unless a copy entry never ran (then the slot is kept). */
void vkb_sm_sem_destroyed(vkb_device *d, VkSemaphore s)
{
    vkb_sm_dev *sm = d->sm;
    if (!sm || !s) return;
    sm_sem *ss = vkb_map_remove(&sm->tsems, (uint64_t)s);
    if (!ss) return;
    int busy = 0;
    for (uint32_t k = 0; k < SM_TK; k++)
        if (ss->vk[k] && !__atomic_load_n(&sm->tdone[ss->ts * SM_TK + k], __ATOMIC_ACQUIRE)) busy = 1;
    pthread_mutex_lock(&sm->lock);
    if (!busy && sm->ntfree < SM_TS) sm->tfree[sm->ntfree++] = ss->ts;
    pthread_mutex_unlock(&sm->lock);
    free(ss);
}

void vkb_sm_sem_unknown(vkb_device *d, VkSemaphore s)
{
    sm_sem *ss = sem_of(d, s);
    if (ss) __atomic_store_n(&ss->unknown, 1, __ATOMIC_RELEASE);
}

static void sem_raise(sm_sem *ss, uint64_t v, uint64_t now)
{
    uint64_t cur = __atomic_load_n(&ss->seen, __ATOMIC_ACQUIRE);
    while (v > cur)
        if (__atomic_compare_exchange_n(&ss->seen, &cur, v, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            __atomic_store_n(&ss->change_ns, now, __ATOMIC_RELAXED);
            break;
        }
}

/* The value known here; *recheck when it has not moved for 50 ms and the server was not asked since. -1 (as u64): not mirrored. */
static int sem_local(vkb_sm_dev *sm, sm_sem *ss, uint64_t now, uint64_t *v, int *recheck)
{
    if (!ss || __atomic_load_n(&ss->unknown, __ATOMIC_ACQUIRE) || !__atomic_load_n(&ss->gate.on, __ATOMIC_ACQUIRE)) return 0;
    sem_raise(ss, __atomic_load_n(&sm->tval[ss->ts], __ATOMIC_ACQUIRE), now);
    *v = __atomic_load_n(&ss->seen, __ATOMIC_ACQUIRE);
    if (*v < __atomic_load_n(&ss->unmirrored, __ATOMIC_ACQUIRE)) return 0; /* a signal it did not copy may still be on the GPU */
    if (now - __atomic_load_n(&ss->change_ns, __ATOMIC_RELAXED) >= SM_RECHECK_NS &&
        now - __atomic_load_n(&ss->wire_ns, __ATOMIC_RELAXED) >= SM_RECHECK_NS)
        *recheck = 1;
    return 1;
}

int vkb_sm_sem_value(vkb_device *d, VkSemaphore s, uint64_t *out, VkResult *r)
{
    sm_sem *ss = sem_of(d, s);
    if (!ss || __atomic_load_n(&ss->unknown, __ATOMIC_ACQUIRE)) return 0;
    vkb_sm_dev *sm = d->sm;
    uint64_t now = sm_now(), v = 0;
    int recheck = 0;
    if (!sem_local(sm, ss, now, &v, &recheck)) {
        /* Not mirrored, or a signal it did not copy may still be running: the server's answer. */
        __atomic_add_fetch(&sm->wire_sem, 1, __ATOMIC_RELAXED);
        __atomic_store_n(&ss->wire_ns, now, __ATOMIC_RELAXED);
        *r = vkb_wire_vkGetSemaphoreCounterValue((VkDevice)d, s, out);
        if (*r == VK_SUCCESS) sem_raise(ss, *out, now);
        return 1;
    }
    if (!recheck) {
        __atomic_add_fetch(&sm->local_sem, 1, __ATOMIC_RELAXED);
        gate_ask(&ss->gate, 1);
        *out = v;
        *r = VK_SUCCESS;
        return 1;
    }
    __atomic_add_fetch(&sm->wire_sem, 1, __ATOMIC_RELAXED);
    __atomic_store_n(&ss->wire_ns, now, __ATOMIC_RELAXED);
    gate_ask(&ss->gate, 1); /* the 50 ms check: still a mirror answer in effect */
    *r = vkb_wire_vkGetSemaphoreCounterValue((VkDevice)d, s, out);
    if (*r == VK_SUCCESS) {
        sem_raise(ss, *out, now);
        if (*out < v) *out = v; /* never backwards */
    }
    return 1;
}

void vkb_sm_sem_host_signal(vkb_device *d, VkSemaphore s, uint64_t v)
{
    sm_sem *ss = sem_of(d, s);
    if (ss) sem_raise(ss, v, sm_now());
}

/* Answered here when every semaphore is mirrored and the wait is met, or it has no time. */
/* VKBRIDGE_SEM_WAIT_LOCAL=1 (guest env, off by default; lite mode). A timeline wait not met yet
 * went to the server, as a request behind everything the process queued before it: the server
 * runs it only after replaying those (Stray: ~70 vkEndCommandBuffer a frame, ~11 ms), so DXVK's
 * queue thread heard of a finished submission that much late, and DXVK holds its next flush while
 * it believes two submissions are still on the GPU (util_flush.cpp), with UE4's render thread
 * spinning on a query that the unflushed work would end. Here the wait is met from the mirror
 * word: a short spin with sched_yield, then 50 us sleeps; every 4 ms one status poll goes to the
 * server without the barrier (a signal the mirror did not see: all 4 entries busy, a lost device);
 * after 50 ms without progress, or a semaphore the mirror does not know, the server waits as before. */
static int sem_wait_local_on(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("VKBRIDGE_SEM_WAIT_LOCAL");
        v = e && *e && strcmp(e, "0") != 0;
        if (v) VKB_INFO("sync mirror: timeline waits met from the mirror (VKBRIDGE_SEM_WAIT_LOCAL=1)");
    }
    return v;
}

static uint64_t local_waits, local_wait_wire;

/* 1: met (or VK_TIMEOUT in *r), 0: hand it to the server. */
static int sem_wait_local(vkb_device *d, const VkSemaphoreWaitInfo *wi, uint64_t timeout, VkResult *r)
{
    vkb_sm_dev *sm = d->sm;
    int any = (wi->flags & VK_SEMAPHORE_WAIT_ANY_BIT) != 0;
    uint64_t t0 = sm_now(), last_poll = t0, last_move = t0, prev_sum = 0;
    uint64_t deadline = timeout > UINT64_MAX - t0 ? UINT64_MAX : t0 + timeout;
    for (uint32_t spin = 0;; spin++) {
        uint64_t now = sm_now(), sum = 0;
        uint32_t met = 0;
        int poll = now - last_poll >= 4000000ull;
        for (uint32_t i = 0; i < wi->semaphoreCount; i++) {
            sm_sem *ss = sem_of(d, wi->pSemaphores[i]);
            uint64_t v = 0;
            int recheck = 0;
            if (!sem_local(sm, ss, now, &v, &recheck)) return 0;
            if (poll && v < wi->pValues[i]) {
                uint64_t w = 0;
                vkb_call_poll_next(); /* no barrier: the GPU's state now, not after the queued requests */
                if (vkb_wire_vkGetSemaphoreCounterValue((VkDevice)d, wi->pSemaphores[i], &w) == VK_SUCCESS) sem_raise(ss, w, now);
                else return 0; /* a lost device: the server's wait says so */
                __atomic_add_fetch(&local_wait_wire, 1, __ATOMIC_RELAXED);
                v = __atomic_load_n(&ss->seen, __ATOMIC_ACQUIRE);
            }
            sum += v;
            met += v >= wi->pValues[i];
        }
        if (poll) last_poll = now;
        if (any ? met > 0 : met == wi->semaphoreCount) {
            __atomic_add_fetch(&local_waits, 1, __ATOMIC_RELAXED);
            *r = VK_SUCCESS;
            return 1;
        }
        if (now >= deadline) {
            *r = VK_TIMEOUT;
            return 1;
        }
        if (sum != prev_sum) prev_sum = sum, last_move = now;
        if (now - last_move >= SM_RECHECK_NS) return 0;
        if (spin < 64) sched_yield();
        else {
            uint64_t left = deadline - now;
            struct timespec ts = {0, (long)(left < 50000 ? left : 50000)};
            nanosleep(&ts, NULL);
        }
    }
}

/* A question about these semaphores, answered here or not. */
static void sems_asked(vkb_device *d, const VkSemaphoreWaitInfo *wi, int hit)
{
    for (uint32_t i = 0; i < wi->semaphoreCount; i++) {
        sm_sem *ss = sem_of(d, wi->pSemaphores[i]);
        if (ss) gate_ask(&ss->gate, hit);
    }
}

VkResult vkb_sm_wait_sems(vkb_device *d, const VkSemaphoreWaitInfo *wi, uint64_t timeout)
{
    vkb_sm_dev *sm = d->sm;
    if (sm && !sm->disabled && wi->semaphoreCount) {
        uint64_t now = sm_now();
        uint32_t met = 0, known = 0;
        int recheck = 0, any = (wi->flags & VK_SEMAPHORE_WAIT_ANY_BIT) != 0;
        for (uint32_t i = 0; i < wi->semaphoreCount; i++) {
            uint64_t v;
            if (!sem_local(sm, sem_of(d, wi->pSemaphores[i]), now, &v, &recheck)) break;
            known++;
            met += v >= wi->pValues[i];
        }
        if (known == wi->semaphoreCount) {
            if (any ? met > 0 : met == known) {
                __atomic_add_fetch(&sm->local_sem, 1, __ATOMIC_RELAXED);
                sems_asked(d, wi, 1);
                return VK_SUCCESS;
            }
            if (!timeout && !recheck) {
                __atomic_add_fetch(&sm->local_sem, 1, __ATOMIC_RELAXED);
                sems_asked(d, wi, 1);
                return VK_TIMEOUT;
            }
            VkResult lr;
            if (timeout && !recheck && sem_wait_local_on() && sem_wait_local(d, wi, timeout, &lr)) return lr;
            sems_asked(d, wi, recheck); /* a wait not met here goes to the server: not answered by the mirror */
        }
        __atomic_add_fetch(&sm->wire_sem, 1, __ATOMIC_RELAXED);
        VkResult r = vkb_wire_vkWaitSemaphores((VkDevice)d, wi, timeout);
        for (uint32_t i = 0; i < wi->semaphoreCount; i++) {
            sm_sem *ss = sem_of(d, wi->pSemaphores[i]);
            if (!ss) continue;
            __atomic_store_n(&ss->wire_ns, now, __ATOMIC_RELAXED);
            if (r == VK_SUCCESS && (!any || wi->semaphoreCount == 1)) sem_raise(ss, wi->pValues[i], now);
        }
        return r;
    }
    return vkb_wire_vkWaitSemaphores((VkDevice)d, wi, timeout);
}

/* 1: signalled, 0: not, -1: ask the server. *recheck: a not-ready answer that is due to be asked of the server. */
static int fence_local(vkb_sm_dev *sm, sm_fence *f, uint64_t now, int *recheck)
{
    int st = __atomic_load_n(&f->state, __ATOMIC_ACQUIRE);
    if (st == F_SIGNALED) return 1;
    if (st == F_PENDING && __atomic_load_n(&sm->slots[f->slot], __ATOMIC_ACQUIRE)) {
        __atomic_compare_exchange_n(&f->state, &(uint8_t){F_PENDING}, F_SIGNALED, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
        return 1;
    }
    if (st != F_PENDING && st != F_UNSIGNALED) return -1;
    if (now - __atomic_load_n(&f->wire_ns, __ATOMIC_RELAXED) >= SM_RECHECK_NS) *recheck = 1;
    return 0;
}

static void fence_wire_said(sm_fence *f, VkResult r, uint64_t now)
{
    __atomic_store_n(&f->wire_ns, now, __ATOMIC_RELAXED);
    if (r == VK_SUCCESS) {
        uint8_t st = __atomic_load_n(&f->state, __ATOMIC_ACQUIRE);
        if (st == F_PENDING || st == F_UNSIGNALED || st == F_UNKNOWN)
            __atomic_compare_exchange_n(&f->state, &st, F_SIGNALED, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    }
}

/* 1 with *out set when answered here. */
int vkb_sm_fence_status(vkb_device *d, VkFence fence, VkResult *out)
{
    sm_fence *f = fence_of(d, fence);
    if (!f) return 0;
    vkb_sm_dev *sm = d->sm;
    uint64_t now = sm_now();
    int recheck = 0, l = fence_local(sm, f, now, &recheck);
    if (l >= 0) gate_ask(&f->gate, 1); /* answered here (or its 50 ms check) */
    if (l >= 0 && !recheck) {
        __atomic_add_fetch(&sm->local_fence, 1, __ATOMIC_RELAXED);
        *out = l ? VK_SUCCESS : VK_NOT_READY;
        return 1;
    }
    __atomic_add_fetch(&sm->wire_fence, 1, __ATOMIC_RELAXED);
    *out = vkb_wire_vkGetFenceStatus((VkDevice)d, fence);
    fence_wire_said(f, *out, now);
    return 1;
}

/* Answered here when the fences are all known and either satisfy the wait or the wait has no time. */
VkResult vkb_sm_wait_fences(vkb_device *d, uint32_t n, const VkFence *fences, VkBool32 all, uint64_t timeout)
{
    vkb_sm_dev *sm = d->sm;
    if (sm && !sm->disabled && n) {
        uint64_t now = sm_now();
        uint32_t done = 0, known = 0;
        int recheck = 0;
        for (uint32_t i = 0; i < n; i++) {
            sm_fence *f = fence_of(d, fences[i]);
            int l = f ? fence_local(sm, f, now, &recheck) : -1;
            if (l < 0) break;
            known++;
            done += l;
        }
        if (known == n) {
            if (all ? done == n : done > 0) {
                __atomic_add_fetch(&sm->local_fence, 1, __ATOMIC_RELAXED);
                for (uint32_t i = 0; i < n; i++) gate_ask(&fence_of(d, fences[i])->gate, 1);
                return VK_SUCCESS;
            }
            if (!timeout && !recheck) {
                __atomic_add_fetch(&sm->local_fence, 1, __ATOMIC_RELAXED);
                for (uint32_t i = 0; i < n; i++) gate_ask(&fence_of(d, fences[i])->gate, 1);
                return VK_TIMEOUT;
            }
            for (uint32_t i = 0; i < n; i++) gate_ask(&fence_of(d, fences[i])->gate, recheck); /* to the server: not answered here */
        }
        __atomic_add_fetch(&sm->wire_fence, 1, __ATOMIC_RELAXED);
        VkResult r = vkb_wire_vkWaitForFences((VkDevice)d, n, fences, all, timeout);
        if (r == VK_SUCCESS && (all || n == 1))
            for (uint32_t i = 0; i < n; i++) {
                sm_fence *f = fence_of(d, fences[i]);
                if (f) fence_wire_said(f, r, now);
            }
        else
            for (uint32_t i = 0; i < n; i++) {
                sm_fence *f = fence_of(d, fences[i]);
                if (f) __atomic_store_n(&f->wire_ns, now, __ATOMIC_RELAXED);
            }
        return r;
    }
    return vkb_wire_vkWaitForFences((VkDevice)d, n, fences, all, timeout);
}

/* ------------------------------------------------------------------ events */

void vkb_sm_event_created(vkb_device *d, const VkEventCreateInfo *ci, VkEvent ev)
{
    vkb_sm_dev *sm = d->sm;
    if (!sm || sm->disabled || !ev || (ci->flags & VK_EVENT_CREATE_DEVICE_ONLY_BIT)) return;
    pthread_mutex_lock(&sm->lock);
    uint32_t slot = sm_ready_locked(d, sm) ? slot_take(sm) : UINT32_MAX;
    pthread_mutex_unlock(&sm->lock);
    if (slot == UINT32_MAX) return;
    sm_event *e = calloc(1, sizeof(*e));
    if (!e) {
        pthread_mutex_lock(&sm->lock);
        slot_give(sm, slot);
        pthread_mutex_unlock(&sm->lock);
        return;
    }
    e->slot = slot;
    e->mirrored = 1;
    e->wire_ns = sm_now();
    __atomic_store_n(&sm->slots[slot], 0, __ATOMIC_RELEASE); /* created reset */
    vkb_map_put(&sm->events, (uint64_t)ev, e);
}

/* The spec has every command buffer that uses it completed: its fills are done. */
void vkb_sm_event_destroyed(vkb_device *d, VkEvent ev)
{
    vkb_sm_dev *sm = d->sm;
    if (!sm || !ev) return;
    sm_event *e = vkb_map_remove(&sm->events, (uint64_t)ev);
    if (!e) return;
    pthread_mutex_lock(&sm->lock);
    slot_give(sm, e->slot);
    pthread_mutex_unlock(&sm->lock);
    free(e);
}

static sm_event *event_of(vkb_device *d, VkEvent ev)
{
    return d->sm && ev ? vkb_map_get(&d->sm->events, (uint64_t)ev) : NULL;
}

void vkb_sm_event_host(vkb_device *d, VkEvent ev, uint32_t set)
{
    sm_event *e = event_of(d, ev);
    if (!e) return;
    __atomic_store_n(&d->sm->slots[e->slot], set, __ATOMIC_RELEASE);
    __atomic_store_n(&e->wire_ns, sm_now(), __ATOMIC_RELAXED);
    __atomic_store_n(&e->mirrored, 1, __ATOMIC_RELEASE);
}

int vkb_sm_event_status(vkb_device *d, VkEvent ev, VkResult *out)
{
    sm_event *e = event_of(d, ev);
    if (!e || !__atomic_load_n(&e->mirrored, __ATOMIC_ACQUIRE)) return 0;
    vkb_sm_dev *sm = d->sm;
    if (__atomic_load_n(&sm->slots[e->slot], __ATOMIC_ACQUIRE)) {
        __atomic_add_fetch(&sm->local_event, 1, __ATOMIC_RELAXED);
        *out = VK_EVENT_SET;
        return 1;
    }
    uint64_t now = sm_now();
    if (now - __atomic_load_n(&e->wire_ns, __ATOMIC_RELAXED) < SM_RECHECK_NS) {
        __atomic_add_fetch(&sm->local_event, 1, __ATOMIC_RELAXED);
        *out = VK_EVENT_RESET;
        return 1;
    }
    __atomic_store_n(&e->wire_ns, now, __ATOMIC_RELAXED);
    __atomic_add_fetch(&sm->wire_event, 1, __ATOMIC_RELAXED);
    *out = vkb_wire_vkGetEventStatus((VkDevice)d, ev);
    return 1;
}

/* Recording: the event's state at the end of the command buffer is what its fill writes. */
void vkb_sm_cb_event(vkb_cmdbuf *cb, VkEvent ev, uint32_t set)
{
    if (!cb->recording || !event_of(cb->device, ev)) return;
    if (cb->nevops == cb->evops_cap) {
        uint32_t cap = cb->evops_cap ? cb->evops_cap * 2 : 16;
        vkb_evop *n = realloc(cb->evops, cap * sizeof(*n));
        if (!n) {
            /* Cannot follow it: the event goes to the server until a host set or reset. */
            sm_event *e = event_of(cb->device, ev);
            if (e) __atomic_store_n(&e->mirrored, 0, __ATOMIC_RELEASE);
            return;
        }
        cb->evops = n;
        cb->evops_cap = cap;
    }
    cb->evops[cb->nevops++] = (vkb_evop){ev, set};
}

void vkb_sm_cb_clear(vkb_cmdbuf *cb)
{
    cb->nevops = 0;
    if (cb->evops_cap > 1024) {
        free(cb->evops);
        cb->evops = NULL;
        cb->evops_cap = 0;
    }
}

void vkb_sm_cb_execute(vkb_cmdbuf *primary, const vkb_cmdbuf *secondary)
{
    for (uint32_t i = 0; i < secondary->nevops; i++) vkb_sm_cb_event(primary, secondary->evops[i].ev, secondary->evops[i].val);
}

/* Called by vkEndCommandBuffer while the command buffer still records (after the query mirror's commands). */
void vkb_sm_cb_end(vkb_cmdbuf *cb)
{
    if (!cb->nevops || cb->level != VK_COMMAND_BUFFER_LEVEL_PRIMARY) return;
    vkb_device *d = cb->device;
    vkb_sm_dev *sm = d->sm;
    VkCommandBuffer commandBuffer = (VkCommandBuffer)cb;
    int can = sm && sm->ready && fam_can_fill(sm, cb->family) && !cb->broken && !cb->stream.oom;
    /* Last state of each event wins: walk backwards, skipping events already taken. */
    uint32_t nk = 0;
    for (uint32_t i = cb->nevops; i-- > 0;) {
        vkb_evop *o = &cb->evops[i];
        int seen = 0;
        for (uint32_t j = i + 1; j < cb->nevops && !seen; j++) seen = cb->evops[j].ev == o->ev && cb->evops[j].val != UINT32_MAX;
        if (seen) {
            o->val = UINT32_MAX; /* taken by a later op */
            continue;
        }
        sm_event *e = event_of(d, o->ev);
        if (!e) {
            o->val = UINT32_MAX;
            continue;
        }
        if (!can) {
            __atomic_store_n(&e->mirrored, 0, __ATOMIC_RELEASE);
            o->val = UINT32_MAX;
            continue;
        }
        nk++;
    }
    if (nk) {
        VkMemoryBarrier all = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT};
        vkb_wire_vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &all, 0, NULL,
                                      0, NULL);
        for (uint32_t i = 0; i < cb->nevops; i++) {
            if (cb->evops[i].val == UINT32_MAX) continue;
            sm_event *e = event_of(d, cb->evops[i].ev);
            if (e) vkb_wire_vkCmdFillBuffer(commandBuffer, sm->buf, (VkDeviceSize)e->slot * 4, 4, cb->evops[i].val);
        }
        VkMemoryBarrier host = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
        vkb_wire_vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, NULL, 0,
                                      NULL);
    }
    cb->nevops = 0;
}

/* ------------------------------------------------------------------ entry points */

VKAPI_ATTR VkResult VKAPI_CALL vkb_ep_vkCreateFence(VkDevice device, const VkFenceCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator,
                                                    VkFence *pFence)
{
    VkResult r = vkb_wire_vkCreateFence(device, pCreateInfo, pAllocator, pFence);
    if (r == VK_SUCCESS) vkb_sm_fence_created(vkb_dev(device), pCreateInfo, *pFence);
    return r;
}

VKAPI_ATTR void VKAPI_CALL vkb_ep_vkDestroyFence(VkDevice device, VkFence fence, const VkAllocationCallbacks *pAllocator)
{
    if (!fence) return;
    vkb_wire_vkDestroyFence(device, fence, pAllocator);
    vkb_sm_fence_destroyed(vkb_dev(device), fence);
}

VKAPI_ATTR VkResult VKAPI_CALL vkb_ep_vkResetFences(VkDevice device, uint32_t fenceCount, const VkFence *pFences)
{
    vkb_sm_fences_reset(vkb_dev(device), fenceCount, pFences);
    VkResult r = vkb_wire_vkResetFences(device, fenceCount, pFences);
    for (uint32_t i = 0; i < fenceCount; i++) vkb_hostop_note((uint64_t)(uintptr_t)pFences[i] * 2); /* icd.c */
    return r;
}

/* A sparse bind's fence and semaphores are not mirrored. */
VKAPI_ATTR VkResult VKAPI_CALL vkb_ep_vkQueueBindSparse(VkQueue queue, uint32_t bindInfoCount, const VkBindSparseInfo *pBindInfo, VkFence fence)
{
    vkb_device *d = ((vkb_queue *)queue)->device;
    if (fence) vkb_sm_fence_unknown(d, fence);
    for (uint32_t i = 0; i < bindInfoCount; i++)
        for (uint32_t j = 0; j < pBindInfo[i].signalSemaphoreCount; j++) vkb_sm_sem_unknown(d, pBindInfo[i].pSignalSemaphores[j]);
    return vkb_wire_vkQueueBindSparse(queue, bindInfoCount, pBindInfo, fence);
}

VKAPI_ATTR VkResult VKAPI_CALL vkb_ep_vkCreateEvent(VkDevice device, const VkEventCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator,
                                                    VkEvent *pEvent)
{
    VkResult r = vkb_wire_vkCreateEvent(device, pCreateInfo, pAllocator, pEvent);
    if (r == VK_SUCCESS) vkb_sm_event_created(vkb_dev(device), pCreateInfo, *pEvent);
    return r;
}

VKAPI_ATTR void VKAPI_CALL vkb_ep_vkDestroyEvent(VkDevice device, VkEvent event, const VkAllocationCallbacks *pAllocator)
{
    if (!event) return;
    vkb_wire_vkDestroyEvent(device, event, pAllocator);
    vkb_sm_event_destroyed(vkb_dev(device), event);
}

VKAPI_ATTR VkResult VKAPI_CALL vkb_ep_vkSetEvent(VkDevice device, VkEvent event)
{
    /* Sent first, then the windows end: a poll from another thread that starts after this is
     * ordered after the set on the server, one already answered is not trusted any more. */
    vkb_sm_event_host(vkb_dev(device), event, 1);
    VkResult r = vkb_wire_vkSetEvent(device, event);
    vkb_event_hostop(event);
    vkb_quiet_over();
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL vkb_ep_vkResetEvent(VkDevice device, VkEvent event)
{
    /* No vkb_quiet_over: a reset cannot make a "reset" answer wrong (DXVK resets its events from
     * the host each time it reuses one). */
    vkb_sm_event_host(vkb_dev(device), event, 0);
    VkResult r = vkb_wire_vkResetEvent(device, event);
    vkb_event_hostop(event);
    return r;
}

VKAPI_ATTR void VKAPI_CALL vkb_ep_vkCmdSetEvent(VkCommandBuffer commandBuffer, VkEvent event, VkPipelineStageFlags stageMask)
{
    vkb_wire_vkCmdSetEvent(commandBuffer, event, stageMask);
    vkb_sm_cb_event(vkb_cb(commandBuffer), event, 1);
}

VKAPI_ATTR void VKAPI_CALL vkb_ep_vkCmdResetEvent(VkCommandBuffer commandBuffer, VkEvent event, VkPipelineStageFlags stageMask)
{
    vkb_wire_vkCmdResetEvent(commandBuffer, event, stageMask);
    vkb_sm_cb_event(vkb_cb(commandBuffer), event, 0);
}

VKAPI_ATTR void VKAPI_CALL vkb_ep_vkCmdSetEvent2(VkCommandBuffer commandBuffer, VkEvent event, const VkDependencyInfo *pDependencyInfo)
{
    vkb_wire_vkCmdSetEvent2(commandBuffer, event, pDependencyInfo);
    vkb_sm_cb_event(vkb_cb(commandBuffer), event, 1);
}

VKAPI_ATTR void VKAPI_CALL vkb_ep_vkCmdResetEvent2(VkCommandBuffer commandBuffer, VkEvent event, VkPipelineStageFlags2 stageMask)
{
    vkb_wire_vkCmdResetEvent2(commandBuffer, event, stageMask);
    vkb_sm_cb_event(vkb_cb(commandBuffer), event, 0);
}

VKAPI_ATTR VkResult VKAPI_CALL vkb_ep_vkCreateSemaphore(VkDevice device, const VkSemaphoreCreateInfo *pCreateInfo,
                                                        const VkAllocationCallbacks *pAllocator, VkSemaphore *pSemaphore)
{
    VkResult r = vkb_wire_vkCreateSemaphore(device, pCreateInfo, pAllocator, pSemaphore);
    if (r == VK_SUCCESS) vkb_sm_sem_created(vkb_dev(device), pCreateInfo, *pSemaphore);
    return r;
}

VKAPI_ATTR void VKAPI_CALL vkb_ep_vkDestroySemaphore(VkDevice device, VkSemaphore semaphore, const VkAllocationCallbacks *pAllocator)
{
    if (!semaphore) return;
    vkb_sm_sem_settle(vkb_dev(device), semaphore);
    vkb_wire_vkDestroySemaphore(device, semaphore, pAllocator);
    vkb_sm_sem_destroyed(vkb_dev(device), semaphore);
}

VKAPI_ATTR VkResult VKAPI_CALL vkb_ep_vkSignalSemaphore(VkDevice device, const VkSemaphoreSignalInfo *pSignalInfo)
{
    VkResult r = vkb_wire_vkSignalSemaphore(device, pSignalInfo);
    if (r == VK_SUCCESS) {
        vkb_sm_sem_host_signal(vkb_dev(device), pSignalInfo->semaphore, pSignalInfo->value);
        vkb_quiet_over();
    }
    return r;
}
