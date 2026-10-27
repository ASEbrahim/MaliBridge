/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
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
    uint32_t forced;       /* on by VKBRIDGE_ERRATA=+name */
    uint32_t targeted;     /* on for a reason other than "always" alone: a probe, the range, an
                              unknown version, VKBRIDGE_ERRATA=+name (a fix may do more then) */
    vkb_dispatch next_fix; /* what each fix calls: the driver's functions, or the imitation's */
    vkb_dispatch next_imi; /* what each imitation calls: the driver's */
    uint64_t example_calls, example_imitated;
    PFN_vkCreateImageView example_next, example_imi_next;
    struct vkb_errata_views *views; /* server_errata_views.c (MK-06, MK-07) */
    struct vkb_errata_bitwise *bitwise; /* server_errata_bitwise.c (MK-03) */
    struct vkb_errata_cmd *cmd;     /* server_errata_cmd.c (MK-04, MK-05) */
    struct vkb_errata_flip *flip;       /* server_errata_flip.c (MK-02) */
    struct vkb_errata_clampzero *clampzero; /* server_errata_clampzero.c (MK-32) */
    struct vkb_errata_mem *mem;     /* server_errata_mem.c (MK-39) */
    PFN_vkCreateShaderModule fp16_imi_next; /* server_errata_fp16.c (MK-20): beneath its imitation */
    struct vkb_errata_ds *ds;       /* server_errata_ds.c (MK-11) */
    struct vkb_errata_vbbake *vbbake;  /* server_errata_vbbake.c (MK-05 option B) */
    struct vkb_errata_nanpos *nanpos;  /* server_errata_nanpos.c (MK-45) */
    struct vkb_vbbake_imi *vbbake_imi;
    int probe; /* the startup probe's: an imitation only, for one probe (vkb_errata_probe_begin);
                  it publishes nothing a server thread without a table would fall back to, and logs
                  no totals */
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

/* A device's setup line (an install's settings): at info level the first time this server process
 * logs that text or with a device whose decisions log in full, at debug level after, so a session's
 * dozens of devices log it once. */
void vkb_errata_info_once(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

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
/* ... and cull_nontri (MK-16), chained above them (installed after) */
void vkb_errata_cull_install(vkb_srv_table *t, vkb_errata_dev *e);
void vkb_errata_cull_imitate(vkb_srv_table *t, vkb_errata_dev *e);
void vkb_errata_cmd_destroy(vkb_srv_table *t, vkb_errata_dev *e);

/* server_errata_mem.c: dedicated_import (MK-39), the dedicated-allocation struct taken off imports
 * whose resource was not made for that handle type; the imitation fails such imports */
void vkb_errata_dedicated_install(vkb_srv_table *t, vkb_errata_dev *e);
void vkb_errata_dedicated_imitate(vkb_srv_table *t, vkb_errata_dev *e);
void vkb_errata_mem_destroy(vkb_srv_table *t, vkb_errata_dev *e);

/* server_errata_fp16.c: fp16_hide (MK-20). The fix is in the feature queries and vkCreateDevice
 * (vkb_errata_fp16_hidden and the functions after it, vkb_server.h); its install logs what the
 * device gets. The imitation (host) miscompiles 16-bit float multiplies and fmas. */
void vkb_errata_fp16_install(vkb_srv_table *t, vkb_errata_dev *e);
void vkb_errata_fp16_imitate(vkb_srv_table *t, vkb_errata_dev *e);
/* server_errata_ds.c: stencil_layout (MK-11), separate stencil layouts of render passes (and of
 * vkCmdBeginRendering) merged into combined ones; the imitation loses the stencil at the end of
 * a pass with a read-only depth and a writable stencil layout */
void vkb_errata_stencil_install(vkb_srv_table *t, vkb_errata_dev *e);
void vkb_errata_vbbake_install(vkb_srv_table *t, vkb_errata_dev *e);
void vkb_errata_vbbake_imitate(vkb_srv_table *t, vkb_errata_dev *e);
void vkb_errata_vbbake_destroy(vkb_errata_dev *e); /* totals logged, tables freed */
void vkb_errata_stencil_imitate(vkb_srv_table *t, vkb_errata_dev *e);
/* server_errata_nanpos.c: nan_position (MK-45), vertex shaders guarded against NaN positions (spirv_nanpos.c) */
void vkb_errata_nanpos_install(vkb_srv_table *t, vkb_errata_dev *e);
void vkb_errata_nanpos_destroy(vkb_srv_table *t, vkb_errata_dev *e);
void vkb_errata_ds_destroy(vkb_srv_table *t, vkb_errata_dev *e);

/* A module an erratum rewrote is named by the code it came from (vkb_spv_app_hash): registers
 * the new code's hash -> the app's hash of `in` (itself maybe an erratum's output); 1 when the
 * new code was not registered before (a module to log) */
int vkb_errata_spv_origin(const uint32_t *out, size_t out_size, const uint32_t *in, size_t in_size);

/* server_errata_flip.c: viewport_flip (MK-02), positive viewports and gl_Position.y negated in
 * the last pre-rasterization stage, pipeline variants picked at draw time; the imitation turns
 * negative viewports into positive ones over the same rows, beneath the fix */
void vkb_errata_flip_install(vkb_srv_table *t, vkb_errata_dev *e);
void vkb_errata_flip_imitate(vkb_srv_table *t, vkb_errata_dev *e);
void vkb_errata_flip_destroy(vkb_srv_table *t, vkb_errata_dev *e); /* before the driver's vkDestroyDevice */

/* server_errata_clampzero.c: clamp_zero (MK-32), the SPIR-V rewrite on module and pipeline
 * creation, chained above bitwise_scalar's; the imitation is the same hooks beneath it */
void vkb_errata_clampzero_install(vkb_srv_table *t, vkb_errata_dev *e);
void vkb_errata_clampzero_imitate(vkb_srv_table *t, vkb_errata_dev *e);
void vkb_errata_clampzero_destroy(vkb_srv_table *t, vkb_errata_dev *e);

/* server_errata_gs.c: gs_hide (issue #11). The fix is in the instance's queries and vkCreateDevice
 * (vkb_errata_gs_hidden and the functions after it, vkb_server.h); its install says whether this
 * device's process is one it hides geometry shaders from. No imitation: the crash is imitated by
 * VKBRIDGE_TEST_CRASH_PIPELINE=gs (server_guard.c), on any build. */
void vkb_errata_gs_install(vkb_srv_table *t, vkb_errata_dev *e);

#endif
