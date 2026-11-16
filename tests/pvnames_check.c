/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * pvnames_check: PV-01 (PowerVR: an input reads zero beside an output of its OpName). The probe's
 * shaders (server/shaders/probe/pvnames.vert: the vertex input TEXCOORD and the output that
 * passes it on carry one OpName, as DXVK writes a passed-through attribute) draw one triangle over
 * an 8x1 target whose colour is that attribute, (0.25, 0.5, 0.75): the pixel must read
 * (64, 128, 191). host/pvnames_test.sh runs it through the bridge with the bug imitated
 * (VKBRIDGE_ERRATA_IMITATE=pv_names: fails with the colour black), the fix over it (passes) and
 * the probe's decision alone.
 *   pvnames_check [--inline]   --inline: the modules chained to the pipeline's stages (maintenance5)
 */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint32_t vs_spv[] = {
#include "pvnames_vert.inc"
};
static const uint32_t fs_spv[] = {
#include "pvnames_frag.inc"
};

#define FAIL(...) (printf("FAIL: " __VA_ARGS__), printf("\n"), 1)

int main(int argc, char **argv)
{
    int inline_mod = argc > 1 && !strcmp(argv[1], "--inline");
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib) return FAIL("no libvulkan.so.1");
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
#define GI(n) PFN_##n n = (PFN_##n)gipa(inst, #n)
    VkInstance inst = VK_NULL_HANDLE;
    GI(vkCreateInstance);
    VkApplicationInfo ai = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "pvnames_check", 1, NULL, 0, VK_API_VERSION_1_1};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &ai};
    if (vkCreateInstance(&ici, NULL, &inst)) return FAIL("instance");
    GI(vkEnumeratePhysicalDevices);
    GI(vkGetPhysicalDeviceMemoryProperties);
    GI(vkEnumerateDeviceExtensionProperties);
    GI(vkCreateDevice);
    GI(vkGetDeviceProcAddr);
    uint32_t n = 1;
    VkPhysicalDevice pd;
    if (vkEnumeratePhysicalDevices(inst, &n, &pd) < 0 || !n) return FAIL("no physical device");
    const char *exts[1] = {VK_KHR_MAINTENANCE_5_EXTENSION_NAME};
    VkPhysicalDeviceMaintenance5FeaturesKHR m5 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR, NULL, VK_TRUE};
    if (inline_mod) {
        uint32_t ne = 0;
        vkEnumerateDeviceExtensionProperties(pd, NULL, &ne, NULL);
        VkExtensionProperties *ep = calloc(ne ? ne : 1, sizeof(*ep));
        vkEnumerateDeviceExtensionProperties(pd, NULL, &ne, ep);
        int has = 0;
        for (uint32_t i = 0; i < ne; i++) has |= !strcmp(ep[i].extensionName, exts[0]);
        free(ep);
        if (!has) {
            printf("SKIP: no VK_KHR_maintenance5 for inline modules\n");
            return 0;
        }
    }
    float prio = 1;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, 0, 1, &prio};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, inline_mod ? &m5 : NULL, 0, 1, &qci};
    if (inline_mod) dci.enabledExtensionCount = 1, dci.ppEnabledExtensionNames = exts;
    VkDevice dev;
    if (vkCreateDevice(pd, &dci, NULL, &dev)) return FAIL("device");
#define GD(n) PFN_##n n = (PFN_##n)vkGetDeviceProcAddr(dev, #n)
    GD(vkGetDeviceQueue); GD(vkCreateShaderModule); GD(vkCreatePipelineLayout); GD(vkCreateGraphicsPipelines);
    GD(vkCreateImage); GD(vkGetImageMemoryRequirements); GD(vkCreateImageView); GD(vkCreateRenderPass); GD(vkCreateFramebuffer);
    GD(vkCreateBuffer); GD(vkGetBufferMemoryRequirements); GD(vkAllocateMemory); GD(vkBindBufferMemory); GD(vkBindImageMemory);
    GD(vkMapMemory); GD(vkCreateCommandPool); GD(vkAllocateCommandBuffers); GD(vkBeginCommandBuffer); GD(vkEndCommandBuffer);
    GD(vkCmdBeginRenderPass); GD(vkCmdEndRenderPass); GD(vkCmdBindPipeline); GD(vkCmdBindVertexBuffers); GD(vkCmdDraw);
    GD(vkCmdCopyImageToBuffer); GD(vkCmdPipelineBarrier); GD(vkQueueSubmit); GD(vkQueueWaitIdle);

    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    VkMemoryPropertyFlags hv = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
#define MEMORY(req, flags, memp)                                                                                          \
    do {                                                                                                                  \
        uint32_t ty = UINT32_MAX;                                                                                         \
        for (uint32_t i = 0; i < mp.memoryTypeCount && ty == UINT32_MAX; i++)                                             \
            if (((req).memoryTypeBits >> i & 1) && (mp.memoryTypes[i].propertyFlags & (flags)) == (flags)) ty = i;        \
        VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, (req).size, ty};                        \
        if (ty == UINT32_MAX || vkAllocateMemory(dev, &mai, NULL, (memp))) return FAIL("memory");                        \
    } while (0)

    /* the vertex buffer: three vertices of (0.25, 0.5, 0.75, 1) */
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, 64, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT};
    VkBuffer vb, rb;
    VkMemoryRequirements mr;
    VkDeviceMemory vm, rm;
    float *vmap;
    uint8_t *rmap;
    if (vkCreateBuffer(dev, &bci, NULL, &vb)) return FAIL("buffer");
    vkGetBufferMemoryRequirements(dev, vb, &mr);
    MEMORY(mr, hv, &vm);
    if (vkBindBufferMemory(dev, vb, vm, 0) || vkMapMemory(dev, vm, 0, VK_WHOLE_SIZE, 0, (void **)&vmap)) return FAIL("vertex memory");
    for (int i = 0; i < 3; i++) vmap[4 * i] = 0.25f, vmap[4 * i + 1] = 0.5f, vmap[4 * i + 2] = 0.75f, vmap[4 * i + 3] = 1.0f;
    bci.size = 64, bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (vkCreateBuffer(dev, &bci, NULL, &rb)) return FAIL("buffer");
    vkGetBufferMemoryRequirements(dev, rb, &mr);
    MEMORY(mr, hv, &rm);
    if (vkBindBufferMemory(dev, rb, rm, 0) || vkMapMemory(dev, rm, 0, VK_WHOLE_SIZE, 0, (void **)&rmap)) return FAIL("readback memory");
    memset(rmap, 0xEE, 32);

    /* the 8x1 target and its pass */
    VkImageCreateInfo ici2 = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {8, 1, 1}, 1, 1,
                              VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL,
                              VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL,
                              VK_IMAGE_LAYOUT_UNDEFINED};
    VkImage img;
    VkDeviceMemory im;
    VkImageView iv;
    if (vkCreateImage(dev, &ici2, NULL, &img)) return FAIL("image");
    vkGetImageMemoryRequirements(dev, img, &mr);
    MEMORY(mr, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &im);
    VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, img, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM,
                                 {0, 0, 0, 0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (vkBindImageMemory(dev, img, im, 0) || vkCreateImageView(dev, &vci, NULL, &iv)) return FAIL("image view");
    VkAttachmentDescription ad = {0, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
                                  VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED,
                                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL};
    VkAttachmentReference ar = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sd = {0, VK_PIPELINE_BIND_POINT_GRAPHICS, 0, NULL, 1, &ar};
    VkSubpassDependency dep = {0, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, 0};
    VkRenderPassCreateInfo rpi = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, NULL, 0, 1, &ad, 1, &sd, 1, &dep};
    VkRenderPass rp;
    VkFramebuffer fb;
    VkFramebufferCreateInfo fbi = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, NULL, 0, 0, 1, &iv, 8, 1, 1};
    if (vkCreateRenderPass(dev, &rpi, NULL, &rp) || (fbi.renderPass = rp, vkCreateFramebuffer(dev, &fbi, NULL, &fb))) return FAIL("render pass");

    /* the pipeline: the modules made first, or chained to the stages (maintenance5) */
    VkShaderModuleCreateInfo smv = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof(vs_spv), vs_spv};
    VkShaderModuleCreateInfo smf = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof(fs_spv), fs_spv};
    VkShaderModule vsm = VK_NULL_HANDLE, fsm = VK_NULL_HANDLE;
    if (!inline_mod && (vkCreateShaderModule(dev, &smv, NULL, &vsm) || vkCreateShaderModule(dev, &smf, NULL, &fsm))) return FAIL("modules");
    VkPipelineShaderStageCreateInfo st[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, inline_mod ? &smv : NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, vsm, "main", NULL},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, inline_mod ? &smf : NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fsm, "main", NULL}};
    VkVertexInputBindingDescription bd = {0, 16, VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription va = {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0};
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, NULL, 0, 1, &bd, 1, &va};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0,
                                                 VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 0};
    VkViewport vp = {0, 0, 8, 1, 0, 1};
    VkRect2D sc = {{0, 0}, {8, 1}};
    VkPipelineViewportStateCreateInfo vps = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL, 0, 1, &vp, 1, &sc};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, NULL, 0, 0, 0, VK_POLYGON_MODE_FILL,
                                                 VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE, 0, 0, 0, 0, 1.0f};
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, NULL, 0, VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState ba = {0, 0, 0, 0, 0, 0, 0, 0xf};
    VkPipelineColorBlendStateCreateInfo cbs = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, NULL, 0, 0, 0, 1, &ba};
    VkPipelineLayoutCreateInfo plci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    VkPipelineLayout pl;
    if (vkCreatePipelineLayout(dev, &plci, NULL, &pl)) return FAIL("pipeline layout");
    VkGraphicsPipelineCreateInfo gpi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, NULL, 0, 2, st, &vi, &ia, NULL, &vps, &rs, &ms, NULL,
                                        &cbs, NULL, pl, rp, 0, VK_NULL_HANDLE, -1};
    VkPipeline pipe;
    if (vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpi, NULL, &pipe)) return FAIL("pipeline");

    VkCommandPoolCreateInfo cpi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, 0, 0};
    VkCommandPool cp;
    VkCommandBuffer cb;
    VkCommandBufferAllocateInfo cbai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, 0, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    if (vkCreateCommandPool(dev, &cpi, NULL, &cp) || (cbai.commandPool = cp, vkAllocateCommandBuffers(dev, &cbai, &cb))) return FAIL("command buffer");
    VkCommandBufferBeginInfo cbbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(cb, &cbbi);
    VkClearValue cv = {{{0, 0, 0, 0}}};
    VkRenderPassBeginInfo rbi = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, NULL, rp, fb, {{0, 0}, {8, 1}}, 1, &cv};
    vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    VkDeviceSize off = 0;
    vkCmdBindVertexBuffers(cb, 0, 1, &vb, &off);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRenderPass(cb);
    VkBufferImageCopy bic = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {8, 1, 1}};
    vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb, 1, &bic);
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    vkEndCommandBuffer(cb);
    VkQueue q;
    vkGetDeviceQueue(dev, 0, 0, &q);
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cb};
    if (vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE) || vkQueueWaitIdle(q)) return FAIL("submit");

    const uint8_t *px = rmap + 3 * 4;
    int ok = px[0] >= 62 && px[0] <= 66 && px[1] >= 126 && px[1] <= 130 && px[2] >= 189 && px[2] <= 193 && px[3] == 255;
    if (!ok) return FAIL("pixel (%u %u %u %u), want (64 128 191 255)", px[0], px[1], px[2], px[3]);
    printf("PASS: pixel (%u %u %u %u)%s\n", px[0], px[1], px[2], px[3], inline_mod ? ", inline modules" : "");
    return 0;
}
