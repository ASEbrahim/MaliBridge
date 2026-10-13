/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * clip_pipeline: builds a graphics pipeline vs [+ gs] + fs from SPIR-V files, draws a quad over a
 * 16x16 RGBA8 target whose shaders set gl_ClipDistance[0] = x (corpus/clip_*.spvasm), and checks
 * that the left half stayed clear and the right half took the fragment shader's colour
 * (255, 64, 191, 255). Natively the GPU clips; through a bridge whose server emulates clip
 * distances (VKBRIDGE_FAKE_MISSING=clip,cull) the fragment discard does. Driven by clip_check.sh.
 *
 *   clip_pipeline vs.spv gs.spv|- fs.spv
 */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 16

static VkDevice dev;
static VkPhysicalDeviceMemoryProperties mp;
#define F(n) static PFN_##n n
F(vkCreateInstance); F(vkEnumeratePhysicalDevices); F(vkGetPhysicalDeviceMemoryProperties); F(vkCreateDevice);
F(vkGetDeviceQueue); F(vkCreateCommandPool); F(vkAllocateCommandBuffers); F(vkBeginCommandBuffer);
F(vkEndCommandBuffer); F(vkQueueSubmit); F(vkQueueWaitIdle); F(vkCreateBuffer); F(vkGetBufferMemoryRequirements);
F(vkAllocateMemory); F(vkBindBufferMemory); F(vkMapMemory); F(vkCreateImage); F(vkGetImageMemoryRequirements);
F(vkBindImageMemory); F(vkCreateImageView); F(vkCreateRenderPass); F(vkCreateFramebuffer); F(vkCreateShaderModule);
F(vkCreatePipelineLayout); F(vkCreateGraphicsPipelines); F(vkCmdBeginRenderPass); F(vkCmdBindPipeline);
F(vkCmdDraw); F(vkCmdEndRenderPass); F(vkCmdCopyImageToBuffer); F(vkGetPhysicalDeviceProperties);
F(vkGetPhysicalDeviceFeatures);
#undef F

static VkDeviceMemory bind_mem(VkMemoryRequirements r, VkMemoryPropertyFlags want)
{
    uint32_t t = 0;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((r.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) {
            t = i;
            break;
        }
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, r.size, t};
    VkDeviceMemory m;
    vkAllocateMemory(dev, &ai, NULL, &m);
    return m;
}

static VkShaderModule module(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return VK_NULL_HANDLE;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    uint32_t *code = malloc((size_t)sz);
    size_t got = fread(code, 1, (size_t)sz, f);
    fclose(f);
    VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, got, code};
    VkShaderModule m = VK_NULL_HANDLE;
    if (got != (size_t)sz || vkCreateShaderModule(dev, &ci, NULL, &m) != VK_SUCCESS) m = VK_NULL_HANDLE;
    free(code);
    return m;
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: %s vs.spv gs.spv|- fs.spv\n", argv[0]);
        return 2;
    }
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib) return 2;
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    vkCreateInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "clip_pipeline", 1, NULL, 0, VK_API_VERSION_1_1};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 0, NULL};
    VkInstance inst;
    if (vkCreateInstance(&ici, NULL, &inst) != VK_SUCCESS) return 2;
#define L(n) n = (PFN_##n)gipa(inst, #n)
    L(vkEnumeratePhysicalDevices); L(vkGetPhysicalDeviceMemoryProperties); L(vkCreateDevice); L(vkGetDeviceQueue);
    L(vkCreateCommandPool); L(vkAllocateCommandBuffers); L(vkBeginCommandBuffer); L(vkEndCommandBuffer);
    L(vkQueueSubmit); L(vkQueueWaitIdle); L(vkCreateBuffer); L(vkGetBufferMemoryRequirements); L(vkAllocateMemory);
    L(vkBindBufferMemory); L(vkMapMemory); L(vkCreateImage); L(vkGetImageMemoryRequirements); L(vkBindImageMemory);
    L(vkCreateImageView); L(vkCreateRenderPass); L(vkCreateFramebuffer); L(vkCreateShaderModule);
    L(vkCreatePipelineLayout); L(vkCreateGraphicsPipelines); L(vkCmdBeginRenderPass); L(vkCmdBindPipeline);
    L(vkCmdDraw); L(vkCmdEndRenderPass); L(vkCmdCopyImageToBuffer); L(vkGetPhysicalDeviceProperties);
    L(vkGetPhysicalDeviceFeatures);
#undef L
    uint32_t n = 1;
    VkPhysicalDevice pd;
    if (vkEnumeratePhysicalDevices(inst, &n, &pd) < 0 || n == 0) return 2;
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(pd, &props);
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    float prio = 1;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, 0, 1, &prio};
    int gs = strcmp(argv[2], "-") != 0;
    VkPhysicalDeviceFeatures have, feat = {0};
    vkGetPhysicalDeviceFeatures(pd, &have);
    feat.geometryShader = gs;
    /* What the device has: a bridge emulating clip/cull reports neither. */
    feat.shaderClipDistance = have.shaderClipDistance;
    feat.shaderCullDistance = have.shaderCullDistance;
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &qci, 0, NULL, 0, NULL, &feat};
    if (vkCreateDevice(pd, &dci, NULL, &dev) != VK_SUCCESS) {
        printf("FAILED: no device%s\n", gs ? " with geometry shaders" : "");
        return 1;
    }
    VkQueue q;
    vkGetDeviceQueue(dev, 0, 0, &q);

    VkImageCreateInfo imci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM,
                              {W, W, 1}, 1, 1, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL,
                              VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_SHARING_MODE_EXCLUSIVE,
                              0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
    VkImage img;
    vkCreateImage(dev, &imci, NULL, &img);
    VkMemoryRequirements r;
    vkGetImageMemoryRequirements(dev, img, &r);
    vkBindImageMemory(dev, img, bind_mem(r, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT), 0);
    VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, img, VK_IMAGE_VIEW_TYPE_2D,
                                 VK_FORMAT_R8G8B8A8_UNORM, {0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkImageView view;
    vkCreateImageView(dev, &vci, NULL, &view);
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, W * W * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                              VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
    VkBuffer buf;
    vkCreateBuffer(dev, &bci, NULL, &buf);
    vkGetBufferMemoryRequirements(dev, buf, &r);
    VkDeviceMemory bmem = bind_mem(r, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vkBindBufferMemory(dev, buf, bmem, 0);

    VkAttachmentDescription att = {0, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR,
                                   VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE,
                                   VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL};
    VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub = {0, VK_PIPELINE_BIND_POINT_GRAPHICS, 0, NULL, 1, &ref, NULL, NULL, 0, NULL};
    /* The copy after the pass waits for its colour writes. */
    VkSubpassDependency dep = {0, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                               VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, 0};
    VkRenderPassCreateInfo rpci = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, NULL, 0, 1, &att, 1, &sub, 1, &dep};
    VkRenderPass rp;
    vkCreateRenderPass(dev, &rpci, NULL, &rp);
    VkFramebufferCreateInfo fci = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, NULL, 0, rp, 1, &view, W, W, 1};
    VkFramebuffer fb;
    vkCreateFramebuffer(dev, &fci, NULL, &fb);

    const VkShaderStageFlagBits stages[3] = {VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_GEOMETRY_BIT, VK_SHADER_STAGE_FRAGMENT_BIT};
    VkPipelineShaderStageCreateInfo ss[3];
    uint32_t nst = 0;
    for (int i = 0; i < 3; i++) {
        if (i == 1 && !gs) continue;
        VkShaderModule m = module(argv[1 + i]);
        if (!m) {
            printf("FAILED: shader module %s\n", argv[1 + i]);
            return 1;
        }
        ss[nst++] = (VkPipelineShaderStageCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, stages[i], m,
                                                      "main", NULL};
    }
    VkPipelineLayoutCreateInfo plci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 0, NULL, 0, NULL};
    VkPipelineLayout pl;
    vkCreatePipelineLayout(dev, &plci, NULL, &pl);
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0,
                                                 VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_FALSE};
    VkViewport vp = {0, 0, W, W, 0, 1};
    VkRect2D sc = {{0, 0}, {W, W}};
    VkPipelineViewportStateCreateInfo vps = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL, 0, 1, &vp, 1, &sc};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState cba = {0};
    cba.colorWriteMask = 0xf;
    VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;
    VkGraphicsPipelineCreateInfo gci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, NULL, 0, nst, ss, &vi, &ia, NULL, &vps, &rs,
                                        &ms, NULL, &cb, NULL, pl, rp, 0, VK_NULL_HANDLE, -1};
    VkPipeline pipe;
    VkResult res = vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gci, NULL, &pipe);
    if (res != VK_SUCCESS) {
        printf("FAILED: vkCreateGraphicsPipelines %d\n", res);
        return 1;
    }

    VkCommandPoolCreateInfo cpci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, 0, 0};
    VkCommandPool pool;
    vkCreateCommandPool(dev, &cpci, NULL, &pool);
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(dev, &cai, &cmd);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
    vkBeginCommandBuffer(cmd, &bi);
    VkClearValue clear = {{{0, 0, 0, 0}}};
    VkRenderPassBeginInfo rbi = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, NULL, rp, fb, sc, 1, &clear};
    vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdDraw(cmd, 6, 1, 0, 0);
    vkCmdEndRenderPass(cmd);
    VkBufferImageCopy reg = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {W, W, 1}};
    vkCmdCopyImageToBuffer(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &reg);
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cmd, 0, NULL};
    if (vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE) != VK_SUCCESS || vkQueueWaitIdle(q) != VK_SUCCESS) {
        printf("FAILED: submit\n");
        return 1;
    }
    uint8_t *px;
    vkMapMemory(dev, bmem, 0, W * W * 4, 0, (void **)&px);
    /* Row W/2: x = -0.5 (clipped) and x = +0.5 (kept). */
    const uint8_t *l = px + ((W / 2) * W + W / 4) * 4, *k = px + ((W / 2) * W + 3 * W / 4) * 4;
    int ok = k[0] == 255 && abs(k[1] - 64) <= 1 && abs(k[2] - 191) <= 1 && k[3] == 255 && !l[0] && !l[1] && !l[2] && !l[3];
    printf("%s: %s, clipped half (%d, %d, %d, %d), kept half (%d, %d, %d, %d)\n", ok ? "OK" : "FAILED",
           props.deviceName, l[0], l[1], l[2], l[3], k[0], k[1], k[2], k[3]);
    return !ok;
}
