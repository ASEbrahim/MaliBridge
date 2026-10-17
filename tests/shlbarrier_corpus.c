/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The shl_barrier pass (server/spirv_shlbarrier.c) over a corpus of SPIR-V modules:
 *   shlbarrier_corpus [-q] outdir file.spv... | @list (one path a line)
 * For each module: rewritten, the new code written to outdir/<name> for spirv-val; checked that a
 * second run gives the same bytes and that the pass over its own output changes nothing (every
 * shift it barriered counted as already barriered). One line per rewritten module (-q: none) -
 *   <name>: <s> scalar, <v> vector barrier(s); <w> not 32-bit; <a> already, <in> -> <out> bytes
 * - then the totals.
 */
#include "../server/spirv_shlbarrier.h"

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
    int quiet = 0, a = 1;
    for (; a < argc && argv[a][0] == '-'; a++)
        if (!strcmp(argv[a], "-q")) quiet = 1;
    if (a + 1 >= argc) {
        fprintf(stderr, "usage: shlbarrier_corpus [-q] outdir file.spv... | @list\n");
        return 2;
    }
    const char *outdir = argv[a++];
    unsigned files = 0, rewritten = 0, errors = 0, failed = 0;
    unsigned long scalar = 0, vector = 0, width = 0;
    unsigned long long rw_in = 0, rw_out = 0;
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
        } else if (a < argc) {
            path = argv[a++];
        } else
            break;
        size_t size;
        uint32_t *code = load(path, &size);
        if (!code) {
            fprintf(stderr, "%s: cannot read\n", path);
            failed++;
            continue;
        }
        files++;
        uint32_t *o = NULL, *o2 = NULL, *o3 = NULL;
        size_t os = 0, os2 = 0, os3 = 0;
        vkb_shlbarrier_stats st, st3;
        const char *why;
        int r = vkb_spv_shl_barrier(code, size, &o, &os, &st, &why);
        const char *base = strrchr(path, '/');
        base = base ? base + 1 : path;
        if (r < 0) {
            printf("%s: not rewritten: %s\n", base, why);
            errors++;
        } else if (r == 1) {
            rewritten++;
            rw_in += size, rw_out += os;
            scalar += st.scalar, vector += st.vector, width += st.skipped_width;
            int bad = 0;
            if (vkb_spv_shl_barrier(code, size, &o2, &os2, NULL, &why) != 1 || os2 != os || memcmp(o, o2, os))
                printf("%s: FAIL: a second run differs\n", base), bad = 1;
            int again = vkb_spv_shl_barrier(o, os, &o3, &os3, &st3, &why);
            if (again != 0 || st3.already != st.scalar + st.vector + st.already)
                printf("%s: FAIL: the pass over its own output: %d, %u barrier(s) more, %u already (want %u)\n", base, again,
                       st3.scalar + st3.vector, st3.already, st.scalar + st.vector + st.already), bad = 1;
            failed += bad;
            char out[8192];
            snprintf(out, sizeof(out), "%s/%s", outdir, base);
            FILE *f = fopen(out, "wb");
            if (!f || fwrite(o, 1, os, f) != os) printf("%s: cannot write %s\n", base, out), failed++;
            if (f) fclose(f);
            if (!quiet)
                printf("%s: %u scalar, %u vector barrier(s); %u not 32-bit; %u already, %zu -> %zu bytes\n", base, st.scalar, st.vector,
                       st.skipped_width, st.already, size, os);
        }
        free(code), free(o), free(o2), free(o3);
    }
    printf("TOTAL: %u modules, %u rewritten (%.1f%%), %u not possible; barriers: %lu scalar, %lu vector; %lu shift(s) not 32-bit left alone\n",
           files, rewritten, files ? 100.0 * rewritten / files : 0.0, errors, scalar, vector, width);
    printf("TOTAL size: rewritten modules %llu -> %llu bytes (+%.1f%%)\n", rw_in, rw_out, rw_in ? 100.0 * ((double)rw_out - (double)rw_in) / (double)rw_in : 0.0);
    if (failed) printf("FAILED: %u\n", failed);
    return failed ? 1 : 0;
}
