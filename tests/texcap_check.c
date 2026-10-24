/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * texcap_check: the texture cap (VKBRIDGE_TEX_MAXDIM, server_texcap.c) through the bridge, the
 * server started with VKBRIDGE_TEX_MAXDIM=512 (`texcap_check`), 512,all (`texcap_check all`) or
 * 512,strict (`texcap_check strict`):
 *   - memory: a 2048x2048 BC1 texture with its 12 levels takes what a 512x512 one with 10 levels
 *     takes, a sixteenth of one the cap leaves alone (ALIAS_BIT keeps it out); a texture without
 *     a mip chain and (without `all`) an uncompressed one are not capped; with `all` a sampled-only
 *     RGBA8 texture is, one that is also a render target is not;
 *   - each level in its own solid colour, uploaded in one command with whole-range barriers, and
 *     level by level from the smallest up with a barrier from UNDEFINED around each (those of the
 *     dropped levels must not reach the kept ones), and copied level by level into another capped
 *     texture: a compute shader samples every view (the whole chain, a dropped level alone, a kept
 *     level alone, a range across the cut) at each LOD, and reads textureSize / textureQueryLevels
 *     (the smaller size: what a capped texture tells a shader);
 *   - with `all`: a clear across the cut, a blit out of a dropped level (the largest kept one is
 *     read, scaled) and a readback of a kept level and of a dropped one (left out);
 *   - with `strict`: the sampling shader reads textureSize, so a texture made after it is not capped.
 * Prints one PASS/FAIL line per check and "<n> checks, <m> failed"; exit 0 when none failed.
 *
 * Shader: tests/shaders/texcap/texcap.comp (glslc -O --target-env=vulkan1.1 -mfmt=num -> texcap_comp.inc).
 */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint32_t spv_comp[] = {
#include "texcap_comp.inc"
};

#define CAP 512

static PFN_vkGetInstanceProcAddr gipa;
static VkInstance inst;
static VkPhysicalDevice pd;
static VkDevice dev;
static VkQueue queue;
static VkCommandPool pool;
static VkPhysicalDeviceMemoryProperties mp;
static int failures, checks;

#define I(name) static PFN_##name name
I(vkCreateInstance); I(vkEnumeratePhysicalDevices); I(vkGetPhysicalDeviceProperties); I(vkGetPhysicalDeviceFeatures);
I(vkGetPhysicalDeviceMemoryProperties); I(vkCreateDevice); I(vkGetDeviceProcAddr);
I(vkGetDeviceQueue); I(vkCreateCommandPool); I(vkAllocateCommandBuffers); I(vkBeginCommandBuffer); I(vkEndCommandBuffer);
I(vkQueueSubmit); I(vkQueueWaitIdle); I(vkFreeCommandBuffers); I(vkCreateBuffer); I(vkGetBufferMemoryRequirements);
I(vkAllocateMemory); I(vkBindBufferMemory); I(vkMapMemory); I(vkCreateImage); I(vkGetImageMemoryRequirements);
I(vkBindImageMemory); I(vkCreateImageView); I(vkCreateSampler); I(vkCreateShaderModule); I(vkCreateDescriptorSetLayout);
I(vkCreatePipelineLayout); I(vkCreateComputePipelines); I(vkCreateDescriptorPool); I(vkAllocateDescriptorSets);
I(vkUpdateDescriptorSets); I(vkCmdBindPipeline); I(vkCmdBindDescriptorSets); I(vkCmdPushConstants); I(vkCmdDispatch);
I(vkCmdPipelineBarrier); I(vkCmdCopyBufferToImage); I(vkCmdCopyImage); I(vkCmdCopyImageToBuffer); I(vkCmdBlitImage);
I(vkCmdClearColorImage); I(vkDestroyImage); I(vkFreeMemory); I(vkResetDescriptorPool);
#undef I

static void check(int ok, const char *what, const char *detail)
{
    checks++;
    if (!ok) failures++;
    printf("%s %s%s%s\n", ok ? "PASS" : "FAIL", what, detail && *detail ? ": " : "", detail ? detail : "");
    fflush(stdout);
}

static uint32_t find_type(uint32_t bits, VkMemoryPropertyFlags want)
{
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if (bits & (1u << i)) return i;
    return 0;
}

static VkBuffer host_buffer(VkDeviceSize size, VkBufferUsageFlags usage, void **map)
{
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, size, usage, VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
    VkBuffer b = VK_NULL_HANDLE;
    if (vkCreateBuffer(dev, &bci, NULL, &b)) return VK_NULL_HANDLE;
    VkMemoryRequirements r;
    vkGetBufferMemoryRequirements(dev, b, &r);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, r.size,
                               find_type(r.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    VkDeviceMemory m;
    if (vkAllocateMemory(dev, &ai, NULL, &m) || vkBindBufferMemory(dev, b, m, 0) || vkMapMemory(dev, m, 0, VK_WHOLE_SIZE, 0, map))
        return VK_NULL_HANDLE;
    return b;
}

/* An image (bound to memory when bind), and its memory requirements' size. */
static VkImage image(VkFormat f, uint32_t size, uint32_t mips, VkImageUsageFlags usage, VkImageCreateFlags flags, int bind, VkDeviceSize *bytes)
{
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, flags, VK_IMAGE_TYPE_2D, f, {size, size, 1}, mips, 1,
                            VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL, usage, VK_SHARING_MODE_EXCLUSIVE, 0, NULL,
                            VK_IMAGE_LAYOUT_UNDEFINED};
    VkImage img = VK_NULL_HANDLE;
    if (vkCreateImage(dev, &ci, NULL, &img)) return VK_NULL_HANDLE;
    VkMemoryRequirements r;
    vkGetImageMemoryRequirements(dev, img, &r);
    if (bytes) *bytes = r.size;
    if (bind) {
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, r.size, find_type(r.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
        VkDeviceMemory m;
        if (vkAllocateMemory(dev, &ai, NULL, &m) || vkBindImageMemory(dev, img, m, 0)) return VK_NULL_HANDLE;
    }
    return img;
}

static VkDeviceSize size_of(VkFormat f, uint32_t size, uint32_t mips, VkImageUsageFlags usage, VkImageCreateFlags flags)
{
    VkDeviceSize b = 0;
    VkImage i = image(f, size, mips, usage, flags, 0, &b);
    if (i) vkDestroyImage(dev, i, NULL);
    return b;
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

static void submit(VkCommandBuffer cb)
{
    vkEndCommandBuffer(cb);
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cb, 0, NULL};
    vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);
    vkFreeCommandBuffers(dev, pool, 1, &cb);
}

static void barrier(VkCommandBuffer cb, VkImage img, uint32_t base, uint32_t count, VkImageLayout from, VkImageLayout to)
{
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT,
                              VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT, from, to,
                              VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, img, {VK_IMAGE_ASPECT_COLOR_BIT, base, count, 0, 1}};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}

/* Level m's colour: a BC1 565 colour (bc), or RGBA8 (uncompressed images), as floats. */
static uint16_t c565(uint32_t m) { return (uint16_t)((((2 + 2 * m) & 31) << 11) | (((60 - 4 * m) & 63) << 5) | ((5 + m) & 31)); }

static void c565_f(uint32_t m, float out[4])
{
    uint16_t c = c565(m);
    uint32_t r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    out[0] = ((r * 527 + 23) >> 6) / 255.0f, out[1] = ((g * 259 + 33) >> 6) / 255.0f, out[2] = ((b * 527 + 23) >> 6) / 255.0f, out[3] = 1;
}

static uint32_t rgba(uint32_t m) { return (uint32_t)(20 + 18 * m) | ((uint32_t)(240 - 16 * m) << 8) | ((uint32_t)(10 * m) << 16) | 0xff000000u; }

static void rgba_f(uint32_t m, float out[4])
{
    uint32_t c = rgba(m);
    for (int k = 0; k < 4; k++) out[k] = ((c >> (8 * k)) & 0xff) / 255.0f;
}

/* Fills one level of bytes at p: solid BC1 blocks or RGBA8 texels of level m's colour. Returns its bytes. */
static size_t fill_level(uint8_t *p, int bc, uint32_t size, uint32_t m)
{
    uint32_t w = size >> m ? size >> m : 1;
    if (bc) {
        uint32_t nb = ((w + 3) / 4) * ((w + 3) / 4);
        uint16_t c = c565(m);
        for (uint32_t i = 0; i < nb; i++) {
            uint8_t blk[8] = {(uint8_t)c, (uint8_t)(c >> 8), (uint8_t)c, (uint8_t)(c >> 8), 0, 0, 0, 0};
            memcpy(p + (size_t)i * 8, blk, 8);
        }
        return (size_t)nb * 8;
    }
    uint32_t c = rgba(m);
    for (uint32_t i = 0; i < w * w; i++) memcpy(p + (size_t)i * 4, &c, 4);
    return (size_t)w * w * 4;
}

/* ------------------------------------------------------------------ sampling */

static VkDescriptorSetLayout dsl;
static VkPipelineLayout layout;
static VkPipeline pipe;
static VkSampler sampler;
static VkDescriptorPool dpool;
static VkBuffer out_buf;
static float *out_map;

static void sampling_init(void)
{
    VkDescriptorSetLayoutBinding b[2] = {{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
                                         {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
    VkDescriptorSetLayoutCreateInfo dci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 2, b};
    vkCreateDescriptorSetLayout(dev, &dci, NULL, &dsl);
    VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 12};
    VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &dsl, 1, &pr};
    vkCreatePipelineLayout(dev, &pli, NULL, &layout);
    VkShaderModuleCreateInfo mci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof(spv_comp), spv_comp};
    VkShaderModule m;
    vkCreateShaderModule(dev, &mci, NULL, &m);
    VkComputePipelineCreateInfo cpi = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, NULL, 0,
                                       {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_COMPUTE_BIT, m, "main", NULL},
                                       layout, VK_NULL_HANDLE, 0};
    vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, NULL, &pipe);
    VkSamplerCreateInfo sci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, NULL, 0, VK_FILTER_NEAREST, VK_FILTER_NEAREST,
                               VK_SAMPLER_MIPMAP_MODE_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                               VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, 0, VK_FALSE, 1, VK_FALSE, VK_COMPARE_OP_ALWAYS, 0, 16,
                               VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK, VK_FALSE};
    vkCreateSampler(dev, &sci, NULL, &sampler);
    VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 64}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 64}};
    VkDescriptorPoolCreateInfo dpi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 64, 2, ps};
    vkCreateDescriptorPool(dev, &dpi, NULL, &dpool);
    out_buf = host_buffer(64 * 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, (void **)&out_map);
}

/* Samples a view of img (levels [base, base + count), count may be VK_REMAINING_MIP_LEVELS) at lod
 * (mode 0) or reads its size and level count there (mode 1): the shader's vec4. */
static void sample(VkImage img, VkFormat f, uint32_t base, uint32_t count, float lod, uint32_t mode, float out[4])
{
    VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, img, VK_IMAGE_VIEW_TYPE_2D, f, {0},
                                 {VK_IMAGE_ASPECT_COLOR_BIT, base, count, 0, 1}};
    VkImageView v = VK_NULL_HANDLE;
    memset(out, 0, 4 * sizeof(float));
    if (vkCreateImageView(dev, &vci, NULL, &v)) return;
    VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, dpool, 1, &dsl};
    VkDescriptorSet set;
    if (vkAllocateDescriptorSets(dev, &ai, &set)) {
        vkResetDescriptorPool(dev, dpool, 0);
        vkAllocateDescriptorSets(dev, &ai, &set);
    }
    VkDescriptorImageInfo ii = {sampler, v, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorBufferInfo bi = {out_buf, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet w[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &ii, NULL, NULL},
                                 {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &bi, NULL}};
    vkUpdateDescriptorSets(dev, 2, w, 0, NULL);
    VkCommandBuffer cb = begin();
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, NULL);
    struct { float lod; uint32_t slot, mode; } pc = {lod, 0, mode};
    vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    vkCmdDispatch(cb, 1, 1, 1);
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    submit(cb);
    memcpy(out, out_map, 4 * sizeof(float));
}

/* The colour sampled from a view at lod against level `want`'s; tolerance for the transcoder's rounding. */
static void expect(const char *what, VkImage img, int bc, uint32_t base, uint32_t count, float lod, uint32_t want)
{
    float got[4], ref[4];
    sample(img, bc ? VK_FORMAT_BC1_RGB_UNORM_BLOCK : VK_FORMAT_R8G8B8A8_UNORM, base, count, lod, 0, got);
    if (bc) c565_f(want, ref);
    else rgba_f(want, ref);
    int ok = 1;
    for (int k = 0; k < 3; k++) ok &= fabsf(got[k] - ref[k]) <= 6.0f / 255.0f;
    char d[200], end[12] = "end";
    if (count != VK_REMAINING_MIP_LEVELS) snprintf(end, sizeof(end), "%u", base + count);
    snprintf(d, sizeof(d), "view [%u, %s), lod %.0f: %.3f %.3f %.3f, level %u's %.3f %.3f %.3f", base, end, lod, got[0], got[1], got[2], want,
             ref[0], ref[1], ref[2]);
    check(ok, what, d);
}

int main(int argc, char **argv)
{
    int all = argc > 1 && !strcmp(argv[1], "all"), strict = argc > 1 && !strcmp(argv[1], "strict");
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib) { printf("FAIL no libvulkan\n"); return 1; }
    gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    vkCreateInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "texcap_check", 1, NULL, 0, VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 0, NULL};
    if (vkCreateInstance(&ici, NULL, &inst)) { printf("FAIL vkCreateInstance\n"); return 1; }
#define L(name) name = (PFN_##name)gipa(inst, #name)
    L(vkEnumeratePhysicalDevices); L(vkGetPhysicalDeviceProperties); L(vkGetPhysicalDeviceFeatures); L(vkGetPhysicalDeviceMemoryProperties);
    L(vkCreateDevice); L(vkGetDeviceProcAddr);
#undef L
    uint32_t n = 1;
    if (vkEnumeratePhysicalDevices(inst, &n, &pd) < 0 || !n) { printf("FAIL no GPU\n"); return 1; }
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(pd, &props);
    VkPhysicalDeviceFeatures feats;
    vkGetPhysicalDeviceFeatures(pd, &feats);
    printf("GPU: %s, textureCompressionBC=%u, cap %u%s\n", props.deviceName, feats.textureCompressionBC, CAP, all ? ",all" : strict ? ",strict" : "");
    if (!feats.textureCompressionBC) { printf("FAIL no BC\n"); return 1; }
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    float prio = 1;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, 0, 1, &prio};
    VkPhysicalDeviceFeatures en = {0};
    en.textureCompressionBC = VK_TRUE;
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &qci, 0, NULL, 0, NULL, &en};
    if (vkCreateDevice(pd, &dci, NULL, &dev)) { printf("FAIL vkCreateDevice\n"); return 1; }
#define D(name) name = (PFN_##name)vkGetDeviceProcAddr(dev, #name)
    D(vkGetDeviceQueue); D(vkCreateCommandPool); D(vkAllocateCommandBuffers); D(vkBeginCommandBuffer); D(vkEndCommandBuffer);
    D(vkQueueSubmit); D(vkQueueWaitIdle); D(vkFreeCommandBuffers); D(vkCreateBuffer); D(vkGetBufferMemoryRequirements);
    D(vkAllocateMemory); D(vkBindBufferMemory); D(vkMapMemory); D(vkCreateImage); D(vkGetImageMemoryRequirements);
    D(vkBindImageMemory); D(vkCreateImageView); D(vkCreateSampler); D(vkCreateShaderModule); D(vkCreateDescriptorSetLayout);
    D(vkCreatePipelineLayout); D(vkCreateComputePipelines); D(vkCreateDescriptorPool); D(vkAllocateDescriptorSets);
    D(vkUpdateDescriptorSets); D(vkCmdBindPipeline); D(vkCmdBindDescriptorSets); D(vkCmdPushConstants); D(vkCmdDispatch);
    D(vkCmdPipelineBarrier); D(vkCmdCopyBufferToImage); D(vkCmdCopyImage); D(vkCmdCopyImageToBuffer); D(vkCmdBlitImage);
    D(vkCmdClearColorImage); D(vkDestroyImage); D(vkFreeMemory); D(vkResetDescriptorPool);
#undef D
    vkGetDeviceQueue(dev, 0, 0, &queue);
    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, 0, 0};
    vkCreateCommandPool(dev, &pci, NULL, &pool);
    const VkFormat BC = VK_FORMAT_BC1_RGB_UNORM_BLOCK, RGBA = VK_FORMAT_R8G8B8A8_UNORM;
    const VkImageUsageFlags tex = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    char d[256];

    /* ---- memory */
    VkDeviceSize capped = size_of(BC, 2048, 12, tex, 0), ref = size_of(BC, CAP, 10, tex, 0), full = size_of(BC, 2048, 12, tex, VK_IMAGE_CREATE_ALIAS_BIT);
    snprintf(d, sizeof(d), "%llu bytes, as a %ux%u texture with 10 levels (%llu); uncapped %llu (%.1f%%)", (unsigned long long)capped, CAP, CAP,
             (unsigned long long)ref, (unsigned long long)full, full ? 100.0 * capped / full : 0);
    check(capped == ref && capped * 8 < full, "BC1 2048x2048, 12 levels: created as 512x512, 10 levels", d);
    VkDeviceSize one = size_of(BC, 2048, 1, tex, 0), one_full = size_of(BC, 2048, 1, tex, VK_IMAGE_CREATE_ALIAS_BIT);
    snprintf(d, sizeof(d), "%llu bytes, uncapped %llu", (unsigned long long)one, (unsigned long long)one_full);
    check(one == one_full, "BC1 2048x2048 without a mip chain: not capped", d);
    VkDeviceSize u = size_of(RGBA, 2048, 12, tex, 0), u_full = size_of(RGBA, 2048, 12, tex, VK_IMAGE_CREATE_ALIAS_BIT),
                 u_ref = size_of(RGBA, CAP, 10, tex, 0);
    snprintf(d, sizeof(d), "%llu bytes, uncapped %llu, as 512x512 %llu", (unsigned long long)u, (unsigned long long)u_full, (unsigned long long)u_ref);
    check(all ? u == u_ref && u * 8 < u_full : u == u_full, all ? "RGBA8 2048x2048 sampled-only: capped (all)" : "RGBA8 2048x2048: not capped (BC only)", d);
    VkDeviceSize rt = size_of(RGBA, 2048, 12, tex | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, 0),
                 rt_full = size_of(RGBA, 2048, 12, tex | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, VK_IMAGE_CREATE_ALIAS_BIT);
    snprintf(d, sizeof(d), "%llu bytes, uncapped %llu", (unsigned long long)rt, (unsigned long long)rt_full);
    check(rt == rt_full, "RGBA8 2048x2048 render target: not capped", d);

    /* ---- uploads */
    void *up;
    VkBuffer ub = host_buffer(8u << 20, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, &up);
    size_t off[12], o = 0;
    for (uint32_t m = 0; m < 12; m++) {
        off[m] = o;
        o += fill_level((uint8_t *)up + o, 1, 2048, m);
        o = (o + 255) & ~(size_t)255;
    }
    VkImage A = image(BC, 2048, 12, tex, 0, 1, NULL), B = image(BC, 2048, 12, tex, 0, 1, NULL), C = image(BC, 2048, 12, tex, 0, 1, NULL);
    if (!A || !B || !C || !ub) { printf("FAIL setup\n"); return 1; }
    VkCommandBuffer cb = begin();
    /* A: one barrier over the whole chain, one copy of all levels, then one barrier a level, in one call */
    barrier(cb, A, 0, VK_REMAINING_MIP_LEVELS, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy r[12];
    for (uint32_t m = 0; m < 12; m++) {
        uint32_t w = 2048 >> m;
        r[m] = (VkBufferImageCopy){off[m], 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, m, 0, 1}, {0, 0, 0}, {w, w, 1}};
    }
    vkCmdCopyBufferToImage(cb, ub, A, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 12, r);
    VkImageMemoryBarrier bs[12];
    for (uint32_t m = 0; m < 12; m++)
        bs[m] = (VkImageMemoryBarrier){VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_QUEUE_FAMILY_IGNORED,
                                       VK_QUEUE_FAMILY_IGNORED, A, {VK_IMAGE_ASPECT_COLOR_BIT, m, 1, 0, 1}};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 12, bs);
    /* B: level by level from the smallest, each from UNDEFINED (a dropped level's must not discard a kept one) */
    for (int m = 11; m >= 0; m--) {
        barrier(cb, B, (uint32_t)m, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vkCmdCopyBufferToImage(cb, ub, B, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r[m]);
        barrier(cb, B, (uint32_t)m, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    /* C: A copied into it level by level */
    barrier(cb, C, 0, 12, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    barrier(cb, A, 0, 12, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkImageCopy ic[12];
    for (uint32_t m = 0; m < 12; m++) {
        uint32_t w = 2048 >> m;
        ic[m] = (VkImageCopy){{VK_IMAGE_ASPECT_COLOR_BIT, m, 0, 1}, {0, 0, 0}, {VK_IMAGE_ASPECT_COLOR_BIT, m, 0, 1}, {0, 0, 0}, {w, w, 1}};
    }
    vkCmdCopyImage(cb, A, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, C, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 12, ic);
    barrier(cb, A, 0, 12, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    barrier(cb, C, 0, 12, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    submit(cb);

    /* ---- sampling (the shader module is made here, after the images: the server logs its scan) */
    sampling_init();
    const uint32_t R = VK_REMAINING_MIP_LEVELS, drop = 2;
    for (uint32_t k = 0; k < 10; k++) expect("BC1 capped, whole chain, by LOD (level LOD + 2: the top 2 dropped)", A, 1, 0, R, (float)k, k + drop);
    expect("BC1 capped, a view of dropped level 0 alone: the largest kept level", A, 1, 0, 1, 0, drop);
    expect("BC1 capped, a view of kept level 5 alone", A, 1, 5, 1, 0, 5);
    expect("BC1 capped, a view of levels 1-4 (across the cut), LOD 0", A, 1, 1, 4, 0, 2);
    expect("BC1 capped, a view of levels 1-4 (across the cut), LOD 2", A, 1, 1, 4, 2, 4);
    expect("BC1 capped, level by level from UNDEFINED, LOD 0", B, 1, 0, R, 0, drop);
    expect("BC1 capped, level by level from UNDEFINED, LOD 9", B, 1, 0, R, 9, 11);
    expect("BC1 capped, copied level by level from a capped one, LOD 0", C, 1, 0, R, 0, drop);
    expect("BC1 capped, copied level by level from a capped one, LOD 3", C, 1, 0, R, 3, 5);
    float q[4];
    sample(A, BC, 0, R, 0, 1, q);
    snprintf(d, sizeof(d), "textureSize %.0fx%.0f, %.0f levels (the app made 2048x2048, 12)", q[0], q[1], q[2]);
    check(q[0] == CAP && q[1] == CAP && q[2] == 10, "BC1 capped: what a shader reads of its size", d);
    if (strict) {
        VkDeviceSize after = size_of(BC, 2048, 12, tex, 0);
        snprintf(d, sizeof(d), "%llu bytes, uncapped %llu", (unsigned long long)after, (unsigned long long)full);
        check(after == full, "strict: a texture made after a shader reading texture sizes is not capped", d);
    }

    if (all) {
        /* ---- an RGBA8 1024x1024 sampled-only texture, 11 levels: the top one dropped */
        VkImage G = image(RGBA, 1024, 11, tex, 0, 1, NULL), T = image(RGBA, 4, 1, tex, 0, 1, NULL);
        size_t go[11];
        o = 0;
        for (uint32_t m = 0; m < 11; m++) {
            go[m] = o;
            o += fill_level((uint8_t *)up + o, 0, 1024, m);
            o = (o + 255) & ~(size_t)255;
        }
        cb = begin();
        barrier(cb, G, 0, R, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        for (uint32_t m = 0; m < 11; m++) {
            uint32_t w = 1024 >> m;
            VkBufferImageCopy gr = {go[m], 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, m, 0, 1}, {0, 0, 0}, {w, w, 1}};
            vkCmdCopyBufferToImage(cb, ub, G, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &gr);
        }
        barrier(cb, G, 0, R, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        /* a blit out of dropped level 0 (the whole of it): the largest kept level is read */
        barrier(cb, T, 0, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageBlit bl = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{0, 0, 0}, {1024, 1024, 1}}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{0, 0, 0}, {4, 4, 1}}};
        vkCmdBlitImage(cb, G, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, T, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_NEAREST);
        /* readbacks: kept level 3 to the buffer's start, dropped level 0 after it (left out: stays 0xab) */
        uint8_t *rb = (uint8_t *)up + (6u << 20);
        memset(rb, 0xab, 1 << 16);
        VkBufferImageCopy k3 = {6u << 20, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 3, 0, 1}, {0, 0, 0}, {4, 4, 1}};
        VkBufferImageCopy k0 = {(6u << 20) + 4096, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {4, 4, 1}};
        vkCmdCopyImageToBuffer(cb, G, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, ub, 1, &k3);
        vkCmdCopyImageToBuffer(cb, G, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, ub, 1, &k0);
        barrier(cb, T, 0, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy tb = {(6u << 20) + 8192, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {4, 4, 1}};
        vkCmdCopyImageToBuffer(cb, T, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, ub, 1, &tb);
        /* a clear of levels 0-1 (across the cut): kept level 1 cleared */
        barrier(cb, G, 0, R, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkClearColorValue cc = {.float32 = {1, 0, 1, 1}};
        VkImageSubresourceRange cr = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 2, 0, 1};
        vkCmdClearColorImage(cb, G, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cc, 1, &cr);
        barrier(cb, G, 0, R, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
        submit(cb);
        uint32_t want3 = rgba(3), want1 = rgba(1), got3, got0, gotb;
        memcpy(&got3, rb, 4), memcpy(&got0, rb + 4096, 4), memcpy(&gotb, rb + 8192, 4);
        snprintf(d, sizeof(d), "0x%08x, level 3's 0x%08x", got3, want3);
        check(got3 == want3, "RGBA8 capped: readback of kept level 3", d);
        snprintf(d, sizeof(d), "0x%08x (0xabababab: left out)", got0);
        check(got0 == 0xababababu, "RGBA8 capped: readback of dropped level 0 left out", d);
        snprintf(d, sizeof(d), "0x%08x, level 1's 0x%08x", gotb, want1);
        check(gotb == want1, "RGBA8 capped: a blit out of dropped level 0 reads the largest kept level", d);
        float c[4];
        sample(G, RGBA, 0, R, 0, 0, c);
        snprintf(d, sizeof(d), "%.3f %.3f %.3f", c[0], c[1], c[2]);
        check(c[0] > 0.99f && c[1] < 0.01f && c[2] > 0.99f, "RGBA8 capped: a clear of levels 0-1 clears kept level 1", d);
        expect("RGBA8 capped, whole chain, LOD 1 (level 2, not cleared)", G, 0, 0, R, 1, 2);
        expect("RGBA8 capped, whole chain, LOD 9", G, 0, 0, R, 9, 10);
    }
    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
