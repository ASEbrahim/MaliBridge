/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * effects_corpus: server/spirv_effects.c alone (what the asynchronous pipeline compile asks of a
 * shader before it lets the pipeline's draws be skipped). The test shaders with a known answer:
 * the compute shaders store to storage buffers, the vertex and fragment shaders write only their
 * outputs; a truncated module and a store through a pointer of unknown type both count as stores.
 * Files given on the command line are scanned and their flags printed (no verdict).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "spirv_effects.h"

static const uint32_t comp_bitwise[] = {
#include "bitwise_comp.inc"
};
static const uint32_t comp_clampzero[] = {
#include "clampzero_comp.inc"
};
static const uint32_t frag_dynstate[] = {
#include "dynstate_frag.inc"
};
static const uint32_t vert_cullmode[] = {
#include "cullmode_vert.inc"
};
static const uint32_t vert_divisor[] = {
#include "divisor_vert.inc"
};
static const uint32_t frag_divisor[] = {
#include "divisor_frag.inc"
};

static int fails, checks;

static void expect(const char *name, const uint32_t *code, size_t bytes, uint32_t want)
{
    uint32_t got = spv_effects(code, bytes);
    checks++;
    if (got != want) fails++;
    printf("%s %s: effects 0x%x (wanted 0x%x)\n", got == want ? "PASS" : "FAIL", name, got, want);
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        FILE *f = fopen(argv[i], "rb");
        if (!f) { printf("SKIP %s: cannot open\n", argv[i]); continue; }
        static uint32_t buf[1 << 20];
        size_t n = fread(buf, 1, sizeof(buf), f);
        fclose(f);
        printf("FILE %s: effects 0x%x\n", argv[i], spv_effects(buf, n));
    }
    expect("bitwise.comp (storage buffer stores)", comp_bitwise, sizeof(comp_bitwise), SPV_FX_STORE);
    expect("clampzero.comp (storage buffer stores)", comp_clampzero, sizeof(comp_clampzero), SPV_FX_STORE);
    expect("dynstate.frag (colour output only)", frag_dynstate, sizeof(frag_dynstate), 0);
    expect("cullmode.vert (outputs only)", vert_cullmode, sizeof(vert_cullmode), 0);
    expect("divisor.vert (outputs only)", vert_divisor, sizeof(vert_divisor), 0);
    expect("divisor.frag (colour output only)", frag_divisor, sizeof(frag_divisor), 0);
    expect("truncated module", frag_dynstate, 16, SPV_FX_BAD | SPV_FX_STORE | SPV_FX_XFB);
    /* dynstate.frag with its OpStore's pointer renamed to the id bound (no id has it): unknown storage class */
    uint32_t *m = malloc(sizeof(frag_dynstate));
    memcpy(m, frag_dynstate, sizeof(frag_dynstate));
    int patched = 0;
    for (size_t i = 5; i < sizeof(frag_dynstate) / 4 && !patched;) {
        uint32_t op = m[i] & 0xFFFF, n = m[i] >> 16;
        if (!n) break;
        if (op == 62) {
            m[i + 1] = m[3];
            patched = 1;
        }
        i += n;
    }
    if (patched) expect("a store through a pointer of unknown type", m, sizeof(frag_dynstate), SPV_FX_STORE);
    free(m);
    /* the Xfb execution mode added to cullmode.vert */
    size_t nw = sizeof(vert_cullmode) / 4;
    uint32_t *x = malloc((nw + 3) * 4);
    size_t at = 5;
    while (at < nw && (vert_cullmode[at] & 0xFFFF) != 15) at += vert_cullmode[at] >> 16; /* after OpEntryPoint */
    if (at < nw) {
        at += vert_cullmode[at] >> 16;
        memcpy(x, vert_cullmode, at * 4);
        x[at] = (3u << 16) | 16; /* OpExecutionMode %main Xfb */
        size_t ep = 5;
        while ((vert_cullmode[ep] & 0xFFFF) != 15) ep += vert_cullmode[ep] >> 16;
        x[at + 1] = vert_cullmode[ep + 2];
        x[at + 2] = 11;
        memcpy(x + at + 3, vert_cullmode + at, (nw - at) * 4);
        expect("cullmode.vert with the Xfb execution mode", x, (nw + 3) * 4, SPV_FX_XFB);
    }
    free(x);
    if (fails) printf("FAIL: %d of %d checks failed\n", fails, checks);
    else printf("OK: %d of %d checks passed\n", checks, checks);
    return fails ? 1 : 0;
}
