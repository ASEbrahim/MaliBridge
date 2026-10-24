/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * The SPIR-V lowering of version promotion (server/spirv_downgrade.c) over a corpus:
 *   downgrade_corpus <1.4|1.5> outdir file.spv...
 * For each module above the target: lowered, the new code written to outdir/<name> for spirv-val;
 * checked that the lowering of its own output changes nothing. One line per module -
 *   <name>: 1.<m> -> 1.<t>, <e> extension(s), <l> layer capabilities, <b> broadcast(s) made shuffles
 *   <name>: at or below the target
 * - then the totals.
 */
#include "../server/spirv_downgrade.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t *load(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint32_t *b = n > 0 ? malloc((size_t)n) : NULL;
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) free(b), b = NULL;
    fclose(f);
    *size = (size_t)n;
    return b;
}

int main(int argc, char **argv)
{
    unsigned tminor;
    if (argc < 4 || sscanf(argv[1], "1.%u", &tminor) != 1) {
        fprintf(stderr, "usage: downgrade_corpus <1.4|1.5> outdir file.spv...\n");
        return 2;
    }
    uint32_t target = 0x00010000u | (tminor << 8);
    const char *outdir = argv[2];
    unsigned files = 0, lowered = 0, failed = 0;
    for (int a = 3; a < argc; a++) {
        size_t size;
        uint32_t *code = load(argv[a], &size);
        const char *base = strrchr(argv[a], '/');
        base = base ? base + 1 : argv[a];
        if (!code) {
            printf("%s: cannot read\n", base);
            failed++;
            continue;
        }
        files++;
        uint32_t *o = NULL, *o2 = NULL;
        size_t os = 0, os2 = 0;
        vkb_downgrade_stats st;
        const char *why;
        int r = vkb_spv_downgrade(code, size, target, &o, &os, &st, &why);
        if (r < 0) {
            printf("%s: FAIL: not lowered: %s\n", base, why);
            failed++;
        } else if (r == 0) {
            printf("%s: at or below the target\n", base);
        } else {
            lowered++;
            if (vkb_spv_downgrade(o, os, target, &o2, &os2, NULL, &why) != 0) printf("%s: FAIL: its own output lowered again\n", base), failed++;
            char out[8192];
            snprintf(out, sizeof(out), "%s/%s", outdir, base);
            FILE *f = fopen(out, "wb");
            if (!f || fwrite(o, 1, os, f) != os) printf("%s: cannot write %s\n", base, out), failed++;
            if (f) fclose(f);
            printf("%s: 1.%u -> 1.%u, %u extension(s), %u layer capabilities, %u broadcast(s) made shuffles\n", base, (st.from >> 8) & 0xff,
                   tminor, st.extensions, st.layer_caps, st.broadcasts);
        }
        free(code), free(o), free(o2);
    }
    printf("TOTAL: %u modules, %u lowered\n", files, lowered);
    if (failed) printf("FAILED: %u\n", failed);
    return failed ? 1 : 0;
}
