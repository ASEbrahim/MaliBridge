/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * xfb_check: transform feedback (VK_EXT_transform_feedback) through the bridge, the way DXVK uses it
 * for D3D11 stream output (Metro Redux, Unity's GPU skinning):
 *   pass 1  bind two capture buffers (sizes given; the second at an offset), begin without counters,
 *           an instanced draw (5 vertices x 2 instances from vertex 3, instance 1), end into counters;
 *   pass 2  bind again (no sizes), begin from the counters (appending), 3 more vertices, end into them;
 *   pass 3  vkCmdDrawIndirectByteCountEXT: as many vertices as pass 1 and 2 captured (the counter
 *           over the stride), captured into two other buffers, ended into one counter with its
 *           offsets passed as NULL.
 * Each captured vertex is compared with what its vertex/instance index says, the gaps in the stride
 * must keep what was there, and the counters must hold the bytes written.
 *
 *   xfb_check       the vertex shader captures its own outputs (Xfb in the vertex stage)
 *   xfb_check gs    DXVK's shape: a plain vertex shader, a pass-through point geometry shader that
 *                   captures, rasterizer discard. A server that emulates anything folds that
 *                   geometry stage into the vertex shader (server_emu_shader.c, Mali's slow geometry
 *                   shaders); VKBRIDGE_NO_XFB_FOLD=1 keeps it. The bytes must be the same.
 * Exit 77: the device has no transform feedback.
 *
 * Shaders: tests/shaders/xfb/ (glslangValidator -V --target-env vulkan1.1 -x -> xfb_*.inc).
 */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint32_t xfb_vert[] = {
#include "xfb_xfb_vert.inc"
};
static const uint32_t plain_vert[] = {
#include "xfb_plain_vert.inc"
};
static const uint32_t so_geom[] = {
#include "xfb_so_geom.inc"
};

#define I(name) static PFN_##name name
I(vkCreateInstance); I(vkEnumeratePhysicalDevices); I(vkGetPhysicalDeviceQueueFamilyProperties); I(vkGetPhysicalDeviceMemoryProperties);
I(vkGetPhysicalDeviceFeatures2); I(vkGetPhysicalDeviceProperties2); I(vkEnumerateDeviceExtensionProperties); I(vkCreateDevice);
I(vkGetDeviceProcAddr); I(vkGetDeviceQueue); I(vkCreateImage); I(vkGetImageMemoryRequirements); I(vkAllocateMemory); I(vkBindImageMemory);
I(vkCreateImageView); I(vkCreateRenderPass); I(vkCreateFramebuffer); I(vkCreateShaderModule); I(vkCreatePipelineLayout);
I(vkCreateGraphicsPipelines); I(vkCreateCommandPool); I(vkAllocateCommandBuffers); I(vkBeginCommandBuffer); I(vkCmdBeginRenderPass);
I(vkCmdBindPipeline); I(vkCmdPushConstants); I(vkCmdDraw); I(vkCmdEndRenderPass); I(vkEndCommandBuffer); I(vkQueueSubmit);
I(vkQueueWaitIdle); I(vkCreateBuffer); I(vkGetBufferMemoryRequirements); I(vkBindBufferMemory); I(vkMapMemory); I(vkCmdFillBuffer);
I(vkCmdCopyBuffer); I(vkCmdPipelineBarrier); I(vkDestroyDevice); I(vkCmdBindTransformFeedbackBuffersEXT);
I(vkCmdBeginTransformFeedbackEXT); I(vkCmdEndTransformFeedbackEXT); I(vkCmdDrawIndirectByteCountEXT);
#undef I

static VkPhysicalDeviceMemoryProperties mp;
static VkDevice dev;

static uint32_t find_type(uint32_t bits, VkMemoryPropertyFlags f)
{
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & f) == f) return i;
    return UINT32_MAX;
}

static VkBuffer make_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags want, void **map)
{
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, size, usage, VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
    VkBuffer b;
    if (vkCreateBuffer(dev, &bci, NULL, &b)) return VK_NULL_HANDLE;
    VkMemoryRequirements r;
    vkGetBufferMemoryRequirements(dev, b, &r);
    uint32_t t = find_type(r.memoryTypeBits, want);
    if (t == UINT32_MAX) t = find_type(r.memoryTypeBits, 0);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, r.size, t};
    VkDeviceMemory m;
    if (vkAllocateMemory(dev, &ai, NULL, &m)) return VK_NULL_HANDLE;
    vkBindBufferMemory(dev, b, m, 0);
    if (map) vkMapMemory(dev, m, 0, VK_WHOLE_SIZE, 0, map);
    return b;
}

static void barrier(VkCommandBuffer cb, VkPipelineStageFlags src, VkAccessFlags sa, VkPipelineStageFlags dst, VkAccessFlags da)
{
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, sa, da};
    vkCmdPipelineBarrier(cb, src, dst, 0, 1, &mb, 0, NULL, 0, NULL);
}

#define STRIDE0 32u
#define BUF0 (64u * STRIDE0)
#define OFF1 16u /* the second buffer is bound this far into its buffer */
#define BUF1 (OFF1 + 64u * 4u)
#define GAP 0xDEADBEEFu

static int fails;
static void check(int ok, const char *what)
{
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) fails++;
}

/* Record r of a pass as the shaders compute it: vertex v, instance i, pass p. */
static int record_ok(const uint8_t *b0, const uint8_t *b1, uint32_t r, float v, float i, float p, char *why, size_t cap)
{
    float a[6], c;
    memcpy(a, b0 + r * STRIDE0, 24);
    memcpy(&c, b1 + OFF1 + r * 4, 4);
    uint32_t gap[2];
    memcpy(gap, b0 + r * STRIDE0 + 24, 8);
    const float want[6] = {v, i, 1.0f + p, 2.0f, v * 0.5f, i * 0.25f};
    for (int k = 0; k < 6; k++)
        if (a[k] != want[k]) {
            snprintf(why, cap, "record %u component %d: %g, want %g", r, k, a[k], want[k]);
            return 0;
        }
    if (c != v + 100.0f * i) {
        snprintf(why, cap, "record %u, second buffer: %g, want %g", r, c, v + 100.0f * i);
        return 0;
    }
    if (gap[0] != GAP || gap[1] != GAP) {
        snprintf(why, cap, "record %u: the 8 bytes past the captured ones were written (0x%08x 0x%08x)", r, gap[0], gap[1]);
        return 0;
    }
    return 1;
}

int main(int argc, char **argv)
{
    int gs = argc > 1 && !strcmp(argv[1], "gs");
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib) return printf("FAIL loader\n"), 2;
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    vkCreateInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "xfb_check", 1, "vkbridge", 1, VK_API_VERSION_1_1};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 0, NULL};
    VkInstance inst;
    if (vkCreateInstance(&ici, NULL, &inst)) return printf("FAIL instance\n"), 1;
#define G(n) n = (PFN_##n)gipa(inst, #n)
    G(vkEnumeratePhysicalDevices); G(vkGetPhysicalDeviceQueueFamilyProperties); G(vkGetPhysicalDeviceMemoryProperties);
    G(vkGetPhysicalDeviceFeatures2); G(vkGetPhysicalDeviceProperties2); G(vkEnumerateDeviceExtensionProperties); G(vkCreateDevice);
    G(vkGetDeviceProcAddr);
#undef G
    uint32_t n = 1;
    VkPhysicalDevice pd;
    vkEnumeratePhysicalDevices(inst, &n, &pd);
    if (!n) return printf("FAIL no device\n"), 1;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    uint32_t ne = 0;
    vkEnumerateDeviceExtensionProperties(pd, NULL, &ne, NULL);
    VkExtensionProperties *ep = calloc(ne ? ne : 1, sizeof(*ep));
    vkEnumerateDeviceExtensionProperties(pd, NULL, &ne, ep);
    int has = 0;
    for (uint32_t i = 0; i < ne; i++) has |= !strcmp(ep[i].extensionName, VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME);
    free(ep);
    if (!has) return printf("OK: skipped (no VK_EXT_transform_feedback)\n"), 77;
    VkPhysicalDeviceTransformFeedbackFeaturesEXT xf = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT};
    VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &xf};
    vkGetPhysicalDeviceFeatures2(pd, &f2);
    VkPhysicalDeviceTransformFeedbackPropertiesEXT xp = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_PROPERTIES_EXT};
    VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &xp};
    vkGetPhysicalDeviceProperties2(pd, &p2);
    printf("transformFeedback %u geometryStreams %u; buffers %u, streams %u, draw %u, rasterizationStreamSelect %u\n", xf.transformFeedback,
           xf.geometryStreams, xp.maxTransformFeedbackBuffers, xp.maxTransformFeedbackStreams, xp.transformFeedbackDraw,
           xp.transformFeedbackRasterizationStreamSelect);
    if (!xf.transformFeedback || xp.maxTransformFeedbackBuffers < 2) return printf("OK: skipped (transformFeedback off or fewer than 2 buffers)\n"), 77;
    if (gs && !f2.features.geometryShader) return printf("OK: skipped (no geometry shaders)\n"), 77;
    int draw_bc = xp.transformFeedbackDraw;

    uint32_t nq = 0;
    VkQueueFamilyProperties qp[16];
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, NULL);
    if (nq > 16) nq = 16;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qp);
    uint32_t qf = 0;
    while (qf < nq && !(qp[qf].queueFlags & VK_QUEUE_GRAPHICS_BIT)) qf++;
    float prio = 1;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, qf, 1, &prio};
    VkPhysicalDeviceTransformFeedbackFeaturesEXT exf = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT, NULL, VK_TRUE, VK_FALSE};
    VkPhysicalDeviceFeatures2 ef = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &exf};
    ef.features.geometryShader = gs ? VK_TRUE : VK_FALSE;
    const char *exts[] = {VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &ef, 0, 1, &qci, 0, NULL, 1, exts, NULL};
    if (vkCreateDevice(pd, &dci, NULL, &dev)) return printf("FAIL device\n"), 1;
#define D(n) n = (PFN_##n)vkGetDeviceProcAddr(dev, #n)
    D(vkGetDeviceQueue); D(vkCreateImage); D(vkGetImageMemoryRequirements); D(vkAllocateMemory); D(vkBindImageMemory);
    D(vkCreateImageView); D(vkCreateRenderPass); D(vkCreateFramebuffer); D(vkCreateShaderModule); D(vkCreatePipelineLayout);
    D(vkCreateGraphicsPipelines); D(vkCreateCommandPool); D(vkAllocateCommandBuffers); D(vkBeginCommandBuffer); D(vkCmdBeginRenderPass);
    D(vkCmdBindPipeline); D(vkCmdPushConstants); D(vkCmdDraw); D(vkCmdEndRenderPass); D(vkEndCommandBuffer); D(vkQueueSubmit);
    D(vkQueueWaitIdle); D(vkCreateBuffer); D(vkGetBufferMemoryRequirements); D(vkBindBufferMemory); D(vkMapMemory); D(vkCmdFillBuffer);
    D(vkCmdCopyBuffer); D(vkCmdPipelineBarrier); D(vkDestroyDevice); D(vkCmdBindTransformFeedbackBuffersEXT);
    D(vkCmdBeginTransformFeedbackEXT); D(vkCmdEndTransformFeedbackEXT); D(vkCmdDrawIndirectByteCountEXT);
#undef D
    if (!vkCmdBindTransformFeedbackBuffersEXT || !vkCmdBeginTransformFeedbackEXT || !vkCmdEndTransformFeedbackEXT || !vkCmdDrawIndirectByteCountEXT)
        return printf("FAIL the transform feedback commands are missing from vkGetDeviceProcAddr\n"), 1;
    VkQueue q;
    vkGetDeviceQueue(dev, qf, 0, &q);

    /* A 1x1 target: nothing is drawn (rasterizer discard), but the commands live in a render pass. */
    VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {1, 1, 1}, 1, 1,
                            VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL,
                            VK_IMAGE_LAYOUT_UNDEFINED};
    VkImage img;
    vkCreateImage(dev, &ii, NULL, &img);
    VkMemoryRequirements r;
    vkGetImageMemoryRequirements(dev, img, &r);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, r.size, find_type(r.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    if (ai.memoryTypeIndex == UINT32_MAX) ai.memoryTypeIndex = find_type(r.memoryTypeBits, 0);
    VkDeviceMemory im;
    vkAllocateMemory(dev, &ai, NULL, &im);
    vkBindImageMemory(dev, img, im, 0);
    VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, img, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM,
                                 {0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkImageView view;
    vkCreateImageView(dev, &vci, NULL, &view);
    VkAttachmentDescription att = {0, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE,
                                   VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED,
                                   VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub = {0, VK_PIPELINE_BIND_POINT_GRAPHICS, 0, NULL, 1, &ref, NULL, NULL, 0, NULL};
    VkRenderPassCreateInfo rpci = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, NULL, 0, 1, &att, 1, &sub, 0, NULL};
    VkRenderPass rp;
    vkCreateRenderPass(dev, &rpci, NULL, &rp);
    VkFramebufferCreateInfo fci = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, NULL, 0, rp, 1, &view, 1, 1, 1};
    VkFramebuffer fb;
    vkCreateFramebuffer(dev, &fci, NULL, &fb);

    VkShaderModule vs, gsm = VK_NULL_HANDLE;
    VkShaderModuleCreateInfo smci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, gs ? sizeof(plain_vert) : sizeof(xfb_vert),
                                     gs ? plain_vert : xfb_vert};
    vkCreateShaderModule(dev, &smci, NULL, &vs);
    if (gs) {
        VkShaderModuleCreateInfo g = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof(so_geom), so_geom};
        vkCreateShaderModule(dev, &g, NULL, &gsm);
    }
    VkPushConstantRange pcr = {VK_SHADER_STAGE_VERTEX_BIT, 0, 4};
    VkPipelineLayoutCreateInfo plci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 0, NULL, 1, &pcr};
    VkPipelineLayout pl;
    vkCreatePipelineLayout(dev, &plci, NULL, &pl);
    VkPipelineShaderStageCreateInfo st[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", NULL},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_GEOMETRY_BIT, gsm, "main", NULL}};
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0, VK_PRIMITIVE_TOPOLOGY_POINT_LIST, 0};
    VkViewport vpt = {0, 0, 1, 1, 0, 1};
    VkRect2D sc = {{0, 0}, {1, 1}};
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL, 0, 1, &vpt, 1, &sc};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.rasterizerDiscardEnable = VK_TRUE; /* as DXVK's stream output without a rasterized stream */
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, NULL, 0, VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState cba = {0};
    cba.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cbs = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, NULL, 0, 0, 0, 1, &cba, {0}};
    VkGraphicsPipelineCreateInfo gci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, NULL, 0, gs ? 2 : 1, st, &vi, &ia, NULL, &vp, &rs, &ms, NULL,
                                        &cbs, NULL, pl, rp, 0, VK_NULL_HANDLE, -1};
    VkPipeline pipe;
    VkResult pr = vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gci, NULL, &pipe);
    if (pr) return printf("FAIL pipeline: %d\n", pr), 1;

    const VkBufferUsageFlags xu = VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer cap[4] = {make_buffer(BUF0, xu, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, NULL), make_buffer(BUF1, xu, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, NULL),
                       make_buffer(BUF0, xu, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, NULL), make_buffer(BUF1, xu, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, NULL)};
    VkBuffer cnt = make_buffer(64, VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_COUNTER_BUFFER_BIT_EXT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                                       VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, NULL);
    uint8_t *rb;
    const VkDeviceSize rbsize = 2 * BUF0 + 2 * BUF1 + 64;
    VkBuffer rbuf = make_buffer(rbsize, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                (void **)&rb);
    for (int i = 0; i < 4; i++)
        if (!cap[i]) return printf("FAIL capture buffer %d\n", i), 1;
    if (!cnt || !rbuf || !rb) return printf("FAIL counter or readback buffer\n"), 1;

    VkCommandPoolCreateInfo cpci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, 0, qf};
    VkCommandPool pool;
    vkCreateCommandPool(dev, &cpci, NULL, &pool);
    VkCommandBufferAllocateInfo cbai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VkCommandBuffer cb;
    vkAllocateCommandBuffers(dev, &cbai, &cb);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
    vkBeginCommandBuffer(cb, &bi);
    for (int i = 0; i < 4; i++) vkCmdFillBuffer(cb, cap[i], 0, VK_WHOLE_SIZE, GAP);
    vkCmdFillBuffer(cb, cnt, 0, VK_WHOLE_SIZE, 0);
    barrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFORM_FEEDBACK_BIT_EXT,
            VK_ACCESS_TRANSFORM_FEEDBACK_WRITE_BIT_EXT | VK_ACCESS_TRANSFORM_FEEDBACK_COUNTER_READ_BIT_EXT |
                VK_ACCESS_TRANSFORM_FEEDBACK_COUNTER_WRITE_BIT_EXT);
    VkRenderPassBeginInfo rbi = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, NULL, rp, fb, {{0, 0}, {1, 1}}, 0, NULL};
    const VkDeviceSize zero_off[2] = {0, 4}; /* counters: buffer 0's at 0, buffer 1's at 4 */

    /* pass 1: sizes given, no counters to resume from */
    float pass = 0;
    vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    VkBuffer b01[2] = {cap[0], cap[1]};
    VkDeviceSize offs[2] = {0, OFF1}, sizes[2] = {BUF0, BUF1 - OFF1};
    vkCmdBindTransformFeedbackBuffersEXT(cb, 0, 2, b01, offs, sizes);
    vkCmdBeginTransformFeedbackEXT(cb, 0, 0, NULL, NULL);
    vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_VERTEX_BIT, 0, 4, &pass);
    vkCmdDraw(cb, 5, 2, 3, 1);
    VkBuffer cc[2] = {cnt, cnt};
    vkCmdEndTransformFeedbackEXT(cb, 0, 2, cc, zero_off);
    vkCmdEndRenderPass(cb);
    barrier(cb, VK_PIPELINE_STAGE_TRANSFORM_FEEDBACK_BIT_EXT, VK_ACCESS_TRANSFORM_FEEDBACK_COUNTER_WRITE_BIT_EXT | VK_ACCESS_TRANSFORM_FEEDBACK_WRITE_BIT_EXT,
            VK_PIPELINE_STAGE_TRANSFORM_FEEDBACK_BIT_EXT, VK_ACCESS_TRANSFORM_FEEDBACK_COUNTER_READ_BIT_EXT | VK_ACCESS_TRANSFORM_FEEDBACK_WRITE_BIT_EXT);

    /* pass 2: no sizes, resumed from the counters */
    pass = 1;
    vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdBindTransformFeedbackBuffersEXT(cb, 0, 2, b01, offs, NULL);
    vkCmdBeginTransformFeedbackEXT(cb, 0, 2, cc, zero_off);
    vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_VERTEX_BIT, 0, 4, &pass);
    vkCmdDraw(cb, 3, 1, 20, 0);
    vkCmdEndTransformFeedbackEXT(cb, 0, 2, cc, zero_off);
    vkCmdEndRenderPass(cb);

    /* pass 3: as many vertices as captured, from the counter */
    if (draw_bc) {
        barrier(cb, VK_PIPELINE_STAGE_TRANSFORM_FEEDBACK_BIT_EXT, VK_ACCESS_TRANSFORM_FEEDBACK_COUNTER_WRITE_BIT_EXT,
                VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT, VK_ACCESS_INDIRECT_COMMAND_READ_BIT);
        pass = 2;
        vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        VkBuffer b23[2] = {cap[2], cap[3]};
        vkCmdBindTransformFeedbackBuffersEXT(cb, 0, 2, b23, offs, sizes);
        vkCmdBeginTransformFeedbackEXT(cb, 0, 0, NULL, NULL);
        vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_VERTEX_BIT, 0, 4, &pass);
        vkCmdDrawIndirectByteCountEXT(cb, 1, 0, cnt, 0, 0, STRIDE0);
        /* buffer 0's counter only, its offset array NULL (offset 0): 13 vertices again */
        vkCmdEndTransformFeedbackEXT(cb, 0, 1, cc, NULL);
        vkCmdEndRenderPass(cb);
    }
    barrier(cb, VK_PIPELINE_STAGE_TRANSFORM_FEEDBACK_BIT_EXT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
            VK_ACCESS_TRANSFORM_FEEDBACK_WRITE_BIT_EXT | VK_ACCESS_TRANSFORM_FEEDBACK_COUNTER_WRITE_BIT_EXT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_ACCESS_TRANSFER_READ_BIT);
    VkBufferCopy c0 = {0, 0, BUF0}, c1 = {0, BUF0, BUF1}, c2 = {0, BUF0 + BUF1, BUF0}, c3 = {0, 2 * BUF0 + BUF1, BUF1},
                 c4 = {0, 2 * BUF0 + 2 * BUF1, 64};
    vkCmdCopyBuffer(cb, cap[0], rbuf, 1, &c0);
    vkCmdCopyBuffer(cb, cap[1], rbuf, 1, &c1);
    vkCmdCopyBuffer(cb, cap[2], rbuf, 1, &c2);
    vkCmdCopyBuffer(cb, cap[3], rbuf, 1, &c3);
    vkCmdCopyBuffer(cb, cnt, rbuf, 1, &c4);
    barrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
    vkEndCommandBuffer(cb);
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cb, 0, NULL};
    if (vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE) || vkQueueWaitIdle(q)) return printf("FAIL submit\n"), 1;

    const uint8_t *b0 = rb, *b1 = rb + BUF0, *b2 = rb + BUF0 + BUF1, *b3 = rb + 2 * BUF0 + BUF1;
    uint32_t counters[16];
    memcpy(counters, rb + 2 * BUF0 + 2 * BUF1, sizeof(counters));
    char why[160] = "", line[256];
    int ok = 1;
    for (uint32_t i = 0; i < 10 && ok; i++) ok = record_ok(b0, b1, i, (float)(3 + i % 5), (float)(1 + i / 5), 0, why, sizeof(why));
    snprintf(line, sizeof(line), "pass 1, an instanced draw: 10 vertices captured into two buffers%s%s", ok ? "" : ": ", ok ? "" : why);
    check(ok, line);
    ok = 1;
    why[0] = 0;
    for (uint32_t i = 0; i < 3 && ok; i++) ok = record_ok(b0, b1, 10 + i, (float)(20 + i), 0, 1, why, sizeof(why));
    if (ok) ok = record_ok(b0, b1, 13, 0, 0, 0, why, sizeof(why)) == 0; /* nothing past the 13th */
    snprintf(line, sizeof(line), "pass 2, resumed from the counters: 3 vertices appended, nothing past them%s%s", ok ? "" : ": ",
             ok ? "" : (why[0] ? why : "a 14th record"));
    check(ok, line);
    if (draw_bc) {
        ok = 1;
        why[0] = 0;
        for (uint32_t i = 0; i < 13 && ok; i++) ok = record_ok(b2, b3, i, (float)i, 0, 2, why, sizeof(why));
        float past;
        memcpy(&past, b2 + 13 * STRIDE0, 4);
        uint32_t pastu;
        memcpy(&pastu, &past, 4);
        if (ok && pastu != GAP) ok = 0, snprintf(why, sizeof(why), "a 14th vertex drawn (0x%08x)", pastu);
        snprintf(line, sizeof(line), "pass 3, vkCmdDrawIndirectByteCountEXT: 13 vertices drawn from the counter%s%s", ok ? "" : ": ", ok ? "" : why);
        check(ok, line);
        snprintf(line, sizeof(line), "counters after pass 3: %u and %u bytes (want %u and %u)", counters[0], counters[1], 13 * STRIDE0, 13 * 4);
        check(counters[0] == 13 * STRIDE0 && counters[1] == 13 * 4, line);
    } else {
        snprintf(line, sizeof(line), "counters: %u and %u bytes (want %u and %u)", counters[0], counters[1], 13 * STRIDE0, 13 * 4);
        check(counters[0] == 13 * STRIDE0 && counters[1] == 13 * 4, line);
        printf("SKIP pass 3 (transformFeedbackDraw is off)\n");
    }
    vkQueueWaitIdle(q);
    vkDestroyDevice(dev, NULL);
    if (fails) return printf("FAILED: %d of %d checks (%s)\n", fails, draw_bc ? 4 : 3, gs ? "geometry stage" : "vertex stage"), 1;
    printf("OK: %d checks passed (%s captures)\n", draw_bc ? 4 : 3, gs ? "geometry stage" : "vertex stage");
    return 0;
}
