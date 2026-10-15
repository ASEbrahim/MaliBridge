/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The MK-32 pass (server/spirv_clampzero.c) over a corpus of SPIR-V modules:
 *   clampzero_corpus [-a] [-b] [-q] outdir file.spv... | @list (one path a line)
 * For each module: rewritten (-a: every min/max/clamp with a constant zero limit, not only on a
 * constant-selected value; -b: the BitFieldInsert opaque zero, not the Private variable), the new
 * code written to outdir/<name> for spirv-val; checked that the pass over its own output finds
 * nothing (no constant zero limit left on a pattern op: deterministic and idempotent), that a
 * second run gives the same bytes, and that the bug's imitation finds nothing in it either. One
 * line per rewritten module (-q: none), then the totals: how many modules hold the pattern.
 */
#include "../server/spirv_clampzero.h"

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
    unsigned flags = 0;
    int quiet = 0, a = 1;
    for (; a < argc && argv[a][0] == '-'; a++) {
        if (!strcmp(argv[a], "-a")) flags |= VKB_CLAMP_ZERO_ALL;
        if (!strcmp(argv[a], "-b")) flags |= VKB_CLAMP_ZERO_BFI;
        if (!strcmp(argv[a], "-q")) quiet = 1;
    }
    if (a + 1 >= argc) {
        fprintf(stderr, "usage: clampzero_corpus [-a] [-b] [-q] outdir file.spv...\n");
        return 2;
    }
    const char *outdir = argv[a++];
    unsigned files = 0, rewritten = 0, with_zero = 0, with_spec = 0, errors = 0, failed = 0;
    unsigned long ops = 0, clamps = 0, minmax = 0, bysel = 0, byphi = 0, bymem = 0, byall = 0, zops = 0, szero = 0, funcs = 0;
    unsigned long long in_bytes = 0, out_bytes = 0, rw_in_bytes = 0;
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
        in_bytes += size;
        uint32_t *o = NULL, *o2 = NULL, *o3 = NULL;
        size_t os = 0, os2 = 0, os3 = 0;
        vkb_clampzero_stats st, st2;
        const char *why;
        int r = vkb_spv_clamp_zero(code, size, flags, &o, &os, &st, &why);
        zops += st.zero_ops, szero += st.spec_zero;
        with_zero += st.zero_ops > 0, with_spec += st.spec_zero > 0;
        const char *base = strrchr(path, '/');
        base = base ? base + 1 : path;
        if (r < 0) {
            printf("%s: not rewritten: %s\n", base, why);
            errors++;
        } else if (r == 1) {
            rewritten++;
            rw_in_bytes += size;
            out_bytes += os;
            ops += st.ops, clamps += st.clamps, minmax += st.minmax, funcs += st.functions;
            bysel += st.by_select, byphi += st.by_phi, bymem += st.by_memory, byall += st.by_all;
            int bad = 0;
            /* the same input, the same bytes */
            if (vkb_spv_clamp_zero(code, size, flags, &o2, &os2, NULL, &why) != 1 || os2 != os || memcmp(o, o2, os))
                printf("%s: FAIL: a second run differs\n", base), bad = 1;
            /* nothing left for the pass or the bug */
            int again = vkb_spv_clamp_zero(o, os, flags & ~VKB_CLAMP_ZERO_BFI, &o3, &os3, &st2, &why);
            if (again != 0) printf("%s: FAIL: the pass finds %u pattern op(s) in its own output (%d)\n", base, st2.ops, again), bad = 1;
            free(o3), o3 = NULL;
            again = vkb_spv_clamp_zero(o, os, VKB_CLAMP_ZERO_IMITATE, &o3, &os3, &st2, &why);
            if (again != 0) printf("%s: FAIL: the imitation finds %u pattern op(s) in the fixed code\n", base, st2.ops), bad = 1;
            failed += bad;
            char path[8192];
            snprintf(path, sizeof(path), "%s/%s", outdir, base);
            FILE *f = fopen(path, "wb");
            if (!f || fwrite(o, 1, os, f) != os) printf("%s: cannot write %s\n", base, path), failed++;
            if (f) fclose(f);
            if (!quiet)
                printf("%s: %u op(s) (clamp %u, min/max %u; value by select %u, phi %u, memory %u%s; %u function(s)), %u zero-limit op(s), "
                       "%zu -> %zu bytes\n",
                       base, st.ops, st.clamps, st.minmax, st.by_select, st.by_phi, st.by_memory, st.by_all ? ", -a" : "", st.functions,
                       st.zero_ops, size, os);
        } else {
            out_bytes += size;
        }
        free(code), free(o), free(o2), free(o3);
    }
    printf("TOTAL: %u modules, %u with min/max/clamp on a constant zero (%lu ops), %u rewritten (%.1f%%), %u not possible%s%s\n", files,
           with_zero, zops, rewritten, files ? 100.0 * rewritten / files : 0.0, errors, flags & VKB_CLAMP_ZERO_ALL ? " [-a: every op]" : "",
           flags & VKB_CLAMP_ZERO_BFI ? " [-b: BitFieldInsert zero]" : "");
    printf("TOTAL ops rewritten: %lu (clamp %lu, min/max %lu; value by select %lu, phi %lu, memory %lu, -a %lu) in %lu function(s); "
           "%lu spec-constant zero limit(s) in %u module(s) left\n", ops, clamps, minmax, bysel, byphi, bymem, byall, funcs, szero, with_spec);
    printf("TOTAL size: rewritten modules %llu -> %llu bytes (+%.1f%%)\n", rw_in_bytes, rewritten ? out_bytes - (in_bytes - rw_in_bytes) : 0,
           rw_in_bytes ? 100.0 * ((double)(out_bytes - (in_bytes - rw_in_bytes)) - (double)rw_in_bytes) / (double)rw_in_bytes : 0.0);
    if (failed) printf("FAILED: %u\n", failed);
    return failed ? 1 : 0;
}
