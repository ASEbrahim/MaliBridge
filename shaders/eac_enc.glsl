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
