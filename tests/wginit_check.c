/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * wginit_check: shared memory with zero initializers reads as zero (VK_KHR_zero_initialize_workgroup_
 * memory, as DXVK uses it). A first dispatch fills the workgroup's shared memory with garbage; a
 * second, whose shared variables (a 100-element array over 64 invocations, an array of structs, a
 * single value) carry OpConstantNull initializers, writes what it reads to a buffer: all 165 values
 * must be 0. Run through the bridge with the server's wg_init pass on (VKBRIDGE_WG_INIT=1) it checks
 * the pass's own zeroing; "OK: ..." / "FAILED: ..." last, "SKIP: ..." without the feature.
 */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint32_t garbage_spv[] = {
#include "wginit_garbage.inc"
};
static const uint32_t reader_spv[] = {
#include "wginit_reader_init.inc"
};

#define N 165

int main(void)
{
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib) {
        printf("FAILED: loader: %s\n", dlerror());
        return 2;
    }
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
#define G(n) PFN_##n n = (PFN_##n)gipa(inst, #n)
    VkInstance inst = VK_NULL_HANDLE;
    G(vkCreateInstance);
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "vkbridge-wginit-check", 1, "vkbridge", 1, VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 0, NULL};
    if (vkCreateInstance(&ici, NULL, &inst)) {
        printf("FAILED: vkCreateInstance\n");
        return 1;
    }
    G(vkEnumeratePhysicalDevices);
    G(vkGetPhysicalDeviceFeatures2);
    G(vkGetPhysicalDeviceQueueFamilyProperties);
    G(vkGetPhysicalDeviceMemoryProperties);
    G(vkCreateDevice);
    G(vkGetDeviceProcAddr);
#undef G
    uint32_t n = 1;
    VkPhysicalDevice pd;
    if (vkEnumeratePhysicalDevices(inst, &n, &pd) < 0 || !n) {
        printf("FAILED: no physical device\n");
        return 1;
    }
    VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f13};
    vkGetPhysicalDeviceFeatures2(pd, &f2);
    if (!f13.shaderZeroInitializeWorkgroupMemory) {
        printf("SKIP: no shaderZeroInitializeWorkgroupMemory\n");
        return 0;
    }
    uint32_t nq = 16, qfam;
    VkQueueFamilyProperties qp[16];
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qp);
    for (qfam = 0; qfam < nq && !(qp[qfam].queueFlags & VK_QUEUE_COMPUTE_BIT); qfam++) {}
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, qfam, 1, &prio};
    VkPhysicalDeviceVulkan13Features en13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    en13.shaderZeroInitializeWorkgroupMemory = VK_TRUE;
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &en13, 0, 1, &qci, 0, NULL, 0, NULL, NULL};
    VkDevice dev;
    if (vkCreateDevice(pd, &dci, NULL, &dev)) {
        printf("FAILED: vkCreateDevice\n");
        return 1;
    }
#define D(n) PFN_##n n = (PFN_##n)vkGetDeviceProcAddr(dev, #n)
    D(vkGetDeviceQueue); D(vkCreateBuffer); D(vkGetBufferMemoryRequirements); D(vkAllocateMemory); D(vkBindBufferMemory);
    D(vkMapMemory); D(vkCreateShaderModule); D(vkCreateDescriptorSetLayout); D(vkCreatePipelineLayout); D(vkCreateComputePipelines);
    D(vkCreateDescriptorPool); D(vkAllocateDescriptorSets); D(vkUpdateDescriptorSets); D(vkCreateCommandPool);
    D(vkAllocateCommandBuffers); D(vkBeginCommandBuffer); D(vkEndCommandBuffer); D(vkCmdBindPipeline); D(vkCmdBindDescriptorSets);
    D(vkCmdDispatch); D(vkCmdPipelineBarrier); D(vkQueueSubmit); D(vkQueueWaitIdle); D(vkDestroyDevice);
#undef D
    VkQueue q;
    vkGetDeviceQueue(dev, qfam, 0, &q);
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, N * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
    VkBuffer buf;
    vkCreateBuffer(dev, &bci, NULL, &buf);
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(dev, buf, &mr);
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    uint32_t type = 0;
    VkMemoryPropertyFlags hv = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    while (type < mp.memoryTypeCount && !((mr.memoryTypeBits & (1u << type)) && (mp.memoryTypes[type].propertyFlags & hv) == hv)) type++;
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, type};
    VkDeviceMemory mem;
    if (type == mp.memoryTypeCount || vkAllocateMemory(dev, &mai, NULL, &mem) || vkBindBufferMemory(dev, buf, mem, 0)) {
        printf("FAILED: memory\n");
        return 1;
    }
    uint32_t *out;
    vkMapMemory(dev, mem, 0, N * 4, 0, (void **)&out);
    for (int i = 0; i < N; i++) out[i] = 0x11111111u;
    VkDescriptorSetLayoutBinding b = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
    VkDescriptorSetLayoutCreateInfo lci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 1, &b};
    VkDescriptorSetLayout dsl;
    vkCreateDescriptorSetLayout(dev, &lci, NULL, &dsl);
    VkPipelineLayoutCreateInfo plci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &dsl, 0, NULL};
    VkPipelineLayout pl;
    vkCreatePipelineLayout(dev, &plci, NULL, &pl);
    VkPipeline pipes[2];
    const uint32_t *codes[2] = {garbage_spv, reader_spv};
    size_t sizes[2] = {sizeof(garbage_spv), sizeof(reader_spv)};
    for (int i = 0; i < 2; i++) {
        VkShaderModuleCreateInfo smci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizes[i], codes[i]};
        VkShaderModule sm;
        if (vkCreateShaderModule(dev, &smci, NULL, &sm)) {
            printf("FAILED: shader module %d\n", i);
            return 1;
        }
        VkComputePipelineCreateInfo cci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        cci.stage = (VkPipelineShaderStageCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_COMPUTE_BIT, sm, "main", NULL};
        cci.layout = pl;
        if (vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cci, NULL, &pipes[i])) {
            printf("FAILED: pipeline %d\n", i);
            return 1;
        }
    }
    VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
    VkDescriptorPoolCreateInfo dpci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 1, 1, &ps};
    VkDescriptorPool pool;
    vkCreateDescriptorPool(dev, &dpci, NULL, &pool);
    VkDescriptorSetAllocateInfo dai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, pool, 1, &dsl};
    VkDescriptorSet set;
    vkAllocateDescriptorSets(dev, &dai, &set);
    VkDescriptorBufferInfo dbi = {buf, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &dbi, NULL};
    vkUpdateDescriptorSets(dev, 1, &w, 0, NULL);
    VkCommandPoolCreateInfo cpci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, 0, qfam};
    VkCommandPool cp;
    vkCreateCommandPool(dev, &cpci, NULL, &cp);
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, cp, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VkCommandBuffer cb;
    vkAllocateCommandBuffers(dev, &cai, &cb);
    VkCommandBufferBeginInfo cbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vkBeginCommandBuffer(cb, &cbi);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &set, 0, NULL);
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT};
    for (int round = 0; round < 4; round++) { /* garbage, then the reader, a few times over */
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipes[0]);
        vkCmdDispatch(cb, 64, 1, 1);
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipes[1]);
        vkCmdDispatch(cb, 1, 1, 1);
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    }
    vkEndCommandBuffer(cb);
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cb, 0, NULL};
    if (vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE) || vkQueueWaitIdle(q)) {
        printf("FAILED: submit\n");
        return 1;
    }
    int bad = 0, first = -1;
    for (int i = 0; i < N; i++)
        if (out[i]) {
            if (first < 0) first = i;
            bad++;
        }
    uint32_t first_v = first >= 0 ? out[first] : 0;
    vkDestroyDevice(dev, NULL);
    if (bad) printf("FAILED: %d of %d shared values not zero (first: [%d] = 0x%08x)\n", bad, N, first, first_v);
    else printf("OK: all %d shared values read as zero\n", N);
    return bad ? 1 : 0;
}
