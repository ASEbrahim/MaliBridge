/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * bcn_check: uploads random blocks of every BC format (every BC6H and BC7 mode forced in turn),
 * blits the texture to RGBA32F and compares each texel with bcdec's CPU decode. Then the shapes
 * uploads take: mip chains with partial blocks, array layers in one region, a sub-region upload,
 * vkCmdCopyBufferToImage2, mutable images with sRGB/UNORM views, a BC -> BC image copy, and a
 * 4096x4096 BC7 array larger than the bridge's per-command-buffer decode scratch. Run it natively
 * (the GPU's own BC decoder) and through the bridge with BC emulated.
 */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BCDEC_IMPLEMENTATION
#define BCDEC_BC4BC5_PRECISE
#include "../third_party/bcdec/bcdec.h"

#define TEX 64 /* 16x16 blocks */

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
    case VK_FORMAT_BC2_UNORM_BLOCK: bcdec_bc2(b, px8, 16); rgba8 = 1; break;
    case VK_FORMAT_BC3_UNORM_BLOCK: bcdec_bc3(b, px8, 16); rgba8 = 1; break;
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

/* ------------------------------------------------------------------ upload shapes */

static int is_srgb(VkFormat f) { return f == VK_FORMAT_BC1_RGBA_SRGB_BLOCK || f == VK_FORMAT_BC7_SRGB_BLOCK; }

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

static void random_level(VkFormat f, level *lv, uint32_t w, uint32_t h)
{
    int bytes = block_bytes(f);
    lv->w = w;
    lv->h = h;
    lv->bw = (w + 3) / 4;
    lv->bh = (h + 3) / 4;
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
        printf("PASS %s: %d mips x %d layers, sub-region%s, BC -> BC copy\n", name, MIPS, LAYERS,
               vkCmdCopyBufferToImage2 ? " (CopyBufferToImage2)" : "");
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
    if (r.size > max) {
        printf("FAIL %s: %llu bytes for %ux%u, more than %u bytes a texel\n", name, (unsigned long long)r.size, W, H, texel_bytes);
        failures++;
    } else {
        printf("PASS %s: %llu bytes for %ux%u\n", name, (unsigned long long)r.size, W, H);
    }
    vkDestroyImage(dev, img, NULL);
    vkFreeMemory(dev, m, NULL);
}

/* 4096x4096 BC7, 2 layers, in one region: 128 MiB decoded, more than the bridge decodes into one
 * command buffer's scratch before reusing it. */
static void test_large(void)
{
    const VkFormat f = VK_FORMAT_BC7_UNORM_BLOCK;
    enum { S = 4096, LAYERS = 2 };
    level lv[LAYERS];
    VkDeviceSize per = (VkDeviceSize)(S / 4) * (S / 4) * 16;
    VkDeviceMemory um, im;
    void *upp;
    VkBuffer up = host_buffer(per * LAYERS, &um, &upp);
    VkImage img = make_image(f, S, S, 1, LAYERS, 0, NULL, 0, &im);
    if (!img) {
        printf("FAIL BC7 4096x4096x2: cannot create the image\n");
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
    submit(cb);
    int bad = 0;
    for (uint32_t l = 0; l < LAYERS; l++) bad += compare_level(img, f, 0, l, &lv[l], 2.5f / 255.0f, "BC7 4096x4096x2");
    if (bad) {
        printf("FAIL BC7 4096x4096x2: %d values differ from bcdec\n", bad);
        failures++;
    } else {
        printf("PASS BC7 4096x4096x2: %d texels match bcdec\n", S * S * LAYERS);
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
        printf("PASS %d command buffers in flight: %d texels match bcdec\n", NCB, NCB * (S * S * LAYERS + 60 * 44));
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
    int gpu = argc > 1 ? atoi(argv[1]) : 0;
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
    test_large();
    test_many_cbs();
    printf("%s\n", failures ? "FAILED" : "OK");
    return failures ? 1 : 0;
}
