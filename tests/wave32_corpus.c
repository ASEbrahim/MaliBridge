/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * The wave32 pass (server/spirv_wave32.c) over SPIR-V modules:
 *   wave32_corpus outdir file.spv... | @list (one path a line)
 * For each module one line:
 *   <name>: <class>; <o> op(s), <b> built-in(s), <n> barrier(s), <s> bytes shared; <i> invocations
 *                                                    (rewritten: outdir/<name>)
 *   <name>: <class>; untouched
 *   <name>: <class>; not rewritten: <why>
 * <class>: none, any size, adapts, fixed (spirv_wave32.h). FAIL lines when a second run gives
 * other bytes or the pass over its own output changes it (or its output still counts on a size);
 * then "TOTAL: <files> modules, <r> rewritten, <f> fixed, <a> adapt, <z> any size". Exit status
 * 1 on a FAIL or an unreadable file.
 */
#include "../server/spirv_wave32.h"

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

static const char *const cls_name[] = {"none", "any size", "adapts", "fixed"};
static unsigned counts[4];

static int one(const char *path, const char *outdir, unsigned *rewritten)
{
    size_t size;
    uint32_t *code = load(path, &size);
    if (!code) {
        fprintf(stderr, "%s: cannot read\n", path);
        return 1;
    }
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    uint32_t *o = NULL, *o2 = NULL, *o3 = NULL;
    size_t os = 0, os2 = 0, os3 = 0;
    vkb_wave32_stats st, st2, st3;
    const char *why;
    int bad = 0;
    int r = vkb_spv_wave32(code, size, 32768, &o, &os, &st, &why);
    counts[st.cls & 3]++;
    if (r < 0)
        printf("%s: %s; not rewritten: %s\n", base, cls_name[st.cls & 3], why);
    else if (r == 0)
        printf("%s: %s; untouched\n", base, cls_name[st.cls & 3]);
    else {
        (*rewritten)++;
        printf("%s: %s; %u op(s), %u built-in(s), %u barrier(s), %u bytes shared; %u invocations%s\n", base, cls_name[st.cls & 3], st.ops,
               st.builtins, st.barriers, st.shared, st.invocations, st.kept ? "; BroadcastFirst kept" : "");
        if (vkb_spv_wave32(code, size, 32768, &o2, &os2, &st2, &why) != 1 || os2 != os || memcmp(o, o2, os))
            printf("FAIL %s: a second run differs\n", base), bad = 1;
        int r3 = vkb_spv_wave32(o, os, 32768, &o3, &os3, &st3, &why);
        if (r3 != 0 || st3.cls >= VKB_SG_ADAPTS)
            printf("FAIL %s: the pass over its own output %s\n", base, r3 > 0 ? "changes it" : r3 < 0 ? why : "still counts on a size"), bad = 1;
        char p[4096];
        snprintf(p, sizeof(p), "%s/%s", outdir, base);
        FILE *f = fopen(p, "wb");
        if (!f || fwrite(o, 1, os, f) != os) printf("FAIL %s: cannot write %s\n", base, p), bad = 1;
        if (f) fclose(f);
    }
    free(o), free(o2), free(o3), free(code);
    return bad;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: wave32_corpus outdir file.spv... | @list\n");
        return 2;
    }
    unsigned files = 0, rewritten = 0;
    int bad = 0;
    char line[4096];
    for (int a = 2; a < argc; a++) {
        if (argv[a][0] == '@') {
            FILE *l = fopen(argv[a] + 1, "r");
            if (!l) {
                fprintf(stderr, "%s: cannot read\n", argv[a] + 1);
                bad = 1;
                continue;
            }
            while (fgets(line, sizeof(line), l)) {
                line[strcspn(line, "\n")] = 0;
                if (!*line) continue;
                files++;
                bad |= one(line, argv[1], &rewritten);
            }
            fclose(l);
        } else {
            files++;
            bad |= one(argv[a], argv[1], &rewritten);
        }
    }
    printf("TOTAL: %u modules, %u rewritten, %u fixed, %u adapt, %u any size\n", files, rewritten, counts[VKB_SG_FIXED],
           counts[VKB_SG_ADAPTS], counts[VKB_SG_ANY_SIZE]);
    return bad;
}
