/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * nms_replay: builds ONE graphics pipeline from a vertex + fragment SPIR-V pair straight on the
 * device's own Vulkan driver (no bridge), with the interface No Man's Sky's failing pipelines have
 * (a player log failed-spv/ 3-vs + 2-fs): set 0 binding 0 a uniform buffer (both stages), set 1 bindings
 * 0-3 and 18 combined image samplers (fragment), vertex inputs at locations 0 and 1 (vec4) and 11
 * (uint), 7 colour attachments. No device feature is enabled, as on a Mali GPU without
 * shaderClipDistance. Prints one line: "built", "failed <VkResult>" or "CRASH signal N in
 * vkCreateGraphicsPipelines" (exit 0 / 1 / 3), so a device run can tell the app's module (which
 * Mali's compiler crashed on) from the bridge's rewritten one.
 *
 *   nms_replay vs.spv fs.spv
 */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define NRT 7

static void on_crash(int sig)
{
    char msg[80];
    int n = snprintf(msg, sizeof(msg), "CRASH signal %d in vkCreateGraphicsPipelines\n", sig);
    if (write(1, msg, (size_t)n) < 0) _exit(4);
    _exit(3);
}

static uint32_t *load(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    uint32_t *c = n > 0 ? malloc((size_t)n) : NULL;
    if (!c || fread(c, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(c);
        return NULL;
    }
    fclose(f);
    *size = (size_t)n;
    return c;
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s vs.spv fs.spv\n", argv[0]);
        return 2;
    }
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib) lib = dlopen("libvulkan.so", RTLD_NOW);
    if (!lib) {
        printf("failed: no libvulkan\n");
        return 1;
    }
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
#define I(n) PFN_##n n = (PFN_##n)gipa(inst, #n)
#define D(n) PFN_##n n = (PFN_##n)gdpa(dev, #n)
    VkInstance inst = VK_NULL_HANDLE;
    I(vkCreateInstance);
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "nms_replay", 1, NULL, 0, VK_API_VERSION_1_1};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 0, NULL};
    if (vkCreateInstance(&ici, NULL, &inst) != VK_SUCCESS) {
        printf("failed: no instance\n");
        return 1;
    }
    I(vkEnumeratePhysicalDevices);
    I(vkGetPhysicalDeviceProperties);
    I(vkCreateDevice);
    I(vkGetDeviceProcAddr);
    PFN_vkGetDeviceProcAddr gdpa = vkGetDeviceProcAddr;
    uint32_t n = 1;
    VkPhysicalDevice pd;
    if (vkEnumeratePhysicalDevices(inst, &n, &pd) < 0 || !n) {
        printf("failed: no GPU\n");
        return 1;
    }
    VkPhysicalDeviceProperties pp;
    vkGetPhysicalDeviceProperties(pd, &pp);
    fprintf(stderr, "GPU: %s, driver 0x%x\n", pp.deviceName, pp.driverVersion);
    float prio = 1;
    VkDeviceQueueCreateInfo q = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, 0, 1, &prio};
    VkPhysicalDeviceFeatures none = {0}; /* shaderClipDistance off, as the bridge's Mali device */
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &q, 0, NULL, 0, NULL, &none};
    VkDevice dev;
    if (vkCreateDevice(pd, &dci, NULL, &dev) != VK_SUCCESS) {
        printf("failed: no device\n");
        return 1;
    }
    D(vkCreateShaderModule);
    D(vkCreateDescriptorSetLayout);
    D(vkCreatePipelineLayout);
    D(vkCreateRenderPass);
    D(vkCreateGraphicsPipelines);
    D(vkDestroyPipeline);
    D(vkDestroyDevice);

    VkShaderModule mod[2];
    for (int i = 0; i < 2; i++) {
        size_t sz;
        uint32_t *code = load(argv[1 + i], &sz);
        if (!code) {
            printf("failed: cannot read %s\n", argv[1 + i]);
            return 1;
        }
        VkShaderModuleCreateInfo mci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sz, code};
        VkResult r = vkCreateShaderModule(dev, &mci, NULL, &mod[i]);
        free(code);
        if (r != VK_SUCCESS) {
            printf("failed %d (vkCreateShaderModule %s)\n", r, argv[1 + i]);
            return 1;
        }
    }

    VkDescriptorSetLayoutBinding b0 = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
    VkDescriptorSetLayoutBinding b1[5];
    static const uint32_t tex[5] = {0, 1, 2, 3, 18};
    for (int i = 0; i < 5; i++)
        b1[i] = (VkDescriptorSetLayoutBinding){tex[i], VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
    VkDescriptorSetLayout sl[2];
    VkDescriptorSetLayoutCreateInfo s0 = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 1, &b0};
    VkDescriptorSetLayoutCreateInfo s1 = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 5, b1};
    VkPipelineLayout pl;
    VkPipelineLayoutCreateInfo plci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 2, sl, 0, NULL};
    if (vkCreateDescriptorSetLayout(dev, &s0, NULL, &sl[0]) || vkCreateDescriptorSetLayout(dev, &s1, NULL, &sl[1]) ||
        vkCreatePipelineLayout(dev, &plci, NULL, &pl)) {
        printf("failed: layouts\n");
        return 1;
    }

    VkAttachmentDescription att[NRT];
    VkAttachmentReference ref[NRT];
    VkPipelineColorBlendAttachmentState blend[NRT];
    for (int i = 0; i < NRT; i++) {
        att[i] = (VkAttachmentDescription){0, VK_FORMAT_R16G16B16A16_SFLOAT, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR,
                                           VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE,
                                           VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        ref[i] = (VkAttachmentReference){(uint32_t)i, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        blend[i] = (VkPipelineColorBlendAttachmentState){0};
        blend[i].colorWriteMask = 0xf;
    }
    VkSubpassDescription sub = {0, VK_PIPELINE_BIND_POINT_GRAPHICS, 0, NULL, NRT, ref, NULL, NULL, 0, NULL};
    VkRenderPassCreateInfo rpci = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, NULL, 0, NRT, att, 1, &sub, 0, NULL};
    VkRenderPass rp;
    if (vkCreateRenderPass(dev, &rpci, NULL, &rp)) {
        printf("failed: render pass\n");
        return 1;
    }

    VkPipelineShaderStageCreateInfo st[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, mod[0], "main", NULL},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, mod[1], "main", NULL}};
    VkVertexInputBindingDescription vb = {0, 40, VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription va[3] = {
        {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0}, {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 16}, {11, 0, VK_FORMAT_R32_UINT, 32}};
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, NULL, 0, 1, &vb, 3, va};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0,
                                                 VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 0};
    VkViewport vp = {0, 0, 64, 64, 0, 1};
    VkRect2D sc = {{0, 0}, {64, 64}};
    VkPipelineViewportStateCreateInfo vs = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL, 0, 1, &vp, 1, &sc};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = NRT;
    cb.pAttachments = blend;
    VkGraphicsPipelineCreateInfo gci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gci.stageCount = 2;
    gci.pStages = st;
    gci.pVertexInputState = &vi;
    gci.pInputAssemblyState = &ia;
    gci.pViewportState = &vs;
    gci.pRasterizationState = &rs;
    gci.pMultisampleState = &ms;
    gci.pColorBlendState = &cb;
    gci.layout = pl;
    gci.renderPass = rp;

    signal(SIGSEGV, on_crash);
    signal(SIGBUS, on_crash);
    signal(SIGABRT, on_crash);
    VkPipeline p;
    VkResult r = vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gci, NULL, &p);
    signal(SIGSEGV, SIG_DFL);
    if (r != VK_SUCCESS) {
        printf("failed %d\n", r);
        return 1;
    }
    printf("built\n");
    vkDestroyPipeline(dev, p, NULL);
    vkDestroyDevice(dev, NULL);
    return 0;
}
