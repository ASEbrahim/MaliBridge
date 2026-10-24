/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * The shm_barrier pass (server/spirv_shmbarrier.c) over SPIR-V modules:
 *   shmbarrier_corpus [-2] [-L] outdir file.spv... | @list (one path a line)    -2: level 2,
 *   -L: the lockstep rewrite (spirv_lockstep.c) first, "<name>: lockstep: <n> loop(s)" when it changes one
 * For each module one line:
 *   <name>: <c> control, <e> entry, <m> memory; <n> entry point(s)[; subgroup operations]
 *                                                    (rewritten: outdir/<name>)
 *   <name>: untouched
 *   <name>: not rewritten: <why>
 * and FAIL lines when a second run gives other bytes or the pass over its own output changes it;
 * then "TOTAL: <files> modules, <r> rewritten". Exit status 1 on a FAIL or an unreadable file.
 */
#include "../server/spirv_lockstep.h"
#include "../server/spirv_shmbarrier.h"

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

static int level = 1, lockstep = 0;

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
    if (lockstep) {
        uint32_t *lo = NULL;
        size_t ls = 0;
        unsigned nl = 0;
        const char *lwhy = NULL;
        int lr = vkb_spv_lockstep_loops(code, size, &lo, &ls, &nl, &lwhy);
        if (lr < 0) printf("%s: lockstep: not possible: %s\n", base, lwhy);
        if (lr > 0) {
            printf("%s: lockstep: %u loop(s)\n", base, nl);
            free(code);
            code = lo, size = ls;
        }
    }
    uint32_t *o = NULL, *o2 = NULL, *o3 = NULL;
    size_t os = 0, os2 = 0, os3 = 0;
    vkb_shmbarrier_stats st, st2, st3;
    const char *why;
    int bad = 0;
    int r = vkb_spv_shm_barrier(code, size, level, &o, &os, &st, &why);
    if (r < 0)
        printf("%s: not rewritten: %s\n", base, why);
    else if (r == 0)
        printf("%s: untouched\n", base);
    else {
        (*rewritten)++;
        if (level >= 2)
            printf("%s: %u control, %u inside, %u entry, %u memory; %u of %u construct(s) uniform; %u entry point(s)%s\n", base,
                   st.control, st.inside, st.entry, st.memory, st.uniform, st.constructs, st.entries, st.subgroup ? "; subgroup operations" : "");
        else
            printf("%s: %u control, %u entry, %u memory; %u entry point(s)%s\n", base, st.control, st.entry, st.memory, st.entries,
                   st.subgroup ? "; subgroup operations" : "");
        if (vkb_spv_shm_barrier(code, size, level, &o2, &os2, &st2, &why) != 1 || os2 != os || memcmp(o, o2, os))
            printf("FAIL %s: a second run differs\n", base), bad = 1;
        int r3 = vkb_spv_shm_barrier(o, os, level, &o3, &os3, &st3, &why);
        if (r3 != 0)
            printf("FAIL %s: the pass over its own output %s\n", base, r3 > 0 ? "changes it" : why), bad = 1;
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
    for (; argc > 1 && (!strcmp(argv[1], "-2") || !strcmp(argv[1], "-L")); argv++, argc--) {
        if (!strcmp(argv[1], "-2")) level = 2;
        else lockstep = 1;
    }
    if (argc < 3) {
        fprintf(stderr, "usage: shmbarrier_corpus [-2] [-L] outdir file.spv... | @list\n");
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
    printf("TOTAL: %u modules, %u rewritten\n", files, rewritten);
    return bad;
}
