/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * lockstep: the strided loops of a 32-thread compute shader written for one 32-lane wave, made
 * to take the same number of iterations in every invocation, so that shm_barrier (level 2) can put
 * workgroup barriers inside them.
 *
 * Resident Evil 2 (DX11) compacts its draw lists in 32-thread workgroups with loops like
 *     for (i = tid; ; i += 32) { g = 0; if (i < n) ...; atomicOr(g, bit); m = g; ...; if (n <= i) break; }
 * and no barrier: shared memory stands in for a 32-lane ballot, which is right only while the 32
 * invocations run as one subgroup in lockstep. Mali runs them as two 16-wide subgroups, and one
 * clears or reads g while the other is still setting its bits: whole objects drop out of the draw
 * list for a frame (the flicker). A barrier is the fix, but the loop's exit is per invocation
 * (n <= i, i = tid + 32k), so in the last iteration not every invocation gets to it, and a
 * barrier there is undefined behaviour (DXVK's d3d11.forceComputeLdsBarriers puts Subgroup-scope
 * barriers there: no flicker on Mali, but geometry missing for good).
 *
 * The rewrite: a loop whose header has i = OpPhi(LocalInvocationId.x, i + W) (W = the
 * workgroup's size, X only, Y = Z = 1), whose one way out is a conditional branch on
 * (n <= i) or (i >= n) with n defined before the loop, after every Workgroup access of the
 * loop, gets a second phi b = OpPhi(0, b + W) and its exit test made on b instead of i: every
 * invocation then runs as many iterations as invocation 0 (the most). That is what a 32-lane wave
 * did: the lanes that were done stayed in the loop as inactive ones. It is only right when an
 * iteration past an invocation's last item does nothing (its work is under i < n), which this
 * code cannot check: the pass is for the modules a per-game preset names
 * (VKBRIDGE_SHARED_BARRIER_LOCKSTEP=<app hashes>), and loops without Workgroup accesses are left
 * alone. New ids from the old bound, in order: a pure function of the input.
 */
#ifndef VKB_SPIRV_LOCKSTEP_H
#define VKB_SPIRV_LOCKSTEP_H

#include <stddef.h>
#include <stdint.h>

/* 1: rewritten (*out malloc'd, *out_size bytes, *loops the loops changed), 0: nothing to do,
 * -1: not possible (*why). */
int vkb_spv_lockstep_loops(const uint32_t *code, size_t size, uint32_t **out, size_t *out_size, unsigned *loops, const char **why);

#endif
