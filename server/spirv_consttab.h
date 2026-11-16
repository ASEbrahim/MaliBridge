/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * PV-10 (PowerVR DXT-48-1536, driver 0x67ab5a, Pixel 10 Pro XL): with robust buffer access on - DXVK
 * turns it on for every game, with robustness2's nullDescriptor - the driver reads a constant array
 * indexed at run time as 0 past a cut-off that moves with the shader's constants (a 246-entry table
 * from entry 174 on in one shader, from 124 in another, a uvec4[64] from element 37). glslang emits
 * such an array as a Function variable stored whole with an OpConstantComposite and read through
 * access chains (its "indexable" locals). The same table in a Private variable stored the same way
 * reads right on the same driver, robustness on (bcn_check spvrun, the probe4 control); so do shared
 * memory and a switch. With nullDescriptor on the driver also ignores VkPipelineRobustnessCreateInfo,
 * so the robustness of a pipeline cannot be turned off for it (PV-09, dropped).
 *
 * This pass moves every Function variable of array type that only ever holds a whole constant (all
 * its stores are of an OpConstantComposite / OpConstantNull) and is only read (loads, access chains
 * read by loads) into a global Private variable with the same id; the stores stay where they are, so
 * every read sees what it saw before. Access chains into it get Private pointer types. SPIR-V 1.4 and
 * later: the variable joins every entry point's interface. Pure SPIR-V, no Vulkan; nothing else changes,
 * new ids are numbered from the old bound in order, and a module without such a variable is handed
 * back untouched.
 */
#ifndef VKB_SPIRV_CONSTTAB_H
#define VKB_SPIRV_CONSTTAB_H

#include <stddef.h>
#include <stdint.h>

typedef struct vkb_consttab_stats {
    unsigned vars;     /* Function arrays moved to Private */
    unsigned words;    /* their constants' size: array elements, summed (outer x inner) */
    unsigned chains;   /* access chains retyped */
    unsigned skipped;  /* Function arrays left: a non-constant store or another use */
} vkb_consttab_stats;

/* 1: rewritten (*out malloc'd, *out_size bytes), 0: nothing to move (nothing allocated), -1: not
 * possible (*why says why; the module is left as it is). st and why may be NULL. */
int vkb_spv_consttab(const uint32_t *code, size_t size, uint32_t **out, size_t *out_size, vkb_consttab_stats *st,
                     const char **why);

#endif
