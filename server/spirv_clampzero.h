/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * MK-32 (clamp_zero): Mali drivers r38p1-r49p0 and r50p0-r51p0 (Arm erratum 3779215, fixed in
 * r49p1 and r52p0) miscompile a clamp whose limit is a constant zero when the clamped value is
 * selected at run time from compile-time constants: the clamp becomes a min or a max. Godot's
 * fullscreen-triangle vertex shaders hit it (clamp(base_arr[gl_VertexIndex], vec2(0), vec2(1)),
 * Godot PRs #92817 and #109994). This pass gives such ops a zero the compiler cannot see as a
 * constant. Pure SPIR-V, no Vulkan: the server's erratum (server_errata_clampzero.c) and the host
 * corpus tool (tests/clampzero_corpus.c) share it.
 *
 * The pattern - an op the pass rewrites:
 *   op      GLSL.std.450 FClamp / NClamp / SClamp / UClamp with a zero minVal or maxVal, or
 *           FMin / FMax / NMin / NMax / SMin / SMax / UMin / UMax with a zero operand,
 *   zero    an OpConstant whose value is all zero bits (+0.0, 0), an OpConstantNull, or an
 *           OpConstantComposite of such (vec2(0.0)); a spec constant is not taken (it may be
 *           specialised to something else; counted in spec_zero),
 *   value   the clamped operand (for min/max the other one) is "constant-selected": an OpSelect
 *           or OpPhi whose values are all constants or constant-selected, a load from a
 *           Function, Private or Output variable that only ever holds such values (its
 *           initializer and every store; no other use of a pointer into it: a constant array
 *           indexed at run time, glslang's if/else locals, gl_Position read back), or an
 *           arithmetic, conversion, composite or GLSL.std.450 op over constants and at least one
 *           constant-selected value.
 * With VKB_CLAMP_ZERO_ALL every min/max/clamp with a zero limit is rewritten, whatever its value
 * (the bisect kit's 14-opaquezero: the G925 LUT compile-hang A/B).
 *
 * The opaque zero (default): a Private variable per scalar type, stored with 0 at the top of
 * every entry point and loaded at the top of each function that needs it (vectors: an
 * OpCompositeConstruct of the load). This is the research's first choice and byte-for-byte the
 * shape of the kit's 14-opaquezero, so a device result of that variant carries over to the pass.
 * VKB_CLAMP_ZERO_BFI: bitcast(OpBitFieldInsert(bitcast(0), 0, 0, 0)) instead (the kit's 14a,
 * leegao's barrier for an older Mali constant-folding bug), for 32-bit types; other widths keep
 * the Private form.
 *
 * Nothing else changes, and the output is a pure function of the input (new ids numbered from the
 * old bound, in order): a module without the pattern is handed back untouched.
 *
 * VKB_CLAMP_ZERO_IMITATE turns the pass into a stand-in for the bug (host tests only): each
 * pattern op loses its zero limit - clamp(x, 0, hi) becomes min(x, hi), clamp(x, lo, 0) max(x, lo),
 * max(x, 0) and min(x, 0) a copy of x - so a test can fail without the fix and pass with it (the
 * fixed code has no constant zero limit left).
 */
#ifndef VKB_SPIRV_CLAMPZERO_H
#define VKB_SPIRV_CLAMPZERO_H

#include <stddef.h>
#include <stdint.h>

enum { VKB_CLAMP_ZERO_ALL = 1, VKB_CLAMP_ZERO_BFI = 2, VKB_CLAMP_ZERO_IMITATE = 4 };

typedef struct vkb_clampzero_stats {
    unsigned ops;                  /* ops rewritten (imitated) */
    unsigned clamps, minmax;
    /* where the value's constant selection came from (the first one found) */
    unsigned by_select, by_phi, by_memory, by_all;
    unsigned zero_ops;             /* min/max/clamp ops with a constant zero limit, pattern or not */
    unsigned spec_zero;            /* ... with a spec-constant limit whose default is zero (left) */
    unsigned functions;            /* functions given an opaque zero */
} vkb_clampzero_stats;

/* 1: rewritten (*out malloc'd, *out_size bytes), 0: no pattern op (nothing allocated), -1: not
 * possible (*why says why; the module is left as it is). st may be NULL. */
int vkb_spv_clamp_zero(const uint32_t *code, size_t size, unsigned flags, uint32_t **out, size_t *out_size,
                       vkb_clampzero_stats *st, const char **why);

#endif
