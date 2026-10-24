/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * What a shader module writes beyond its own outputs, for the asynchronous pipeline compile
 * (server_async.c): a pipeline whose draws may be skipped while it compiles must have no effect
 * other than its colour (and depth) output - no stores to storage buffers or images, no atomics,
 * no transform feedback. The scan is conservative: what it cannot place counts as a store.
 */
#ifndef VKB_SPIRV_EFFECTS_H
#define VKB_SPIRV_EFFECTS_H

#include <stddef.h>
#include <stdint.h>

#define SPV_FX_STORE 0x1u /* memory other invocations or later passes see: storage buffers, images, atomics */
#define SPV_FX_XFB 0x2u   /* transform feedback (the Xfb execution mode or capability) */
#define SPV_FX_BAD 0x4u   /* not SPIR-V the scan could read (treated as both) */

uint32_t spv_effects(const uint32_t *code, size_t bytes);

#endif
