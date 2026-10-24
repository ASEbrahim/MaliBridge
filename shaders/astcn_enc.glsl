/* ASTC 6x6 / 8x8 LDR block encoder (ASTC_N: 6 or 8) for the Mali bridge's BC transcoding
 * (bc_decode.comp, ENC_ASTCN; VKBRIDGE_BC_ASTC_BLOCK). An ASTC block is always 128 bits, so these
 * hold a texture in 2.25 (6x6, 3.56 bits a texel) or 4 (8x8, 2 bits) times fewer bytes than ASTC
 * 4x4, at a cost in detail.
 *
 * Built on astc_enc.glsl (included before this file), which is ported from leegao/etc2_encode
 * (MIT, Lee Gao; the licence is in that file and in third_party/etc2_encode/LICENSE): its bit and
 * integer-sequence packing (astc_put, astc_trits, astc_quints), and the way it fits endpoints - the
 * principal axis by power iteration, its extremes, the blue-contraction swap, a least-squares refit
 * to the chosen weights - are reused here. New for blocks larger than the weight grid they carry:
 * the weight grid is decimated (texels' ideal weights averaged onto the grid through the
 * specification's bilinear infill, then one correction pass against the infilled values), any
 * weight and endpoint range of the specification is coded (endpoints take the largest range the
 * bits left by the weights allow, as a decoder computes it), and the block modes for grids of 4x4 to
 * 8x5 are laid out from the specification's block mode table (single and dual plane).
 *
 * Input: g_bpx[ASTC_N * ASTC_N] (row-major texels, RGBA8 packed, red in the low byte). Output: the
 * 128-bit block, little-endian words.
 */

#define AN_T (ASTC_N * ASTC_N)

uint g_bpx[AN_T];
uint g_ise[48]; /* values for an_ise: the weights from 0, the endpoints from 40 */

vec4 an_px(uint i)
{
    uint p = g_bpx[i];
    return vec4(float(p & 0xffu), float((p >> 8) & 0xffu), float((p >> 16) & 0xffu), float(p >> 24));
}

/* Range q of the specification's quantization table (QUANT_2 = 0, QUANT_3, QUANT_4, QUANT_5,
 * QUANT_6, QUANT_8, ... QUANT_256 = 20): bits of a value, and 1 for a trit or 2 for a quint. */
uvec2 an_range(uint q)
{
    if (q == 0u) return uvec2(1u, 0u);
    if (q == 1u) return uvec2(0u, 1u);
    uint k = (q - 2u) / 3u, r = q - 2u - 3u * k;
    return r == 0u ? uvec2(k + 2u, 0u) : (r == 1u ? uvec2(k, 2u) : uvec2(k + 1u, 1u));
}

uint an_levels(uint q)
{
    uvec2 g = an_range(q);
    return (g.y == 0u ? 1u : (g.y == 1u ? 3u : 5u)) << g.x;
}

/* Bits n values of range q take in an integer sequence. */
uint an_bits(uint n, uint q)
{
    uvec2 g = an_range(q);
    return n * g.x + (g.y == 1u ? (8u * n + 4u) / 5u : (g.y == 2u ? (7u * n + 2u) / 3u : 0u));
}

/* The endpoint range a decoder takes for nv values in avail bits (the largest that fits), or ~0u
 * below QUANT_6. */
uint an_ep_range(int avail, uint nv)
{
    for (int q = 20; q >= 4; q--)
        if (int(an_bits(nv, uint(q))) <= avail) return uint(q);
    return ~0u;
}

/* The 8-bit value a decoder makes of endpoint v of range q (the specification's unquantization:
 * bit replication, or for trits and quints T = D * C + B, inverted by the low bit). */
uint an_unq_c(uint q, uint v)
{
    uvec2 g = an_range(q);
    uint nb = g.x;
    if (g.y == 0u) {
        uint x = 0u;
        for (int p = 8 - int(nb); p > -int(nb); p -= int(nb)) x |= p >= 0 ? v << uint(p) : v >> uint(-p);
        return x & 0xffu;
    }
    uint D = v >> nb, h = (v & ((1u << nb) - 1u)) >> 1, A = (v & 1u) != 0u ? 0x1ffu : 0u, B, C;
    if (g.y == 1u) {
        C = nb == 1u ? 204u : (nb == 2u ? 93u : (nb == 3u ? 44u : (nb == 4u ? 22u : (nb == 5u ? 11u : 5u))));
        B = nb == 2u ? (h << 8) | (h << 4) | (h << 2) | (h << 1)
          : nb == 3u ? (h << 7) | (h << 2) | h
          : nb == 4u ? (h << 6) | h
          : nb == 5u ? (h << 5) | (h >> 2)
          : nb == 6u ? (h << 4) | (h >> 4) : 0u;
    } else {
        C = nb == 1u ? 113u : (nb == 2u ? 54u : (nb == 3u ? 26u : (nb == 4u ? 13u : 6u)));
        B = nb == 2u ? (h << 8) | (h << 3) | (h << 2)
          : nb == 3u ? (h << 7) | (h << 1) | (h >> 1)
          : nb == 4u ? (h << 6) | (h >> 1)
          : nb == 5u ? (h << 5) | (h >> 3) : 0u;
    }
    uint T = (D * C + B) ^ A;
    return (A & 0x80u) | (T >> 2);
}

/* The integer-sequence value of the r-th smallest endpoint of range q. A trit or quint range's
 * lower half has the low bit clear, ordered by the bits above it, then the trit / quint; its upper
 * half mirrors it (as astc_rank). */
uint an_rank(uint q, uint r)
{
    uvec2 g = an_range(q);
    if (g.y == 0u) return r;
    uint n = an_levels(q), M = g.y == 1u ? 3u : 5u;
    bool up = r >= (n >> 1);
    uint rr = up ? n - 1u - r : r, h = rr / M;
    return ((rr - h * M) << g.x) | (h << 1) | (up ? 1u : 0u);
}

/* The nearest endpoint value of range q to v: integer-sequence value, decoded value. */
uvec2 an_ep(uint q, float v)
{
    v = clamp(v, 0.0, 255.0);
    uint n = an_levels(q) - 1u, r = uint(v * float(n) / 255.0 + 0.5);
    uvec2 best = uvec2(0u, 0u);
    float d = 1e9;
    for (uint k = r > 0u ? r - 1u : 0u; k <= min(r + 1u, n); k++) {
        uint e = an_rank(q, k), dv = an_unq_c(q, e);
        float dd = abs(float(dv) - v);
        if (dd < d) { d = dd; best = uvec2(e, dv); }
    }
    return best;
}

/* Weight ranges used: QUANT_2, 3, 4, 5, 8, 16 (no bits beside a trit or quint, so the value is the
 * rank). Value 0..64 of index k (the specification's unquantization). */
uint an_unq_w(uint q, uint k)
{
    if (q == 1u) return k * 32u;
    if (q == 3u) return k * 16u;
    uint b = an_range(q).x, w = b == 1u ? k * 63u : (b == 2u ? k * 21u : (b == 3u ? k * 9u : (k << 2) | (k >> 2)));
    return w > 32u ? w + 1u : w;
}

/* The index of range q whose weight is nearest t (0..1). */
uint an_wq(uint q, float t)
{
    uint n = an_levels(q) - 1u, k = uint(clamp(t, 0.0, 1.0) * float(n) + 0.5);
    t *= 64.0;
    float wk = float(an_unq_w(q, k));
    uint k2 = wk < t ? min(k + 1u, n) : (k > 0u ? k - 1u : 0u);
    return abs(float(an_unq_w(q, k2)) - t) < abs(wk - t) ? k2 : k;
}

/* g_ise[first, first + n) in range q at bit pos of b; returns the bit after them. */
uint an_ise(inout uvec4 b, uint pos, uint q, uint first, uint n)
{
    uvec2 g = an_range(q);
    uint nb = g.x;
    if (g.y == 0u) {
        for (uint i = 0u; i < n; i++) {
            astc_put(b, pos, nb, g_ise[first + i]);
            pos += nb;
        }
        return pos;
    }
    uint gs = g.y == 2u ? 3u : 5u;
    for (uint s = 0u; s < n; s += gs) {
        uint x[5];
        for (uint j = 0u; j < 5u; j++) x[j] = s + j < n && j < gs ? g_ise[first + s + j] >> nb : 0u;
        uint tq = g.y == 2u ? astc_quints(x[0], x[1], x[2]) : astc_trits(x[0], x[1], x[2], x[3], x[4]);
        for (uint j = 0u; j < gs && s + j < n; j++) {
            if (nb > 0u) astc_put(b, pos, nb, g_ise[first + s + j]);
            pos += nb;
            uint k = g.y == 2u ? (j == 0u ? 3u : 2u) : (j == 2u || j == 4u ? 1u : 2u);
            uint sh = g.y == 2u ? (j == 0u ? 0u : 2u * j + 1u) : 2u * j - (j >= 3u ? 1u : 0u);
            astc_put(b, pos, k, tq >> sh);
            pos += k;
        }
    }
    return pos;
}

/* Texel i's place on a gx x gy weight grid (the specification's infill): the grid index of its
 * top-left neighbour, and the four neighbours' weights in fw (top-left, right, below, below-right;
 * summing to 16). */
uint an_infill(uint i, uint gx, uint gy, out uvec4 fw)
{
    const uint Ds = (1024u + ASTC_N / 2u) / (ASTC_N - 1u);
    uint s = i % uint(ASTC_N), t = i / uint(ASTC_N);
    uint gs = (Ds * s * (gx - 1u) + 32u) >> 6, gt = (Ds * t * (gy - 1u) + 32u) >> 6;
    uint fs = gs & 15u, ft = gt & 15u, w11 = (fs * ft + 8u) >> 4;
    fw = uvec4(16u - fs - ft + w11, fs - w11, ft - w11, w11);
    return (gs >> 4) + (gt >> 4) * gx;
}

/* The 11 block mode bits for a gx x gy grid of weight range wq (2D layouts of the specification's
 * block mode table; R in bits 4 / 0-1 or 4 / 2-3, H bit 9, D bit 10). The 6..9 x 6..9 layout has
 * neither H nor D: callers use it only with wq <= QUANT_8 and one plane. */
uint an_mode(uint gx, uint gy, uint wq, bool dual)
{
    uint rq = wq < 6u ? wq + 2u : wq - 4u, H = wq < 6u ? 0u : 1u;
    uint R0 = rq & 1u, R1 = (rq >> 1) & 1u, R2 = (rq >> 2) & 1u;
    uint lo = R1 | (R2 << 1), hi = (R0 << 4) | (H << 9) | ((dual ? 1u : 0u) << 10);
    if (gx >= 4u && gx <= 7u && gy >= 2u && gy <= 5u) return lo | ((gy - 2u) << 5) | ((gx - 4u) << 7) | hi;
    if (gx >= 8u && gx <= 11u && gy >= 2u && gy <= 5u) return lo | (1u << 2) | ((gy - 2u) << 5) | ((gx - 8u) << 7) | hi;
    if (gx >= 2u && gx <= 5u && gy >= 8u && gy <= 11u) return lo | (2u << 2) | ((gx - 2u) << 5) | ((gy - 8u) << 7) | hi;
    if (gx >= 2u && gx <= 5u && gy >= 6u && gy <= 7u) return lo | (3u << 2) | ((gx - 2u) << 5) | ((gy - 6u) << 7) | hi;
    if (gx >= 2u && gx <= 3u && gy >= 2u && gy <= 5u) return lo | (3u << 2) | ((gy - 2u) << 5) | ((gx - 2u) << 7) | (1u << 8) | hi;
    return (R1 << 2) | (R2 << 3) | (R0 << 4) | ((gx - 6u) << 5) | (2u << 7) | ((gy - 6u) << 9);
}

/* The line through the block's texels (channels in m) along their principal axis, ends at the
 * extremes (astc_line over the whole block). */
void an_line(vec4 m, out vec4 lo, out vec4 hi)
{
    vec4 c = vec4(0.0), mn = vec4(255.0), mx = vec4(0.0);
    for (uint p = 0u; p < uint(AN_T); p++) {
        vec4 v = an_px(p);
        c += v;
        mn = min(mn, v);
        mx = max(mx, v);
    }
    c /= float(AN_T);
    mat4 cov = mat4(0.0);
    for (uint p = 0u; p < uint(AN_T); p++) {
        vec4 d = (an_px(p) - c) * m;
        cov += outerProduct(d, d);
    }
    vec4 ax = (mx - mn) * m;
    lo = c;
    hi = c;
    if (dot(ax, ax) < 1e-4) return;
    for (int i = 0; i < (BC_QUALITY == 0u ? 3 : 6); i++) {
        vec4 nx = cov * ax;
        float l = dot(nx, nx);
        if (l < 1e-12) break;
        ax = nx * inversesqrt(l);
    }
    ax = normalize(ax);
    float t0 = 1e9, t1 = -1e9;
    for (uint p = 0u; p < uint(AN_T); p++) {
        float t = dot((an_px(p) - c) * m, ax);
        t0 = min(t0, t);
        t1 = max(t1, t);
    }
    lo = clamp(c + ax * t0, 0.0, 255.0);
    hi = clamp(c + ax * t1, 0.0, 255.0);
}

#define AN_GMAX 40u /* grid weights at most (8x5; a dual plane grid at most 20 a plane) */

/* One candidate: a gx x gy grid of weight range wq, endpoints from l0 / l1 (dual: the colour line
 * in .rgb, alpha's extremes in .a). Returns the lower of its error and limit (the best so far),
 * and its block in blk where lower. */
float an_try(bool rgba, bool dual, uint gx, uint gy, uint wq, vec4 l0, vec4 l1, float limit, inout uvec4 blk)
{
    uint ng = gx * gy, np = dual ? 2u : 1u, nw = ng * np, nv = rgba ? 8u : 6u;
    uint wbits = an_bits(nw, wq);
    uint eq = an_ep_range(128 - 17 - (dual ? 2 : 0) - int(wbits), nv);
    if (eq == ~0u || nw > AN_GMAX) return limit;
    vec4 m = rgba && !dual ? vec4(1.0) : vec4(1.0, 1.0, 1.0, 0.0);
    vec4 rest = vec4(1.0) - m;
    if (dual) rest.a = 0.0;
    vec4 e0 = l0, e1 = l1;
    if (!rgba) e0.a = e1.a = 255.0;
    float best = limit;
    for (int pass = 0; pass < 2; pass++) {
        uint v[8];
        vec4 d0, d1;
        for (uint c = 0u; c < 4u; c++) {
            uvec2 a = an_ep(eq, e0[c]), b = an_ep(eq, e1[c]);
            if (2u * c < nv) {
                v[2u * c] = a.x;
                v[2u * c + 1u] = b.x;
            }
            d0[c] = float(a.y);
            d1[c] = float(b.y);
        }
        if (!rgba) d0.a = d1.a = 255.0;
        /* a decoder blue-contracts endpoints whose second colour sums lower: swap them */
        if (d1.r + d1.g + d1.b < d0.r + d0.g + d0.b) {
            vec4 t = d0; d0 = d1; d1 = t;
            for (uint c = 0u; 2u * c < nv; c++) {
                uint x = v[2u * c];
                v[2u * c] = v[2u * c + 1u];
                v[2u * c + 1u] = x;
            }
        }
        vec4 ax = (d1 - d0) * m;
        float l = dot(ax, ax);
        ax *= l > 0.0 ? 1.0 / l : 0.0;
        float ra = d1.a - d0.a;
        ra = ra != 0.0 ? 1.0 / ra : 0.0;
        /* the texels' ideal weights averaged onto the grid, then (not fast) corrected once by the
         * residual of the infilled grid */
        float gw[AN_GMAX], num[AN_GMAX], den[AN_GMAX];
        for (uint j = 0u; j < nw; j++) num[j] = den[j] = gw[j] = 0.0;
        for (int it = 0; it < (BC_QUALITY == 0u ? 1 : 2); it++) {
            for (uint j = 0u; j < nw; j++) num[j] = 0.0;
            for (uint i = 0u; i < uint(AN_T); i++) {
                vec4 px = an_px(i);
                uvec4 fw;
                uint b0 = an_infill(i, gx, gy, fw);
                uint idx[4] = uint[4](b0, min(b0 + 1u, ng - 1u), min(b0 + gx, ng - 1u), min(b0 + gx + 1u, ng - 1u));
                for (uint p = 0u; p < np; p++) {
                    float t = p == 0u ? clamp(dot((px - d0) * m, ax), 0.0, 1.0) : clamp((px.a - d0.a) * ra, 0.0, 1.0);
                    if (it > 0) {
                        float f = 0.0;
                        for (uint k = 0u; k < 4u; k++) f += float(fw[k]) * gw[idx[k] * np + p];
                        t -= f / 16.0;
                    }
                    for (uint k = 0u; k < 4u; k++) {
                        num[idx[k] * np + p] += float(fw[k]) * t;
                        if (it == 0) den[idx[k] * np + p] += float(fw[k]);
                    }
                }
            }
            for (uint j = 0u; j < nw; j++) gw[j] = clamp(gw[j] + (den[j] > 0.0 ? num[j] / den[j] : 0.0), 0.0, 1.0);
        }
        uint gq[AN_GMAX];
        for (uint j = 0u; j < nw; j++) {
            g_ise[j] = an_wq(wq, gw[j]);
            gq[j] = an_unq_w(wq, g_ise[j]);
        }
        /* the error as a decoder infills the quantized grid, and the sums for the refit */
        float e = 0.0, sa = 0.0, sb = 0.0, sc = 0.0, aa = 0.0, ab = 0.0, ac = 0.0, a0 = 0.0, a1 = 0.0;
        vec4 s0 = vec4(0.0), s1 = vec4(0.0);
        for (uint i = 0u; i < uint(AN_T); i++) {
            vec4 px = an_px(i);
            uvec4 fw;
            uint b0 = an_infill(i, gx, gy, fw);
            uint idx[4] = uint[4](b0, min(b0 + 1u, ng - 1u), min(b0 + gx, ng - 1u), min(b0 + gx + 1u, ng - 1u));
            uint w0 = 0u, w1 = 0u;
            for (uint k = 0u; k < 4u; k++) {
                w0 += fw[k] * gq[idx[k] * np];
                if (dual) w1 += fw[k] * gq[idx[k] * np + 1u];
            }
            float x = float((w0 + 8u) >> 4) / 64.0, y = 1.0 - x;
            vec4 d = (px - mix(d0, d1, x)) * m + (px - d0) * rest;
            e += dot(d, d);
            sa += y * y; sb += y * x; sc += x * x;
            s0 += y * px; s1 += x * px;
            if (dual) {
                float xa = float((w1 + 8u) >> 4) / 64.0, ya = 1.0 - xa, da = px.a - mix(d0.a, d1.a, xa);
                e += da * da;
                aa += ya * ya; ab += ya * xa; ac += xa * xa; a0 += ya * px.a; a1 += xa * px.a;
            }
        }
        if (e < best) {
            best = e;
            uvec4 b = uvec4(0u);
            astc_put(b, 0u, 11u, an_mode(gx, gy, wq, dual));
            astc_put(b, 13u, 4u, rgba ? 12u : 8u); /* bits 11-12 zero: one partition */
            for (uint k = 0u; k < nv; k++) g_ise[40u + k] = v[k];
            an_ise(b, 17u, eq, 40u, nv);
            if (dual) astc_put(b, 128u - wbits - 2u, 2u, 3u); /* plane 2 is alpha, just below the weights */
            uvec4 w = uvec4(0u);
            an_ise(w, 0u, wq, 0u, nw);
            /* weights run from the top of the block down */
            blk = b | uvec4(bitfieldReverse(w.w), bitfieldReverse(w.z), bitfieldReverse(w.y), bitfieldReverse(w.x));
        }
        if (pass == 0 && (e == 0.0 || BC_QUALITY == 0u)) break;
        /* pass 1: the endpoints refitted to these weights */
        float det = sa * sc - sb * sb;
        if (abs(det) >= 1e-3) {
            e0 = mix(e0, clamp((sc * s0 - sb * s1) / det, 0.0, 255.0), m);
            e1 = mix(e1, clamp((sa * s1 - sb * s0) / det, 0.0, 255.0), m);
        }
        det = aa * ac - ab * ab;
        if (dual && abs(det) >= 1e-3) {
            e0.a = clamp((ac * a0 - ab * a1) / det, 0.0, 255.0);
            e1.a = clamp((aa * a1 - ab * a0) / det, 0.0, 255.0);
        }
    }
    return best;
}

/* Candidates, tried in order until one is good enough (an error of about 1 a texel; fast: 4; high:
 * all of them). gx, gy, weight range (QUANT_3 = 1, QUANT_4 = 2, QUANT_5 = 3, QUANT_8 = 5,
 * QUANT_16 = 8), dual plane; the endpoint range follows from the bits left:
 *   8x8 colour: 6x6 trits + 8-bit endpoints, 4x4 4-bit + QUANT_192, 5x5 quints + 8-bit,
 *               8x5 trits + QUANT_192, 6x5 2-bit + 8-bit
 *   8x8 alpha:  6x6 trits + QUANT_96, 5x5 2-bit + QUANT_192, dual 4x4 trits + QUANT_128,
 *               dual 4x4 2-bit + QUANT_48
 *   6x6 colour: 6x6 trits + 8-bit, 6x5 2-bit + 8-bit, 5x5 3-bit + QUANT_64, 6x6 2-bit + QUANT_80,
 *               4x4 4-bit + QUANT_192
 *   6x6 alpha:  6x6 trits + QUANT_96, 5x5 2-bit + QUANT_192, dual 4x4 trits + QUANT_128,
 *               dual 5x4 trits + QUANT_48 */
uvec4 an_cand(bool alpha, uint i)
{
#if ASTC_N == 8
    if (!alpha) return i == 0u ? uvec4(6, 6, 1, 0) : i == 1u ? uvec4(4, 4, 8, 0) : i == 2u ? uvec4(5, 5, 3, 0) : i == 3u ? uvec4(8, 5, 1, 0) : uvec4(6, 5, 2, 0);
    return i == 0u ? uvec4(6, 6, 1, 0) : i == 1u ? uvec4(5, 5, 2, 0) : i == 2u ? uvec4(4, 4, 1, 1) : uvec4(4, 4, 2, 1);
#else
    if (!alpha) return i == 0u ? uvec4(6, 6, 1, 0) : i == 1u ? uvec4(6, 5, 2, 0) : i == 2u ? uvec4(5, 5, 5, 0) : i == 3u ? uvec4(6, 6, 2, 0) : uvec4(4, 4, 8, 0);
    return i == 0u ? uvec4(6, 6, 1, 0) : i == 1u ? uvec4(5, 5, 2, 0) : i == 2u ? uvec4(4, 4, 1, 1) : uvec4(5, 4, 1, 1);
#endif
}

uvec4 astcn_encode()
{
    bool alpha = false, same = true;
    for (uint p = 0u; p < uint(AN_T); p++) {
        alpha = alpha || (g_bpx[p] >> 24) != 255u;
        same = same && g_bpx[p] == g_bpx[0];
    }
    if (same) {
        /* one colour: a void-extent block, the colour in UNORM16 */
        uvec4 c = uvec4(an_px(0u)) * 257u;
        return uvec4(0xfffffdfcu, 0xffffffffu, c.r | (c.g << 16), c.b | (c.a << 16));
    }
    vec4 s0, s1, c0, c1;
    an_line(alpha ? vec4(1.0) : vec4(1.0, 1.0, 1.0, 0.0), s0, s1);
    if (alpha) {
        /* for the dual plane candidates: the colour's own line, alpha from its extremes */
        an_line(vec4(1.0, 1.0, 1.0, 0.0), c0, c1);
        float amin = 255.0, amax = 0.0;
        for (uint p = 0u; p < uint(AN_T); p++) {
            float a = float(g_bpx[p] >> 24);
            amin = min(amin, a);
            amax = max(amax, a);
        }
        c0.a = amin;
        c1.a = amax;
    }
    uint n = alpha ? 4u : 5u, tries = BC_QUALITY == 0u ? 2u : (BC_QUALITY == 1u ? 3u : n);
    float good = BC_QUALITY == 0u ? 4.0 * float(AN_T) : (BC_QUALITY == 1u ? float(AN_T) : -1.0);
    uvec4 blk = uvec4(0u);
    float e = 1e30;
    for (uint i = 0u; i < tries; i++) {
        uvec4 c = an_cand(alpha, i);
        bool dual = c.w != 0u;
        e = an_try(alpha, dual, c.x, c.y, c.z, dual ? c0 : s0, dual ? c1 : s1, e, blk);
        if (e <= good) break;
    }
    return blk;
}
