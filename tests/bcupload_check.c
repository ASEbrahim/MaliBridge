/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * bcupload_check: many small BC textures streamed in as GTA V does through DXVK 3.1.1 (* ~1700 textures of ~25 KB in a second, mostly BC1), for the cost the BC conversion adds per
 * upload (server/server_emu_bcn.c, bc_batch). host/bcupload_test.sh runs it against a server of
 * its own that converts BC (VKBRIDGE_FAKE_MISSING=bc).
 *
 *   bcupload_check [--size N] [--count T] [--frames F] [--pattern dxvk3|pertex|mixed]
 *
 * Each frame makes T textures (default 400) of NxN or Nx(N/2) (default 128), every mip level,
 * two BC1 to one BC3, and uploads them:
 *   dxvk3   as DXVK 3.1.1's uploadImageHw: the layout transitions in a command buffer of their own
 *           submitted first, then one with only the copies, one vkCmdCopyBufferToImage per mip level
 *           (one region each), back to back, then the transitions out (a third command buffer);
 *   pertex  each texture between two barriers of its own in one command buffer, all its levels in one
 *           copy (tests/bcburst_check.c's way, older DXVK's);
 *   mixed   dxvk3 with an RGBA8 texture uploaded after every 5th BC one (a copy into an image that is
 *           not BC) and a vkCmdFillBuffer after every 7th (a command that is not a copy).
 * Frames wait for their uploads; steady_ms adds up the frames after the first (which builds the
 * converters). Then every level of every texture is blitted (nearest, same size)
 * into RGBA8 and read back: compared with bcdec's decode of its blocks (max channel difference: a
 * decoded BC1 UNORM goes through R5G6B5, so within 5; BC3 within 1) and hashed (FNV-1a over every
 * texel read back, RGBA8 textures included), so that two servers' results can be compared byte for byte.
 *
 * Prints "frame <n> <ms>", "RESULT key=value ..." and "OK: ..." / "FAILED: ..." last.
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
I(vkCmdPipelineBarrier); I(vkQueueSubmit); I(vkDeviceWaitIdle); I(vkCreateFence); I(vkWaitForFences); I(vkResetFences);
I(vkGetPhysicalDeviceMemoryProperties); I(vkAllocateMemory); I(vkCreateImage); I(vkGetImageMemoryRequirements);
I(vkBindImageMemory); I(vkCreateBuffer); I(vkGetBufferMemoryRequirements); I(vkBindBufferMemory); I(vkMapMemory);
I(vkCmdCopyBufferToImage); I(vkResetCommandBuffer); I(vkCmdBlitImage); I(vkCmdCopyImageToBuffer); I(vkCmdFillBuffer);
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

/* A texel of level texture `seed`: gradients, hard-edged patches, noise and a varying alpha. */
static void texel(uint32_t seed, uint32_t x, uint32_t y, uint32_t w, uint8_t o[4])
{
    double u = (double)x / w, v = (double)y / w, sd = seed * 0.37;
    double r = 0.5 + 0.45 * sin(6.0 * u + sd), g = 0.5 + 0.45 * cos(5.0 * v + 2 * sd), b = 0.5 + 0.4 * sin(4.0 * (u + v) + sd);
    if (((x / 16) + (y / 16) + seed) % 7 == 0) r = 1 - r, b = 1 - b;
    uint32_t h = (x * 73856093u) ^ (y * 19349663u) ^ (seed * 83492791u);
    double n = ((h >> 8) & 15) / 255.0;
    o[0] = (uint8_t)fmin(255, (r + n) * 255), o[1] = (uint8_t)fmin(255, (g + n) * 255), o[2] = (uint8_t)fmin(255, (b + n) * 255);
    o[3] = (uint8_t)(128 + 127 * sin(3.0 * u + 2.0 * v + sd));
}

/* BC1 4-colour block: endpoints at the per-channel extremes (RGB565), indices by projection. */
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
    static const int to_idx[4] = {0, 2, 3, 1};
    for (int i = 0; i < 16; i++) {
        double p = dd > 0 ? ((t[i][0] - e0[0]) * d[0] + (t[i][1] - e0[1]) * d[1] + (t[i][2] - e0[2]) * d[2]) / dd : 0;
        int k = (int)lround(fmin(3, fmax(0, p * 3)));
        idx |= (uint32_t)to_idx[k] << (2 * i);
    }
    out[0] = (uint8_t)c0, out[1] = (uint8_t)(c0 >> 8), out[2] = (uint8_t)c1, out[3] = (uint8_t)(c1 >> 8);
    memcpy(out + 4, &idx, 4);
}

/* BC3: an 8-value alpha block (a0 > a1) and a BC1 colour block. */
static void enc_bc3(const uint8_t t[16][4], uint8_t out[16])
{
    int lo = 255, hi = 0;
    for (int i = 0; i < 16; i++) lo = t[i][3] < lo ? t[i][3] : lo, hi = t[i][3] > hi ? t[i][3] : hi;
    if (hi == lo) hi = lo < 255 ? lo + 1 : 255, lo = hi - 1;
    out[0] = (uint8_t)hi, out[1] = (uint8_t)lo;
    static const int to_idx[8] = {1, 7, 6, 5, 4, 3, 2, 0}; /* from a1 up: a1, 6a1+a0 /7 ... a0 */
    uint64_t bits = 0;
    for (int i = 0; i < 16; i++) {
        int k = (int)lround((double)(t[i][3] - lo) * 7 / (hi - lo));
        bits |= (uint64_t)to_idx[k < 0 ? 0 : k > 7 ? 7 : k] << (3 * i);
    }
    for (int i = 0; i < 6; i++) out[2 + i] = (uint8_t)(bits >> (8 * i));
    enc_bc1(t, out + 8);
}

typedef struct tex {
    VkImage img;
    VkFormat fmt;
    uint32_t w, h, mips, seed;
    uint8_t *blocks; /* every level, back to back (tex_level_off) */
} tex;

static tex *texs;
static int ntex, captex;

static uint32_t bb(VkFormat f) { return f == VK_FORMAT_BC1_RGBA_UNORM_BLOCK ? 8 : f == VK_FORMAT_BC3_UNORM_BLOCK ? 16 : 0; }
static uint32_t lw(uint32_t v, uint32_t m) { return v >> m ? v >> m : 1; }
static VkDeviceSize level_bytes(const tex *t, uint32_t m)
{
    uint32_t w = lw(t->w, m), h = lw(t->h, m);
    return bb(t->fmt) ? (VkDeviceSize)((w + 3) / 4) * ((h + 3) / 4) * bb(t->fmt) : (VkDeviceSize)w * h * 4;
}
static VkDeviceSize level_off(const tex *t, uint32_t m)
{
    VkDeviceSize o = 0;
    for (uint32_t i = 0; i < m; i++) o += (level_bytes(t, i) + 63) & ~(VkDeviceSize)63;
    return o;
}

static void make_blocks(tex *t)
{
    t->blocks = malloc(level_off(t, t->mips));
    for (uint32_t m = 0; m < t->mips; m++) {
        uint32_t w = lw(t->w, m), h = lw(t->h, m);
        uint8_t *o = t->blocks + level_off(t, m), blk[16][4];
        if (!bb(t->fmt)) {
            for (uint32_t y = 0; y < h; y++)
                for (uint32_t x = 0; x < w; x++) texel(t->seed + m * 101, x, y, w, o + ((size_t)y * w + x) * 4);
            continue;
        }
        uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
        for (uint32_t by = 0; by < bh; by++)
            for (uint32_t bx = 0; bx < bw; bx++) {
                for (uint32_t i = 0; i < 16; i++) {
                    uint32_t x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
                    texel(t->seed + m * 101, x < w ? x : w - 1, y < h ? y : h - 1, w, blk[i]);
                }
                uint8_t *dst = o + ((size_t)by * bw + bx) * bb(t->fmt);
                if (t->fmt == VK_FORMAT_BC1_RGBA_UNORM_BLOCK) enc_bc1((const uint8_t(*)[4])blk, dst);
                else enc_bc3((const uint8_t(*)[4])blk, dst);
            }
    }
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
        (map && vkMapMemory(dev, m, 0, bytes, 0, map)))
        return VK_NULL_HANDLE;
    return buf;
}

static VkImage device_image(VkFormat f, uint32_t w, uint32_t h, uint32_t mips, VkImageUsageFlags usage)
{
    VkImageCreateInfo ici = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, f, {w, h, 1}, mips, 1, VK_SAMPLE_COUNT_1_BIT,
                             VK_IMAGE_TILING_OPTIMAL, usage, VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
    VkImage img;
    if (vkCreateImage(dev, &ici, NULL, &img)) return VK_NULL_HANDLE;
    VkMemoryRequirements r;
    vkGetImageMemoryRequirements(dev, img, &r);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, r.size, mem_type(r.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    VkDeviceMemory m;
    if (ai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(dev, &ai, NULL, &m) || vkBindImageMemory(dev, img, m, 0)) return VK_NULL_HANDLE;
    return img;
}

static VkImageMemoryBarrier img_barrier(VkImage img, uint32_t mips, VkAccessFlags sa, VkAccessFlags da, VkImageLayout ol, VkImageLayout nl)
{
    return (VkImageMemoryBarrier){VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, sa, da, ol, nl, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
                                  img, {VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, 1}};
}

static void submit_wait(VkCommandBuffer *cbs, uint32_t n, VkFence fence)
{
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, n, cbs, 0, NULL};
    vkQueueSubmit(queue, 1, &si, fence);
    vkWaitForFences(dev, 1, &fence, VK_TRUE, 120000000000ull);
    vkResetFences(dev, 1, &fence);
}

static void begin(VkCommandBuffer cb)
{
    vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(cb, &bi);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    uint32_t size = 128;
    int count = 400, frames = 5;
    const char *pattern = "dxvk3";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--size") && i + 1 < argc) size = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--count") && i + 1 < argc) count = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--pattern") && i + 1 < argc) pattern = argv[++i];
    }
    int pertex = !strcmp(pattern, "pertex"), mixed = !strcmp(pattern, "mixed");
    if (!pertex && !mixed && strcmp(pattern, "dxvk3")) {
        printf("FAILED: unknown pattern %s\n", pattern);
        return 2;
    }
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib) {
        printf("FAILED: loader: %s\n", dlerror());
        return 2;
    }
    gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
#define G(n) n = (PFN_##n)gipa(inst, #n)
    G(vkCreateInstance);
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "vkbridge-bcupload-check", 1, "vkbridge", 1, VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 0, NULL};
    if (vkCreateInstance(&ici, NULL, &inst)) {
        printf("FAILED: setup: vkCreateInstance\n");
        return 2;
    }
    G(vkEnumeratePhysicalDevices); G(vkGetPhysicalDeviceQueueFamilyProperties); G(vkCreateDevice); G(vkGetDeviceQueue);
    G(vkCreateCommandPool); G(vkAllocateCommandBuffers); G(vkBeginCommandBuffer); G(vkEndCommandBuffer); G(vkCmdPipelineBarrier);
    G(vkQueueSubmit); G(vkDeviceWaitIdle); G(vkCreateFence); G(vkWaitForFences); G(vkResetFences);
    G(vkGetPhysicalDeviceMemoryProperties); G(vkAllocateMemory); G(vkCreateImage); G(vkGetImageMemoryRequirements);
    G(vkBindImageMemory); G(vkCreateBuffer); G(vkGetBufferMemoryRequirements); G(vkBindBufferMemory); G(vkMapMemory);
    G(vkCmdCopyBufferToImage); G(vkResetCommandBuffer); G(vkCmdBlitImage); G(vkCmdCopyImageToBuffer); G(vkCmdFillBuffer);
#undef G
    uint32_t n = 1;
    vkEnumeratePhysicalDevices(inst, &n, &pd);
    if (!n) {
        printf("FAILED: setup: no GPU\n");
        return 2;
    }
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    uint32_t nq = 16;
    VkQueueFamilyProperties qp[16];
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qp);
    uint32_t family = UINT32_MAX;
    for (uint32_t i = 0; i < nq && family == UINT32_MAX; i++)
        if (qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) family = i;
    float prio = 1;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, family, 1, &prio};
    VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f2.features.textureCompressionBC = VK_TRUE; /* emulated by the server where the GPU lacks it */
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f2, 0, 1, &qci, 0, NULL, 0, NULL, NULL};
    if (family == UINT32_MAX || vkCreateDevice(pd, &dci, NULL, &dev)) {
        printf("FAILED: setup: vkCreateDevice\n");
        return 2;
    }
    vkGetDeviceQueue(dev, family, 0, &queue);
    VkCommandPool pool;
    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, family};
    vkCreateCommandPool(dev, &pci, NULL, &pool);
    VkCommandBuffer cbs[3];
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 3};
    vkAllocateCommandBuffers(dev, &cai, cbs);
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence;
    vkCreateFence(dev, &fci, NULL, &fence);
    VkBuffer junk = host_buffer(1 << 16, VK_BUFFER_USAGE_TRANSFER_DST_BIT, NULL);

    /* the textures and their blocks, made before the frames (a game reads them from its files) */
    uint32_t seed = 1;
    for (int f = 0; f < frames; f++)
        for (int k = 0; k < count; k++) {
            if (ntex + 2 > captex) {
                captex = captex ? captex * 2 : 256;
                texs = realloc(texs, captex * sizeof(*texs));
            }
            for (int rgba = 0; rgba < 1 + (mixed && k % 5 == 4); rgba++) {
                tex *t = &texs[ntex++];
                uint32_t h = (k & 2) ? size / 2 : size;
                *t = (tex){VK_NULL_HANDLE, rgba ? VK_FORMAT_R8G8B8A8_UNORM : k % 3 == 2 ? VK_FORMAT_BC3_UNORM_BLOCK : VK_FORMAT_BC1_RGBA_UNORM_BLOCK,
                           rgba ? 64 : size, rgba ? 64 : h, 0, seed++, NULL};
                for (uint32_t s = t->w > t->h ? t->w : t->h; s; s >>= 1) t->mips++;
                make_blocks(t);
            }
        }
    VkDeviceSize stage_cap = 0, per_frame = 0;
    for (int i = 0, k = 0; i < ntex; i++) {
        per_frame += level_off(&texs[i], texs[i].mips) + 64;
        if (++k == count + (mixed ? count / 5 : 0) || i == ntex - 1) stage_cap = per_frame > stage_cap ? per_frame : stage_cap, per_frame = 0, k = 0;
    }
    void *stage_map;
    VkBuffer stage = host_buffer(stage_cap + (1 << 20), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &stage_map);
    if (!stage) {
        printf("FAILED: setup: staging buffer\n");
        return 2;
    }

    double total = 0, worst = 0, steady = 0; /* steady: frames after the first (its converters are built in it) */
    int next = 0;
    VkImageMemoryBarrier *bars = malloc(sizeof(*bars) * (count * 2 + 8));
    for (int f = 0; f < frames; f++) {
        int first = next, last = next;
        while (last < ntex && last - first < count + (mixed ? count / 5 : 0)) last++;
        next = last;
        double t0 = now_ms();
        VkDeviceSize used = 0;
        for (int i = first; i < last; i++) {
            tex *t = &texs[i];
            t->img = device_image(t->fmt, t->w, t->h, t->mips,
                                  VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
            if (!t->img) {
                printf("FAILED: texture %d: vkCreateImage\n", i);
                return 1;
            }
        }
        if (pertex) {
            begin(cbs[0]);
            for (int i = first; i < last; i++) {
                tex *t = &texs[i];
                VkBufferImageCopy regs[16];
                for (uint32_t m = 0; m < t->mips; m++) {
                    used = (used + 63) & ~(VkDeviceSize)63;
                    memcpy((uint8_t *)stage_map + used, t->blocks + level_off(t, m), level_bytes(t, m));
                    regs[m] = (VkBufferImageCopy){used, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, m, 0, 1}, {0, 0, 0}, {lw(t->w, m), lw(t->h, m), 1}};
                    used += level_bytes(t, m);
                }
                VkImageMemoryBarrier b = img_barrier(t->img, t->mips, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                                                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                vkCmdPipelineBarrier(cbs[0], VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
                vkCmdCopyBufferToImage(cbs[0], stage, t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, t->mips, regs);
                b = img_barrier(t->img, t->mips, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                vkCmdPipelineBarrier(cbs[0], VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
            }
            vkEndCommandBuffer(cbs[0]);
            submit_wait(cbs, 1, fence);
        } else {
            /* DXVK 3.1.1: transitions in, the copies alone, transitions out */
            uint32_t nb = 0;
            for (int i = first; i < last; i++)
                bars[nb++] = img_barrier(texs[i].img, texs[i].mips, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            begin(cbs[0]);
            vkCmdPipelineBarrier(cbs[0], VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, nb, bars);
            vkEndCommandBuffer(cbs[0]);
            begin(cbs[1]);
            int bc_seen = 0;
            for (int i = first; i < last; i++) {
                tex *t = &texs[i];
                for (uint32_t m = 0; m < t->mips; m++) {
                    used = (used + 63) & ~(VkDeviceSize)63;
                    memcpy((uint8_t *)stage_map + used, t->blocks + level_off(t, m), level_bytes(t, m));
                    VkBufferImageCopy r = {used, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, m, 0, 1}, {0, 0, 0}, {lw(t->w, m), lw(t->h, m), 1}};
                    vkCmdCopyBufferToImage(cbs[1], stage, t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
                    used += level_bytes(t, m);
                }
                if (mixed && bb(t->fmt) && ++bc_seen % 7 == 0) vkCmdFillBuffer(cbs[1], junk, 0, 256, (uint32_t)i);
            }
            vkEndCommandBuffer(cbs[1]);
            nb = 0;
            for (int i = first; i < last; i++)
                bars[nb++] = img_barrier(texs[i].img, texs[i].mips, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            begin(cbs[2]);
            vkCmdPipelineBarrier(cbs[2], VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, nb, bars);
            vkEndCommandBuffer(cbs[2]);
            submit_wait(cbs, 3, fence);
        }
        double ms = now_ms() - t0;
        printf("frame %d %.1f\n", f + 1, ms);
        total += ms;
        if (f) steady += ms, worst = ms > worst ? ms : worst;
    }

    /* every level of every texture, blitted into RGBA8 and read back */
    uint32_t aw = size * 2, ah = size;
    VkImage atlas = device_image(VK_FORMAT_R8G8B8A8_UNORM, aw, ah, 1, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    void *down_map;
    VkBuffer down = host_buffer((VkDeviceSize)aw * ah * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &down_map);
    if (!atlas || !down) {
        printf("FAILED: setup: readback\n");
        return 2;
    }
    uint64_t hash = 0xcbf29ce484222325ull;
    int maxd1 = 0, maxd3 = 0, maxdr = 0, nbc = 0;
    for (int i = 0; i < ntex; i++) {
        tex *t = &texs[i];
        begin(cbs[0]);
        VkImageMemoryBarrier b = img_barrier(atlas, 1, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vkCmdPipelineBarrier(cbs[0], VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
        /* level m at x = 0 (m 0) or t->w, y = h - (h >> (m - 1)) */
        for (uint32_t m = 0; m < t->mips; m++) {
            int32_t x = m ? (int32_t)t->w : 0, y = m ? (int32_t)(t->h - lw(t->h, m - 1)) : 0;
            if (m && lw(t->h, m - 1) == 1 && t->h > 1) y = (int32_t)t->h - 1;
            VkImageBlit bl = {{VK_IMAGE_ASPECT_COLOR_BIT, m, 0, 1}, {{0, 0, 0}, {(int32_t)lw(t->w, m), (int32_t)lw(t->h, m), 1}},
                              {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{x, y, 0}, {x + (int32_t)lw(t->w, m), y + (int32_t)lw(t->h, m), 1}}};
            vkCmdBlitImage(cbs[0], t->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, atlas, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_NEAREST);
        }
        b = img_barrier(atlas, 1, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        vkCmdPipelineBarrier(cbs[0], VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
        VkBufferImageCopy rc = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {aw, ah, 1}};
        vkCmdCopyImageToBuffer(cbs[0], atlas, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, down, 1, &rc);
        VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
        vkCmdPipelineBarrier(cbs[0], VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
        vkEndCommandBuffer(cbs[0]);
        submit_wait(cbs, 1, fence);
        nbc += bb(t->fmt) != 0;
        for (uint32_t m = 0; m < t->mips; m++) {
            uint32_t w = lw(t->w, m), h = lw(t->h, m), bw = (w + 3) / 4;
            int32_t x0 = m ? (int32_t)t->w : 0, y0 = m ? (int32_t)(t->h - lw(t->h, m - 1)) : 0;
            if (m && lw(t->h, m - 1) == 1 && t->h > 1) y0 = (int32_t)t->h - 1;
            const uint8_t *lv = t->blocks + level_off(t, m);
            for (uint32_t y = 0; y < h; y++)
                for (uint32_t x = 0; x < w; x++) {
                    const uint8_t *g = (const uint8_t *)down_map + ((size_t)(y0 + y) * aw + x0 + x) * 4;
                    uint8_t want[4], dec[16 * 4];
                    if (!bb(t->fmt)) {
                        memcpy(want, lv + ((size_t)y * w + x) * 4, 4);
                    } else {
                        const uint8_t *blk = lv + ((size_t)(y / 4) * bw + x / 4) * bb(t->fmt);
                        if (t->fmt == VK_FORMAT_BC1_RGBA_UNORM_BLOCK) bcdec_bc1(blk, dec, 16);
                        else bcdec_bc3(blk, dec, 16);
                        memcpy(want, dec + ((y & 3) * 4 + (x & 3)) * 4, 4);
                    }
                    for (int c = 0; c < 4; c++) {
                        int d = abs((int)g[c] - (int)want[c]);
                        int *md = !bb(t->fmt) ? &maxdr : t->fmt == VK_FORMAT_BC1_RGBA_UNORM_BLOCK ? &maxd1 : &maxd3;
                        if (d > *md) *md = d;
                        hash = (hash ^ g[c]) * 0x100000001b3ull;
                    }
                }
        }
    }
    printf("RESULT pattern=%s textures=%d bc=%d frames=%d upload_ms=%.1f steady_ms=%.1f worst_frame_ms=%.1f maxdiff_bc1=%d maxdiff_bc3=%d maxdiff_rgba=%d "
           "hash=%016llx\n",
           pattern, ntex, nbc, frames, total, steady, worst, maxd1, maxd3, maxdr, (unsigned long long)hash);
    if (maxdr) {
        printf("FAILED: RGBA8 textures read back changed (max difference %d)\n", maxdr);
        return 1;
    }
    printf("OK: %d textures (%d BC) uploaded in %d frames (%s), every level read back\n", ntex, nbc, frames, pattern);
    return 0;
}
