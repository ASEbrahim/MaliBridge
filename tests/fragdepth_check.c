/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * fragdepth_check: MK-49 (server_errata_fragdepth.c). A full-screen triangle whose fragment shader writes 0.25 to its colour
 * output and 0.75 to gl_FragDepth (tests/fragdepth.frag), drawn with dynamic rendering into a 4x4 R32_SFLOAT image:
 *   fragdepth_check color   no depth attachment (vkd3d-proton's layered copy on RE Village / The Witcher 3): the colour must be 0.25;
 *   fragdepth_check depth   with a D32_SFLOAT attachment (compare ALWAYS, writes on): colour 0.25 and depth 0.75, so a pipeline
 *                           with a depth target keeps its depth output.
 * Prints one PASS/FAIL line; host/fragdepth_test.sh runs it with the imitation and the fix.
 */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint32_t vs_spv[] = {
#include "fragdepth_vert.inc"
};
static const uint32_t fs_spv[] = {
#include "fragdepth_frag.inc"
};

#define W 4
#define H 4

static PFN_vkGetDeviceProcAddr gdpa;
static VkDevice dev;
#define GD(n) PFN_##n n = (PFN_##n)gdpa(dev, #n)

static uint32_t mem_type(const VkPhysicalDeviceMemoryProperties *mp, uint32_t bits, VkMemoryPropertyFlags want)
{
    for (uint32_t i = 0; i < mp->memoryTypeCount; i++)
        if ((bits >> i & 1) && (mp->memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

int main(int argc, char **argv)
{
    int with_depth = argc > 1 && !strcmp(argv[1], "depth");
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib) {
        printf("FAIL: no libvulkan.so.1\n");
        return 1;
    }
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
#define GI(n) PFN_##n n = (PFN_##n)gipa(inst, #n)
    VkInstance inst = VK_NULL_HANDLE;
    GI(vkCreateInstance);
    VkApplicationInfo ai = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "fragdepth_check", 1, NULL, 0, VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &ai};
    if (vkCreateInstance(&ici, NULL, &inst)) {
        printf("FAIL: instance\n");
        return 1;
    }
    GI(vkEnumeratePhysicalDevices);
    GI(vkGetPhysicalDeviceMemoryProperties);
    GI(vkGetPhysicalDeviceProperties);
    GI(vkCreateDevice);
    GI(vkGetDeviceProcAddr);
    GI(vkDestroyInstance);
    gdpa = vkGetDeviceProcAddr;
    uint32_t n = 1;
    VkPhysicalDevice pd;
    if (vkEnumeratePhysicalDevices(inst, &n, &pd) < 0 || !n) {
        printf("FAIL: no physical device\n");
        return 1;
    }
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(pd, &props);
    if (props.apiVersion < VK_API_VERSION_1_3) {
        printf("SKIP: Vulkan 1.3 (dynamic rendering) not offered\n");
        return 0;
    }
    VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    f13.dynamicRendering = VK_TRUE;
    float prio = 1;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, 0, 1, &prio};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f13, 0, 1, &qci};
    if (vkCreateDevice(pd, &dci, NULL, &dev)) {
        printf("FAIL: device\n");
        return 1;
    }
    GD(vkDestroyDevice); GD(vkGetDeviceQueue); GD(vkCreateShaderModule); GD(vkDestroyShaderModule); GD(vkCreatePipelineLayout);
    GD(vkDestroyPipelineLayout); GD(vkCreateGraphicsPipelines); GD(vkDestroyPipeline); GD(vkCreateImage); GD(vkDestroyImage);
    GD(vkGetImageMemoryRequirements); GD(vkAllocateMemory); GD(vkFreeMemory); GD(vkBindImageMemory); GD(vkCreateImageView);
    GD(vkDestroyImageView); GD(vkCreateBuffer); GD(vkDestroyBuffer); GD(vkGetBufferMemoryRequirements); GD(vkBindBufferMemory);
    GD(vkMapMemory); GD(vkCreateCommandPool); GD(vkDestroyCommandPool); GD(vkAllocateCommandBuffers); GD(vkBeginCommandBuffer);
    GD(vkEndCommandBuffer); GD(vkCmdPipelineBarrier); GD(vkCmdBeginRendering); GD(vkCmdEndRendering); GD(vkCmdBindPipeline);
    GD(vkCmdSetViewport); GD(vkCmdSetScissor); GD(vkCmdDraw); GD(vkCmdCopyImageToBuffer); GD(vkQueueSubmit); GD(vkQueueWaitIdle);
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);

    /* images: colour R32_SFLOAT, depth D32_SFLOAT (depth mode) */
    VkFormat fmt[2] = {VK_FORMAT_R32_SFLOAT, VK_FORMAT_D32_SFLOAT};
    VkImageUsageFlags use[2] = {VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
    VkImageAspectFlags asp[2] = {VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_ASPECT_DEPTH_BIT};
    VkImage img[2] = {0};
    VkDeviceMemory imem[2] = {0};
    VkImageView view[2] = {0};
    int nimg = with_depth ? 2 : 1;
    for (int i = 0; i < nimg; i++) {
        VkImageCreateInfo ic = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, fmt[i], {W, H, 1}, 1, 1,
                                VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL, use[i], VK_SHARING_MODE_EXCLUSIVE, 0, NULL,
                                VK_IMAGE_LAYOUT_UNDEFINED};
        VkMemoryRequirements mr;
        if (vkCreateImage(dev, &ic, NULL, &img[i])) {
            printf("FAIL: image %d\n", i);
            return 1;
        }
        vkGetImageMemoryRequirements(dev, img[i], &mr);
        VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, mem_type(&mp, mr.memoryTypeBits, 0)};
        VkImageViewCreateInfo vc = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, img[i], VK_IMAGE_VIEW_TYPE_2D, fmt[i], {0},
                                    {asp[i], 0, 1, 0, 1}};
        if (vkAllocateMemory(dev, &mai, NULL, &imem[i]) || vkBindImageMemory(dev, img[i], imem[i], 0) ||
            vkCreateImageView(dev, &vc, NULL, &view[i])) {
            printf("FAIL: image memory/view %d\n", i);
            return 1;
        }
    }
    /* read-back buffer: colour then depth */
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, 2 * W * H * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VkBuffer buf;
    VkDeviceMemory bmem;
    VkMemoryRequirements bmr;
    float *map;
    if (vkCreateBuffer(dev, &bci, NULL, &buf)) {
        printf("FAIL: buffer\n");
        return 1;
    }
    vkGetBufferMemoryRequirements(dev, buf, &bmr);
    VkMemoryAllocateInfo bmai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, bmr.size,
                                 mem_type(&mp, bmr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    if (vkAllocateMemory(dev, &bmai, NULL, &bmem) || vkBindBufferMemory(dev, buf, bmem, 0) ||
        vkMapMemory(dev, bmem, 0, VK_WHOLE_SIZE, 0, (void **)&map)) {
        printf("FAIL: buffer memory\n");
        return 1;
    }
    memset(map, 0, 2 * W * H * 4);

    /* pipeline: dynamic rendering, colour R32_SFLOAT (+ D32 in depth mode) */
    VkShaderModuleCreateInfo vmi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof(vs_spv), vs_spv};
    VkShaderModuleCreateInfo fmi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof(fs_spv), fs_spv};
    VkShaderModule vsm, fsm;
    VkPipelineLayoutCreateInfo plci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    VkPipelineLayout pl;
    if (vkCreateShaderModule(dev, &vmi, NULL, &vsm) || vkCreateShaderModule(dev, &fmi, NULL, &fsm) || vkCreatePipelineLayout(dev, &plci, NULL, &pl)) {
        printf("FAIL: modules/layout\n");
        return 1;
    }
    VkPipelineShaderStageCreateInfo st[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, vsm, "main", NULL},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fsm, "main", NULL}};
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL, 0, 1, NULL, 1, NULL};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, NULL, 0, VK_SAMPLE_COUNT_1_BIT};
    VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = ds.depthWriteEnable = with_depth ? VK_TRUE : VK_FALSE;
    ds.depthCompareOp = VK_COMPARE_OP_ALWAYS;
    VkPipelineColorBlendAttachmentState cba = {0};
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT;
    VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, NULL, 0, VK_FALSE, 0, 1, &cba};
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, NULL, 0, 2, dyn};
    VkPipelineRenderingCreateInfo pri = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, NULL, 0, 1, &fmt[0],
                                         with_depth ? VK_FORMAT_D32_SFLOAT : VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED};
    VkGraphicsPipelineCreateInfo gp = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &pri, 0, 2, st, &vi, &ia, NULL, &vp, &rs, &ms, &ds, &cb, &dy, pl};
    VkPipeline pipe;
    VkResult pr = vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gp, NULL, &pipe);
    if (pr) {
        printf("FAIL: pipeline (%d) (%s)\n", pr, with_depth ? "depth" : "color");
        return 1;
    }

    VkCommandPoolCreateInfo cpi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, 0, 0};
    VkCommandPool cp;
    VkCommandBuffer cmd;
    VkCommandBufferAllocateInfo cbai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, 0, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    if (vkCreateCommandPool(dev, &cpi, NULL, &cp) || (cbai.commandPool = cp, vkAllocateCommandBuffers(dev, &cbai, &cmd))) {
        printf("FAIL: command buffer\n");
        return 1;
    }
    VkCommandBufferBeginInfo cbbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(cmd, &cbbi);
    VkImageMemoryBarrier to[2];
    VkImageLayout att[2] = {VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL};
    for (int i = 0; i < nimg; i++)
        to[i] = (VkImageMemoryBarrier){VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, 0,
                                       i ? VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT : VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                                       att[i], VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, img[i], {asp[i], 0, 1, 0, 1}};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, 0, 0, NULL, 0, NULL, (uint32_t)nimg, to);
    VkRenderingAttachmentInfo ca = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, NULL, view[0], att[0]};
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingAttachmentInfo da = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, NULL, view[1], att[1]};
    da.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    da.clearValue.depthStencil.depth = 0.0f;
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO, NULL, 0, {{0, 0}, {W, H}}, 1, 0, 1, &ca, with_depth ? &da : NULL, NULL};
    vkCmdBeginRendering(cmd, &ri);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    VkViewport v = {0, 0, W, H, 0, 1};
    VkRect2D sc = {{0, 0}, {W, H}};
    vkCmdSetViewport(cmd, 0, 1, &v);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
    VkImageMemoryBarrier tr[2];
    for (int i = 0; i < nimg; i++)
        tr[i] = (VkImageMemoryBarrier){VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL,
                                       i ? VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT : VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                                       VK_ACCESS_TRANSFER_READ_BIT, att[i], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_QUEUE_FAMILY_IGNORED,
                                       VK_QUEUE_FAMILY_IGNORED, img[i], {asp[i], 0, 1, 0, 1}};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, (uint32_t)nimg, tr);
    for (int i = 0; i < nimg; i++) {
        VkBufferImageCopy rc = {(VkDeviceSize)i * W * H * 4, 0, 0, {asp[i], 0, 0, 1}, {0, 0, 0}, {W, H, 1}};
        vkCmdCopyImageToBuffer(cmd, img[i], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &rc);
    }
    vkEndCommandBuffer(cmd);
    VkQueue q;
    vkGetDeviceQueue(dev, 0, 0, &q);
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cmd};
    if (vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE) || vkQueueWaitIdle(q)) {
        printf("FAIL: submit\n");
        return 1;
    }
    float c0 = map[W * H / 2 + 1], d0 = with_depth ? map[W * H + W * H / 2 + 1] : 0.75f;
    vkDestroyCommandPool(dev, cp, NULL);
    vkDestroyPipeline(dev, pipe, NULL);
    vkDestroyPipelineLayout(dev, pl, NULL);
    vkDestroyShaderModule(dev, vsm, NULL);
    vkDestroyShaderModule(dev, fsm, NULL);
    for (int i = 0; i < nimg; i++) vkDestroyImageView(dev, view[i], NULL), vkDestroyImage(dev, img[i], NULL), vkFreeMemory(dev, imem[i], NULL);
    vkDestroyBuffer(dev, buf, NULL);
    vkFreeMemory(dev, bmem, NULL);
    vkDestroyDevice(dev, NULL);
    vkDestroyInstance(inst, NULL);
    if (c0 != 0.25f || d0 != 0.75f) {
        printf("FAIL: colour %g (want 0.25), depth %g (want 0.75) (%s)\n", c0, d0, with_depth ? "depth" : "color");
        return 1;
    }
    printf("PASS: colour 0.25%s (%s)\n", with_depth ? ", depth 0.75" : "", with_depth ? "depth" : "color");
    return 0;
}
