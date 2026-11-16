/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * splitvary_unit: the MK-47 SPIR-V pass (server/spirv_splitvary.c) alone, on splitvary_check's
 * shaders: the interface scan of each, the merge where the vertex stage writes the Location whole
 * and the fragment stage reads it split (Zink's fixed-function pair), nothing changed elsewhere,
 * the merged module scanning clean (no split input left) with one whole input at the Location,
 * the imitation, and a deterministic output. With an argument (a directory) it writes the outputs
 * there as .spv for spirv-val (host/splitvary_test.sh).
 */
#include "spirv_edit.h"
#include "spirv_splitvary.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define S(n) static const uint32_t n[] = {
S(vec4_vert)
#include "splitvary_vec4_vert.inc"
};
S(split_vert)
#include "splitvary_split_vert.inc"
};
S(split_frag)
#include "splitvary_split_frag.inc"
};
S(merged_frag)
#include "splitvary_merged_frag.inc"
};
S(st_frag)
#include "splitvary_st_frag.inc"
};
S(q_frag)
#include "splitvary_q_frag.inc"
};
S(zink_vert)
#include "splitvary_zink_vert.inc"
};
S(zink_frag)
#include "splitvary_zink_frag.inc"
};
/* Zink's (Mesa 26.0.8) fixed-function pair for GL_REPLACE texturing, dumped on a Mali-G68 r38p1 (Galaxy A35,
 * glstates.py 'replace'): the q input's id is 20, and the push-constant block has 'OpMemberDecorate %12 4 Offset 20' */
S(zrep_vert)
#include "splitvary_zinkreplace_vert.inc"
};
S(zrep_frag)
#include "splitvary_zinkreplace_frag.inc"
};

static int fails, checks;
static const char *outdir;

static void check(int ok, const char *what)
{
    checks++;
    if (!ok) fails++;
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
}

/* The Input variable(s) at a Location: count, and the component count of the first */
static int inputs_at(const uint32_t *code, size_t size, uint32_t loc, uint32_t *width)
{
    spv_mod m;
    int n = 0;
    if (spv_parse(&m, code, size) < 0) return -1;
    for (uint32_t i = 0; i < m.count; i++) {
        const spv_inst *in = &m.ins[i];
        if (in->op != SpvOpVariable || in->w[2] != SpvStorageClassInput) continue;
        if (spv_get_decoration(&m, in->w[1], SpvDecorationLocation) != loc) continue;
        if (!n++) {
            int p = spv_find_def(&m, in->w[0]);
            int t = p >= 0 ? spv_find_def(&m, m.ins[p].w[2]) : -1;
            *width = t >= 0 && m.ins[t].op == SpvOpTypeVector ? m.ins[t].w[2] : 1;
            if (spv_get_decoration(&m, in->w[1], SpvDecorationComponent) > 0) *width += 100; /* not at component 0 */
        }
    }
    spv_free(&m);
    return n;
}

static void save(const char *name, const uint32_t *code, size_t size)
{
    if (!outdir) return;
    char p[512];
    snprintf(p, sizeof(p), "%s/%s.spv", outdir, name);
    FILE *f = fopen(p, "wb");
    if (f) fwrite(code, 1, size, f), fclose(f);
}

int main(int argc, char **argv)
{
    outdir = argc > 1 ? argv[1] : NULL;
    vkb_sv_info vi, si, fi, zi;
    char msg[256];

    /* scans */
    vkb_spv_sv_scan(vec4_vert, sizeof(vec4_vert), &vi);
    snprintf(msg, sizeof(msg), "scan vec4.vert: model %d, whole 0x%x, location 1 width %u type %u", vi.model, vi.whole, vi.width[1], vi.ctype[1]);
    check(vi.model == 0 && vi.whole == 0x3 && vi.width[1] == 4 && vi.ctype[1] == VKB_SV_FLOAT && !vi.split, msg);
    vkb_spv_sv_scan(split_vert, sizeof(split_vert), &si);
    snprintf(msg, sizeof(msg), "scan split.vert: whole 0x%x (location 1 written in pieces: not whole)", si.whole);
    check(si.model == 0 && si.whole == 0x1, msg);
    vkb_spv_sv_scan(split_frag, sizeof(split_frag), &fi);
    snprintf(msg, sizeof(msg), "scan split.frag: model %d, split 0x%x", fi.model, fi.split);
    check(fi.model == 4 && fi.split == 0x2 && !fi.whole, msg);
    vkb_spv_sv_scan(zink_vert, sizeof(zink_vert), &zi);
    snprintf(msg, sizeof(msg), "scan zink.vert (SPIR-V 1.6, plain Position): whole 0x%x", zi.whole);
    check(zi.model == 0 && zi.whole == 0x3 && zi.width[1] == 4, msg);
    vkb_sv_info mi;
    vkb_spv_sv_scan(merged_frag, sizeof(merged_frag), &mi);
    check(mi.model == 4 && !mi.split, "scan merged.frag: no split input");
    check(vkb_spv_sv_scan((const uint32_t *)"not spirv at all....", 20, &mi) < 0, "scan: not SPIR-V refused");

    /* the merge */
    const struct {
        const char *name;
        const uint32_t *fs;
        size_t size;
        const vkb_sv_info *prev;
        int want;     /* 1 merged, 0 untouched */
        unsigned loads;
    } cs[] = {
        {"vec4-split", split_frag, sizeof(split_frag), &vi, 1, 2},
        {"vec4-q", q_frag, sizeof(q_frag), &vi, 1, 1},
        {"zink-split", zink_frag, sizeof(zink_frag), &zi, 1, 2},
        {"vec4-merged", merged_frag, sizeof(merged_frag), &vi, 0, 0},
        {"vec4-st", st_frag, sizeof(st_frag), &vi, 0, 0},
        {"split-split", split_frag, sizeof(split_frag), &si, 0, 0},
    };
    for (unsigned i = 0; i < sizeof(cs) / sizeof(cs[0]); i++) {
        uint32_t *out = NULL, *out2 = NULL;
        size_t os = 0, os2 = 0;
        vkb_splitvary_stats st;
        const char *why;
        int r = vkb_spv_split_varying(cs[i].fs, cs[i].size, cs[i].prev, 0, &out, &os, &st, &why);
        if (!cs[i].want) {
            snprintf(msg, sizeof(msg), "%s: untouched (r %d)", cs[i].name, r);
            check(r == 0 && !out, msg);
            continue;
        }
        vkb_sv_info oi;
        uint32_t w = 0;
        int n = r == 1 ? inputs_at(out, os, 1, &w) : -1;
        int nscan = r == 1 ? vkb_spv_sv_scan(out, os, &oi) : -1;
        snprintf(msg, sizeof(msg), "%s: merged (r %d, locations 0x%x, %u inputs, %u loads); after: %d input(s) at location 1, width %u, split 0x%x",
                 cs[i].name, r, st.locations, st.pieces, st.loads, n, w, nscan == 0 ? oi.split : 0xff);
        check(r == 1 && st.locations == 0x2 && st.loads == cs[i].loads && n == 1 && w == 4 && nscan == 0 && !oi.split && oi.model == 4, msg);
        save(cs[i].name, out, os);
        int r2 = vkb_spv_split_varying(cs[i].fs, cs[i].size, cs[i].prev, 0, &out2, &os2, NULL, &why);
        snprintf(msg, sizeof(msg), "%s: the same output again (%zu bytes)", cs[i].name, os);
        check(r2 == 1 && os2 == os && !memcmp(out, out2, os), msg);
        /* nothing left for the pass or the imitation */
        uint32_t *o3 = NULL;
        size_t s3 = 0;
        int r3 = vkb_spv_split_varying(out, os, cs[i].prev, 0, &o3, &s3, NULL, &why);
        int r4 = vkb_spv_split_varying(out, os, cs[i].prev, VKB_SPLITVARY_IMITATE, &o3, &s3, NULL, &why);
        snprintf(msg, sizeof(msg), "%s: the merged module left alone by the pass (%d) and the imitation (%d)", cs[i].name, r3, r4);
        check(r3 == 0 && r4 == 0, msg);
        /* the imitation on the original */
        uint32_t *oi_code = NULL;
        size_t ois = 0;
        int ri = vkb_spv_split_varying(cs[i].fs, cs[i].size, cs[i].prev, VKB_SPLITVARY_IMITATE, &oi_code, &ois, &st, &why);
        snprintf(msg, sizeof(msg), "%s: imitated (r %d, %u load(s) give 0)", cs[i].name, ri, st.loads);
        check(ri == 1 && st.loads == 1, msg);
        if (ri == 1) {
            char nm[64];
            snprintf(nm, sizeof(nm), "%s-imitated", cs[i].name);
            save(nm, oi_code, ois);
        }
        free(out), free(out2), free(oi_code);
    }

    /* a previous stage that writes nothing whole there: no pairing */
    vkb_sv_info none;
    memset(&none, 0, sizeof(none));
    uint32_t *o = NULL;
    size_t os = 0;
    const char *why;
    check(vkb_spv_split_varying(split_frag, sizeof(split_frag), &none, 0, &o, &os, NULL, &why) == 0 && !o, "no whole output before: untouched");
    /* a vertex module given as the fragment one: untouched */
    check(vkb_spv_split_varying(vec4_vert, sizeof(vec4_vert), &vi, 0, &o, &os, NULL, &why) == 0 && !o, "a vertex module: untouched");

    /* a player log (test42): a refused module's line said only "decoration, use or type". The reason names the
     * Location, the component and the rule: Zink's FS with an extra decoration (Invariant) on the component-3 input. */
    {
        spv_mod m;
        uint32_t c3 = 0, *dc = NULL;
        size_t ds = 0;
        if (spv_parse(&m, zink_frag, sizeof(zink_frag)) == 0) {
            for (uint32_t i = 0; i < m.count; i++)
                if (m.ins[i].op == SpvOpDecorate && m.ins[i].w[1] == SpvDecorationComponent && m.ins[i].w[2] == 3) c3 = m.ins[i].w[0];
            uint32_t d[2] = {c3, 18 /* Invariant */};
            spv_insert(&m, spv_annotations_end(&m), SpvOpDecorate, d, 2);
            ds = spv_write(&m, &dc);
        }
        spv_free(&m);
        vkb_splitvary_stats st;
        const char *why = "";
        int r = dc ? vkb_spv_split_varying(dc, ds, &zi, 0, &o, &os, &st, &why) : 0;
        printf("     why: %s\n", why);
        check(c3 && r == -1 && !o && st.skipped == 0x2 && strstr(why, "location 1") && strstr(why, "float4") && strstr(why, "component 3") &&
                  strstr(why, "decoration 18"),
              "refused (an extra decoration on the component-3 input): the reason names location 1, component 3, decoration 18");
        free(dc);
    }

    /* a player log: Zink's GL_REPLACE fragment shader was refused because a literal (an Offset of 20) equals the id
     * of the q input (20). Literals are not uses: merged, one whole vec4 input at Location 0, clean */
    {
        vkb_sv_info rv;
        vkb_spv_sv_scan(zrep_vert, sizeof(zrep_vert), &rv);
        vkb_splitvary_stats st;
        const char *why = "";
        uint32_t *out = NULL, w = 0;
        size_t outs = 0;
        int r = vkb_spv_split_varying(zrep_frag, sizeof(zrep_frag), &rv, 0, &out, &outs, &st, &why);
        vkb_sv_info oi;
        int n = out ? inputs_at(out, outs, 0, &w) : -1;
        if (out) vkb_spv_sv_scan(out, outs, &oi);
        printf("     r=%d why: %s\n", r, why);
        check(rv.whole & 1 && r == 1 && st.locations == 0x1 && st.loads == 2 && n == 1 && w == 4 && !oi.split,
              "zink-replace (a literal equal to a piece's id): merged into one vec4 input at Location 0");
        if (out) save("zink-replace-merged", out, outs);
        free(out);
    }
    /* a real use the rewrite does not follow (the q input's pointer copied) is still refused, and named */
    {
        spv_mod m;
        uint32_t c3 = 0, *dc = NULL, ptype = 0;
        size_t ds = 0;
        if (spv_parse(&m, zink_frag, sizeof(zink_frag)) == 0) {
            for (uint32_t i = 0; i < m.count; i++)
                if (m.ins[i].op == SpvOpDecorate && m.ins[i].w[1] == SpvDecorationComponent && m.ins[i].w[2] == 3) c3 = m.ins[i].w[0];
            int d = spv_find_def(&m, c3);
            ptype = d >= 0 ? m.ins[d].w[0] : 0;
            int lab = spv_find_op(&m, SpvOpLabel, 0);
            uint32_t ops[3] = {ptype, spv_new_id(&m), c3};
            if (lab >= 0) spv_insert(&m, (uint32_t)lab + 1, 83 /* OpCopyObject */, ops, 3);
            ds = spv_write(&m, &dc);
        }
        spv_free(&m);
        vkb_splitvary_stats st;
        const char *why = "";
        int r = dc ? vkb_spv_split_varying(dc, ds, &zi, 0, &o, &os, &st, &why) : 0;
        printf("     why: %s\n", why);
        check(c3 && ptype && r == -1 && !o && strstr(why, "component 3") && strstr(why, "use: op 83"),
              "a real use of a piece (OpCopyObject of its pointer): still refused, the reason names op 83");
        free(dc);
    }

    printf("%s: %d of %d checks %s\n", fails ? "FAIL" : "OK", fails ? fails : checks, checks, fails ? "failed" : "passed");
    return fails ? 1 : 0;
}
