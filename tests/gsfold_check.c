/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * gsfold_check: a vertex + layer-only pass-through geometry + fragment pipeline (UE4's
 * WriteToSliceGS shape, which the Mali-G925 compiler never finished in Stray) draws into the
 * layer the vertex shader picked. Run against a plain server and one with VKBRIDGE_LAYER_GS_FOLD=1,
 * where the bridge drops the geometry stage and the vertex shader writes Layer itself
 * (spirv_gsfold.c): the picture must be the same. Draws into slice 1 of a 2-layer image, then
 * slice 0, and checks both layers each time.
 *
 * --mark: the geometry stage carries server_guard.c's test marker, so a server with
 * VKBRIDGE_TEST_CRASH_PIPELINE=spin spins in that compile; the compile watchdog must then build
 * the pipeline without the geometry stage (its first fallback) and the picture must still be right.
 *
 * --lut: the fragment stage is UE4's colour-grading LUT shape (shaders/gsfold/lut.frag) and carries
 * the marker: the server's compile watchdog must replace it with its neutral LUT stand-in, and
 * the drawn texel must be that stand-in's value (no grading) - not this shader's red tint.
 *
 * Shaders: tests/shaders/gsfold/ (glslc -O --target-env=vulkan1.2 -mfmt=num -> gsfold_*.inc).
 */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint32_t vert_spv[] = {
#include "gsfold_vert.inc"
};
static const uint32_t geom_spv[] = {
#include "gsfold_geom.inc"
};
static const uint32_t lut_spv[] = {
#include "gsfold_lut.inc"
};
static const uint32_t frag_spv[] = {
#include "gsfold_frag.inc"
};

#define W 16
#define H 16
#define I(name) static PFN_##name name
I(vkCreateInstance); I(vkEnumeratePhysicalDevices); I(vkGetPhysicalDeviceQueueFamilyProperties); I(vkGetPhysicalDeviceMemoryProperties);
I(vkGetPhysicalDeviceFeatures2); I(vkCreateDevice); I(vkGetDeviceProcAddr); I(vkGetDeviceQueue); I(vkCreateImage);
I(vkGetImageMemoryRequirements); I(vkAllocateMemory); I(vkBindImageMemory); I(vkCreateImageView); I(vkCreateRenderPass);
I(vkCreateFramebuffer); I(vkCreateShaderModule); I(vkCreatePipelineLayout); I(vkCreateGraphicsPipelines); I(vkCreateCommandPool);
I(vkAllocateCommandBuffers); I(vkBeginCommandBuffer); I(vkCmdBeginRenderPass); I(vkCmdBindPipeline); I(vkCmdPushConstants);
I(vkCmdDraw); I(vkCmdEndRenderPass); I(vkCmdCopyImageToBuffer); I(vkEndCommandBuffer); I(vkQueueSubmit); I(vkQueueWaitIdle);
I(vkCreateBuffer); I(vkGetBufferMemoryRequirements); I(vkBindBufferMemory); I(vkMapMemory); I(vkCmdSetViewport); I(vkCmdSetScissor);
I(vkDestroyDevice);
#undef I

static VkPhysicalDeviceMemoryProperties mp;
static uint32_t find_type(uint32_t bits, VkMemoryPropertyFlags f)
{
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & f) == f) return i;
    return UINT32_MAX;
}

static const VkSpecializationMapEntry marker_entry = {0x7E57C0DEu, 0, 4}; /* server_guard.c */
static const uint32_t marker_value = 1;
static const VkSpecializationInfo marker = {1, &marker_entry, 4, &marker_value};

int main(int argc, char **argv)
{
    int mark = argc > 1 && !strcmp(argv[1], "--mark"), lut = argc > 1 && !strcmp(argv[1], "--lut");
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib) return printf("FAIL loader\n"), 2;
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    vkCreateInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "gsfold_check", 1, "vkbridge", 1, VK_API_VERSION_1_2};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 0, NULL};
    VkInstance inst;
    if (vkCreateInstance(&ici, NULL, &inst)) return printf("FAIL instance\n"), 1;
#define G(n) n = (PFN_##n)gipa(inst, #n)
    G(vkEnumeratePhysicalDevices); G(vkGetPhysicalDeviceQueueFamilyProperties); G(vkGetPhysicalDeviceMemoryProperties);
    G(vkGetPhysicalDeviceFeatures2); G(vkCreateDevice); G(vkGetDeviceProcAddr);
#undef G
    uint32_t n = 1;
    VkPhysicalDevice pd;
    vkEnumeratePhysicalDevices(inst, &n, &pd);
    if (!n) return printf("FAIL no device\n"), 1;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f12};
    vkGetPhysicalDeviceFeatures2(pd, &f2);
    if (!f2.features.geometryShader) return printf("OK: skipped (no geometry shaders)\n"), 0;
    VkPhysicalDeviceVulkan12Features e12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    e12.shaderOutputLayer = f12.shaderOutputLayer; /* as DXVK does where the GPU has it */
    VkPhysicalDeviceFeatures2 ef = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &e12};
    ef.features.geometryShader = VK_TRUE;
    uint32_t nq = 0;
    VkQueueFamilyProperties qp[16];
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, NULL);
    if (nq > 16) nq = 16;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qp);
    uint32_t qf = 0;
    while (qf < nq && !(qp[qf].queueFlags & VK_QUEUE_GRAPHICS_BIT)) qf++;
    float prio = 1;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, qf, 1, &prio};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &ef, 0, 1, &qci, 0, NULL, 0, NULL, NULL};
    VkDevice dev;
    if (vkCreateDevice(pd, &dci, NULL, &dev)) return printf("FAIL device\n"), 1;
#define D(n) n = (PFN_##n)vkGetDeviceProcAddr(dev, #n)
    D(vkGetDeviceQueue); D(vkCreateImage); D(vkGetImageMemoryRequirements); D(vkAllocateMemory); D(vkBindImageMemory);
    D(vkCreateImageView); D(vkCreateRenderPass); D(vkCreateFramebuffer); D(vkCreateShaderModule); D(vkCreatePipelineLayout);
    D(vkCreateGraphicsPipelines); D(vkCreateCommandPool); D(vkAllocateCommandBuffers); D(vkBeginCommandBuffer); D(vkCmdBeginRenderPass);
    D(vkCmdBindPipeline); D(vkCmdPushConstants); D(vkCmdDraw); D(vkCmdEndRenderPass); D(vkCmdCopyImageToBuffer); D(vkEndCommandBuffer);
    D(vkQueueSubmit); D(vkQueueWaitIdle); D(vkCreateBuffer); D(vkGetBufferMemoryRequirements); D(vkBindBufferMemory); D(vkMapMemory);
    D(vkCmdSetViewport); D(vkCmdSetScissor); D(vkDestroyDevice);
#undef D
    VkQueue q;
    vkGetDeviceQueue(dev, qf, 0, &q);

    VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {W, H, 1}, 1, 2,
                            VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                            VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
    VkImage img;
    vkCreateImage(dev, &ii, NULL, &img);
    VkMemoryRequirements r;
    vkGetImageMemoryRequirements(dev, img, &r);
    uint32_t t = find_type(r.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (t == UINT32_MAX) t = find_type(r.memoryTypeBits, 0);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, r.size, t};
    VkDeviceMemory im;
    vkAllocateMemory(dev, &ai, NULL, &im);
    vkBindImageMemory(dev, img, im, 0);
    VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, img, VK_IMAGE_VIEW_TYPE_2D_ARRAY, VK_FORMAT_R8G8B8A8_UNORM,
                                 {0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 2}};
    VkImageView view;
    vkCreateImageView(dev, &vci, NULL, &view);
    VkAttachmentDescription att = {0, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
                                   VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED,
                                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL};
    VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub = {0, VK_PIPELINE_BIND_POINT_GRAPHICS, 0, NULL, 1, &ref, NULL, NULL, 0, NULL};
    VkSubpassDependency dep = {0, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, 0};
    VkRenderPassCreateInfo rpci = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, NULL, 0, 1, &att, 1, &sub, 1, &dep};
    VkRenderPass rp;
    vkCreateRenderPass(dev, &rpci, NULL, &rp);
    VkFramebufferCreateInfo fci = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, NULL, 0, rp, 1, &view, W, H, 2};
    VkFramebuffer fb;
    vkCreateFramebuffer(dev, &fci, NULL, &fb);

    VkShaderModule mods[3];
    const uint32_t *codes[3] = {vert_spv, geom_spv, lut ? lut_spv : frag_spv};
    size_t sizes[3] = {sizeof(vert_spv), sizeof(geom_spv), lut ? sizeof(lut_spv) : sizeof(frag_spv)};
    for (int i = 0; i < 3; i++) {
        VkShaderModuleCreateInfo smci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizes[i], codes[i]};
        vkCreateShaderModule(dev, &smci, NULL, &mods[i]);
    }
    VkPushConstantRange pcr = {VK_SHADER_STAGE_VERTEX_BIT, 0, 4};
    VkPipelineLayoutCreateInfo plci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 0, NULL, 1, &pcr};
    VkPipelineLayout pl;
    vkCreatePipelineLayout(dev, &plci, NULL, &pl);
    VkPipelineShaderStageCreateInfo st[3] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, mods[0], "main", NULL},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_GEOMETRY_BIT, mods[1], "main", mark ? &marker : NULL},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, mods[2], "main", lut ? &marker : NULL}};
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 0};
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL, 0, 1, NULL, 1, NULL};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, NULL, 0, VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState cba = {0};
    cba.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cbs = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, NULL, 0, 0, 0, 1, &cba, {0}};
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, NULL, 0, 2, dyn};
    VkGraphicsPipelineCreateInfo gci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, NULL, 0, 3, st, &vi, &ia, NULL, &vp, &rs, &ms, NULL, &cbs, &ds,
                                        pl, rp, 0, VK_NULL_HANDLE, -1};
    VkPipeline pipe;
    VkResult pr = vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gci, NULL, &pipe);
    if (pr) return printf("FAIL pipeline: %d\n", pr), 1;

    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, W * H * 4 * 2, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
    VkBuffer buf;
    vkCreateBuffer(dev, &bci, NULL, &buf);
    vkGetBufferMemoryRequirements(dev, buf, &r);
    ai.allocationSize = r.size;
    ai.memoryTypeIndex = find_type(r.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkDeviceMemory bm;
    vkAllocateMemory(dev, &ai, NULL, &bm);
    vkBindBufferMemory(dev, buf, bm, 0);
    uint8_t *px;
    vkMapMemory(dev, bm, 0, VK_WHOLE_SIZE, 0, (void **)&px);
    VkCommandPoolCreateInfo cpci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, qf};
    VkCommandPool pool;
    vkCreateCommandPool(dev, &cpci, NULL, &pool);
    VkCommandBufferAllocateInfo cbai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VkCommandBuffer cb;
    vkAllocateCommandBuffers(dev, &cbai, &cb);
    int fails = 0;
    for (int slice = 1; slice >= 0; slice--) {
        VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        vkBeginCommandBuffer(cb, &bi);
        VkClearValue clear = {{{0, 0, 0, 0}}};
        VkRenderPassBeginInfo rbi = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, NULL, rp, fb, {{0, 0}, {W, H}}, 1, &clear};
        vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        VkViewport v = {0, 0, W, H, 0, 1};
        VkRect2D sc = {{0, 0}, {W, H}};
        vkCmdSetViewport(cb, 0, 1, &v);
        vkCmdSetScissor(cb, 0, 1, &sc);
        vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_VERTEX_BIT, 0, 4, &slice);
        vkCmdDraw(cb, 3, 1, 0, 0);
        vkCmdEndRenderPass(cb);
        VkBufferImageCopy c = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 2}, {0, 0, 0}, {W, H, 1}};
        vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &c);
        vkEndCommandBuffer(cb);
        VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cb, 0, NULL};
        vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE);
        vkQueueWaitIdle(q);
        for (int layer = 0; layer < 2; layer++) {
            const uint8_t *p = px + (size_t)layer * W * H * 4 + ((H / 4) * W + W / 4) * 4; /* inside the triangle */
            int drawn = p[0] > 50 && p[0] < 80 && p[1] > 115 && p[1] < 140 && p[2] > 180 && p[2] < 200;
            if (lut) {
                /* the bridge's neutral LUT stand-in (server/shaders/ue4_lut_standin.frag) at this texel */
                float uv = (W / 4 + 0.5f) / W, n[3] = {(uv - 0.5f / 32) * (32.0f / 31), (uv - 0.5f / 32) * (32.0f / 31), slice / 31.0f};
                int good = 1;
                for (int c = 0; c < 3; c++) {
                    float l = fmaxf(exp2f((n[c] - 0.434017599f) * 14.0f) * 0.18f - 0.00266771927f, 0.0f);
                    float t = fminf(fmaxf(l * (2.51f * l + 0.03f) / (l * (2.43f * l + 0.59f) + 0.14f), 0.0f), 1.0f);
                    float e = (t < 0.0031308f ? t * 12.92f : 1.055f * powf(t, 1.0f / 2.4f) - 0.055f) / 1.05f;
                    good &= abs((int)p[c] - (int)lrintf(e * 255.0f)) <= 2;
                }
                drawn = layer == slice ? good : (p[0] | p[1] | p[2]) != 0;
            }
            int want = layer == slice;
            printf("%s slice %d: layer %d %s (pixel %u %u %u %u)\n", drawn == want ? "PASS" : "FAIL", slice, layer,
                   drawn ? "drawn" : "empty", p[0], p[1], p[2], p[3]);
            fails += drawn != want;
        }
    }
    vkDestroyDevice(dev, NULL);
    printf("%s: %d of 4 checks passed\n", fails ? "FAILED" : "OK", 4 - fails);
    return fails ? 1 : 0;
}
