/* ETC2 RGB8 / RGB8A1 block encoder for the Mali bridge's BC transcoding (bc_decode.comp, ENC_ETC2).
 *
 * Ported to GLSL from compress.slang of leegao/etc2_encode (commit a32883c; "Realtime rgba8 ->
 * astc/etc2/etc1 encoder as a compute shader"), whose licence follows. Kept from it: the ETC1
 * subblock search around the subblock's mean colour, the T-mode colour pair from the mean and the
 * colour farthest off the grey axis refined by 2-means, the T-mode overflow trick and the bit
 * layouts. Changed: errors are taken against the colours a decoder expands the 4/5-bit bases to;
 * every table and both flips are tried; the individual mode is used where the differential one
 * cannot reach; the base colour is refitted once to the chosen modifiers; T mode tries both colour
 * roles; planar mode (gradients, which BC1 textures are full of) is added; and ETC2 RGB8A1
 * punch-through alpha, which BC1 with alpha needs, is added.
 *
 * MIT License
 *
 * Copyright (c) 2026 Lee Gao
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * Input: g_px[16] (row-major texels, 8-bit RGBA). Output: the block as two words in memory order
 * (ETC2 blocks are big-endian 64-bit values).
 */

/* ETC1 modifier tables (small, large) and the T-mode distances. */
const ivec2 ETC1_MOD[8] = ivec2[8](ivec2(2, 8), ivec2(5, 17), ivec2(9, 29), ivec2(13, 42), ivec2(18, 60), ivec2(24, 80),
                                   ivec2(33, 106), ivec2(47, 183));
const int ETC2_DIST[8] = int[8](3, 6, 11, 16, 23, 32, 41, 64);

bool etc_punch; /* RGB8A1: texels with alpha < 128 are transparent (punch-through) */

bool etc_clear(uint p) { return etc_punch && g_px[p].a < 128; }

int etc_err(ivec3 a, ivec3 b)
{
    ivec3 d = a - b;
    return d.x * d.x + d.y * d.y + d.z * d.z;
}

ivec3 etc_x5(ivec3 c) { return (c << 3) | (c >> 2); }
ivec3 etc_x4(ivec3 c) { return c * 17; }

/* Texel i (0-7) of subblock sb: 2x4 side by side, or with the flip bit 4x2 stacked. */
uint etc_subpx(bool flip, uint sb, uint i) { return flip ? sb * 8u + i : (i >> 1) * 4u + sb * 2u + (i & 1u); }

/* Modifier of index k (msb:lsb) in table t; in a transparent-capable block (opaque bit 0) index 0
 * means no modifier and index 2 transparent. */
int etc_mod(uint t, uint k, bool clear)
{
    ivec2 m = ETC1_MOD[t];
    if (clear) return k == 0u ? 0 : (k == 1u ? m.y : -m.y);
    return k == 0u ? m.x : (k == 1u ? m.y : (k == 2u ? -m.x : -m.y));
}

/* The 32 selector bits: pixel (x, y) at bit x * 4 + y, msb 16 higher. sel holds 2 bits per texel. */
uint etc_sel(uint sel)
{
    uint w = 0u;
    for (uint p = 0u; p < 16u; p++) {
        uint k = (sel >> (2u * p)) & 3u, pos = (p & 3u) * 4u + (p >> 2);
        w |= ((k & 1u) << pos) | ((k >> 1) << (pos + 16u));
    }
    return w;
}


/* Best table for one subblock around `base`; its selectors go into sel, and the mean of its texels
 * less their chosen modifiers (the base that would fit them better) into target. */
int etc1_sub(ivec3 base, bool flip, uint sb, bool clear, out uint table, inout uint sel, out vec3 target)
{
    int best = 0x7fffffff;
    uint bsel = 0u;
    table = 0u;
    target = vec3(base);
    for (uint t = 0u; t < 8u; t++) {
        int e = 0, n = 0;
        uint s = 0u;
        ivec3 acc = ivec3(0);
        for (uint i = 0u; i < 8u; i++) {
            uint p = etc_subpx(flip, sb, i);
            if (etc_clear(p)) {
                s |= 2u << (2u * p);
                continue;
            }
            int be = 0x7fffffff, bm = 0;
            uint bk = 0u;
            for (uint k = 0u; k < 4u; k++) {
                if (clear && k == 2u) continue;
                int md = etc_mod(t, k, clear);
                int ee = etc_err(g_px[p].rgb, clamp(base + md, 0, 255));
                if (ee < be) { be = ee; bk = k; bm = md; }
            }
            e += be;
            s |= bk << (2u * p);
            acc += g_px[p].rgb - bm;
            n++;
        }
        if (e < best) {
            best = e;
            table = t;
            bsel = s;
            target = n > 0 ? vec3(acc) / float(n) : vec3(base);
        }
    }
    sel |= bsel;
    return best;
}

/* ETC1 differential / individual mode with one flip. Without punch-through the opaque bit is the
 * differential flag; with it there is no individual mode, and an out-of-reach second colour is
 * pulled in. */
int etc1_try(bool flip, bool clear, out uvec2 hl)
{
    vec3 avg[2];
    for (uint sb = 0u; sb < 2u; sb++) {
        ivec3 sum = ivec3(0);
        int n = 0;
        for (uint i = 0u; i < 8u; i++) {
            uint p = etc_subpx(flip, sb, i);
            if (!etc_clear(p)) { sum += g_px[p].rgb; n++; }
        }
        avg[sb] = n > 0 ? vec3(sum) / float(n) : vec3(0.0);
    }
    int best = 0x7fffffff;
    hl = uvec2(0u);
    for (int pass = 0; pass < 2; pass++) {
        ivec3 q5a = clamp(ivec3(round(avg[0] * (31.0 / 255.0))), 0, 31);
        ivec3 q5b = clamp(ivec3(round(avg[1] * (31.0 / 255.0))), 0, 31);
        ivec3 d = q5b - q5a;
        bool diff = all(greaterThanEqual(d, ivec3(-4))) && all(lessThanEqual(d, ivec3(3)));
        if (!diff && etc_punch) {
            q5b = clamp(q5b, q5a - 4, q5a + 3);
            diff = true;
        }
        ivec3 q4a = clamp(ivec3(round(avg[0] / 17.0)), 0, 15), q4b = clamp(ivec3(round(avg[1] / 17.0)), 0, 15);
        ivec3 b0 = diff ? etc_x5(q5a) : etc_x4(q4a), b1 = diff ? etc_x5(q5b) : etc_x4(q4b);
        uint t0, t1, sel = 0u;
        vec3 n0, n1;
        int e = etc1_sub(b0, flip, 0u, clear, t0, sel, n0) + etc1_sub(b1, flip, 1u, clear, t1, sel, n1);
        if (e < best) {
            best = e;
            uint hi = (flip ? 1u : 0u) | ((etc_punch ? (clear ? 0u : 1u) : (diff ? 1u : 0u)) << 1) | (t0 << 5) | (t1 << 2);
            if (diff) {
                uvec3 a = uvec3(q5a), dd = uvec3(q5b - q5a) & 7u;
                hi |= (a.r << 27) | (dd.r << 24) | (a.g << 19) | (dd.g << 16) | (a.b << 11) | (dd.b << 8);
            } else {
                uvec3 a = uvec3(q4a), b = uvec3(q4b);
                hi |= (a.r << 28) | (b.r << 24) | (a.g << 20) | (b.g << 16) | (a.b << 12) | (b.b << 8);
            }
            hl = uvec2(hi, etc_sel(sel));
        }
        /* refit the bases to the texels less their chosen modifiers */
        avg[0] = clamp(n0, 0.0, 255.0);
        avg[1] = clamp(n1, 0.0, 255.0);
    }
    return best;
}

/* T mode: paint colours c1, c2 + d, c2, c2 - d (c2 transparent in a transparent-capable block). */
int etct_eval(ivec3 c1, ivec3 c2, bool clear, out uint dist, out uint sel)
{
    int best = 0x7fffffff;
    dist = 0u;
    sel = 0u;
    for (uint di = 0u; di < 8u; di++) {
        int d = ETC2_DIST[di];
        ivec3 pc[4] = ivec3[4](c1, clamp(c2 + d, 0, 255), c2, clamp(c2 - d, 0, 255));
        int e = 0;
        uint s = 0u;
        for (uint p = 0u; p < 16u; p++) {
            if (etc_clear(p)) {
                s |= 2u << (2u * p);
                continue;
            }
            int be = 0x7fffffff;
            uint bk = 0u;
            for (uint k = 0u; k < 4u; k++) {
                if (clear && k == 2u) continue;
                int ee = etc_err(g_px[p].rgb, pc[k]);
                if (ee < be) { be = ee; bk = k; }
            }
            e += be;
            s |= bk << (2u * p);
        }
        if (e < best) { best = e; dist = di; sel = s; }
    }
    return best;
}

int etct_try(bool clear, out uvec2 hl)
{
    ivec3 sum = ivec3(0);
    int n = 0;
    for (uint p = 0u; p < 16u; p++)
        if (!etc_clear(p)) { sum += g_px[p].rgb; n++; }
    ivec3 mean = n > 0 ? sum / n : ivec3(0);
    /* the texel farthest from the mean off the grey axis */
    int far = -1;
    ivec3 seed = mean;
    for (uint p = 0u; p < 16u; p++) {
        if (etc_clear(p)) continue;
        ivec3 dv = g_px[p].rgb - mean;
        int s = dv.x + dv.y + dv.z, o = etc_err(dv, ivec3(0)) - (s * s) / 3;
        if (o > far) { far = o; seed = g_px[p].rgb; }
    }
    ivec3 a = mean, b = seed;
    for (int it = 0; it < 2; it++) {
        ivec3 sa = ivec3(0), sb = ivec3(0);
        int na = 0, nb = 0;
        for (uint p = 0u; p < 16u; p++) {
            if (etc_clear(p)) continue;
            if (etc_err(g_px[p].rgb, a) <= etc_err(g_px[p].rgb, b)) { sa += g_px[p].rgb; na++; }
            else { sb += g_px[p].rgb; nb++; }
        }
        if (na > 0) a = sa / na;
        if (nb > 0) b = sb / nb;
    }
    ivec3 qa = clamp((a * 15 + 127) / 255, 0, 15), qb = clamp((b * 15 + 127) / 255, 0, 15);
    int best = 0x7fffffff;
    hl = uvec2(0u);
    for (int role = 0; role < 2; role++) {
        ivec3 q1 = role == 0 ? qa : qb, q2 = role == 0 ? qb : qa;
        uint dist, sel;
        int e = etct_eval(etc_x4(q1), etc_x4(q2), clear, dist, sel);
        if (e < best) {
            best = e;
            uvec3 c1 = uvec3(q1), c2 = uvec3(q2);
            uint lo2 = c1.r & 3u, hi2 = c1.r >> 2;
            /* R + dR under- or overflows: that selects T mode */
            uint neg = hi2 + lo2 < 4u ? 1u : 0u, top = hi2 + lo2 < 4u ? 0u : 7u;
            uint hi = (dist & 1u) | ((etc_punch ? (clear ? 0u : 1u) : 1u) << 1) | ((dist >> 1) << 2) | (c2.b << 4) | (c2.g << 8) |
                      (c2.r << 12) | (c1.b << 16) | (c1.g << 20) | (lo2 << 24) | (neg << 26) | (hi2 << 27) | (top << 29);
            hl = uvec2(hi, etc_sel(sel));
        }
    }
    return best;
}

/* Planar mode (opaque): colours O, H, V at (0,0), (4,0), (0,4), 6:7:6 bits, a plane through them. */
ivec3 etcp_x(ivec3 q) { return ivec3((q.r << 2) | (q.r >> 4), (q.g << 1) | (q.g >> 6), (q.b << 2) | (q.b >> 4)); }

void etc_put(inout uvec2 hl, uint pos, uint n, uint v) /* bits pos..pos+n-1 of the 64-bit block */
{
    v &= (1u << n) - 1u;
    if (pos >= 32u) hl.x |= v << (pos - 32u);
    else {
        hl.y |= v << pos;
        if (pos + n > 32u) hl.x |= v >> (32u - pos);
    }
}

int etcp_try(out uvec2 hl)
{
    vec3 m = vec3(0.0), gx = vec3(0.0), gy = vec3(0.0);
    for (uint p = 0u; p < 16u; p++) {
        vec3 c = vec3(g_px[p].rgb);
        m += c;
        gx += (float(p & 3u) - 1.5) * c;
        gy += (float(p >> 2) - 1.5) * c;
    }
    m /= 16.0;
    gx /= 20.0;
    gy /= 20.0;
    vec3 o = m - 1.5 * gx - 1.5 * gy, h = o + 4.0 * gx, v = o + 4.0 * gy;
    const vec3 sc = vec3(63.0, 127.0, 63.0) / 255.0;
    ivec3 qo = clamp(ivec3(round(o * sc)), ivec3(0), ivec3(63, 127, 63));
    ivec3 qh = clamp(ivec3(round(h * sc)), ivec3(0), ivec3(63, 127, 63));
    ivec3 qv = clamp(ivec3(round(v * sc)), ivec3(0), ivec3(63, 127, 63));
    ivec3 eo = etcp_x(qo), eh = etcp_x(qh), ev = etcp_x(qv);
    int e = 0;
    for (uint p = 0u; p < 16u; p++) {
        int x = int(p & 3u), y = int(p >> 2);
        ivec3 c = clamp((x * (eh - eo) + y * (ev - eo) + 4 * eo + 2) >> 2, 0, 255);
        e += etc_err(g_px[p].rgb, c);
    }
    uvec3 uo = uvec3(qo), uh = uvec3(qh), uv = uvec3(qv);
    hl = uvec2(0u);
    etc_put(hl, 57u, 6u, uo.r);
    etc_put(hl, 56u, 1u, uo.g >> 6);
    etc_put(hl, 49u, 6u, uo.g);
    etc_put(hl, 48u, 1u, uo.b >> 5);
    etc_put(hl, 43u, 2u, uo.b >> 3);
    etc_put(hl, 39u, 3u, uo.b);
    etc_put(hl, 34u, 5u, uh.r >> 1);
    etc_put(hl, 32u, 1u, uh.r);
    etc_put(hl, 25u, 7u, uh.g);
    etc_put(hl, 19u, 6u, uh.b);
    etc_put(hl, 13u, 6u, uv.r);
    etc_put(hl, 6u, 7u, uv.g);
    etc_put(hl, 0u, 6u, uv.b);
    etc_put(hl, 33u, 1u, 1u);
    /* R and G (5-bit base + 3-bit signed delta) must stay in range, B must leave it */
    int dr = int(((uo.r & 3u) << 1) | (uo.g >> 6));
    dr = dr >= 4 ? dr - 8 : dr;
    if (int(uo.r >> 2) + dr < 0) etc_put(hl, 63u, 1u, 1u);
    int dg = int(((uo.g & 3u) << 1) | (uo.b >> 5));
    dg = dg >= 4 ? dg - 8 : dg;
    if (int((uo.g >> 2) & 15u) + dg < 0) etc_put(hl, 55u, 1u, 1u);
    uint q = (uo.b >> 3) & 3u, r = (uo.b >> 1) & 3u;
    if (q + r < 4u) etc_put(hl, 42u, 1u, 1u);
    else etc_put(hl, 45u, 3u, 7u);
    return e;
}

uvec2 etc2_encode(bool punch)
{
    etc_punch = punch;
    bool clear = false;
    for (uint p = 0u; p < 16u; p++) clear = clear || etc_clear(p);
    uvec2 best, hl;
    int e = etc1_try(false, clear, best), e2;
    e2 = etc1_try(true, clear, hl);
    if (e2 < e) { e = e2; best = hl; }
    e2 = etct_try(clear, hl);
    if (e2 < e) { e = e2; best = hl; }
    if (!clear) {
        e2 = etcp_try(hl);
        if (e2 < e) { e = e2; best = hl; }
    }
    return uvec2(bswap32(best.x), bswap32(best.y));
}
