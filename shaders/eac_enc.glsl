/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * EAC R11 block encoder for the Mali bridge's BC4/BC5 transcoding (bc_decode.comp, ENC_EAC). A
 * block is a base codeword, a multiplier and one of 16 modifier tables, each texel one of the
 * table's 8 modifiers: value = base * 8 (+ 4 unsigned) + modifier * multiplier * 8, clamped to
 * 0..2047 (unsigned) or -1023..1023 (signed). Every table is tried with the base and multiplier
 * that span the block's range, and the best one again with its neighbours (not when fast).
 */

const int EAC_TAB[16][8] = int[16][8](
    int[8](-3, -6, -9, -15, 2, 5, 8, 14), int[8](-3, -7, -10, -13, 2, 6, 9, 12), int[8](-2, -5, -8, -13, 1, 4, 7, 12),
    int[8](-2, -4, -6, -13, 1, 3, 5, 12), int[8](-3, -6, -8, -12, 2, 5, 7, 11), int[8](-3, -7, -9, -11, 2, 6, 8, 10),
    int[8](-4, -7, -8, -11, 3, 6, 7, 10), int[8](-3, -5, -8, -11, 2, 4, 7, 10), int[8](-2, -6, -8, -10, 1, 5, 7, 9),
    int[8](-2, -5, -8, -10, 1, 4, 7, 9), int[8](-2, -4, -8, -10, 1, 3, 7, 9), int[8](-2, -5, -7, -10, 1, 4, 6, 9),
    int[8](-3, -4, -7, -10, 2, 3, 6, 9), int[8](-1, -2, -3, -10, 0, 1, 2, 9), int[8](-4, -6, -8, -9, 3, 5, 7, 8),
    int[8](-3, -5, -7, -9, 2, 4, 6, 8));

/* Error of (codeword, multiplier, table) over the block; the indices, column-major, 3 bits each. */
float eac_eval(float v[16], bool sgn, int cw, int m, int t, out uvec2 idx)
{
    float base = sgn ? float(cw * 8) : float(cw * 8 + 4), mul = m == 0 ? 1.0 : float(m * 8);
    float lo = sgn ? -1023.0 : 0.0, hi = sgn ? 1023.0 : 2047.0;
    float e = 0.0;
    idx = uvec2(0u);
    for (uint p = 0u; p < 16u; p++) {
        float be = 1e30;
        uint bk = 0u;
        for (uint k = 0u; k < 8u; k++) {
            float d = v[p] - clamp(base + float(EAC_TAB[t][k]) * mul, lo, hi);
            if (d * d < be) { be = d * d; bk = k; }
        }
        e += be;
        uint i = (p & 3u) * 4u + (p >> 2), pos = 45u - 3u * i; /* texel (x, y) is index x * 4 + y, msb first */
        if (pos >= 32u) idx.x |= bk << (pos - 32u);
        else {
            idx.y |= bk << pos;
            if (pos + 3u > 32u) idx.x |= bk >> (32u - pos);
        }
    }
    return e;
}

/* v: the texels in the format's units (0..2047, or -1023..1023 when signed). */
uvec2 eac_encode(float v[16], bool sgn)
{
    float vmin = v[0], vmax = v[0];
    for (uint p = 1u; p < 16u; p++) { vmin = min(vmin, v[p]); vmax = max(vmax, v[p]); }
    int cwlo = sgn ? -127 : 0, cwhi = sgn ? 127 : 255;
    float best = 1e30;
    int bcw = 0, bm = 0, bt = 0;
    uvec2 bidx = uvec2(0u), idx;
    for (int t = 0; t < 16; t++) {
        float tlo = float(EAC_TAB[t][3]), thi = float(EAC_TAB[t][7]);
        int m = clamp(int(round((vmax - vmin) / ((thi - tlo) * 8.0))), 0, 15);
        float mul = m == 0 ? 1.0 : float(m * 8);
        float base = 0.5 * (vmin + vmax) - 0.5 * (tlo + thi) * mul - (sgn ? 0.0 : 4.0);
        int cw = clamp(int(round(base / 8.0)), cwlo, cwhi);
        float e = eac_eval(v, sgn, cw, m, t, idx);
        if (e < best) { best = e; bcw = cw; bm = m; bt = t; bidx = idx; }
    }
    int cw0 = bcw, m0 = bm;
    for (int dm = -1; dm <= 1 && BC_QUALITY != 0u; dm++) /* fast (BC_QUALITY 0): not refined */
        for (int dc = -2; dc <= 2; dc++) {
            int m = m0 + dm, cw = cw0 + dc;
            if ((dm == 0 && dc == 0) || m < 0 || m > 15 || cw < cwlo || cw > cwhi) continue;
            float e = eac_eval(v, sgn, cw, m, bt, idx);
            if (e < best) { best = e; bcw = cw; bm = m; bidx = idx; }
        }
    uint hi = ((uint(bcw) & 0xffu) << 24) | (uint(bm) << 20) | (uint(bt) << 16) | bidx.x;
    return uvec2(bswap32(hi), bswap32(bidx.y));
}

#if defined(BC_PALETTE) && !defined(SRC_RAW)
/* ---------------------------------------------------------------------------------------------
 * Palette domain (BC_PALETTE variants, VKBRIDGE_BC_PALETTE; BC4 / BC5 sources). A BC4 channel is 8 values and a 3-bit
 * index a texel, so the search above tries each candidate against each of the 8 values once instead of against each
 * of the 16 texels, and then sums the texels' errors in the texels' own order (p = 0..15, each the error of its value):
 * the same floats added in the same order as eac_eval, so the same totals, the same choices and the same bits (not a
 * sum weighted by counts, whose rounding differs and broke ties the other way). eac_pv: the values as bc4_exact gives
 * them (the same function makes them), eac_pi: texel p's value (4 bits at 4p, x: texels 0-7), eac_used: bit j, a
 * texel has value j.
 */
float eac_pv[8];
uvec2 eac_pi;
uint eac_used;

uint eac_pk(uint p) { return ((p < 8u ? eac_pi.x : eac_pi.y) >> (4u * (p & 7u))) & 7u; }

/* Error of (codeword, multiplier, table) over the block, summed texel by texel in eac_eval's order; pb: each palette
 * value's choice, 3 bits at 3j (the selectors are made once, for the winner: eac_encode_p). */
float eac_eval_p(bool sgn, int cw, int m, int t, out uint pb)
{
    float base = sgn ? float(cw * 8) : float(cw * 8 + 4), mul = m == 0 ? 1.0 : float(m * 8);
    float lo = sgn ? -1023.0 : 0.0, hi = sgn ? 1023.0 : 2047.0;
    /* the table's 8 values (eac_eval makes the same ones for each texel), then each palette value's best of them:
     * unrolled, constant indices (Mali keeps arrays indexed by data on the stack) */
    float cand[8], pe[8];
    pb = 0u;
    [[unroll]] for (uint k = 0u; k < 8u; k++) cand[k] = clamp(base + float(EAC_TAB[t][k]) * mul, lo, hi);
    [[unroll]] for (uint j = 0u; j < 8u; j++) {
        float be = 1e30;
        uint bk = 0u;
        [[unroll]] for (uint k = 0u; k < 8u; k++) {
            float d = eac_pv[j] - cand[k];
            if (d * d < be) { be = d * d; bk = k; }
        }
        pe[j] = be;
        pb |= bk << (3u * j);
    }
    float e = 0.0;
    [[unroll]] for (uint p = 0u; p < 16u; p++) {
        uint j = eac_pk(p);
        float be = pe[0];
        [[unroll]] for (uint c = 1u; c < 8u; c++) be = j == c ? pe[c] : be; /* selects */
        e += be;
    }
    return e;
}

/* eac_encode over the palette (eac_pv, eac_pi, eac_used set): the same codeword, multiplier, table and selectors */
uvec2 eac_encode_p(bool sgn)
{
    float vmin = eac_pv[0], vmax = eac_pv[0];
    {
        uint j0 = eac_pk(0u);
        [[unroll]] for (uint c = 1u; c < 8u; c++)
            if (j0 == c) { vmin = eac_pv[c]; vmax = eac_pv[c]; }
    }
    [[unroll]] for (uint j = 0u; j < 8u; j++)
        if (((eac_used >> j) & 1u) != 0u) { vmin = min(vmin, eac_pv[j]); vmax = max(vmax, eac_pv[j]); }
    int cwlo = sgn ? -127 : 0, cwhi = sgn ? 127 : 255;
    float best = 1e30;
    int bcw = 0, bm = 0, bt = 0, cw0 = 0, m0 = 0;
    uint bpb = 0u, pb;
    /* eac_encode's two loops as one (the 16 tables, then the winner's neighbours, in the same order), so that the
     * search is inlined once: two copies of it did not fit Mali's registers */
    for (int i = 0; i < (BC_QUALITY != 0u ? 31 : 16); i++) {
        int t, m, cw;
        if (i < 16) {
            t = i;
            float tlo = float(EAC_TAB[t][3]), thi = float(EAC_TAB[t][7]);
            m = clamp(int(round((vmax - vmin) / ((thi - tlo) * 8.0))), 0, 15);
            float mul = m == 0 ? 1.0 : float(m * 8);
            float base = 0.5 * (vmin + vmax) - 0.5 * (tlo + thi) * mul - (sgn ? 0.0 : 4.0);
            cw = clamp(int(round(base / 8.0)), cwlo, cwhi);
        } else {
            if (i == 16) cw0 = bcw, m0 = bm;
            int dm = (i - 16) / 5 - 1, dc = (i - 16) % 5 - 2;
            t = bt, m = m0 + dm, cw = cw0 + dc;
            if ((dm == 0 && dc == 0) || m < 0 || m > 15 || cw < cwlo || cw > cwhi) continue;
        }
        float e = eac_eval_p(sgn, cw, m, t, pb);
        if (e < best) { best = e; bcw = cw; bm = m; bt = t; bpb = pb; }
    }
    uvec2 bidx = uvec2(0u);
    for (uint p = 0u; p < 16u; p++) {
        uint bk = (bpb >> (3u * eac_pk(p))) & 7u;
        uint i = (p & 3u) * 4u + (p >> 2), pos = 45u - 3u * i; /* as eac_eval */
        if (pos >= 32u) bidx.x |= bk << (pos - 32u);
        else {
            bidx.y |= bk << pos;
            if (pos + 3u > 32u) bidx.x |= bk >> (32u - pos);
        }
    }
    uint hi = ((uint(bcw) & 0xffu) << 24) | (uint(bm) << 20) | (uint(bt) << 16) | bidx.x;
    return uvec2(bswap32(hi), bswap32(bidx.y));
}
#endif
