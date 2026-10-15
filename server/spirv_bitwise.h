/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * MK-03 (bitwise_scalar): Mali drivers r18p0-r53p0 miscompile an integer vector bitwise op whose
 * operand is a lane swizzle or a vector built lane by lane (Arm erratum 3843237; Dolphin's
 * BUG_BROKEN_VECTOR_BITWISE_AND, e.g. uvec2(x, 2u) | 1u). This pass rewrites such ops into one
 * scalar op per lane and an OpCompositeConstruct of the results. Pure SPIR-V, no Vulkan: the
 * server's erratum (server_errata_bitwise.c) and the host corpus tool (tests/bitwise_corpus.c)
 * share it.
 *
 * The pattern - an op the pass rewrites:
 *   op      OpBitwiseAnd / OpBitwiseOr / OpBitwiseXor / OpNot / OpShiftLeftLogical /
 *           OpShiftRightLogical / OpShiftRightArithmetic (the erratum's "bitwise op" and the shifts
 *           the known-issues survey adds), in function code,
 *   type    an integer vector result (any width, any signedness),
 *   operand at least one comes from
 *             OpVectorShuffle                         a swizzle (v.yx, v.wzyx, v.xxxx),
 *             OpCompositeConstruct / OpCompositeInsert a vector built lane by lane (uvec2(x, 2u),
 *                                                     OpCompositeExtract-built vectors),
 *             OpConstantComposite / OpSpecConstantComposite / OpSpecConstantOp of a shuffle,
 *                                                     construct or insert (uvec4(0xF0, 0x0F, ...),
 *                                                     the splat of a scalar operand: "| 1u"),
 *             an op this pass rewrote (now an OpCompositeConstruct itself),
 *           looked through OpCopyObject and OpBitcast (DXVK's float <-> uint reinterpretations).
 * Operands loaded, computed (OpIAdd, ...) or passed in otherwise do not make the pattern; with
 * VKB_BITWISE_ALL every integer vector op is rewritten (the plan's wider scope, for measuring).
 * Nothing else changes, and the output is a pure function of the input (new ids numbered from the
 * old bound, in order): a module without the pattern is handed back untouched, so the driver's
 * pipeline cache keeps its entries; one with it gets the same new code every launch.
 *
 * VKB_BITWISE_IMITATE turns the pass into a stand-in for the bug (host tests only): each pattern
 * operand of each pattern op is replaced by the splat of its lane 0 - the lane swizzle lost - so
 * a test can fail without the fix and pass with it (the fixed code has no pattern op left).
 */
#ifndef VKB_SPIRV_BITWISE_H
#define VKB_SPIRV_BITWISE_H

#include <stddef.h>
#include <stdint.h>

enum { VKB_BITWISE_ALL = 1, VKB_BITWISE_IMITATE = 2 };

typedef struct vkb_bitwise_stats {
    unsigned ops;                   /* ops rewritten (imitated) */
    unsigned and_or_xor, not_, shifts;
    /* the first operand kind that made each op a pattern op */
    unsigned by_shuffle, by_construct, by_constant, by_cascade, by_all;
    unsigned int_vector_ops;        /* integer vector ops of these kinds in the module, pattern or not */
} vkb_bitwise_stats;

/* 1: rewritten (*out malloc'd, *out_size bytes), 0: no pattern op (nothing allocated), -1: not
 * possible (*why says why; the module is left as it is). st may be NULL. */
int vkb_spv_bitwise(const uint32_t *code, size_t size, unsigned flags, uint32_t **out, size_t *out_size,
                    vkb_bitwise_stats *st, const char **why);

#endif
