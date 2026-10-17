/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * shl_barrier: an optimization barrier after every left shift by a constant, for Mali's (and
 * Adreno's) shader compiler. A shift by a constant is folded by the compiler into the address
 * arithmetic around it, and on some drivers that fold turns into speculative memory reads/writes
 * and GPU faults (INSTR_BARRIER_FAULT / CS_RESOURCE_TERMINATED in the kernel log, a device lost).
 * After
 *     %r = OpShiftLeftLogical %T %x %c          (%c an OpConstant / OpConstantComposite / OpConstantNull)
 * the pass writes
 *     %t = OpShiftLeftLogical %T %x %c
 *     %r = OpBitFieldInsert %T %t %zero %u0 %u0  (insert 0 bits: a no-op the compiler does not see through)
 * %r keeps its id, so its decorations and uses are untouched.
 *
 * A port of Lee Gao's MaliOptimizationBarrierPass (SPIRV-Tools fork, source/opt/
 * mali_optimization_barrier_pass.cpp, Apache-2.0; bionic-vulkan-wrapper PR #162, where it is on by
 * default for Arm and Qualcomm drivers). This version works on the words, without SPIRV-Tools, and
 * differs in three ways: only 32-bit integer results are barriered (Vulkan allows OpBitFieldInsert
 * on 32-bit integers only, VUID-StandaloneSpirv-Base-04781, unless maintenance9: the original
 * wrote 32-bit zeros for 16/64-bit shifts, which is invalid), a shift already followed by such a
 * barrier is left as it is (running the pass twice changes nothing), and the output is a pure
 * function of the input (new ids numbered from the old bound, in order), so the driver's pipeline
 * cache keeps hitting from one launch to the next.
 */
#ifndef VKB_SPIRV_SHLBARRIER_H
#define VKB_SPIRV_SHLBARRIER_H

#include <stddef.h>
#include <stdint.h>

typedef struct vkb_shlbarrier_stats {
    unsigned scalar, vector;  /* barriers inserted */
    unsigned skipped_width;   /* constant shifts left alone: not 32-bit */
    unsigned already;         /* constant shifts already followed by a barrier */
} vkb_shlbarrier_stats;

/* 1: rewritten (*out malloc'd, *out_size bytes), 0: nothing to do (nothing allocated), -1: not
 * possible (*why says why; the module is left as it is). st may be NULL. */
int vkb_spv_shl_barrier(const uint32_t *code, size_t size, uint32_t **out, size_t *out_size, vkb_shlbarrier_stats *st,
                        const char **why);

#endif
