/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * Workgroup uniformity of one function's structured control flow (spirv_uniform.c), for
 * shm_barrier: which selections and loops every invocation of a workgroup that reaches them takes
 * the same way, so a workgroup barrier is allowed inside them.
 *
 * A value is uniform when it is the same in every invocation of the workgroup: constants and spec
 * constants; loads from Uniform, PushConstant and UniformConstant memory and from the WorkgroupId,
 * NumWorkgroups and WorkgroupSize built-ins, through uniform indices; loads from Private and
 * Function variables whose every store (in this function, and none in another) puts a uniform
 * value in at a uniform point through uniform indices (DXVK keeps its registers in such
 * variables); arithmetic, logic, comparisons, conversions and composites of uniform values; an
 * OpPhi of uniform values in the header of a uniform loop or the merge block of a uniform
 * construct. Everything else (invocation ids, shared and storage memory, images, atomics,
 * subgroup operations, calls) is not.
 *
 * A selection is uniform when its condition (or selector) is. A loop is uniform when nothing
 * inside it returns or stops the invocation, and every branch to its merge block or back to its
 * header comes from a point inside the loop that every iteration's invocations reach together
 * (only uniform constructs between it and the loop), on a uniform condition: then every invocation
 * runs the same iterations. Loop counters are uniform by the same argument, so the analysis takes
 * the greatest consistent answer (everything uniform first, then lowered until nothing changes).
 */
#ifndef VKB_SPIRV_UNIFORM_H
#define VKB_SPIRV_UNIFORM_H

#include <stddef.h>
#include <stdint.h>

typedef struct vkb_uniform vkb_uniform;

/* The analysis of the function at code[start, end) of the module code[0, n) (n words, ids below
 * bound). NULL when it cannot tell (control flow not structured as expected, out of memory). */
vkb_uniform *vkb_spv_uniform(const uint32_t *code, size_t n, size_t start, size_t end, uint32_t bound);

/* The same, with the results in `given` (indexed by id, bound entries; NULL: none) taken as
 * uniform wherever they are computed: the caller vouches for them (wave32: an emulated subgroup
 * operation's result is the same for the whole workgroup when the workgroup is one subgroup, and
 * the caller checks afterwards that every such operation is reached uniformly). */
vkb_uniform *vkb_spv_uniform_given(const uint32_t *code, size_t n, size_t start, size_t end, uint32_t bound, const uint8_t *given);

/* The construct (selection or loop) whose header block is `label`: 1 uniform, 0 not or no such
 * construct. For a loop, *from and *to (when not NULL) are the input offsets of its header's
 * OpLabel and of its merge block's OpLabel. */
int vkb_uniform_construct(const vkb_uniform *u, uint32_t label, size_t *from, size_t *to);

/* Constructs found, and how many of them are uniform */
void vkb_uniform_counts(const vkb_uniform *u, unsigned *constructs, unsigned *uniform);

void vkb_uniform_free(vkb_uniform *u);

#endif
