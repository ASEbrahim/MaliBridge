/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * MK-45 (nan_position): the Mali-G77's r32p1 (Retroid Pocket 4 Pro) rasterizes a primitive whose
 * clip-space position holds NaN as a sheet over the whole render target, where other GPUs drop
 * it. Ys Origin's dead particles keep NaN world matrices (154 of 208 sprite draws in one frame),
 * harmless on a PC and a white screen here. This pass guards every vertex shader: right before
 * each return of the entry point, a position with a NaN in any component becomes (2, 2, 2, 1),
 * outside the clip volume, so the primitive is clipped away. Pure SPIR-V, no Vulkan.
 *
 * Nothing else changes, and the output is a pure function of the input (new ids numbered from the
 * old bound, in order): a module that is not a vertex shader, or has no Position output, is handed
 * back untouched.
 */
#ifndef VKB_SPIRV_NANPOS_H
#define VKB_SPIRV_NANPOS_H

#include <stddef.h>
#include <stdint.h>

typedef struct vkb_nanpos_stats {
    unsigned guards; /* returns guarded */
    unsigned member;  /* 1: the position is a member of gl_PerVertex, 0: a plain variable */
} vkb_nanpos_stats;

/* 1: rewritten (*out malloc'd, *out_size bytes), 0: nothing to do (nothing allocated), -1: not
 * possible (*why says why; the module is left as it is). st may be NULL. */
int vkb_spv_nan_position(const uint32_t *code, size_t size, uint32_t **out, size_t *out_size, vkb_nanpos_stats *st, const char **why);

#endif
