/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * wave32: 32-wide subgroups emulated in compute shaders, for GPUs whose subgroups are narrower.
 *
 * D3D12 compute shaders made for 32-wide waves count on it: a ballot's bits 16-31, shuffles 16
 * lanes apart, WaveGetLaneCount() in arithmetic, prefix sums over 32 lanes (culling and
 * compaction). Mali's subgroups are 16 wide (the G925: minSubgroupSize = maxSubgroupSize = 16),
 * so requiring 32 is not possible there, and such shaders get half the lanes they count on.
 *
 * The pass gives the shader virtual 32-lane subgroups made of the workgroup's invocations in
 * order (virtual subgroup = LocalInvocationIndex / 32, lane = LocalInvocationIndex % 32) and does
 * every subgroup operation through shared memory: each invocation writes its value into a
 * Workgroup array, a workgroup barrier, each reads the lane it wants, another barrier. Ballots,
 * votes and reductions are butterflies of such exchanges (5 steps), scans Hillis-Steele ones,
 * shuffles, broadcasts and quad operations one exchange; the ballot-mask operations
 * (BallotBitCount, BitExtract, FindLSB/MSB, InverseBallot) are the lane's own arithmetic.
 * SubgroupSize reads 32, SubgroupLocalInvocationId the lane, SubgroupId and NumSubgroups the
 * virtual ones, the Eq/Ge/Gt/Le/Lt masks the lane's (through Private variables set at the start
 * of the entry point).
 *
 * Only when that is exact, else the module is left as it is (*why says why):
 *   - one GLCompute entry point, its workgroup size a constant multiple of 32;
 *   - every subgroup operation in the entry point's function (no calls to make one), on 32-bit
 *     scalars or vectors (ints, floats, bools), at Subgroup scope;
 *   - every operation that exchanges values (all but the ballot-mask arithmetic) where the whole
 *     workgroup gets together (spirv_uniform.h: only uniform constructs around it, nobody gone
 *     before): the barriers need all invocations, and then all 32 lanes are active, as the
 *     operations' results assume. When the workgroup is one virtual subgroup (32 invocations),
 *     ballots, votes, broadcasts and reductions are uniform across it, so ifs and loops on them
 *     count as uniform;
 *   - the shared memory it adds (4 bytes per invocation and component) within the limit given.
 * BroadcastFirst where the workgroup may not be together is kept as it is (its result does not
 * depend on the subgroup size where it is used to make a value uniform or in a waterfall loop).
 * Modules that only use BroadcastFirst or Elect (vkd3d-proton's uniform descriptor index: any
 * size gives the same result) and modules that compare SubgroupSize (they adapt to the size, as
 * vkd3d-proton's own GDeflate shader does) are not touched either.
 * The output is a pure function of the input (new ids from the old bound, in order); running the
 * pass over its own output changes nothing.
 */
#ifndef VKB_SPIRV_WAVE32_H
#define VKB_SPIRV_WAVE32_H

#include <stddef.h>
#include <stdint.h>

/* What a module's subgroup operations count on */
enum {
    VKB_SG_NONE,      /* none */
    VKB_SG_ANY_SIZE,  /* only BroadcastFirst / Elect: the same result at any subgroup size */
    VKB_SG_ADAPTS,    /* compares SubgroupSize: made to work at other sizes */
    VKB_SG_FIXED,     /* the rest: may count on one size (32 for D3D12 shaders made for 32-wide waves) */
};

/* The class of a module (any stage); *compute (may be NULL): it has a GLCompute entry point */
int vkb_spv_subgroup_class(const uint32_t *code, size_t size, int *compute);

typedef struct vkb_wave32_stats {
    int cls;              /* VKB_SG_* */
    int compute;          /* a GLCompute module */
    unsigned ops;         /* subgroup operations done the 32-lane way */
    unsigned builtins;    /* subgroup built-in variables given their 32-lane values */
    unsigned barriers;    /* workgroup barriers put in */
    unsigned invocations; /* the workgroup's size */
    unsigned shared;      /* bytes of shared memory added */
    unsigned kept;        /* BroadcastFirst where the workgroup may not be together, kept as it was */
} vkb_wave32_stats;

/* 1: rewritten (*out malloc'd, *out_size bytes); 0: nothing to do (not compute, or not
 * VKB_SG_FIXED); -1: not possible (*why; the module is left as it is). shared_limit: the
 * device's maxComputeSharedMemorySize. st may be NULL. */
int vkb_spv_wave32(const uint32_t *code, size_t size, unsigned shared_limit, uint32_t **out, size_t *out_size, vkb_wave32_stats *st,
                   const char **why);

#endif
