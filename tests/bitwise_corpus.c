/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The MK-03 pass (server/spirv_bitwise.c) over a corpus of SPIR-V modules:
 *   bitwise_corpus [-a] [-q] outdir file.spv... | @list (one path a line)
 * For each module: rewritten (with -a: every integer vector bitwise op, not only the pattern), the
 * new code written to outdir/<name> for spirv-val; checked that the pass over its own output finds
 * nothing (no pattern op left: deterministic and idempotent), that a second run gives the same
 * bytes, and that the bug's imitation finds nothing in it either. One line per rewritten module
 * (-q: none), then the totals: how many modules hold the pattern.
 */
#include "../server/spirv_bitwise.h"

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
        if (!strcmp(argv[a], "-a")) flags |= VKB_BITWISE_ALL;
        if (!strcmp(argv[a], "-q")) quiet = 1;
    }
    if (a + 1 >= argc) {
        fprintf(stderr, "usage: bitwise_corpus [-a] [-q] outdir file.spv...\n");
        return 2;
    }
    const char *outdir = argv[a++];
    unsigned files = 0, rewritten = 0, with_ivec = 0, errors = 0, failed = 0;
    unsigned long ops = 0, aox = 0, nots = 0, shifts = 0, shuf = 0, cons = 0, cnst = 0, casc = 0, all = 0, ivec = 0;
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
        vkb_bitwise_stats st, st2;
        const char *why;
        int r = vkb_spv_bitwise(code, size, flags, &o, &os, &st, &why);
        ivec += st.int_vector_ops;
        with_ivec += st.int_vector_ops > 0;
        const char *base = strrchr(path, '/');
        base = base ? base + 1 : path;
        if (r < 0) {
            printf("%s: not rewritten: %s\n", base, why);
            errors++;
        } else if (r == 1) {
            rewritten++;
            rw_in_bytes += size;
            out_bytes += os;
            ops += st.ops, aox += st.and_or_xor, nots += st.not_, shifts += st.shifts;
            shuf += st.by_shuffle, cons += st.by_construct, cnst += st.by_constant, casc += st.by_cascade, all += st.by_all;
            int bad = 0;
            /* the same input, the same bytes */
            if (vkb_spv_bitwise(code, size, flags, &o2, &os2, NULL, &why) != 1 || os2 != os || memcmp(o, o2, os))
                printf("%s: FAIL: a second run differs\n", base), bad = 1;
            /* nothing left for the pass or the bug */
            int again = vkb_spv_bitwise(o, os, flags & ~VKB_BITWISE_ALL, &o3, &os3, &st2, &why);
            if (again != 0) printf("%s: FAIL: the pass finds %u pattern op(s) in its own output (%d)\n", base, st2.ops, again), bad = 1;
            free(o3), o3 = NULL;
            again = vkb_spv_bitwise(o, os, VKB_BITWISE_IMITATE, &o3, &os3, &st2, &why);
            if (again != 0) printf("%s: FAIL: the imitation finds %u pattern op(s) in the fixed code\n", base, st2.ops), bad = 1;
            failed += bad;
            char path[8192];
            snprintf(path, sizeof(path), "%s/%s", outdir, base);
            FILE *f = fopen(path, "wb");
            if (!f || fwrite(o, 1, os, f) != os) printf("%s: cannot write %s\n", base, path), failed++;
            if (f) fclose(f);
            if (!quiet)
                printf("%s: %u op(s) (and/or/xor %u, not %u, shift %u; by shuffle %u, construct %u, constant %u, cascade %u%s), %zu -> %zu bytes\n",
                       base, st.ops, st.and_or_xor, st.not_, st.shifts, st.by_shuffle, st.by_construct, st.by_constant, st.by_cascade,
                       st.by_all ? ", -a" : "", size, os);
        } else {
            out_bytes += size;
        }
        free(code), free(o), free(o2), free(o3);
    }
    printf("TOTAL: %u modules, %u with integer vector bitwise/shift ops (%lu ops), %u rewritten (%.1f%%), %u not possible%s\n", files,
           with_ivec, ivec, rewritten, files ? 100.0 * rewritten / files : 0.0, errors, flags & VKB_BITWISE_ALL ? " [-a: every op]" : "");
    printf("TOTAL ops rewritten: %lu (and/or/xor %lu, not %lu, shift %lu; first pattern operand: shuffle %lu, construct/insert %lu, "
           "constant composite %lu, cascade %lu, -a %lu)\n", ops, aox, nots, shifts, shuf, cons, cnst, casc, all);
    printf("TOTAL size: rewritten modules %llu -> %llu bytes (+%.1f%%)\n", rw_in_bytes, rewritten ? out_bytes - (in_bytes - rw_in_bytes) : 0,
           rw_in_bytes ? 100.0 * ((double)(out_bytes - (in_bytes - rw_in_bytes)) - (double)rw_in_bytes) / (double)rw_in_bytes : 0.0);
    if (failed) printf("FAILED: %u\n", failed);
    return failed ? 1 : 0;
}
