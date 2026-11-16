/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * The PV-01 SPIR-V pass (server/spirv_names.c) over a corpus of modules:
 *   pvnames_corpus [-q] outdir file.spv... | @list (one path a line)
 * For each module: the names dropped (the new code written to outdir/<name> for spirv-val), checked
 * that no OpName or OpMemberName is left, that nothing else changed (the instruction count fell by
 * exactly the names', every other word the same, in order), that a second pass and a second run
 * find/give the same, and that the bug's imitation finds nothing in the stripped code. Counts how
 * many modules hold an input and an output sharing an OpName (the pattern the imitation breaks).
 */
#include "../server/spirv_names.h"

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

/* The module's words with every OpName/OpMemberName removed, done here without the pass; and how many it had */
static size_t reference(const uint32_t *c, size_t n, uint32_t *o, unsigned *names)
{
    size_t k = 5;
    memcpy(o, c, 20);
    for (size_t i = 5; i < n;) {
        uint32_t op = c[i] & 0xffff, len = c[i] >> 16;
        if (!len || i + len > n) return 0;
        if (op == 5 || op == 6) (*names)++;
        else memcpy(o + k, c + i, (size_t)len * 4), k += len;
        i += len;
    }
    return k;
}

int main(int argc, char **argv)
{
    int quiet = 0, a = 1;
    if (a < argc && !strcmp(argv[a], "-q")) quiet = 1, a++;
    if (a + 1 >= argc) {
        fprintf(stderr, "usage: pvnames_corpus [-q] outdir file.spv...\n");
        return 2;
    }
    const char *outdir = argv[a++];
    unsigned files = 0, stripped = 0, unnamed = 0, shared = 0, failed = 0;
    unsigned long names_total = 0, in_bytes = 0, out_bytes = 0;
    FILE *list = NULL;
    char line[4096];
    for (;;) {
        const char *path;
        if (list) {
            if (!fgets(line, sizeof(line), list)) {
                fclose(list), list = NULL;
                continue;
            }
            line[strcspn(line, "\n")] = 0;
            path = line;
        } else if (a < argc && argv[a][0] == '@') {
            if (!(list = fopen(argv[a] + 1, "r"))) fprintf(stderr, "%s: cannot read\n", argv[a] + 1), failed++;
            a++;
            continue;
        } else if (a < argc)
            path = argv[a++];
        else
            break;
        size_t size;
        uint32_t *code = load(path, &size);
        const char *base = strrchr(path, '/');
        base = base ? base + 1 : path;
        if (!code) {
            fprintf(stderr, "%s: cannot read\n", path);
            failed++;
            continue;
        }
        files++;
        in_bytes += size;
        uint32_t *o = NULL, *o2 = NULL, *o3 = NULL, *ref = malloc(size);
        size_t os = 0, os2 = 0, os3 = 0;
        unsigned names = 0, bad = 0;
        size_t rn = ref ? reference(code, size / 4, ref, &names) : 0;
        int r = vkb_spv_strip_names(code, size, &o, &os);
        unsigned sh = 0;
        uint32_t *im = NULL;
        size_t ims = 0;
        if (vkb_spv_names_imitate(code, size, &im, &ims, &sh) == 1) shared++;
        free(im);
        if (!rn) printf("%s: FAIL: not readable as a module\n", base), bad = 1;
        else if (r < 0) printf("%s: FAIL: the pass refused a readable module\n", base), bad = 1;
        else if (r == 0) {
            if (names) printf("%s: FAIL: %u name(s) and the pass found none\n", base, names), bad = 1;
            unnamed++;
            out_bytes += size;
        } else {
            stripped++;
            names_total += names;
            out_bytes += os;
            if (os != rn * 4 || memcmp(o, ref, os)) printf("%s: FAIL: not exactly the module without its names\n", base), bad = 1;
            if (vkb_spv_strip_names(o, os, &o3, &os3) != 0) printf("%s: FAIL: names left in the pass's own output\n", base), bad = 1;
            if (vkb_spv_strip_names(code, size, &o2, &os2) != 1 || os2 != os || memcmp(o, o2, os))
                printf("%s: FAIL: a second run differs\n", base), bad = 1;
            free(o3), o3 = NULL;
            if (vkb_spv_names_imitate(o, os, &o3, &os3, NULL) != 0) printf("%s: FAIL: the imitation finds a shared name in the fixed code\n", base), bad = 1;
            char out[8192];
            snprintf(out, sizeof(out), "%s/%s", outdir, base);
            FILE *f = fopen(out, "wb");
            if (!f || fwrite(o, 1, os, f) != os) printf("%s: cannot write %s\n", base, out), bad = 1;
            if (f) fclose(f);
            if (!quiet) printf("%s: %u name(s) dropped, %zu -> %zu bytes%s\n", base, names, size, os, sh ? ", a shared name" : "");
        }
        failed += bad;
        free(code), free(o), free(o2), free(o3), free(ref);
    }
    printf("TOTAL: %u modules, %u with names (%lu names dropped, %lu -> %lu bytes), %u without, %u with an input and an output of one name; %u failed\n",
           files, stripped, names_total, in_bytes, out_bytes, unnamed, shared, failed);
    return failed ? 1 : 0;
}
