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
    vkb_dispatch next_fix; /* what each fix calls: the driver's functions, or the imitation's */
    vkb_dispatch next_imi; /* what each imitation calls: the driver's */
    uint64_t example_calls, example_imitated;
    struct vkb_errata_views *views; /* server_errata_views.c (MK-06, MK-07) */
} vkb_errata_dev;

/* The device a wrapper runs for: the call's table; on a thread without one (the server's own
 * threads), the last device made - the driver's functions are the same for every device. */
vkb_errata_dev *vkb_errata_cur(void);

/* Puts fn in place of the driver-level function at *real (and of the app-facing one, when nothing
 * above has wrapped it yet). next_fix and next_imi are taken once, beneath all fixes and all
 * imitations: two errata that hook one function bypass each other through them, so such errata
 * share one wrapper set that calls t->real as it found it (server_errata_views.c). */
#define ERRATA_HOOK(t, field, fn)                                                                        \
    do {                                                                                                 \
        if ((t)->real.field) {                                                                           \
            if ((t)->dt.field == (t)->real.field) (t)->dt.field = (fn);                                  \
            (t)->real.field = (fn);                                                                      \
        }                                                                                                \
    } while (0)

/* server_errata_views.c: one set of image and image-view wrappers for view_quarantine (MK-06) and
 * view_counts (MK-07), fixes and imitations alike (installed once, whichever asks first) */
void vkb_errata_views_install(vkb_srv_table *t, vkb_errata_dev *e);
void vkb_errata_views_destroy(vkb_srv_table *t, vkb_errata_dev *e); /* before the driver's vkDestroyDevice */

#endif
