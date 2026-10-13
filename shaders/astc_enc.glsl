/* ASTC 4x4 LDR block encoder for the Mali bridge's BC transcoding (bc_decode.comp, ENC_ASTC).
 *
 * Ported to GLSL from astc_enc.slang and astc_pack.slang of leegao/etc2_encode (commit a32883c;
 * "Realtime rgba8 -> astc/etc2/etc1 encoder as a compute shader"), whose licence follows. Kept from
 * it: endpoints from the principal axis (power iteration on the covariance), the blue-contraction
 * swap, the block mode and BISE packing, and its trit, quint and endpoint tables (here only the
 * ranges used, rank -> ISE value | decoded value << 8). Changed: float32 instead of half
 * arithmetic; weights picked by the error against the quantized endpoints as a decoder expands
 * them; one least-squares refit of the endpoints to those weights; several weight / endpoint
 * precisions tried per block (2-bit weights with 8-bit endpoints keep BC1/BC3 colour almost
 * exactly), for blocks with alpha a second weight plane for alpha, and for BC7 blocks of 2 or 3
 * subsets 2 partitions with the ASTC partition nearest BC7's (from a 256-entry table instead of
 * upstream's k-means and 48 KB lookup); the lowest error is kept.
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

const uint ASTC_Q48[48] = uint[48](
    0x0000u, 0x0510u, 0x0b20u, 0x1002u, 0x1512u, 0x1b22u, 0x2004u, 0x2614u, 0x2b24u, 0x3006u,
    0x3616u, 0x3b26u, 0x4108u, 0x4618u, 0x4c28u, 0x510au, 0x561au, 0x5c2au, 0x610cu, 0x671cu,
    0x6c2cu, 0x710eu, 0x771eu, 0x7c2eu, 0x832fu, 0x881fu, 0x8e0fu, 0x932du, 0x981du, 0x9e0du,
    0xa32bu, 0xa91bu, 0xae0bu, 0xb329u, 0xb919u, 0xbe09u, 0xc427u, 0xc917u, 0xcf07u, 0xd425u,
    0xd915u, 0xdf05u, 0xe423u, 0xea13u, 0xef03u, 0xf421u, 0xfa11u, 0xff01u);

const uint ASTC_QUINTS[125] = uint[125](
    0u, 1u, 2u, 3u, 4u, 8u, 9u, 10u, 11u, 12u, 16u, 17u, 18u, 19u, 20u, 24u,
    25u, 26u, 27u, 28u, 5u, 13u, 21u, 29u, 6u, 32u, 33u, 34u, 35u, 36u, 40u, 41u,
    42u, 43u, 44u, 48u, 49u, 50u, 51u, 52u, 56u, 57u, 58u, 59u, 60u, 37u, 45u, 53u,
    61u, 14u, 64u, 65u, 66u, 67u, 68u, 72u, 73u, 74u, 75u, 76u, 80u, 81u, 82u, 83u,
    84u, 88u, 89u, 90u, 91u, 92u, 69u, 77u, 85u, 93u, 22u, 96u, 97u, 98u, 99u, 100u,
    104u, 105u, 106u, 107u, 108u, 112u, 113u, 114u, 115u, 116u, 120u, 121u, 122u, 123u, 124u, 101u,
    109u, 117u, 125u, 30u, 102u, 103u, 70u, 71u, 38u, 110u, 111u, 78u, 79u, 46u, 118u, 119u,
    86u, 87u, 54u, 126u, 127u, 94u, 95u, 62u, 39u, 47u, 55u, 63u, 31u);

const uint ASTC_Q40[40] = uint[40](
    0x0000u, 0x0608u, 0x0d10u, 0x1318u, 0x1a20u, 0x2002u, 0x270au, 0x2d12u, 0x341au, 0x3a22u,
    0x4104u, 0x470cu, 0x4e14u, 0x541cu, 0x5b24u, 0x6106u, 0x680eu, 0x6e16u, 0x751eu, 0x7b26u,
    0x8427u, 0x8a1fu, 0x9117u, 0x970fu, 0x9e07u, 0xa425u, 0xab1du, 0xb115u, 0xb80du, 0xbe05u,
    0xc523u, 0xcb1bu, 0xd213u, 0xd80bu, 0xdf03u, 0xe521u, 0xec19u, 0xf211u, 0xf909u, 0xff01u);

const uint ASTC_Q192[192] = uint[192](
    0x0000u, 0x0140u, 0x0280u, 0x0402u, 0x0542u, 0x0682u, 0x0804u, 0x0944u, 0x0a84u, 0x0c06u,
    0x0d46u, 0x0e86u, 0x1008u, 0x1148u, 0x1288u, 0x140au, 0x154au, 0x168au, 0x180cu, 0x194cu,
    0x1a8cu, 0x1c0eu, 0x1d4eu, 0x1e8eu, 0x2010u, 0x2150u, 0x2290u, 0x2412u, 0x2552u, 0x2692u,
    0x2814u, 0x2954u, 0x2a94u, 0x2c16u, 0x2d56u, 0x2e96u, 0x3018u, 0x3158u, 0x3298u, 0x341au,
    0x355au, 0x369au, 0x381cu, 0x395cu, 0x3a9cu, 0x3c1eu, 0x3d5eu, 0x3e9eu, 0x4020u, 0x4160u,
    0x42a0u, 0x4422u, 0x4562u, 0x46a2u, 0x4824u, 0x4964u, 0x4aa4u, 0x4c26u, 0x4d66u, 0x4ea6u,
    0x5028u, 0x5168u, 0x52a8u, 0x542au, 0x556au, 0x56aau, 0x582cu, 0x596cu, 0x5aacu, 0x5c2eu,
    0x5d6eu, 0x5eaeu, 0x6030u, 0x6170u, 0x62b0u, 0x6432u, 0x6572u, 0x66b2u, 0x6834u, 0x6974u,
    0x6ab4u, 0x6c36u, 0x6d76u, 0x6eb6u, 0x7038u, 0x7178u, 0x72b8u, 0x743au, 0x757au, 0x76bau,
    0x783cu, 0x797cu, 0x7abcu, 0x7c3eu, 0x7d7eu, 0x7ebeu, 0x81bfu, 0x827fu, 0x833fu, 0x85bdu,
    0x867du, 0x873du, 0x89bbu, 0x8a7bu, 0x8b3bu, 0x8db9u, 0x8e79u, 0x8f39u, 0x91b7u, 0x9277u,
    0x9337u, 0x95b5u, 0x9675u, 0x9735u, 0x99b3u, 0x9a73u, 0x9b33u, 0x9db1u, 0x9e71u, 0x9f31u,
    0xa1afu, 0xa26fu, 0xa32fu, 0xa5adu, 0xa66du, 0xa72du, 0xa9abu, 0xaa6bu, 0xab2bu, 0xada9u,
    0xae69u, 0xaf29u, 0xb1a7u, 0xb267u, 0xb327u, 0xb5a5u, 0xb665u, 0xb725u, 0xb9a3u, 0xba63u,
    0xbb23u, 0xbda1u, 0xbe61u, 0xbf21u, 0xc19fu, 0xc25fu, 0xc31fu, 0xc59du, 0xc65du, 0xc71du,
    0xc99bu, 0xca5bu, 0xcb1bu, 0xcd99u, 0xce59u, 0xcf19u, 0xd197u, 0xd257u, 0xd317u, 0xd595u,
    0xd655u, 0xd715u, 0xd993u, 0xda53u, 0xdb13u, 0xdd91u, 0xde51u, 0xdf11u, 0xe18fu, 0xe24fu,
    0xe30fu, 0xe58du, 0xe64du, 0xe70du, 0xe98bu, 0xea4bu, 0xeb0bu, 0xed89u, 0xee49u, 0xef09u,
    0xf187u, 0xf247u, 0xf307u, 0xf585u, 0xf645u, 0xf705u, 0xf983u, 0xfa43u, 0xfb03u, 0xfd81u,
    0xfe41u, 0xff01u);

/* Candidates (4x4 weights; bits: 17 header, 2 more for the plane selector, 29 with 2 partitions):
 *   CEM 8 (RGB):   2-bit weights + 8-bit endpoints (32 + 48), 3-bit + 8-bit (48 + 48), 4-bit + QUANT_192 (64 + 46)
 *   CEM 12 (RGBA): 2-bit + 8-bit (32 + 64), 3-bit + QUANT_192 (48 + 61), 4-bit + QUANT_48 (64 + 45),
 *                  dual plane (alpha its own) 2-bit + QUANT_48 (64 + 45 + 2)
 *   2 partitions, CEM 8: 2-bit weights + QUANT_40 endpoints (32 + 64), where the block was a BC7 block
 *                  of 2 or 3 subsets: the ASTC partition closest to BC7's (gen_astc_tables.py).
 * The endpoint range is not the encoder's to choose: a decoder takes the largest that fits the bits
 * the weights leave. */

#include "astc_tables.glsl"

uint g_apart; /* texels (bit t) in partition 1 */

bool astc_in(uint p, uint part) { return ((g_apart >> p) & 1u) == part; }

uint astc_rank(uint q, uint r) { return q == 40u ? ASTC_Q40[r] : (q == 48u ? ASTC_Q48[r] : ASTC_Q192[r]); }

/* The nearest endpoint value of range q (40, 48, 192, 256): ISE value, decoded value. */
uvec2 astc_ep(uint q, float v)
{
    v = clamp(v, 0.0, 255.0);
    if (q == 256u) {
        uint u = uint(v + 0.5);
        return uvec2(u, u);
    }
    uint n = q - 1u, r = uint(v * float(n) / 255.0 + 0.5);
    uint e = astc_rank(q, r), e2 = astc_rank(q, r > 0u ? r - 1u : 0u);
    float d = abs(float(e >> 8) - v);
    if (abs(float(e2 >> 8) - v) < d) { e = e2; d = abs(float(e2 >> 8) - v); }
    e2 = astc_rank(q, min(r + 1u, n));
    if (abs(float(e2 >> 8) - v) < d) e = e2;
    return uvec2(e & 0xffu, e >> 8);
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

/* Weights for decoded endpoints d0, d1 per partition (plane 0 over the channels in m, plane 1 alpha
 * when dual): the error of the block, the indices in g_aw, the chosen weights (0..1) in wf / wa. */
float astc_weights(vec4 d0[2], vec4 d1[2], vec4 m, bool dual, uint wb, out float wf[16], out float wa[16])
{
    uint levels = 1u << wb;
    float e = 0.0;
    vec4 rest = vec4(1.0) - m;
    if (dual) rest.a = 0.0;
    for (uint p = 0u; p < 16u; p++) {
        uint part = (g_apart >> p) & 1u;
        vec4 v = vec4(g_px[p]), a = d0[part], b = d1[part];
        float be = 1e30, ba = 1e30;
        uint bk = 0u, bka = 0u;
        for (uint k = 0u; k < levels; k++) {
            float w = astc_w(wb, k) / 64.0;
            vec4 d = (v - mix(a, b, w));
            float ee = dot(d * d, m);
            if (ee < be) { be = ee; bk = k; }
            if (dual && d.a * d.a < ba) { ba = d.a * d.a; bka = k; }
        }
        vec4 dr = (v - a) * rest;
        e += be + dot(dr, dr) + (dual ? ba : 0.0);
        wf[p] = astc_w(wb, bk) / 64.0;
        wa[p] = astc_w(wb, bka) / 64.0;
        if (dual) {
            g_aw[2u * p] = bk;
            g_aw[2u * p + 1u] = bka;
        } else {
            g_aw[p] = bk;
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

/* One candidate: its error, its block in out. seed: the partition seed with 2 partitions (g_apart
 * set), else ~0u. */
float astc_try(bool rgba, bool dual, uint wb, uint q, uint seed, out uvec4 blk)
{
    vec4 m = rgba && !dual ? vec4(1.0) : vec4(1.0, 1.0, 1.0, 0.0);
    bool two = seed != ~0u;
    uint np = two ? 2u : 1u;
    if (!two) g_apart = 0u;
    vec4 e0[2], e1[2];
    for (uint k = 0u; k < np; k++) {
        astc_line(m, k, e0[k], e1[k]);
        if (!rgba) e0[k].a = e1[k].a = 255.0;
    }
    if (!two) { e0[1] = e0[0]; e1[1] = e1[0]; }
    if (dual) {
        float amin = 255.0, amax = 0.0;
        for (uint p = 0u; p < 16u; p++) { amin = min(amin, float(g_px[p].a)); amax = max(amax, float(g_px[p].a)); }
        e0[0].a = amin;
        e1[0].a = amax;
    }
    uint nv = rgba ? 8u : 6u;
    float best = 1e30;
    blk = uvec4(0u);
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
        if (pass == 0) {
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

/* part: for a BC7 block of 2 or 3 subsets, its entry in ASTC_BC7P (seed << 16 | partition-1
 * texels), else ~0u. */
uvec4 astc_encode(uint part)
{
    bool alpha = false;
    for (uint p = 0u; p < 16u; p++) alpha = alpha || g_px[p].a != 255;
    uvec4 best, b;
    float e, e2;
    if (!alpha) {
        e = astc_try(false, false, 2u, 256u, ~0u, best);
        e2 = astc_try(false, false, 3u, 256u, ~0u, b);
        if (e2 < e) { e = e2; best = b; }
        e2 = astc_try(false, false, 4u, 192u, ~0u, b);
        if (e2 < e) { e = e2; best = b; }
        if (part != ~0u) {
            g_apart = part & 0xffffu;
            e2 = astc_try(false, false, 2u, 40u, part >> 16, b);
            if (e2 < e) { e = e2; best = b; }
        }
    } else {
        e = astc_try(true, false, 2u, 256u, ~0u, best);
        e2 = astc_try(true, false, 3u, 192u, ~0u, b);
        if (e2 < e) { e = e2; best = b; }
        e2 = astc_try(true, false, 4u, 48u, ~0u, b);
        if (e2 < e) { e = e2; best = b; }
        e2 = astc_try(true, true, 2u, 48u, ~0u, b);
        if (e2 < e) { e = e2; best = b; }
    }
    return best;
}
