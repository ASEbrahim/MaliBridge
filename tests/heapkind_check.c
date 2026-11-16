/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * heapkind_check: a known set of objects for the server's memory-by-kind lines (server/server_heapkind.c,
 * host/heapkind_test.sh): 7 shader modules (3 destroyed), 5 pipeline layouts (2 destroyed), 1 descriptor
 * set layout, 4 compute pipelines (1 destroyed), 2 descriptor pools, 1 pipeline cache; then it holds them
 * for HOLD seconds (argv[1], default 12) so the server's 5-s memory thread writes its lines, and exits.
 * Expected live counts: shader 4, layout 3, setlayout 1, pipeline 3, descpool 2, pcache 1, memory 1 (1 MB of type 0,
 * last: the server's memory thread starts at a first allocation).
 * Prints "pid N" first and "OK: ..." / "FAILED: ..." last.
 */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "smoke_spv.h"

int main(int argc, char **argv)
{
    int hold = argc > 1 ? atoi(argv[1]) : 12;
    printf("pid %d\n", (int)getpid());
    fflush(stdout);
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib) {
        printf("FAILED: loader: %s\n", dlerror());
        return 2;
    }
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "vkbridge-heapkind-check", 1, "vkbridge", 1, VK_API_VERSION_1_1};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 0, NULL};
    VkInstance inst;
    if (((PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance"))(&ici, NULL, &inst)) {
        printf("FAILED: vkCreateInstance\n");
        return 1;
    }
    uint32_t n = 1;
    VkPhysicalDevice pd;
    if (((PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices"))(inst, &n, &pd) < 0 || !n) {
        printf("FAILED: no physical device\n");
        return 1;
    }
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, 0, 1, &prio};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &qci, 0, NULL, 0, NULL, NULL};
    VkDevice dev;
    if (((PFN_vkCreateDevice)gipa(inst, "vkCreateDevice"))(pd, &dci, NULL, &dev)) {
        printf("FAILED: vkCreateDevice\n");
        return 1;
    }
    PFN_vkGetDeviceProcAddr gdpa = (PFN_vkGetDeviceProcAddr)gipa(inst, "vkGetDeviceProcAddr");
#define G(name) PFN_##name name = (PFN_##name)gdpa(dev, #name)
    G(vkCreateDescriptorSetLayout); G(vkCreateDescriptorPool); G(vkCreatePipelineLayout); G(vkDestroyPipelineLayout);
    G(vkCreateShaderModule); G(vkDestroyShaderModule); G(vkCreateComputePipelines); G(vkDestroyPipeline); G(vkCreatePipelineCache);
    G(vkDeviceWaitIdle); G(vkAllocateMemory);
#undef G
    int fails = 0;
    VkShaderModuleCreateInfo smci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof(spv_smoke_comp), spv_smoke_comp};
    VkShaderModule sm[7];
    for (int i = 0; i < 7; i++) fails += vkCreateShaderModule(dev, &smci, NULL, &sm[i]) != VK_SUCCESS;
    VkDescriptorSetLayoutBinding b = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
    VkDescriptorSetLayoutCreateInfo lci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 1, &b};
    VkDescriptorSetLayout sl;
    fails += vkCreateDescriptorSetLayout(dev, &lci, NULL, &sl) != VK_SUCCESS;
    VkPipelineLayoutCreateInfo plci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &sl, 0, NULL};
    VkPipelineLayout pl[5];
    for (int i = 0; i < 5; i++) fails += vkCreatePipelineLayout(dev, &plci, NULL, &pl[i]) != VK_SUCCESS;
    VkPipelineCacheCreateInfo pcci = {VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, NULL, 0, 0, NULL};
    VkPipelineCache pc;
    fails += vkCreatePipelineCache(dev, &pcci, NULL, &pc) != VK_SUCCESS;
    VkPipeline p[4];
    for (int i = 0; i < 4; i++) {
        VkComputePipelineCreateInfo cci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, NULL, 0,
                                           {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_COMPUTE_BIT, sm[i], "main", NULL},
                                           pl[i], VK_NULL_HANDLE, -1};
        fails += vkCreateComputePipelines(dev, pc, 1, &cci, NULL, &p[i]) != VK_SUCCESS;
    }
    VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 16};
    VkDescriptorPoolCreateInfo dpci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 16, 1, &ps};
    VkDescriptorPool dp[2];
    for (int i = 0; i < 2; i++) fails += vkCreateDescriptorPool(dev, &dpci, NULL, &dp[i]) != VK_SUCCESS;
    for (int i = 4; i < 7; i++) vkDestroyShaderModule(dev, sm[i], NULL);
    vkDestroyPipeline(dev, p[3], NULL);
    vkDestroyPipelineLayout(dev, pl[3], NULL);
    vkDestroyPipelineLayout(dev, pl[4], NULL);
    /* one allocation: the server's memory thread starts at its first (server_memory.c vkb_mem_track), so its
     * first look comes after every object above */
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, 1 << 20, 0};
    VkDeviceMemory mem;
    fails += vkAllocateMemory(dev, &mai, NULL, &mem) != VK_SUCCESS;
    vkDeviceWaitIdle(dev); /* a round trip: the destroys (no reply) have run */
    printf("made: 7 shader modules (3 destroyed), 5 layouts (2 destroyed), 1 set layout, 4 pipelines (1 destroyed), 2 pools, 1 cache\n");
    fflush(stdout);
    sleep((unsigned)hold);
    if (fails) printf("FAILED: %d creates failed\n", fails);
    else printf("OK: objects made and held %d s\n", hold);
    fflush(stdout);
    _exit(fails ? 1 : 0); /* no teardown: the counts stay as they were at the lines */
}
