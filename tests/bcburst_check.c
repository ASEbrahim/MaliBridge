/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * bcburst_check: a game streaming BC textures in, for the conversion budget (VKBRIDGE_BC_TX_BUDGET,
 * server/server_emu_bcn.c). host/bcbudget_test.sh runs it against a server of its own that
 * converts BC (VKBRIDGE_FAKE_MISSING=bc, transcoding to ETC2 / ASTC) with its GPU slowed to a
 * Mali's (VKBRIDGE_TEST_BC_SLOW) and the per-hitch line on (VKBRIDGE_HITCH_MS).
 *
 *   bcburst_check [--size N] [--burst T] [--stream P] [--frames F] [--query] [--hold MS]
 *
 * Frames are an acquire, a barrier and a present on a headless surface. First 3 warm frames, each
 * uploading one small BC7 and one BC1 texture (the converters get built), then a pause (--hold,
 * default 400 ms) like a level's start. Then the burst: one frame creating and uploading T
 * textures of NxN with every mip level (half BC7, half BC1 RGBA; default 32 of 1024x1024), then F
 * frames (default 30) uploading P each (default 2) - the streaming after a load. Each frame waits
 * for its uploads before the next one, as a game waiting on its frame does. --query: every texture's
 * memory requirements are asked first (vkGetDeviceImageMemoryRequirements, as vkd3d does), and the
 * image made must not need more.
 *
 * Then every texture's top level is blitted to RGBA8 and compared with bcdec's decode of its
 * blocks: PSNR at least the bcn_check floors (BC7 31 dB, BC1 29 dB). Textures that match exactly
 * (every channel within 1, or BC1's 5-bit decode) were decoded, the rest transcoded.
 *
 * Prints "frame <n> <what> <ms>" (n: the present that ends it, counted from 1, as the hitch
 * line's frame=), "RESULT key=value ..." and "OK: ..." / "FAILED: ..." last.
 */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define BCDEC_IMPLEMENTATION
#include "../third_party/bcdec/bcdec.h"

static PFN_vkGetInstanceProcAddr gipa;
static VkInstance inst;
static VkPhysicalDevice pd;
static VkDevice dev;
static VkQueue queue;
static VkPhysicalDeviceMemoryProperties mp;

#define I(name) static PFN_##name name
I(vkCreateInstance); I(vkEnumeratePhysicalDevices); I(vkGetPhysicalDeviceQueueFamilyProperties); I(vkCreateDevice);
I(vkGetDeviceQueue); I(vkCreateCommandPool); I(vkAllocateCommandBuffers); I(vkBeginCommandBuffer); I(vkEndCommandBuffer);
I(vkCmdPipelineBarrier); I(vkQueueSubmit); I(vkDeviceWaitIdle); I(vkGetDeviceProcAddr);
I(vkCreateHeadlessSurfaceEXT); I(vkGetPhysicalDeviceSurfaceCapabilitiesKHR); I(vkGetPhysicalDeviceSurfaceSupportKHR);
I(vkCreateSwapchainKHR); I(vkGetSwapchainImagesKHR); I(vkAcquireNextImageKHR); I(vkQueuePresentKHR); I(vkCreateFence);
I(vkWaitForFences); I(vkResetFences); I(vkGetPhysicalDeviceMemoryProperties); I(vkAllocateMemory); I(vkFreeMemory);
I(vkCreateImage); I(vkGetImageMemoryRequirements); I(vkBindImageMemory); I(vkCreateBuffer); I(vkGetBufferMemoryRequirements);
I(vkBindBufferMemory); I(vkMapMemory); I(vkCmdCopyBufferToImage); I(vkResetCommandBuffer); I(vkDestroyImage); I(vkDestroyBuffer);
I(vkCmdBlitImage); I(vkCmdCopyImageToBuffer); I(vkGetDeviceImageMemoryRequirements);
#undef I

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want)
{
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

/* ------------------------------------------------------------------ content */

/* A level of texture `seed`, w x w RGBA8: soft gradients and a few hard edges, with a little noise. */
static void level_texels(uint32_t seed, uint32_t w, uint8_t *out)
{
    double sd = seed * 0.37, *t = malloc(sizeof(double) * w * 5);
    double *A = t, *C = t + w, *D = t + 2 * w, *B = t + 3 * w, *E = t + 4 * w;
    for (uint32_t i = 0; i < w; i++) {
        double u = (double)i / w;
        A[i] = sin(6.0 * u + sd), C[i] = sin(4.0 * u), D[i] = cos(4.0 * u), B[i] = cos(5.0 * u + 2 * sd), E[i] = 4.0 * u + 3 * sd;
    }
    for (uint32_t y = 0; y < w; y++) {
        double ce = cos(E[y]), se = sin(E[y]);
        for (uint32_t x = 0; x < w; x++) {
            double r = 0.5 + 0.45 * A[x], g = 0.5 + 0.45 * B[y], b = 0.5 + 0.4 * (C[x] * ce + D[x] * se);
            if (((x / 64) + (y / 64) + seed) % 7 == 0) r = 1 - r, b = 1 - b; /* hard-edged patches */
            uint32_t h = (x * 73856093u) ^ (y * 19349663u) ^ (seed * 83492791u);
            double n = ((h >> 8) & 15) / 255.0;
            uint8_t *o = out + ((size_t)y * w + x) * 4;
            o[0] = (uint8_t)fmin(255, (r + n) * 255), o[1] = (uint8_t)fmin(255, (g + n) * 255), o[2] = (uint8_t)fmin(255, (b + n) * 255);
            o[3] = 255;
        }
    }
    free(t);
}

/* BC1 (4-colour): endpoints at the block's per-channel extremes in RGB565, indices by projection. */
static void enc_bc1(const uint8_t t[16][4], uint8_t out[8])
{
    int lo[3] = {255, 255, 255}, hi[3] = {0, 0, 0};
    for (int i = 0; i < 16; i++)
        for (int c = 0; c < 3; c++) lo[c] = t[i][c] < lo[c] ? t[i][c] : lo[c], hi[c] = t[i][c] > hi[c] ? t[i][c] : hi[c];
    uint16_t c0 = (uint16_t)(((hi[0] >> 3) << 11) | ((hi[1] >> 2) << 5) | (hi[2] >> 3));
    uint16_t c1 = (uint16_t)(((lo[0] >> 3) << 11) | ((lo[1] >> 2) << 5) | (lo[2] >> 3));
    uint32_t idx = 0;
    if (c0 == c1) {
        if (c0 == 0xffff) c1--; else c0++;
    }
    if (c0 < c1) { uint16_t k = c0; c0 = c1, c1 = k; }
    double e0[3] = {(c0 >> 11) * 255.0 / 31, ((c0 >> 5) & 63) * 255.0 / 63, (c0 & 31) * 255.0 / 31};
    double e1[3] = {(c1 >> 11) * 255.0 / 31, ((c1 >> 5) & 63) * 255.0 / 63, (c1 & 31) * 255.0 / 31};
    double d[3] = {e1[0] - e0[0], e1[1] - e0[1], e1[2] - e0[2]}, dd = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
    static const int to_idx[4] = {0, 2, 3, 1}; /* along c0 -> c1: c0, 2/3 c0 + 1/3 c1, 1/3 c0 + 2/3 c1, c1 */
    for (int i = 0; i < 16; i++) {
        double p = dd > 0 ? ((t[i][0] - e0[0]) * d[0] + (t[i][1] - e0[1]) * d[1] + (t[i][2] - e0[2]) * d[2]) / dd : 0;
        int k = (int)lround(fmin(3, fmax(0, p * 3)));
        idx |= (uint32_t)to_idx[k] << (2 * i);
    }
    out[0] = (uint8_t)c0, out[1] = (uint8_t)(c0 >> 8), out[2] = (uint8_t)c1, out[3] = (uint8_t)(c1 >> 8);
    memcpy(out + 4, &idx, 4);
}

/* BC7 mode 6 (one subset, RGBA 7.7.7.7 + p-bit endpoints, 4-bit indices). */
static void enc_bc7(const uint8_t t[16][4], uint8_t out[16])
{
    int lo[4] = {255, 255, 255, 255}, hi[4] = {0, 0, 0, 0};
    for (int i = 0; i < 16; i++)
        for (int c = 0; c < 4; c++) lo[c] = t[i][c] < lo[c] ? t[i][c] : lo[c], hi[c] = t[i][c] > hi[c] ? t[i][c] : hi[c];
    int e[2][4];
    for (int c = 0; c < 4; c++) e[0][c] = lo[c] >> 1, e[1][c] = hi[c] >> 1;
    double a[4], d[4], dd = 0;
    for (int c = 0; c < 4; c++) a[c] = e[0][c] << 1, d[c] = (e[1][c] << 1) - a[c], dd += d[c] * d[c];
    int idx[16];
    for (int i = 0; i < 16; i++) {
        double p = 0;
        for (int c = 0; c < 4; c++) p += (t[i][c] - a[c]) * d[c];
        idx[i] = dd > 0 ? (int)lround(fmin(15, fmax(0, p / dd * 15))) : 0;
    }
    if (idx[0] >= 8) { /* the anchor's top bit is implied 0: swap the endpoints */
        for (int c = 0; c < 4; c++) { int k = e[0][c]; e[0][c] = e[1][c], e[1][c] = k; }
        for (int i = 0; i < 16; i++) idx[i] = 15 - idx[i];
    }
    memset(out, 0, 16);
    int bit = 0;
#define PUT(v, n) do { for (int _b = 0; _b < (n); _b++, bit++) if (((v) >> _b) & 1) out[bit >> 3] |= (uint8_t)(1u << (bit & 7)); } while (0)
    PUT(1 << 6, 7);
    for (int c = 0; c < 4; c++) {
        PUT(e[0][c], 7);
        PUT(e[1][c], 7);
    }
    PUT(0, 2); /* p-bits */
    PUT(idx[0], 3);
    for (int i = 1; i < 16; i++) PUT(idx[i], 4);
#undef PUT
}

typedef struct tex {
    VkImage img;
    VkFormat fmt;
    uint32_t seed;
    const uint8_t *top;    /* its blocks (encode_texture), the top level first, for the check */
    VkDeviceSize queried;  /* --query: the size vkGetDeviceImageMemoryRequirements gave */
} tex;

static uint32_t size = 1024, mips;
static int query;
static VkCommandPool pool;
static VkBuffer stage;
static void *stage_map;
static VkDeviceSize stage_cap, stage_used;
static tex *texs;
static int ntex, cap_tex, query_bad;

static VkDeviceSize level_bytes(VkFormat f, uint32_t w, uint32_t h)
{
    return (VkDeviceSize)((w + 3) / 4) * ((h + 3) / 4) * (f == VK_FORMAT_BC7_UNORM_BLOCK ? 16 : 8);
}

/* Every level of texture `seed` in format f (n x n), each level 16-byte aligned: what a game reads
 * from its files, made before the frames that upload it. */
static uint8_t *encode_texture(VkFormat f, uint32_t n, uint32_t seed)
{
    uint32_t nm = 0, bb = f == VK_FORMAT_BC7_UNORM_BLOCK ? 16 : 8;
    for (uint32_t s = n; s; s >>= 1) nm++;
    VkDeviceSize total = 0;
    for (uint32_t m = 0; m < nm; m++) total += (level_bytes(f, n >> m ? n >> m : 1, n >> m ? n >> m : 1) + 15) & ~(VkDeviceSize)15;
    uint8_t *blob = malloc(total), *rgba = malloc((size_t)(n < 4 ? 4 : n) * (n < 4 ? 4 : n) * 4), blk[16][4];
    VkDeviceSize off = 0;
    for (uint32_t m = 0; m < nm; m++) {
        uint32_t w = n >> m ? n >> m : 1, bw = (w + 3) / 4;
        level_texels(seed + m * 101, w, rgba);
        for (uint32_t by = 0; by < bw; by++)
            for (uint32_t bx = 0; bx < bw; bx++) {
                for (uint32_t i = 0; i < 16; i++) {
                    uint32_t x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
                    memcpy(blk[i], rgba + ((size_t)(y < w ? y : w - 1) * w + (x < w ? x : w - 1)) * 4, 4);
                }
                if (bb == 16) enc_bc7((const uint8_t(*)[4])blk, blob + off + ((size_t)by * bw + bx) * 16);
                else enc_bc1((const uint8_t(*)[4])blk, blob + off + ((size_t)by * bw + bx) * 8);
            }
        off += (level_bytes(f, w, w) + 15) & ~(VkDeviceSize)15;
    }
    free(rgba);
    return blob;
}

/* Creates texture `seed` of format f, copies its blocks (encode_texture) to the staging buffer and
 * records its upload. */
static int add_texture(VkCommandBuffer cb, VkFormat f, uint32_t n, uint32_t seed, const uint8_t *blob)
{
    uint32_t nm = 0;
    for (uint32_t s = n; s; s >>= 1) nm++;
    VkImageCreateInfo ici = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, f, {n, n, 1}, nm, 1, VK_SAMPLE_COUNT_1_BIT,
                             VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                             VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
    VkDeviceSize queried = 0;
    if (query) {
        VkDeviceImageMemoryRequirements dq = {VK_STRUCTURE_TYPE_DEVICE_IMAGE_MEMORY_REQUIREMENTS, NULL, &ici, 0};
        VkMemoryRequirements2 r2 = {VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
        vkGetDeviceImageMemoryRequirements(dev, &dq, &r2);
        queried = r2.memoryRequirements.size;
    }
    VkImage img;
    if (vkCreateImage(dev, &ici, NULL, &img)) return 0;
    VkMemoryRequirements r;
    vkGetImageMemoryRequirements(dev, img, &r);
    if (query && r.size > queried) query_bad++;
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, query && queried > r.size ? queried : r.size,
                               mem_type(r.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    VkDeviceMemory im;
    if (ai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(dev, &ai, NULL, &im) || vkBindImageMemory(dev, img, im, 0)) return 0;
    if (ntex == cap_tex) {
        cap_tex = cap_tex ? cap_tex * 2 : 64;
        texs = realloc(texs, cap_tex * sizeof(*texs));
    }
    tex *t = &texs[ntex++];
    *t = (tex){img, f, seed, blob, queried};
    VkBufferImageCopy regs[16];
    VkDeviceSize off = 0;
    for (uint32_t m = 0; m < nm; m++) {
        uint32_t w = n >> m ? n >> m : 1;
        VkDeviceSize bytes = level_bytes(f, w, w);
        stage_used = (stage_used + 15) & ~(VkDeviceSize)15;
        if (stage_used + bytes > stage_cap) {
            printf("FAILED: staging buffer too small\n");
            return 0;
        }
        memcpy((uint8_t *)stage_map + stage_used, blob + off, bytes);
        off += (bytes + 15) & ~(VkDeviceSize)15;
        regs[m] = (VkBufferImageCopy){stage_used, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, m, 0, 1}, {0, 0, 0}, {w, w, 1}};
        stage_used += bytes;
    }
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, img,
                              {VK_IMAGE_ASPECT_COLOR_BIT, 0, nm, 0, 1}};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    vkCmdCopyBufferToImage(cb, stage, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, nm, regs);
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL,
                         0, NULL, 1, &b);
    return 1;
}

static VkBuffer host_buffer(VkDeviceSize bytes, VkBufferUsageFlags usage, void **map)
{
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, bytes, usage, VK_SHARING_MODE_EXCLUSIVE};
    VkBuffer buf;
    if (vkCreateBuffer(dev, &bci, NULL, &buf)) return VK_NULL_HANDLE;
    VkMemoryRequirements r;
    vkGetBufferMemoryRequirements(dev, buf, &r);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, r.size,
                               mem_type(r.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    VkDeviceMemory m;
    if (ai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(dev, &ai, NULL, &m) || vkBindBufferMemory(dev, buf, m, 0) ||
        vkMapMemory(dev, m, 0, bytes, 0, map))
        return VK_NULL_HANDLE;
    return buf;
}

/* Texture t's top level against bcdec: its PSNR (colour), *exact: every channel as decoded. */
static double check_texture(VkCommandBuffer cb, VkFence fence, VkBuffer down, const void *map, const tex *t, int *exact)
{
    VkImageCreateInfo ici = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {size, size, 1}, 1, 1,
                             VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                             VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
    VkImage rgba;
    if (vkCreateImage(dev, &ici, NULL, &rgba)) return -1;
    VkMemoryRequirements r;
    vkGetImageMemoryRequirements(dev, rgba, &r);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, r.size, mem_type(r.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    VkDeviceMemory m;
    if (vkAllocateMemory(dev, &ai, NULL, &m) || vkBindImageMemory(dev, rgba, m, 0)) return -1;
    vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(cb, &bi);
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, rgba,
                              {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    VkImageBlit bl = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{0, 0, 0}, {(int32_t)size, (int32_t)size, 1}},
                      {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{0, 0, 0}, {(int32_t)size, (int32_t)size, 1}}};
    vkCmdBlitImage(cb, t->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rgba, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_NEAREST);
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    VkBufferImageCopy rc = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {size, size, 1}};
    vkCmdCopyImageToBuffer(cb, rgba, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, down, 1, &rc);
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    vkEndCommandBuffer(cb);
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cb, 0, NULL};
    vkQueueSubmit(queue, 1, &si, fence);
    vkWaitForFences(dev, 1, &fence, VK_TRUE, 30000000000ull);
    vkResetFences(dev, 1, &fence);
    int bc7 = t->fmt == VK_FORMAT_BC7_UNORM_BLOCK, bb = bc7 ? 16 : 8;
    uint32_t bw = (size + 3) / 4;
    double se = 0;
    long n = 0;
    int maxd = 0;
    for (uint32_t by = 0; by < bw; by++)
        for (uint32_t bx = 0; bx < bw; bx++) {
            uint8_t want[16 * 4];
            const uint8_t *src = t->top + ((size_t)by * bw + bx) * bb;
            if (bc7) bcdec_bc7(src, want, 16);
            else bcdec_bc1(src, want, 16);
            for (int i = 0; i < 16; i++) {
                const uint8_t *g = (const uint8_t *)map + (((size_t)(by * 4 + (i >> 2)) * size) + bx * 4 + (i & 3)) * 4;
                for (int c = 0; c < 3; c++) {
                    int d = (int)g[c] - (int)want[i * 4 + c];
                    se += (double)d * d;
                    if (abs(d) > maxd) maxd = abs(d);
                }
                n++;
            }
        }
    vkDestroyImage(dev, rgba, NULL);
    vkFreeMemory(dev, m, NULL);
    /* decoded: exact (BC1 UNORM through R5G6B5 / A1R5G5B5: within that format's rounding) */
    *exact = maxd <= (bc7 ? 1 : 5);
    return se > 0 ? 10 * log10(255.0 * 255.0 * 3 * n / se) : 99;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    int burst = 32, stream = 2, frames = 30, hold = 400;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--size") && i + 1 < argc) size = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--burst") && i + 1 < argc) burst = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--stream") && i + 1 < argc) stream = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--hold") && i + 1 < argc) hold = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--query")) query = 1;
    }
    for (uint32_t s = size; s; s >>= 1) mips++;
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib) {
        printf("FAILED: loader: %s\n", dlerror());
        return 2;
    }
    gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
#define G(n) n = (PFN_##n)gipa(inst, #n)
    G(vkCreateInstance);
    const char *iexts[2] = {"VK_KHR_surface", "VK_EXT_headless_surface"};
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "vkbridge-bcburst-check", 1, "vkbridge", 1, VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 2, iexts};
    if (vkCreateInstance(&ici, NULL, &inst)) {
        printf("FAILED: setup: vkCreateInstance\n");
        return 2;
    }
    G(vkEnumeratePhysicalDevices); G(vkGetPhysicalDeviceQueueFamilyProperties); G(vkCreateDevice); G(vkGetDeviceQueue);
    G(vkCreateCommandPool); G(vkAllocateCommandBuffers); G(vkBeginCommandBuffer); G(vkEndCommandBuffer); G(vkCmdPipelineBarrier);
    G(vkQueueSubmit); G(vkDeviceWaitIdle); G(vkGetDeviceProcAddr);
    G(vkCreateHeadlessSurfaceEXT); G(vkGetPhysicalDeviceSurfaceCapabilitiesKHR); G(vkGetPhysicalDeviceSurfaceSupportKHR);
    G(vkCreateSwapchainKHR); G(vkGetSwapchainImagesKHR); G(vkAcquireNextImageKHR); G(vkQueuePresentKHR); G(vkCreateFence);
    G(vkWaitForFences); G(vkResetFences); G(vkGetPhysicalDeviceMemoryProperties); G(vkAllocateMemory); G(vkFreeMemory);
    G(vkCreateImage); G(vkGetImageMemoryRequirements); G(vkBindImageMemory); G(vkCreateBuffer); G(vkGetBufferMemoryRequirements);
    G(vkBindBufferMemory); G(vkMapMemory); G(vkCmdCopyBufferToImage); G(vkResetCommandBuffer); G(vkDestroyImage); G(vkDestroyBuffer);
    G(vkCmdBlitImage); G(vkCmdCopyImageToBuffer); G(vkGetDeviceImageMemoryRequirements);
#undef G
    uint32_t n = 1;
    vkEnumeratePhysicalDevices(inst, &n, &pd);
    if (!n) {
        printf("FAILED: setup: no GPU\n");
        return 2;
    }
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    VkSurfaceKHR surface;
    VkHeadlessSurfaceCreateInfoEXT hci = {VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT};
    if (!vkCreateHeadlessSurfaceEXT || vkCreateHeadlessSurfaceEXT(inst, &hci, NULL, &surface)) {
        printf("FAILED: setup: vkCreateHeadlessSurfaceEXT\n");
        return 2;
    }
    uint32_t nq = 16;
    VkQueueFamilyProperties qp[16];
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qp);
    uint32_t family = UINT32_MAX;
    for (uint32_t i = 0; i < nq && family == UINT32_MAX; i++) {
        VkBool32 ok = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(pd, i, surface, &ok);
        if ((qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && ok) family = i;
    }
    if (family == UINT32_MAX) {
        printf("FAILED: setup: no queue can present\n");
        return 2;
    }
    float prio = 1;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, family, 1, &prio};
    VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f2.features.textureCompressionBC = VK_TRUE; /* emulated by the server where the GPU lacks it */
    const char *dexts[1] = {"VK_KHR_swapchain"};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f2, 0, 1, &qci, 0, NULL, 1, dexts, NULL};
    if (vkCreateDevice(pd, &dci, NULL, &dev)) {
        printf("FAILED: setup: vkCreateDevice\n");
        return 2;
    }
    vkGetDeviceQueue(dev, family, 0, &queue);
    if (query) vkGetDeviceImageMemoryRequirements = (PFN_vkGetDeviceImageMemoryRequirements)vkGetDeviceProcAddr(dev, "vkGetDeviceImageMemoryRequirements");
    if (query && !vkGetDeviceImageMemoryRequirements) {
        printf("FAILED: setup: no vkGetDeviceImageMemoryRequirements\n");
        return 2;
    }
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surface, &caps);
    uint32_t want = caps.minImageCount > 3 ? caps.minImageCount : 3;
    if (caps.maxImageCount && want > caps.maxImageCount) want = caps.maxImageCount;
    VkExtent2D ext = caps.currentExtent.width == 0xFFFFFFFFu ? (VkExtent2D){64, 64} : caps.currentExtent;
    VkSwapchainCreateInfoKHR sci = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, NULL, 0, surface, want, VK_FORMAT_B8G8R8A8_UNORM,
                                    VK_COLOR_SPACE_SRGB_NONLINEAR_KHR, ext, 1, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                    VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
                                    VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_PRESENT_MODE_FIFO_KHR, VK_TRUE, VK_NULL_HANDLE};
    VkSwapchainKHR sc;
    if (vkCreateSwapchainKHR(dev, &sci, NULL, &sc)) {
        printf("FAILED: setup: vkCreateSwapchainKHR\n");
        return 2;
    }
    uint32_t nimg = 8;
    VkImage imgs[8];
    vkGetSwapchainImagesKHR(dev, sc, &nimg, imgs);
    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, family};
    vkCreateCommandPool(dev, &pci, NULL, &pool);
    VkCommandBuffer cbs[9];
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, nimg + 1};
    vkAllocateCommandBuffers(dev, &cai, cbs);
    VkCommandBuffer work = cbs[nimg];
    for (uint32_t i = 0; i < nimg; i++) {
        VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        vkBeginCommandBuffer(cbs[i], &bi);
        VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, 0, 0, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                                  VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, imgs[i], {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        vkCmdPipelineBarrier(cbs[i], VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
        vkEndCommandBuffer(cbs[i]);
    }
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence acq_fence, work_fence;
    vkCreateFence(dev, &fci, NULL, &acq_fence);
    vkCreateFence(dev, &fci, NULL, &work_fence);
    /* staging for the largest frame: the burst's textures, every level */
    VkDeviceSize per = 0;
    for (uint32_t m = 0; m < mips; m++) per += level_bytes(VK_FORMAT_BC7_UNORM_BLOCK, size >> m ? size >> m : 1, size >> m ? size >> m : 1) + 16;
    stage_cap = per * (VkDeviceSize)(burst > stream ? burst : stream) + (1 << 20);
    stage = host_buffer(stage_cap, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &stage_map);
    if (!stage) {
        printf("FAILED: setup: staging buffer\n");
        return 2;
    }

    /* the textures' blocks, made before the frames (a game reads them from its files) */
    int planned = burst + frames * stream;
    uint8_t **blobs = malloc(sizeof(*blobs) * (planned + 6));
    for (int k = 0; k < planned + 6; k++) {
        int warmk = k < 6;
        int j = warmk ? k : k - 6;
        int fk = warmk ? (k & 1) : (j < burst ? j : (j - burst) % stream) & 1;
        blobs[k] = encode_texture(fk ? VK_FORMAT_BC1_RGBA_UNORM_BLOCK : VK_FORMAT_BC7_UNORM_BLOCK, warmk ? 64 : size, (uint32_t)k + 1);
    }
    /* 3 warm frames (small textures), a pause, the burst, then the streaming frames */
    int total = 3 + 1 + frames, presents = 0, burst_frame = 0;
    double t_frame = now_ms(), t_load0 = 0, burst_ms = 0, stream_max = 0, stream_sum = 0;
    uint32_t seed = 1;
    for (int f = 0; f < total; f++) {
        if (f == 3) {
            usleep((useconds_t)hold * 1000);
            t_frame = now_ms();
            t_load0 = t_frame;
        }
        uint32_t idx = 0;
        VkResult r = vkAcquireNextImageKHR(dev, sc, 5000000000ull, VK_NULL_HANDLE, acq_fence, &idx);
        if (r < 0) {
            printf("FAILED: acquire %d at frame %d\n", r, f);
            return 1;
        }
        vkWaitForFences(dev, 1, &acq_fence, VK_TRUE, 5000000000ull);
        vkResetFences(dev, 1, &acq_fence);
        int count = f < 3 ? 2 : f == 3 ? burst : stream;
        uint32_t tsize = f < 3 ? 64 : size;
        vkResetCommandBuffer(work, 0);
        VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        vkBeginCommandBuffer(work, &bi);
        stage_used = 0;
        int warm_from = ntex;
        for (int k = 0; k < count; k++, seed++)
            if (!add_texture(work, k & 1 ? VK_FORMAT_BC1_RGBA_UNORM_BLOCK : VK_FORMAT_BC7_UNORM_BLOCK, tsize, seed, blobs[seed - 1])) {
                printf("FAILED: texture %d of frame %d\n", k, f);
                return 1;
            }
        if (f < 3) ntex = warm_from; /* the warm textures are not checked (another size) */
        vkEndCommandBuffer(work);
        VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &work, 0, NULL};
        vkQueueSubmit(queue, 1, &si, work_fence);
        si.pCommandBuffers = &cbs[idx];
        vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
        VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, NULL, 0, NULL, 1, &sc, &idx, NULL};
        r = vkQueuePresentKHR(queue, &pi);
        presents++;
        if (r < 0) {
            printf("FAILED: present %d at frame %d\n", r, f);
            return 1;
        }
        /* the next frame waits for this one's uploads */
        vkWaitForFences(dev, 1, &work_fence, VK_TRUE, 60000000000ull);
        vkResetFences(dev, 1, &work_fence);
        usleep(4000);
        double t = now_ms(), ms = t - t_frame;
        t_frame = t;
        if (f >= 3) {
            /* the hitch line's frame= counts presents: the frame this one's uploads delay ends at the next present */
            printf("frame %d %s %.1f\n", presents + 1, f == 3 ? "burst" : "stream", ms);
            if (f == 3) burst_ms = ms, burst_frame = presents + 1;
            else stream_sum += ms, stream_max = ms > stream_max ? ms : stream_max;
        }
    }
    double load_ms = now_ms() - t_load0;
    /* one more present, so that the last frame's time reaches the server */
    uint32_t idx = 0;
    if (vkAcquireNextImageKHR(dev, sc, 5000000000ull, VK_NULL_HANDLE, acq_fence, &idx) >= 0) {
        vkWaitForFences(dev, 1, &acq_fence, VK_TRUE, 5000000000ull);
        VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cbs[idx], 0, NULL};
        vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
        VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, NULL, 0, NULL, 1, &sc, &idx, NULL};
        vkQueuePresentKHR(queue, &pi);
    }
    vkDeviceWaitIdle(dev);

    /* the textures' contents */
    void *down_map;
    VkBuffer down = host_buffer((VkDeviceSize)size * size * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &down_map);
    int bad = 0, exact = 0, exact7 = 0, n7 = 0;
    double min_psnr = 99;
    for (int i = 0; i < ntex; i++) {
        int ex = 0, bc7 = texs[i].fmt == VK_FORMAT_BC7_UNORM_BLOCK;
        double p = check_texture(work, work_fence, down, down_map, &texs[i], &ex);
        double floor = bc7 ? 31 : 29;
        if (p < floor) {
            bad++;
            if (bad <= 4) printf("FAIL texture %d (%s, seed %u): PSNR %.1f dB, floor %.0f\n", i, bc7 ? "BC7" : "BC1", texs[i].seed, p, floor);
        }
        if (p < min_psnr) min_psnr = p;
        exact += ex;
        n7 += bc7;
        exact7 += ex && bc7;
    }
    printf("RESULT burst_frame=%d burst_ms=%.1f stream_max_ms=%.1f stream_avg_ms=%.1f load_ms=%.1f textures=%d exact=%d exact_bc7=%d of %d "
           "min_psnr=%.1f below_floor=%d query_bigger=%d\n",
           burst_frame, burst_ms, stream_max, frames ? stream_sum / frames : 0, load_ms, ntex, exact, exact7, n7, min_psnr, bad, query_bad);
    if (bad || query_bad) {
        printf("FAILED: %d texture(s) below the PSNR floor, %d image(s) needing more memory than queried\n", bad, query_bad);
        return 1;
    }
    printf("OK: %d textures (%dx%d, every level) in a burst of %d and %d frames of %d; each at or over its PSNR floor\n", ntex, size, size,
           burst, frames, stream);
    return 0;
}
