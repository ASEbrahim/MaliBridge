/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * server_lostlog.c: what a lost device was given, for the log at its loss (ledger row 225). The
 * records are written on the submit path (server_lost.c's wrappers, beneath every layer) with plain
 * and relaxed atomic stores only: no lock, no wait, no driver call. See server_lostlog.c.
 */
#ifndef VKB_LOSTLOG_H
#define VKB_LOSTLOG_H

#include "vkb_server.h"

#define VKB_LL_RING 32  /* submissions kept per device (a power of two) */
#define VKB_LL_DONE 32  /* finished answers kept per device (a power of two) */
#define VKB_LL_CBS 4    /* command buffers named per submission */
#define VKB_LL_PIPES 4  /* pipelines named per command buffer (the last bound) */
#define VKB_LL_IN_DRIVER 1 /* a submission's answer while the driver's call has not returned */

typedef struct vkb_ll_sub {
    uint32_t seq;         /* 0 while written; else its number (1, 2, ...) */
    uint8_t kind;         /* 0 vkQueueSubmit, 1 vkQueueSubmit2, 2 vkQueueBindSparse */
    uint16_t nbatch, ncb, nwait, nsig;
    int32_t r;            /* the driver's answer (VKB_LL_IN_DRIVER: not returned yet) */
    uint64_t t_ns;        /* CLOCK_MONOTONIC */
    uint64_t queue, fence, sig_sem, sig_val;
    uint64_t cb[VKB_LL_CBS];
} vkb_ll_sub;

typedef struct vkb_ll_done {
    uint64_t h, v; /* a fence (v 0) or a semaphore reached v, as a wait or a fence status said */
    uint32_t after; /* submissions recorded when it said so: it covers those (a fence is reset before it is submitted again) */
} vkb_ll_done;

typedef struct vkb_ll {
    vkb_srv_table *t;
    uint32_t n;                      /* submissions recorded */
    vkb_ll_sub ring[VKB_LL_RING];
    uint32_t dn;
    vkb_ll_done done[VKB_LL_DONE];
    const char *said;                /* a lost answer not confirmed (server_lost.c suspect); NULL: none */
    uint64_t said_ns;
    int reported;                    /* the record is written (or being written) */
    volatile int helper;             /* 0 none, 1 the helper thread runs, 2 it is done */
    char why[200];
    int driver_said;                 /* the driver itself said lost: its fault report is asked for */
    uint64_t lost_ns;
    PFN_vkGetDeviceFaultInfoEXT fault_info;
} vkb_ll;

/* server_lost.c's calls */
vkb_ll *vkb_ll_device_init(vkb_srv_table *t);
void vkb_ll_device_destroy(vkb_ll *l); /* the record of a device said lost and never confirmed, then freed */
int vkb_ll_busy(vkb_ll *l, double wait_s); /* the helper still runs after waiting up to wait_s */
void vkb_ll_lost(vkb_ll *l, const char *why, int driver_said); /* at the latch: the record on a helper thread */
void vkb_ll_said(vkb_ll *l, const char *what);
void vkb_ll_unsaid(vkb_ll *l);
/* before the driver's call (a submission that blocks in the driver is named too), then its answer */
vkb_ll_sub *vkb_ll_submit(vkb_ll *l, int kind, VkQueue q, uint32_t n, const void *infos, VkFence f);
static inline void vkb_ll_answer(vkb_ll_sub *e, VkResult r)
{
    if (e) __atomic_store_n(&e->r, (int32_t)r, __ATOMIC_RELAXED);
}
void vkb_ll_fences_done(vkb_ll *l, uint32_t n, const VkFence *f, VkBool32 all);
void vkb_ll_sems_done(vkb_ll *l, const VkSemaphoreWaitInfo *wi);

#endif
