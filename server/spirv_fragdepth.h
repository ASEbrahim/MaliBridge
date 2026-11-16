/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * fragdepth_nodepth (MK-49): a fragment shader's depth output removed, for pipelines that render
 * to no depth attachment (server_errata_fragdepth.c). Pure functions on SPIR-V words.
 */
#ifndef VKB_SPIRV_FRAGDEPTH_H
#define VKB_SPIRV_FRAGDEPTH_H

#include <stddef.h>
#include <stdint.h>

/* 1 when the module is a fragment shader with an output decorated BuiltIn FragDepth, else 0 */
int vkb_spv_fragdepth_writes(const uint32_t *code, size_t bytes);

/* The module without its FragDepth output: the variable, its names and decorations, its place in
 * the entry point's interface, the stores to it and the Depth* execution modes go; everything else
 * stays word for word (the values the stores wrote are left computed and unused). Returns 1 with
 * *out (malloc'd) and *out_bytes, 0 when there is no such output, -1 when the module uses the
 * variable another way (a load, an access chain, a call argument, several entry points) with *why. */
int vkb_spv_strip_fragdepth(const uint32_t *code, size_t bytes, uint32_t **out, size_t *out_bytes, const char **why);

#endif
