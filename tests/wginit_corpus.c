/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * The wg_init pass (server/spirv_wginit.c) over a corpus of SPIR-V modules:
 *   wginit_corpus outdir file.spv...
 * For each module: rewritten, the new code written to outdir/<name> for spirv-val; checked that the
 * pass over its own output has nothing left to do. One line per module, then the totals.
 */
#include "../server/spirv_wginit.h"

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
    uint32_t *b = malloc(n > 0 ? (size_t)n : 4);
    *size = b && fread(b, 1, (size_t)n, f) == (size_t)n ? (size_t)n : 0;
    fclose(f);
    return b;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: wginit_corpus outdir file.spv...\n");
        return 2;
    }
    int bad = 0, changed = 0, left = 0;
    for (int i = 2; i < argc; i++) {
        size_t size;
        uint32_t *code = load(argv[i], &size), *out = NULL, *out2 = NULL;
        if (!code || !size) continue;
        const char *name = strrchr(argv[i], '/') ? strrchr(argv[i], '/') + 1 : argv[i];
        vkb_wginit_stats st, st2;
        const char *why;
        size_t os = 0, os2 = 0;
        int r = vkb_spv_wg_init(code, size, &out, &os, &st, &why);
        if (r < 0) {
            printf("%s: left as it is (%s)\n", name, why);
            left++;
        } else if (r > 0) {
            int r2 = vkb_spv_wg_init(out, os, &out2, &os2, &st2, &why);
            printf("%s: %u variable(s), %u array(s), %u single(s), %u store(s); %zu -> %zu bytes%s\n", name, st.vars, st.arrays, st.singles,
                   st.stores, size, os, r2 == 0 ? "" : " - SECOND RUN NOT CLEAN");
            if (r2 != 0) bad++;
            char path[1024];
            snprintf(path, sizeof(path), "%s/%s", argv[1], name);
            FILE *o = fopen(path, "wb");
            if (o) fwrite(out, 1, os, o), fclose(o);
            changed++;
        }
        free(code), free(out), free(out2);
    }
    printf("%s: %d rewritten, %d left as they were, %d bad\n", bad ? "FAILED" : "OK", changed, left, bad);
    return bad ? 1 : 0;
}
