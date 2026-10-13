/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * bcn_check: uploads random blocks of every BC format (every BC6H and BC7 mode forced in turn),
 * blits the texture to RGBA32F and compares each texel with bcdec's CPU decode. Then the shapes
 * uploads take: mip chains with partial blocks, array layers in one region, a sub-region upload,
 * vkCmdCopyBufferToImage2, mutable images with sRGB/UNORM views, a BC -> BC image copy, and a
 * 4096x4096 BC7 array larger than the bridge's per-command-buffer decode scratch. Run it natively
 * (the GPU's own BC decoder) and through the bridge with BC emulated.
 *
 * `bcn_check transcode`: the bridge transcodes BC to ETC2 / EAC / ASTC (a GPU that samples those),
 * which is lossy: textures are then made of blocks this file encodes from synthetic images (photo-
 * like noise with hard edges, gradients, soft and cut-out alpha, normal maps) and every level is
 * compared with bcdec by PSNR. The quality table per format and image comes first. BC6H goes to
 * ASTC 4x4 HDR where the GPU samples it; the transcoder itself is also run directly on HDR images
 * (photo, gradient, sky) and its blocks decoded here (astc_decode), so a GPU without ASTC HDR (RADV)
 * still measures it. `bcn_check hdr` runs only that.
 */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define BCDEC_IMPLEMENTATION
#define BCDEC_BC4BC5_PRECISE
#include "../third_party/bcdec/bcdec.h"
#include "../server/bc_spv.h" /* the bridge's BC6H -> ASTC HDR transcoder, run directly (test_hdr_shader) */

#define TEX 64 /* 16x16 blocks */

static int tx; /* the bridge transcodes (argument "transcode"): compare by PSNR */
static int tx_hdr; /* ... BC6H too (to ASTC 4x4 HDR, where the GPU samples it) */
static double min_psnr;

static int is_bc1_unorm(VkFormat f) { return f == VK_FORMAT_BC1_RGB_UNORM_BLOCK || f == VK_FORMAT_BC1_RGBA_UNORM_BLOCK; }

/* Through the bridge BC1 UNORM decodes to R5G6B5 / A1R5G5B5: a texel matches when it is bcdec's
 * value rounded to that format's bits (exactly, give or take an 8-bit readback), or within tol of
 * bcdec as a GPU's own decoder gives it. */
static int texel_ok(VkFormat f, int ch, float g, float w, float tol)
{
    if (fabsf(g - w) <= tol) return 1;
    if (!is_bc1_unorm(f)) return 0;
    float bits = ch == 3 ? 1.0f : (ch == 1 && f == VK_FORMAT_BC1_RGB_UNORM_BLOCK) ? 63.0f : 31.0f;
    return fabsf(g - roundf(w * bits) / bits) <= 1.01f / 255.0f;
}

static PFN_vkGetInstanceProcAddr gipa;
static VkInstance inst;
static VkPhysicalDevice pd;
static VkDevice dev;
static VkQueue queue;
static VkCommandPool pool;
static VkPhysicalDeviceMemoryProperties mp;
#define F(n) static PFN_##n n
F(vkCreateInstance); F(vkEnumeratePhysicalDevices); F(vkGetPhysicalDeviceMemoryProperties); F(vkCreateDevice);
F(vkGetDeviceProcAddr); F(vkGetPhysicalDeviceFeatures); F(vkGetPhysicalDeviceFormatProperties); F(vkGetDeviceQueue);
F(vkCreateCommandPool); F(vkAllocateCommandBuffers); F(vkBeginCommandBuffer); F(vkEndCommandBuffer); F(vkQueueSubmit);
F(vkQueueWaitIdle); F(vkCreateBuffer); F(vkGetBufferMemoryRequirements); F(vkAllocateMemory); F(vkBindBufferMemory);
F(vkMapMemory); F(vkCreateImage); F(vkGetImageMemoryRequirements); F(vkBindImageMemory); F(vkCmdPipelineBarrier);
F(vkCmdCopyBufferToImage); F(vkCmdBlitImage); F(vkCmdCopyImageToBuffer); F(vkDestroyImage); F(vkDestroyBuffer);
F(vkFreeMemory); F(vkGetPhysicalDeviceProperties); F(vkCreateImageView); F(vkDestroyImageView); F(vkCmdCopyImage);
F(vkFreeCommandBuffers); F(vkCmdCopyBufferToImage2); F(vkCreateShaderModule); F(vkDestroyShaderModule);
F(vkCreateDescriptorSetLayout); F(vkCreatePipelineLayout); F(vkCreateComputePipelines); F(vkCreateDescriptorPool);
F(vkAllocateDescriptorSets); F(vkUpdateDescriptorSets); F(vkCmdBindPipeline); F(vkCmdBindDescriptorSets); F(vkCmdPushConstants);
F(vkCmdDispatch);
#undef F

static uint32_t find_type(uint32_t bits, VkMemoryPropertyFlags want)
{
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if (bits & (1u << i)) return i;
    return 0;
}

static VkDeviceMemory bind_mem(VkMemoryRequirements r, VkMemoryPropertyFlags want)
{
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, r.size, find_type(r.memoryTypeBits, want)};
    VkDeviceMemory m;
    vkAllocateMemory(dev, &ai, NULL, &m);
    return m;
}

static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return (uint32_t)(rng >> 16);
}

/* Random block; for BC6H/BC7 the mode is chosen by `mode` (cycled per block). */
static void make_block(VkFormat f, uint8_t *b, int bytes, int mode)
{
    for (int i = 0; i < bytes; i++) b[i] = (uint8_t)rnd();
    if (f == VK_FORMAT_BC7_UNORM_BLOCK || f == VK_FORMAT_BC7_SRGB_BLOCK) {
        int m = mode % 8;
        b[0] = (uint8_t)((b[0] & ~((1u << (m + 1)) - 1)) | (1u << m));
    } else if (f == VK_FORMAT_BC6H_UFLOAT_BLOCK || f == VK_FORMAT_BC6H_SFLOAT_BLOCK) {
        static const uint8_t codes[14] = {0, 1, 2, 6, 10, 14, 18, 22, 26, 30, 3, 7, 11, 15};
        int c = codes[mode % 14];
        if (c < 2) b[0] = (uint8_t)((b[0] & ~3u) | c);
        else b[0] = (uint8_t)((b[0] & ~31u) | c);
    }
}

static float h2f(uint16_t h)
{
    uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
    float v;
    if (e == 0) v = ldexpf((float)m, -24);
    else if (e == 31) v = m ? NAN : INFINITY;
    else v = ldexpf((float)(m | 1024), (int)e - 25);
    return s ? -v : v;
}

/* ------------------------------------------------------------------ ASTC 4x4 reference decoder */

/* A CPU decoder for the ASTC 4x4 blocks the BC6H transcoder makes, written from the specification
 * (Khronos Data Format Specification, ASTC chapter): void-extent blocks, 1-4 partitions with one
 * colour endpoint mode, every endpoint range, weight ranges of whole bits, colour endpoint modes
 * 7 and 11 (HDR RGB). It lets the transcoder be checked by PSNR on
 * a GPU that cannot sample ASTC HDR. Returns 0 for anything else. */

static uint32_t ab_get(const uint8_t *b, int pos, int n)
{
    uint32_t v = 0;
    for (int i = 0; i < n; i++, pos++) v |= (uint32_t)((b[pos >> 3] >> (pos & 7)) & 1) << i;
    return v;
}

/* Levels of an integer sequence encoding range: bits, trits, quints. */
static void ise_kind(int levels, int *bits, int *trit, int *quint)
{
    *trit = *quint = 0;
    *bits = 0;
    int l = levels;
    if (l % 3 == 0) *trit = 1, l /= 3;
    else if (l % 5 == 0) *quint = 1, l /= 5;
    while ((1 << *bits) < l) (*bits)++;
}

static int ise_size(int levels, int n)
{
    int bits, t, q;
    ise_kind(levels, &bits, &t, &q);
    return n * bits + (t ? (n * 8 + 4) / 5 : 0) + (q ? (n * 7 + 2) / 3 : 0);
}

/* n values of range `levels` from bit pos of b. */
static void ise_decode(const uint8_t *b, int pos, int levels, int n, int *out)
{
    int bits, t, q;
    ise_kind(levels, &bits, &t, &q);
    int gs = t ? 5 : q ? 3 : 1;
    for (int g = 0; g < n; g += gs) {
        int m[5] = {0}, x[5] = {0};
        uint32_t T = 0;
        for (int j = 0; j < gs && g + j < n; j++) { /* a last, short group has no more bits */
            m[j] = (int)ab_get(b, pos, bits);
            pos += bits;
            if (t) {
                static const int tb[5] = {2, 2, 1, 2, 1}, ts[5] = {0, 2, 4, 5, 7};
                T |= ab_get(b, pos, tb[j]) << ts[j];
                pos += tb[j];
            } else if (q) {
                static const int qb[3] = {3, 2, 2}, qs[3] = {0, 3, 5};
                T |= ab_get(b, pos, qb[j]) << qs[j];
                pos += qb[j];
            }
        }
        if (t) {
            int C, t0, t1, t2, t3, t4;
            if (((T >> 2) & 7) == 7) {
                C = (int)((((T >> 5) & 7) << 2) | (T & 3));
                t4 = t3 = 2;
            } else {
                C = (int)(T & 31);
                if (((T >> 5) & 3) == 3) t4 = 2, t3 = (int)((T >> 7) & 1);
                else t4 = (int)((T >> 7) & 1), t3 = (int)((T >> 5) & 3);
            }
            if ((C & 3) == 3) t2 = 2, t1 = (C >> 4) & 1, t0 = (((C >> 3) & 1) << 1) | ((C >> 2) & 1 & ~(C >> 3));
            else if (((C >> 2) & 3) == 3) t2 = 2, t1 = 2, t0 = C & 3;
            else t2 = (C >> 4) & 1, t1 = (C >> 2) & 3, t0 = (((C >> 1) & 1) << 1) | (C & 1 & ~(C >> 1));
            x[0] = t0, x[1] = t1, x[2] = t2, x[3] = t3, x[4] = t4;
        } else if (q) {
            int q0, q1, q2;
            if (((T >> 1) & 3) == 3 && ((T >> 5) & 3) == 0) {
                q2 = (int)(((T & 1) << 2) | ((((T >> 4) & 1) & ~T & 1) << 1) | (((T >> 3) & 1) & ~T & 1));
                q1 = q0 = 4;
            } else {
                int C;
                if (((T >> 1) & 3) == 3) q2 = 4, C = (int)((((T >> 3) & 3) << 3) | ((~(T >> 5) & 3) << 1) | (T & 1));
                else q2 = (int)((T >> 5) & 3), C = (int)(T & 31);
                if ((C & 7) == 5) q1 = 4, q0 = (C >> 3) & 3;
                else q1 = (C >> 3) & 3, q0 = C & 7;
            }
            x[0] = q0, x[1] = q1, x[2] = q2;
        }
        for (int j = 0; j < gs && g + j < n; j++) out[g + j] = (x[j] << bits) | m[j];
    }
}

/* An endpoint value of range `levels` as the 8-bit value it stands for. */
static int ep_unquant(int levels, int v)
{
    int bits, t, q;
    ise_kind(levels, &bits, &t, &q);
    if (!t && !q) {
        int r = v << (8 - bits);
        for (int s = bits; s < 8; s += bits) r |= r >> bits;
        return r & 255;
    }
    int m = v & ((1 << bits) - 1), D = v >> bits, A = (m & 1) ? 0x1ff : 0, h = m >> 1, B = 0, C;
    if (t) { /* the specification's bit patterns of B: b000b0bb0, cb000cbcb, dcb000dcb, edcb000ed, fedcb000f */
        B = bits == 2 ? (h << 8) | (h << 4) | (h << 2) | (h << 1) : bits == 3 ? (h << 7) | (h << 2) | h : bits == 4 ? (h << 6) | h
          : bits == 5 ? (h << 5) | (h >> 2) : bits == 6 ? (h << 4) | (h >> 4) : 0;
    } else { /* b0000bb00, cb0000cbc, dcb0000dc, edcb0000e */
        B = bits == 2 ? (h << 8) | (h << 3) | (h << 2) : bits == 3 ? (h << 7) | (h << 1) | (h >> 1) : bits == 4 ? (h << 6) | (h >> 1)
          : bits == 5 ? (h << 5) | (h >> 3) : 0;
    }
    static const int ct[7] = {0, 204, 93, 44, 22, 11, 5}, cq[6] = {0, 113, 54, 26, 13, 6};
    C = t ? ct[bits] : cq[bits];
    int T = (D * C + B) ^ A;
    return (A & 0x80) | (T >> 2);
}

static uint32_t ap_hash(uint32_t i)
{
    i ^= i >> 15;
    i *= 0xEEDE0891u;
    i ^= i >> 5;
    i += i << 16;
    i ^= i >> 7;
    i ^= i >> 3;
    i ^= i << 6;
    i ^= i >> 17;
    return i;
}

/* The partition of texel (x, y) of a 4x4 block (the specification's selection function). */
static int ap_select(int seed, int x, int y, int pc)
{
    x <<= 1, y <<= 1; /* fewer than 31 texels */
    seed += (pc - 1) * 1024;
    uint32_t r = ap_hash((uint32_t)seed);
    int s[12];
    for (int k = 0; k < 8; k++) s[k] = (int)((r >> (4 * k)) & 15);
    s[8] = (int)((r >> 18) & 15), s[9] = (int)((r >> 22) & 15), s[10] = (int)((r >> 26) & 15), s[11] = (int)(((r >> 30) | (r << 2)) & 15);
    for (int k = 0; k < 12; k++) s[k] *= s[k];
    int sh1, sh2;
    if (seed & 1) sh1 = (seed & 2) ? 4 : 5, sh2 = pc == 3 ? 6 : 5;
    else sh1 = pc == 3 ? 6 : 5, sh2 = (seed & 2) ? 4 : 5;
    int sh3 = (seed & 0x10) ? sh1 : sh2;
    for (int k = 0; k < 8; k++) s[k] >>= (k & 1) ? sh2 : sh1;
    for (int k = 8; k < 12; k++) s[k] >>= sh3;
    int a = (int)((s[0] * x + s[1] * y + (r >> 14)) & 0x3f), b = (int)((s[2] * x + s[3] * y + (r >> 10)) & 0x3f);
    int c = (int)((s[4] * x + s[5] * y + (r >> 6)) & 0x3f), d = (int)((s[6] * x + s[7] * y + (r >> 2)) & 0x3f);
    if (pc < 4) d = 0;
    if (pc < 3) c = 0;
    if (a >= b && a >= c && a >= d) return 0;
    if (b >= c && b >= d) return 1;
    return c >= d ? 2 : 3;
}

static int ap_clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* Colour endpoint mode 11 (HDR RGB direct): 16-bit LNS endpoints. */
static void cem11(const int *v, int e0[4], int e1[4])
{
    int majcomp = ((v[4] & 0x80) >> 7) | ((v[5] & 0x80) >> 6);
    e0[3] = e1[3] = 0x7800;
    if (majcomp == 3) {
        e0[0] = v[0] << 8, e0[1] = v[2] << 8, e0[2] = (v[4] & 0x7f) << 9;
        e1[0] = v[1] << 8, e1[1] = v[3] << 8, e1[2] = (v[5] & 0x7f) << 9;
        return;
    }
    int mode = ((v[1] & 0x80) >> 7) | ((v[2] & 0x80) >> 6) | ((v[3] & 0x80) >> 5);
    int va = v[0] | ((v[1] & 0x40) << 2), vb0 = v[2] & 0x3f, vb1 = v[3] & 0x3f, vc = v[1] & 0x3f, vd0 = v[4] & 0x7f, vd1 = v[5] & 0x7f;
    static const int dbits[8] = {7, 6, 7, 6, 5, 6, 5, 6};
    int x0 = (v[2] >> 6) & 1, x1 = (v[3] >> 6) & 1, x2 = (v[4] >> 6) & 1, x3 = (v[5] >> 6) & 1, x4 = (v[4] >> 5) & 1, x5 = (v[5] >> 5) & 1;
    int oh = 1 << mode;
    if (oh & 0xa4) va |= x0 << 9;
    if (oh & 0x8) va |= x2 << 9;
    if (oh & 0x50) va |= x4 << 9;
    if (oh & 0x50) va |= x5 << 10;
    if (oh & 0xa0) va |= x1 << 10;
    if (oh & 0xc0) va |= x2 << 11;
    if (oh & 0x4) vc |= x1 << 6;
    if (oh & 0xe8) vc |= x3 << 6;
    if (oh & 0x20) vc |= x2 << 7;
    if (oh & 0x5b) vb0 |= x0 << 6, vb1 |= x1 << 6;
    if (oh & 0x12) vb0 |= x2 << 7, vb1 |= x3 << 7;
    if (oh & 0xaf) vd0 |= x4 << 5, vd1 |= x5 << 5;
    if (oh & 0x5) vd0 |= x2 << 6, vd1 |= x3 << 6;
    int db = dbits[mode];
    vd0 &= (1 << db) - 1, vd1 &= (1 << db) - 1; /* the bits above are the variable ones */
    if (vd0 & (1 << (db - 1))) vd0 -= 1 << db;
    if (vd1 & (1 << (db - 1))) vd1 -= 1 << db;
    int sh = (mode >> 1) ^ 3;
    va <<= sh, vb0 <<= sh, vb1 <<= sh, vc <<= sh, vd0 *= 1 << sh, vd1 *= 1 << sh;
    int c1[3] = {va, va - vb0, va - vb1}, c0[3] = {va - vc, va - vb0 - vc - vd0, va - vb1 - vc - vd1};
    for (int k = 0; k < 3; k++) c0[k] = ap_clamp(c0[k], 0, 4095), c1[k] = ap_clamp(c1[k], 0, 4095);
    int sw = majcomp == 1 ? 1 : majcomp == 2 ? 2 : 0;
    if (sw) {
        int t = c0[0]; c0[0] = c0[sw]; c0[sw] = t;
        t = c1[0]; c1[0] = c1[sw]; c1[sw] = t;
    }
    for (int k = 0; k < 3; k++) e0[k] = c0[k] << 4, e1[k] = c1[k] << 4;
}

/* Colour endpoint mode 7 (HDR RGB, base and scale). */
static void cem7(const int *v, int e0[4], int e1[4])
{
    int modeval = ((v[0] & 0xc0) >> 6) | (((v[1] & 0x80) >> 7) << 2) | (((v[2] & 0x80) >> 7) << 3);
    int majcomp, mode;
    if ((modeval & 0xc) != 0xc) majcomp = modeval >> 2, mode = modeval & 3;
    else if (modeval != 0xf) majcomp = modeval & 3, mode = 4;
    else majcomp = 0, mode = 5;
    int red = v[0] & 0x3f, green = v[1] & 0x1f, blue = v[2] & 0x1f, scale = v[3] & 0x1f;
    int x0 = (v[1] >> 6) & 1, x1 = (v[1] >> 5) & 1, x2 = (v[2] >> 6) & 1, x3 = (v[2] >> 5) & 1, x4 = (v[3] >> 7) & 1, x5 = (v[3] >> 6) & 1,
        x6 = (v[3] >> 5) & 1;
    int oh = 1 << mode;
    if (oh & 0x30) green |= x0 << 6;
    if (oh & 0x3a) green |= x1 << 5;
    if (oh & 0x30) blue |= x2 << 6;
    if (oh & 0x3a) blue |= x3 << 5;
    if (oh & 0x3d) scale |= x6 << 5;
    if (oh & 0x2d) scale |= x5 << 6;
    if (oh & 0x04) scale |= x4 << 7;
    if (oh & 0x3b) red |= x4 << 6;
    if (oh & 0x04) red |= x3 << 6;
    if (oh & 0x10) red |= x5 << 7;
    if (oh & 0x0f) red |= x2 << 7;
    if (oh & 0x05) red |= x1 << 8;
    if (oh & 0x0a) red |= x0 << 8;
    if (oh & 0x05) red |= x0 << 9;
    if (oh & 0x02) red |= x6 << 9;
    if (oh & 0x01) red |= x3 << 10;
    if (oh & 0x02) red |= x5 << 10;
    static const int shamt[6] = {1, 1, 2, 3, 4, 5};
    int s = shamt[mode];
    red <<= s, green <<= s, blue <<= s, scale <<= s;
    if (mode != 5) green = red - green, blue = red - blue;
    int t;
    if (majcomp == 1) t = red, red = green, green = t;
    else if (majcomp == 2) t = red, red = blue, blue = t;
    int c1[3] = {red, green, blue}, c0[3] = {red - scale, green - scale, blue - scale};
    for (int k = 0; k < 3; k++) e0[k] = ap_clamp(c0[k], 0, 0xfff) << 4, e1[k] = ap_clamp(c1[k], 0, 0xfff) << 4;
    e0[3] = e1[3] = 0x7800;
}

static uint16_t lns_half(int c)
{
    int m = c & 0x7ff, mt = m < 512 ? 3 * m : m >= 1536 ? 5 * m - 2048 : 4 * m - 512;
    int h = ((c >> 11) << 10) + (mt >> 3);
    return (uint16_t)(h > 0x7bff ? 0x7bff : h);
}

/* Decodes a block into RGBA floats; 0 if the block is outside what this decoder handles. */
static int astc_decode(const uint8_t *b, float out[16][4])
{
    uint32_t mode = ab_get(b, 0, 11);
    if ((mode & 0x1ff) == 0x1fc) {
        int hdr = (mode >> 9) & 1;
        for (int t = 0; t < 16; t++)
            for (int c = 0; c < 4; c++) {
                uint32_t v = ab_get(b, 64 + 16 * c, 16);
                out[t][c] = hdr ? h2f((uint16_t)v) : v / 65535.0f;
            }
        return 1;
    }
    /* 4x4 weights, layout bits[3:2] = 00 with A = 2, B = 0, or bits[1:0] = 00 forms are not made */
    if (!(mode & 3) || ((mode >> 2) & 3) != 0 || ((mode >> 5) & 3) != 2 || ((mode >> 7) & 3) != 0) return 0;
    int R = (int)(((mode & 3) << 1) | ((mode >> 4) & 1)), H = (int)((mode >> 9) & 1), dual = (int)((mode >> 10) & 1);
    static const int wl[2][8] = {{0, 0, 2, 3, 4, 5, 6, 8}, {0, 0, 10, 12, 16, 20, 24, 32}};
    int wlev = wl[H][R], wbits, wt, wq;
    ise_kind(wlev, &wbits, &wt, &wq);
    if (wt || wq) return 0; /* weight ranges of whole bits only */
    int nw = 16 * (dual ? 2 : 1), wsize = ise_size(wlev, nw);
    int pc = (int)ab_get(b, 11, 2) + 1, seed = 0, cem, pos;
    if (pc == 1) {
        cem = (int)ab_get(b, 13, 4);
        pos = 17;
    } else {
        seed = (int)ab_get(b, 13, 10);
        uint32_t cf = ab_get(b, 23, 6);
        if (cf & 3) return 0; /* one mode for every partition only */
        cem = (int)(cf >> 2);
        pos = 29;
    }
    if (cem != 7 && cem != 11) return 0;
    int nv = pc * ((cem >> 2) + 1) * 2;
    if (nv > 18) return 0;
    int avail = 128 - wsize - pos - (dual ? 2 : 0);
    static const int eplev[] = {256, 192, 160, 128, 96, 80, 64, 48, 40, 32, 24, 20, 16, 12, 10, 8, 6};
    int elev = 0;
    for (size_t i = 0; i < sizeof(eplev) / sizeof(eplev[0]) && !elev; i++)
        if (ise_size(eplev[i], nv) <= avail) elev = eplev[i];
    if (!elev) return 0;
    int ev[32], v[32];
    ise_decode(b, pos, elev, nv, ev);
    for (int i = 0; i < nv; i++) v[i] = ep_unquant(elev, ev[i]);
    /* weights: read from the top of the block down */
    uint8_t rb[16];
    for (int i = 0; i < 16; i++) {
        uint8_t x = b[15 - i], y = 0;
        for (int k = 0; k < 8; k++) y |= (uint8_t)(((x >> k) & 1) << (7 - k));
        rb[i] = y;
    }
    int w[32];
    ise_decode(rb, 0, wlev, nw, w);
    for (int i = 0; i < nw; i++) {
        int r = w[i] << (6 - wbits);
        for (int s = wbits; s < 6; s += wbits) r |= r >> wbits;
        r &= 63;
        w[i] = r > 32 ? r + 1 : r;
    }
    int plane2 = dual ? (int)ab_get(b, 128 - wsize - 2, 2) : -1;
    for (int t = 0; t < 16; t++) {
        int p = pc > 1 ? ap_select(seed, t & 3, t >> 2, pc) : 0;
        const int *pv = v + p * ((cem >> 2) + 1) * 2;
        int e0[4], e1[4];
        if (cem == 11) cem11(pv, e0, e1);
        else cem7(pv, e0, e1);
        for (int c = 0; c < 4; c++) {
            int wt = dual && c == plane2 ? w[2 * t + 1] : w[dual ? 2 * t : t];
            out[t][c] = h2f(lns_half((e0[c] * (64 - wt) + e1[c] * wt + 32) >> 6));
        }
    }
    return 1;
}

/* Reference texels (RGBA float, sRGB formats still encoded) of one block. */
static void decode_block(VkFormat f, const uint8_t *b, float out[16][4])
{
    uint8_t px8[16 * 4];
    int8_t spx[16 * 2];
    uint16_t px16[16 * 3];
    int rgba8 = 0;
    for (int t = 0; t < 16; t++) {
        out[t][0] = out[t][1] = out[t][2] = 0;
        out[t][3] = 1;
    }
    switch (f) {
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: case VK_FORMAT_BC1_RGBA_SRGB_BLOCK: bcdec_bc1(b, px8, 16); rgba8 = 1; break;
    case VK_FORMAT_BC1_RGB_UNORM_BLOCK: case VK_FORMAT_BC1_RGB_SRGB_BLOCK: /* bcdec's colours (black where it has
                                                                           * transparent), alpha 1 */
        bcdec_bc1(b, px8, 16);
        for (int t = 0; t < 16; t++) px8[t * 4 + 3] = 255;
        rgba8 = 1;
        break;
    case VK_FORMAT_BC2_UNORM_BLOCK: case VK_FORMAT_BC2_SRGB_BLOCK: bcdec_bc2(b, px8, 16); rgba8 = 1; break;
    case VK_FORMAT_BC3_UNORM_BLOCK: case VK_FORMAT_BC3_SRGB_BLOCK: bcdec_bc3(b, px8, 16); rgba8 = 1; break;
    case VK_FORMAT_BC7_UNORM_BLOCK: case VK_FORMAT_BC7_SRGB_BLOCK: bcdec_bc7(b, px8, 16); rgba8 = 1; break;
    case VK_FORMAT_BC4_UNORM_BLOCK:
        bcdec_bc4(b, px8, 4, 0);
        for (int t = 0; t < 16; t++) out[t][0] = px8[t] / 255.0f;
        break;
    case VK_FORMAT_BC4_SNORM_BLOCK:
        bcdec_bc4(b, spx, 4, 1);
        for (int t = 0; t < 16; t++) out[t][0] = fmaxf(spx[t] / 127.0f, -1.0f);
        break;
    case VK_FORMAT_BC5_UNORM_BLOCK:
        bcdec_bc5(b, px8, 8, 0);
        for (int t = 0; t < 16; t++) {
            out[t][0] = px8[t * 2] / 255.0f;
            out[t][1] = px8[t * 2 + 1] / 255.0f;
        }
        break;
    case VK_FORMAT_BC5_SNORM_BLOCK:
        bcdec_bc5(b, spx, 8, 1);
        for (int t = 0; t < 16; t++) {
            out[t][0] = fmaxf(spx[t * 2] / 127.0f, -1.0f);
            out[t][1] = fmaxf(spx[t * 2 + 1] / 127.0f, -1.0f);
        }
        break;
    case VK_FORMAT_BC6H_UFLOAT_BLOCK:
    case VK_FORMAT_BC6H_SFLOAT_BLOCK:
        bcdec_bc6h_half(b, px16, 12, f == VK_FORMAT_BC6H_SFLOAT_BLOCK);
        for (int t = 0; t < 16; t++)
            for (int c = 0; c < 3; c++) out[t][c] = h2f(px16[t * 3 + c]);
        break;
    default: break;
    }
    if (rgba8)
        for (int t = 0; t < 16; t++)
            for (int c = 0; c < 4; c++) out[t][c] = px8[t * 4 + c] / 255.0f;
}

/* Reference texel (RGBA float) at (x, y) of the texture made from `blocks`. */
static void reference(VkFormat f, const uint8_t *blocks, int bytes, int x, int y, float out[4])
{
    float t[16][4];
    decode_block(f, blocks + ((y / 4) * (TEX / 4) + (x / 4)) * bytes, t);
    memcpy(out, t[(y & 3) * 4 + (x & 3)], sizeof(t[0]));
}

static VkCommandBuffer begin(void)
{
    VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VkCommandBuffer cb;
    vkAllocateCommandBuffers(dev, &ai, &cb);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
    vkBeginCommandBuffer(cb, &bi);
    return cb;
}

static void barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to)
{
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, VK_ACCESS_MEMORY_WRITE_BIT,
                              VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, from, to, VK_QUEUE_FAMILY_IGNORED,
                              VK_QUEUE_FAMILY_IGNORED, img, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}

static int failures;

static int block_bytes(VkFormat f)
{
    return (f == VK_FORMAT_BC1_RGB_UNORM_BLOCK || f == VK_FORMAT_BC1_RGB_SRGB_BLOCK || f == VK_FORMAT_BC1_RGBA_UNORM_BLOCK ||
            f == VK_FORMAT_BC1_RGBA_SRGB_BLOCK || f == VK_FORMAT_BC4_UNORM_BLOCK || f == VK_FORMAT_BC4_SNORM_BLOCK) ? 8 : 16;
}

static void test_format(VkFormat f, const char *name, float tol)
{
    int bytes = block_bytes(f);
    int nblocks = (TEX / 4) * (TEX / 4);
    uint8_t *blocks = malloc((size_t)nblocks * bytes);
    for (int i = 0; i < nblocks; i++) make_block(f, blocks + i * bytes, bytes, i);
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(pd, f, &fp);
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT)) {
        printf("SKIP %s: no blit from this format\n", name);
        free(blocks);
        return;
    }
    /* staging */
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, (VkDeviceSize)TEX * TEX * 16,
                              VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
    VkBuffer up, down;
    vkCreateBuffer(dev, &bci, NULL, &up);
    vkCreateBuffer(dev, &bci, NULL, &down);
    VkMemoryRequirements r;
    vkGetBufferMemoryRequirements(dev, up, &r);
    VkDeviceMemory upm = bind_mem(r, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vkBindBufferMemory(dev, up, upm, 0);
    vkGetBufferMemoryRequirements(dev, down, &r);
    VkDeviceMemory downm = bind_mem(r, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vkBindBufferMemory(dev, down, downm, 0);
    void *upp, *downp;
    vkMapMemory(dev, upm, 0, VK_WHOLE_SIZE, 0, &upp);
    vkMapMemory(dev, downm, 0, VK_WHOLE_SIZE, 0, &downp);
    /* upload at an odd 16-byte offset with a padded row, to exercise the decoder's addressing */
    const VkDeviceSize off = 48;
    const uint32_t row_tex = TEX + 8;
    memset(upp, 0xcd, (size_t)TEX * TEX * 16);
    for (int by = 0; by < TEX / 4; by++)
        memcpy((uint8_t *)upp + off + (size_t)by * (row_tex / 4) * bytes, blocks + (size_t)by * (TEX / 4) * bytes, (size_t)(TEX / 4) * bytes);
    /* images */
    VkImageCreateInfo ici = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, f, {TEX, TEX, 1}, 1, 1, VK_SAMPLE_COUNT_1_BIT,
                             VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                             VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
    VkImage bc, rgba;
    if (vkCreateImage(dev, &ici, NULL, &bc) != VK_SUCCESS) {
        printf("FAIL %s: cannot create the image\n", name);
        failures++;
        return;
    }
    VkMemoryRequirements ir;
    vkGetImageMemoryRequirements(dev, bc, &ir);
    VkDeviceMemory bcm = bind_mem(ir, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkBindImageMemory(dev, bc, bcm, 0);
    ici.format = VK_FORMAT_R32G32B32A32_SFLOAT;
    vkCreateImage(dev, &ici, NULL, &rgba);
    vkGetImageMemoryRequirements(dev, rgba, &ir);
    VkDeviceMemory rgbam = bind_mem(ir, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkBindImageMemory(dev, rgba, rgbam, 0);
    VkCommandBuffer cb = begin();
    barrier(cb, bc, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy c = {off, row_tex, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {TEX, TEX, 1}};
    vkCmdCopyBufferToImage(cb, up, bc, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    barrier(cb, bc, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    barrier(cb, rgba, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageBlit bl = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{0, 0, 0}, {TEX, TEX, 1}}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{0, 0, 0}, {TEX, TEX, 1}}};
    vkCmdBlitImage(cb, bc, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rgba, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_NEAREST);
    barrier(cb, rgba, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy rb = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {TEX, TEX, 1}};
    vkCmdCopyImageToBuffer(cb, rgba, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, down, 1, &rb);
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    vkEndCommandBuffer(cb);
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cb, 0, NULL};
    vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);
    const float *got = downp;
    int bad = 0, nan_skip = 0;
    float worst = 0;
    int wx = 0, wy = 0, wc = 0;
    for (int y = 0; y < TEX; y++)
        for (int x = 0; x < TEX; x++) {
            float want[4];
            reference(f, blocks, bytes, x, y, want);
            for (int ch = 0; ch < 4; ch++) {
                float g = got[(y * TEX + x) * 4 + ch], w = want[ch];
                if (isnan(w) || isinf(w)) {
                    nan_skip++;
                    continue;
                }
                float d = fabsf(g - w);
                float lim = (f == VK_FORMAT_BC6H_UFLOAT_BLOCK || f == VK_FORMAT_BC6H_SFLOAT_BLOCK) ? tol * fmaxf(1.0f, fabsf(w)) : tol;
                if (!(d <= lim) && !texel_ok(f, ch, g, w, lim)) {
                    bad++;
                    if (!(d <= worst)) {
                        worst = d;
                        wx = x;
                        wy = y;
                        wc = ch;
                    }
                }
            }
        }
    if (bad) {
        float want[4];
        reference(f, blocks, bytes, wx, wy, want);
        printf("FAIL %s: %d of %d values differ from bcdec; worst %.4f at (%d,%d) ch %d: got %g want %g (block mode byte 0x%02x)\n", name, bad,
               TEX * TEX * 4, worst, wx, wy, wc, got[(wy * TEX + wx) * 4 + wc], want[wc],
               blocks[((wy / 4) * (TEX / 4) + (wx / 4)) * bytes]);
        failures++;
    } else {
        printf("PASS %s: %d texels match bcdec%s\n", name, TEX * TEX, nan_skip ? " (inf/nan references skipped)" : "");
    }
    vkDestroyImage(dev, bc, NULL);
    vkDestroyImage(dev, rgba, NULL);
    vkDestroyBuffer(dev, up, NULL);
    vkDestroyBuffer(dev, down, NULL);
    vkFreeMemory(dev, upm, NULL);
    vkFreeMemory(dev, downm, NULL);
    vkFreeMemory(dev, bcm, NULL);
    vkFreeMemory(dev, rgbam, NULL);
    free(blocks);
}

/* ------------------------------------------------------------------ test images and a small BC encoder */

/* Synthetic images, 8-bit RGBA: value noise in octaves with hard-edged patches (photo-like), the
 * same at texel scale with colours of their own (rough: gravel, foliage), smooth gradients, soft or
 * cut-out alpha over the photo, and a normal map of a noise height field. */
enum { IMG_PHOTO, IMG_ROUGH, IMG_GRADIENT, IMG_ALPHA_SOFT, IMG_ALPHA_CUT, IMG_NORMAL, IMG_HDR_PHOTO, IMG_HDR_GRADIENT, IMG_HDR_SKY, IMG_COUNT };
static const char *const img_name[IMG_COUNT] = {"photo", "rough", "gradient", "soft alpha", "cut-out alpha", "normal map",
                                                "HDR photo", "HDR gradient", "HDR sky"};

static float lattice(int x, int y, uint32_t seed)
{
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return ((h ^ (h >> 16)) & 0xffff) / 65535.0f;
}

static float vnoise(float x, float y, uint32_t seed)
{
    int xi = (int)floorf(x), yi = (int)floorf(y);
    float fx = x - xi, fy = y - yi;
    fx = fx * fx * (3 - 2 * fx);
    fy = fy * fy * (3 - 2 * fy);
    float a = lattice(xi, yi, seed), b = lattice(xi + 1, yi, seed), c = lattice(xi, yi + 1, seed), d = lattice(xi + 1, yi + 1, seed);
    return (a + (b - a) * fx) + ((c + (d - c) * fx) - (a + (b - a) * fx)) * fy;
}

static float fbm(float x, float y, uint32_t seed, int oct)
{
    float v = 0, a = 0.5f, f = 1, n = 0;
    for (int o = 0; o < oct; o++, a *= 0.5f, f *= 2) {
        v += a * vnoise(x * f, y * f, seed + (uint32_t)o * 101u);
        n += a;
    }
    return v / n;
}

static uint8_t u8(float v) { return (uint8_t)lroundf(fminf(fmaxf(v, 0), 1) * 255); }

/* The normal at (x, y) of the height field, in [-1, 1]. */
static void normal_at(int x, int y, float n[3])
{
    float dx = fbm((x + 1) / 24.0f, y / 24.0f, 77, 4) - fbm((x - 1) / 24.0f, y / 24.0f, 77, 4);
    float dy = fbm(x / 24.0f, (y + 1) / 24.0f, 77, 4) - fbm(x / 24.0f, (y - 1) / 24.0f, 77, 4);
    float k = 6, l = sqrtf(dx * dx * k * k + dy * dy * k * k + 1);
    n[0] = -dx * k / l, n[1] = -dy * k / l, n[2] = 1 / l;
}

static void image_px(int kind, int x, int y, uint8_t out[4])
{
    if (kind == IMG_GRADIENT) {
        float u = x / 255.0f, v = y / 255.0f;
        out[0] = u8(0.1f + 0.8f * u), out[1] = u8(0.2f + 0.6f * v), out[2] = u8(0.5f + 0.4f * sinf(3.0f * (u + v))), out[3] = 255;
        return;
    }
    if (kind == IMG_NORMAL) {
        float n[3];
        normal_at(x, y, n);
        out[0] = u8(n[0] * 0.5f + 0.5f), out[1] = u8(n[1] * 0.5f + 0.5f), out[2] = u8(n[2] * 0.5f + 0.5f), out[3] = 255;
        return;
    }
    if (kind == IMG_ROUGH) {
        float l = fbm(x / 3.0f, y / 3.0f, 71, 3), e = fbm(x / 16.0f, y / 16.0f, 73, 2) > 0.5f ? 0.25f : 0;
        out[0] = u8(0.2f + 0.7f * l + e - 0.3f * (fbm(x / 2.0f, y / 2.0f, 79, 2) - 0.5f));
        out[1] = u8(0.1f + 0.8f * l - 0.4f * (fbm(x / 2.5f, y / 2.5f, 83, 2) - 0.5f));
        out[2] = u8(0.6f * l + e + 0.4f * (fbm(x / 1.5f, y / 1.5f, 89, 2) - 0.5f));
        out[3] = 255;
        return;
    }
    float l = fbm(x / 32.0f, y / 32.0f, 11, 5), c1 = fbm(x / 48.0f, y / 48.0f, 23, 3), c2 = fbm(x / 40.0f, y / 40.0f, 37, 3);
    float detail = fbm(x / 4.0f, y / 4.0f, 41, 2) - 0.5f;
    int patch = fbm(x / 64.0f, y / 64.0f, 53, 2) > 0.56f; /* hard-edged regions of another colour */
    float r = 0.15f + 0.9f * l + 0.5f * (c1 - 0.5f) + 0.15f * detail, g = 0.1f + 0.85f * l - 0.3f * (c2 - 0.5f) + 0.15f * detail;
    float b = 0.05f + 0.7f * l + 0.6f * (c2 - 0.5f) + 0.15f * detail;
    if (patch) r = 0.9f - 0.5f * r, g = 0.3f + 0.5f * g, b = 0.8f * b;
    out[0] = u8(r), out[1] = u8(g), out[2] = u8(b), out[3] = 255;
    if (kind == IMG_ALPHA_SOFT) out[3] = u8((fbm(x / 40.0f, y / 40.0f, 61, 3) - 0.35f) * 3.0f);
    if (kind == IMG_ALPHA_CUT) out[3] = fbm(x / 20.0f, y / 20.0f, 67, 3) > 0.5f ? 255 : 0;
}

static void bits_put(uint8_t *b, int *pos, int n, uint32_t v)
{
    for (int i = 0; i < n; i++, (*pos)++)
        if ((v >> i) & 1) b[*pos >> 3] |= (uint8_t)(1u << (*pos & 7));
}

/* Ends of the principal axis through the texels (channels 0..nc-1, those with mask set). */
static void axis_ends(const float px[16][4], const int *mask, int nc, float lo[4], float hi[4])
{
    float m[4] = {0}, cov[4][4] = {{0}}, ax[4] = {1, 1, 1, 1};
    int n = 0;
    for (int t = 0; t < 16; t++) {
        if (mask && !mask[t]) continue;
        for (int c = 0; c < nc; c++) m[c] += px[t][c];
        n++;
    }
    for (int c = 0; c < 4; c++) lo[c] = hi[c] = n ? m[c] / n : 0;
    if (!n) return;
    for (int c = 0; c < nc; c++) m[c] /= n;
    for (int t = 0; t < 16; t++) {
        if (mask && !mask[t]) continue;
        for (int i = 0; i < nc; i++)
            for (int j = 0; j < nc; j++) cov[i][j] += (px[t][i] - m[i]) * (px[t][j] - m[j]);
    }
    for (int it = 0; it < 8; it++) {
        float nx[4] = {0}, l = 0;
        for (int i = 0; i < nc; i++) {
            for (int j = 0; j < nc; j++) nx[i] += cov[i][j] * ax[j];
            l += nx[i] * nx[i];
        }
        if (l < 1e-9f) break;
        for (int i = 0; i < nc; i++) ax[i] = nx[i] / sqrtf(l);
    }
    float t0 = 1e9f, t1 = -1e9f;
    for (int t = 0; t < 16; t++) {
        if (mask && !mask[t]) continue;
        float d = 0;
        for (int c = 0; c < nc; c++) d += (px[t][c] - m[c]) * ax[c];
        t0 = fminf(t0, d), t1 = fmaxf(t1, d);
    }
    for (int c = 0; c < nc; c++) lo[c] = fminf(fmaxf(m[c] + ax[c] * t0, 0), 255), hi[c] = fminf(fmaxf(m[c] + ax[c] * t1, 0), 255);
}

static uint16_t to565(const float c[3])
{
    int r = (int)lroundf(c[0] * 31 / 255), g = (int)lroundf(c[1] * 63 / 255), b = (int)lroundf(c[2] * 31 / 255);
    return (uint16_t)((r << 11) | (g << 5) | b);
}

static void from565(uint16_t v, float c[3])
{
    int r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
    c[0] = (float)((r << 3) | (r >> 2)), c[1] = (float)((g << 2) | (g >> 4)), c[2] = (float)((b << 3) | (b >> 2));
}

/* BC1 colour block (8 bytes); punch: texels with alpha < 128 become BC1's transparent black. */
static void enc_bc1(const float px[16][4], uint8_t *out, int punch)
{
    int mask[16], clear = 0;
    for (int t = 0; t < 16; t++) mask[t] = !punch || px[t][3] >= 128, clear |= !mask[t];
    float lo[4], hi[4], pal[4][3];
    axis_ends(px, mask, 3, lo, hi);
    uint16_t c0 = to565(hi), c1 = to565(lo);
    if (clear ? c0 > c1 : c0 < c1) {
        uint16_t t = c0;
        c0 = c1, c1 = t;
    }
    from565(c0, pal[0]);
    from565(c1, pal[1]);
    for (int c = 0; c < 3; c++) {
        if (c0 > c1) pal[2][c] = (2 * pal[0][c] + pal[1][c]) / 3, pal[3][c] = (pal[0][c] + 2 * pal[1][c]) / 3;
        else pal[2][c] = (pal[0][c] + pal[1][c]) / 2, pal[3][c] = 0;
    }
    uint32_t idx = 0;
    for (int t = 0; t < 16; t++) {
        int best = 0;
        float be = 1e30f;
        for (int k = 0; k < (c0 > c1 ? 4 : 3) && mask[t]; k++) {
            float e = 0;
            for (int c = 0; c < 3; c++) e += (px[t][c] - pal[k][c]) * (px[t][c] - pal[k][c]);
            if (e < be) be = e, best = k;
        }
        idx |= (uint32_t)(mask[t] ? best : 3) << (2 * t);
    }
    out[0] = (uint8_t)c0, out[1] = (uint8_t)(c0 >> 8), out[2] = (uint8_t)c1, out[3] = (uint8_t)(c1 >> 8);
    memcpy(out + 4, &idx, 4);
}

/* BC4 channel block (8 bytes), values 0..255 or -127..127. */
static void enc_bc4(const float v[16], uint8_t *out, int sgn)
{
    float mn = v[0], mx = v[0];
    for (int t = 1; t < 16; t++) mn = fminf(mn, v[t]), mx = fmaxf(mx, v[t]);
    int e0 = (int)lroundf(mx), e1 = (int)lroundf(mn);
    float pal[8] = {(float)e0, (float)e1};
    for (int i = 1; i < 7; i++) pal[1 + i] = ((7 - i) * e0 + i * e1) / 7.0f;
    memset(out, 0, 8);
    out[0] = (uint8_t)e0, out[1] = (uint8_t)e1;
    int pos = 16;
    for (int t = 0; t < 16; t++) {
        int best = 0;
        for (int k = 1; k < 8; k++)
            if (fabsf(v[t] - pal[k]) < fabsf(v[t] - pal[best])) best = k;
        bits_put(out, &pos, 3, (uint32_t)best);
    }
    (void)sgn;
}

/* BC7 mode 6 (one subset, 7-bit RGBA endpoints with a p-bit each, 4-bit indices); the error. */
static float enc_bc7_mode6(const float px[16][4], uint8_t *out)
{
    static const int w4[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};
    float e[2][4];
    axis_ends(px, NULL, 4, e[1], e[0]);
    int q[2][4], p[2], ep[2][4];
    for (int k = 0; k < 2; k++) {
        float best = 1e30f;
        for (int pb = 0; pb < 2; pb++) {
            float err = 0;
            int qq[4];
            for (int c = 0; c < 4; c++) {
                qq[c] = (int)lroundf((e[k][c] - pb) / 2);
                qq[c] = qq[c] < 0 ? 0 : qq[c] > 127 ? 127 : qq[c];
                float d = e[k][c] - ((qq[c] << 1) | pb);
                err += d * d;
            }
            if (err < best) {
                best = err, p[k] = pb;
                memcpy(q[k], qq, sizeof(qq));
            }
        }
        for (int c = 0; c < 4; c++) ep[k][c] = (q[k][c] << 1) | p[k];
    }
    int idx[16];
    float total = 0;
    for (int t = 0; t < 16; t++) {
        float be = 1e30f;
        for (int i = 0; i < 16; i++) {
            float err = 0;
            for (int c = 0; c < 4; c++) {
                float v = (float)((ep[0][c] * (64 - w4[i]) + ep[1][c] * w4[i] + 32) >> 6) - px[t][c];
                err += v * v;
            }
            if (err < be) be = err, idx[t] = i;
        }
        total += be;
    }
    if (idx[0] & 8) { /* the anchor index has no top bit: swap the ends */
        for (int c = 0; c < 4; c++) {
            int x = q[0][c];
            q[0][c] = q[1][c], q[1][c] = x;
        }
        int x = p[0];
        p[0] = p[1], p[1] = x;
        for (int t = 0; t < 16; t++) idx[t] = 15 - idx[t];
    }
    memset(out, 0, 16);
    int pos = 0;
    bits_put(out, &pos, 7, 0x40);
    for (int c = 0; c < 4; c++) {
        bits_put(out, &pos, 7, (uint32_t)q[0][c]);
        bits_put(out, &pos, 7, (uint32_t)q[1][c]);
    }
    bits_put(out, &pos, 1, (uint32_t)p[0]);
    bits_put(out, &pos, 1, (uint32_t)p[1]);
    for (int t = 0; t < 16; t++) bits_put(out, &pos, t ? 4 : 3, (uint32_t)idx[t]);
    return total;
}

/* BC7's two-subset partitions (from bcdec: a mode 1 block with subset 1 white) and their anchors. */
static uint8_t bc7_part2[64][16];
static const uint8_t bc7_anchor2[64] = {15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 2, 8, 2, 2, 8, 8, 15, 2, 8, 2, 2, 8, 8, 2, 2,
                                        15, 15, 6, 8, 2, 8, 15, 15, 2, 8, 2, 2, 2, 15, 15, 6, 6, 2, 6, 8, 15, 15, 2, 2, 15, 15, 15, 15, 15, 2, 2, 15};

static void bc7_partitions(void)
{
    for (int p = 0; p < 64; p++) {
        uint8_t b[16] = {0}, px[64];
        int pos = 0;
        bits_put(b, &pos, 2, 2);
        bits_put(b, &pos, 6, (uint32_t)p);
        for (int c = 0; c < 3; c++)
            for (int e = 0; e < 4; e++) bits_put(b, &pos, 6, e >= 2 ? 63 : 0);
        bits_put(b, &pos, 1, 0);
        bits_put(b, &pos, 1, 1);
        bcdec_bc7(b, px, 16);
        for (int t = 0; t < 16; t++) bc7_part2[p][t] = px[t * 4] > 128;
    }
}

/* BC7 mode 1 (two subsets, 6-bit RGB endpoints with a p-bit per subset, 3-bit indices), the best
 * partition; the error. Real BC7 textures are mostly multi-subset blocks like these. */
static float enc_bc7_mode1(const float px[16][4], uint8_t *out)
{
    static const int w3[8] = {0, 9, 18, 27, 37, 46, 55, 64};
    float best = 1e30f;
    for (int p = 0; p < 64; p++) {
        int q[4][3], pb[2], ex[4][3], idx[16];
        float total = 0;
        for (int sub = 0; sub < 2; sub++) {
            int mask[16];
            for (int t = 0; t < 16; t++) mask[t] = bc7_part2[p][t] == sub;
            float e[2][4];
            axis_ends(px, mask, 3, e[0], e[1]);
            float be = 1e30f;
            for (int pp = 0; pp < 2; pp++) {
                float err = 0;
                int qq[2][3], xx[2][3];
                for (int k = 0; k < 2; k++)
                    for (int c = 0; c < 3; c++) {
                        int v = (int)lroundf((e[k][c] * 127 / 255 - pp) / 2);
                        v = v < 0 ? 0 : v > 63 ? 63 : v;
                        int v7 = (v << 1) | pp;
                        qq[k][c] = v, xx[k][c] = (v7 << 1) | (v7 >> 6);
                        err += (e[k][c] - xx[k][c]) * (e[k][c] - xx[k][c]);
                    }
                if (err < be) {
                    be = err, pb[sub] = pp;
                    memcpy(q[2 * sub], qq, sizeof(qq));
                    memcpy(ex[2 * sub], xx, sizeof(xx));
                }
            }
            for (int t = 0; t < 16; t++) {
                if (!mask[t]) continue;
                float bt = 1e30f;
                for (int i = 0; i < 8; i++) {
                    float err = 0;
                    for (int c = 0; c < 3; c++) {
                        float v = (float)((ex[2 * sub][c] * (64 - w3[i]) + ex[2 * sub + 1][c] * w3[i] + 32) >> 6) - px[t][c];
                        err += v * v;
                    }
                    if (err < bt) bt = err, idx[t] = i;
                }
                total += bt + (px[t][3] - 255) * (px[t][3] - 255);
            }
            int anchor = sub ? bc7_anchor2[p] : 0;
            if (idx[anchor] & 4) {
                for (int c = 0; c < 3; c++) {
                    int x = q[2 * sub][c];
                    q[2 * sub][c] = q[2 * sub + 1][c], q[2 * sub + 1][c] = x;
                }
                for (int t = 0; t < 16; t++)
                    if (mask[t]) idx[t] = 7 - idx[t];
            }
        }
        if (total >= best) continue;
        best = total;
        memset(out, 0, 16);
        int pos = 0;
        bits_put(out, &pos, 2, 2);
        bits_put(out, &pos, 6, (uint32_t)p);
        for (int c = 0; c < 3; c++)
            for (int e = 0; e < 4; e++) bits_put(out, &pos, 6, (uint32_t)q[e][c]);
        bits_put(out, &pos, 1, (uint32_t)pb[0]);
        bits_put(out, &pos, 1, (uint32_t)pb[1]);
        for (int t = 0; t < 16; t++) bits_put(out, &pos, t == 0 || t == bc7_anchor2[p] ? 2 : 3, (uint32_t)idx[t]);
    }
    return best;
}

/* BC7: mode 6, or mode 1 where it does better. */
static void enc_bc7(const float px[16][4], uint8_t *out)
{
    uint8_t m1[16];
    if (!bc7_part2[0][15] && !bc7_part2[0][0]) bc7_partitions();
    float e6 = enc_bc7_mode6(px, out), e1 = enc_bc7_mode1(px, m1);
    if (e1 < e6) memcpy(out, m1, 16);
}

/* HDR test images, linear RGB, for BC6H: photo-like (value noise in octaves as exp2 over about 9
 * stops, hard-edged patches 4x brighter, small lamps 16x brighter), an exponential gradient (12
 * stops corner to corner, its colour turning), and a sky (blue, brighter toward the horizon, soft
 * clouds, a sun with a glow up to about 60). */
static void hdr_px(int kind, int x, int y, float out[3])
{
    if (kind == IMG_HDR_GRADIENT) {
        float u = x / 255.0f, v = y / 255.0f, l = exp2f(-6.0f + 12.0f * (0.6f * u + 0.4f * v));
        out[0] = l * (0.6f + 0.4f * u), out[1] = l * (0.5f + 0.4f * sinf(2.0f * v + 1.0f)), out[2] = l * (0.9f - 0.6f * u * v);
        return;
    }
    if (kind == IMG_HDR_SKY) {
        float v = y / 255.0f, hz = 0.4f + 2.2f * v * v;
        float cl = fminf(fmaxf((fbm(x / 48.0f, y / 20.0f, 97, 5) - 0.48f) * 3.0f, 0.0f), 1.0f);
        float shade = 0.7f + 0.8f * fbm(x / 12.0f, y / 8.0f, 101, 3);
        float dx = x - 180.0f, dy = y - 70.0f, d = sqrtf(dx * dx + dy * dy);
        float sun = 60.0f * expf(-(d / 4.0f) * (d / 4.0f)) + 3.0f * expf(-d / 30.0f);
        const float sky[3] = {0.18f, 0.36f, 0.95f}, cloud[3] = {1.5f, 1.45f, 1.4f}, sc[3] = {1.0f, 0.85f, 0.6f};
        for (int c = 0; c < 3; c++) out[c] = (sky[c] * hz) * (1 - cl) + cloud[c] * shade * cl + sc[c] * sun * (1 - 0.7f * cl);
        return;
    }
    float l = fbm(x / 32.0f, y / 32.0f, 11, 5), c1 = fbm(x / 48.0f, y / 48.0f, 23, 3), c2 = fbm(x / 40.0f, y / 40.0f, 37, 3);
    float detail = fbm(x / 4.0f, y / 4.0f, 41, 2) - 0.5f;
    float lum = exp2f(-4.0f + 9.0f * l + 1.2f * detail);
    float r = 0.8f + 0.9f * (c1 - 0.5f), g = 0.7f - 0.5f * (c2 - 0.5f), b = 0.5f + 0.9f * (c2 - 0.5f);
    if (fbm(x / 64.0f, y / 64.0f, 53, 2) > 0.56f) lum *= 4.0f, r = 1.2f - 0.5f * r, g = 0.4f + 0.5f * g; /* hard-edged regions */
    int lx = x & 63, ly = y & 63, cx = 20 + (int)(lattice(x >> 6, y >> 6, 7) * 24), cy = 20 + (int)(lattice(x >> 6, y >> 6, 8) * 24);
    if ((lx - cx) * (lx - cx) + (ly - cy) * (ly - cy) <= 5) lum *= 16.0f; /* lamps */
    out[0] = lum * r, out[1] = lum * g, out[2] = lum * b;
}

static uint16_t f2h(float f)
{
    if (!(f > 0)) return 0;
    if (f >= 65504.0f) return 0x7bff;
    int e;
    float m = frexpf(f, &e); /* f = m * 2^e, m in [0.5, 1) */
    if (e < -13) return (uint16_t)lroundf(f * 16777216.0f); /* subnormal */
    int mm = (int)lroundf((m * 2 - 1) * 1024);
    if (mm == 1024) mm = 0, e++;
    return (uint16_t)(((e + 14) << 10) | mm);
}

/* BC6H, the modes this encoder uses as bcdec reads them: field (r0..r3, g0..g3, b0..b3 = 0..11,
 * partition 12), bits, shift, reversed. Mode 1 (two regions, 10-bit base, 5-bit deltas), mode 10
 * (two regions, 6 bits each), mode 11 (one region, 10 bits each), mode 14 (one region, 16-bit base,
 * 4-bit deltas). */
typedef struct bc6f {
    uint8_t f, n, sh, rev;
} bc6f;
static const bc6f bc6_l1[] = {{6, 1, 4, 0}, {10, 1, 4, 0}, {11, 1, 4, 0}, {0, 10, 0, 0}, {4, 10, 0, 0}, {8, 10, 0, 0}, {1, 5, 0, 0},
                              {7, 1, 4, 0}, {6, 4, 0, 0}, {5, 5, 0, 0}, {11, 1, 0, 0}, {7, 4, 0, 0}, {9, 5, 0, 0}, {11, 1, 1, 0},
                              {10, 4, 0, 0}, {2, 5, 0, 0}, {11, 1, 2, 0}, {3, 5, 0, 0}, {11, 1, 3, 0}, {12, 5, 0, 0}};
static const bc6f bc6_l10[] = {{0, 6, 0, 0}, {7, 1, 4, 0}, {11, 1, 0, 0}, {11, 1, 1, 0}, {10, 1, 4, 0}, {4, 6, 0, 0}, {6, 1, 5, 0}, {10, 1, 5, 0},
                               {11, 1, 2, 0}, {6, 1, 4, 0}, {8, 6, 0, 0}, {7, 1, 5, 0}, {11, 1, 3, 0}, {11, 1, 5, 0}, {11, 1, 4, 0}, {1, 6, 0, 0},
                               {6, 4, 0, 0}, {5, 6, 0, 0}, {7, 4, 0, 0}, {9, 6, 0, 0}, {10, 4, 0, 0}, {2, 6, 0, 0}, {3, 6, 0, 0}, {12, 5, 0, 0}};
static const bc6f bc6_l11[] = {{0, 10, 0, 0}, {4, 10, 0, 0}, {8, 10, 0, 0}, {1, 10, 0, 0}, {5, 10, 0, 0}, {9, 10, 0, 0}};
static const bc6f bc6_l14[] = {{0, 10, 0, 0}, {4, 10, 0, 0}, {8, 10, 0, 0}, {1, 4, 0, 0}, {0, 6, 10, 1},
                               {5, 4, 0, 0}, {4, 6, 10, 1}, {9, 4, 0, 0}, {8, 6, 10, 1}};
static const struct {
    uint32_t code, code_bits, w, d, two, delta;
    const bc6f *l;
    size_t n;
} bc6_modes[4] = {{0, 2, 10, 5, 1, 1, bc6_l1, sizeof(bc6_l1) / sizeof(bc6f)}, {30, 5, 6, 6, 1, 0, bc6_l10, sizeof(bc6_l10) / sizeof(bc6f)},
                  {3, 5, 10, 10, 0, 0, bc6_l11, sizeof(bc6_l11) / sizeof(bc6f)}, {15, 5, 16, 4, 0, 1, bc6_l14, sizeof(bc6_l14) / sizeof(bc6f)}};

/* bcdec's endpoint unquantization. */
static int bc6_unq(int q, int w, int sgn)
{
    if (!sgn) {
        if (w >= 15) return q;
        if (q == 0) return 0;
        if (q == (1 << w) - 1) return 0xffff;
        return ((q << 16) + 0x8000) >> w;
    }
    if (w >= 16) return q;
    int s = q < 0, v = s ? -q : q, u;
    if (v == 0) u = 0;
    else if (v >= (1 << (w - 1)) - 1) u = 0x7fff;
    else u = ((v << 15) + 0x4000) >> (w - 1);
    return s ? -u : u;
}

static int bc6_quant(float u, int w, int sgn)
{
    int hi = sgn ? (1 << (w - 1)) - 1 : (1 << w) - 1;
    int q = (int)lroundf(sgn ? u * (float)(1 << (w - 1)) / 32768.0f - 0.5f : u * (float)(1 << w) / 65536.0f - 0.5f), best = 0;
    float be = 1e30f;
    for (int k = q - 1; k <= q + 1; k++) {
        int c = k < 0 ? 0 : k > hi ? hi : k;
        float e = fabsf((float)bc6_unq(c, w, sgn) - u);
        if (e < be) be = e, best = c;
    }
    return best;
}

/* One BC6H block of mode m (bc6_modes) and partition part from half-float texels h; its error
 * (squared, in half-float steps, by bcdec's decode), 1e30 where the endpoints do not fit. */
static double bc6_try(const uint16_t h[16][3], int sgn, int m, int part, uint8_t *out)
{
    int w = (int)bc6_modes[m].w, two = (int)bc6_modes[m].two, ib = two ? 3 : 4;
    static const int w3[8] = {0, 9, 18, 27, 37, 46, 55, 64}, w4[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};
    const int *wt = two ? w3 : w4;
    float u[16][4];
    for (int t = 0; t < 16; t++) {
        for (int c = 0; c < 3; c++) u[t][c] = h[t][c] * (sgn ? 32.0f / 31.0f : 64.0f / 31.0f);
        u[t][3] = 0;
    }
    int q[4][3], idx[16];
    for (int s = 0; s < 1 + two; s++) {
        int mask[16];
        for (int t = 0; t < 16; t++) mask[t] = (two ? bc7_part2[part][t] : 0) == s;
        /* the principal axis of the subset, extremes along it */
        float mean[3] = {0}, cov[3][3] = {{0}}, ax[3] = {1, 1, 1};
        int n = 0;
        for (int t = 0; t < 16; t++)
            if (mask[t]) {
                for (int c = 0; c < 3; c++) mean[c] += u[t][c];
                n++;
            }
        for (int c = 0; c < 3; c++) mean[c] /= n;
        for (int t = 0; t < 16; t++)
            if (mask[t])
                for (int i = 0; i < 3; i++)
                    for (int j = 0; j < 3; j++) cov[i][j] += (u[t][i] - mean[i]) * (u[t][j] - mean[j]);
        for (int it = 0; it < 8; it++) {
            float nx[3] = {0}, l = 0;
            for (int i = 0; i < 3; i++) {
                for (int j = 0; j < 3; j++) nx[i] += cov[i][j] * ax[j];
                l += nx[i] * nx[i];
            }
            if (l < 1e-9f) break;
            for (int i = 0; i < 3; i++) ax[i] = nx[i] / sqrtf(l);
        }
        float t0 = 1e30f, t1 = -1e30f;
        for (int t = 0; t < 16; t++)
            if (mask[t]) {
                float d = 0;
                for (int c = 0; c < 3; c++) d += (u[t][c] - mean[c]) * ax[c];
                t0 = fminf(t0, d), t1 = fmaxf(t1, d);
            }
        int ue[2][3];
        for (int c = 0; c < 3; c++) {
            q[2 * s][c] = bc6_quant(mean[c] + ax[c] * t0, w, sgn);
            q[2 * s + 1][c] = bc6_quant(mean[c] + ax[c] * t1, w, sgn);
            ue[0][c] = bc6_unq(q[2 * s][c], w, sgn), ue[1][c] = bc6_unq(q[2 * s + 1][c], w, sgn);
        }
        for (int t = 0; t < 16; t++) {
            if (!mask[t]) continue;
            double be = 1e30;
            for (int i = 0; i < (1 << ib); i++) {
                double e = 0;
                for (int c = 0; c < 3; c++) {
                    int v = (ue[0][c] * (64 - wt[i]) + ue[1][c] * wt[i] + 32) >> 6;
                    float d = (float)(sgn ? (v * 31) >> 5 : (v * 31) >> 6) - h[t][c];
                    e += d * d;
                }
                if (e < be) be = e, idx[t] = i;
            }
        }
        int anchor = s ? bc7_anchor2[part] : 0;
        if (idx[anchor] >> (ib - 1)) { /* the anchor's index has no top bit: swap the ends */
            for (int c = 0; c < 3; c++) {
                int x = q[2 * s][c];
                q[2 * s][c] = q[2 * s + 1][c], q[2 * s + 1][c] = x;
            }
            for (int t = 0; t < 16; t++)
                if (mask[t]) idx[t] = (1 << ib) - 1 - idx[t];
        }
    }
    uint32_t f[13] = {0};
    int ne = two ? 4 : 2, d = (int)bc6_modes[m].d;
    for (int e = 0; e < ne; e++)
        for (int c = 0; c < 3; c++) {
            int v = q[e][c];
            if (e && bc6_modes[m].delta) {
                v -= q[0][c];
                if (v < -(1 << (d - 1)) || v >= (1 << (d - 1))) return 1e30;
            }
            f[c * 4 + e] = (uint32_t)v & ((1u << (e ? d : w)) - 1);
        }
    f[12] = (uint32_t)part;
    memset(out, 0, 16);
    int pos = 0;
    bits_put(out, &pos, (int)bc6_modes[m].code_bits, bc6_modes[m].code);
    for (size_t i = 0; i < bc6_modes[m].n; i++) {
        const bc6f *l = &bc6_modes[m].l[i];
        uint32_t v = (f[l->f] >> l->sh) & ((1u << l->n) - 1), r = 0;
        if (l->rev)
            for (int k = 0; k < l->n; k++) r |= ((v >> k) & 1) << (l->n - 1 - k);
        bits_put(out, &pos, l->n, l->rev ? r : v);
    }
    for (int t = 0; t < 16; t++) bits_put(out, &pos, ib - (t == 0 || (two && t == bc7_anchor2[part])), (uint32_t)idx[t]);
    uint16_t dec[16 * 3];
    bcdec_bc6h_half(out, dec, 12, sgn);
    double err = 0;
    for (int t = 0; t < 16; t++)
        for (int c = 0; c < 3; c++) {
            double x = (double)(int16_t)dec[t * 3 + c] - h[t][c];
            err += x * x;
        }
    return err;
}

/* BC6H block (UFLOAT or SFLOAT) from linear RGB texels: the best of modes 11 and 14 and, over the
 * 32 partitions, modes 1 and 10. */
static void enc_bc6h(const float px[16][3], int sgn, uint8_t *out)
{
    uint16_t h[16][3];
    for (int t = 0; t < 16; t++)
        for (int c = 0; c < 3; c++) h[t][c] = f2h(px[t][c]);
    if (!bc7_part2[0][15] && !bc7_part2[0][0]) bc7_partitions();
    uint8_t cand[16];
    double best = 1e300;
    for (int m = 0; m < 4; m++)
        for (int p = 0; p < (bc6_modes[m].two ? 32 : 1); p++) {
            double e = bc6_try(h, sgn, m, p, cand);
            if (e < best) best = e, memcpy(out, cand, 16);
        }
}

static int is_snorm(VkFormat f) { return f == VK_FORMAT_BC4_SNORM_BLOCK || f == VK_FORMAT_BC5_SNORM_BLOCK; }

/* One block of format f from texels px (8-bit RGBA; a normal map's x, y for BC5; BC6H takes the HDR
 * image's own texels). */
static void enc_block(VkFormat f, const uint8_t rgba[16][4], int kind, int bx, int by, uint8_t *out)
{
    float px[16][4], a[16];
    for (int t = 0; t < 16; t++)
        for (int c = 0; c < 4; c++) px[t][c] = rgba[t][c];
    switch (f) {
    case VK_FORMAT_BC1_RGB_UNORM_BLOCK: case VK_FORMAT_BC1_RGB_SRGB_BLOCK: enc_bc1(px, out, 0); break;
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: case VK_FORMAT_BC1_RGBA_SRGB_BLOCK: enc_bc1(px, out, 1); break;
    case VK_FORMAT_BC2_UNORM_BLOCK: case VK_FORMAT_BC2_SRGB_BLOCK:
        memset(out, 0, 8);
        for (int t = 0; t < 16; t++) out[t / 2] |= (uint8_t)((int)lroundf(px[t][3] / 17) << (4 * (t & 1)));
        enc_bc1(px, out + 8, 0);
        break;
    case VK_FORMAT_BC3_UNORM_BLOCK: case VK_FORMAT_BC3_SRGB_BLOCK:
        for (int t = 0; t < 16; t++) a[t] = px[t][3];
        enc_bc4(a, out, 0);
        enc_bc1(px, out + 8, 0);
        break;
    case VK_FORMAT_BC4_UNORM_BLOCK: case VK_FORMAT_BC4_SNORM_BLOCK: case VK_FORMAT_BC5_UNORM_BLOCK: case VK_FORMAT_BC5_SNORM_BLOCK:
        for (int ch = 0; ch < (f >= VK_FORMAT_BC5_UNORM_BLOCK ? 2 : 1); ch++) {
            for (int t = 0; t < 16; t++) {
                if (is_snorm(f) && kind == IMG_NORMAL) {
                    float n[3];
                    normal_at(bx * 4 + (t & 3), by * 4 + (t >> 2), n);
                    a[t] = roundf(n[ch] * 127);
                } else {
                    a[t] = is_snorm(f) ? px[t][ch] - 128 : px[t][ch];
                }
                if (a[t] < -127) a[t] = -127;
            }
            enc_bc4(a, out + 8 * ch, is_snorm(f));
        }
        break;
    case VK_FORMAT_BC6H_UFLOAT_BLOCK: case VK_FORMAT_BC6H_SFLOAT_BLOCK: {
        float h[16][3];
        for (int t = 0; t < 16; t++) hdr_px(kind, bx * 4 + (t & 3), by * 4 + (t >> 2), h[t]);
        enc_bc6h(h, f == VK_FORMAT_BC6H_SFLOAT_BLOCK, out);
        break;
    }
    default: enc_bc7(px, out); break;
    }
}

/* What a transcoded format is tested with. */
static int image_for(VkFormat f)
{
    if (f == VK_FORMAT_BC5_UNORM_BLOCK || f == VK_FORMAT_BC5_SNORM_BLOCK) return IMG_NORMAL;
    if (f == VK_FORMAT_BC1_RGBA_UNORM_BLOCK || f == VK_FORMAT_BC1_RGBA_SRGB_BLOCK) return IMG_ALPHA_CUT;
    if (f >= VK_FORMAT_BC2_UNORM_BLOCK && f <= VK_FORMAT_BC3_SRGB_BLOCK) return IMG_ALPHA_SOFT;
    if (f == VK_FORMAT_BC7_UNORM_BLOCK || f == VK_FORMAT_BC7_SRGB_BLOCK) return IMG_ALPHA_SOFT;
    if (f == VK_FORMAT_BC6H_UFLOAT_BLOCK || f == VK_FORMAT_BC6H_SFLOAT_BLOCK) return IMG_HDR_PHOTO;
    return IMG_PHOTO;
}

/* Blocks of a w x h image of `kind` whose top-left block is block (ox, oy) of a 256x256 image of
 * it (wrapping), encoded once per format and kind. */
static uint8_t *image_blocks(VkFormat f, int kind, uint32_t w, uint32_t h, uint32_t ox, uint32_t oy)
{
    static uint8_t *pool[VK_FORMAT_BC7_SRGB_BLOCK + 1][IMG_COUNT];
    int bytes = block_bytes(f);
    if (!pool[f][kind]) {
        pool[f][kind] = malloc((size_t)64 * 64 * bytes);
        for (int by = 0; by < 64; by++)
            for (int bx = 0; bx < 64; bx++) {
                uint8_t px[16][4] = {{0}};
                for (int t = 0; t < 16; t++)
                    if (kind < IMG_HDR_PHOTO) image_px(kind, bx * 4 + (t & 3), by * 4 + (t >> 2), px[t]);
                enc_block(f, px, kind, bx, by, pool[f][kind] + ((size_t)by * 64 + bx) * bytes);
            }
    }
    uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
    uint8_t *b = malloc((size_t)bw * bh * bytes);
    for (uint32_t by = 0; by < bh; by++)
        for (uint32_t bx = 0; bx < bw; bx++)
            memcpy(b + ((size_t)by * bw + bx) * bytes, pool[f][kind] + ((size_t)((by + oy) % 64) * 64 + (bx + ox) % 64) * bytes, bytes);
    return b;
}

/* ------------------------------------------------------------------ upload shapes */

static int is_srgb(VkFormat f)
{
    return f == VK_FORMAT_BC1_RGB_SRGB_BLOCK || f == VK_FORMAT_BC1_RGBA_SRGB_BLOCK || f == VK_FORMAT_BC2_SRGB_BLOCK ||
           f == VK_FORMAT_BC3_SRGB_BLOCK || f == VK_FORMAT_BC7_SRGB_BLOCK;
}

static int is_bc6h(VkFormat f) { return f == VK_FORMAT_BC6H_UFLOAT_BLOCK || f == VK_FORMAT_BC6H_SFLOAT_BLOCK; }

/* Transcoded by the bridge (in transcode mode): all but BC6H, and BC6H where the GPU samples ASTC HDR. */
static int transcoded(VkFormat f) { return tx && (!is_bc6h(f) || tx_hdr); }

/* Lowest PSNR (dB) a transcoded level may have against bcdec, colour and alpha, a little under the
 * worst measured on the test images (2026-10-13, RADV decoding ETC2/ASTC): BC1 -> ETC2 31.3 (rough),
 * BC7 -> ASTC 33.9 (rough), BC4/BC5 -> EAC 43.8, alpha 40.4. BC6H -> ASTC HDR: linear values against
 * the level's largest, 40 (the target; measured: see test_hdr_shader). */
static double psnr_floor(VkFormat f, int alpha)
{
    if (alpha) return 36;
    switch (f) {
    case VK_FORMAT_BC1_RGB_UNORM_BLOCK: case VK_FORMAT_BC1_RGB_SRGB_BLOCK: case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
        return 29;
    case VK_FORMAT_BC4_UNORM_BLOCK: case VK_FORMAT_BC4_SNORM_BLOCK: case VK_FORMAT_BC5_UNORM_BLOCK: case VK_FORMAT_BC5_SNORM_BLOCK: return 40;
    case VK_FORMAT_BC6H_UFLOAT_BLOCK: case VK_FORMAT_BC6H_SFLOAT_BLOCK: return 40;
    default: return 31;
    }
}

static double last_psnr, last_psnr_a; /* of the last compare_level in transcode mode (alpha: 0 if none) */

static double psnr(double se, long n, double peak) { return n && se > 0 ? 10 * log10(peak * peak * n / se) : 99; }

static float lin2srgb(float v)
{
    v = fminf(fmaxf(v, 0.0f), 1.0f);
    return v <= 0.0031308f ? v * 12.92f : 1.055f * powf(v, 1.0f / 2.4f) - 0.055f;
}

static VkBuffer host_buffer(VkDeviceSize size, VkDeviceMemory *mem, void **map)
{
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, size,
                              VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                              VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
    VkBuffer b;
    vkCreateBuffer(dev, &bci, NULL, &b);
    VkMemoryRequirements r;
    vkGetBufferMemoryRequirements(dev, b, &r);
    *mem = bind_mem(r, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vkBindBufferMemory(dev, b, *mem, 0);
    vkMapMemory(dev, *mem, 0, VK_WHOLE_SIZE, 0, map);
    return b;
}

static VkImage make_image(VkFormat f, uint32_t w, uint32_t h, uint32_t mips, uint32_t layers, VkImageCreateFlags flags,
                          const VkFormat *list, uint32_t nlist, VkDeviceMemory *mem)
{
    VkImageFormatListCreateInfo fl = {VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO, NULL, nlist, list};
    VkImageCreateInfo ici = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, nlist ? &fl : NULL, flags, VK_IMAGE_TYPE_2D, f, {w, h, 1}, mips, layers,
                             VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL,
                             VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                             VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
    VkImage img;
    if (vkCreateImage(dev, &ici, NULL, &img) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkMemoryRequirements r;
    vkGetImageMemoryRequirements(dev, img, &r);
    *mem = bind_mem(r, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkBindImageMemory(dev, img, *mem, 0);
    return img;
}

static void barrier_all(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to)
{
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT,
                              VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, from, to, VK_QUEUE_FAMILY_IGNORED,
                              VK_QUEUE_FAMILY_IGNORED, img, {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS}};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}

static void submit(VkCommandBuffer cb)
{
    vkEndCommandBuffer(cb);
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cb, 0, NULL};
    vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);
    vkFreeCommandBuffers(dev, pool, 1, &cb);
}

/* One mip level of one layer: its blocks, as uploaded. */
typedef struct level {
    uint32_t w, h, bw, bh;
    uint8_t *blocks;
} level;

/* Where levels are read back, kept for the whole run: through the bridge on the desktop, reading a
 * fresh host-visible allocation that replaced a freed one sometimes saw zeros where the GPU had
 * copied (a bridge memory issue of its own, not BC emulation). */
static VkBuffer readback;
static void *readback_map;

/* Blits a level of a BC image (in TRANSFER_SRC_OPTIMAL) to RGBA - 32F, or 8-bit UNORM for big
 * ones - and compares it with bcdec. Returns the values that differ. */
static int compare_level(VkImage bc, VkFormat f, uint32_t mip, uint32_t layer, const level *lv, float tol, const char *name)
{
    int big = lv->w * lv->h > 1024 * 1024;
    VkFormat rf = big ? VK_FORMAT_R8G8B8A8_UNORM : VK_FORMAT_R32G32B32A32_SFLOAT;
    VkDeviceMemory rm;
    VkImage rgba = make_image(rf, lv->w, lv->h, 1, 1, 0, NULL, 0, &rm);
    if (!readback) {
        VkDeviceMemory bm;
        readback = host_buffer((VkDeviceSize)4096 * 4096 * 4, &bm, &readback_map);
    }
    VkBuffer down = readback;
    const void *map = readback_map;
    VkCommandBuffer cb = begin();
    barrier(cb, rgba, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageBlit bl = {{VK_IMAGE_ASPECT_COLOR_BIT, mip, layer, 1}, {{0, 0, 0}, {(int32_t)lv->w, (int32_t)lv->h, 1}},
                      {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{0, 0, 0}, {(int32_t)lv->w, (int32_t)lv->h, 1}}};
    vkCmdBlitImage(cb, bc, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rgba, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_NEAREST);
    barrier(cb, rgba, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy rb = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {lv->w, lv->h, 1}};
    vkCmdCopyImageToBuffer(cb, rgba, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, down, 1, &rb);
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    submit(cb);
    int bad = 0, bytes = block_bytes(f), six = f == VK_FORMAT_BC6H_UFLOAT_BLOCK || f == VK_FORMAT_BC6H_SFLOAT_BLOCK;
    if (transcoded(f)) {
        /* lossy: squared errors of the colour channels and of alpha */
        int nc = f <= VK_FORMAT_BC4_SNORM_BLOCK && f >= VK_FORMAT_BC4_UNORM_BLOCK ? 1 : f <= VK_FORMAT_BC5_SNORM_BLOCK && f >= VK_FORMAT_BC5_UNORM_BLOCK ? 2 : 3;
        int alpha = nc == 3 && f != VK_FORMAT_BC1_RGB_UNORM_BLOCK && f != VK_FORMAT_BC1_RGB_SRGB_BLOCK && !six;
        double se = 0, sea = 0, top = 0;
        long n = 0;
        for (uint32_t by = 0; by < lv->bh; by++)
            for (uint32_t bx = 0; bx < lv->bw; bx++) {
                float want[16][4];
                decode_block(f, lv->blocks + ((size_t)by * lv->bw + bx) * bytes, want);
                for (uint32_t t = 0; t < 16; t++) {
                    uint32_t x = bx * 4 + (t & 3), y = by * 4 + (t >> 2);
                    if (x >= lv->w || y >= lv->h) continue;
                    for (int ch = 0; ch < 4; ch++) {
                        size_t i = ((size_t)y * lv->w + x) * 4 + ch;
                        float g = big ? ((const uint8_t *)map)[i] / 255.0f : ((const float *)map)[i], w = want[t][ch];
                        if (is_srgb(f) && ch < 3) g = lin2srgb(g);
                        if (ch < nc && six && (!isfinite(w) || w < 0)) continue; /* ASTC HDR has no sign */
                        if (ch < nc) se += (double)(g - w) * (g - w), top = fmax(top, w);
                        else if (ch == 3 && alpha) sea += (double)(g - w) * (g - w);
                    }
                    n++;
                }
            }
        double peak = is_snorm(f) ? 2.0 : six ? top : 1.0; /* BC6H: the level's largest value */
        last_psnr = psnr(se, n * nc, peak);
        last_psnr_a = alpha ? psnr(sea, n, 1.0) : 0;
        if (last_psnr < min_psnr) min_psnr = last_psnr;
        if (getenv("BCN_VERBOSE")) printf("    %s: mip %u layer %u (%ux%u): PSNR %.1f dB, alpha %.1f dB\n", name, mip, layer, lv->w, lv->h, last_psnr, last_psnr_a);
        if (last_psnr < psnr_floor(f, 0) || (alpha && last_psnr_a < psnr_floor(f, 1))) {
            printf("  %s: mip %u layer %u (%ux%u): PSNR %.1f dB, alpha %.1f dB\n", name, mip, layer, lv->w, lv->h, last_psnr, last_psnr_a);
            bad = 1;
        }
        vkDestroyImage(dev, rgba, NULL);
        vkFreeMemory(dev, rm, NULL);
        return bad;
    }
    for (uint32_t by = 0; by < lv->bh; by++)
        for (uint32_t bx = 0; bx < lv->bw; bx++) {
            float want[16][4];
            decode_block(f, lv->blocks + ((size_t)by * lv->bw + bx) * bytes, want);
            for (uint32_t t = 0; t < 16; t++) {
                uint32_t x = bx * 4 + (t & 3), y = by * 4 + (t >> 2);
                if (x >= lv->w || y >= lv->h) continue;
                for (int ch = 0; ch < 4; ch++) {
                    size_t i = ((size_t)y * lv->w + x) * 4 + ch;
                    float g = big ? ((const uint8_t *)map)[i] / 255.0f : ((const float *)map)[i], w = want[t][ch];
                    if (isnan(w) || isinf(w)) continue;
                    if (is_srgb(f) && ch < 3) g = lin2srgb(g);
                    float lim = six ? tol * fmaxf(1.0f, fabsf(w)) : tol;
                    if (!(fabsf(g - w) <= lim) && !texel_ok(f, ch, g, w, lim)) {
                        if (!bad)
                            printf("  %s: mip %u layer %u (%u,%u) ch %d: got %g want %g\n", name, mip, layer, x, y, ch, g, w);
                        bad++;
                    }
                }
            }
        }
    vkDestroyImage(dev, rgba, NULL);
    vkFreeMemory(dev, rm, NULL);
    return bad;
}

/* Random blocks, or in transcode mode part of a test image (at a random place). */
static void random_level(VkFormat f, level *lv, uint32_t w, uint32_t h)
{
    int bytes = block_bytes(f);
    lv->w = w;
    lv->h = h;
    lv->bw = (w + 3) / 4;
    lv->bh = (h + 3) / 4;
    if (transcoded(f)) {
        lv->blocks = image_blocks(f, image_for(f), w, h, rnd() % 64, rnd() % 64);
        return;
    }
    lv->blocks = malloc((size_t)lv->bw * lv->bh * bytes);
    for (uint32_t i = 0; i < lv->bw * lv->bh; i++) make_block(f, lv->blocks + (size_t)i * bytes, bytes, (int)i);
}

/* A 60x44 texture with 6 mips (down to 1x1, partial blocks from 15x11 on) and 3 layers: mip 0 one
 * region per layer (layer 0 with padded rows), then a 32x16 sub-region of layer 0 over it (through
 * vkCmdCopyBufferToImage2 when there is one), then each smaller mip in one region of all layers.
 * Mutable images get a view in `view_fmt`; mip 0 layer 1 is also copied to a second BC image. */
static void test_shapes(VkFormat f, const char *name, float tol, VkImageCreateFlags flags, const VkFormat *list, uint32_t nlist,
                        VkFormat view_fmt)
{
    enum { W = 60, H = 44, MIPS = 6, LAYERS = 3 };
    int bytes = block_bytes(f);
    level lv[MIPS][LAYERS], sub;
    min_psnr = 99;
    VkDeviceSize total = 4096;
    for (uint32_t m = 0; m < MIPS; m++)
        for (uint32_t l = 0; l < LAYERS; l++) {
            uint32_t w = W >> m, h = H >> m;
            random_level(f, &lv[m][l], w ? w : 1, h ? h : 1);
            total += (VkDeviceSize)(lv[m][l].bw + 2) * lv[m][l].bh * bytes + 256;
        }
    random_level(f, &sub, 32, 16);
    total += (VkDeviceSize)sub.bw * sub.bh * bytes;
    VkDeviceMemory um, im, cm;
    void *upp;
    VkBuffer up = host_buffer(total, &um, &upp);
    memset(upp, 0xcd, total);
    VkImage img = make_image(f, W, H, MIPS, LAYERS, flags, list, nlist, &im);
    VkImage copy = make_image(f, W, H, 1, 1, 0, NULL, 0, &cm);
    if (!img || !copy) {
        printf("FAIL %s: cannot create the images\n", name);
        failures++;
        return;
    }
    if (view_fmt) {
        VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, img, VK_IMAGE_VIEW_TYPE_2D_ARRAY, view_fmt, {0},
                                     {VK_IMAGE_ASPECT_COLOR_BIT, 0, MIPS, 0, LAYERS}};
        VkImageView v;
        if (vkCreateImageView(dev, &vci, NULL, &v) != VK_SUCCESS) {
            printf("FAIL %s: cannot create a view in format %d\n", name, view_fmt);
            failures++;
        } else {
            vkDestroyImageView(dev, v, NULL);
        }
    }
    VkCommandBuffer cb = begin();
    barrier_all(cb, img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    barrier_all(cb, copy, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkDeviceSize off = 32;
    /* mip 0, a region per layer; layer 0's rows padded by 2 blocks */
    VkBufferImageCopy r0[LAYERS];
    for (uint32_t l = 0; l < LAYERS; l++) {
        uint32_t row = l == 0 ? lv[0][l].bw + 2 : lv[0][l].bw;
        for (uint32_t by = 0; by < lv[0][l].bh; by++)
            memcpy((uint8_t *)upp + off + (size_t)by * row * bytes, lv[0][l].blocks + (size_t)by * lv[0][l].bw * bytes, (size_t)lv[0][l].bw * bytes);
        r0[l] = (VkBufferImageCopy){off, l == 0 ? row * 4 : 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, l, 1}, {0, 0, 0}, {W, H, 1}};
        off += (VkDeviceSize)row * lv[0][l].bh * bytes;
        off = (off + 255) & ~(VkDeviceSize)255;
    }
    vkCmdCopyBufferToImage(cb, up, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, LAYERS, r0);
    /* a 32x16 sub-region of mip 0 layer 0 at (16, 8) */
    memcpy((uint8_t *)upp + off, sub.blocks, (size_t)sub.bw * sub.bh * bytes);
    for (uint32_t by = 0; by < sub.bh; by++)
        memcpy(lv[0][0].blocks + ((size_t)(by + 2) * lv[0][0].bw + 4) * bytes, sub.blocks + (size_t)by * sub.bw * bytes, (size_t)sub.bw * bytes);
    if (vkCmdCopyBufferToImage2) {
        VkBufferImageCopy2 r2 = {VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2, NULL, off, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {16, 8, 0}, {32, 16, 1}};
        VkCopyBufferToImageInfo2 ci2 = {VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2, NULL, up, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r2};
        vkCmdCopyBufferToImage2(cb, &ci2);
    } else {
        VkBufferImageCopy r1 = {off, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {16, 8, 0}, {32, 16, 1}};
        vkCmdCopyBufferToImage(cb, up, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r1);
    }
    off += (VkDeviceSize)sub.bw * sub.bh * bytes;
    off = (off + 255) & ~(VkDeviceSize)255;
    /* smaller mips: one region holding every layer */
    VkBufferImageCopy rm[MIPS];
    for (uint32_t m = 1; m < MIPS; m++) {
        rm[m] = (VkBufferImageCopy){off, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, m, 0, LAYERS}, {0, 0, 0}, {lv[m][0].w, lv[m][0].h, 1}};
        for (uint32_t l = 0; l < LAYERS; l++) {
            size_t n = (size_t)lv[m][l].bw * lv[m][l].bh * bytes;
            memcpy((uint8_t *)upp + off, lv[m][l].blocks, n);
            off += n;
        }
        off = (off + 255) & ~(VkDeviceSize)255;
    }
    vkCmdCopyBufferToImage(cb, up, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, MIPS - 1, &rm[1]);
    barrier_all(cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkImageCopy ic = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 1}, {0, 0, 0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {W, H, 1}};
    vkCmdCopyImage(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, copy, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &ic);
    barrier_all(cb, copy, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    submit(cb);
    int bad = 0;
    for (uint32_t m = 0; m < MIPS; m++)
        for (uint32_t l = 0; l < LAYERS; l++) bad += compare_level(img, f, m, l, &lv[m][l], tol, name);
    /* Through the bridge a BC1 UNORM image mutable into sRGB stays RGBA8 while the copy is 16-bit,
     * and the copy becomes a blit: RADV rounds RGBA8 -> A1R5G5B5 toward zero (17/31 for 144/255),
     * which the spec leaves to the implementation, so one 5-bit step is allowed there. */
    float ctol = (flags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT) && is_bc1_unorm(f) ? 1.0f / 31.0f + 1.0f / 255.0f : tol;
    bad += compare_level(copy, f, 0, 0, &lv[0][1], ctol, name);
    if (bad) {
        printf("FAIL %s: %d values differ from bcdec\n", name, bad);
        failures++;
    } else {
        printf("PASS %s: %d mips x %d layers, sub-region%s, BC -> BC copy", name, MIPS, LAYERS,
               vkCmdCopyBufferToImage2 ? " (CopyBufferToImage2)" : "");
        if (transcoded(f)) printf(", PSNR >= %.1f dB", min_psnr);
        printf("\n");
    }
    for (uint32_t m = 0; m < MIPS; m++)
        for (uint32_t l = 0; l < LAYERS; l++) free(lv[m][l].blocks);
    free(sub.blocks);
    vkDestroyImage(dev, img, NULL);
    vkDestroyImage(dev, copy, NULL);
    vkDestroyBuffer(dev, up, NULL);
    vkFreeMemory(dev, um, NULL);
    vkFreeMemory(dev, im, NULL);
    vkFreeMemory(dev, cm, NULL);
}

/* Transcode mode: a 256x256 test image of each kind in format f, uploaded and compared with bcdec
 * (PSNR of the transcoded texels against the BC texels, colour and alpha). */
static void test_quality(VkFormat f, const char *name, const int *kinds, int nk)
{
    for (int k = 0; k < nk; k++) {
        enum { S = 256 };
        level lv = {S, S, S / 4, S / 4, image_blocks(f, kinds[k], S, S, 0, 0)};
        size_t n = (size_t)lv.bw * lv.bh * block_bytes(f);
        VkDeviceMemory um, im;
        void *upp;
        VkBuffer up = host_buffer(n, &um, &upp);
        memcpy(upp, lv.blocks, n);
        VkImage img = make_image(f, S, S, 1, 1, 0, NULL, 0, &im);
        if (!img) {
            printf("FAIL %s: cannot create the image\n", name);
            failures++;
            return;
        }
        VkCommandBuffer cb = begin();
        barrier_all(cb, img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy r = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {S, S, 1}};
        vkCmdCopyBufferToImage(cb, up, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
        barrier_all(cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        submit(cb);
        int bad = compare_level(img, f, 0, 0, &lv, 0, name);
        /* exact would mean the bridge decoded instead of transcoding */
        if (last_psnr >= 99) bad = 1;
        printf("%s %s, %s: PSNR %.1f dB", bad ? "FAIL" : "PASS", name, img_name[kinds[k]], last_psnr);
        if (last_psnr_a) printf(", alpha %.1f dB", last_psnr_a);
        printf(" (floor %.0f)\n", psnr_floor(f, 0));
        failures += bad;
        free(lv.blocks);
        vkDestroyImage(dev, img, NULL);
        vkDestroyBuffer(dev, up, NULL);
        vkFreeMemory(dev, um, NULL);
        vkFreeMemory(dev, im, NULL);
    }
}

/* ------------------------------------------------------------------ BC6H -> ASTC HDR, the shader itself */

/* The bridge's BC6H transcoder (spv_bc6h_astc from server/bc_spv.h) run directly over bw x bh
 * blocks, as the bridge dispatches it: the ASTC blocks (malloc'd), NULL if it cannot run. Lets the
 * encoder be checked by PSNR (astc_decode) on a GPU that cannot sample ASTC HDR, and on one that
 * can, apart from how the bridge picks and creates the image. */
static uint8_t *run_bc6h_astc(const uint8_t *blocks, uint32_t bw, uint32_t bh, int sgn, double *ms)
{
    static VkDescriptorSetLayout dsl;
    static VkPipelineLayout layout;
    static VkPipeline pipe;
    static VkDescriptorPool dpool;
    static VkDescriptorSet set;
    if (!pipe) {
        VkDescriptorSetLayoutBinding b[2] = {{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
                                             {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
        VkDescriptorSetLayoutCreateInfo dci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 2, b};
        VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 32};
        VkPipelineLayoutCreateInfo plci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &dsl, 1, &pr};
        VkShaderModuleCreateInfo smci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof(spv_bc6h_astc), spv_bc6h_astc};
        VkShaderModule sm;
        if (vkCreateDescriptorSetLayout(dev, &dci, NULL, &dsl) || vkCreatePipelineLayout(dev, &plci, NULL, &layout) ||
            vkCreateShaderModule(dev, &smci, NULL, &sm))
            return NULL;
        VkComputePipelineCreateInfo cpci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, NULL, 0,
                                            {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_COMPUTE_BIT, sm, "main", NULL},
                                            layout, VK_NULL_HANDLE, 0};
        VkResult r = vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, NULL, &pipe);
        vkDestroyShaderModule(dev, sm, NULL);
        VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2};
        VkDescriptorPoolCreateInfo dpci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 1, 1, &ps};
        VkDescriptorSetAllocateInfo dai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, VK_NULL_HANDLE, 1, &dsl};
        if (r || vkCreateDescriptorPool(dev, &dpci, NULL, &dpool)) return NULL;
        dai.descriptorPool = dpool;
        if (vkAllocateDescriptorSets(dev, &dai, &set)) return NULL;
    }
    VkDeviceSize n = (VkDeviceSize)bw * bh * 16;
    VkDeviceMemory sm, dm;
    void *sp, *dp;
    VkBuffer src = host_buffer(n, &sm, &sp), dst = host_buffer(n, &dm, &dp);
    memcpy(sp, blocks, n);
    memset(dp, 0, n);
    VkDescriptorBufferInfo bi[2] = {{src, 0, VK_WHOLE_SIZE}, {dst, 0, VK_WHOLE_SIZE}};
    VkWriteDescriptorSet wr[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &bi[0], NULL},
                                  {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &bi[1], NULL}};
    vkUpdateDescriptorSets(dev, 2, wr, 0, NULL);
    /* push constants as the bridge's pc_t: src_word, row_blocks, layer_blocks, flags (2: signed),
     * dst_word, pad, extent */
    uint32_t pc[8] = {0, bw, bw * bh, sgn ? 2u : 0u, 0, 0, bw * 4, bh * 4};
    VkCommandBuffer cb = begin();
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, NULL);
    vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), pc);
    vkCmdDispatch(cb, (bw + 7) / 8, (bh + 7) / 8, 1);
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    submit(cb);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    if (ms) *ms = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
    uint8_t *out = malloc(n);
    memcpy(out, dp, n);
    vkDestroyBuffer(dev, src, NULL);
    vkDestroyBuffer(dev, dst, NULL);
    vkFreeMemory(dev, sm, NULL);
    vkFreeMemory(dev, dm, NULL);
    return out;
}

/* PSNR (dB, linear values, peak: the largest reference value) of ASTC blocks against the BC6H
 * blocks they were made from, as bcdec decodes them; *undecoded: ASTC blocks astc_decode refused.
 * References that are not finite, and negative ones (ASTC HDR has no sign), are left out. */
static double astc_psnr(VkFormat f, const uint8_t *bc, const uint8_t *astc, uint32_t nblocks, int *undecoded)
{
    double se = 0, peak = 0;
    long n = 0;
    *undecoded = 0;
    for (uint32_t i = 0; i < nblocks; i++) {
        float want[16][4], got[16][4];
        decode_block(f, bc + 16 * i, want);
        if (!astc_decode(astc + 16 * i, got)) {
            (*undecoded)++;
            continue;
        }
        for (int t = 0; t < 16; t++)
            for (int c = 0; c < 3; c++) {
                float w = want[t][c];
                if (!isfinite(w) || w < 0) continue;
                if (!isfinite(got[t][c])) got[t][c] = 1e9f;
                se += (double)(got[t][c] - w) * (got[t][c] - w);
                if (w > peak) peak = w;
                n++;
            }
    }
    return psnr(se, n, peak);
}

/* The transcoder over the HDR test images (UFLOAT and SFLOAT), each 256x256, by PSNR against
 * bcdec; then over random blocks of all 14 modes (wide, often extreme values: every block must
 * decode, PSNR only reported). */
static void test_hdr_shader(void)
{
    static const VkFormat fmts[2] = {VK_FORMAT_BC6H_UFLOAT_BLOCK, VK_FORMAT_BC6H_SFLOAT_BLOCK};
    static const int kinds[3] = {IMG_HDR_PHOTO, IMG_HDR_GRADIENT, IMG_HDR_SKY};
    for (int fi = 0; fi < 2; fi++) {
        VkFormat f = fmts[fi];
        const char *name = fi ? "BC6H sfloat -> ASTC 4x4 HDR (shader, CPU decode)" : "BC6H ufloat -> ASTC 4x4 HDR (shader, CPU decode)";
        for (int k = 0; k < 3; k++) {
            enum { S = 256 };
            uint8_t *bc = image_blocks(f, kinds[k], S, S, 0, 0);
            double ms;
            uint8_t *astc = run_bc6h_astc(bc, S / 4, S / 4, fi, &ms);
            if (!astc) {
                printf("FAIL %s: cannot run the transcoder\n", name);
                failures++;
                free(bc);
                return;
            }
            int und;
            double p = astc_psnr(f, bc, astc, (S / 4) * (S / 4), &und);
            int bad = und || p < psnr_floor(f, 0) || p >= 99;
            printf("%s %s, %s: PSNR %.1f dB (floor %.0f)", bad ? "FAIL" : "PASS", name, img_name[kinds[k]], p, psnr_floor(f, 0));
            if (und) printf(", %d blocks undecodable", und);
            if (getenv("BCN_VERBOSE")) printf(", %.2f ms", ms);
            printf("\n");
            failures += bad;
            free(bc);
            free(astc);
        }
        enum { N = 4096 };
        uint8_t *bc = malloc(N * 16);
        for (int i = 0; i < N; i++) make_block(f, bc + 16 * i, 16, i);
        uint8_t *astc = run_bc6h_astc(bc, 64, N / 64, fi, NULL);
        int und = 0;
        double p = astc ? astc_psnr(f, bc, astc, N, &und) : 0;
        if (!astc || und) {
            printf("FAIL %s, random blocks of all 14 modes: %d of %d blocks undecodable\n", name, astc ? und : N, N);
            failures++;
        } else {
            printf("PASS %s, random blocks of all 14 modes: every block decodes (PSNR %.1f dB, no floor)\n", name, p);
        }
        free(bc);
        free(astc);
    }
}

/* What the image costs: through the bridge the decoded image's requirements, natively the BC
 * image's. BC5 decodes to RG8 (2 bytes a texel) and BC4 to R8, also when mutable between UNORM
 * and SNORM as DXVK makes every BC5 image; RGBA8 would be twice that. */
static void test_size(VkFormat f, const char *name, uint32_t texel_bytes, VkImageCreateFlags flags, const VkFormat *list, uint32_t nlist)
{
    enum { W = 512, H = 512 };
    VkDeviceMemory m;
    VkImage img = make_image(f, W, H, 1, 1, flags, list, nlist, &m);
    if (!img) {
        printf("FAIL %s: cannot create the image\n", name);
        failures++;
        return;
    }
    VkMemoryRequirements r;
    vkGetImageMemoryRequirements(dev, img, &r);
    /* room for tiling padding and metadata, not for twice the texel size */
    VkDeviceSize max = (VkDeviceSize)W * H * texel_bytes * 5 / 4 + 65536;
    if (tx) {
        /* a GPU that emulates ETC2/ASTC (RADV) adds a decoded copy: only reported */
        printf("INFO %s: %llu bytes for %ux%u (%.2f bytes a texel)\n", name, (unsigned long long)r.size, W, H, (double)r.size / (W * H));
    } else if (r.size > max) {
        printf("FAIL %s: %llu bytes for %ux%u, more than %u bytes a texel\n", name, (unsigned long long)r.size, W, H, texel_bytes);
        failures++;
    } else {
        printf("PASS %s: %llu bytes for %ux%u\n", name, (unsigned long long)r.size, W, H);
    }
    vkDestroyImage(dev, img, NULL);
    vkFreeMemory(dev, m, NULL);
}

/* 4096x4096 BC7, 2 layers, in one region: 128 MiB decoded, more than the bridge decodes into one
 * command buffer's scratch before reusing it. BCN_BENCH=1: also BC1, BC3, BC4 and BC5, for the
 * upload times. */
static void test_large(VkFormat f, const char *name)
{
    enum { S = 4096, LAYERS = 2 };
    level lv[LAYERS];
    VkDeviceSize per = (VkDeviceSize)(S / 4) * (S / 4) * block_bytes(f);
    VkDeviceMemory um, im;
    void *upp;
    VkBuffer up = host_buffer(per * LAYERS, &um, &upp);
    VkImage img = make_image(f, S, S, 1, LAYERS, 0, NULL, 0, &im);
    if (!img) {
        printf("FAIL %s 4096x4096x2: cannot create the image\n", name);
        failures++;
        return;
    }
    for (uint32_t l = 0; l < LAYERS; l++) {
        random_level(f, &lv[l], S, S);
        memcpy((uint8_t *)upp + per * l, lv[l].blocks, per);
    }
    VkCommandBuffer cb = begin();
    barrier_all(cb, img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy r = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, LAYERS}, {0, 0, 0}, {S, S, 1}};
    vkCmdCopyBufferToImage(cb, up, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
    barrier_all(cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    submit(cb);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    printf("INFO %s 4096x4096x2 upload: %.1f ms (submit to idle)\n", name, (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6);
    int bad = 0;
    for (uint32_t l = 0; l < LAYERS; l++) bad += compare_level(img, f, 0, l, &lv[l], 2.5f / 255.0f, name);
    if (bad) {
        printf("FAIL %s 4096x4096x2: %d values differ from bcdec\n", name, bad);
        failures++;
    } else {
        printf("PASS %s 4096x4096x2: %d texels match bcdec%s\n", name, S * S * LAYERS, tx ? " (PSNR)" : "");
    }
    for (uint32_t l = 0; l < LAYERS; l++) free(lv[l].blocks);
    vkDestroyImage(dev, img, NULL);
    vkDestroyBuffer(dev, up, NULL);
    vkFreeMemory(dev, um, NULL);
    vkFreeMemory(dev, im, NULL);
}

/* 8 command buffers recorded before any is submitted, each uploading a 1024x1024x4 BC7 texture
 * (16 MiB decoded) after a small BC1 one: 130 MiB of decode in flight, past the bridge's 96 MiB
 * cap on scratch, so the later ones decode through what they already hold. */
static void test_many_cbs(void)
{
    const VkFormat f = VK_FORMAT_BC7_UNORM_BLOCK;
    enum { NCB = 8, S = 1024, LAYERS = 4 };
    VkDeviceSize per = (VkDeviceSize)(S / 4) * (S / 4) * 16;
    level lv[NCB][LAYERS], small[NCB];
    VkImage img[NCB], simg[NCB];
    VkDeviceMemory im[NCB], sm[NCB], um;
    VkCommandBuffer cbs[NCB];
    void *upp;
    VkBuffer up = host_buffer(per * LAYERS * NCB + (VkDeviceSize)NCB * 4096, &um, &upp);
    for (int c = 0; c < NCB; c++) {
        img[c] = make_image(f, S, S, 1, LAYERS, 0, NULL, 0, &im[c]);
        simg[c] = make_image(VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 60, 44, 1, 1, 0, NULL, 0, &sm[c]);
        if (!img[c] || !simg[c]) {
            printf("FAIL %d command buffers: cannot create the images\n", NCB);
            failures++;
            return;
        }
        VkDeviceSize base = (per * LAYERS + 4096) * c;
        random_level(VK_FORMAT_BC1_RGBA_UNORM_BLOCK, &small[c], 60, 44);
        memcpy((uint8_t *)upp + base, small[c].blocks, (size_t)small[c].bw * small[c].bh * 8);
        for (uint32_t l = 0; l < LAYERS; l++) {
            random_level(f, &lv[c][l], S, S);
            memcpy((uint8_t *)upp + base + 4096 + per * l, lv[c][l].blocks, per);
        }
        cbs[c] = begin();
        barrier_all(cbs[c], simg[c], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        barrier_all(cbs[c], img[c], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy rs = {base, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {60, 44, 1}};
        vkCmdCopyBufferToImage(cbs[c], up, simg[c], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rs);
        VkBufferImageCopy r = {base + 4096, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, LAYERS}, {0, 0, 0}, {S, S, 1}};
        vkCmdCopyBufferToImage(cbs[c], up, img[c], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
        barrier_all(cbs[c], simg[c], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        barrier_all(cbs[c], img[c], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        vkEndCommandBuffer(cbs[c]);
    }
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, NCB, cbs, 0, NULL};
    vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);
    vkFreeCommandBuffers(dev, pool, NCB, cbs);
    int bad = 0;
    for (int c = 0; c < NCB; c++) {
        bad += compare_level(simg[c], VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 0, 0, &small[c], 2.5f / 255.0f, "many command buffers (BC1)");
        for (uint32_t l = 0; l < LAYERS; l++) bad += compare_level(img[c], f, 0, l, &lv[c][l], 2.5f / 255.0f, "many command buffers");
    }
    if (bad) {
        printf("FAIL %d command buffers in flight: %d values differ from bcdec\n", NCB, bad);
        failures++;
    } else {
        printf("PASS %d command buffers in flight: %d texels match bcdec%s\n", NCB, NCB * (S * S * LAYERS + 60 * 44), tx ? " (PSNR)" : "");
    }
    for (int c = 0; c < NCB; c++) {
        for (uint32_t l = 0; l < LAYERS; l++) free(lv[c][l].blocks);
        free(small[c].blocks);
        vkDestroyImage(dev, img[c], NULL);
        vkDestroyImage(dev, simg[c], NULL);
        vkFreeMemory(dev, im[c], NULL);
        vkFreeMemory(dev, sm[c], NULL);
    }
    vkDestroyBuffer(dev, up, NULL);
    vkFreeMemory(dev, um, NULL);
}

int main(int argc, char **argv)
{
    int gpu = 0, hdr_only = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "transcode")) tx = 1;
        else if (!strcmp(argv[i], "hdr")) hdr_only = 1;
        else gpu = atoi(argv[i]);
    }
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
    gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
#define G(n) n = (PFN_##n)gipa(inst, #n)
    G(vkCreateInstance);
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "bcn_check", 1, NULL, 0, VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 0, NULL};
    if (vkCreateInstance(&ici, NULL, &inst)) return 2;
    G(vkEnumeratePhysicalDevices); G(vkGetPhysicalDeviceMemoryProperties); G(vkCreateDevice); G(vkGetDeviceProcAddr);
    G(vkGetPhysicalDeviceFeatures); G(vkGetPhysicalDeviceFormatProperties); G(vkGetPhysicalDeviceProperties);
    uint32_t n = 8;
    VkPhysicalDevice pds[8];
    vkEnumeratePhysicalDevices(inst, &n, pds);
    pd = pds[gpu];
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(pd, &props);
    VkPhysicalDeviceFeatures feats;
    vkGetPhysicalDeviceFeatures(pd, &feats);
    printf("%s, textureCompressionBC=%u\n", props.deviceName, feats.textureCompressionBC);
    if (!feats.textureCompressionBC) return 1;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    float prio = 1;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, 0, 1, &prio};
    VkPhysicalDeviceFeatures en = {0};
    en.textureCompressionBC = VK_TRUE;
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &qci, 0, NULL, 0, NULL, &en};
    if (vkCreateDevice(pd, &dci, NULL, &dev)) return 2;
#undef G
#define G(n) n = (PFN_##n)vkGetDeviceProcAddr(dev, #n)
    G(vkGetDeviceQueue); G(vkCreateCommandPool); G(vkAllocateCommandBuffers); G(vkBeginCommandBuffer); G(vkEndCommandBuffer);
    G(vkQueueSubmit); G(vkQueueWaitIdle); G(vkCreateBuffer); G(vkGetBufferMemoryRequirements); G(vkAllocateMemory);
    G(vkBindBufferMemory); G(vkMapMemory); G(vkCreateImage); G(vkGetImageMemoryRequirements); G(vkBindImageMemory);
    G(vkCmdPipelineBarrier); G(vkCmdCopyBufferToImage); G(vkCmdBlitImage); G(vkCmdCopyImageToBuffer); G(vkDestroyImage);
    G(vkDestroyBuffer); G(vkFreeMemory); G(vkCreateImageView); G(vkDestroyImageView); G(vkCmdCopyImage); G(vkFreeCommandBuffers);
    if (props.apiVersion >= VK_API_VERSION_1_3) G(vkCmdCopyBufferToImage2);
    G(vkCreateShaderModule); G(vkDestroyShaderModule); G(vkCreateDescriptorSetLayout); G(vkCreatePipelineLayout);
    G(vkCreateComputePipelines); G(vkCreateDescriptorPool); G(vkAllocateDescriptorSets); G(vkUpdateDescriptorSets);
    G(vkCmdBindPipeline); G(vkCmdBindDescriptorSets); G(vkCmdPushConstants); G(vkCmdDispatch);
#undef G
    vkGetDeviceQueue(dev, 0, 0, &queue);
    if (tx && props.apiVersion >= VK_API_VERSION_1_3) {
        /* the bridge transcodes BC6H where the GPU has ASTC HDR and samples, filters and copies
         * ASTC 4x4 HDR images (server_emu_bcn.c); the checks then blit from them */
        PFN_vkGetPhysicalDeviceFeatures2 gpdf2 = (PFN_vkGetPhysicalDeviceFeatures2)gipa(inst, "vkGetPhysicalDeviceFeatures2");
        VkPhysicalDeviceTextureCompressionASTCHDRFeatures hf = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TEXTURE_COMPRESSION_ASTC_HDR_FEATURES};
        VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &hf};
        if (gpdf2) gpdf2(pd, &f2);
        VkFormatProperties hp;
        vkGetPhysicalDeviceFormatProperties(pd, VK_FORMAT_ASTC_4x4_SFLOAT_BLOCK, &hp);
        const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
                                          VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
        tx_hdr = hf.textureCompressionASTC_HDR && (hp.optimalTilingFeatures & need) == need;
        if (tx_hdr && !(hp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT))
            printf("INFO no blit from ASTC 4x4 HDR: the BC6H checks through the bridge will fail\n");
    }
    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, 0, 0};
    vkCreateCommandPool(dev, &pci, NULL, &pool);
    if (hdr_only) { /* `bcn_check hdr`: only the BC6H -> ASTC HDR transcoder, run directly (any GPU) */
        test_hdr_shader();
        printf("%s\n", failures ? "FAILED" : "OK");
        return failures ? 1 : 0;
    }
    float u8 = 2.5f / 255.0f; /* hardware decoders round differently from bcdec by up to 2 */
    if (tx) {
        static const int photo[] = {IMG_PHOTO, IMG_ROUGH, IMG_GRADIENT}, cut[] = {IMG_PHOTO, IMG_ROUGH, IMG_GRADIENT, IMG_ALPHA_CUT};
        static const int soft[] = {IMG_PHOTO, IMG_ROUGH, IMG_GRADIENT, IMG_ALPHA_SOFT}, normal[] = {IMG_NORMAL};
        test_quality(VK_FORMAT_BC1_RGB_UNORM_BLOCK, "BC1 RGB -> ETC2 RGB8", photo, 3);
        test_quality(VK_FORMAT_BC1_RGBA_UNORM_BLOCK, "BC1 RGBA -> ETC2 RGB8A1", cut, 4);
        test_quality(VK_FORMAT_BC1_RGBA_SRGB_BLOCK, "BC1 RGBA sRGB -> ETC2 RGB8A1 sRGB", cut, 4);
        test_quality(VK_FORMAT_BC2_UNORM_BLOCK, "BC2 -> ASTC 4x4", soft, 4);
        test_quality(VK_FORMAT_BC3_UNORM_BLOCK, "BC3 -> ASTC 4x4", soft, 4);
        test_quality(VK_FORMAT_BC3_SRGB_BLOCK, "BC3 sRGB -> ASTC 4x4 sRGB", soft, 4);
        test_quality(VK_FORMAT_BC7_UNORM_BLOCK, "BC7 -> ASTC 4x4", soft, 4);
        test_quality(VK_FORMAT_BC4_UNORM_BLOCK, "BC4 -> EAC R11", photo, 3);
        test_quality(VK_FORMAT_BC4_SNORM_BLOCK, "BC4 snorm -> EAC R11 snorm", photo, 3);
        test_quality(VK_FORMAT_BC5_UNORM_BLOCK, "BC5 -> EAC RG11", normal, 1);
        test_quality(VK_FORMAT_BC5_SNORM_BLOCK, "BC5 snorm -> EAC RG11 snorm", normal, 1);
        /* BC6H -> ASTC 4x4 HDR: through the bridge where the GPU samples ASTC HDR, else decoded as
         * before; the transcoder itself either way, its blocks decoded here */
        if (tx_hdr) {
            static const int hdr[] = {IMG_HDR_PHOTO, IMG_HDR_GRADIENT, IMG_HDR_SKY};
            test_quality(VK_FORMAT_BC6H_UFLOAT_BLOCK, "BC6H ufloat -> ASTC 4x4 HDR", hdr, 3);
            test_quality(VK_FORMAT_BC6H_SFLOAT_BLOCK, "BC6H sfloat -> ASTC 4x4 HDR", hdr, 3);
        } else {
            printf("INFO the GPU does not sample ASTC 4x4 HDR: BC6H is decoded\n");
            test_format(VK_FORMAT_BC6H_UFLOAT_BLOCK, "BC6H ufloat (all 14 modes, decoded)", 0.002f);
            test_format(VK_FORMAT_BC6H_SFLOAT_BLOCK, "BC6H sfloat (all 14 modes, decoded)", 0.002f);
        }
        test_hdr_shader();
    } else {
        test_format(VK_FORMAT_BC1_RGBA_UNORM_BLOCK, "BC1 RGBA", u8);
        test_format(VK_FORMAT_BC1_RGB_UNORM_BLOCK, "BC1 RGB", u8);
        test_format(VK_FORMAT_BC2_UNORM_BLOCK, "BC2", u8);
        test_format(VK_FORMAT_BC3_UNORM_BLOCK, "BC3", u8);
        test_format(VK_FORMAT_BC4_UNORM_BLOCK, "BC4 unorm", u8);
        test_format(VK_FORMAT_BC4_SNORM_BLOCK, "BC4 snorm", 2.5f / 127.0f);
        test_format(VK_FORMAT_BC5_UNORM_BLOCK, "BC5 unorm", u8);
        test_format(VK_FORMAT_BC5_SNORM_BLOCK, "BC5 snorm", 2.5f / 127.0f);
        test_format(VK_FORMAT_BC6H_UFLOAT_BLOCK, "BC6H ufloat (all 14 modes)", 0.002f);
        test_format(VK_FORMAT_BC6H_SFLOAT_BLOCK, "BC6H sfloat (all 14 modes)", 0.002f);
        test_format(VK_FORMAT_BC7_UNORM_BLOCK, "BC7 (all 8 modes)", u8);
    }
    static const VkFormat bc1_list[] = {VK_FORMAT_BC1_RGBA_UNORM_BLOCK, VK_FORMAT_BC1_RGBA_SRGB_BLOCK};
    test_shapes(VK_FORMAT_BC1_RGBA_UNORM_BLOCK, "BC1 shapes, mutable with a format list, sRGB view", u8, VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT,
                bc1_list, 2, VK_FORMAT_BC1_RGBA_SRGB_BLOCK);
    test_shapes(VK_FORMAT_BC1_RGBA_SRGB_BLOCK, "BC1 sRGB shapes", u8, 0, NULL, 0, VK_FORMAT_BC1_RGBA_SRGB_BLOCK);
    test_shapes(VK_FORMAT_BC1_RGBA_UNORM_BLOCK, "BC1 RGBA shapes", u8, 0, NULL, 0, VK_FORMAT_BC1_RGBA_UNORM_BLOCK);
    static const VkFormat bc1_rgb_list[] = {VK_FORMAT_BC1_RGB_UNORM_BLOCK};
    test_shapes(VK_FORMAT_BC1_RGB_UNORM_BLOCK, "BC1 RGB shapes, mutable with only its own format", u8, VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT,
                bc1_rgb_list, 1, VK_FORMAT_BC1_RGB_UNORM_BLOCK);
    test_shapes(VK_FORMAT_BC7_SRGB_BLOCK, "BC7 sRGB shapes, mutable without a list, UNORM view", u8, VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT, NULL, 0,
                VK_FORMAT_BC7_UNORM_BLOCK);
    test_shapes(VK_FORMAT_BC4_UNORM_BLOCK, "BC4 shapes", u8, 0, NULL, 0, 0);
    test_shapes(VK_FORMAT_BC5_SNORM_BLOCK, "BC5 snorm shapes", 2.5f / 127.0f, 0, NULL, 0, 0);
    test_shapes(VK_FORMAT_BC6H_UFLOAT_BLOCK, "BC6H shapes", 0.002f, 0, NULL, 0, 0);
    static const VkFormat bc5_list[] = {VK_FORMAT_BC5_UNORM_BLOCK, VK_FORMAT_BC5_SNORM_BLOCK};
    test_shapes(VK_FORMAT_BC5_UNORM_BLOCK, "BC5 unorm shapes, mutable with a format list, SNORM view", u8, VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT,
                bc5_list, 2, VK_FORMAT_BC5_SNORM_BLOCK);
    test_size(VK_FORMAT_BC5_UNORM_BLOCK, "BC5 unorm size (RG8)", 2, 0, NULL, 0);
    test_size(VK_FORMAT_BC5_UNORM_BLOCK, "BC5 unorm mutable size (RG8)", 2, VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT, bc5_list, 2);
    test_size(VK_FORMAT_BC5_SNORM_BLOCK, "BC5 snorm size (RG8)", 2, 0, NULL, 0);
    test_size(VK_FORMAT_BC4_UNORM_BLOCK, "BC4 unorm size (R8)", 1, 0, NULL, 0);
    test_size(VK_FORMAT_BC1_RGBA_UNORM_BLOCK, "BC1 RGBA size (A1R5G5B5)", 2, 0, NULL, 0);
    test_large(VK_FORMAT_BC7_UNORM_BLOCK, "BC7");
    if (getenv("BCN_BENCH")) {
        test_large(VK_FORMAT_BC1_RGBA_UNORM_BLOCK, "BC1");
        test_large(VK_FORMAT_BC3_UNORM_BLOCK, "BC3");
        test_large(VK_FORMAT_BC4_UNORM_BLOCK, "BC4");
        test_large(VK_FORMAT_BC5_UNORM_BLOCK, "BC5");
    }
    test_many_cbs();
    printf("%s\n", failures ? "FAILED" : "OK");
    return failures ? 1 : 0;
}
