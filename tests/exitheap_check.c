/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * exitheap_check: what a game leaves in the server when its process ends without destroying its
 * device (a player log, Tab S11 G925, test12: the server's heap 195 -> 617 -> 1037 -> 1446 MB after three
 * Skyrim SE runs in one session, each "exited without destroying device"; it never came back, and
 * the session ended with Android's low-memory kill). Wine/DXVK games end that way as a rule.
 * One process makes what a game holds at its end - descriptor pools with sets allocated from them,
 * shader modules, pipeline layouts, compute pipelines, a pipeline cache, command pools with
 * recorded command buffers, fences, semaphores, buffers with memory - and then
 *   exitheap_check exit     _exit(0)s with all of it alive (the server's cleanup is on its own)
 *   exitheap_check destroy  destroys all of it and the device, as an app should
 * EXITHEAP_POOLS / _SETS / _PIPES / _CMDS change the counts (16 / 2048 / 96 / 256).
 * The suite (host/exitheap_test.sh) runs it a few times against one server and reads the server's
 * heap from its memory lines.
 */
#define _GNU_SOURCE
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "smoke_spv.h"

static int envi(const char *n, int d)
{
    const char *v = getenv(n);
    return v && *v ? atoi(v) : d;
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "exit";
    int npools = envi("EXITHEAP_POOLS", 16), nsets = envi("EXITHEAP_SETS", 2048), npipes = envi("EXITHEAP_PIPES", 96),
        ncmds = envi("EXITHEAP_CMDS", 256);
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib) {
        printf("FAILED: loader: %s\n", dlerror());
        return 2;
    }
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    VkInstance inst;
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "vkbridge-exitheap-check", 1, "vkbridge", 1, VK_API_VERSION_1_1};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 0, NULL};
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
    VkPhysicalDeviceMemoryProperties mp;
    ((PFN_vkGetPhysicalDeviceMemoryProperties)gipa(inst, "vkGetPhysicalDeviceMemoryProperties"))(pd, &mp);
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
    G(vkCreateDescriptorSetLayout); G(vkDestroyDescriptorSetLayout); G(vkCreateDescriptorPool); G(vkDestroyDescriptorPool);
    G(vkAllocateDescriptorSets); G(vkUpdateDescriptorSets); G(vkDestroyDevice);
    G(vkCreatePipelineLayout); G(vkDestroyPipelineLayout); G(vkCreateShaderModule); G(vkDestroyShaderModule);
    G(vkCreateComputePipelines); G(vkDestroyPipeline); G(vkCreatePipelineCache); G(vkDestroyPipelineCache);
    G(vkCreateCommandPool); G(vkDestroyCommandPool); G(vkAllocateCommandBuffers); G(vkBeginCommandBuffer);
    G(vkEndCommandBuffer); G(vkCmdBindPipeline); G(vkCmdBindDescriptorSets); G(vkCmdDispatch);
    G(vkCreateFence); G(vkDestroyFence); G(vkCreateSemaphore); G(vkDestroySemaphore);
    G(vkCreateBuffer); G(vkDestroyBuffer); G(vkGetBufferMemoryRequirements); G(vkAllocateMemory); G(vkFreeMemory);
    G(vkBindBufferMemory); G(vkDeviceWaitIdle);
#undef G
    PFN_vkDestroyInstance vkDestroyInstance = (PFN_vkDestroyInstance)gipa(inst, "vkDestroyInstance");

    VkDescriptorSetLayoutBinding pb[2] = {{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
                                          {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
    VkDescriptorSetLayoutCreateInfo lci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 2, pb};
    VkDescriptorSetLayout dsl;
    VkPushConstantRange pcr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 8};
    VkPipelineLayoutCreateInfo plci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &dsl, 1, &pcr};
    VkPipelineLayout pl;
    VkShaderModuleCreateInfo mci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof(spv_smoke_comp), spv_smoke_comp};
    VkShaderModule mod;
    VkPipelineCacheCreateInfo pcci = {VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, NULL, 0, 0, NULL};
    VkPipelineCache cache;
    if (vkCreateDescriptorSetLayout(dev, &lci, NULL, &dsl) || vkCreatePipelineLayout(dev, &plci, NULL, &pl) ||
        vkCreateShaderModule(dev, &mci, NULL, &mod) || vkCreatePipelineCache(dev, &pcci, NULL, &cache)) {
        printf("FAILED: setup\n");
        return 1;
    }
    /* a buffer the sets name */
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, 1 << 20, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 0, 0, NULL};
    VkBuffer buf;
    VkDeviceMemory mem;
    VkMemoryRequirements mr;
    if (vkCreateBuffer(dev, &bci, NULL, &buf)) return printf("FAILED: buffer\n"), 1;
    vkGetBufferMemoryRequirements(dev, buf, &mr);
    uint32_t ti = 0;
    while (ti < mp.memoryTypeCount && !(mr.memoryTypeBits & (1u << ti))) ti++;
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, ti};
    if (vkAllocateMemory(dev, &mai, NULL, &mem) || vkBindBufferMemory(dev, buf, mem, 0)) return printf("FAILED: memory\n"), 1;

    VkDescriptorPool *pools = calloc((size_t)npools, sizeof(*pools));
    VkDescriptorSet *sets = calloc((size_t)nsets, sizeof(*sets));
    VkDescriptorSetLayout *layouts = calloc((size_t)nsets, sizeof(*layouts));
    for (int i = 0; i < nsets; i++) layouts[i] = dsl;
    VkDescriptorPoolSize dps = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, (uint32_t)nsets * 2};
    VkDescriptorPoolCreateInfo dpci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, (uint32_t)nsets, 1, &dps};
    VkDescriptorBufferInfo dbi = {buf, 0, VK_WHOLE_SIZE};
    int sets_made = 0;
    for (int p = 0; p < npools; p++) {
        if (vkCreateDescriptorPool(dev, &dpci, NULL, &pools[p])) return printf("FAILED: pool %d\n", p), 1;
        VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, pools[p], (uint32_t)nsets, layouts};
        if (vkAllocateDescriptorSets(dev, &ai, sets) == VK_SUCCESS) {
            sets_made += nsets;
            for (int s = 0; s < nsets; s += 64) {
                VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, sets[s], 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &dbi, NULL};
                vkUpdateDescriptorSets(dev, 1, &w, 0, NULL);
            }
        }
    }
    VkPipeline *pipes = calloc((size_t)npipes, sizeof(*pipes));
    int pipes_made = 0;
    for (int i = 0; i < npipes; i++) {
        uint32_t spec_val = (uint32_t)i + 1;
        VkSpecializationMapEntry me = {777, 0, 4};
        VkSpecializationInfo spec = {1, &me, 4, &spec_val};
        VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage = (VkPipelineShaderStageCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_COMPUTE_BIT,
                                                     mod, "main", &spec};
        ci.layout = pl;
        pipes_made += vkCreateComputePipelines(dev, cache, 1, &ci, NULL, &pipes[i]) == VK_SUCCESS;
    }
    VkCommandPoolCreateInfo cpci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, 0, 0};
    VkCommandPool cp;
    if (vkCreateCommandPool(dev, &cpci, NULL, &cp)) return printf("FAILED: command pool\n"), 1;
    VkCommandBuffer *cbs = calloc((size_t)ncmds, sizeof(*cbs));
    VkCommandBufferAllocateInfo cbai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, cp, VK_COMMAND_BUFFER_LEVEL_PRIMARY, (uint32_t)ncmds};
    if (ncmds && vkAllocateCommandBuffers(dev, &cbai, cbs)) return printf("FAILED: command buffers\n"), 1;
    VkCommandBufferBeginInfo cbbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, 0, NULL};
    for (int i = 0; i < ncmds; i++) {
        vkBeginCommandBuffer(cbs[i], &cbbi);
        for (int k = 0; k < 64 && pipes_made; k++) {
            vkCmdBindPipeline(cbs[i], VK_PIPELINE_BIND_POINT_COMPUTE, pipes[k % pipes_made]);
            vkCmdBindDescriptorSets(cbs[i], VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &sets[0], 0, NULL);
            vkCmdDispatch(cbs[i], 1, 1, 1);
        }
        vkEndCommandBuffer(cbs[i]);
    }
    VkFence fences[16];
    VkSemaphore sems[16];
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, NULL, 0};
    VkSemaphoreCreateInfo sci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, NULL, 0};
    for (int i = 0; i < 16; i++) vkCreateFence(dev, &fci, NULL, &fences[i]), vkCreateSemaphore(dev, &sci, NULL, &sems[i]);
    printf("made: %d pools, %d sets, %d pipelines, %d command buffers\n", npools, sets_made, pipes_made, ncmds);
    fflush(stdout);
    if (!strcmp(mode, "exit")) _exit(0);

    vkDeviceWaitIdle(dev);
    for (int i = 0; i < 16; i++) vkDestroyFence(dev, fences[i], NULL), vkDestroySemaphore(dev, sems[i], NULL);
    vkDestroyCommandPool(dev, cp, NULL);
    for (int i = 0; i < npipes; i++)
        if (pipes[i]) vkDestroyPipeline(dev, pipes[i], NULL);
    for (int p = 0; p < npools; p++) vkDestroyDescriptorPool(dev, pools[p], NULL);
    vkDestroyPipelineCache(dev, cache, NULL);
    vkDestroyShaderModule(dev, mod, NULL);
    vkDestroyPipelineLayout(dev, pl, NULL);
    vkDestroyDescriptorSetLayout(dev, dsl, NULL);
    vkDestroyBuffer(dev, buf, NULL);
    vkFreeMemory(dev, mem, NULL);
    vkDestroyDevice(dev, NULL);
    vkDestroyInstance(inst, NULL);
    printf("OK: destroyed\n");
    return 0;
}
