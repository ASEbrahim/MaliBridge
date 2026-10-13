/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * pervertex_check: runs the server's per-vertex block rewrite (spirv_pervertex.c) on SPIR-V files
 * and checks the result: rewritten, no loose Position/PointSize/ClipDistance/CullDistance
 * interface variable left, every per-vertex built-in a Block member, and (with $PERVERTEX_VAL, a
 * validator command such as "spirv-val --target-env vulkan1.3") a valid module. Files named
 * *reject* must instead come back untouched with a reason. Driven by pervertex_check.sh.
 *
 *   cc -o pervertex_check pervertex_check.c ../server/spirv_pervertex.c \
 *      ../server/spirv_edit.c
 *   pervertex_check outdir file.spv...
 */
#include "../server/spirv_edit.h"
#include "../server/spirv_pervertex.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int per_vertex(int64_t b)
{
    return b == SpvBuiltInPosition || b == SpvBuiltInPointSize || b == SpvBuiltInClipDistance || b == SpvBuiltInCullDistance;
}

/* Loose per-vertex built-in variables, and Block members carrying one. */
static void count(const uint32_t *code, size_t size, int *loose, int *members)
{
    spv_mod m;
    *loose = *members = 0;
    if (spv_parse(&m, code, size) != 0) {
        *loose = -1;
        return;
    }
    for (uint32_t i = 0; i < m.count; i++) {
        const spv_inst *in = &m.ins[i];
        if (in->op == SpvOpVariable && (in->w[2] == SpvStorageClassInput || in->w[2] == SpvStorageClassOutput) &&
            per_vertex(spv_get_decoration(&m, in->w[1], SpvDecorationBuiltIn)))
            (*loose)++;
        if (in->op == SpvOpMemberDecorate && in->w[2] == SpvDecorationBuiltIn && per_vertex(in->w[3]) &&
            spv_get_decoration(&m, in->w[0], SpvDecorationBlock) >= 0)
            (*members)++;
    }
    spv_free(&m);
}

static uint32_t *load(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    uint32_t *code = malloc((size_t)sz);
    if (!code || fread(code, 1, (size_t)sz, f) != (size_t)sz) {
        fclose(f);
        free(code);
        return NULL;
    }
    fclose(f);
    *size = (size_t)sz;
    return code;
}

static int check(const char *outdir, const char *path)
{
    size_t size, osz;
    uint32_t *code = load(path, &size), *out = NULL;
    if (!code) {
        printf("FAIL %s: cannot read\n", path);
        return 1;
    }
    const char *base = strrchr(path, '/') ? strrchr(path, '/') + 1 : path, *why = NULL;
    int reject = strstr(base, "reject") != NULL;
    int r = vkb_spv_wrap_pervertex(code, size, &out, &osz, &why);
    int loose0, members0;
    count(code, size, &loose0, &members0);
    free(code);
    if (reject) {
        int ok = !r && why;
        printf("%s %s: %s (%s)\n", ok ? "ok  " : "FAIL", base, r ? "rewritten" : "untouched", why ? why : "no reason");
        free(out);
        return !ok;
    }
    if (!r) {
        printf("FAIL %s: untouched (%s)\n", base, why ? why : "nothing to do");
        return 1;
    }
    int loose, members;
    count(out, osz, &loose, &members);
    char opath[4096];
    snprintf(opath, sizeof(opath), "%s/rewritten_%s", outdir, base);
    FILE *o = fopen(opath, "wb");
    int wrote = o && fwrite(out, 1, osz, o) == osz;
    if (o) fclose(o);
    free(out);
    if (!wrote || loose != 0 || members != loose0 + members0) {
        printf("FAIL %s: %d loose per-vertex built-ins before, %d after; %d block members (want %d)%s\n", base, loose0,
               loose, members, loose0 + members0, wrote ? "" : "; cannot write output");
        return 1;
    }
    const char *val = getenv("PERVERTEX_VAL");
    if (val && *val) {
        char cmd[8192];
        snprintf(cmd, sizeof(cmd), "%s '%s'", val, opath);
        if (system(cmd) != 0) {
            printf("FAIL %s: the validator rejects %s\n", base, opath);
            return 1;
        }
    }
    printf("ok   %s: %d loose per-vertex built-ins folded into blocks%s%s\n", base, loose0, val && *val ? ", valid" : "",
           why ? " (one interface kept loose!)" : "");
    return why != NULL;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s outdir file.spv...\n", argv[0]);
        return 2;
    }
    int fails = 0;
    for (int i = 2; i < argc; i++) fails += check(argv[1], argv[i]);
    printf("== %d checked, %d failed\n", argc - 2, fails);
    return fails != 0;
}
