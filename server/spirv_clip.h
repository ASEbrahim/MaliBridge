/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 ASEbrahim and MaliBridge contributors */
#ifndef VKB_SPIRV_CLIP_H
#define VKB_SPIRV_CLIP_H

#include "spirv_edit.h"

/* What a device lacks (spirv_clip.c): ClipDistance, CullDistance, PointSize in geometry/tessellation. */
enum { VKB_SPV_CLIP = 1, VKB_SPV_CULL = 2, VKB_SPV_POINT_SIZE = 4 };

typedef struct vkb_spv_clip_info {
    uint32_t out[3], in[3]; /* locations taken per vertex by the demoted clip, cull, point size outputs / inputs */
    uint32_t clip_in_var;   /* the ClipDistance input as a Location variable (fragment shaders read it), or 0 */
    const char *why;        /* set when a built-in had to be left */
} vkb_spv_clip_info;

/* 1 when the module uses something of `what`: by capability or by a BuiltIn decoration alone
 * (Unity's HLSLcc writes ClipDistance through gl_PerVertex with only OpCapability Shader). */
int vkb_spv_clip_needed(const spv_mod *m, unsigned what);
/* Highest Location of the stage's inputs and outputs (per vertex for arrayed interfaces), or -1. */
int vkb_spv_clip_max_location(const spv_mod *m);
/* Turns the built-ins of `what` - loose variables or members of interface blocks, outputs and
 * inputs - into Location variables: clip at `loc`, cull after the clip distances, point size after
 * those. An input's next slot starts after max(its own count, prev_out[] of the stage before), so
 * a stage's inputs land where the previous stage's outputs did (prev_out may be NULL).
 * 0 when nothing of `what` is left as a built-in; -1 otherwise (info->why says why). */
int vkb_spv_clip_demote(spv_mod *m, unsigned what, uint32_t loc, const uint32_t *prev_out, vkb_spv_clip_info *info);
/* Fragment side: discard when any of the `n` distances at `loc` is negative. `var` is the module's
 * own input there (a demoted gl_ClipDistance) or 0 for a new one. */
int vkb_spv_clip_discard(spv_mod *m, uint32_t loc, uint32_t n, uint32_t var);

#endif
