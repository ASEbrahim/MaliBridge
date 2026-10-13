/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * clip_check: runs the server's clip/cull distance demotion (spirv_clip.c) on SPIR-V files the way
 * a Mali device gets it - geometry/tessellation shaders first through the per-vertex fold
 * (spirv_pervertex.c, which sits above the emulation in the server), fragment shaders also through
 * the discard - and checks the result: no ClipDistance/CullDistance built-in or capability left,
 * something demoted when the input had one (nothing for *unused* files: built-ins declared and never
 * accessed are removed, not demoted), and (with $CLIP_VAL, a validator command such as
 * "spirv-val --target-env vulkan1.3") a valid module. Driven by clip_check.sh.
 *
 * With --scan, checks only that no clip/cull built-in is left in modules a server gave its driver.
 *
 *   clip_check outdir file.spv...
 *   clip_check --scan file.spv...
 */
#include "../server/spirv_clip.h"
#include "../server/spirv_pervertex.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ClipDistance/CullDistance built-ins and capabilities. */
static int clip_cull(const spv_mod *m)
{
    int n = 0;
    for (uint32_t i = 0; i < m->count; i++) {
        const spv_inst *in = &m->ins[i];
        if (in->op == SpvOpDecorate && in->n >= 4 && in->w[1] == SpvDecorationBuiltIn &&
            (in->w[2] == SpvBuiltInClipDistance || in->w[2] == SpvBuiltInCullDistance))
            n++;
        if (in->op == SpvOpMemberDecorate && in->n >= 5 && in->w[2] == SpvDecorationBuiltIn &&
            (in->w[3] == SpvBuiltInClipDistance || in->w[3] == SpvBuiltInCullDistance))
            n++;
        if (in->op == SpvOpCapability && (in->w[0] == SpvCapabilityClipDistance || in->w[0] == SpvCapabilityCullDistance)) n++;
    }
    return n;
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
    size_t size;
    uint32_t *code = load(path, &size);
    const char *base = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
    if (!code) {
        printf("FAIL %s: cannot read\n", base);
        return 1;
    }
    spv_mod m;
    if (spv_parse(&m, code, size) != 0) {
        printf("FAIL %s: cannot parse\n", base);
        free(code);
        return 1;
    }
    int model = spv_execution_model(&m), folded = 0;
    if (model == SpvExecutionModelGeometry || model == SpvExecutionModelTessellationControl ||
        model == SpvExecutionModelTessellationEvaluation) {
        uint32_t *fc;
        size_t fsz;
        const char *why;
        if (vkb_spv_wrap_pervertex(code, size, &fc, &fsz, &why)) {
            spv_free(&m);
            spv_parse(&m, fc, fsz);
            free(fc);
            folded = 1;
        }
    }
    free(code);
    int before = clip_cull(&m);
    unsigned what = VKB_SPV_CLIP | VKB_SPV_CULL | VKB_SPV_POINT_SIZE;
    vkb_spv_clip_info info;
    memset(&info, 0, sizeof(info));
    uint32_t loc = (uint32_t)(vkb_spv_clip_max_location(&m) + 1);
    int r = vkb_spv_clip_needed(&m, what) ? vkb_spv_clip_demote(&m, what, loc, NULL, &info) : 0;
    int fs = model == SpvExecutionModelFragment;
    if (r == 0 && fs) r = vkb_spv_clip_discard(&m, loc, info.in[0] ? info.in[0] : 2, info.clip_in_var);
    int after = clip_cull(&m);
    uint32_t *out;
    size_t osz = spv_write(&m, &out);
    spv_free(&m);
    char opath[4096];
    snprintf(opath, sizeof(opath), "%s/rewritten_%s%s", outdir, base, strstr(base, ".spv") ? "" : ".spv");
    FILE *o = fopen(opath, "wb");
    int wrote = o && osz && fwrite(out, 1, osz, o) == osz;
    if (o) fclose(o);
    free(out);
    int demoted = info.out[0] + info.out[1] + info.in[0] + info.in[1], unused = strstr(base, "unused") != NULL;
    if (r != 0 || after || !wrote || (before && (demoted != 0) == unused)) {
        printf("FAIL %s: rewrite %d (%s), %d clip/cull built-ins/capabilities before, %d after, %d slots demoted%s\n", base, r,
               info.why ? info.why : "-", before, after, demoted, wrote ? "" : "; cannot write output");
        return 1;
    }
    const char *val = getenv("CLIP_VAL");
    if (val && *val) {
        char cmd[8192];
        snprintf(cmd, sizeof(cmd), "%s '%s'", val, opath);
        if (system(cmd) != 0) {
            printf("FAIL %s: the validator rejects %s\n", base, opath);
            return 1;
        }
    }
    printf("ok   %s:%s %d clip/cull built-ins/capabilities -> 0; at location %u: out clip %u cull %u, in clip %u cull %u%s%s\n", base,
           folded ? " folded," : "", before, loc, info.out[0], info.out[1], info.in[0], info.in[1], fs ? ", discard" : "",
           val && *val ? ", valid" : "");
    return 0;
}

/* --scan: modules a server handed its driver (VKBRIDGE_DUMP_SPIRV) - none may carry a clip/cull
 * built-in; fragment discards counted, at least one wanted. */
static int scan(int n, char **files)
{
    int bad = 0, discards = 0;
    for (int i = 0; i < n; i++) {
        size_t size;
        uint32_t *code = load(files[i], &size);
        spv_mod m;
        if (!code || spv_parse(&m, code, size) != 0) {
            printf("FAIL %s: cannot read\n", files[i]);
            free(code);
            bad++;
            continue;
        }
        free(code);
        if (clip_cull(&m)) {
            printf("FAIL %s: %d clip/cull built-ins/capabilities reach the driver\n", files[i], clip_cull(&m));
            bad++;
        }
        discards += spv_find_op(&m, SpvOpKill, 0) >= 0 || spv_find_op(&m, SpvOpTerminateInvocation, 0) >= 0;
        spv_free(&m);
    }
    printf("== %d driver modules: %d with clip/cull built-ins, %d with the fragment discard\n", n, bad, discards);
    return bad || !discards;
}

int main(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "--scan")) return scan(argc - 2, argv + 2);
    if (argc < 3) {
        fprintf(stderr, "usage: %s outdir file.spv... | --scan file.spv...\n", argv[0]);
        return 2;
    }
    int fails = 0;
    for (int i = 2; i < argc; i++) fails += check(argv[1], argv[i]);
    printf("== %d checked, %d failed\n", argc - 2, fails);
    return fails != 0;
}
