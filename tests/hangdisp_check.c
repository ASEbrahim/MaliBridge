/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * hangdisp_check: a long compute dispatch shaped like The Witcher 3's light culling on the G925 (h-96e7965da9ba6414):
 * its loop count is read from a storage buffer that a push constant picks out of an array of 16
 * (vkd3d-proton's bindless CBVs are storage buffers indexed by a root table offset in push constants). With the hang
 * detector on and VKBRIDGE_HANG_AFTER_MS small, host/hangdisp_test.sh checks the HANG report names the dispatch, its
 * compute push constants, and the buffer descriptor it read (range included), which the report gave for draws only.
 *   hangdisp_check [count]   the loop count (default 4026531840: ~0.9 s per submission on lavapipe, measured; 268435456 took 0.08 s, too short for the detector); 3 submissions
 * Layout: set 0 binding 0 = output, set 1 binding 2 = 16 storage buffers; element 5 + 3 = 8 is a 2048-byte range at
 * offset 256 of a 4096-byte buffer, holding the count at vec4 100 (byte 1600, inside the range).
 */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint32_t comp_spv[] = {
#include "hangdisp_comp.inc"
};

int main(int argc, char **argv)
{
    uint32_t count = argc > 1 ? (uint32_t)strtoul(argv[1], NULL, 0) : 4026531840u;
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib) {
        printf("FAIL: no libvulkan.so.1\n");
        return 1;
    }
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
#define GI(n) PFN_##n n = (PFN_##n)gipa(inst, #n)
    VkInstance inst = VK_NULL_HANDLE;
    GI(vkCreateInstance);
    VkApplicationInfo ai = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "hangdisp_check", 1, NULL, 0, VK_API_VERSION_1_1};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &ai};
    if (vkCreateInstance(&ici, NULL, &inst)) {
        printf("FAIL: instance\n");
        return 1;
    }
    GI(vkEnumeratePhysicalDevices);
    GI(vkGetPhysicalDeviceMemoryProperties);
    GI(vkGetPhysicalDeviceFeatures);
    GI(vkCreateDevice);
    GI(vkGetDeviceProcAddr);
    GI(vkDestroyInstance);
    uint32_t n = 1;
    VkPhysicalDevice pd;
    if (vkEnumeratePhysicalDevices(inst, &n, &pd) < 0 || !n) {
        printf("FAIL: no physical device\n");
        return 1;
    }
    VkPhysicalDeviceFeatures have;
    vkGetPhysicalDeviceFeatures(pd, &have);
    if (!have.shaderStorageBufferArrayDynamicIndexing) {
        printf("SKIP: no shaderStorageBufferArrayDynamicIndexing\n");
        return 0;
    }
    VkPhysicalDeviceFeatures feat = {0};
    feat.shaderStorageBufferArrayDynamicIndexing = VK_TRUE;
    float prio = 1;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, 0, 1, &prio};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &qci};
    dci.pEnabledFeatures = &feat;
    VkDevice dev;
    if (vkCreateDevice(pd, &dci, NULL, &dev)) {
        printf("FAIL: device\n");
        return 1;
    }
#define GD(n) PFN_##n n = (PFN_##n)vkGetDeviceProcAddr(dev, #n)
    GD(vkDestroyDevice); GD(vkGetDeviceQueue); GD(vkCreateShaderModule); GD(vkDestroyShaderModule);
    GD(vkCreateDescriptorSetLayout); GD(vkDestroyDescriptorSetLayout); GD(vkCreatePipelineLayout); GD(vkDestroyPipelineLayout);
    GD(vkCreateComputePipelines); GD(vkDestroyPipeline); GD(vkCreateBuffer); GD(vkDestroyBuffer); GD(vkGetBufferMemoryRequirements);
    GD(vkAllocateMemory); GD(vkFreeMemory); GD(vkBindBufferMemory); GD(vkMapMemory); GD(vkCreateDescriptorPool);
    GD(vkDestroyDescriptorPool); GD(vkAllocateDescriptorSets); GD(vkUpdateDescriptorSets); GD(vkCreateCommandPool);
    GD(vkDestroyCommandPool); GD(vkAllocateCommandBuffers); GD(vkBeginCommandBuffer); GD(vkEndCommandBuffer);
    GD(vkCmdBindPipeline); GD(vkCmdBindDescriptorSets); GD(vkCmdPushConstants); GD(vkCmdDispatch); GD(vkQueueSubmit);
    GD(vkQueueWaitIdle);

    VkShaderModuleCreateInfo smci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof(comp_spv), comp_spv};
    VkShaderModule sm;
    VkDescriptorSetLayoutBinding b0 = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
    VkDescriptorSetLayoutBinding b2 = {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 16, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
    VkDescriptorSetLayoutCreateInfo l0 = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 1, &b0};
    VkDescriptorSetLayoutCreateInfo l1 = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 1, &b2};
    VkDescriptorSetLayout dsl[2];
    VkPushConstantRange pcr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 8};
    VkPipelineLayoutCreateInfo plci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 2, dsl, 1, &pcr};
    VkPipelineLayout pl;
    if (vkCreateShaderModule(dev, &smci, NULL, &sm) || vkCreateDescriptorSetLayout(dev, &l0, NULL, &dsl[0]) ||
        vkCreateDescriptorSetLayout(dev, &l1, NULL, &dsl[1]) || vkCreatePipelineLayout(dev, &plci, NULL, &pl)) {
        printf("FAIL: setup\n");
        return 1;
    }
    VkComputePipelineCreateInfo cpci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpci.stage = (VkPipelineShaderStageCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_COMPUTE_BIT,
                                                   sm, "main", NULL};
    cpci.layout = pl;
    VkPipeline pipe;
    if (vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, NULL, &pipe)) {
        printf("FAIL: pipeline\n");
        return 1;
    }

    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    VkBuffer buf[2];
    VkDeviceMemory mem[2];
    uint8_t *map[2];
    const VkDeviceSize size[2] = {256 * 4, 4096};
    for (int i = 0; i < 2; i++) {
        VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, size[i], VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
        VkMemoryRequirements mr;
        if (vkCreateBuffer(dev, &bci, NULL, &buf[i])) {
            printf("FAIL: buffer\n");
            return 1;
        }
        vkGetBufferMemoryRequirements(dev, buf[i], &mr);
        uint32_t type = UINT32_MAX;
        VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        for (uint32_t k = 0; k < mp.memoryTypeCount && type == UINT32_MAX; k++)
            if ((mr.memoryTypeBits >> k & 1) && (mp.memoryTypes[k].propertyFlags & want) == want) type = k;
        VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, type};
        if (type == UINT32_MAX || vkAllocateMemory(dev, &mai, NULL, &mem[i]) || vkBindBufferMemory(dev, buf[i], mem[i], 0) ||
            vkMapMemory(dev, mem[i], 0, VK_WHOLE_SIZE, 0, (void **)&map[i])) {
            printf("FAIL: memory\n");
            return 1;
        }
        memset(map[i], 0, size[i]);
    }
    memcpy(map[1] + 256 + 1600, &count, 4); /* the count: element 8's range starts at 256; vec4 100 is byte 1600 of it */

    VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 17};
    VkDescriptorPoolCreateInfo dpci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 2, 1, &ps};
    VkDescriptorPool dp;
    VkDescriptorSet set[2];
    VkDescriptorSetAllocateInfo dsai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, 0, 2, dsl};
    if (vkCreateDescriptorPool(dev, &dpci, NULL, &dp) || (dsai.descriptorPool = dp, vkAllocateDescriptorSets(dev, &dsai, set))) {
        printf("FAIL: descriptors\n");
        return 1;
    }
    VkDescriptorBufferInfo out = {buf[0], 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo tbl[16];
    for (int i = 0; i < 16; i++) tbl[i] = (VkDescriptorBufferInfo){buf[1], 0, 256};
    tbl[8] = (VkDescriptorBufferInfo){buf[1], 256, 2048};
    VkWriteDescriptorSet wr[2] = {
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set[0], 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &out},
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set[1], 2, 0, 16, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, tbl},
    };
    vkUpdateDescriptorSets(dev, 2, wr, 0, NULL);
    VkCommandPoolCreateInfo cpi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, 0, 0};
    VkCommandPool cp;
    VkCommandBuffer cb;
    VkCommandBufferAllocateInfo cbai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, 0, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    if (vkCreateCommandPool(dev, &cpi, NULL, &cp) || (cbai.commandPool = cp, vkAllocateCommandBuffers(dev, &cbai, &cb))) {
        printf("FAIL: command buffer\n");
        return 1;
    }
    const uint32_t pcv[2] = {7, 5};
    VkQueue q;
    vkGetDeviceQueue(dev, 0, 0, &q);
    for (int r = 0; r < 3; r++) {
        VkCommandBufferBeginInfo cbbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        vkBeginCommandBuffer(cb, &cbbi);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 2, set, 0, NULL);
        vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pcv), pcv);
        vkCmdDispatch(cb, 4, 4, 1);
        vkEndCommandBuffer(cb);
        VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cb};
        if (vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE) || vkQueueWaitIdle(q)) {
            printf("FAIL: submit %d\n", r);
            return 1;
        }
    }
    uint32_t o0;
    memcpy(&o0, map[0], 4);
    vkDestroyCommandPool(dev, cp, NULL);
    vkDestroyDescriptorPool(dev, dp, NULL);
    vkDestroyPipeline(dev, pipe, NULL);
    vkDestroyPipelineLayout(dev, pl, NULL);
    vkDestroyDescriptorSetLayout(dev, dsl[0], NULL);
    vkDestroyDescriptorSetLayout(dev, dsl[1], NULL);
    vkDestroyShaderModule(dev, sm, NULL);
    for (int i = 0; i < 2; i++) {
        vkDestroyBuffer(dev, buf[i], NULL);
        vkFreeMemory(dev, mem[i], NULL);
    }
    vkDestroyDevice(dev, NULL);
    vkDestroyInstance(inst, NULL);
    /* the CPU's value for invocation 0: acc = 7, then acc * 1664525 + i for i = 0, 256, ... < count */
    uint32_t want = 7;
    for (uint32_t i = 0; i < count; i += 256u) want = want * 1664525u + i;
    if (o0 != want) {
        printf("FAIL: o[0] = 0x%x, want 0x%x (count %u)\n", o0, want, count);
        return 1;
    }
    printf("PASS: 3 dispatches of the %u-long loop, o[0] as the CPU's\n", count);
    return 0;
}
