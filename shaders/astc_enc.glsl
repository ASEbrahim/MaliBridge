/* ASTC 4x4 LDR block encoder for the Mali bridge's BC transcoding (bc_decode.comp, ENC_ASTC).
 *
 * Ported to GLSL from astc_enc.slang and astc_pack.slang of leegao/etc2_encode (commit a32883c;
 * "Realtime rgba8 -> astc/etc2/etc1 encoder as a compute shader"), whose licence follows. Kept from
 * it: endpoints from the principal axis (power iteration on the covariance), the blue-contraction
 * swap, the block mode and BISE packing. Changed: float32 instead of half
 * arithmetic; weights picked by the error against the quantized endpoints as a decoder expands
 * them; one least-squares refit of the endpoints to those weights; up to two weight / endpoint
 * precisions tried per block, the second only when the first is not good enough (2-bit weights with
 * 8-bit endpoints keep BC1/BC3 colour almost exactly), for blocks with alpha a second weight plane
 * for alpha, and for BC7 blocks of 2 or 3 subsets 2 partitions with the ASTC partition nearest
 * BC7's (from a 256-entry table instead of upstream's k-means and 48 KB lookup); the lowest error
 * is kept. A block of one colour is a void-extent block. BC_QUALITY 0 (fast): no refit, a shorter
 * power iteration, a second candidate only for a poor block, and BC7's 1-partition blocks one
 * candidate; 2 (high): every candidate, and BC7's 2-partition blocks 1 partition too.
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
 * Input: g_px[16] (row-major texels, 8-bit RGBA). Output: the 128-bit block, little-endian words.
 */

/* Candidates (4x4 weights; bits: 17 header, 2 more for the plane selector, 29 with 2 partitions):
 *   CEM 8 (RGB):   2-bit weights + 8-bit endpoints (32 + 48), 3-bit + 8-bit (48 + 48), 4-bit + QUANT_192 (64 + 46)
 *   CEM 12 (RGBA): 3-bit + QUANT_192 (48 + 61), 4-bit + QUANT_48 (64 + 45),
 *                  dual plane (alpha its own) 2-bit + QUANT_48 (64 + 45 + 2)
 *   2 partitions, CEM 8: 2-bit weights + QUANT_40 endpoints (32 + 64), where the block was a BC7 block
 *                  of 2 or 3 subsets: the ASTC partition closest to BC7's (gen_astc_tables.py).
 * The endpoint range is not the encoder's to choose: a decoder takes the largest that fits the bits
 * the weights leave. */

#include "astc_tables.glsl"

uint g_apart; /* texels (bit t) in partition 1 */

bool astc_in(uint p, uint part) { return ((g_apart >> p) & 1u) == part; }

/* Rank r (by decoded value) of endpoint range q (40, 48, 192, 256): ISE value, decoded value. A trit
 * or quint range's lower half has the low bit clear and its upper half mirrors it; the decoded value
 * is the specification's unquantization (T = D * C + B, inverted by the low bit). */
uvec2 astc_rank(uint q, uint r)
{
    if (q == 256u) return uvec2(r, r);
    bool up = r >= (q >> 1);
    uint rr = up ? q - 1u - r : r;
    /* trit / quint, the bits above the low one (rr / 5 or rr / 3 by multiplication; rr < 96) */
    uint h = q == 40u ? (rr * 52429u) >> 18 : (rr * 43691u) >> 17, t = rr - h * (q == 40u ? 5u : 3u);
    uint nb, B, C;
    if (q == 192u) { nb = 6u; B = (h << 4) | (h >> 4); C = 5u; }
    else if (q == 48u) { nb = 4u; B = (h << 6) | h; C = 22u; }
    else { nb = 3u; B = (h << 7) | (h << 1) | (h >> 1); C = 26u; }
    uint T = t * C + B;
    if (up) T ^= 0x1ffu;
    return uvec2((t << nb) | (h << 1) | (up ? 1u : 0u), (up ? 0x80u : 0u) | (T >> 2));
}

/* The nearest endpoint value of range q: ISE value, decoded value. */
uvec2 astc_ep(uint q, float v)
{
    v = clamp(v, 0.0, 255.0);
    if (q == 256u) {
        uint u = uint(v + 0.5);
        return uvec2(u, u);
    }
    uint n = q - 1u, r = uint(v * float(n) / 255.0 + 0.5);
    /* the decoded values grow with the rank and lie within a step of uniform: the nearest is r or its
     * neighbour on v's side */
    uvec2 e = astc_rank(q, r);
    uint r2 = float(e.y) < v ? min(r + 1u, n) : (r > 0u ? r - 1u : 0u);
    uvec2 e2 = astc_rank(q, r2);
    return abs(float(e2.y) - v) < abs(float(e.y) - v) ? e2 : e;
}

/* Weight value (0..64) of index k in a b-bit weight range (bit replication). */
float astc_w(uint b, uint k)
{
    uint w = b == 2u ? k * 21u : (b == 3u ? k * 9u : (k << 2) | (k >> 2));
    return float(w > 32u ? w + 1u : w);
}

/* The line through partition part's texels (channels in m) along their principal axis, ends at the
 * extremes. */
void astc_line(vec4 m, uint part, out vec4 lo, out vec4 hi)
{
    vec4 c = vec4(0.0), mn = vec4(255.0), mx = vec4(0.0);
    float n = 0.0;
    for (uint p = 0u; p < 16u; p++) {
        if (!astc_in(p, part)) continue;
        vec4 v = vec4(g_px[p]);
        c += v;
        mn = min(mn, v);
        mx = max(mx, v);
        n += 1.0;
    }
    c /= max(n, 1.0);
    mat4 cov = mat4(0.0);
    for (uint p = 0u; p < 16u; p++) {
        if (!astc_in(p, part)) continue;
        vec4 d = (vec4(g_px[p]) - c) * m;
        cov += outerProduct(d, d);
    }
    vec4 ax = (mx - mn) * m;
    lo = c;
    hi = c;
    if (dot(ax, ax) < 1e-4) return;
    for (int i = 0; i < (BC_QUALITY == 0u ? 2 : 6); i++) {
        vec4 nx = cov * ax;
        float l = dot(nx, nx);
        if (l < 1e-12) break;
        ax = nx * inversesqrt(l);
    }
    ax = normalize(ax);
    float t0 = 1e9, t1 = -1e9;
    for (uint p = 0u; p < 16u; p++) {
        if (!astc_in(p, part)) continue;
        float t = dot((vec4(g_px[p]) - c) * m, ax);
        t0 = min(t0, t);
        t1 = max(t1, t);
    }
    lo = clamp(c + ax * t0, 0.0, 255.0);
    hi = clamp(c + ax * t1, 0.0, 255.0);
}

void astc_put(inout uvec4 b, uint pos, uint n, uint v)
{
    v &= (1u << n) - 1u;
    uint w = pos >> 5, o = pos & 31u;
    b[w] |= v << o;
    if (o + n > 32u) b[w + 1u] |= v >> (32u - o);
}

/* 5 trits as the 8 bits T of the specification's integer sequence encoding, 3 quints as its 7 bits
 * Q (its decoding procedures inverted; where bits are free they are 0). */
uint astc_trits(uint t0, uint t1, uint t2, uint t3, uint t4)
{
    uint c = t2 == 2u ? (t1 == 2u ? 12u | t0 : (t1 << 4) | (t0 << 2) | 3u) : (t2 << 4) | (t1 << 2) | t0;
    if (t3 == 2u && t4 == 2u) return ((c >> 2) << 5) | 0x1cu | (c & 3u);
    return t4 == 2u ? (t3 << 7) | 0x60u | c : (t4 << 7) | (t3 << 5) | c;
}

uint astc_quints(uint q0, uint q1, uint q2)
{
    if (q1 == 4u && q0 == 4u) return 6u | (q2 == 4u ? 1u : ((q2 >> 1) << 4) | ((q2 & 1u) << 3));
    uint c = q1 == 4u ? (q0 << 3) | 5u : (q1 << 3) | q0;
    return q2 == 4u ? (c & 0x18u) | ((~(c >> 1) & 3u) << 5) | 6u | (c & 1u) : (q2 << 5) | c;
}

/* Endpoints in BISE: 8 bits each, quints (QUANT_40: 3 bits) in groups of 3, or trits (QUANT_48:
 * 4 bits, QUANT_192: 6 bits) in groups of 5, each value's bits followed by its share of the group's
 * Q (3, 2, 2 bits) or T (2, 2, 1, 2, 1). */
uint astc_ise(inout uvec4 b, uint pos, uint q, uint v[16], uint n)
{
    if (q == 256u) {
        for (uint i = 0u; i < n; i++) astc_put(b, pos + 8u * i, 8u, v[i]);
        return pos + 8u * n;
    }
    uint nb = q == 40u ? 3u : (q == 48u ? 4u : 6u), gs = q == 40u ? 3u : 5u;
    for (uint g = 0u; g < n; g += gs) {
        uint x[5];
        for (uint j = 0u; j < 5u; j++) x[j] = g + j < n ? v[g + j] >> nb : 0u;
        uint tq = q == 40u ? astc_quints(x[0], x[1], x[2]) : astc_trits(x[0], x[1], x[2], x[3], x[4]);
        for (uint j = 0u; j < gs && g + j < n; j++) {
            astc_put(b, pos, nb, v[g + j]);
            pos += nb;
            uint k = q == 40u ? (j == 0u ? 3u : 2u) : (j == 2u || j == 4u ? 1u : 2u);
            uint sh = q == 40u ? (j == 0u ? 0u : 2u * j + 1u) : 2u * j - (j >= 3u ? 1u : 0u);
            astc_put(b, pos, k, tq >> sh);
            pos += k;
        }
    }
    return pos;
}

/* The level of a b-bit weight range nearest t (0..1): the levels lie within half a step of uniform,
 * so it is the rounded index or its neighbour on t's side. */
uint astc_wq(uint b, float t)
{
    uint n = (1u << b) - 1u, k = uint(clamp(t, 0.0, 1.0) * float(n) + 0.5);
    float wk = astc_w(b, k);
    t *= 64.0;
    uint k2 = wk < t ? min(k + 1u, n) : (k > 0u ? k - 1u : 0u);
    return abs(astc_w(b, k2) - t) < abs(wk - t) ? k2 : k;
}

/* n bits of v at bit pos of a 64-bit field. */
void astc_put64(inout uvec2 w, uint pos, uint n, uint v)
{
    if (pos >= 32u) {
        w.y |= v << (pos - 32u);
    } else {
        w.x |= v << pos;
        if (pos + n > 32u) w.y |= v >> (32u - pos);
    }
}

/* Weights for decoded endpoints d0, d1 per partition (plane 0 over the channels in m, plane 1 alpha
 * when dual): the error of the block and the weight indices in wi (in texel order, interleaved by
 * plane when dual: at most 64 bits). A texel's error is quadratic in the weight along the
 * endpoints' line, so its best level is the one nearest its projection on the line. Also, in r0 /
 * r1, the endpoints refitted to those weights by least squares (d0 / d1 where that is singular). */
float astc_weights(vec4 d0[2], vec4 d1[2], vec4 m, bool dual, uint wb, out uvec2 wi, out vec4 r0[2], out vec4 r1[2])
{
    float e = 0.0;
    vec4 rest = vec4(1.0) - m;
    if (dual) rest.a = 0.0;
    vec4 ax[2], s0[2], s1[2];
    float sa[2], sb[2], sc[2];
    for (uint k = 0u; k < 2u; k++) {
        ax[k] = (d1[k] - d0[k]) * m;
        float l = dot(ax[k], ax[k]);
        ax[k] *= l > 0.0 ? 1.0 / l : 0.0;
        s0[k] = s1[k] = vec4(0.0);
        sa[k] = sb[k] = sc[k] = 0.0;
    }
    float ra = d1[0].a - d0[0].a;
    ra = ra != 0.0 ? 1.0 / ra : 0.0;
    vec4 al = vec4(0.0); /* dual: alpha's sums (y * y, y * x, x * x, y * a), x * a in a1 */
    float a1 = 0.0;
    wi = uvec2(0u);
    uint pos = 0u;
    for (uint p = 0u; p < 16u; p++) {
        uint part = (g_apart >> p) & 1u;
        vec4 v = vec4(g_px[p]), a = d0[part], b = d1[part];
        uint k = astc_wq(wb, dot((v - a) * m, ax[part]));
        float x = astc_w(wb, k) / 64.0, y = 1.0 - x;
        vec4 d = (v - mix(a, b, x)) * m + (v - a) * rest;
        e += dot(d, d);
        sa[part] += y * y;
        sb[part] += y * x;
        sc[part] += x * x;
        s0[part] += y * v;
        s1[part] += x * v;
        astc_put64(wi, pos, wb, k);
        pos += wb;
        if (dual) {
            uint ka = astc_wq(wb, (v.a - a.a) * ra);
            float xa = astc_w(wb, ka) / 64.0, ya = 1.0 - xa;
            float da = v.a - mix(a.a, b.a, xa);
            e += da * da;
            al += vec4(ya * ya, ya * xa, xa * xa, ya * v.a);
            a1 += xa * v.a;
            astc_put64(wi, pos, wb, ka);
            pos += wb;
        }
    }
    for (uint k = 0u; k < 2u; k++) {
        r0[k] = d0[k];
        r1[k] = d1[k];
        float det = sa[k] * sc[k] - sb[k] * sb[k];
        if (abs(det) < 1e-3) continue;
        r0[k] = mix(r0[k], clamp((sc[k] * s0[k] - sb[k] * s1[k]) / det, 0.0, 255.0), m);
        r1[k] = mix(r1[k], clamp((sa[k] * s1[k] - sb[k] * s0[k]) / det, 0.0, 255.0), m);
    }
    float det = al.x * al.z - al.y * al.y;
    if (dual && abs(det) >= 1e-3) {
        r0[0].a = clamp((al.z * al.w - al.y * a1) / det, 0.0, 255.0);
        r1[0].a = clamp((al.x * a1 - al.y * al.w) / det, 0.0, 255.0);
    }
    return e;
}

/* One candidate from endpoints l0, l1 per partition (a principal axis' extremes; with dual, alpha's
 * extremes in .a): the lower of its error and limit (the best error so far), and its block in blk
 * where it is the lower. seed: the partition seed with 2 partitions (g_apart set), else ~0u. */
float astc_try(bool rgba, bool dual, uint wb, uint q, uint seed, vec4 l0[2], vec4 l1[2], float limit, inout uvec4 blk)
{
    vec4 m = rgba && !dual ? vec4(1.0) : vec4(1.0, 1.0, 1.0, 0.0);
    bool two = seed != ~0u;
    uint np = two ? 2u : 1u;
    vec4 e0[2], e1[2];
    for (uint k = 0u; k < 2u; k++) {
        e0[k] = l0[k];
        e1[k] = l1[k];
        if (!rgba) e0[k].a = e1[k].a = 255.0;
    }
    uint nv = rgba ? 8u : 6u;
    float best = limit;
    for (int pass = 0; pass < 2; pass++) {
        uint v[16];
        vec4 d0[2], d1[2];
        for (uint k = 0u; k < np; k++) {
            for (uint c = 0u; c < 4u; c++) {
                uvec2 a = astc_ep(q, e0[k][c]), b = astc_ep(q, e1[k][c]);
                v[k * nv + 2u * c] = a.x;
                if (2u * c + 1u < nv) v[k * nv + 2u * c + 1u] = b.x;
                d0[k][c] = float(a.y);
                d1[k][c] = float(b.y);
            }
            if (!rgba) d0[k].a = d1[k].a = 255.0;
            /* A decoder blue-contracts endpoints whose second colour sums lower: swap them. */
            if (d1[k].r + d1[k].g + d1[k].b < d0[k].r + d0[k].g + d0[k].b) {
                vec4 t = d0[k]; d0[k] = d1[k]; d1[k] = t;
                for (uint c = 0u; 2u * c < nv; c++) {
                    uint x = v[k * nv + 2u * c];
                    v[k * nv + 2u * c] = v[k * nv + 2u * c + 1u];
                    v[k * nv + 2u * c + 1u] = x;
                }
            }
        }
        if (!two) { d0[1] = d0[0]; d1[1] = d1[0]; }
        uvec2 wi;
        float e = astc_weights(d0, d1, m, dual, wb, wi, e0, e1);
        if (e < best) {
            best = e;
            /* block mode: 4x4 weights (A = 2, B = 0), range R/H, D for the second plane */
            uint r = wb == 2u ? 4u : (wb == 3u ? 7u : 4u), h = wb == 4u ? 1u : 0u;
            uint bm = ((r >> 1) & 3u) | ((r & 1u) << 4) | (2u << 5) | (h << 9) | ((dual ? 1u : 0u) << 10);
            uvec4 b = uvec4(0u);
            astc_put(b, 0u, 11u, bm);
            if (two) {
                astc_put(b, 11u, 2u, 1u);
                astc_put(b, 13u, 10u, seed);
                astc_put(b, 25u, 4u, rgba ? 12u : 8u); /* bits 23-24 zero: every partition this CEM */
                astc_ise(b, 29u, q, v, 2u * nv);
            } else {
                astc_put(b, 13u, 4u, rgba ? 12u : 8u);
                astc_ise(b, 17u, q, v, nv);
            }
            if (dual) astc_put(b, 128u - 64u - 2u, 2u, 3u); /* plane 2 is alpha (after 64 weight bits) */
            /* weights run from the top of the block down */
            blk = b | uvec4(0u, 0u, bitfieldReverse(wi.y), bitfieldReverse(wi.x));
        }
        if (pass == 0 && (e == 0.0 || BC_QUALITY == 0u)) break;
        /* pass 1: the endpoints refitted to pass 0's weights (e0 / e1) */
    }
    return best;
}

/* A block error (squared differences summed over the texels' channels, 8-bit units) good enough to
 * stop at: about 0.7 a texel, under a decoder's own rounding. */
const float ASTC_GOOD = 8.0;

/* The error past which the next candidate is tried: fast, about 2 a texel; high, any. */
float astc_good() { return BC_QUALITY == 0u ? 64.0 : (BC_QUALITY == 1u ? ASTC_GOOD : -1.0); }

/* part: for a BC7 block of 2 or 3 subsets, its entry in ASTC_BC7P (seed << 16 | partition-1
 * texels), else ~0u. fine: BC7 (BC2 / BC3 colour is BC1's: 4 shades on a 5:6:5 line, which 2-bit
 * weights and 8-bit endpoints keep almost exactly).
 *
 * Candidates are tried from the one that wins most on such content, until one is good enough; on
 * bcn_check's images (photo, rough, gradient, soft alpha) each of the others won on few blocks or by
 * a small margin. Without alpha: BC2 / BC3 2-bit weights and 8-bit endpoints, then 4-bit weights
 * and QUANT_192; BC7 3-bit and 8-bit, then 4-bit and QUANT_192, or for a BC7 block of 2 or 3 subsets
 * its 2 partitions first and 4-bit weights only after. With alpha: 3-bit weights and QUANT_192, then
 * for BC2 / BC3 a second plane for alpha (2-bit, QUANT_48: their alpha seldom follows the colour),
 * for BC7 4-bit weights and QUANT_48 (high: then a second plane as well). */
uvec4 astc_encode(uint part, bool fine)
{
    bool alpha = false, same = true;
    for (uint p = 0u; p < 16u; p++) {
        alpha = alpha || g_px[p].a != 255;
        same = same && g_px[p] == g_px[0];
    }
    if (same) {
        /* one colour: a void-extent block, the colour in UNORM16 */
        uvec4 c = uvec4(g_px[0]) * 257u;
        return uvec4(0xfffffdfcu, 0xffffffffu, c.r | (c.g << 16), c.b | (c.a << 16));
    }
    g_apart = 0u;
    vec4 s0[2], s1[2]; /* one partition: the line over the colour (with alpha when that varies) */
    astc_line(alpha ? vec4(1.0) : vec4(1.0, 1.0, 1.0, 0.0), 0u, s0[0], s1[0]);
    s0[1] = s0[0];
    s1[1] = s1[0];
    uvec4 blk = uvec4(0u);
    float e;
    if (alpha) {
        e = astc_try(true, false, 3u, 192u, ~0u, s0, s1, 1e30, blk);
        if (e <= astc_good()) return blk;
        if (fine) {
            e = astc_try(true, false, 4u, 48u, ~0u, s0, s1, e, blk);
            if (BC_QUALITY < 2u) return blk;
        }
        /* dual plane: the colour's own line, alpha from its extremes */
        vec4 d0[2], d1[2];
        astc_line(vec4(1.0, 1.0, 1.0, 0.0), 0u, d0[0], d1[0]);
        float amin = 255.0, amax = 0.0;
        for (uint p = 0u; p < 16u; p++) {
            amin = min(amin, float(g_px[p].a));
            amax = max(amax, float(g_px[p].a));
        }
        d0[0].a = amin;
        d1[0].a = amax;
        d0[1] = d0[0];
        d1[1] = d1[0];
        astc_try(true, true, 2u, 48u, ~0u, d0, d1, e, blk);
        return blk;
    }
    if (part != ~0u) {
        g_apart = part & 0xffffu;
        vec4 p0[2], p1[2];
        astc_line(vec4(1.0, 1.0, 1.0, 0.0), 0u, p0[0], p1[0]);
        astc_line(vec4(1.0, 1.0, 1.0, 0.0), 1u, p0[1], p1[1]);
        e = astc_try(false, false, 2u, 40u, part >> 16, p0, p1, 1e30, blk);
        g_apart = 0u;
        if (BC_QUALITY == 2u) e = astc_try(false, false, 3u, 256u, ~0u, s0, s1, e, blk);
    } else if (fine && BC_QUALITY == 0u) {
        astc_try(false, false, 4u, 192u, ~0u, s0, s1, 1e30, blk); /* the usual winner on BC7 */
        return blk;
    } else {
        e = fine ? astc_try(false, false, 3u, 256u, ~0u, s0, s1, 1e30, blk) : astc_try(false, false, 2u, 256u, ~0u, s0, s1, 1e30, blk);
    }
    if (e > astc_good()) astc_try(false, false, 4u, 192u, ~0u, s0, s1, e, blk);
    return blk;
}
