/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * capgate_unit: the capability check's SPIR-V scan (server/spirv_caps.c) on hand-made modules and on
 * the corpus: ULTRAKILL's Unity vertex shader declares gl_ClipDistance in gl_PerVertex WITHOUT the
 * ClipDistance capability (Mali's compiler died on it, 0x40): the scan must see it by its decoration.
 *   capgate_unit <tests/corpus dir>
 * One line per check, "OK: ..." / "FAILED: ..." last.
 */
#include "spirv_caps.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails, checks;

static void check(int ok, const char *what, uint32_t got, uint32_t want)
{
    checks++;
    char g[400], w[400];
    vkb_spv_caps_text(got, g, sizeof(g));
    vkb_spv_caps_text(want, w, sizeof(w));
    printf("%s %s: %s%s%s\n", ok ? "PASS" : "FAIL", what, got ? g : "nothing", ok ? "" : " (want ", ok ? "" : (want ? w : "nothing"));
    if (!ok) fails++;
}

static uint32_t *load(const char *path, size_t *bytes)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    uint32_t *c = malloc((size_t)sz);
    *bytes = fread(c, 1, (size_t)sz, f);
    fclose(f);
    return c;
}

#define HDR 0x07230203u, 0x00010000u, 0, 100, 0
#define OP(op, wc) (((uint32_t)(wc) << 16) | (op))

int main(int argc, char **argv)
{
    /* OpCapability Shader, ClipDistance, Float64; then a type: the scan stops before the decoration after it */
    const uint32_t caps[] = {HDR, OP(17, 2), 1, OP(17, 2), 32, OP(17, 2), 10, OP(19, 2), 1, OP(71, 4), 9, 11, 4};
    uint32_t m = vkb_spv_caps(caps, sizeof(caps));
    check(m == (VKB_SPVCAP_CLIP | VKB_SPVCAP_F64), "capabilities ClipDistance + Float64, a CullDistance decoration after the first type ignored", m,
          VKB_SPVCAP_CLIP | VKB_SPVCAP_F64);
    /* OpMemberDecorate %block 2 BuiltIn CullDistance, no capability */
    const uint32_t member[] = {HDR, OP(17, 2), 1, OP(72, 5), 7, 2, 11, 4, OP(19, 2), 1};
    m = vkb_spv_caps(member, sizeof(member));
    check(m == VKB_SPVCAP_CULL, "gl_PerVertex member BuiltIn CullDistance, no capability", m, VKB_SPVCAP_CULL);
    /* OpDecorate %x BuiltIn Position (0): nothing */
    const uint32_t pos[] = {HDR, OP(17, 2), 1, OP(71, 4), 7, 11, 0, OP(19, 2), 1};
    m = vkb_spv_caps(pos, sizeof(pos));
    check(m == 0, "BuiltIn Position only", m, 0);
    /* not SPIR-V, truncated instruction */
    const uint32_t bad[] = {0x12345678u, 0, 0, 0, 0, OP(17, 2), 32};
    m = vkb_spv_caps(bad, sizeof(bad));
    check(m == 0, "not SPIR-V (bad magic)", m, 0);
    const uint32_t cut[] = {HDR, OP(17, 9), 32};
    m = vkb_spv_caps(cut, sizeof(cut));
    check(m == 0, "an instruction running past the end", m, 0);
    if (argc > 1) {
        static const struct {
            const char *file;
            uint32_t want;
            const char *what;
        } corpus[] = {
            {"ultrakill_unity_vs_clipblock.spv", VKB_SPVCAP_CLIP, "ULTRAKILL's Unity VS: gl_ClipDistance in gl_PerVertex, no capability"},
            {"ultrakill_unity_fs.spv", 0, "ULTRAKILL's Unity FS"},
        };
        for (size_t i = 0; i < sizeof(corpus) / sizeof(corpus[0]); i++) {
            char path[1024];
            size_t bytes = 0;
            snprintf(path, sizeof(path), "%s/%s", argv[1], corpus[i].file);
            uint32_t *c = load(path, &bytes);
            if (!c) {
                checks++, fails++;
                printf("FAIL %s: %s not readable\n", corpus[i].what, path);
                continue;
            }
            m = vkb_spv_caps(c, bytes);
            check(m == corpus[i].want, corpus[i].what, m, corpus[i].want);
            free(c);
        }
    }
    printf("%s: %d of %d checks passed\n", fails ? "FAILED" : "OK", checks - fails, checks);
    return fails != 0;
}
