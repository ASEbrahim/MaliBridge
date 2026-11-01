/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * Workgroup variables zeroed by the shader instead of by initializers (spirv_wginit.c).
 */
#ifndef VKB_SPIRV_WGINIT_H
#define VKB_SPIRV_WGINIT_H

#include <stddef.h>
#include <stdint.h>

typedef struct vkb_wginit_stats {
    unsigned vars;     /* initializers removed */
    unsigned arrays;   /* ... of arrays, zeroed across the workgroup */
    unsigned singles;  /* ... of single values, zeroed by invocation 0 */
    unsigned stores;   /* stores placed */
} vkb_wginit_stats;

/* 1: *out (malloc'd, *out_size bytes) is the rewritten module; 0: nothing to do; -1: left as it is (*why) */
int vkb_spv_wg_init(const uint32_t *code, size_t size, uint32_t **out, size_t *out_size, vkb_wginit_stats *st, const char **why);

#endif
