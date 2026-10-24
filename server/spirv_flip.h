/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * MK-02 (viewport_flip): a driver that ignores a negative viewport height (Mali-G715 r54p2) is
 * given positive heights instead, and the last pre-rasterization stage negates gl_Position.y.
 * With the viewport (x, y + h, w, -h) rewritten to (x, y, w, h) and y_clip negated, every vertex
 * lands on the same framebuffer position: fb_y = y + h * (1 - y_ndc) / 2 both ways. So winding
 * (culling, gl_FrontFacing), gl_FragCoord, derivatives, gl_PointCoord and depth are unchanged;
 * the clip volume is symmetric in y, so clipping is too.
 *
 * This pass writes the negation. Pure SPIR-V, no Vulkan (server_errata_flip.c uses it; a host
 * tool could run it over a corpus). The sites:
 *   vertex, tessellation evaluation   before each OpReturn of the entry point's function: the
 *                                     final gl_Position is loaded, y negated, stored back;
 *   geometry                          before each OpEmitVertex / OpEmitStreamVertex (any
 *                                     function), and the app's value stored back after it, so a
 *                                     shader that emits one position twice still sees its own.
 * gl_Position may be a loose output or a gl_PerVertex block member. Varyings the shader computes
 * from its own position are left as they are (only the rasterized position moves).
 *
 * Multi-viewport: with a negative and a positive viewport in one draw, only the negative ones
 * flip. mask bit i says "flip for ViewportIndex i" (of count viewports in use); a stage that writes
 * ViewportIndex then gets   y *= ((mask >> ViewportIndex) & 1) ? -1.0 : 1.0   at each site; one
 * that does not uses viewport 0 only (bit 0). A full mask is a plain negation.
 *
 * The output is a pure function of (code, model, entry, mask, count): new ids from the old bound,
 * in order, so the driver's pipeline cache sees the same module every launch.
 */
#ifndef VKB_SPIRV_FLIP_H
#define VKB_SPIRV_FLIP_H

#include <stddef.h>
#include <stdint.h>

typedef struct vkb_flip_info {
    int position;        /* the entry point has a gl_Position output */
    int in_block;        /* ... as a block member */
    int writes_vpindex;  /* a ViewportIndex output */
    int xfb;             /* ExecutionMode Xfb: a captured gl_Position would be captured flipped */
    unsigned sites;      /* returns / emits rewritten */
    int conditional;     /* the flip is picked by ViewportIndex */
} vkb_flip_info;

/* What the pass would see: 0 with *info filled, -1 when the entry point is not there (*why). */
int vkb_spv_flip_info(const uint32_t *code, size_t size, uint32_t model, const char *entry, vkb_flip_info *info, const char **why);

/* 1: rewritten (*out malloc'd, *out_size bytes); 0: nothing to flip (no gl_Position, or a mask
 * that flips no viewport this stage can reach); -1: not possible (*why). info may be NULL. */
int vkb_spv_flip(const uint32_t *code, size_t size, uint32_t model, const char *entry, uint32_t mask, uint32_t count,
                 uint32_t **out, size_t *out_size, vkb_flip_info *info, const char **why);

#endif
