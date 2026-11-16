/* BC1 -> ETC2 by palette, VKBRIDGE_BC_FAST (BC_FAST variants of the BC_PALETTE ones; included by etc2_enc.glsl).
 *
 * The same search as the palette section of etc2_enc.glsl, function for function: the same candidates in the same order,
 * the same integer errors, the same ties (strict <, the first wins), the same float refits, so the same bits (tests/bcn_check.c
 * test_fast, and the startup probe on the device, compare every block with the old variant). What changes is how much state
 * a thread holds while it searches, which is what makes the old one load/store-bound on Mali (malioc: 64 work registers at
 * half occupancy and 360-430 bytes of spills; each spilled value is a stack load or store in the inner loops):
 *  - the block's four colours are four 32-bit words (8 bits a channel), not twelve ints;
 *  - how many texels use each colour is one word (a byte a colour), not four;
 *  - the loops over the four colours and the four modifiers are unrolled, so nothing is an array indexed by data (which Mali
 *    keeps on the stack), and a colour of a texel is picked by selects.
 */
uvec4 g_pcw;   /* the block's four colours: r | g << 8 | b << 16 */
uint g_pcl;    /* bit k: colour k is a transparent texel (RGB8A1 only) */
uint g_pidx;   /* the colour of texel p: bits 2p, 2p + 1 */

ivec3 pf_rgb(uint w) { return ivec3(w & 255u, (w >> 8) & 255u, w >> 16); }
ivec3 pf_col(uint k) /* k a constant where the loops are unrolled; selects otherwise */
{
    return pf_rgb(k == 0u ? g_pcw.x : (k == 1u ? g_pcw.y : (k == 2u ? g_pcw.z : g_pcw.w)));
}
uint pf_n(uint cnt, uint k) { return (cnt >> (8u * k)) & 255u; } /* texels of colour k in a count word */
uint pf_total(uint cnt) { return (cnt * 0x01010101u) >> 24; }   /* all four (at most 16: no carry) */

uint pe_k(uint p) { return (g_pidx >> (2u * p)) & 3u; }
bool pe_clear(uint p) { return ((g_pcl >> pe_k(p)) & 1u) != 0u; }
ivec3 pe_rgb(uint p) { return pf_col(pe_k(p)); }

/* texels of each colour among those of subblock sb (all 16 when sb > 1), the transparent ones left out: a byte a colour */
uint pe_count(bool flip, uint sb)
{
    uint n = 0u;
    uint last = sb > 1u ? 16u : 8u;
    for (uint i = 0u; i < last; i++) {
        uint p = sb > 1u ? i : etc_subpx(flip, sb, i);
        n += 1u << (8u * pe_k(p));
    }
    uint keep = 0u;
    [[unroll]] for (uint k = 0u; k < 4u; k++)
        if (((g_pcl >> k) & 1u) == 0u) keep |= 255u << (8u * k);
    return n & keep;
}

ivec3 pe_sum(uint cnt)
{
    ivec3 s = ivec3(0);
    [[unroll]] for (uint k = 0u; k < 4u; k++) s += int(pf_n(cnt, k)) * pf_col(k);
    return s;
}

/* the selectors of subblock sb (all 16 texels when sb > 1), from the choice bpk of each colour (2 bits) */
uint pe_sel(bool flip, uint sb, uint bpk)
{
    uint s = 0u, last = sb > 1u ? 16u : 8u;
    for (uint i = 0u; i < last; i++) {
        uint p = sb > 1u ? i : etc_subpx(flip, sb, i);
        s |= (pe_clear(p) ? 2u : (bpk >> (2u * pe_k(p))) & 3u) << (2u * p);
    }
    return s;
}

int pe1_sub(ivec3 base, uint cnt, bool clear, out uint table, out uint bpk, out vec3 target)
{
    int best = 0x7fffffff;
    bpk = 0u;
    table = 0u;
    target = vec3(base);
    uint t0 = 0u, t1 = 7u;
    if (BC_QUALITY == 0u) {
        int far = 0;
        [[unroll]] for (uint k = 0u; k < 4u; k++)
            if (pf_n(cnt, k) != 0u) {
                ivec3 d = pf_col(k) - base;
                far = max(far, abs(d.x + d.y + d.z) / 3);
            }
        uint tf = 7u;
        for (uint t = 0u; t < 7u; t++)
            if (ETC1_MOD[t].y >= far) { tf = t; break; }
        t0 = max(tf, 1u) - 1u;
        t1 = min(tf + 1u, 7u);
    }
    for (uint t = t0; t <= t1; t++) {
        /* the table's modifiers in index order (etc_mod; transparent-capable: 0, +large, none, -large) and, at the default
         * search (8 tables), the four candidate colours made once for all the block's colours (the fast search, 3 tables,
         * keeps fewer registers making them per colour) */
        ivec2 m = ETC1_MOD[t];
        ivec4 md = clear ? ivec4(0, m.y, 0, -m.y) : ivec4(m.x, m.y, -m.x, -m.y);
        ivec3 p0, p1, p2, p3;
        if (BC_QUALITY != 0u) {
            p0 = clamp(base + md.x, 0, 255);
            p1 = clamp(base + md.y, 0, 255);
            p2 = clamp(base + md.z, 0, 255);
            p3 = clamp(base + md.w, 0, 255);
        }
        int e = 0;
        uint n = 0u, pk = 0u;
        ivec3 acc = ivec3(0);
        [[unroll]] for (uint k = 0u; k < 4u; k++) {
            uint nk = pf_n(cnt, k);
            if (nk == 0u) continue;
            ivec3 c = pf_col(k);
            int be, bm = 0, ee;
            uint bk = 0u;
            if (BC_QUALITY != 0u) {
                be = etc_err(c, p0);
                bm = md.x;
                ee = etc_err(c, p1);
                if (ee < be) { be = ee; bk = 1u; bm = md.y; }
                if (!clear) {
                    ee = etc_err(c, p2);
                    if (ee < be) { be = ee; bk = 2u; bm = md.z; }
                }
                ee = etc_err(c, p3);
                if (ee < be) { be = ee; bk = 3u; bm = md.w; }
            } else {
                be = 0x7fffffff;
                [[unroll]] for (uint j = 0u; j < 4u; j++) {
                    if (clear && j == 2u) continue;
                    int mj = j == 0u ? md.x : (j == 1u ? md.y : (j == 2u ? md.z : md.w));
                    ee = etc_err(c, clamp(base + mj, 0, 255));
                    if (ee < be) { be = ee; bk = j; bm = mj; }
                }
            }
            e += int(nk) * be;
            pk |= bk << (2u * k);
            acc += int(nk) * (c - bm);
            n += nk;
        }
        if (e < best) {
            best = e;
            table = t;
            bpk = pk;
            target = n > 0u ? vec3(acc) / float(n) : vec3(base);
        }
    }
    return best;
}

int pe1_try(bool flip, bool clear, out uvec2 hl)
{
    uint cnt0 = pe_count(flip, 0u), cnt1 = pe_count(flip, 1u);
    vec3 avg[2];
    {
        ivec3 s0 = pe_sum(cnt0), s1 = pe_sum(cnt1);
        uint n0 = pf_total(cnt0), n1 = pf_total(cnt1);
        avg[0] = n0 > 0u ? vec3(s0) / float(n0) : vec3(0.0);
        avg[1] = n1 > 0u ? vec3(s1) / float(n1) : vec3(0.0);
    }
    int best = 0x7fffffff;
    hl = uvec2(0u);
    for (int pass = 0; pass < (BC_QUALITY == 0u ? 1 : 2); pass++) {
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
        uint t0, t1, bp0, bp1;
        vec3 n0, n1;
        int e = pe1_sub(b0, cnt0, clear, t0, bp0, n0) + pe1_sub(b1, cnt1, clear, t1, bp1, n1);
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
            hl = uvec2(hi, bp0 | (bp1 << 8));
        }
        avg[0] = clamp(n0, 0.0, 255.0);
        avg[1] = clamp(n1, 0.0, 255.0);
    }
    return best;
}

int pet_eval(ivec3 c1, ivec3 c2, bool clear, uint cnt, out uint dist, out uint bpk)
{
    int best = 0x7fffffff;
    dist = 0u;
    bpk = 0u;
    for (uint di = 0u; di < 8u; di++) {
        int d = ETC2_DIST[di];
        ivec3 ca = clamp(c2 + d, 0, 255), cb = clamp(c2 - d, 0, 255);
        int e = 0;
        uint pk = 0u;
        [[unroll]] for (uint k = 0u; k < 4u; k++) {
            uint nk = pf_n(cnt, k);
            if (nk == 0u) continue;
            ivec3 c = pf_col(k);
            /* paint colours c1, c2 + d, c2, c2 - d (index order; c2 not a choice in a transparent-capable block) */
            int be = etc_err(c, c1);
            uint bk = 0u;
            int ee = etc_err(c, ca);
            if (ee < be) { be = ee; bk = 1u; }
            if (!clear) {
                ee = etc_err(c, c2);
                if (ee < be) { be = ee; bk = 2u; }
            }
            ee = etc_err(c, cb);
            if (ee < be) { be = ee; bk = 3u; }
            e += int(nk) * be;
            pk |= bk << (2u * k);
        }
        if (e < best) { best = e; dist = di; bpk = pk; }
    }
    return best;
}

int pet_try(bool clear, out uvec2 hl)
{
    uint cnt = pe_count(false, 2u);
    uint n = pf_total(cnt);
    ivec3 sum = pe_sum(cnt);
    ivec3 mean = n > 0u ? sum / int(n) : ivec3(0);
    /* the texel farthest from the mean off the grey axis (the first, as etct_try) */
    ivec4 fo;
    [[unroll]] for (uint k = 0u; k < 4u; k++) {
        ivec3 dv = pf_col(k) - mean;
        int s = dv.x + dv.y + dv.z;
        fo[k] = etc_err(dv, ivec3(0)) - (s * s) / 3;
    }
    int far = -1;
    uint seedk = 4u;
    for (uint p = 0u; p < 16u; p++) {
        uint k = pe_k(p);
        int fk = k == 0u ? fo.x : (k == 1u ? fo.y : (k == 2u ? fo.z : fo.w));
        if (pf_n(cnt, k) != 0u && fk > far) { far = fk; seedk = k; }
    }
    ivec3 a = mean, b = seedk < 4u ? pf_col(seedk) : mean;
    for (int it = 0; it < 2; it++) {
        ivec3 sa = ivec3(0), sb = ivec3(0);
        int na = 0, nb = 0;
        [[unroll]] for (uint k = 0u; k < 4u; k++) {
            int nk = int(pf_n(cnt, k));
            if (nk == 0) continue;
            ivec3 c = pf_col(k);
            if (etc_err(c, a) <= etc_err(c, b)) { sa += nk * c; na += nk; }
            else { sb += nk * c; nb += nk; }
        }
        if (na > 0) a = sa / na;
        if (nb > 0) b = sb / nb;
    }
    ivec3 qa = clamp((a * 15 + 127) / 255, 0, 15), qb = clamp((b * 15 + 127) / 255, 0, 15);
    int best = 0x7fffffff;
    hl = uvec2(0u);
    for (int role = 0; role < 2; role++) {
        ivec3 q1 = role == 0 ? qa : qb, q2 = role == 0 ? qb : qa;
        uint dist, bpk;
        int e = pet_eval(etc_x4(q1), etc_x4(q2), clear, cnt, dist, bpk);
        if (e < best) {
            best = e;
            uvec3 c1 = uvec3(q1), c2 = uvec3(q2);
            uint lo2 = c1.r & 3u, hi2 = c1.r >> 2;
            uint neg = hi2 + lo2 < 4u ? 1u : 0u, top = hi2 + lo2 < 4u ? 0u : 7u;
            uint hi = (dist & 1u) | ((etc_punch ? (clear ? 0u : 1u) : 1u) << 1) | ((dist >> 1) << 2) | (c2.b << 4) | (c2.g << 8) |
                      (c2.r << 12) | (c1.b << 16) | (c1.g << 20) | (lo2 << 24) | (neg << 26) | (hi2 << 27) | (top << 29);
            hl = uvec2(hi, bpk);
        }
    }
    return best;
}

int pep_try(out uvec2 hl)
{
    vec3 m = vec3(0.0), gx = vec3(0.0), gy = vec3(0.0);
    for (uint p = 0u; p < 16u; p++) {
        vec3 c = vec3(pe_rgb(p));
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
        e += etc_err(pe_rgb(p), c);
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

/* g_pcw, g_pcl and g_pidx are set */
uvec2 etc2_encode_p(bool punch)
{
    etc_punch = punch;
    bool clear = false; /* a colour flagged transparent that no texel uses is not a transparent texel */
    for (uint p = 0u; p < 16u; p++) clear = clear || pe_clear(p);
    uvec2 best, hl;
    int e, e2, mode = 0; /* 0: ETC1, 1: T, 2: planar (best.y: the choices of each colour, or the whole selector word) */
    if (BC_QUALITY == 0u) {
        ivec3 l = ivec3(0), r = ivec3(0), t = ivec3(0), b = ivec3(0);
        for (uint p = 0u; p < 16u; p++) {
            ivec3 c = pe_rgb(p);
            if ((p & 3u) < 2u) l += c; else r += c;
            if (p < 8u) t += c; else b += c;
        }
        e = pe1_try(etc_err(t, b) > etc_err(l, r), clear, best);
    } else {
        e = pe1_try(false, clear, best);
        e2 = pe1_try(true, clear, hl);
        if (e2 < e) { e = e2; best = hl; }
    }
    if (BC_QUALITY != 0u || e > 16 * 64) {
        e2 = pet_try(clear, hl);
        if (e2 < e) { e = e2; best = hl; mode = 1; }
    }
    if (!clear) {
        e2 = pep_try(hl);
        if (e2 < e) { e = e2; best = hl; mode = 2; }
    }
    if (mode == 0) best.y = etc_sel(pe_sel((best.x & 1u) != 0u, 0u, best.y & 0xffu) | pe_sel((best.x & 1u) != 0u, 1u, best.y >> 8));
    else if (mode == 1) best.y = etc_sel(pe_sel(false, 2u, best.y));
    return uvec2(bswap32(best.x), bswap32(best.y));
}
