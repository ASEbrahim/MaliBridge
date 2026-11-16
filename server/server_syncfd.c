/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim
 *
 * Sync-fd exports with a deadline. vkGetSemaphoreFdKHR and vkGetFenceFdKHR with the SYNC_FD
 * handle type need the payload's signal handed to the kernel: a driver that holds a batch in
 * user space until its waits are met (Mesa's RADV with a submit thread on the host, and, it seems,
 * Samsung's Xclipse driver) answers the export only when that batch goes out. Where that batch
 * waits on something only the caller would do next, the export never returns. a player log (Galaxy S24+,
 * Xclipse 940): Xwayland sat in vkGetSemaphoreFdKHR for 15 minutes, the X server answered no one,
 * Steam's update window stopped after its first frame and the update with it.
 *
 * Here the driver's call runs on a thread of its own. If it has not returned within
 * VKBRIDGE_SYNCFD_EXPORT_MS (default 1000 on Samsung's driver, off elsewhere; 0 off, any other
 * value on for every driver), the app gets VK_SUCCESS and fd -1, which the spec lets a sync-fd
 * export give for a payload already signalled: the caller goes on without waiting for that GPU
 * work (at worst a frame shown before it is drawn) instead of stopping for good. The driver's call
 * goes on; when it returns, its fd is closed and the time logged. Until then a vkDestroySemaphore
 * or vkDestroyFence of that handle is held back and done after it, and the device's end waits for
 * it (or, if it never returns, leaves the device to the process's end rather than destroy it
 * under a driver call). The queue's later work is not touched.
 *
 * The request-order wait too (VKBRIDGE_SYNCFD_ORDER_MS, default 1000 on Samsung's driver, off
 * elsewhere; 0 off, any other value on for every driver): a sync-fd export waits at most that long
 * for the process's earlier asynchronous requests (server_conn.c); past it, a sync-fd export is
 * answered with fd -1 without the driver's call, an opaque one waits on as before. The client sends
 * its queued requests ahead of such an export on its flusher's connection, not the export's own
 * (VKB_KNOB_SYNCFD_SIDE_SHIFT, client/conn.c), so the export is not read only after them.
 * a player log (Galaxy S24+, v0.5.4.2): Xwayland again in vkGetSemaphoreFdKHR for 2 hours with the
 * export bound on, and no line from the export or the order wait.
 * Over every layer but the session report's.
 */
#define _GNU_SOURCE
#include "vkb_server.h"
#include "server_emu.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define SX_SAMSUNG_DEFAULT_MS 1000
#define SX_ORDER_SAMSUNG_DEFAULT_MS 1000
#define SX_DEVICE_END_WAIT_MS 2000

typedef struct sx_job {
    int fence;                         /* 1: vkGetFenceFdKHR, 0: vkGetSemaphoreFdKHR */
    PFN_vkGetSemaphoreFdKHR sem_fd;
    PFN_vkGetFenceFdKHR fence_fd;
    VkDevice dev;
    uint32_t table;
    uint64_t handle;
    VkSemaphoreGetFdInfoKHR si;
    VkFenceGetFdInfoKHR fi;
    VkResult r;
    int fd;
    int done, abandoned;
    int finished;                      /* abandoned: everything after the driver's call done too (the device may go) */
    vkb_srv_table *t;                  /* the call's table, current on the worker (the layers beneath find their state by it) */
    uint64_t t0;
    pthread_cond_t cv;
    struct sx_job *next;               /* abandoned ones (sx_stuck) */
} sx_job;

/* A destroy held back until the exports blocked on its handle return. */
typedef struct sx_destroy {
    uint32_t table;
    uint64_t handle;
    int fence;
    VkDevice dev;
    PFN_vkDestroySemaphore sem_destroy;
    PFN_vkDestroyFence fence_destroy;
    struct sx_destroy *next;
} sx_destroy;

static pthread_mutex_t sx_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t sx_gone = PTHREAD_COND_INITIALIZER; /* an abandoned job finished */
static sx_job *sx_stuck;
static sx_destroy *sx_held;
static uint64_t sx_timeouts, sx_late;

static int sx_ms[VKB_MAX_TABLES]; /* per device: the deadline, 0 off */
static int so_ms[VKB_MAX_TABLES]; /* per device: the order wait's deadline for an export, 0 off */
static uint64_t so_skipped;
static PFN_vkGetSemaphoreFdKHR next_sem_fd[VKB_MAX_TABLES];
static PFN_vkGetFenceFdKHR next_fence_fd[VKB_MAX_TABLES];
static PFN_vkDestroySemaphore next_sem_destroy[VKB_MAX_TABLES];
static PFN_vkDestroyFence next_fence_destroy[VKB_MAX_TABLES];

static uint64_t sx_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* Under sx_lock: an abandoned export of that handle still in the driver. */
static int sx_busy(uint32_t table, uint64_t handle)
{
    for (sx_job *j = sx_stuck; j; j = j->next)
        if (j->table == table && j->handle == handle && !j->done) return 1;
    return 0;
}

static void *sx_worker(void *arg)
{
    sx_job *j = arg;
    vkb_srv_set_current_table(j->t);
    VkResult r;
    int fd = -1;
    if (j->fence) r = j->fence_fd(j->dev, &j->fi, &fd);
    else r = j->sem_fd(j->dev, &j->si, &fd);
    pthread_mutex_lock(&sx_lock);
    j->r = r;
    j->fd = fd;
    j->done = 1;
    if (!j->abandoned) {
        pthread_cond_signal(&j->cv);
        pthread_mutex_unlock(&sx_lock);
        return NULL; /* the caller frees it */
    }
    /* Abandoned: the app already has its answer. A destroy held back for the handle is done here
     * once no other export of it is in the driver; the job stays listed (not finished) until then,
     * so the device's end waits for that destroy too. */
    sx_destroy *d = NULL;
    if (!sx_busy(j->table, j->handle))
        for (sx_destroy **pp = &sx_held; *pp; pp = &(*pp)->next)
            if ((*pp)->table == j->table && (*pp)->handle == j->handle) {
                d = *pp;
                *pp = d->next;
                break;
            }
    uint64_t n = ++sx_late;
    pthread_mutex_unlock(&sx_lock);
    if (fd >= 0) close(fd);
    if (n <= 8 || (n & (n - 1)) == 0)
        VKB_INFO("sync-fd export: the driver's %s returned after %.1f s (%d; %llu so far), its fd closed%s",
                 j->fence ? "vkGetFenceFdKHR" : "vkGetSemaphoreFdKHR", (sx_now() - j->t0) / 1e9, r, (unsigned long long)n,
                 d ? "; the held-back destroy done" : "");
    if (d) {
        if (d->fence) d->fence_destroy(d->dev, (VkFence)(uintptr_t)d->handle, NULL);
        else d->sem_destroy(d->dev, (VkSemaphore)(uintptr_t)d->handle, NULL);
        free(d);
    }
    pthread_mutex_lock(&sx_lock);
    for (sx_job **pp = &sx_stuck; *pp; pp = &(*pp)->next)
        if (*pp == j) {
            *pp = j->next;
            break;
        }
    pthread_cond_broadcast(&sx_gone);
    pthread_mutex_unlock(&sx_lock);
    pthread_cond_destroy(&j->cv);
    free(j);
    return NULL;
}

/* The driver's call on a thread of its own, waited for at most `ms`. */
static VkResult sx_run(sx_job *j, int ms, int *pFd)
{
    pthread_cond_init(&j->cv, NULL);
    j->t0 = sx_now();
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, 256u << 10);
    pthread_t th;
    pthread_mutex_lock(&sx_lock);
    if (pthread_create(&th, &attr, sx_worker, j) != 0) {
        pthread_mutex_unlock(&sx_lock);
        pthread_attr_destroy(&attr);
        pthread_cond_destroy(&j->cv);
        VkResult r = j->fence ? j->fence_fd(j->dev, &j->fi, pFd) : j->sem_fd(j->dev, &j->si, pFd);
        free(j);
        return r;
    }
    pthread_attr_destroy(&attr);
    struct timespec until;
    clock_gettime(CLOCK_MONOTONIC, &until);
    until.tv_sec += ms / 1000;
    until.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (until.tv_nsec >= 1000000000L) until.tv_sec++, until.tv_nsec -= 1000000000L;
    while (!j->done) {
        /* the condition variable's clock is CLOCK_REALTIME: wait in short steps against CLOCK_MONOTONIC */
        struct timespec now, step;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec > until.tv_sec || (now.tv_sec == until.tv_sec && now.tv_nsec >= until.tv_nsec)) break;
        clock_gettime(CLOCK_REALTIME, &step);
        step.tv_nsec += 20000000L;
        if (step.tv_nsec >= 1000000000L) step.tv_sec++, step.tv_nsec -= 1000000000L;
        pthread_cond_timedwait(&j->cv, &sx_lock, &step);
    }
    if (j->done) {
        VkResult r = j->r;
        *pFd = j->fd;
        pthread_mutex_unlock(&sx_lock);
        pthread_cond_destroy(&j->cv);
        free(j);
        return r;
    }
    j->abandoned = 1;
    j->next = sx_stuck;
    sx_stuck = j;
    uint64_t n = ++sx_timeouts;
    int fence = j->fence; /* j is the worker's from here on */
    pthread_mutex_unlock(&sx_lock);
    vkb_srv_proc *p = vkb_srv_current_proc();
    if (n <= 8 || (n & (n - 1)) == 0)
        VKB_WARN("sync-fd export: the driver's %s has not returned in %d ms (pid %d %s); answered with fd -1 "
                 "(already signalled), the call left to finish on its own (%llu so far; "
                 "VKBRIDGE_SYNCFD_EXPORT_MS)",
                 fence ? "vkGetFenceFdKHR" : "vkGetSemaphoreFdKHR", ms, p ? p->pid : 0, p ? p->comm : "?", (unsigned long long)n);
    *pFd = -1;
    return VK_SUCCESS;
}

/* VKBRIDGE_SYNCFD_ORDER_MS for a device of that driver: 0 off. */
static int so_setting(uint32_t driver_id, int *from_env)
{
    const char *v = vkb_srv_env("VKBRIDGE_SYNCFD_ORDER_MS");
    *from_env = v && *v;
    int ms = *from_env ? atoi(v) : (driver_id == VK_DRIVER_ID_SAMSUNG_PROPRIETARY ? SX_ORDER_SAMSUNG_DEFAULT_MS : 0);
    return ms < 0 ? 0 : ms;
}

int vkb_syncfd_order_ms(uint32_t table, uint32_t cmd)
{
    if (cmd != VKB_CMD_vkGetSemaphoreFdKHR && cmd != VKB_CMD_vkGetFenceFdKHR) return 0;
    if (!table || table >= VKB_MAX_TABLES) return 0;
    return __atomic_load_n(&so_ms[table], __ATOMIC_RELAXED);
}

uint32_t vkb_syncfd_client_knobs(uint32_t driver_id)
{
    int from_env;
    int ms = so_setting(driver_id, &from_env);
    if (!ms && !from_env) return 0;
    return (ms ? VKB_KNOB_ON : VKB_KNOB_OFF) << VKB_KNOB_SYNCFD_SIDE_SHIFT;
}

/* The order wait ran out for this export (server_conn.c): a sync-fd one is answered with fd -1, the
 * driver not called (its signal may not be in the driver yet); anything else waits on. 1: answered. */
static int so_answer(int sync_fd, const char *what, int *pFd)
{
    if (!vkb_srv_order_skipped()) return 0;
    if (!sync_fd) {
        vkb_srv_order_finish();
        return 0;
    }
    vkb_srv_order_done();
    uint64_t n = __atomic_add_fetch(&so_skipped, 1, __ATOMIC_RELAXED);
    vkb_srv_proc *p = vkb_srv_current_proc();
    if (n <= 8 || (n & (n - 1)) == 0)
        VKB_WARN("sync-fd export: %s answered with fd -1 after the order wait's deadline, the driver not called (pid %d %s; "
                 "%llu so far; VKBRIDGE_SYNCFD_ORDER_MS)",
                 what, p ? p->pid : 0, p ? p->comm : "?", (unsigned long long)n);
    *pFd = -1;
    return 1;
}

static VKAPI_ATTR VkResult VKAPI_CALL sx_GetSemaphoreFdKHR(VkDevice d, const VkSemaphoreGetFdInfoKHR *gi, int *pFd)
{
    vkb_srv_table *t = vkb_srv_current_table();
    uint32_t id = t ? t->id : 0;
    PFN_vkGetSemaphoreFdKHR next = t ? next_sem_fd[id] : NULL;
    if (!next) return VK_ERROR_DEVICE_LOST;
    if (so_answer(gi && gi->handleType == VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT, "vkGetSemaphoreFdKHR", pFd)) return VK_SUCCESS;
    sx_job *j;
    if (!gi || gi->handleType != VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT || !sx_ms[id] || !(j = calloc(1, sizeof(*j))))
        return next(d, gi, pFd);
    j->sem_fd = next;
    j->t = t;
    j->dev = d;
    j->table = id;
    j->handle = (uint64_t)(uintptr_t)gi->semaphore;
    j->si = *gi;
    j->si.pNext = NULL; /* no extension structs are defined for it; the app's chain is not ours to keep */
    return sx_run(j, sx_ms[id], pFd);
}

static VKAPI_ATTR VkResult VKAPI_CALL sx_GetFenceFdKHR(VkDevice d, const VkFenceGetFdInfoKHR *gi, int *pFd)
{
    vkb_srv_table *t = vkb_srv_current_table();
    uint32_t id = t ? t->id : 0;
    PFN_vkGetFenceFdKHR next = t ? next_fence_fd[id] : NULL;
    if (!next) return VK_ERROR_DEVICE_LOST;
    if (so_answer(gi && gi->handleType == VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT, "vkGetFenceFdKHR", pFd)) return VK_SUCCESS;
    sx_job *j;
    if (!gi || gi->handleType != VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT || !sx_ms[id] || !(j = calloc(1, sizeof(*j))))
        return next(d, gi, pFd);
    j->fence = 1;
    j->fence_fd = next;
    j->t = t;
    j->dev = d;
    j->table = id;
    j->handle = (uint64_t)(uintptr_t)gi->fence;
    j->fi = *gi;
    j->fi.pNext = NULL;
    return sx_run(j, sx_ms[id], pFd);
}

/* 1 when the destroy was held back. */
static int sx_hold_destroy(uint32_t table, uint64_t handle, int fence, VkDevice d)
{
    if (!handle) return 0;
    pthread_mutex_lock(&sx_lock);
    int busy = sx_busy(table, handle);
    if (busy) {
        sx_destroy *h = calloc(1, sizeof(*h));
        if (h) {
            *h = (sx_destroy){table, handle, fence, d, next_sem_destroy[table], next_fence_destroy[table], sx_held};
            sx_held = h;
        } else {
            busy = 0; /* no memory: destroyed now, as before this layer */
        }
    }
    pthread_mutex_unlock(&sx_lock);
    if (busy) VKB_ONCE(VKB_LOG_INFO, "sync-fd export: a destroy of a handle whose export is still in the driver held back until it returns");
    return busy;
}

static VKAPI_ATTR void VKAPI_CALL sx_DestroySemaphore(VkDevice d, VkSemaphore s, const VkAllocationCallbacks *a)
{
    vkb_srv_table *t = vkb_srv_current_table();
    uint32_t id = t ? t->id : 0;
    if (!t || !next_sem_destroy[id]) return;
    if (sx_ms[id] && sx_hold_destroy(id, (uint64_t)(uintptr_t)s, 0, d)) return;
    next_sem_destroy[id](d, s, a);
}

static VKAPI_ATTR void VKAPI_CALL sx_DestroyFence(VkDevice d, VkFence f, const VkAllocationCallbacks *a)
{
    vkb_srv_table *t = vkb_srv_current_table();
    uint32_t id = t ? t->id : 0;
    if (!t || !next_fence_destroy[id]) return;
    if (sx_ms[id] && sx_hold_destroy(id, (uint64_t)(uintptr_t)f, 1, d)) return;
    next_fence_destroy[id](d, f, a);
}

void vkb_syncfd_device_init(vkb_srv_table *t)
{
    if (!t || t->id >= VKB_MAX_TABLES) return;
    const char *v = vkb_srv_env("VKBRIDGE_SYNCFD_EXPORT_MS");
    uint32_t drv = t->pd ? t->pd->info.driver_id : 0;
    int ms = v && *v ? atoi(v) : (drv == VK_DRIVER_ID_SAMSUNG_PROPRIETARY ? SX_SAMSUNG_DEFAULT_MS : 0);
    int oenv, oms = so_setting(drv, &oenv);
    if (ms < 0) ms = 0;
    sx_ms[t->id] = 0;
    so_ms[t->id] = 0;
    if ((!ms && !oms) || !t->dt.vkDestroySemaphore || !t->dt.vkDestroyFence) return;
    sx_ms[t->id] = ms;
    next_sem_fd[t->id] = t->dt.vkGetSemaphoreFdKHR;
    next_fence_fd[t->id] = t->dt.vkGetFenceFdKHR;
    next_sem_destroy[t->id] = t->dt.vkDestroySemaphore;
    next_fence_destroy[t->id] = t->dt.vkDestroyFence;
    if (t->dt.vkGetSemaphoreFdKHR) t->dt.vkGetSemaphoreFdKHR = sx_GetSemaphoreFdKHR;
    if (t->dt.vkGetFenceFdKHR) t->dt.vkGetFenceFdKHR = sx_GetFenceFdKHR;
    t->dt.vkDestroySemaphore = sx_DestroySemaphore;
    t->dt.vkDestroyFence = sx_DestroyFence;
    __atomic_store_n(&so_ms[t->id], oms, __ATOMIC_RELAXED); /* last: the layer is in place before the order wait relies on it */
    if (ms)
        VKB_ONCE(VKB_LOG_INFO, "sync-fd export: answered within %d ms (VKBRIDGE_SYNCFD_EXPORT_MS%s)", ms,
                 v && *v ? "" : "; Samsung's driver's default");
    if (oms)
        VKB_ONCE(VKB_LOG_INFO, "sync-fd export: waits at most %d ms for the process's earlier requests (VKBRIDGE_SYNCFD_ORDER_MS%s)",
                 oms, oenv ? "" : "; Samsung's driver's default");
}

/*
 * Before the device goes: its exports still in the driver are waited for (SX_DEVICE_END_WAIT_MS),
 * then their held-back destroys done. 1 when one is still in the driver: the caller then leaves
 * the device alive (destroying it under the driver's call could take the whole server down).
 */
int vkb_syncfd_device_destroy(vkb_srv_table *t)
{
    if (!t || t->id >= VKB_MAX_TABLES) return 0;
    __atomic_store_n(&so_ms[t->id], 0, __ATOMIC_RELAXED);
    if (!sx_ms[t->id]) return 0;
    uint32_t id = t->id;
    uint64_t until = sx_now() + SX_DEVICE_END_WAIT_MS * 1000000ull;
    pthread_mutex_lock(&sx_lock);
    int busy = 0;
    for (;;) {
        busy = 0;
        for (sx_job *j = sx_stuck; j; j = j->next) busy |= j->table == id; /* listed: not finished */
        if (!busy || sx_now() >= until) break;
        struct timespec step;
        clock_gettime(CLOCK_REALTIME, &step);
        step.tv_nsec += 50000000L;
        if (step.tv_nsec >= 1000000000L) step.tv_sec++, step.tv_nsec -= 1000000000L;
        pthread_cond_timedwait(&sx_gone, &sx_lock, &step);
    }
    /* What is still held for this device: done now if nothing is in the driver, else left. */
    sx_destroy *mine = NULL;
    if (!busy)
        for (sx_destroy **pp = &sx_held; *pp;) {
            if ((*pp)->table == id) {
                sx_destroy *d = *pp;
                *pp = d->next;
                d->next = mine;
                mine = d;
            } else {
                pp = &(*pp)->next;
            }
        }
    sx_ms[id] = 0;
    pthread_mutex_unlock(&sx_lock);
    while (mine) {
        sx_destroy *d = mine;
        mine = d->next;
        if (d->fence) d->fence_destroy(d->dev, (VkFence)(uintptr_t)d->handle, NULL);
        else d->sem_destroy(d->dev, (VkSemaphore)(uintptr_t)d->handle, NULL);
        free(d);
    }
    if (busy)
        VKB_WARN("device %u: a sync-fd export is still in the driver after %d ms; the device is left alive (destroyed with the "
                 "process) rather than destroyed under it",
                 id, SX_DEVICE_END_WAIT_MS);
    return busy;
}

/*
 * vkbAcquireSignal: device, queue, semaphore, fence (u64 each, 0: none). An acquired swapchain image's
 * semaphore and fence signalled (client/wsi.c, VKBRIDGE_ASYNC_ACQUIRE): sent with no reply, in the client's
 * queue of asynchronous requests, so it runs before anything the app sends after its acquire (its submit
 * waiting on the semaphore; a fence wait carries the queue's barrier). The same steps the client took itself
 * with a round trip each: a signalled sync fd (-1) imported as the temporary payload, and where the driver
 * refuses that, an empty submit on the queue signals them. A failure can only be logged here (the app was
 * told VK_SUCCESS, as with the client's own empty submit before). VKBRIDGE_FAKE_MISSING=syncfdimport (host
 * tests): the driver's refusal imitated, so the fallback runs (host/present_path_test.sh).
 */
/*
 * Steam's performance overlay (VKBRIDGE_HUD_FPS, default 0 = off in v0.5.5.4, 12 in 0.5.6; the server's switch: <cache dir>/env;
 * it needs VKBRIDGE_ASYNC_ACQUIRE on).
 * With the overlay shown, gamescope runs mangoapp (MangoHud's app mode), which draws its whole window again
 * for every game frame (its render loop waits on gamescope's per-frame message): 43-56 frames a second next
 * to Stray, ~115 next to Portal on the S11 Ultra, each one a GPU context's submit with the sync mirror's
 * batches, server requests, gamescope's second plane and the app compositor's commit, for numbers that change
 * twice a second (MangoHud's fps_sampling_period, 500 ms). Stray on the Ultra: 50 fps with the overlay shown
 * vs 57 with no mangoapp (2026-11-12). Here an acquire of mangoapp's next image is signalled no
 * sooner than 1/N s after its previous one: its thread waits in this request (mangoapp's own ordered stream,
 * no other process's), its next fence wait or present waits for it, so it draws at most N frames a second.
 * Nothing it shows is lost or wrong: MangoHud takes gamescope's frame times on a thread of its own and each
 * drawing shows the latest (at most 1/N s old). 12 keeps the frame gap (83 ms) under the 100 ms hitch line.
 * Only the process named exactly "mangoapp"; with the overlay hidden or off there is no such process.
 */
static uint64_t hud_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static uint32_t hud_fps(void)
{
    static int v = -1;
    int c = __atomic_load_n(&v, __ATOMIC_RELAXED);
    if (c >= 0) return (uint32_t)c;
    const char *e = vkb_srv_env_global("VKBRIDGE_HUD_FPS");
    long n = 0; /* v0.5.5.4: off by default (the cap rides on the async acquire, also off here); VKBRIDGE_HUD_FPS=12 */
    if (e && *e) {
        char *end;
        n = strtol(e, &end, 10);
        if (*end || n < 0) n = 0;
        if (n > 240) n = 240;
    }
    __atomic_store_n(&v, (int)n, __ATOMIC_RELAXED);
    return (uint32_t)n;
}

static void hud_pace(vkb_srv_proc *p)
{
    if (!p || strcmp(p->comm, "mangoapp") != 0) return;
    uint32_t fps = hud_fps();
    if (!fps) return;
    const uint64_t gap = 1000000000ull / fps;
    uint64_t last = __atomic_load_n(&p->hud_last_ns, __ATOMIC_RELAXED), now = hud_now_ns();
    uint64_t n = __atomic_add_fetch(&p->hud_frames, 1, __ATOMIC_RELAXED);
    if (n == 1)
        VKB_INFO("hud: process %d is Steam's performance overlay (mangoapp): it draws at most %u frames a second "
                 "(VKBRIDGE_HUD_FPS=%u; 0 turns this off)", p->pid, fps, fps);
    if (last && now - last < gap) {
        uint64_t w = gap - (now - last);
        struct timespec ts = {(time_t)(w / 1000000000ull), (long)(w % 1000000000ull)};
        while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {}
        __atomic_add_fetch(&p->hud_paced, 1, __ATOMIC_RELAXED);
        __atomic_add_fetch(&p->hud_paced_ns, w, __ATOMIC_RELAXED);
        now = hud_now_ns();
    }
    __atomic_store_n(&p->hud_last_ns, now, __ATOMIC_RELAXED);
    if (n % 600 == 0) /* about once a minute at 10-12 a second */
        VKB_INFO("hud: mangoapp (process %d): %llu frames, %llu waited for the cap (%.0f ms each on average)", p->pid,
                 (unsigned long long)n, (unsigned long long)__atomic_load_n(&p->hud_paced, __ATOMIC_RELAXED),
                 __atomic_load_n(&p->hud_paced, __ATOMIC_RELAXED)
                     ? __atomic_load_n(&p->hud_paced_ns, __ATOMIC_RELAXED) / 1e6 / (double)__atomic_load_n(&p->hud_paced, __ATOMIC_RELAXED)
                     : 0.0);
}

void vkb_sv_vkbAcquireSignal(vkb_srv_call *c)
{
    VkDevice device = (VkDevice)(uintptr_t)vkb_dec_u64(&c->d);
    VkQueue queue = (VkQueue)(uintptr_t)vkb_dec_u64(&c->d);
    VkSemaphore sem = (VkSemaphore)(uintptr_t)vkb_dec_u64(&c->d);
    VkFence fence = (VkFence)(uintptr_t)vkb_dec_u64(&c->d);
    vkb_srv_table *t = c->table;
    if (c->d.err || !t || !t->is_device || t->device != device || !queue) {
        vkb_srv_bad_message(c);
        return;
    }
    hud_pace(c->conn ? c->conn->proc : NULL);
    const vkb_dispatch *dt = c->dt;
    static int said_sem, said_fence, said_fail, refuse = -1;
    int no_import = __atomic_load_n(&refuse, __ATOMIC_RELAXED);
    if (no_import < 0) {
        no_import = vkb_emu_faked("syncfdimport");
        __atomic_store_n(&refuse, no_import, __ATOMIC_RELAXED);
    }
    VkResult r = VK_SUCCESS;
    if (sem) {
        VkImportSemaphoreFdInfoKHR si = {VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR, NULL, sem, VK_SEMAPHORE_IMPORT_TEMPORARY_BIT,
                                         VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT, -1};
        r = no_import ? VK_ERROR_INVALID_EXTERNAL_HANDLE
            : dt->vkImportSemaphoreFdKHR ? dt->vkImportSemaphoreFdKHR(device, &si) : VK_ERROR_FEATURE_NOT_PRESENT;
        if (r != VK_SUCCESS) {
            if (!__atomic_exchange_n(&said_sem, 1, __ATOMIC_RELAXED))
                VKB_INFO("acquire: the driver took no signalled sync fd for a semaphore (%d); an empty submit signals it", r);
            VkSubmitInfo s = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 0, NULL, 1, &sem};
            r = dt->vkQueueSubmit(queue, 1, &s, fence);
            if (r != VK_SUCCESS && !__atomic_exchange_n(&said_fail, 1, __ATOMIC_RELAXED))
                VKB_ERR("acquire: the empty submit that signals an acquired image's semaphore failed (%d)", r);
            return;
        }
    }
    if (fence) {
        VkImportFenceFdInfoKHR fi = {VK_STRUCTURE_TYPE_IMPORT_FENCE_FD_INFO_KHR, NULL, fence, VK_FENCE_IMPORT_TEMPORARY_BIT,
                                     VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT, -1};
        r = no_import ? VK_ERROR_INVALID_EXTERNAL_HANDLE
            : dt->vkImportFenceFdKHR ? dt->vkImportFenceFdKHR(device, &fi) : VK_ERROR_FEATURE_NOT_PRESENT;
        if (r != VK_SUCCESS) {
            if (!__atomic_exchange_n(&said_fence, 1, __ATOMIC_RELAXED))
                VKB_INFO("acquire: the driver took no signalled sync fd for a fence (%d); an empty submit signals it", r);
            r = dt->vkQueueSubmit(queue, 0, NULL, fence);
            if (r != VK_SUCCESS && !__atomic_exchange_n(&said_fail, 1, __ATOMIC_RELAXED))
                VKB_ERR("acquire: the empty submit that signals an acquired image's fence failed (%d)", r);
        }
    }
}
