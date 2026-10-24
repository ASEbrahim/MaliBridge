/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * ASTC 4x4 HDR block encoder for the Mali bridge's BC6H transcoding (bc_decode.comp, ENC_ASTC_HDR;
 * VK_FORMAT_ASTC_4x4_SFLOAT_BLOCK). Input: g_hpx[16], the BC6H texels as half floats (row-major).
 * Output: the 128-bit block, little-endian words.
 *
 * An ASTC HDR endpoint is a 12-bit value in a logarithmic number system (LNS): 5 bits of exponent
 * and 7 of a mantissa whose steps approximate log2, so one unit is about 1/128 of an octave (0.54%).
 * Texels interpolate the endpoints (shifted to 16 bits) in that domain and a decoder turns the
 * result into a half float. BC6H also interpolates in a domain linear in half-float bits, so a BC6H
 * block's texels lie near a line in LNS, and a line there is what one ASTC partition holds.
 *
 * Endpoints: the principal axis through the texels' LNS values, its extremes, encoded as colour
 * endpoint mode 11 (HDR RGB, direct) in 8-bit values (QUANT_256), with 3-bit weights. Mode 11
 * stores the brighter endpoint's largest channel (a) and differences from it (b: the other
 * channels, c: the major channel's span, d: the others' spans relative to c) in one of 8 bit
 * layouts that trade a's precision against the differences' range (a in 9-12 bits, so endpoints
 * exact to 1-8 LNS units); the most precise layout that holds the block is used, and a block no
 * layout holds takes mode 11's direct form (8 bits per channel, 7 for blue). Weights: each texel's
 * projection on the line between the endpoints as decoded, rounded to the nearest level; then the
 * endpoints refitted to those weights by least squares and encoded again, and whichever of the two
 * gives the lower error in linear values is kept.
 *
 * Where that leaves an error over 0.1% of the block's largest value (RMS), also colour endpoint
 * mode 7 (HDR RGB, base and scale: 4 bytes, so 4-bit weights fit) on the line through the texels
 * along (1, 1, 1) in LNS, i.e. one chromaticity with the intensity varying, likewise refitted once.
 * Wide-ranging blocks of noisy detail (bcn_check's HDR photo) mostly vary in intensity, and 16
 * weight levels instead of 8 halve their error. A block of one colour is an HDR void-extent block
 * (its half floats exactly). Negative values (BC6H SFLOAT) have no ASTC HDR encoding and become 0.
 *
 * Uses astc_put, astc_put64, astc_w and astc_wq from astc_enc.glsl.
 */

const float HDR_MAX = 3967.0; /* the largest 12-bit endpoint whose 16-bit value decodes under the half-float maximum */

/* A half float's LNS value (16-bit units): the decoder's mantissa map (M -> 3M, 4M - 512, 5M - 2048,
 * whose top 10 of 13 bits are the half's mantissa) inverted at the middle of the half's step. */
float hdr_lns(uint h)
{
    if ((h & 0x8000u) != 0u) return 0.0;
    h = min(h, 0x7bffu);
    float mt = float(h & 1023u) * 8.0 + 3.5;
    float m = mt < 1536.0 ? mt / 3.0 : (mt < 5632.0 ? (mt + 512.0) * 0.25 : (mt + 2048.0) * 0.2);
    return float(h >> 10) * 2048.0 + min(m, 2047.0);
}

/* A 16-bit LNS value as the half float a decoder makes of it. */
uint hdr_half(uint c)
{
    uint m = c & 0x7ffu;
    uint mt = m < 512u ? 3u * m : (m >= 1536u ? 5u * m - 2048u : 4u * m - 512u);
    return min(((c >> 11) << 10) + (mt >> 3), 0x7bffu);
}

vec3 hdr_texel(uint t)
{
    uvec3 h = g_hpx[t];
    return vec3(hdr_lns(h.x), hdr_lns(h.y), hdr_lns(h.z));
}

vec3 hdr_linear(uvec3 h)
{
    h = (h & 0x8000u) != uvec3(0u) ? uvec3(0u) : h;
    return vec3(unpackHalf2x16(h.x).x, unpackHalf2x16(h.y).x, unpackHalf2x16(h.z).x);
}

/* Mode 11 values for endpoints p0, p1 (12-bit LNS units): the six bytes in v (low byte first: v0,
 * v1, v2, v3 in v.x, v4, v5 in v.y) and the endpoints as a decoder makes them in d0, d1. */
void hdr_cem11(vec3 p0, vec3 p1, out uvec2 v, out ivec3 d0, out ivec3 d1)
{
    p0 = clamp(p0, 0.0, HDR_MAX);
    p1 = clamp(p1, 0.0, HDR_MAX);
    /* the second endpoint holds the largest channel; it is swizzled to red */
    if (max(p0.r, max(p0.g, p0.b)) > max(p1.r, max(p1.g, p1.b))) {
        vec3 t = p0; p0 = p1; p1 = t;
    }
    uint maj = p1.r >= p1.g && p1.r >= p1.b ? 0u : (p1.g >= p1.b ? 1u : 2u);
    if (maj == 1u) { p0 = p0.grb; p1 = p1.grb; }
    else if (maj == 2u) { p0 = p0.bgr; p1 = p1.bgr; }
    /* layouts from the most precise a: (a, b, c, d) bits 12,6,7,6 / 12,7,7,5 / 11,6,8,6 / 11,8,6,5 /
     * 10,7,7,6 / 10,6,7,7 / 9,8,6,6 / 9,7,6,7 for modes 7, 6, ..., 0; values scaled by 2^(3 - mode / 2) */
    for (int k = 0; k < 8; k++) {
        uint mode = uint(7 - k), sh = 3u - (mode >> 1);
        uint ab = 9u + (mode >> 1);
        uint bb = mode == 1u || mode == 4u ? 8u : (mode == 0u || mode == 3u || mode == 6u ? 7u : 6u);
        uint cb = mode == 5u ? 8u : (mode == 2u || mode == 3u || mode == 6u || mode == 7u ? 7u : 6u);
        uint db = mode == 0u || mode == 2u ? 7u : (mode == 4u || mode == 6u ? 5u : 6u);
        float s = float(1u << sh);
        int a = clamp(int(round(p1.r / s)), 0, int((1u << ab) - 1u));
        int c = max(int(round((float(a) * s - p0.r) / s)), 0);
        int b0 = max(int(round((float(a) * s - p1.g) / s)), 0);
        int b1 = max(int(round((float(a) * s - p1.b) / s)), 0);
        if (c >= int(1u << cb) || b0 >= int(1u << bb) || b1 >= int(1u << bb)) continue;
        int dl = int(1u << (db - 1u));
        int e0 = int(round((float(a - b0 - c) * s - p0.g) / s));
        int e1 = int(round((float(a - b1 - c) * s - p0.b) / s));
        if (e0 < -dl || e0 >= dl || e1 < -dl || e1 >= dl) continue;
        uint ua = uint(a), uc = uint(c), u0 = uint(b0), u1 = uint(b1), x0 = uint(e0), x1 = uint(e1);
        /* the six bits whose meaning the layout chooses */
        uint bit0, bit1, bit2, bit3, bit4, bit5;
        bool bmode = mode == 0u || mode == 1u || mode == 3u || mode == 4u || mode == 6u;
        bit0 = bmode ? (u0 >> 6) & 1u : (ua >> 9) & 1u;
        bit1 = bmode ? (u1 >> 6) & 1u : (mode == 2u ? (uc >> 6) & 1u : (ua >> 10) & 1u);
        bit2 = mode == 0u || mode == 2u ? (x0 >> 6) & 1u : (mode == 1u || mode == 4u ? (u0 >> 7) & 1u
             : (mode == 3u ? (ua >> 9) & 1u : (mode == 5u ? (uc >> 7) & 1u : (ua >> 11) & 1u)));
        bit3 = mode == 0u || mode == 2u ? (x1 >> 6) & 1u : (mode == 1u || mode == 4u ? (u1 >> 7) & 1u : (uc >> 6) & 1u);
        bit4 = mode == 4u || mode == 6u ? (ua >> 9) & 1u : (x0 >> 5) & 1u;
        bit5 = mode == 4u || mode == 6u ? (ua >> 10) & 1u : (x1 >> 5) & 1u;
        uint v0 = ua & 0xffu;
        uint v1 = ((mode & 1u) << 7) | (((ua >> 8) & 1u) << 6) | (uc & 0x3fu);
        uint v2 = (((mode >> 1) & 1u) << 7) | (bit0 << 6) | (u0 & 0x3fu);
        uint v3 = (((mode >> 2) & 1u) << 7) | (bit1 << 6) | (u1 & 0x3fu);
        uint v4 = ((maj & 1u) << 7) | (bit2 << 6) | (bit4 << 5) | (x0 & 0x1fu);
        uint v5 = ((maj >> 1) << 7) | (bit3 << 6) | (bit5 << 5) | (x1 & 0x1fu);
        v = uvec2(v0 | (v1 << 8) | (v2 << 16) | (v3 << 24), v4 | (v5 << 8));
        int is = int(s);
        ivec3 q1 = clamp(ivec3(a, a - b0, a - b1) * is, 0, 4095);
        ivec3 q0 = clamp(ivec3(a - c, a - b0 - c - e0, a - b1 - c - e1) * is, 0, 4095);
        if (maj == 1u) { q0 = q0.grb; q1 = q1.grb; }
        else if (maj == 2u) { q0 = q0.bgr; q1 = q1.bgr; }
        d0 = q0;
        d1 = q1;
        return;
    }
    /* no layout holds the differences: the direct form, red and green in 8 bits, blue in 7 */
    if (maj == 1u) { p0 = p0.grb; p1 = p1.grb; }
    else if (maj == 2u) { p0 = p0.bgr; p1 = p1.bgr; }
    uvec3 q0 = uvec3(clamp(round(p0 / vec3(16.0, 16.0, 32.0)), vec3(0.0), vec3(255.0, 255.0, 127.0)));
    uvec3 q1 = uvec3(clamp(round(p1 / vec3(16.0, 16.0, 32.0)), vec3(0.0), vec3(255.0, 255.0, 127.0)));
    v = uvec2(q0.r | (q1.r << 8) | (q0.g << 16) | (q1.g << 24), (0x80u | q0.b) | ((0x80u | q1.b) << 8));
    d0 = ivec3(q0 * uvec3(16u, 16u, 32u));
    d1 = ivec3(q1 * uvec3(16u, 16u, 32u));
}

/* Mode 7 values (HDR RGB, base and scale: the endpoints differ by one amount in every channel) for
 * the segment p0, p1 (12-bit LNS units) moved to that form about its middle: the four bytes, low
 * byte first, and the endpoints as a decoder makes them. The brighter endpoint's largest channel
 * (red, swizzled) and the others' differences from it share the bits with the scale in one of 6
 * layouts; the most precise that holds the block is used. */
void hdr_cem7(vec3 p0, vec3 p1, out uint v, out ivec3 d0, out ivec3 d1)
{
    float sc = max((p1.r + p1.g + p1.b - p0.r - p0.g - p0.b) / 3.0, 0.0);
    vec3 m = clamp((p0 + p1) * 0.5, vec3(sc * 0.5), vec3(HDR_MAX - sc * 0.5));
    vec3 e1 = m + sc * 0.5, u1 = e1;
    uint maj = e1.r >= e1.g && e1.r >= e1.b ? 0u : (e1.g >= e1.b ? 1u : 2u);
    if (maj == 1u) e1 = e1.grb;
    else if (maj == 2u) e1 = e1.bgr;
    /* layouts from the finest step: (red, green and blue, scale) bits 11,5,7 / 11,6,5 / 10,5,8 /
     * 9,6,7 / 8,7,6 / 7,7,7 for modes 0..5, values scaled by 2, 2, 4, 8, 16, 32; mode 5's channels
     * are absolute and not swizzled */
    uint mode = 5u, r = 0u, g = 0u, b = 0u, s = 0u;
    for (uint k = 0u; k < 6u; k++) {
        uint rb = 11u - (k == 0u ? 0u : k - 1u);
        uint gb = k == 0u || k == 2u ? 5u : (k == 1u || k == 3u ? 6u : 7u);
        uint sb = k == 1u ? 5u : (k == 2u ? 8u : (k == 4u ? 6u : 7u));
        float st = float(1u << (k == 0u ? 1u : k));
        if (k == 5u) e1 = u1;
        int ri = clamp(int(round(e1.r / st)), 0, int((1u << rb) - 1u));
        int gi = k == 5u ? int(round(e1.g / st)) : int(round((float(ri) * st - e1.g) / st));
        int bi = k == 5u ? int(round(e1.b / st)) : int(round((float(ri) * st - e1.b) / st));
        int si = int(round(sc / st));
        if (k < 5u && (gi < 0 || bi < 0)) continue;
        if (k == 5u) { gi = clamp(gi, 0, 127); bi = clamp(bi, 0, 127); si = min(si, 127); }
        if (gi >= int(1u << gb) || bi >= int(1u << gb) || si >= int(1u << sb)) continue;
        mode = k;
        r = uint(ri);
        g = uint(gi);
        b = uint(bi);
        s = uint(si);
        break;
    }
    /* the seven bits whose meaning the layout chooses */
    uint x0, x1, x2, x3, x4, x5, x6;
    bool odd = mode == 1u || mode == 3u, high = mode >= 4u;
    x0 = high ? (g >> 6) & 1u : (odd ? (r >> 8) & 1u : (r >> 9) & 1u);
    x1 = high || odd ? (g >> 5) & 1u : (r >> 8) & 1u;
    x2 = high ? (b >> 6) & 1u : (r >> 7) & 1u;
    x3 = high || odd ? (b >> 5) & 1u : (mode == 0u ? (r >> 10) & 1u : (r >> 6) & 1u);
    x4 = mode == 2u ? (s >> 7) & 1u : (r >> 6) & 1u;
    x5 = mode == 1u ? (r >> 10) & 1u : (mode == 4u ? (r >> 7) & 1u : (s >> 6) & 1u);
    x6 = mode == 1u ? (r >> 9) & 1u : (s >> 5) & 1u;
    uint mv = mode < 4u ? (maj << 2) | mode : (mode == 4u ? 12u | maj : 15u);
    v = (r & 0x3fu) | ((mv & 3u) << 6) | (((g & 0x1fu) | (x1 << 5) | (x0 << 6) | (((mv >> 2) & 1u) << 7)) << 8)
      | (((b & 0x1fu) | (x3 << 5) | (x2 << 6) | ((mv >> 3) << 7)) << 16) | (((s & 0x1fu) | (x6 << 5) | (x5 << 6) | (x4 << 7)) << 24);
    int st = 1 << (mode == 0u ? 1u : mode);
    ivec3 q1 = mode == 5u ? ivec3(r, g, b) * st : ivec3(r, r - g, r - b) * st;
    if (mode == 5u) maj = 0u;
    if (maj == 1u) q1 = q1.grb;
    else if (maj == 2u) q1 = q1.bgr;
    d0 = clamp(q1 - int(s) * st, 0, 4095);
    d1 = clamp(q1, 0, 4095);
}

/* One candidate: endpoints p0, p1 (12-bit LNS units) encoded in colour endpoint mode cem (7 with
 * 4-bit weights, 11 with 3-bit), the weights for them, the endpoints refitted to those weights (r0,
 * r1). The error (squared, linear values, over the block) and, where it is below limit, the block
 * in blk. */
float hdr_try(uint cem, vec3 p0, vec3 p1, float limit, inout uvec4 blk, out vec3 r0, out vec3 r1)
{
    uvec2 v = uvec2(0u);
    ivec3 d0, d1;
    uint wb = cem == 7u ? 4u : 3u;
    if (cem == 7u) hdr_cem7(p0, p1, v.x, d0, d1);
    else hdr_cem11(p0, p1, v, d0, d1);
    vec3 e0 = vec3(d0 * 16), e1 = vec3(d1 * 16), ax = e1 - e0;
    float l = dot(ax, ax);
    ax *= l > 0.0 ? 1.0 / l : 0.0;
    float err = 0.0, sa = 0.0, sb = 0.0, sc = 0.0;
    vec3 s0 = vec3(0.0), s1 = vec3(0.0);
    uvec2 wi = uvec2(0u);
    for (uint t = 0u; t < 16u; t++) {
        vec3 x = hdr_texel(t);
        uint k = astc_wq(wb, dot(x - e0, ax));
        uint w = uint(astc_w(wb, k));
        uvec3 c = (uvec3(d0 * 16) * (64u - w) + uvec3(d1 * 16) * w + 32u) >> 6;
        vec3 dv = hdr_linear(uvec3(hdr_half(c.r), hdr_half(c.g), hdr_half(c.b))) - hdr_linear(g_hpx[t]);
        err += dot(dv, dv);
        float fx = float(w) / 64.0, fy = 1.0 - fx;
        sa += fy * fy;
        sb += fy * fx;
        sc += fx * fx;
        s0 += fy * x;
        s1 += fx * x;
        astc_put64(wi, wb * t, wb, k);
    }
    r0 = p0;
    r1 = p1;
    float det = sa * sc - sb * sb;
    if (abs(det) >= 1e-3) {
        r0 = (sc * s0 - sb * s1) / (det * 16.0);
        r1 = (sa * s1 - sb * s0) / (det * 16.0);
    }
    if (err < limit) {
        /* block mode: 4x4 weights (A = 2, B = 0), range 0..7 (R = 7, H = 0) or 0..15 (R = 4, H = 1);
         * one partition */
        uvec4 b = uvec4(0u);
        astc_put(b, 0u, 11u, wb == 3u ? 3u | (1u << 4) | (2u << 5) : 2u | (2u << 5) | (1u << 9));
        astc_put(b, 13u, 4u, cem);
        astc_put(b, 17u, 16u, v.x);
        astc_put(b, 33u, 16u, v.x >> 16);
        if (cem == 11u) astc_put(b, 49u, 16u, v.y);
        blk = b | uvec4(0u, 0u, bitfieldReverse(wi.y), bitfieldReverse(wi.x));
    }
    return min(err, limit);
}

uvec4 astc_hdr_encode()
{
    bool same = true;
    for (uint t = 1u; t < 16u; t++) same = same && g_hpx[t] == g_hpx[0];
    if (same) {
        /* one colour: an HDR void-extent block, the colour in half floats (alpha 1.0) */
        uvec3 h = (g_hpx[0] & 0x8000u) != uvec3(0u) ? uvec3(0u) : min(g_hpx[0], uvec3(0x7bffu));
        return uvec4(0xfffffffcu, 0xffffffffu, h.r | (h.g << 16), h.b | (0x3c00u << 16));
    }
    /* the principal axis of the texels' LNS values, its extremes */
    vec3 c = vec3(0.0);
    float top = 0.0;
    for (uint t = 0u; t < 16u; t++) {
        c += hdr_texel(t);
        vec3 l = hdr_linear(g_hpx[t]);
        top = max(top, max(l.r, max(l.g, l.b)));
    }
    c /= 16.0;
    mat3 cov = mat3(0.0);
    vec3 mn = vec3(1e9), mx = vec3(-1e9);
    for (uint t = 0u; t < 16u; t++) {
        vec3 d = hdr_texel(t) - c;
        cov += outerProduct(d, d);
        mn = min(mn, d);
        mx = max(mx, d);
    }
    vec3 ax = mx - mn;
    for (int i = 0; i < 6; i++) {
        vec3 nx = cov * ax;
        float l = dot(nx, nx);
        if (l < 1e-12) break;
        ax = nx * inversesqrt(l);
    }
    float la = dot(ax, ax);
    ax = la > 0.0 ? ax * inversesqrt(la) : vec3(0.0);
    float t0 = 1e9, t1 = -1e9;
    for (uint t = 0u; t < 16u; t++) {
        float p = dot(hdr_texel(t) - c, ax);
        t0 = min(t0, p);
        t1 = max(t1, p);
    }
    uvec4 blk = uvec4(0u);
    vec3 r0, r1, q0, q1;
    float e = hdr_try(11u, (c + ax * t0) / 16.0, (c + ax * t1) / 16.0, 1e30, blk, r0, r1);
    if (e > 0.0) e = hdr_try(11u, r0, r1, e, blk, q0, q1);
    /* mode 7 and 4-bit weights, the line through the mean along (1, 1, 1), unless the error is
     * already under 0.1% of the block's largest value (RMS over its 48 values) */
    if (e > 48.0 * (top * 0.001) * (top * 0.001)) {
        const vec3 g = vec3(0.57735027);
        float u0 = 1e9, u1 = -1e9;
        for (uint t = 0u; t < 16u; t++) {
            float p = dot(hdr_texel(t) - c, g);
            u0 = min(u0, p);
            u1 = max(u1, p);
        }
        e = hdr_try(7u, (c + g * u0) / 16.0, (c + g * u1) / 16.0, e, blk, r0, r1);
        if (e > 0.0) hdr_try(7u, r0, r1, e, blk, q0, q1);
    }
    return blk;
}
