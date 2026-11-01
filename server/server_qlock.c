/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim
 *
 * Per-queue locks. Vulkan wants every use of a VkQueue externally synchronized; the bridge gets
 * that today from the protocol's one ordered stream per process, which also makes every queue
 * wait for every other queue's work and the WSI present submit wait for DXVK's. Any relaxation of
 * that ordering needs the queue's own lock first. Installed beneath every layer, so the layers'
 * own submits (polygon edges, BC transcodes, the hang detector's fences) take it too. Submit,
 * Submit2, WaitIdle and BindSparse.
 *
 * A driver may block inside vkQueueSubmit (RADV, on a binary semaphore whose signalling batch is
 * itself held: FIX-ml5n32), and a lock held across that block would also stop the submit that
 * releases it. Two rules keep the lock from ever making a hang of its own:
 *   - vkQueueWaitIdle is not waited out under the lock: an empty submit with a fence takes it,
 *     the wait on that fence does not;
 *   - a submit that cannot get its queue's lock in 2 s goes ahead without it (logged once, with
 *     what holds it and for how long), as it did before the lock existed.
 * One lock per (device, queue), freed with the device: a long-lived server sees hundreds of
 * devices (one per game start), and a lock shared by all queues past a fixed count would have
 * let one game's blocked submit hold up another's.
 */
#include "vkb_server.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <time.h>

#define QL_GIVE_UP_S 2

typedef struct ql_slot {
    uint32_t table;
    VkQueue q;
    pthread_mutex_t m;
    const char *what;        /* what holds it (atomic) */
    uint64_t since_ns;
    struct ql_slot *next;
} ql_slot;

static ql_slot *ql_head;
static pthread_mutex_t ql_lock = PTHREAD_MUTEX_INITIALIZER;

static PFN_vkQueueSubmit next_submit[VKB_MAX_TABLES];
static PFN_vkQueueSubmit2 next_submit2[VKB_MAX_TABLES];
static PFN_vkQueueWaitIdle next_idle[VKB_MAX_TABLES];
static PFN_vkQueueBindSparse next_sparse[VKB_MAX_TABLES];

static uint64_t ql_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* A device has a handful of queues: a list is enough. Slots live until their device is destroyed. */
static ql_slot *ql_of(uint32_t table, VkQueue q)
{
    pthread_mutex_lock(&ql_lock);
    ql_slot *s = ql_head;
    while (s && (s->table != table || s->q != q)) s = s->next;
    if (!s && (s = calloc(1, sizeof(*s)))) {
        s->table = table;
        s->q = q;
        pthread_mutex_init(&s->m, NULL);
        s->next = ql_head;
        ql_head = s;
    }
    pthread_mutex_unlock(&ql_lock);
    return s;
}

/* 1 when locked, 0 when given up on (the call goes ahead without the lock). */
static int ql_take(ql_slot *s, const char *what)
{
    if (!s) return 0;
    if (pthread_mutex_trylock(&s->m) != 0) {
        struct timespec until;
        clock_gettime(CLOCK_REALTIME, &until);
        until.tv_sec += QL_GIVE_UP_S;
        int e;
        while ((e = pthread_mutex_timedlock(&s->m, &until)) == EINTR) {}
        if (e) {
            static int told;
            const char *holder = __atomic_load_n(&s->what, __ATOMIC_ACQUIRE);
            uint64_t since = __atomic_load_n(&s->since_ns, __ATOMIC_RELAXED);
            if (!__atomic_exchange_n(&told, 1, __ATOMIC_RELAXED))
                VKB_WARN("queue lock: a %s waited %d s for its queue (held by a %s for %.1f s, blocked in the driver); going ahead "
                         "without the lock (logged once)",
                         what, QL_GIVE_UP_S, holder ? holder : "?", since ? (ql_now() - since) / 1e9 : 0.0);
            return 0;
        }
    }
    __atomic_store_n(&s->since_ns, ql_now(), __ATOMIC_RELAXED);
    __atomic_store_n(&s->what, what, __ATOMIC_RELEASE);
    return 1;
}

static void ql_give(ql_slot *s, int locked)
{
    if (!locked) return;
    __atomic_store_n(&s->what, NULL, __ATOMIC_RELEASE);
    __atomic_store_n(&s->since_ns, 0, __ATOMIC_RELAXED);
    pthread_mutex_unlock(&s->m);
}

static VKAPI_ATTR VkResult VKAPI_CALL ql_QueueSubmit(VkQueue q, uint32_t n, const VkSubmitInfo *si, VkFence f)
{
    vkb_srv_table *t = vkb_srv_current_table();
    PFN_vkQueueSubmit next = t ? next_submit[t->id] : NULL;
    if (!next) return VK_ERROR_DEVICE_LOST;
    ql_slot *s = ql_of(t->id, q);
    int locked = ql_take(s, "vkQueueSubmit");
    VkResult r = next(q, n, si, f);
    ql_give(s, locked);
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL ql_QueueSubmit2(VkQueue q, uint32_t n, const VkSubmitInfo2 *si, VkFence f)
{
    vkb_srv_table *t = vkb_srv_current_table();
    PFN_vkQueueSubmit2 next = t ? next_submit2[t->id] : NULL;
    if (!next) return VK_ERROR_DEVICE_LOST;
    ql_slot *s = ql_of(t->id, q);
    int locked = ql_take(s, "vkQueueSubmit2");
    VkResult r = next(q, n, si, f);
    ql_give(s, locked);
    return r;
}

/* Not waited out under the lock: the work being waited for may need a later submit on this queue
 * (a semaphore another thread signals), which would then never get the lock. An empty submit
 * with a fence, under the lock, then the wait on the fence: the same "everything submitted
 * before is done". The driver's own call where no fence can be made. */
static VKAPI_ATTR VkResult VKAPI_CALL ql_QueueWaitIdle(VkQueue q)
{
    vkb_srv_table *t = vkb_srv_current_table();
    PFN_vkQueueWaitIdle next = t ? next_idle[t->id] : NULL;
    PFN_vkQueueSubmit submit = t ? next_submit[t->id] : NULL;
    if (!next) return VK_ERROR_DEVICE_LOST;
    ql_slot *s = ql_of(t->id, q);
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence f = VK_NULL_HANDLE;
    if (!submit || !t->real.vkCreateFence || !t->dt.vkWaitForFences || t->real.vkCreateFence(t->device, &fci, NULL, &f) != VK_SUCCESS) {
        int locked = ql_take(s, "vkQueueWaitIdle");
        VkResult r = next(q);
        ql_give(s, locked);
        return r;
    }
    int locked = ql_take(s, "vkQueueWaitIdle's submit");
    VkResult r = submit(q, 0, NULL, f);
    ql_give(s, locked);
    /* through every layer: the Mali drivers' failed waits are retried there (server_wait.c) */
    if (r == VK_SUCCESS) r = t->dt.vkWaitForFences(t->device, 1, &f, VK_TRUE, UINT64_MAX);
    t->real.vkDestroyFence(t->device, f, NULL);
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL ql_QueueBindSparse(VkQueue q, uint32_t n, const VkBindSparseInfo *bi, VkFence f)
{
    vkb_srv_table *t = vkb_srv_current_table();
    PFN_vkQueueBindSparse next = t ? next_sparse[t->id] : NULL;
    if (!next) return VK_ERROR_DEVICE_LOST;
    ql_slot *s = ql_of(t->id, q);
    int locked = ql_take(s, "vkQueueBindSparse");
    VkResult r = next(q, n, bi, f);
    ql_give(s, locked);
    return r;
}

void vkb_qlock_device_init(vkb_srv_table *t)
{
    if (!t || t->id >= VKB_MAX_TABLES || t->dt.vkQueueSubmit != t->real.vkQueueSubmit) return;
    next_submit[t->id] = t->dt.vkQueueSubmit;
    next_submit2[t->id] = t->dt.vkQueueSubmit2;
    next_idle[t->id] = t->dt.vkQueueWaitIdle;
    next_sparse[t->id] = t->dt.vkQueueBindSparse;
    /* t->real too: the layers that sit on the driver calls (the hang detector) build on it */
    if (t->dt.vkQueueSubmit) t->dt.vkQueueSubmit = t->real.vkQueueSubmit = ql_QueueSubmit;
    if (t->dt.vkQueueSubmit2) t->dt.vkQueueSubmit2 = t->real.vkQueueSubmit2 = ql_QueueSubmit2;
    if (t->dt.vkQueueWaitIdle) t->dt.vkQueueWaitIdle = t->real.vkQueueWaitIdle = ql_QueueWaitIdle;
    if (t->dt.vkQueueBindSparse) t->dt.vkQueueBindSparse = t->real.vkQueueBindSparse = ql_QueueBindSparse;
}

/* After the device's last submit (its queues are idle): its locks go. */
void vkb_qlock_device_destroy(vkb_srv_table *t)
{
    if (!t) return;
    pthread_mutex_lock(&ql_lock);
    for (ql_slot **pp = &ql_head; *pp;) {
        ql_slot *s = *pp;
        if (s->table == t->id) {
            *pp = s->next;
            pthread_mutex_destroy(&s->m);
            free(s);
        } else {
            pp = &s->next;
        }
    }
    pthread_mutex_unlock(&ql_lock);
}
