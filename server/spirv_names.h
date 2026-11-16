/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * pv_names (PowerVR DXT 25.3): a vertex shader whose input and output variable carry the same
 * OpName reads its input as zero (DXVK names a passed-through attribute and its varying alike,
 * "TEXCOORD": Unity's full-screen blits then sample one texel). Names are debug information, so
 * the fix drops every OpName and OpMemberName of the module on its way to the driver. Pure SPIR-V,
 * no Vulkan: server_errata_pvnames.c and the host corpus share it.
 *
 * vkb_spv_strip_names: 1 with *out the module without its names (malloc'd), 0 when it has none
 * (nothing allocated), -1 when it is not a SPIR-V module. The output is a pure function of the
 * input, so the driver's pipeline cache keeps its entries across launches.
 *
 * vkb_spv_names_imitate (host tests only): the bug itself - when an Input and an Output variable
 * share an OpName, every store to that Output variable stores zero (an OpConstantNull of the
 * stored type). 1 with *out the changed module (*shared: how many output variables), 0 when no
 * name is shared, -1 when the module cannot be read or edited.
 */
#ifndef VKB_SPIRV_NAMES_H
#define VKB_SPIRV_NAMES_H

#include <stddef.h>
#include <stdint.h>

int vkb_spv_strip_names(const uint32_t *code, size_t size, uint32_t **out, size_t *out_size);
int vkb_spv_names_imitate(const uint32_t *code, size_t size, uint32_t **out, size_t *out_size, unsigned *shared);

#endif
