/* BC7 -> ASTC 4x4, VKBRIDGE_BC_FAST (BC_FAST variants of spv_bc7_astc; included by bc_decode.comp after astc_enc.glsl).
 *
 * The same encoder as astc_encode(part, true) in astc_enc.glsl, function for function: the same candidates in the same order,
 * the same float operations in the same order, the same ties (strict <, the first kept), so the same bits (tests/bcn_check.c
 * test_fast7, and the startup probe on the device, compare every block with the old variant). What changes is how much
 * state a thread holds, which is what makes the old one load/store-bound on Mali (malioc: 800 bytes of arrays on the stack and
 * 200 of spills; every texel read was a stack load):
 *  - the block's texels are 16 32-bit words (8 bits a channel; g_pw), not 16 ivec4, and every loop over them is unrolled, so
 *    they stay in registers (nothing is indexed by data);
 *  - a texel's partition picks its endpoints and sums by select, not by indexing an array with its partition bit, and the one
 *    partition candidates have no second partition's state at all (it is never used there: the partition mask is 0).
 * The shared helpers (astc_rank, astc_ep, astc_w, astc_wq, astc_put, astc_put64, astc_trits, astc_quints) are astc_enc.glsl's.
 */

uint g_pw[16]; /* the block's texels: r | g << 8 | b << 16 | a << 24 */
/* g_am, g_tam: astc_enc.glsl (ASTC_ALPHA_SPLIT; this file is built only with FMT_BC7) */

vec4 af_px(uint w) { return vec4(uvec4(w & 255u, (w >> 8) & 255u, (w >> 16) & 255u, w >> 24)); }

/* astc_line over the texels in mask pm (bit p: texel p; astc_in's test, given as a mask) */
void af_line(vec4 m, uint pm, out vec4 lo, out vec4 hi)
{
    vec4 c = vec4(0.0), mn = vec4(255.0), mx = vec4(0.0);
    float n = 0.0;
    [[unroll]] for (uint p = 0u; p < 16u; p++) {
        if (((pm >> p) & 1u) == 0u) continue;
        vec4 v = af_px(g_pw[p]);
        c += v;
        mn = min(mn, v);
        mx = max(mx, v);
        n += 1.0;
    }
    c /= max(n, 1.0);
    mat4 cov = mat4(0.0);
    [[unroll]] for (uint p = 0u; p < 16u; p++) {
        if (((pm >> p) & 1u) == 0u) continue;
        vec4 d = (af_px(g_pw[p]) - c) * m;
        cov += outerProduct(d, d);
    }
    vec4 ax = (mx - mn) * m;
    lo = c;
    hi = c;
    if (dot(ax, ax) < 1e-4) return;
    [[unroll]] for (int i = 0; i < (BC_QUALITY == 0u ? 2 : 6); i++) {
        vec4 nx = cov * ax;
        float l = dot(nx, nx);
        if (l < 1e-12) break;
        ax = nx * inversesqrt(l);
    }
    ax = normalize(ax);
    float t0 = 1e9, t1 = -1e9;
    [[unroll]] for (uint p = 0u; p < 16u; p++) {
        if (((pm >> p) & 1u) == 0u) continue;
        float t = dot((af_px(g_pw[p]) - c) * m, ax);
        t0 = min(t0, t);
        t1 = max(t1, t);
    }
    lo = clamp(c + ax * t0, 0.0, 255.0);
    hi = clamp(c + ax * t1, 0.0, 255.0);
}

/* astc_ise with every loop unrolled (n, q constants where it is called: the values stay in registers) */
uint af_ise(inout uvec4 b, uint pos, uint q, uint v[16], uint n)
{
    if (q == 256u) {
        [[unroll]] for (uint i = 0u; i < n; i++) astc_put(b, pos + 8u * i, 8u, v[i]);
        return pos + 8u * n;
    }
    uint nb = q == 40u ? 3u : (q == 48u ? 4u : 6u), gs = q == 40u ? 3u : 5u;
    [[unroll]] for (uint g = 0u; g < n; g += gs) {
        uint x[5];
        [[unroll]] for (uint j = 0u; j < 5u; j++) x[j] = g + j < n ? v[g + j] >> nb : 0u;
        uint tq = q == 40u ? astc_quints(x[0], x[1], x[2]) : astc_trits(x[0], x[1], x[2], x[3], x[4]);
        [[unroll]] for (uint j = 0u; j < gs && g + j < n; j++) {
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

/* astc_weights for one partition (the mask is 0: every texel's partition is 0; the second partition's sums stay 0 and its
 * refit is the endpoints as given, which nothing reads): r0 / r1 partition 0 only */
float af_weights1(vec4 d0, vec4 d1, vec4 m, bool dual, uint wb, out uvec2 wi, out vec4 r0, out vec4 r1)
{
    float e = 0.0;
    vec4 rest = vec4(1.0) - m;
    if (dual) rest.a = 0.0;
    vec4 ax, s0, s1;
    float sa, sb, sc;
    ax = (d1 - d0) * m;
    float l = dot(ax, ax);
    ax *= l > 0.0 ? 1.0 / l : 0.0;
    s0 = s1 = vec4(0.0);
    sa = sb = sc = 0.0;
    float ra = d1.a - d0.a;
    ra = ra != 0.0 ? 1.0 / ra : 0.0;
    vec4 al = vec4(0.0); /* dual: alpha's sums (y * y, y * x, x * x, y * a), x * a in a1 */
    float a1 = 0.0;
    wi = uvec2(0u);
    uint pos = 0u;
    g_am = 0.0;
    [[unroll]] for (uint p = 0u; p < 16u; p++) {
        vec4 v = af_px(g_pw[p]), a = d0, b = d1;
        uint k = astc_wq(wb, dot((v - a) * m, ax));
        float x = astc_w(wb, k) / 64.0, y = 1.0 - x;
        vec4 d = (v - mix(a, b, x)) * m + (v - a) * rest;
        e += dot(d, d);
        g_am = max(g_am, abs(d.a));
        sa += y * y;
        sb += y * x;
        sc += x * x;
        s0 += y * v;
        s1 += x * v;
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
    r0 = d0;
    r1 = d1;
    float det = sa * sc - sb * sb;
    if (abs(det) >= 1e-3) {
        r0 = mix(r0, clamp((sc * s0 - sb * s1) / det, 0.0, 255.0), m);
        r1 = mix(r1, clamp((sa * s1 - sb * s0) / det, 0.0, 255.0), m);
    }
    det = al.x * al.z - al.y * al.y;
    if (dual && abs(det) >= 1e-3) {
        r0.a = clamp((al.z * al.w - al.y * a1) / det, 0.0, 255.0);
        r1.a = clamp((al.x * a1 - al.y * al.w) / det, 0.0, 255.0);
    }
    return e;
}

/* astc_weights for two partitions (texel p in partition 1 where bit p of g_apart is set), never dual: each texel's endpoints
 * and sums picked by select */
float af_weights2(vec4 d00, vec4 d01, vec4 d10, vec4 d11, vec4 m, uint wb, out uvec2 wi, out vec4 r00, out vec4 r01, out vec4 r10, out vec4 r11)
{
    float e = 0.0;
    vec4 rest = vec4(1.0) - m;
    vec4 ax0 = (d10 - d00) * m, ax1 = (d11 - d01) * m;
    float l0 = dot(ax0, ax0);
    ax0 *= l0 > 0.0 ? 1.0 / l0 : 0.0;
    float l1 = dot(ax1, ax1);
    ax1 *= l1 > 0.0 ? 1.0 / l1 : 0.0;
    vec4 s00 = vec4(0.0), s01 = vec4(0.0), s10 = vec4(0.0), s11 = vec4(0.0);
    float sa0 = 0.0, sb0 = 0.0, sc0 = 0.0, sa1 = 0.0, sb1 = 0.0, sc1 = 0.0;
    wi = uvec2(0u);
    uint pos = 0u;
    [[unroll]] for (uint p = 0u; p < 16u; p++) {
        bool one = ((g_apart >> p) & 1u) != 0u;
        vec4 v = af_px(g_pw[p]), a = one ? d01 : d00, b = one ? d11 : d10;
        uint k = astc_wq(wb, dot((v - a) * m, one ? ax1 : ax0));
        float x = astc_w(wb, k) / 64.0, y = 1.0 - x;
        vec4 d = (v - mix(a, b, x)) * m + (v - a) * rest;
        e += dot(d, d);
        if (one) {
            sa1 += y * y;
            sb1 += y * x;
            sc1 += x * x;
            s01 += y * v;
            s11 += x * v;
        } else {
            sa0 += y * y;
            sb0 += y * x;
            sc0 += x * x;
            s00 += y * v;
            s10 += x * v;
        }
        astc_put64(wi, pos, wb, k);
        pos += wb;
    }
    r00 = d00;
    r10 = d10;
    float det = sa0 * sc0 - sb0 * sb0;
    if (abs(det) >= 1e-3) {
        r00 = mix(r00, clamp((sc0 * s00 - sb0 * s10) / det, 0.0, 255.0), m);
        r10 = mix(r10, clamp((sa0 * s10 - sb0 * s00) / det, 0.0, 255.0), m);
    }
    r01 = d01;
    r11 = d11;
    det = sa1 * sc1 - sb1 * sb1;
    if (abs(det) >= 1e-3) {
        r01 = mix(r01, clamp((sc1 * s01 - sb1 * s11) / det, 0.0, 255.0), m);
        r11 = mix(r11, clamp((sa1 * s11 - sb1 * s01) / det, 0.0, 255.0), m);
    }
    return e;
}

/* One partition's endpoints l0 / l1 quantized to range q: their ISE values into v from at, the decoded ones in d0 / d1,
 * swapped where a decoder would blue-contract them (astc_try's per-partition step) */
void af_quant(bool rgba, uint q, vec4 l0, vec4 l1, inout uint v[16], uint at, out vec4 d0, out vec4 d1)
{
    uint nv = rgba ? 8u : 6u;
    [[unroll]] for (uint c = 0u; c < 4u; c++) {
        uvec2 a = astc_ep(q, l0[c]), b = astc_ep(q, l1[c]);
        v[at + 2u * c] = a.x;
        if (2u * c + 1u < nv) v[at + 2u * c + 1u] = b.x;
        d0[c] = float(a.y);
        d1[c] = float(b.y);
    }
    if (!rgba) d0.a = d1.a = 255.0;
    if (d1.r + d1.g + d1.b < d0.r + d0.g + d0.b) {
        vec4 t = d0; d0 = d1; d1 = t;
        [[unroll]] for (uint c = 0u; 2u * c < nv; c++) {
            uint x = v[at + 2u * c];
            v[at + 2u * c] = v[at + 2u * c + 1u];
            v[at + 2u * c + 1u] = x;
        }
    }
}

/* the block of a candidate (astc_try's packing) */
uvec4 af_block(bool rgba, bool dual, uint wb, uint q, uint seed, uint v[16], uvec2 wi)
{
    bool two = seed != ~0u;
    uint nv = rgba ? 8u : 6u;
    uint r = wb == 2u ? 4u : (wb == 3u ? 7u : 4u), h = wb == 4u ? 1u : 0u;
    uint bm = ((r >> 1) & 3u) | ((r & 1u) << 4) | (2u << 5) | (h << 9) | ((dual ? 1u : 0u) << 10);
    uvec4 b = uvec4(0u);
    astc_put(b, 0u, 11u, bm);
    if (two) {
        astc_put(b, 11u, 2u, 1u);
        astc_put(b, 13u, 10u, seed);
        astc_put(b, 25u, 4u, rgba ? 12u : 8u);
        af_ise(b, 29u, q, v, 2u * nv);
    } else {
        astc_put(b, 13u, 4u, rgba ? 12u : 8u);
        af_ise(b, 17u, q, v, nv);
    }
    if (dual) astc_put(b, 128u - 64u - 2u, 2u, 3u);
    return b | uvec4(0u, 0u, bitfieldReverse(wi.y), bitfieldReverse(wi.x));
}

/* astc_try with one partition (seed ~0u; g_apart 0) */
float af_try1(bool rgba, bool dual, uint wb, uint q, vec4 l0, vec4 l1, float limit, inout uvec4 blk)
{
    vec4 m = rgba && !dual ? vec4(1.0) : vec4(1.0, 1.0, 1.0, 0.0);
    vec4 e0 = l0, e1 = l1;
    if (!rgba) e0.a = e1.a = 255.0;
    float best = limit;
    for (int pass = 0; pass < 2; pass++) {
        uint v[16];
        vec4 d0, d1;
        af_quant(rgba, q, e0, e1, v, 0u, d0, d1);
        uvec2 wi;
        float e = af_weights1(d0, d1, m, dual, wb, wi, e0, e1);
        if (e < best) {
            best = e;
            g_tam = g_am;
            blk = af_block(rgba, dual, wb, q, ~0u, v, wi);
        }
        if (pass == 0 && (e == 0.0 || BC_QUALITY == 0u)) break;
    }
    return best;
}

/* astc_try with two partitions (RGB, not dual; g_apart the partition-1 texels) */
float af_try2(uint wb, uint q, uint seed, vec4 l00, vec4 l01, vec4 l10, vec4 l11, float limit, inout uvec4 blk)
{
    vec4 m = vec4(1.0, 1.0, 1.0, 0.0);
    vec4 e00 = l00, e01 = l01, e10 = l10, e11 = l11;
    e00.a = e10.a = 255.0;
    e01.a = e11.a = 255.0;
    float best = limit;
    for (int pass = 0; pass < 2; pass++) {
        uint v[16];
        vec4 d00, d01, d10, d11;
        af_quant(false, q, e00, e10, v, 0u, d00, d10);
        af_quant(false, q, e01, e11, v, 6u, d01, d11);
        uvec2 wi;
        float e = af_weights2(d00, d01, d10, d11, m, wb, wi, e00, e01, e10, e11);
        if (e < best) {
            best = e;
            blk = af_block(false, false, wb, q, seed, v, wi);
        }
        if (pass == 0 && (e == 0.0 || BC_QUALITY == 0u)) break;
    }
    return best;
}

/* astc_encode(part, true) (a BC7 block): the same candidates, the same order */
uvec4 astc_encode_f(uint part)
{
    bool alpha = false, same = true;
    [[unroll]] for (uint p = 0u; p < 16u; p++) {
        alpha = alpha || (g_pw[p] >> 24) != 255u;
        same = same && g_pw[p] == g_pw[0];
    }
    if (same) {
        uvec4 c = uvec4(g_pw[0] & 255u, (g_pw[0] >> 8) & 255u, (g_pw[0] >> 16) & 255u, g_pw[0] >> 24) * 257u;
        return uvec4(0xfffffdfcu, 0xffffffffu, c.r | (c.g << 16), c.b | (c.a << 16));
    }
    g_apart = 0u;
    vec4 s0, s1;
    af_line(alpha ? vec4(1.0) : vec4(1.0, 1.0, 1.0, 0.0), 0xffffu, s0, s1);
    uvec4 blk = uvec4(0u);
    float e;
    if (alpha) {
        e = af_try1(true, false, 3u, 192u, s0, s1, 1e30, blk);
        if (e <= astc_good()) return blk;
        if (BC_QUALITY == 2u || g_tam < 48.0) { /* alpha follows the colour (astc_enc.glsl: ASTC_ALPHA_SPLIT) */
            e = af_try1(true, false, 4u, 48u, s0, s1, e, blk);
            if (BC_QUALITY < 2u) return blk;
        }
        vec4 d0, d1;
        af_line(vec4(1.0, 1.0, 1.0, 0.0), 0xffffu, d0, d1);
        float amin = 255.0, amax = 0.0;
        [[unroll]] for (uint p = 0u; p < 16u; p++) {
            amin = min(amin, float(g_pw[p] >> 24));
            amax = max(amax, float(g_pw[p] >> 24));
        }
        d0.a = amin;
        d1.a = amax;
        af_try1(true, true, 2u, 48u, d0, d1, e, blk);
        return blk;
    }
    if (part != ~0u) {
        g_apart = part & 0xffffu;
        vec4 p00, p10, p01, p11;
        af_line(vec4(1.0, 1.0, 1.0, 0.0), ~g_apart & 0xffffu, p00, p10);
        af_line(vec4(1.0, 1.0, 1.0, 0.0), g_apart, p01, p11);
        e = af_try2(2u, 40u, part >> 16, p00, p01, p10, p11, 1e30, blk);
        g_apart = 0u;
        if (BC_QUALITY == 2u) e = af_try1(false, false, 3u, 256u, s0, s1, e, blk);
    } else if (BC_QUALITY == 0u) {
        af_try1(false, false, 4u, 192u, s0, s1, 1e30, blk);
        return blk;
    } else {
        e = af_try1(false, false, 3u, 256u, s0, s1, 1e30, blk);
    }
    if (e > astc_good()) af_try1(false, false, 4u, 192u, s0, s1, e, blk);
    return blk;
}
