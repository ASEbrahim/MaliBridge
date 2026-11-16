/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * pldedup_check: pipeline layouts made the same way, as vkd3d-proton makes them for root signatures
 * (Pixel 10 Pro XL, PowerVR: the driver keeps a table per buffer descriptor in each pipeline layout,
 * 305 MB for vkd3d's six bindless sets of 1,000,000; RE3's root signatures took the server's heap
 * past 3 GB in one second). It makes a uniform-buffer set layout (update after bind, variable count)
 * and a sampled-image one, then
 *   A1, A2  two layouts over both sets with the same push range   -> prints "A same" or "A different"
 *   B       the same sets with another push range                   -> prints "B differs" or "B equals A"
 *   one of A1/A2 destroyed, a compute pipeline made with the other  -> prints "pipeline after one destroy: <VkResult>"
 *   the other destroyed, A made again (A3), then B destroyed
 *   C1, C2  over the uniform-buffer set and one of two sampler set layouts made alike (as vkd3d-proton makes one for each
 *           root signature's static samplers)                      -> prints "C same" or "C different"
 *   the first sampler set layout destroyed, a compute pipeline made with C2 -> prints "pipeline after its set layout went: <VkResult>"
 *   D1..D5  over vkd3d-like costly sets (70,000 uniform buffers, 70,000 storage buffers, one small storage set) with the
 *           push ranges vkd3d-proton's root signatures differ in: VS|FS 16, ALL_GRAPHICS 64, none, COMPUTE 16, COMPUTE 32
 *           -> "D graphics same|different" (D1-D3), "D compute same|different" (D4, D5), "D kinds differ|equal"
 *   a compute pipeline made with D5; a command buffer pushes 16 bytes on D1 (VS|FS), then {111, 222} on D4 (COMPUTE),
 *           binds the small set through D5 and dispatches -> "D dispatch read <value>" (222111 when right)
 * and destroys the device with A3 and D3 still alive (the server's cleanup destroys them beneath).
 * Prints "OK: ..." last when every call returned.
 */
#define _GNU_SOURCE
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* an empty compute shader: void main() {} with local size 1x1x1 */
/* glslc -O: push_constant { uint a; uint b; }; set 2 binding 0 buffer { uint o; }; main() { o = a + b * 1000; } */
static const uint32_t pc_comp[] = {
    0x07230203, 0x00010300, 0x000d000a, 0x0000001d, 0x00000000, 0x00020011, 0x00000001, 0x0006000b, 0x00000001,
    0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e, 0x00000000, 0x00000001, 0x0005000f, 0x00000005,
    0x00000004, 0x6e69616d, 0x00000000, 0x00060010, 0x00000004, 0x00000011, 0x00000001, 0x00000001, 0x00000001,
    0x00050048, 0x00000007, 0x00000000, 0x00000023, 0x00000000, 0x00030047, 0x00000007, 0x00000002, 0x00040047,
    0x00000009, 0x00000022, 0x00000002, 0x00040047, 0x00000009, 0x00000021, 0x00000000, 0x00050048, 0x0000000c,
    0x00000000, 0x00000023, 0x00000000, 0x00050048, 0x0000000c, 0x00000001, 0x00000023, 0x00000004, 0x00030047,
    0x0000000c, 0x00000002, 0x00040047, 0x0000001c, 0x0000000b, 0x00000019, 0x00020013, 0x00000002, 0x00030021,
    0x00000003, 0x00000002, 0x00040015, 0x00000006, 0x00000020, 0x00000000, 0x0003001e, 0x00000007, 0x00000006,
    0x00040020, 0x00000008, 0x0000000c, 0x00000007, 0x0004003b, 0x00000008, 0x00000009, 0x0000000c, 0x00040015,
    0x0000000a, 0x00000020, 0x00000001, 0x0004002b, 0x0000000a, 0x0000000b, 0x00000000, 0x0004001e, 0x0000000c,
    0x00000006, 0x00000006, 0x00040020, 0x0000000d, 0x00000009, 0x0000000c, 0x0004003b, 0x0000000d, 0x0000000e,
    0x00000009, 0x00040020, 0x0000000f, 0x00000009, 0x00000006, 0x0004002b, 0x0000000a, 0x00000012, 0x00000001,
    0x0004002b, 0x00000006, 0x00000015, 0x000003e8, 0x00040020, 0x00000018, 0x0000000c, 0x00000006, 0x00040017,
    0x0000001a, 0x00000006, 0x00000003, 0x0004002b, 0x00000006, 0x0000001b, 0x00000001, 0x0006002c, 0x0000001a,
    0x0000001c, 0x0000001b, 0x0000001b, 0x0000001b, 0x00050036, 0x00000002, 0x00000004, 0x00000000, 0x00000003,
    0x000200f8, 0x00000005, 0x00050041, 0x0000000f, 0x00000010, 0x0000000e, 0x0000000b, 0x0004003d, 0x00000006,
    0x00000011, 0x00000010, 0x00050041, 0x0000000f, 0x00000013, 0x0000000e, 0x00000012, 0x0004003d, 0x00000006,
    0x00000014, 0x00000013, 0x00050084, 0x00000006, 0x00000016, 0x00000014, 0x00000015, 0x00050080, 0x00000006,
    0x00000017, 0x00000011, 0x00000016, 0x00050041, 0x00000018, 0x00000019, 0x00000009, 0x0000000b, 0x0003003e,
    0x00000019, 0x00000017, 0x000100fd, 0x00010038
};

static const uint32_t empty_comp[] = {
    0x07230203, 0x00010000, 0, 6, 0,
    (2u << 16) | 17, 1,                               /* OpCapability Shader */
    (3u << 16) | 14, 0, 1,                            /* OpMemoryModel Logical GLSL450 */
    (5u << 16) | 15, 5, 4, 0x6e69616d, 0,             /* OpEntryPoint GLCompute %4 "main" */
    (6u << 16) | 16, 4, 17, 1, 1, 1,                  /* OpExecutionMode %4 LocalSize 1 1 1 */
    (2u << 16) | 19, 2,                               /* %2 = OpTypeVoid */
    (3u << 16) | 33, 3, 2,                            /* %3 = OpTypeFunction %2 */
    (5u << 16) | 54, 2, 4, 0, 3,                      /* %4 = OpFunction %2 None %3 */
    (2u << 16) | 248, 5,                              /* %5 = OpLabel */
    (1u << 16) | 253,                                 /* OpReturn */
    (1u << 16) | 56,                                  /* OpFunctionEnd */
};

int main(void)
{
    alarm(60);
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib) return printf("FAILED: loader: %s\n", dlerror()), 2;
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    VkInstance inst;
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "vkbridge-pldedup-check", 1, "vkd3d", 1, VK_API_VERSION_1_2};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 0, NULL};
    if (((PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance"))(&ici, NULL, &inst)) return printf("FAILED: vkCreateInstance\n"), 1;
    uint32_t n = 1;
    VkPhysicalDevice pd;
    if (((PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices"))(inst, &n, &pd) < 0 || !n)
        return printf("FAILED: no physical device\n"), 1;
    VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f12};
    ((PFN_vkGetPhysicalDeviceFeatures2)gipa(inst, "vkGetPhysicalDeviceFeatures2"))(pd, &f2);
    int uab = f12.descriptorBindingUniformBufferUpdateAfterBind && f12.descriptorBindingVariableDescriptorCount;
    VkPhysicalDeviceVulkan12Features want = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    want.descriptorBindingUniformBufferUpdateAfterBind = f12.descriptorBindingUniformBufferUpdateAfterBind;
    want.descriptorBindingSampledImageUpdateAfterBind = f12.descriptorBindingSampledImageUpdateAfterBind;
    want.descriptorBindingVariableDescriptorCount = f12.descriptorBindingVariableDescriptorCount;
    want.descriptorBindingPartiallyBound = f12.descriptorBindingPartiallyBound;
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, 0, 1, &prio};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &want, 0, 1, &qci, 0, NULL, 0, NULL, NULL};
    VkDevice dev;
    if (((PFN_vkCreateDevice)gipa(inst, "vkCreateDevice"))(pd, &dci, NULL, &dev)) return printf("FAILED: vkCreateDevice\n"), 1;
    PFN_vkGetDeviceProcAddr gdpa = (PFN_vkGetDeviceProcAddr)gipa(inst, "vkGetDeviceProcAddr");
#define G(name) PFN_##name name = (PFN_##name)gdpa(dev, #name)
    G(vkCreateDescriptorSetLayout); G(vkDestroyDescriptorSetLayout); G(vkCreatePipelineLayout); G(vkDestroyPipelineLayout);
    G(vkCreateShaderModule); G(vkDestroyShaderModule); G(vkCreateComputePipelines); G(vkDestroyPipeline); G(vkDestroyDevice);
    G(vkCreateDescriptorPool); G(vkDestroyDescriptorPool); G(vkAllocateDescriptorSets); G(vkUpdateDescriptorSets);
    G(vkCreateBuffer); G(vkDestroyBuffer); G(vkGetBufferMemoryRequirements); G(vkAllocateMemory); G(vkFreeMemory);
    G(vkBindBufferMemory); G(vkMapMemory); G(vkCreateCommandPool); G(vkDestroyCommandPool); G(vkAllocateCommandBuffers);
    G(vkBeginCommandBuffer); G(vkEndCommandBuffer); G(vkCmdBindPipeline); G(vkCmdBindDescriptorSets); G(vkCmdPushConstants);
    G(vkCmdDispatch); G(vkGetDeviceQueue); G(vkQueueSubmit); G(vkQueueWaitIdle);
#undef G
    PFN_vkDestroyInstance vkDestroyInstance = (PFN_vkDestroyInstance)gipa(inst, "vkDestroyInstance");

    VkDescriptorSetLayout sets[2];
    VkDescriptorType types[2] = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE};
    for (int i = 0; i < 2; i++) {
        VkDescriptorSetLayoutBinding b = {0, types[i], 1024, VK_SHADER_STAGE_ALL, NULL};
        VkDescriptorBindingFlags bf = uab ? VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
                                                VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT
                                          : 0;
        VkDescriptorSetLayoutBindingFlagsCreateInfo fi = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO, NULL, 1, &bf};
        VkDescriptorSetLayoutCreateInfo ci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, uab ? &fi : NULL,
                                              uab ? VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT : 0, 1, &b};
        if (vkCreateDescriptorSetLayout(dev, &ci, NULL, &sets[i])) return printf("FAILED: set layout %d\n", i), 1;
    }
    VkPushConstantRange pa = {VK_SHADER_STAGE_ALL, 0, 64}, pb = {VK_SHADER_STAGE_ALL, 0, 128};
    VkPipelineLayoutCreateInfo ca = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 2, sets, 1, &pa};
    VkPipelineLayoutCreateInfo cb = ca;
    cb.pPushConstantRanges = &pb;
    VkPipelineLayout a1, a2, b, a3;
    if (vkCreatePipelineLayout(dev, &ca, NULL, &a1) || vkCreatePipelineLayout(dev, &ca, NULL, &a2) ||
        vkCreatePipelineLayout(dev, &cb, NULL, &b))
        return printf("FAILED: pipeline layouts\n"), 1;
    printf("A %s\n", a1 == a2 ? "same" : "different");
    printf("B %s\n", b != a1 && b != a2 ? "differs" : "equals A");
    vkDestroyPipelineLayout(dev, a1, NULL);
    VkShaderModuleCreateInfo mci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof(empty_comp), empty_comp};
    VkShaderModule m;
    if (vkCreateShaderModule(dev, &mci, NULL, &m)) return printf("FAILED: shader module\n"), 1;
    VkComputePipelineCreateInfo pci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, NULL, 0,
                                       {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_COMPUTE_BIT, m, "main", NULL},
                                       a2, VK_NULL_HANDLE, -1};
    VkPipeline p = VK_NULL_HANDLE;
    VkResult pr = vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &pci, NULL, &p);
    printf("pipeline after one destroy: %d\n", pr);
    if (p) vkDestroyPipeline(dev, p, NULL);
    vkDestroyShaderModule(dev, m, NULL);
    vkDestroyPipelineLayout(dev, a2, NULL);
    if (vkCreatePipelineLayout(dev, &ca, NULL, &a3)) return printf("FAILED: A again\n"), 1;
    vkDestroyPipelineLayout(dev, b, NULL);
    VkDescriptorSetLayout samp[2];
    for (int i = 0; i < 2; i++) {
        VkDescriptorSetLayoutBinding sb = {0, VK_DESCRIPTOR_TYPE_SAMPLER, 4, VK_SHADER_STAGE_ALL, NULL};
        VkDescriptorSetLayoutCreateInfo sci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 1, &sb};
        if (vkCreateDescriptorSetLayout(dev, &sci, NULL, &samp[i])) return printf("FAILED: sampler set layout %d\n", i), 1;
    }
    VkPipelineLayout c1, c2;
    VkDescriptorSetLayout s1[2] = {sets[0], samp[0]}, s2[2] = {sets[0], samp[1]};
    VkPipelineLayoutCreateInfo cc = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 2, s1, 1, &pa};
    if (vkCreatePipelineLayout(dev, &cc, NULL, &c1)) return printf("FAILED: C1\n"), 1;
    cc.pSetLayouts = s2;
    if (vkCreatePipelineLayout(dev, &cc, NULL, &c2)) return printf("FAILED: C2\n"), 1;
    printf("C %s\n", c1 == c2 ? "same" : "different");
    vkDestroyPipelineLayout(dev, c1, NULL);
    vkDestroyDescriptorSetLayout(dev, samp[0], NULL);
    if (vkCreateShaderModule(dev, &mci, NULL, &m)) return printf("FAILED: shader module 2\n"), 1;
    pci.stage.module = m;
    pci.layout = c2;
    p = VK_NULL_HANDLE;
    pr = vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &pci, NULL, &p);
    printf("pipeline after its set layout went: %d\n", pr);
    if (p) vkDestroyPipeline(dev, p, NULL);
    vkDestroyShaderModule(dev, m, NULL);
    vkDestroyPipelineLayout(dev, c2, NULL);
    vkDestroyDescriptorSetLayout(dev, samp[1], NULL);
    /* D: vkd3d-like costly sets, layouts that differ only in their push range */
    VkDescriptorSetLayout big[3];
    VkDescriptorType btypes[3] = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
    uint32_t bcount[3] = {70000, 70000, 1};
    for (int i = 0; i < 3; i++) {
        VkDescriptorSetLayoutBinding bb = {0, btypes[i], bcount[i], i < 2 ? VK_SHADER_STAGE_ALL : VK_SHADER_STAGE_COMPUTE_BIT, NULL};
        VkDescriptorSetLayoutCreateInfo bci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 1, &bb};
        if (vkCreateDescriptorSetLayout(dev, &bci, NULL, &big[i])) return printf("FAILED: costly set layout %d\n", i), 1;
    }
    VkPushConstantRange dr[5] = {{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16},
                                 {VK_SHADER_STAGE_ALL_GRAPHICS, 0, 64},
                                 {0, 0, 0},
                                 {VK_SHADER_STAGE_COMPUTE_BIT, 0, 16},
                                 {VK_SHADER_STAGE_COMPUTE_BIT, 0, 32}};
    VkPipelineLayout d[5];
    for (int i = 0; i < 5; i++) {
        VkPipelineLayoutCreateInfo dci2 = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 3, big, i == 2 ? 0 : 1, &dr[i]};
        if (vkCreatePipelineLayout(dev, &dci2, NULL, &d[i])) return printf("FAILED: D%d\n", i + 1), 1;
    }
    printf("D graphics %s\n", d[0] == d[1] && d[1] == d[2] ? "same" : "different");
    printf("D compute %s\n", d[3] == d[4] ? "same" : "different");
    printf("D kinds %s\n", d[0] != d[3] ? "differ" : "equal");
    mci.codeSize = sizeof(pc_comp);
    mci.pCode = pc_comp;
    if (vkCreateShaderModule(dev, &mci, NULL, &m)) return printf("FAILED: push shader module\n"), 1;
    pci.stage.module = m;
    pci.layout = d[4];
    p = VK_NULL_HANDLE;
    if (vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &pci, NULL, &p)) return printf("FAILED: D pipeline\n"), 1;
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, 256, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
    VkBuffer buf;
    if (vkCreateBuffer(dev, &bci, NULL, &buf)) return printf("FAILED: buffer\n"), 1;
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(dev, buf, &mr);
    PFN_vkGetPhysicalDeviceMemoryProperties gmp = (PFN_vkGetPhysicalDeviceMemoryProperties)gipa(inst, "vkGetPhysicalDeviceMemoryProperties");
    VkPhysicalDeviceMemoryProperties mp;
    gmp(pd, &mp);
    uint32_t mt = 0;
    VkMemoryPropertyFlags hv = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    while (mt < mp.memoryTypeCount && (!(mr.memoryTypeBits & (1u << mt)) || (mp.memoryTypes[mt].propertyFlags & hv) != hv)) mt++;
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, mt};
    VkDeviceMemory mem;
    if (mt == mp.memoryTypeCount || vkAllocateMemory(dev, &mai, NULL, &mem) || vkBindBufferMemory(dev, buf, mem, 0))
        return printf("FAILED: buffer memory\n"), 1;
    volatile uint32_t *host;
    if (vkMapMemory(dev, mem, 0, VK_WHOLE_SIZE, 0, (void **)&host)) return printf("FAILED: map\n"), 1;
    host[0] = 7;
    VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
    VkDescriptorPoolCreateInfo dpi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 1, 1, &ps};
    VkDescriptorPool pool;
    if (vkCreateDescriptorPool(dev, &dpi, NULL, &pool)) return printf("FAILED: pool\n"), 1;
    VkDescriptorSetAllocateInfo dai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, pool, 1, &big[2]};
    VkDescriptorSet ds;
    if (vkAllocateDescriptorSets(dev, &dai, &ds)) return printf("FAILED: set\n"), 1;
    VkDescriptorBufferInfo dbi = {buf, 0, 256};
    VkWriteDescriptorSet wds = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, ds, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &dbi, NULL};
    vkUpdateDescriptorSets(dev, 1, &wds, 0, NULL);
    VkCommandPoolCreateInfo cpi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, 0, 0};
    VkCommandPool cpool;
    if (vkCreateCommandPool(dev, &cpi, NULL, &cpool)) return printf("FAILED: command pool\n"), 1;
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, cpool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VkCommandBuffer cmd;
    if (vkAllocateCommandBuffers(dev, &cai, &cmd)) return printf("FAILED: command buffer\n"), 1;
    VkCommandBufferBeginInfo cbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
    uint32_t gfx[4] = {5, 6, 7, 8}, cpc[2] = {111, 222};
    vkBeginCommandBuffer(cmd, &cbi);
    vkCmdPushConstants(cmd, d[0], VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(gfx), gfx);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p);
    vkCmdPushConstants(cmd, d[3], VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(cpc), cpc);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, d[4], 2, 1, &ds, 0, NULL);
    vkCmdDispatch(cmd, 1, 1, 1);
    vkEndCommandBuffer(cmd);
    VkQueue q;
    vkGetDeviceQueue(dev, 0, 0, &q);
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cmd, 0, NULL};
    if (vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE) || vkQueueWaitIdle(q)) return printf("FAILED: submit\n"), 1;
    printf("D dispatch read %u\n", host[0]);
    vkDestroyCommandPool(dev, cpool, NULL);
    vkDestroyDescriptorPool(dev, pool, NULL);
    vkDestroyPipeline(dev, p, NULL);
    vkDestroyShaderModule(dev, m, NULL);
    vkDestroyBuffer(dev, buf, NULL);
    vkFreeMemory(dev, mem, NULL);
    for (int i = 0; i < 5; i++)
        if (i != 2) vkDestroyPipelineLayout(dev, d[i], NULL);
    for (int i = 0; i < 3; i++) vkDestroyDescriptorSetLayout(dev, big[i], NULL);
    vkDestroyDevice(dev, NULL); /* a3 and d[2] still alive */
    vkDestroyInstance(inst, NULL);
    printf("OK: uab %d\n", uab);
    return 0;
}
