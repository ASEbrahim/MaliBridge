/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * wg_zero: workgroup variables declared with an initializer (VK_KHR_zero_initialize_workgroup_memory,
 * core in Vulkan 1.3) zeroed by one invocation of each workgroup instead of by the driver.
 *
 * DXVK 3.x declares every compute shader's groupshared array with an OpConstantNull initializer.
 * Mali r44p1 (Immortalis-G720) makes that initialization cost about as if every invocation wrote
 * the whole array: in Crash Bandicoot 4 a 16x16 reduction with 4 KB of shared memory took 69 ms of
 * GPU time per frame (14,400 workgroups) and 3.9 ms with the initializer dropped. With this pass
 * the frame's compute dispatches took 10.6 ms instead of 78, and the game went from 4.4 to 6.3 fps.
 *
 * The pass drops the initializers and starts the entry point with
 *     new entry:  the function's OpVariables; %i = load LocalInvocationIndex
 *                 OpSelectionMerge %old_entry; branch (%i == 0) ? %zero : %old_entry
 *     %zero:      OpStore %var %initializer, for each variable; OpBranch %old_entry
 *     %old_entry: OpControlBarrier Workgroup Workgroup (AcquireRelease | WorkgroupMemory) ...
 * The old entry block has no predecessors, so it can be a merge block and the OpPhis that name it
 * stay right. Under the Vulkan memory model the stores are NonPrivatePointer and the barrier makes
 * them available and visible, as DXVK's own barriers do. LocalInvocationIndex is added when the
 * shader has none. Only single-entry-point GLCompute modules are rewritten.
 */
#ifndef VKB_SPIRV_WGZERO_H
#define VKB_SPIRV_WGZERO_H

#include <stddef.h>
#include <stdint.h>

/* 1: rewritten (*out malloc'd, *out_size bytes, *vars initializers moved), 0: nothing to do,
 * -1: not possible (*why says why; the module is left as it is). */
int vkb_spv_wg_zero(const uint32_t *code, size_t size, uint32_t **out, size_t *out_size, unsigned *vars, const char **why);

#endif
