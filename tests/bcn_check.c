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
 * compared with bcdec by PSNR. The quality table per format and image comes first.
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

#define TEX 64 /* 16x16 blocks */

static int tx; /* the bridge transcodes (argument "transcode"): compare by PSNR */
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
F(vkFreeCommandBuffers); F(vkCmdCopyBufferToImage2);
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
enum { IMG_PHOTO, IMG_ROUGH, IMG_GRADIENT, IMG_ALPHA_SOFT, IMG_ALPHA_CUT, IMG_NORMAL, IMG_COUNT };
static const char *const img_name[IMG_COUNT] = {"photo", "rough", "gradient", "soft alpha", "cut-out alpha", "normal map"};

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

static int is_snorm(VkFormat f) { return f == VK_FORMAT_BC4_SNORM_BLOCK || f == VK_FORMAT_BC5_SNORM_BLOCK; }

/* One block of format f from texels px (8-bit RGBA; a normal map's x, y for BC5). */
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
                uint8_t px[16][4];
                for (int t = 0; t < 16; t++) image_px(kind, bx * 4 + (t & 3), by * 4 + (t >> 2), px[t]);
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

/* Transcoded by the bridge (in transcode mode): all but BC6H. */
static int transcoded(VkFormat f) { return tx && f != VK_FORMAT_BC6H_UFLOAT_BLOCK && f != VK_FORMAT_BC6H_SFLOAT_BLOCK; }

/* Lowest PSNR (dB) a transcoded level may have against bcdec, colour and alpha, a little under the
 * worst measured on the test images (2026-10-13, RADV decoding ETC2/ASTC): BC1 -> ETC2 31.3 (rough),
 * BC7 -> ASTC 33.9 (rough), BC4/BC5 -> EAC 43.8, alpha 40.4. */
static double psnr_floor(VkFormat f, int alpha)
{
    if (alpha) return 36;
    switch (f) {
    case VK_FORMAT_BC1_RGB_UNORM_BLOCK: case VK_FORMAT_BC1_RGB_SRGB_BLOCK: case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
        return 29;
    case VK_FORMAT_BC4_UNORM_BLOCK: case VK_FORMAT_BC4_SNORM_BLOCK: case VK_FORMAT_BC5_UNORM_BLOCK: case VK_FORMAT_BC5_SNORM_BLOCK: return 40;
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
                              VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
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
        int alpha = nc == 3 && f != VK_FORMAT_BC1_RGB_UNORM_BLOCK && f != VK_FORMAT_BC1_RGB_SRGB_BLOCK;
        double se = 0, sea = 0;
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
                        if (ch < nc) se += (double)(g - w) * (g - w);
                        else if (ch == 3 && alpha) sea += (double)(g - w) * (g - w);
                    }
                    n++;
                }
            }
        double peak = is_snorm(f) ? 2.0 : 1.0;
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
    int gpu = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "transcode")) tx = 1;
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
#undef G
    vkGetDeviceQueue(dev, 0, 0, &queue);
    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, 0, 0};
    vkCreateCommandPool(dev, &pci, NULL, &pool);
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
        test_format(VK_FORMAT_BC6H_UFLOAT_BLOCK, "BC6H ufloat (all 14 modes, decoded)", 0.002f);
        test_format(VK_FORMAT_BC6H_SFLOAT_BLOCK, "BC6H sfloat (all 14 modes, decoded)", 0.002f);
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
