/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * MK-47 (split_varying): Mali drivers (seen on Mali-G68 r38p1 JM and Mali-G925 r49p1 CSF) read a
 * fragment input wrong when it sits at a Component other than 0 of a Location (OpDecorate
 * Component 3) and the stage before writes that Location as ONE whole vector (no Component
 * decoration). Components 0-1 read right, component 3 reads 0 or garbage (tests/splitvary_check.c on
 * a Galaxy A35, Mali-G68 r38p1: 'vec4-split', 'vec4-q' and 'zink-split' wrong; the same pair
 * with a merged vec4 input right; a vertex stage split the same way right). Zink's fixed-function
 * texturing makes exactly this pair: its vertex shader writes the texture coordinate as one vec4,
 * its fragment shader reads (s, t) as a vec2 at component 0 and q as a float at component 3 for
 * texture2DProj - so q is wrong, s/q and t/q land on the wrong texels, and GoldSrc's menu text
 * (Half-Life, a player log) drew as fragments.
 *
 * The pass, on a fragment shader paired with the outputs of the last pre-rasterization stage it
 * is linked with: the inputs that share a Location the previous stage writes as one whole vector
 * become ONE input of that vector's type at component 0, and each load of a piece becomes a load
 * of the whole input and an OpCompositeExtract (scalar) or OpVectorShuffle (vector) of its
 * components. The interface then matches the previous stage's exactly, which Mali reads right.
 * A Location is merged only when all of these hold (anything else is left as it is):
 *   - at least one piece is at a Component above 0,
 *   - the previous stage writes it as a plain variable of a 32-bit int/float scalar or vector, no
 *     Component decoration, covering every component the pieces read,
 *   - every piece is a plain Input variable (not arrayed, not a block member) of a 32-bit scalar
 *     or vector of that same component type, with the same interpolation decorations
 *     (Flat/NoPerspective/Centroid/Sample) and no other decoration than Location, Component,
 *     those and RelaxedPrecision,
 *   - the pieces are used only by OpLoad (and named in OpEntryPoint / OpName / OpDecorate); only id operands
 *     count as uses - a literal operand (an Offset, a component index, an OpExtInst number) that happens to
 *     equal a piece's id does not (Zink's GL_REPLACE shader: q is %20 and the push constants say Offset 20),
 *   - the module has exactly one entry point, a Fragment one.
 * Nothing else changes, and the output is a pure function of the input and the previous stage's
 * outputs (new ids numbered from the old bound, in order).
 *
 * VKB_SPLITVARY_IMITATE (host tests only): a stand-in for the bug instead of the fix - each load of
 * a piece at a Component above 0 in such a Location gives 0 (OpConstantNull), which is what the
 * Mali-G68 r38p1 read for Zink's q. The fixed code has no piece at a Component above 0 left there.
 */
#ifndef VKB_SPIRV_SPLITVARY_H
#define VKB_SPIRV_SPLITVARY_H

#include <stddef.h>
#include <stdint.h>

enum { VKB_SPLITVARY_IMITATE = 1 };

#define VKB_SV_LOCATIONS 32

/* What a module's interface says, from one scan of its words (no allocation) */
typedef struct vkb_sv_info {
    int model;              /* execution model of its only entry point; -1: none or more than one */
    /* pre-rasterization stage (Vertex, TessellationEvaluation, Geometry): Locations written as one
     * plain whole 32-bit scalar/vector Output variable with no Component decoration */
    uint32_t whole;         /* bit L */
    uint8_t width[VKB_SV_LOCATIONS];  /* components (1-4) */
    uint8_t ctype[VKB_SV_LOCATIONS];  /* VKB_SV_FLOAT, _SINT, _UINT */
    /* fragment stage: Locations with an Input variable at a Component above 0 */
    uint32_t split;         /* bit L */
} vkb_sv_info;

enum { VKB_SV_FLOAT = 1, VKB_SV_SINT = 2, VKB_SV_UINT = 3 };

/* Scans a module: 0 with *info filled, -1 when it is not SPIR-V. A module whose interface is too
 * large to follow (more than a few hundred interface variables or types) gives model -1. */
int vkb_spv_sv_scan(const uint32_t *code, size_t size, vkb_sv_info *info);

typedef struct vkb_splitvary_stats {
    uint32_t locations;     /* bit L: merged (imitated) */
    unsigned pieces;        /* input variables merged */
    unsigned loads;         /* loads rewritten */
    uint32_t skipped;       /* bit L: a split Location the previous stage writes whole, left (a rule above) */
    char why[224];          /* skipped: the first such Location, its input and the rule it breaks (*why points here) */
} vkb_splitvary_stats;

/* 1: rewritten (*out malloc'd, *out_size bytes), 0: nothing to merge (nothing allocated),
 * -1: not possible (*why says why; nothing allocated). prev: the previous stage's scan. st may be
 * NULL (then *why is a short static text; with st it names the Location, component and rule, in
 * st->why, valid while st is). */
int vkb_spv_split_varying(const uint32_t *code, size_t size, const vkb_sv_info *prev, unsigned flags, uint32_t **out,
                          size_t *out_size, vkb_splitvary_stats *st, const char **why);

#endif
