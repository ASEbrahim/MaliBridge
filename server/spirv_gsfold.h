/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A layer-only pass-through geometry shader folded into the vertex shader (spirv_gsfold.c).
 */
#ifndef VKB_SPIRV_GSFOLD_H
#define VKB_SPIRV_GSFOLD_H

#include <stddef.h>
#include <stdint.h>

/* 1 when the geometry shader only passes triangles through and writes Layer from the scalar input
 * at *layer_location (set then); 0 otherwise. */
int vkb_gs_layer_passthrough(const uint32_t *code, size_t size, uint32_t *layer_location);
/* The vertex shader with its output at that location written to BuiltIn Layer instead (malloc'd,
 * *out_size bytes), or NULL when the output is not a lone 32-bit integer. */
uint32_t *vkb_vs_write_layer(const uint32_t *code, size_t size, uint32_t location, size_t *out_size);

#endif
