/* ASTC 4x4 LDR block encoder for the Mali bridge's BC transcoding (bc_decode.comp, ENC_ASTC).
 *
 * Ported to GLSL from astc_enc.slang and astc_pack.slang of leegao/etc2_encode (commit a32883c;
 * "Realtime rgba8 -> astc/etc2/etc1 encoder as a compute shader"), whose licence follows. Kept from
 * it: endpoints from the principal axis (power iteration on the covariance), the blue-contraction
 * swap, the block mode and BISE packing, and its trit and quint tables. Changed: float32 instead of half
 * arithmetic; weights picked by the error against the quantized endpoints as a decoder expands
 * them; one least-squares refit of the endpoints to those weights; up to two weight / endpoint
 * precisions tried per block, the second only when the first is not good enough (2-bit weights with
 * 8-bit endpoints keep BC1/BC3 colour almost exactly), for blocks with alpha a second weight plane
 * for alpha, and for BC7 blocks of 2 or 3 subsets 2 partitions with the ASTC partition nearest
 * BC7's (from a 256-entry table instead of upstream's k-means and 48 KB lookup); the lowest error
 * is kept. A block of one colour is a void-extent block.
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

const uint ASTC_TRITS[243] = uint[243](
    0u, 1u, 2u, 4u, 5u, 6u, 8u, 9u, 10u, 16u, 17u, 18u, 20u, 21u, 22u, 24u,
    25u, 26u, 3u, 7u, 15u, 19u, 23u, 27u, 12u, 13u, 14u, 32u, 33u, 34u, 36u, 37u,
    38u, 40u, 41u, 42u, 48u, 49u, 50u, 52u, 53u, 54u, 56u, 57u, 58u, 35u, 39u, 47u,
    51u, 55u, 59u, 44u, 45u, 46u, 64u, 65u, 66u, 68u, 69u, 70u, 72u, 73u, 74u, 80u,
    81u, 82u, 84u, 85u, 86u, 88u, 89u, 90u, 67u, 71u, 79u, 83u, 87u, 91u, 76u, 77u,
    78u, 128u, 129u, 130u, 132u, 133u, 134u, 136u, 137u, 138u, 144u, 145u, 146u, 148u, 149u, 150u,
    152u, 153u, 154u, 131u, 135u, 143u, 147u, 151u, 155u, 140u, 141u, 142u, 160u, 161u, 162u, 164u,
    165u, 166u, 168u, 169u, 170u, 176u, 177u, 178u, 180u, 181u, 182u, 184u, 185u, 186u, 163u, 167u,
    175u, 179u, 183u, 187u, 172u, 173u, 174u, 192u, 193u, 194u, 196u, 197u, 198u, 200u, 201u, 202u,
    208u, 209u, 210u, 212u, 213u, 214u, 216u, 217u, 218u, 195u, 199u, 207u, 211u, 215u, 219u, 204u,
    205u, 206u, 96u, 97u, 98u, 100u, 101u, 102u, 104u, 105u, 106u, 112u, 113u, 114u, 116u, 117u,
    118u, 120u, 121u, 122u, 99u, 103u, 111u, 115u, 119u, 123u, 108u, 109u, 110u, 224u, 225u, 226u,
    228u, 229u, 230u, 232u, 233u, 234u, 240u, 241u, 242u, 244u, 245u, 246u, 248u, 249u, 250u, 227u,
    231u, 239u, 243u, 247u, 251u, 236u, 237u, 238u, 28u, 29u, 30u, 60u, 61u, 62u, 92u, 93u,
    94u, 156u, 157u, 158u, 188u, 189u, 190u, 220u, 221u, 222u, 31u, 63u, 127u, 159u, 191u, 255u,
    252u, 253u, 254u);

const uint ASTC_QUINTS[125] = uint[125](
    0u, 1u, 2u, 3u, 4u, 8u, 9u, 10u, 11u, 12u, 16u, 17u, 18u, 19u, 20u, 24u,
    25u, 26u, 27u, 28u, 5u, 13u, 21u, 29u, 6u, 32u, 33u, 34u, 35u, 36u, 40u, 41u,
    42u, 43u, 44u, 48u, 49u, 50u, 51u, 52u, 56u, 57u, 58u, 59u, 60u, 37u, 45u, 53u,
    61u, 14u, 64u, 65u, 66u, 67u, 68u, 72u, 73u, 74u, 75u, 76u, 80u, 81u, 82u, 83u,
    84u, 88u, 89u, 90u, 91u, 92u, 69u, 77u, 85u, 93u, 22u, 96u, 97u, 98u, 99u, 100u,
    104u, 105u, 106u, 107u, 108u, 112u, 113u, 114u, 115u, 116u, 120u, 121u, 122u, 123u, 124u, 101u,
    109u, 117u, 125u, 30u, 102u, 103u, 70u, 71u, 38u, 110u, 111u, 78u, 79u, 46u, 118u, 119u,
    86u, 87u, 54u, 126u, 127u, 94u, 95u, 62u, 39u, 47u, 55u, 63u, 31u);

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
    for (int i = 0; i < 6; i++) {
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

/* Endpoints in BISE: 8 bits each, quints (QUANT_40: 3 bits) in groups of 3, or trits (QUANT_48:
 * 4 bits, QUANT_192: 6 bits) in groups of 5. */
uint astc_ise(inout uvec4 b, uint pos, uint q, uint v[16], uint n)
{
    if (q == 256u) {
        for (uint i = 0u; i < n; i++) astc_put(b, pos + 8u * i, 8u, v[i]);
        return pos + 8u * n;
    }
    if (q == 40u) {
        for (uint g = 0u; g < n; g += 3u) {
            uint x0 = v[g], x1 = g + 1u < n ? v[g + 1u] : 0u, x2 = g + 2u < n ? v[g + 2u] : 0u;
            uint Q = ASTC_QUINTS[(x2 >> 3) * 25u + (x1 >> 3) * 5u + (x0 >> 3)];
            const uint qb[3] = uint[3](3u, 2u, 2u), qs[3] = uint[3](0u, 3u, 5u);
            uint xs[3] = uint[3](x0, x1, x2);
            for (uint j = 0u; j < 3u && g + j < n; j++) {
                astc_put(b, pos, 3u, xs[j]);
                pos += 3u;
                astc_put(b, pos, qb[j], Q >> qs[j]);
                pos += qb[j];
            }
        }
        return pos;
    }
    uint nb = q == 48u ? 4u : 6u;
    for (uint g = 0u; g < n; g += 5u) {
        uint t[5], m[5];
        for (uint j = 0u; j < 5u; j++) {
            uint x = g + j < n ? v[g + j] : 0u;
            m[j] = x & ((1u << nb) - 1u);
            t[j] = x >> nb;
        }
        uint T = ASTC_TRITS[t[4] * 81u + t[3] * 27u + t[2] * 9u + t[1] * 3u + t[0]];
        const uint tb[5] = uint[5](2u, 2u, 1u, 2u, 1u), ts[5] = uint[5](0u, 2u, 4u, 5u, 7u);
        for (uint j = 0u; j < 5u && g + j < n; j++) {
            astc_put(b, pos, nb, m[j]);
            pos += nb;
            astc_put(b, pos, tb[j], T >> ts[j]);
            pos += tb[j];
        }
    }
    return pos;
}

uint g_aw[32]; /* the candidate's weight indices (interleaved by plane when dual) */

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

/* Weights for decoded endpoints d0, d1 per partition (plane 0 over the channels in m, plane 1 alpha
 * when dual): the error of the block, the chosen weights (0..1) in wf / wa, their indices in g_aw. A texel's error is quadratic in the weight along the endpoints' line, so its best level
 * is the one nearest its projection on the line. */
float astc_weights(vec4 d0[2], vec4 d1[2], vec4 m, bool dual, uint wb, out float wf[16], out float wa[16])
{
    float e = 0.0;
    vec4 rest = vec4(1.0) - m;
    if (dual) rest.a = 0.0;
    vec4 ax[2];
    for (uint k = 0u; k < 2u; k++) {
        ax[k] = (d1[k] - d0[k]) * m;
        float l = dot(ax[k], ax[k]);
        ax[k] *= l > 0.0 ? 1.0 / l : 0.0;
    }
    float ra = d1[0].a - d0[0].a;
    ra = ra != 0.0 ? 1.0 / ra : 0.0;
    for (uint p = 0u; p < 16u; p++) {
        uint part = (g_apart >> p) & 1u;
        vec4 v = vec4(g_px[p]), a = d0[part], b = d1[part];
        uint k = astc_wq(wb, dot((v - a) * m, ax[part]));
        float w = astc_w(wb, k) / 64.0;
        vec4 d = (v - mix(a, b, w)) * m + (v - a) * rest;
        e += dot(d, d);
        wf[p] = w;
        if (dual) {
            uint ka = astc_wq(wb, (v.a - a.a) * ra);
            wa[p] = astc_w(wb, ka) / 64.0;
            float da = v.a - mix(a.a, b.a, wa[p]);
            e += da * da;
            g_aw[2u * p] = k;
            g_aw[2u * p + 1u] = ka;
        } else {
            g_aw[p] = k;
        }
    }
    return e;
}

/* Least-squares endpoints of partition part for weights w over the channels in m. */
void astc_refit(vec4 m, uint part, float w[16], inout vec4 e0, inout vec4 e1)
{
    float a = 0.0, b = 0.0, c = 0.0;
    vec4 s0 = vec4(0.0), s1 = vec4(0.0);
    for (uint p = 0u; p < 16u; p++) {
        if (!astc_in(p, part)) continue;
        float x = w[p], y = 1.0 - x;
        vec4 v = vec4(g_px[p]);
        a += y * y;
        b += y * x;
        c += x * x;
        s0 += y * v;
        s1 += x * v;
    }
    float det = a * c - b * b;
    if (abs(det) < 1e-3) return;
    e0 = mix(e0, clamp((c * s0 - b * s1) / det, 0.0, 255.0), m);
    e1 = mix(e1, clamp((a * s1 - b * s0) / det, 0.0, 255.0), m);
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
        float wf[16], wa[16];
        float e = astc_weights(d0, d1, m, dual, wb, wf, wa);
        if (e < best) {
            best = e;
            {
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
                uint nw = dual ? 32u : 16u, wbits = nw * wb;
                if (dual) astc_put(b, 128u - wbits - 2u, 2u, 3u); /* plane 2 is alpha */
                uvec4 w = uvec4(0u);
                for (uint i = 0u; i < nw; i++) astc_put(w, i * wb, wb, g_aw[i]);
                /* weights run from the top of the block down */
                b |= uvec4(bitfieldReverse(w.w), bitfieldReverse(w.z), bitfieldReverse(w.y), bitfieldReverse(w.x));
                blk = b;
            }
        }
        if (pass == 0) {
            if (e == 0.0) break;
            for (uint k = 0u; k < np; k++) {
                e0[k] = d0[k];
                e1[k] = d1[k];
                astc_refit(m, k, wf, e0[k], e1[k]);
            }
            if (dual) astc_refit(vec4(0.0, 0.0, 0.0, 1.0), 0u, wa, e0[0], e1[0]);
        }
    }
    return best;
}

/* A block error (squared differences summed over the texels' channels, 8-bit units) good enough to
 * stop at: about 0.7 a texel, under a decoder's own rounding. */
const float ASTC_GOOD = 8.0;

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
 * for BC7 4-bit weights and QUANT_48. */
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
        if (e <= ASTC_GOOD) return blk;
        if (fine) {
            astc_try(true, false, 4u, 48u, ~0u, s0, s1, e, blk);
            return blk;
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
    } else {
        e = fine ? astc_try(false, false, 3u, 256u, ~0u, s0, s1, 1e30, blk) : astc_try(false, false, 2u, 256u, ~0u, s0, s1, 1e30, blk);
    }
    if (e > ASTC_GOOD) astc_try(false, false, 4u, 192u, ~0u, s0, s1, e, blk);
    return blk;
}
