/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 ASEbrahim and MaliBridge contributors */
#ifndef VKB_SPIRV_PERVERTEX_H
#define VKB_SPIRV_PERVERTEX_H

#include <stddef.h>
#include <stdint.h>

/* Folds a geometry/tessellation shader's loose per-vertex built-ins into gl_PerVertex blocks
 * (spirv_pervertex.c). 1 and a malloc'd module in *out when it rewrote; 0 when nothing to do or
 * it could not (then *why says why, or is NULL when there was simply nothing to do). */
int vkb_spv_wrap_pervertex(const uint32_t *code, size_t size, uint32_t **out, size_t *out_size, const char **why);

#endif
