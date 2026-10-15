/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * MK-03: integer vector bitwise ops on a swizzle, scalarised (see spirv_bitwise.h). One pass over
 * the words to find the pattern (most modules end there, untouched), a second to write the new
 * code: for  %r = OpBitwiseOr %v2uint %a %b
 *            %a0 = OpCompositeExtract %uint %a 0   %b0 = OpCompositeExtract %uint %b 0
 *            %r0 = OpBitwiseOr %uint %a0 %b0       (and so for lane 1)
 *            %r = OpCompositeConstruct %v2uint %r0 %r1
 * %r keeps its id (and so its decorations and uses).
 */
#include "spirv_bitwise.h"

#include <stdlib.h>
#include <string.h>

#include "spirv_opclass.inc"

enum {
    OP_TYPE_INT = 21, OP_TYPE_VECTOR = 23, OP_CONSTANT_COMPOSITE = 44, OP_SPEC_CONSTANT_COMPOSITE = 51,
    OP_SPEC_CONSTANT_OP = 52, OP_FUNCTION = 54, OP_FUNCTION_END = 56, OP_VECTOR_SHUFFLE = 79,
    OP_COMPOSITE_CONSTRUCT = 80, OP_COMPOSITE_EXTRACT = 81, OP_COMPOSITE_INSERT = 82, OP_COPY_OBJECT = 83,
    OP_BITCAST = 124, OP_SHR_LOGICAL = 194, OP_SHR_ARITH = 195, OP_SHL_LOGICAL = 196, OP_OR = 197, OP_XOR = 198,
    OP_AND = 199, OP_NOT = 200,
};

#define MAX_BOUND (1u << 22)
#define MAX_LANES 16

/* What the pass knows of an id */
enum { K_NONE, K_SHUFFLE, K_CONSTRUCT, K_CONSTANT, K_CASCADE, K_LOOK }; /* K_LOOK: copy/bitcast of w */
typedef struct {
    uint32_t type;  /* result type; for OpTypeVector its component type */
    uint32_t from;  /* K_LOOK: the operand looked through */
    uint16_t op;    /* the defining opcode */
    uint8_t lanes;  /* OpTypeVector: its component count */
    uint8_t kind;   /* K_* */
    uint8_t is_int; /* OpTypeInt */
} id_info;

static int target_op(uint32_t op)
{
    return (op >= OP_SHR_LOGICAL && op <= OP_NOT);
}

static int nsrc(uint32_t op)
{
    return op == OP_NOT ? 1 : 2;
}

/* The kind of pattern operand an id is (K_NONE: not one), through copies and bitcasts. */
static int src_kind(const id_info *ii, uint32_t bound, uint32_t id)
{
    for (int depth = 0; depth < 8 && id && id < bound; depth++) {
        const id_info *x = &ii[id];
        if (x->kind != K_LOOK) return x->kind;
        id = x->from;
    }
    return K_NONE;
}

/* The integer vector type's lane count, or 0 */
static unsigned int_vector(const id_info *ii, uint32_t bound, uint32_t type, uint32_t *comp)
{
    if (!type || type >= bound || ii[type].op != OP_TYPE_VECTOR) return 0;
    uint32_t c = ii[type].type;
    if (!c || c >= bound || !ii[c].is_int) return 0;
    if (comp) *comp = c;
    return ii[type].lanes;
}

int vkb_spv_bitwise(const uint32_t *code, size_t size, unsigned flags, uint32_t **out, size_t *out_size,
                    vkb_bitwise_stats *st, const char **why)
{
    vkb_bitwise_stats local;
    if (!st) st = &local;
    memset(st, 0, sizeof(*st));
    *why = NULL;
    size_t n = size / 4;
    if (n < 5 || (size & 3) || code[0] != 0x07230203u) return 0;
    uint32_t bound = code[3];
    if (!bound || bound > MAX_BOUND) {
        *why = "id bound out of range";
        return -1;
    }
    int imitate = flags & VKB_BITWISE_IMITATE;

    /* Pass 1: what each id is, and which ops make the pattern (marked by word index). */
    id_info *ii = calloc(bound, sizeof(*ii));
    uint8_t *hit = NULL; /* per word: 1 at a pattern op's first word */
    if (!ii) {
        *why = "out of memory";
        return -1;
    }
    unsigned nhit = 0;
    size_t grow = 0; /* words the rewrite adds */
    int in_func = 0, rc = 0;
    for (size_t i = 5; i < n;) {
        uint32_t op = code[i] & 0xffff, len = code[i] >> 16;
        if (!len || i + len > n) {
            *why = "malformed instruction stream";
            rc = -1;
            goto done;
        }
        const uint32_t *w = code + i + 1;
        uint32_t nw = len - 1;
        int cls = spv_op_class(op);
        uint32_t rid = cls == 1 && nw >= 1 ? w[0] : cls == 2 && nw >= 2 ? w[1] : 0;
        if (rid >= bound) {
            *why = "id past the bound";
            rc = -1;
            goto done;
        }
        if (op == OP_FUNCTION) in_func = 1;
        if (op == OP_FUNCTION_END) in_func = 0;
        if (rid) {
            id_info *x = &ii[rid];
            x->op = (uint16_t)op;
            if (cls == 2) x->type = w[0];
            switch (op) {
            case OP_TYPE_INT: x->is_int = 1; break;
            case OP_TYPE_VECTOR:
                if (nw >= 3) x->type = w[1], x->lanes = (uint8_t)(w[2] <= MAX_LANES ? w[2] : 0);
                break;
            case OP_VECTOR_SHUFFLE: x->kind = K_SHUFFLE; break;
            case OP_COMPOSITE_CONSTRUCT: case OP_COMPOSITE_INSERT: x->kind = K_CONSTRUCT; break;
            case OP_CONSTANT_COMPOSITE: case OP_SPEC_CONSTANT_COMPOSITE: x->kind = K_CONSTANT; break;
            case OP_SPEC_CONSTANT_OP:
                if (nw >= 3 && (w[2] == OP_VECTOR_SHUFFLE || w[2] == OP_COMPOSITE_INSERT)) x->kind = K_CONSTANT;
                break;
            case OP_COPY_OBJECT: case OP_BITCAST:
                if (nw >= 3) x->kind = K_LOOK, x->from = w[2];
                break;
            }
        }
        if (target_op(op) && in_func && nw >= 2 + (uint32_t)nsrc(op)) {
            uint32_t comp;
            unsigned lanes = int_vector(ii, bound, w[0], &comp);
            if (lanes) {
                st->int_vector_ops++;
                int k = 0;
                for (int s = 0; s < nsrc(op) && !k; s++) k = src_kind(ii, bound, w[2 + s]);
                if (!k && (flags & VKB_BITWISE_ALL)) k = -1;
                if (k) {
                    if (!hit && !(hit = calloc(n, 1))) {
                        *why = "out of memory";
                        rc = -1;
                        goto done;
                    }
                    hit[i] = 1;
                    nhit++;
                    st->ops++;
                    if (op == OP_NOT) st->not_++;
                    else if (op <= OP_SHL_LOGICAL) st->shifts++;
                    else st->and_or_xor++;
                    if (k == K_SHUFFLE) st->by_shuffle++;
                    else if (k == K_CONSTRUCT) st->by_construct++;
                    else if (k == K_CONSTANT) st->by_constant++;
                    else if (k == K_CASCADE) st->by_cascade++;
                    else st->by_all++;
                    if (!imitate) ii[w[1]].kind = K_CASCADE; /* now an OpCompositeConstruct */
                    /* fix: per lane an extract per operand and the op, then the construct;
                     * imitation: per pattern operand an extract and a splat */
                    size_t ns = (size_t)nsrc(op);
                    grow += imitate ? ns * (5 + 3 + lanes) : lanes * (5 * ns + 3 + ns) + 3 + lanes - len;
                }
            }
        }
        i += len;
    }
    if (!nhit) goto done;

    /* Pass 2: the new code. */
    uint32_t *o = malloc((n + grow) * 4);
    if (!o) {
        *why = "out of memory";
        rc = -1;
        goto done;
    }
    memcpy(o, code, 20);
    size_t p = 5;
    uint32_t next = bound;
#define EMIT(op_, len_) (o[p++] = ((uint32_t)(len_) << 16) | (op_))
    for (size_t i = 5; i < n;) {
        uint32_t op = code[i] & 0xffff, len = code[i] >> 16;
        if (!hit[i]) {
            memcpy(o + p, code + i, (size_t)len * 4);
            p += len;
            i += len;
            continue;
        }
        const uint32_t *w = code + i + 1;
        uint32_t rtype = w[0], rid = w[1], comp = 0;
        unsigned lanes = int_vector(ii, bound, rtype, &comp);
        int ns = nsrc(op);
        uint32_t src[2] = {w[2], ns > 1 ? w[3] : 0}, scomp[2] = {0, 0};
        for (int s = 0; s < ns; s++) {
            uint32_t t = src[s] < bound ? ii[src[s]].type : 0;
            if (int_vector(ii, bound, t, &scomp[s]) != lanes) {
                *why = "an operand's type is not an integer vector of the result's size";
                free(o);
                rc = -1;
                goto done;
            }
        }
        if (imitate) {
            /* the lane swizzle lost: each pattern operand becomes the splat of its lane 0 */
            uint32_t nsrc_ids[2] = {src[0], src[1]};
            for (int s = 0; s < ns; s++) {
                if (!src_kind(ii, bound, src[s])) continue;
                uint32_t x0 = next++, splat = next++;
                EMIT(OP_COMPOSITE_EXTRACT, 5);
                o[p++] = scomp[s], o[p++] = x0, o[p++] = src[s], o[p++] = 0;
                EMIT(OP_COMPOSITE_CONSTRUCT, 3 + lanes);
                o[p++] = ii[src[s]].type, o[p++] = splat;
                for (unsigned l = 0; l < lanes; l++) o[p++] = x0;
                nsrc_ids[s] = splat;
            }
            memcpy(o + p, code + i, (size_t)len * 4);
            for (int s = 0; s < ns; s++) o[p + 3 + s] = nsrc_ids[s];
            p += len;
        } else {
            uint32_t res[MAX_LANES];
            for (unsigned l = 0; l < lanes; l++) {
                uint32_t lane[2];
                for (int s = 0; s < ns; s++) {
                    lane[s] = next++;
                    EMIT(OP_COMPOSITE_EXTRACT, 5);
                    o[p++] = scomp[s], o[p++] = lane[s], o[p++] = src[s], o[p++] = l;
                }
                res[l] = next++;
                EMIT(op, 3 + ns);
                o[p++] = comp, o[p++] = res[l];
                for (int s = 0; s < ns; s++) o[p++] = lane[s];
            }
            EMIT(OP_COMPOSITE_CONSTRUCT, 3 + lanes);
            o[p++] = rtype, o[p++] = rid;
            for (unsigned l = 0; l < lanes; l++) o[p++] = res[l];
        }
        i += len;
    }
#undef EMIT
    if (next > MAX_BOUND) {
        *why = "the new ids pass the bound limit";
        free(o);
        rc = -1;
        goto done;
    }
    o[3] = next;
    *out = o;
    *out_size = p * 4;
    rc = 1;
done:
    free(ii);
    free(hit);
    return rc;
}
