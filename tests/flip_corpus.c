/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The MK-02 pass (server/spirv_flip.c) over a corpus of SPIR-V modules:
 *   flip_corpus [-q] outdir file.spv... | @list (one path a line)
 * For each vertex, tessellation evaluation and geometry entry point with a gl_Position: flipped
 * for every viewport (written to outdir/<name>.<n>.all.spv) and, when the stage writes
 * ViewportIndex, for viewport 1 only (picked at run time: .<n>.vp1.spv), for spirv-val; each run
 * twice, the bytes compared (the output is a function of the input). Other stages and modules
 * without gl_Position must come back untouched. One line per flipped entry point (-q: none), then
 * the totals.
 */
#include "../server/spirv_flip.h"

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

static int save(const char *path, const uint32_t *code, size_t size)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    int ok = fwrite(code, 1, size, f) == size;
    return fclose(f) || !ok ? -1 : 0;
}

static int modules, entries, flipped, conditional, failed;

static int one(const char *outdir, const char *path, int quiet)
{
    size_t size;
    uint32_t *code = load(path, &size);
    if (!code || size < 20 || code[0] != 0x07230203u) {
        printf("FAIL %s: not SPIR-V\n", path);
        free(code);
        return 1;
    }
    modules++;
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    int bad = 0, k = 0;
    for (size_t pos = 5; pos < size / 4;) {
        uint32_t n = code[pos] >> 16, op = code[pos] & 0xffff;
        if (!n || pos + n > size / 4) break;
        if (op == 54) break; /* OpFunction: the entry points are behind */
        if (op != 15 || n < 4) {
            pos += n;
            continue;
        }
        uint32_t model = code[pos + 1];
        const char *name = (const char *)&code[pos + 3];
        pos += n;
        k++;
        if (model != 0 && model != 2 && model != 3) continue; /* VS, TES, GS */
        entries++;
        for (int vari = 0; vari < 2; vari++) {
            uint32_t *out = NULL, *out2 = NULL;
            size_t os = 0, os2 = 0;
            vkb_flip_info info;
            const char *why = NULL;
            uint32_t mask = vari ? 2u : 0xffffffffu;
            int r = vkb_spv_flip(code, size, model, name, mask, 32, &out, &os, &info, &why);
            if (vari && !info.writes_vpindex) {
                free(out);
                break;
            }
            if (r < 0) {
                printf("FAIL %s %s: %s\n", base, name, why);
                failed++, bad = 1;
                break;
            }
            if (r == 0) {
                if (info.position) printf("FAIL %s %s: a gl_Position but nothing flipped\n", base, name), bad = 1, failed++;
                break;
            }
            int r2 = vkb_spv_flip(code, size, model, name, mask, 32, &out2, &os2, NULL, NULL);
            if (r2 != 1 || os2 != os || memcmp(out, out2, os)) printf("FAIL %s %s: a second run differs\n", base, name), bad = 1, failed++;
            char o[4096];
            snprintf(o, sizeof(o), "%s/%s.%d.%s.spv", outdir, base, k, vari ? "vp1" : "all");
            if (save(o, out, os)) printf("FAIL %s: cannot write\n", o), bad = 1;
            if (!vari) flipped++;
            else conditional++;
            if (!quiet)
                printf("flipped %s %s (%s): %u site(s)%s%s%s, %zu -> %zu bytes\n", base, name, model == 0 ? "vertex" : model == 2 ? "tess eval" : "geometry",
                       info.sites, info.in_block ? ", gl_PerVertex" : ", loose gl_Position", info.conditional ? ", by ViewportIndex" : "",
                       info.xfb ? ", transform feedback" : "", size, os);
            free(out);
            free(out2);
        }
    }
    free(code);
    return bad;
}

int main(int argc, char **argv)
{
    int quiet = 0, a = 1, bad = 0;
    for (; a < argc && argv[a][0] == '-'; a++)
        if (!strcmp(argv[a], "-q")) quiet = 1;
    if (a + 1 >= argc) {
        fprintf(stderr, "usage: flip_corpus [-q] outdir file.spv...\n");
        return 2;
    }
    const char *outdir = argv[a++];
    for (; a < argc; a++) {
        if (argv[a][0] == '@') {
            FILE *f = fopen(argv[a] + 1, "r");
            char line[4096];
            while (f && fgets(line, sizeof(line), f)) {
                line[strcspn(line, "\n")] = 0;
                if (*line) bad |= one(outdir, line, quiet);
            }
            if (f) fclose(f);
        } else {
            bad |= one(outdir, argv[a], quiet);
        }
    }
    printf("corpus: %d modules, %d pre-rasterization entry points, %d flipped, %d by ViewportIndex, %d failed\n", modules, entries, flipped,
           conditional, failed);
    return bad;
}
