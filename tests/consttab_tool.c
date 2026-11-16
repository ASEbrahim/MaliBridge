/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 * consttab_tool <in.spv> <out.spv>: PV-10's pass (server/spirv_consttab.c) on one module; prints what moved.
 * Exit 0 rewritten, 1 nothing to move (out not written), 2 error. */
#include "../server/spirv_consttab.h"

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    if (argc != 3) { fprintf(stderr, "usage: consttab_tool <in.spv> <out.spv>\n"); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    static uint32_t code[1 << 22];
    size_t n = fread(code, 1, sizeof(code), f);
    fclose(f);
    uint32_t *out;
    size_t osz;
    vkb_consttab_stats st;
    const char *why;
    int r = vkb_spv_consttab(code, n, &out, &osz, &st, &why);
    printf("%s: %s: %u moved (%u elements), %u access chains, %u left\n", argv[1], r > 0 ? "rewritten" : r == 0 ? "nothing to move" : why,
           st.vars, st.words, st.chains, st.skipped);
    if (r <= 0) return r == 0 ? 1 : 2;
    f = fopen(argv[2], "wb");
    if (!f || fwrite(out, 1, osz, f) != osz) { perror(argv[2]); return 2; }
    fclose(f);
    free(out);
    return 0;
}
