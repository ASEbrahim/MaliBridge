/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim
 *
 * Sync-fd exports with a deadline: past it the app gets VK_SUCCESS and fd -1, and destroys of the handle wait for the driver's call.
 */
#define _GNU_SOURCE
#include "vkb_server.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define SX_SAMSUNG_DEFAULT_MS 1000
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
    /* Abandoned: a destroy held back for the handle is done here once no other export of it is in the driver. */
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

static VKAPI_ATTR VkResult VKAPI_CALL sx_GetSemaphoreFdKHR(VkDevice d, const VkSemaphoreGetFdInfoKHR *gi, int *pFd)
{
    vkb_srv_table *t = vkb_srv_current_table();
    uint32_t id = t ? t->id : 0;
    PFN_vkGetSemaphoreFdKHR next = t ? next_sem_fd[id] : NULL;
    if (!next) return VK_ERROR_DEVICE_LOST;
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
    int ms = v && *v ? atoi(v) : (t->pd && t->pd->info.driver_id == VK_DRIVER_ID_SAMSUNG_PROPRIETARY ? SX_SAMSUNG_DEFAULT_MS : 0);
    if (ms < 0) ms = 0;
    sx_ms[t->id] = ms;
    if (!ms || !t->dt.vkDestroySemaphore || !t->dt.vkDestroyFence) {
        sx_ms[t->id] = 0;
        return;
    }
    next_sem_fd[t->id] = t->dt.vkGetSemaphoreFdKHR;
    next_fence_fd[t->id] = t->dt.vkGetFenceFdKHR;
    next_sem_destroy[t->id] = t->dt.vkDestroySemaphore;
    next_fence_destroy[t->id] = t->dt.vkDestroyFence;
    if (t->dt.vkGetSemaphoreFdKHR) t->dt.vkGetSemaphoreFdKHR = sx_GetSemaphoreFdKHR;
    if (t->dt.vkGetFenceFdKHR) t->dt.vkGetFenceFdKHR = sx_GetFenceFdKHR;
    t->dt.vkDestroySemaphore = sx_DestroySemaphore;
    t->dt.vkDestroyFence = sx_DestroyFence;
    VKB_ONCE(VKB_LOG_INFO, "sync-fd export: answered within %d ms (VKBRIDGE_SYNCFD_EXPORT_MS%s)", ms,
             v && *v ? "" : "; Samsung's driver's default");
}

/* Before the device goes: its exports still in the driver are waited for, then their held-back destroys done; 1 when one is still in the driver. */
int vkb_syncfd_device_destroy(vkb_srv_table *t)
{
    if (!t || t->id >= VKB_MAX_TABLES || !sx_ms[t->id]) return 0;
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
