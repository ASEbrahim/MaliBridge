/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The driver-errata layer's insides, shared by server_errata.c (the decisions) and the files that
 * hold its fixes (server_errata_*.c).
 */
#ifndef VKB_SERVER_ERRATA_H
#define VKB_SERVER_ERRATA_H

#include "vkb_server.h"

typedef struct vkb_errata_dev {
    uint32_t on;           /* 1 << VKB_ERRATUM_*: decided on */
    uint32_t imitated;
    uint32_t targeted;     /* on for a reason other than "always" alone: a probe, the range, an
                              unknown version, VKBRIDGE_ERRATA=+name (a fix may do more then) */
    vkb_dispatch next_fix; /* what each fix calls: the driver's functions, or the imitation's */
    vkb_dispatch next_imi; /* what each imitation calls: the driver's */
    uint64_t example_calls, example_imitated;
    PFN_vkCreateImageView example_next, example_imi_next;
    struct vkb_errata_views *views; /* server_errata_views.c (MK-06, MK-07) */
    struct vkb_errata_bitwise *bitwise; /* server_errata_bitwise.c (MK-03) */
    struct vkb_errata_cmd *cmd;     /* server_errata_cmd.c (MK-04, MK-05) */
} vkb_errata_dev;

/* The device a wrapper runs for: the call's table; on a thread without one (the server's own
 * threads), the last device made - the driver's functions are the same for every device. */
vkb_errata_dev *vkb_errata_cur(void);

/* Puts fn in place of the driver-level function at *real (and of the app-facing one, when nothing
 * above has wrapped it yet). A wrapper calls the function it replaced: t->real as its install found
 * it (each install keeps that copy), so the errata that hook one function chain - each one's
 * wrapper calls the next one down (server_errata_views.c, server_errata_cmd.c, the example).
 * next_fix and next_imi are taken once, beneath all fixes and all imitations: a wrapper that calls
 * them skips every other erratum's wrapper of the same function. */
#define ERRATA_HOOK(t, field, fn)                                                                        \
    do {                                                                                                 \
        if ((t)->real.field) {                                                                           \
            if ((t)->dt.field == (t)->real.field) (t)->dt.field = (fn);                                  \
            (t)->real.field = (fn);                                                                      \
        }                                                                                                \
    } while (0)

/* What a fix's wrapper calls next (imitation 0: e->next_fix) or an imitation's (1: next_imi), for
 * the call's device; with no device left (a server thread after the last vkDestroyDevice), the
 * last device's. */
const vkb_dispatch *vkb_errata_next(int imitation);

/* server_wait.c: wait_retry (MK-01), waits the driver fails with -3 after a signal */
void vkb_wait_retry_install(vkb_srv_table *t, vkb_errata_dev *e);
void vkb_wait_retry_imitate(vkb_srv_table *t, vkb_errata_dev *e);

/* server_errata_views.c: one set of image and image-view wrappers for view_quarantine (MK-06) and
 * view_counts (MK-07), fixes and imitations alike (installed once, whichever asks first) */
void vkb_errata_views_install(vkb_srv_table *t, vkb_errata_dev *e);
void vkb_errata_views_destroy(vkb_srv_table *t, vkb_errata_dev *e); /* before the driver's vkDestroyDevice */

/* server_errata_bitwise.c: bitwise_scalar (MK-03), the SPIR-V rewrite on module and pipeline
 * creation; the imitation is the same hooks beneath it */
void vkb_errata_bitwise_install(vkb_srv_table *t, vkb_errata_dev *e);
void vkb_errata_bitwise_imitate(vkb_srv_table *t, vkb_errata_dev *e);
void vkb_errata_bitwise_destroy(vkb_srv_table *t, vkb_errata_dev *e);
/* server_errata_cmd.c: command-buffer state - dyn_reemit (MK-04) and vb_split (MK-05); their
 * wrappers chain, the command-buffer and pipeline records are installed once beneath them */
void vkb_errata_dyn_install(vkb_srv_table *t, vkb_errata_dev *e);
void vkb_errata_dyn_imitate(vkb_srv_table *t, vkb_errata_dev *e);
void vkb_errata_vb_install(vkb_srv_table *t, vkb_errata_dev *e);
void vkb_errata_vb_imitate(vkb_srv_table *t, vkb_errata_dev *e);
void vkb_errata_cmd_destroy(vkb_srv_table *t, vkb_errata_dev *e);

#endif
