/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * shm_barrier: workgroup barriers between shared-memory writes and the reads after them, in
 * compute shaders written for GPUs whose subgroups are 32 or 64 wide.
 *
 * Some games' compute shaders write groupshared memory and read what other invocations wrote
 * with no barrier in between, which works only where the threads involved run as one subgroup in
 * lockstep (a 32-wide wave on the GPUs they were made for). Mali runs 16-wide subgroups
 * (minSubgroupSize = maxSubgroupSize = 16 on the G925), so the reads see stale values: geometry
 * culled when it is visible, flicker (Resident Evil 2's occlusion culling, Mesa MR 23408). A
 * required subgroup size of 32 is not possible there, so the pass puts the barriers in instead.
 *
 * In each GLCompute entry point's function, walked in its structured order, an access to
 * Workgroup memory that conflicts with one since the last barrier (a read after a write, or a
 * write after a read: what lockstep execution would have ordered) gets:
 *   - OpControlBarrier(Workgroup, Workgroup, AcquireRelease | WorkgroupMemory) right before it,
 *     when the access is at the top level of the function (outside every selection and loop:
 *     every invocation of the workgroup gets there);
 *   - the same barrier at the entry of the top-level selection or loop the access is in
 *     (before the header's OpSelectionMerge, or before the branch into the loop header), when the
 *     access it conflicts with came before that construct;
 *   - otherwise (the conflict inside one construct, or after an invocation may have returned
 *     early inside a construct) only OpMemoryBarrier(Workgroup, AcquireRelease | WorkgroupMemory)
 *     before the access: allowed in any control flow; it orders the memory accesses and keeps the
 *     compiler from caching or moving them, but cannot make other subgroups catch up.
 * Level 2 (spirv_uniform.h) also counts selections and loops that the whole workgroup takes the
 * same way as uniform: barriers go inside them, and in such a loop between one iteration's
 * accesses and the next's.
 * A control barrier is never put in non-uniform control flow (which can hang the GPU). Reads and
 * writes are loads, stores, copies and atomics through Workgroup pointers, and calls to functions
 * that make them. A module whose top level is not structured as expected is left as it is.
 * The stats also say whether such a module uses subgroup operations (GroupNonUniform*, the
 * subgroup built-ins): those may count on a subgroup size this GPU does not have, which no
 * barrier fixes. D3D11 shaders (SM5) have none of their own.
 * Running the pass over its own output changes nothing, and the output is a pure function of the
 * input (new ids from the old bound, in order), so the driver's pipeline cache keeps hitting.
 */
#ifndef VKB_SPIRV_SHMBARRIER_H
#define VKB_SPIRV_SHMBARRIER_H

#include <stddef.h>
#include <stdint.h>

typedef struct vkb_shmbarrier_stats {
    unsigned control; /* control barriers before a top-level access */
    unsigned inside;  /* control barriers before an access inside a uniform construct (level 2) */
    unsigned entry;   /* control barriers at the entry of a construct */
    unsigned memory;  /* memory barriers where a control barrier is not allowed */
    unsigned entries; /* GLCompute entry points walked */
    unsigned constructs, uniform; /* level 2: the selections and loops of the functions walked, and the uniform ones */
    unsigned no_uniform;          /* level 2: functions whose uniformity could not be told (walked as level 1) */
    unsigned shared;   /* the module has a GLCompute entry point and Workgroup memory */
    unsigned subgroup; /* the module uses subgroup operations or built-ins (a size it may assume:
                          barriers do not help a shader that counts on 32 lanes in a subgroup) */
} vkb_shmbarrier_stats;

/* level 1: control barriers at the top level and at the entry of top-level constructs; level 2:
 * also inside selections and loops that are uniform across the workgroup (spirv_uniform.h), at
 * the entry of a construct nested in uniform ones, and, in a loop, between one iteration's
 * accesses and the next's (what lockstep execution ordered).
 * 1: rewritten (*out malloc'd, *out_size bytes), 0: nothing to do (nothing allocated), -1: not
 * possible (*why says why; the module is left as it is). st may be NULL. */
int vkb_spv_shm_barrier(const uint32_t *code, size_t size, int level, uint32_t **out, size_t *out_size, vkb_shmbarrier_stats *st,
                        const char **why);

#endif
