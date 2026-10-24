/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * wave32_check: wave32 (server/spirv_wave32.c). A compute shader (shaders/wave32.comp) written
 * for 32-wide subgroups: a ballot's bit count, a butterfly sum by shuffles 16 lanes apart and
 * less, the lane / size / subgroup id / count, an exclusive prefix sum; 16 workgroups of 64,
 * every value compared with the CPU's 32-lane answer. Natively that holds only where subgroups
 * are 32 wide; through VKBRIDGE_WAVE32 it holds at any width. host/wave32_test.sh runs it.
 *   wave32_check [--require N] [--spv file [--groups G] --dump out]
 *     --require N  VK_EXT_subgroup_size_control's requiredSubgroupSize N for the pipeline (SKIP
 *                  when the device cannot: N outside its min..max, or no such control)
 *     --spv file   this module instead of the built-in one (binding 0: a storage buffer of 20
 *                  uints per invocation), G workgroups (16); nothing checked, the buffer written
 *                  to `out` (a differential check: the same module on two GPUs or paths)
 * The last line: PASS / FAIL / SKIP / DONE, with the device's subgroup size.
 */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint32_t comp_spv[] = {
#include "wave32_comp.inc"
};

#define GROUPS 16
#define N (GROUPS * 64)
#define WORDS (N * 20) /* the buffer: 20 uints an invocation for --spv modules (4 for the built-in one) */

static int fail(const char *what)
{
    printf("FAIL: %s\n", what);
    return 1;
}

static uint32_t xval(uint32_t g)
{
    return (g * 2654435761u) >> 20;
}

int main(int argc, char **argv)
{
    uint32_t require = 0;
    const char *spv_path = NULL, *dump = NULL;
    uint32_t groups = GROUPS;
    for (int a = 1; a + 1 < argc; a += 2)
        if (!strcmp(argv[a], "--require")) require = (uint32_t)atoi(argv[a + 1]);
        else if (!strcmp(argv[a], "--spv")) spv_path = argv[a + 1];
        else if (!strcmp(argv[a], "--dump")) dump = argv[a + 1];
        else if (!strcmp(argv[a], "--groups")) groups = (uint32_t)atoi(argv[a + 1]);
    if (!groups || groups > GROUPS) return fail("--groups: 1 to 16");
    const uint32_t *code = comp_spv;
    size_t code_size = sizeof(comp_spv);
    if (spv_path) {
        FILE *f = fopen(spv_path, "rb");
        if (!f) return fail("cannot read --spv");
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        uint32_t *b = n > 0 ? malloc((size_t)n) : NULL;
        if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) return fail("cannot read --spv");
        fclose(f);
        code = b, code_size = (size_t)n;
    }
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib) return fail("no libvulkan.so.1");
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
#define GI(n) PFN_##n n = (PFN_##n)gipa(inst, #n)
    VkInstance inst = VK_NULL_HANDLE;
    GI(vkCreateInstance);
    VkApplicationInfo ai = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "wave32_check", 1, NULL, 0, VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &ai};
    if (vkCreateInstance(&ici, NULL, &inst)) return fail("instance");
    GI(vkEnumeratePhysicalDevices);
    GI(vkGetPhysicalDeviceMemoryProperties);
    GI(vkGetPhysicalDeviceProperties);
    GI(vkGetPhysicalDeviceProperties2);
    GI(vkCreateDevice);
    GI(vkGetDeviceProcAddr);
    GI(vkDestroyInstance);
    uint32_t n = 1;
    VkPhysicalDevice pd;
    if (vkEnumeratePhysicalDevices(inst, &n, &pd) < 0 || !n) return fail("no physical device");
    VkPhysicalDeviceProperties pp;
    vkGetPhysicalDeviceProperties(pd, &pp);
    VkPhysicalDeviceSubgroupSizeControlProperties scp = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES};
    VkPhysicalDeviceSubgroupProperties sgp = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES,
                                              pp.apiVersion >= VK_API_VERSION_1_3 ? &scp : NULL};
    VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &sgp};
    if (vkGetPhysicalDeviceProperties2) vkGetPhysicalDeviceProperties2(pd, &p2);
    VkSubgroupFeatureFlags want_ops = VK_SUBGROUP_FEATURE_BALLOT_BIT | VK_SUBGROUP_FEATURE_SHUFFLE_BIT | VK_SUBGROUP_FEATURE_ARITHMETIC_BIT;
    if (!(sgp.supportedStages & VK_SHADER_STAGE_COMPUTE_BIT) || (sgp.supportedOperations & want_ops) != want_ops) {
        printf("SKIP: no ballot, shuffle and arithmetic subgroup operations in compute shaders\n");
        return 0;
    }
    uint32_t size = sgp.subgroupSize;
    if (require) {
        if (pp.apiVersion < VK_API_VERSION_1_3 || require < scp.minSubgroupSize || require > scp.maxSubgroupSize ||
            !(scp.requiredSubgroupSizeStages & VK_SHADER_STAGE_COMPUTE_BIT)) {
            printf("SKIP: a subgroup size of %u cannot be required (device: %u..%u)\n", require, scp.minSubgroupSize, scp.maxSubgroupSize);
            return 0;
        }
        size = require;
    }
    VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    f13.subgroupSizeControl = VK_TRUE;
    float prio = 1;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, 0, 1, &prio};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, require ? &f13 : NULL, 0, 1, &qci};
    VkDevice dev;
    if (vkCreateDevice(pd, &dci, NULL, &dev)) return fail("device");
#define GD(n) PFN_##n n = (PFN_##n)vkGetDeviceProcAddr(dev, #n)
    GD(vkDestroyDevice); GD(vkGetDeviceQueue); GD(vkCreateShaderModule); GD(vkDestroyShaderModule);
    GD(vkCreateDescriptorSetLayout); GD(vkDestroyDescriptorSetLayout); GD(vkCreatePipelineLayout); GD(vkDestroyPipelineLayout);
    GD(vkCreateComputePipelines); GD(vkDestroyPipeline); GD(vkCreateBuffer); GD(vkDestroyBuffer); GD(vkGetBufferMemoryRequirements);
    GD(vkAllocateMemory); GD(vkFreeMemory); GD(vkBindBufferMemory); GD(vkMapMemory); GD(vkCreateDescriptorPool);
    GD(vkDestroyDescriptorPool); GD(vkAllocateDescriptorSets); GD(vkUpdateDescriptorSets); GD(vkCreateCommandPool);
    GD(vkDestroyCommandPool); GD(vkAllocateCommandBuffers); GD(vkBeginCommandBuffer); GD(vkEndCommandBuffer);
    GD(vkCmdBindPipeline); GD(vkCmdBindDescriptorSets); GD(vkCmdDispatch); GD(vkQueueSubmit); GD(vkQueueWaitIdle);

    VkShaderModuleCreateInfo smci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, code_size, code};
    VkShaderModule sm = VK_NULL_HANDLE;
    VkDescriptorSetLayoutBinding bind = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
    VkDescriptorSetLayoutCreateInfo lci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 1, &bind};
    VkDescriptorSetLayout dsl;
    VkPipelineLayoutCreateInfo plci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &dsl, 0, NULL};
    VkPipelineLayout pl;
    if (vkCreateShaderModule(dev, &smci, NULL, &sm) || vkCreateDescriptorSetLayout(dev, &lci, NULL, &dsl) ||
        vkCreatePipelineLayout(dev, &plci, NULL, &pl))
        return fail("setup");
    VkPipelineShaderStageRequiredSubgroupSizeCreateInfo rss = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO, NULL, require};
    VkComputePipelineCreateInfo cpci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpci.stage = (VkPipelineShaderStageCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, require ? (const void *)&rss : NULL, 0,
                                                   VK_SHADER_STAGE_COMPUTE_BIT, sm, "main", NULL};
    cpci.layout = pl;
    VkPipeline pipe;
    if (vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, NULL, &pipe)) return fail("pipeline");

    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, WORDS * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
    VkBuffer buf;
    VkMemoryRequirements mr;
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    if (vkCreateBuffer(dev, &bci, NULL, &buf)) return fail("buffer");
    vkGetBufferMemoryRequirements(dev, buf, &mr);
    uint32_t type = UINT32_MAX;
    VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t i = 0; i < mp.memoryTypeCount && type == UINT32_MAX; i++)
        if ((mr.memoryTypeBits >> i & 1) && (mp.memoryTypes[i].propertyFlags & want) == want) type = i;
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, type};
    VkDeviceMemory mem;
    uint32_t *map;
    if (type == UINT32_MAX || vkAllocateMemory(dev, &mai, NULL, &mem) || vkBindBufferMemory(dev, buf, mem, 0) ||
        vkMapMemory(dev, mem, 0, VK_WHOLE_SIZE, 0, (void **)&map))
        return fail("memory");
    memset(map, 0xAB, WORDS * 4);
    VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
    VkDescriptorPoolCreateInfo dpci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 1, 1, &ps};
    VkDescriptorPool dp;
    VkDescriptorSet set;
    VkDescriptorSetAllocateInfo dsai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, 0, 1, &dsl};
    if (vkCreateDescriptorPool(dev, &dpci, NULL, &dp) || (dsai.descriptorPool = dp, vkAllocateDescriptorSets(dev, &dsai, &set)))
        return fail("descriptors");
    VkDescriptorBufferInfo dbi = {buf, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet wr = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &dbi};
    vkUpdateDescriptorSets(dev, 1, &wr, 0, NULL);
    VkCommandPoolCreateInfo cpi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, 0, 0};
    VkCommandPool cp;
    VkCommandBuffer cb;
    VkCommandBufferAllocateInfo cbai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, 0, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    if (vkCreateCommandPool(dev, &cpi, NULL, &cp) || (cbai.commandPool = cp, vkAllocateCommandBuffers(dev, &cbai, &cb)))
        return fail("command buffer");
    VkCommandBufferBeginInfo cbbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(cb, &cbbi);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &set, 0, NULL);
    vkCmdDispatch(cb, spv_path ? groups : GROUPS, 1, 1);
    vkEndCommandBuffer(cb);
    VkQueue q;
    vkGetDeviceQueue(dev, 0, 0, &q);
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cb};
    if (vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE) || vkQueueWaitIdle(q)) return fail("submit");
    int bad = 0;
    char first[200] = "";
    if (spv_path) {
        FILE *o = dump ? fopen(dump, "wb") : NULL;
        if (!o || fwrite(map, 4, WORDS, o) != WORDS) return fail("cannot write --dump");
        fclose(o);
        printf("DONE: %s run, %u words to %s (subgroup size %u)\n", spv_path, WORDS, dump, size);
        return 0;
    }
    for (uint32_t g = 0; g < N; g++) {
        uint32_t lii = g % 64, lane = lii % 32, vb = g - lane, count = 0, sum = 0, pre = 0;
        for (uint32_t j = 0; j < 32; j++) {
            uint32_t x = xval(vb + j);
            count += x & 1u, sum += x;
            if (j < lane) pre += x & 7u;
        }
        uint32_t want[4] = {count, sum, lane | (32u << 8) | ((lii / 32) << 16) | (2u << 24), pre};
        for (int k = 0; k < 4; k++)
            if (map[g * 4 + k] != want[k]) {
                static const char *const what[4] = {"ballot count", "shuffle sum", "lane/size/id/count", "exclusive sum"};
                if (!bad)
                    snprintf(first, sizeof(first), "; first: invocation %u %s 0x%x, want 0x%x", g, what[k], map[g * 4 + k], want[k]);
                bad++;
            }
    }
    vkDestroyCommandPool(dev, cp, NULL);
    vkDestroyDescriptorPool(dev, dp, NULL);
    vkDestroyPipeline(dev, pipe, NULL);
    vkDestroyPipelineLayout(dev, pl, NULL);
    vkDestroyDescriptorSetLayout(dev, dsl, NULL);
    vkDestroyShaderModule(dev, sm, NULL);
    vkDestroyBuffer(dev, buf, NULL);
    vkFreeMemory(dev, mem, NULL);
    vkDestroyDevice(dev, NULL);
    vkDestroyInstance(inst, NULL);
    if (bad) {
        printf("FAIL: %d of %d values wrong%s (subgroup size %u)\n", bad, N * 4, first, size);
        return 1;
    }
    printf("PASS: %d values as 32 lanes give (subgroup size %u)\n", N * 4, size);
    return 0;
}
