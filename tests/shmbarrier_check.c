/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * shmbarrier_check: shm_barrier (server/spirv_shmbarrier.c). A compute shader
 * (shaders/shmbarrier.comp) writes and reads shared memory across a 256-invocation workgroup with
 * no barrier, the pattern of shaders made for lockstep 32-wide waves, then sums the workgroup in
 * a loop (a tree reduction) with no barrier either; 64 workgroups, each value and sum compared
 * with the CPU's. Level 1 cannot order the loop's iterations; level 2 does. With VKBRIDGE_SHARED_BARRIER=1 the bridge puts the barriers in and
 * every value is right on any GPU; without, the reads race (wrong values are likely on 16-wide
 * Mali, possible anywhere). host/shmbarrier_test.sh runs it.
 *   shmbarrier_check [--inline]   --inline: the module chained to the pipeline's stage
 *                                 (maintenance5), not made with vkCreateShaderModule
 */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint32_t comp_spv[] = {
#include "shmbarrier_comp.inc"
};

#define GROUPS 64
#define N (GROUPS * 512 + GROUPS)

static int fail(const char *what)
{
    printf("FAIL: %s\n", what);
    return 1;
}

int main(int argc, char **argv)
{
    int inline_mod = argc > 1 && !strcmp(argv[1], "--inline");
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib) return fail("no libvulkan.so.1");
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
#define GI(n) PFN_##n n = (PFN_##n)gipa(inst, #n)
    VkInstance inst = VK_NULL_HANDLE;
    GI(vkCreateInstance);
    VkApplicationInfo ai = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "shmbarrier_check", 1, NULL, 0, VK_API_VERSION_1_1};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &ai};
    if (vkCreateInstance(&ici, NULL, &inst)) return fail("instance");
    GI(vkEnumeratePhysicalDevices);
    GI(vkGetPhysicalDeviceMemoryProperties);
    GI(vkEnumerateDeviceExtensionProperties);
    GI(vkCreateDevice);
    GI(vkGetDeviceProcAddr);
    GI(vkDestroyInstance);
    uint32_t n = 1;
    VkPhysicalDevice pd;
    if (vkEnumeratePhysicalDevices(inst, &n, &pd) < 0 || !n) return fail("no physical device");
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
    if (vkCreateDevice(pd, &dci, NULL, &dev)) return fail("device");
#define GD(n) PFN_##n n = (PFN_##n)vkGetDeviceProcAddr(dev, #n)
    GD(vkDestroyDevice); GD(vkGetDeviceQueue); GD(vkCreateShaderModule); GD(vkDestroyShaderModule);
    GD(vkCreateDescriptorSetLayout); GD(vkDestroyDescriptorSetLayout); GD(vkCreatePipelineLayout); GD(vkDestroyPipelineLayout);
    GD(vkCreateComputePipelines); GD(vkDestroyPipeline); GD(vkCreateBuffer); GD(vkDestroyBuffer); GD(vkGetBufferMemoryRequirements);
    GD(vkAllocateMemory); GD(vkFreeMemory); GD(vkBindBufferMemory); GD(vkMapMemory); GD(vkCreateDescriptorPool);
    GD(vkDestroyDescriptorPool); GD(vkAllocateDescriptorSets); GD(vkUpdateDescriptorSets); GD(vkCreateCommandPool);
    GD(vkDestroyCommandPool); GD(vkAllocateCommandBuffers); GD(vkBeginCommandBuffer); GD(vkEndCommandBuffer);
    GD(vkCmdBindPipeline); GD(vkCmdBindDescriptorSets); GD(vkCmdDispatch); GD(vkQueueSubmit); GD(vkQueueWaitIdle);

    VkShaderModuleCreateInfo smci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof(comp_spv), comp_spv};
    VkShaderModule sm = VK_NULL_HANDLE;
    VkDescriptorSetLayoutBinding bind = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
    VkDescriptorSetLayoutCreateInfo lci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 1, &bind};
    VkDescriptorSetLayout dsl;
    VkPipelineLayoutCreateInfo plci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &dsl, 0, NULL};
    VkPipelineLayout pl;
    if ((!inline_mod && vkCreateShaderModule(dev, &smci, NULL, &sm)) || vkCreateDescriptorSetLayout(dev, &lci, NULL, &dsl) ||
        vkCreatePipelineLayout(dev, &plci, NULL, &pl))
        return fail("setup");
    VkComputePipelineCreateInfo cpci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpci.stage = (VkPipelineShaderStageCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, inline_mod ? &smci : NULL, 0,
                                                   VK_SHADER_STAGE_COMPUTE_BIT, sm, "main", NULL};
    cpci.layout = pl;
    VkPipeline pipe;
    if (vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, NULL, &pipe)) return fail("pipeline");

    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, N * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
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
    memset(map, 0xAB, N * 4);
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
    vkCmdDispatch(cb, GROUPS, 1, 1);
    vkEndCommandBuffer(cb);
    VkQueue q;
    vkGetDeviceQueue(dev, 0, 0, &q);
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cb};
    if (vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE) || vkQueueWaitIdle(q)) return fail("submit");
    int bad = 0;
    char first[160] = "";
    for (uint32_t g = 0; g < GROUPS; g++)
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t a = (255u - i) * 7u + 1u, b = i < 128u ? (127u - i) * 7u + 1001u : 0u;
            const uint32_t *o = map + g * 512;
            if (o[i] != a || o[256 + i] != b) {
                if (!bad)
                    snprintf(first, sizeof(first), "; first group %u invocation %u: %u %u, want %u %u", g, i, o[i], o[256 + i], a, b);
                bad++;
            }
        }
    int sums = 0;
    for (uint32_t g = 0; g < GROUPS; g++)
        if (map[GROUPS * 512 + g] != 32640u) {
            if (!sums && !bad)
                snprintf(first, sizeof(first), "; first wrong sum: group %u %u, want 32640", g, map[GROUPS * 512 + g]);
            sums++;
        }
    vkDestroyCommandPool(dev, cp, NULL);
    vkDestroyDescriptorPool(dev, dp, NULL);
    vkDestroyPipeline(dev, pipe, NULL);
    vkDestroyPipelineLayout(dev, pl, NULL);
    vkDestroyDescriptorSetLayout(dev, dsl, NULL);
    if (sm) vkDestroyShaderModule(dev, sm, NULL);
    vkDestroyBuffer(dev, buf, NULL);
    vkFreeMemory(dev, mem, NULL);
    vkDestroyDevice(dev, NULL);
    vkDestroyInstance(inst, NULL);
    const char *how = inline_mod ? "inline" : "vkCreateShaderModule";
    if (bad || sums) {
        printf("FAIL: %d of %d invocations wrong, %d of %d tree sums wrong%s (%s module)\n", bad, GROUPS * 256, sums, GROUPS, first, how);
        return 1;
    }
    printf("PASS: %d invocations and %d tree sums as the CPU's (%s module)\n", GROUPS * 256, GROUPS, how);
    return 0;
}
