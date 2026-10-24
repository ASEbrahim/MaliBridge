/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * spv_effects (spirv_effects.h): two passes over the words. The first records each pointer type's
 * storage class and each result's type; the second looks at every write - OpStore, OpCopyMemory*,
 * the atomics that write, OpImageWrite - and calls it a store unless its pointer is plainly local
 * (Function, Private, Output, Workgroup). A pointer whose type the scan did not see counts as a
 * store, so does any atomic on one.
 */
#include "spirv_effects.h"

#include <stdlib.h>
#include <string.h>

#include "spirv_opclass.inc"

#define MAGIC 0x07230203u
#define BOUND_MAX (1u << 22)

enum {
    OP_EXECUTION_MODE = 16, OP_CAPABILITY = 17, OP_TYPE_POINTER = 32, OP_TYPE_FORWARD_POINTER = 39, OP_STORE = 62,
    OP_COPY_MEMORY = 63, OP_COPY_MEMORY_SIZED = 64, OP_IMAGE_WRITE = 99, OP_ATOMIC_STORE = 228, OP_ATOMIC_EXCHANGE = 229,
    OP_ATOMIC_XOR = 242, OP_ATOMIC_FLAG_TEST_AND_SET = 318, OP_ATOMIC_FLAG_CLEAR = 319, OP_ATOMIC_FMIN = 5614,
    OP_ATOMIC_FMAX = 5615, OP_ATOMIC_FADD = 6035,
};
enum { SC_INPUT = 1, SC_OUTPUT = 3, SC_WORKGROUP = 4, SC_PRIVATE = 6, SC_FUNCTION = 7, SC_UNKNOWN = 0xFFFFFFFFu };
enum { EXEC_MODE_XFB = 11, CAP_TRANSFORM_FEEDBACK = 53 };

static int local_class(uint32_t sc)
{
    return sc == SC_FUNCTION || sc == SC_PRIVATE || sc == SC_OUTPUT || sc == SC_WORKGROUP || sc == SC_INPUT;
}

uint32_t spv_effects(const uint32_t *code, size_t bytes)
{
    if (!code || bytes % 4 || bytes < 20 || code[0] != MAGIC || code[3] == 0 || code[3] > BOUND_MAX)
        return SPV_FX_BAD | SPV_FX_STORE | SPV_FX_XFB;
    size_t nw = bytes / 4;
    uint32_t bound = code[3];
    /* type_of[id]: the result type of id; sc_of[id]: the storage class of pointer type id */
    uint32_t *type_of = malloc((size_t)bound * sizeof(uint32_t)), *sc_of = malloc((size_t)bound * sizeof(uint32_t));
    if (!type_of || !sc_of) {
        free(type_of);
        free(sc_of);
        return SPV_FX_BAD | SPV_FX_STORE | SPV_FX_XFB;
    }
    memset(type_of, 0, (size_t)bound * sizeof(uint32_t));
    memset(sc_of, 0xFF, (size_t)bound * sizeof(uint32_t));
    uint32_t fx = 0;
    for (int pass = 0; pass < 2 && !(fx & SPV_FX_BAD); pass++) {
        for (size_t i = 5; i < nw;) {
            uint32_t op = code[i] & 0xFFFF, n = code[i] >> 16;
            if (!n || i + n > nw) {
                fx |= SPV_FX_BAD | SPV_FX_STORE | SPV_FX_XFB;
                break;
            }
            const uint32_t *w = code + i + 1;
            uint32_t nops = n - 1;
            i += n;
            if (pass == 0) {
                if ((op == OP_TYPE_POINTER || op == OP_TYPE_FORWARD_POINTER) && nops >= 2 && w[0] < bound) sc_of[w[0]] = w[1];
                else if (spv_op_class(op) == 2 && nops >= 2 && w[1] < bound) type_of[w[1]] = w[0];
                else if (op == OP_EXECUTION_MODE && nops >= 2 && w[1] == EXEC_MODE_XFB) fx |= SPV_FX_XFB;
                else if (op == OP_CAPABILITY && nops >= 1 && w[0] == CAP_TRANSFORM_FEEDBACK) fx |= SPV_FX_XFB;
                continue;
            }
            uint32_t ptr = 0;
            int atomic = 0;
            if (op == OP_STORE || op == OP_COPY_MEMORY || op == OP_COPY_MEMORY_SIZED || op == OP_ATOMIC_STORE || op == OP_ATOMIC_FLAG_CLEAR) {
                if (nops < 1) continue;
                ptr = w[0];
                atomic = op == OP_ATOMIC_STORE || op == OP_ATOMIC_FLAG_CLEAR;
            } else if ((op >= OP_ATOMIC_EXCHANGE && op <= OP_ATOMIC_XOR) || op == OP_ATOMIC_FLAG_TEST_AND_SET || op == OP_ATOMIC_FMIN ||
                       op == OP_ATOMIC_FMAX || op == OP_ATOMIC_FADD) {
                if (nops < 3) continue;
                ptr = w[2];
                atomic = 1;
            } else if (op == OP_IMAGE_WRITE) {
                fx |= SPV_FX_STORE;
                continue;
            } else {
                continue;
            }
            uint32_t pt = ptr < bound ? type_of[ptr] : 0;
            uint32_t sc = pt && pt < bound ? sc_of[pt] : SC_UNKNOWN;
            if (sc == SC_UNKNOWN || !local_class(sc) || (atomic && sc != SC_WORKGROUP && sc != SC_FUNCTION && sc != SC_PRIVATE))
                fx |= SPV_FX_STORE;
        }
    }
    free(type_of);
    free(sc_of);
    return fx;
}
