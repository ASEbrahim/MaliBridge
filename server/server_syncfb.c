/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim
 *
 * The server's half of the client's fence mirror (client/syncmirror.c). The client answers a
 * fence's status from a slot its GPU work writes at the end of the fence's submit, a moment
 * before the driver signals the fence itself. An app that saw it done there may reset or destroy
 * the fence at once, while the driver's signal is still on its way, which the spec does not
 * allow. So a fence submitted and not yet seen signalled here is waited for (it is done or about
 * to be) before a vkResetFences or vkDestroyFence reaches the driver. Fences the app waited on
 * or polled to done are forgotten at once, so the wait is only paid where the mirror answered.
 * Over every layer but the session report's.
 */
#include "vkb_server.h"

#include <pthread.h>
#include <stdlib.h>

#define FB_WAIT_NS (2ull * 1000 * 1000 * 1000)

typedef struct fb_pending {
    uint32_t table;
    VkFence fence;
} fb_pending;

static fb_pending *pend;
static uint32_t npend, cap_pend;
static pthread_mutex_t fb_lock = PTHREAD_MUTEX_INITIALIZER;

static PFN_vkQueueSubmit next_submit[VKB_MAX_TABLES];
static PFN_vkQueueSubmit2 next_submit2[VKB_MAX_TABLES];
static PFN_vkQueueBindSparse next_sparse[VKB_MAX_TABLES];
static PFN_vkGetFenceStatus next_status[VKB_MAX_TABLES];
static PFN_vkWaitForFences next_wait[VKB_MAX_TABLES];
static PFN_vkResetFences next_reset[VKB_MAX_TABLES];
static PFN_vkDestroyFence next_destroy[VKB_MAX_TABLES];

static void fb_add(uint32_t table, VkFence f)
{
    if (!f) return;
    pthread_mutex_lock(&fb_lock);
    uint32_t i = 0;
    for (; i < npend && (pend[i].table != table || pend[i].fence != f); i++) {}
    if (i == npend) {
        if (npend == cap_pend) {
            uint32_t cap = cap_pend ? cap_pend * 2 : 64;
            fb_pending *n = realloc(pend, cap * sizeof(*n));
            if (!n) {
                pthread_mutex_unlock(&fb_lock);
                return;
            }
            pend = n, cap_pend = cap;
        }
        pend[npend++] = (fb_pending){table, f};
    }
    pthread_mutex_unlock(&fb_lock);
}

/* 1 when it was pending. */
static int fb_take(uint32_t table, VkFence f)
{
    if (!f || !__atomic_load_n(&npend, __ATOMIC_RELAXED)) return 0;
    int had = 0;
    pthread_mutex_lock(&fb_lock);
    for (uint32_t i = 0; i < npend; i++)
        if (pend[i].table == table && pend[i].fence == f) {
            pend[i] = pend[--npend];
            had = 1;
            break;
        }
    pthread_mutex_unlock(&fb_lock);
    return had;
}

static void fb_settle(vkb_srv_table *t, VkDevice d, VkFence f)
{
    if (!fb_take(t->id, f) || !next_wait[t->id]) return;
    VkResult r = next_wait[t->id](d, 1, &f, VK_TRUE, FB_WAIT_NS);
    if (r == VK_TIMEOUT) {
        static int told;
        if (!told++)
            VKB_WARN("fence reset or destroyed while its submit was still running (2 s waited; the app's own error, or a GPU hang); "
                     "logged once");
    }
}

static VKAPI_ATTR VkResult VKAPI_CALL fb_QueueSubmit(VkQueue q, uint32_t n, const VkSubmitInfo *si, VkFence f)
{
    vkb_srv_table *t = vkb_srv_current_table();
    PFN_vkQueueSubmit next = t ? next_submit[t->id] : NULL;
    if (!next) return VK_ERROR_DEVICE_LOST;
    fb_add(t->id, f);
    VkResult r = next(q, n, si, f);
    if (r != VK_SUCCESS) fb_take(t->id, f);
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL fb_QueueSubmit2(VkQueue q, uint32_t n, const VkSubmitInfo2 *si, VkFence f)
{
    vkb_srv_table *t = vkb_srv_current_table();
    PFN_vkQueueSubmit2 next = t ? next_submit2[t->id] : NULL;
    if (!next) return VK_ERROR_DEVICE_LOST;
    fb_add(t->id, f);
    VkResult r = next(q, n, si, f);
    if (r != VK_SUCCESS) fb_take(t->id, f);
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL fb_QueueBindSparse(VkQueue q, uint32_t n, const VkBindSparseInfo *bi, VkFence f)
{
    vkb_srv_table *t = vkb_srv_current_table();
    PFN_vkQueueBindSparse next = t ? next_sparse[t->id] : NULL;
    if (!next) return VK_ERROR_DEVICE_LOST;
    fb_add(t->id, f);
    VkResult r = next(q, n, bi, f);
    if (r != VK_SUCCESS) fb_take(t->id, f);
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL fb_GetFenceStatus(VkDevice d, VkFence f)
{
    vkb_srv_table *t = vkb_srv_current_table();
    PFN_vkGetFenceStatus next = t ? next_status[t->id] : NULL;
    if (!next) return VK_ERROR_DEVICE_LOST;
    VkResult r = next(d, f);
    if (r == VK_SUCCESS) fb_take(t->id, f);
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL fb_WaitForFences(VkDevice d, uint32_t n, const VkFence *fs, VkBool32 all, uint64_t timeout)
{
    vkb_srv_table *t = vkb_srv_current_table();
    PFN_vkWaitForFences next = t ? next_wait[t->id] : NULL;
    if (!next) return VK_ERROR_DEVICE_LOST;
    VkResult r = next(d, n, fs, all, timeout);
    if (r == VK_SUCCESS && (all || n == 1))
        for (uint32_t i = 0; i < n; i++) fb_take(t->id, fs[i]);
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL fb_ResetFences(VkDevice d, uint32_t n, const VkFence *fs)
{
    vkb_srv_table *t = vkb_srv_current_table();
    PFN_vkResetFences next = t ? next_reset[t->id] : NULL;
    if (!next) return VK_ERROR_DEVICE_LOST;
    for (uint32_t i = 0; i < n; i++) fb_settle(t, d, fs[i]);
    return next(d, n, fs);
}

static VKAPI_ATTR void VKAPI_CALL fb_DestroyFence(VkDevice d, VkFence f, const VkAllocationCallbacks *a)
{
    vkb_srv_table *t = vkb_srv_current_table();
    PFN_vkDestroyFence next = t ? next_destroy[t->id] : NULL;
    if (!next) return;
    fb_settle(t, d, f);
    next(d, f, a);
}

void vkb_syncfb_device_init(vkb_srv_table *t)
{
    if (!t || t->id >= VKB_MAX_TABLES || !t->dt.vkWaitForFences) return;
    next_submit[t->id] = t->dt.vkQueueSubmit;
    next_submit2[t->id] = t->dt.vkQueueSubmit2;
    next_sparse[t->id] = t->dt.vkQueueBindSparse;
    next_status[t->id] = t->dt.vkGetFenceStatus;
    next_wait[t->id] = t->dt.vkWaitForFences;
    next_reset[t->id] = t->dt.vkResetFences;
    next_destroy[t->id] = t->dt.vkDestroyFence;
    if (t->dt.vkQueueSubmit) t->dt.vkQueueSubmit = fb_QueueSubmit;
    if (t->dt.vkQueueSubmit2) t->dt.vkQueueSubmit2 = fb_QueueSubmit2;
    if (t->dt.vkQueueBindSparse) t->dt.vkQueueBindSparse = fb_QueueBindSparse;
    if (t->dt.vkGetFenceStatus) t->dt.vkGetFenceStatus = fb_GetFenceStatus;
    t->dt.vkWaitForFences = fb_WaitForFences;
    if (t->dt.vkResetFences) t->dt.vkResetFences = fb_ResetFences;
    if (t->dt.vkDestroyFence) t->dt.vkDestroyFence = fb_DestroyFence;
}

/* The device's fences are gone with it. */
void vkb_syncfb_device_destroy(vkb_srv_table *t)
{
    if (!t) return;
    pthread_mutex_lock(&fb_lock);
    for (uint32_t i = 0; i < npend;)
        if (pend[i].table == t->id) pend[i] = pend[--npend];
        else i++;
    pthread_mutex_unlock(&fb_lock);
}
