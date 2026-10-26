/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * pcache_check: compiles distinct compute pipelines (one shader, a different specialization map
 * per pipeline) for the persistent pipeline cache checks in host/pcache_test.sh. Every argument
 * it does not know is ignored, so it can stand in for Steam's fossilize_replay (whose archive
 * paths name the game).
 *   --first N --count N   which pipelines (default 0, 32)
 *   --app-caches K        make K empty pipeline caches and compile into the first (default: none)
 *   --ext-sync            ... made EXTERNALLY_SYNCHRONIZED, as Zink makes its per-program ones
 *   --hold S              wait S seconds before tearing down (periodic saves happen meanwhile)
 *   --no-destroy          end with _exit() and destroy nothing, as a killed game
 *   --mem MB              also hold MB of device memory meanwhile (the report's GPU memory per process)
 */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "smoke_spv.h"

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

int main(int argc, char **argv)
{
    int first = 0, count = 32, app_caches = 0, ext_sync = 0, hold = 0, no_destroy = 0, mem_mb = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--first") && i + 1 < argc) first = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--count") && i + 1 < argc) count = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--app-caches") && i + 1 < argc) app_caches = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--hold") && i + 1 < argc) hold = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--mem") && i + 1 < argc) mem_mb = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--ext-sync")) ext_sync = 1;
        else if (!strcmp(argv[i], "--no-destroy")) no_destroy = 1;
    }
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib) {
        printf("FAIL: no libvulkan.so.1\n");
        return 1;
    }
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
#define GI(n) PFN_##n n = (PFN_##n)gipa(inst, #n)
    VkInstance inst = VK_NULL_HANDLE;
    GI(vkCreateInstance);
    VkApplicationInfo ai = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "pcache_check", 1, NULL, 0, VK_API_VERSION_1_1};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &ai};
    if (vkCreateInstance(&ici, NULL, &inst)) {
        printf("FAIL: instance\n");
        return 1;
    }
    GI(vkEnumeratePhysicalDevices);
    GI(vkGetPhysicalDeviceQueueFamilyProperties);
    GI(vkCreateDevice);
    GI(vkGetDeviceProcAddr);
    GI(vkDestroyInstance);
    uint32_t n = 1;
    VkPhysicalDevice pd;
    if (vkEnumeratePhysicalDevices(inst, &n, &pd) < 0 || !n) {
        printf("FAIL: no physical device\n");
        return 1;
    }
    float prio = 1;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, 0, 1, &prio};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &qci};
    VkDevice dev;
    if (vkCreateDevice(pd, &dci, NULL, &dev)) {
        printf("FAIL: device\n");
        return 1;
    }
#define GD(n) PFN_##n n = (PFN_##n)vkGetDeviceProcAddr(dev, #n)
    GD(vkDestroyDevice);
    GD(vkCreateShaderModule);
    GD(vkDestroyShaderModule);
    GD(vkCreateDescriptorSetLayout);
    GD(vkDestroyDescriptorSetLayout);
    GD(vkCreatePipelineLayout);
    GD(vkDestroyPipelineLayout);
    GD(vkCreateComputePipelines);
    GD(vkDestroyPipeline);
    GD(vkCreatePipelineCache);
    GD(vkDestroyPipelineCache);
    GD(vkAllocateMemory);
    GD(vkFreeMemory);

    VkShaderModuleCreateInfo smci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof(spv_smoke_comp), spv_smoke_comp};
    VkShaderModule sm;
    VkDescriptorSetLayoutBinding binds[2] = {{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
                                             {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
    VkDescriptorSetLayoutCreateInfo lci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 2, binds};
    VkDescriptorSetLayout dsl;
    VkPushConstantRange pcr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 8};
    VkPipelineLayoutCreateInfo plci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &dsl, 1, &pcr};
    VkPipelineLayout pl;
    if (vkCreateShaderModule(dev, &smci, NULL, &sm) || vkCreateDescriptorSetLayout(dev, &lci, NULL, &dsl) ||
        vkCreatePipelineLayout(dev, &plci, NULL, &pl)) {
        printf("FAIL: setup\n");
        return 1;
    }
    VkPipelineCache *caches = calloc(app_caches > 0 ? app_caches : 1, sizeof(*caches));
    for (int i = 0; i < app_caches; i++) {
        VkPipelineCacheCreateInfo pcci = {VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, NULL,
                                          ext_sync ? VK_PIPELINE_CACHE_CREATE_EXTERNALLY_SYNCHRONIZED_BIT : 0, 0, NULL};
        if (vkCreatePipelineCache(dev, &pcci, NULL, &caches[i])) {
            printf("FAIL: pipeline cache %d\n", i);
            return 1;
        }
    }
    VkPipeline *pipes = calloc(count > 0 ? count : 1, sizeof(*pipes));
    double t0 = now_ms();
    for (int i = 0; i < count; i++) {
        /* Constant id 1000 is not in the shader: the map changes the pipeline's key, not its code. */
        uint32_t value = (uint32_t)(first + i);
        VkSpecializationMapEntry me = {1000, 0, 4};
        VkSpecializationInfo si = {1, &me, 4, &value};
        VkComputePipelineCreateInfo cpci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        cpci.stage = (VkPipelineShaderStageCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_COMPUTE_BIT,
                                                       sm, "main", &si};
        cpci.layout = pl;
        if (vkCreateComputePipelines(dev, app_caches ? caches[0] : VK_NULL_HANDLE, 1, &cpci, NULL, &pipes[i])) {
            printf("FAIL: pipeline %d\n", first + i);
            return 1;
        }
    }
    double ms = now_ms() - t0;
    VkDeviceMemory held = VK_NULL_HANDLE;
    if (mem_mb > 0) {
        VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, (VkDeviceSize)mem_mb << 20, 0};
        if (vkAllocateMemory(dev, &mai, NULL, &held)) {
            printf("FAIL: %d MB of device memory\n", mem_mb);
            return 1;
        }
    }
    if (hold > 0) sleep((unsigned)hold);
    printf("OK: %d pipelines (%d..%d) in %.1f ms%s\n", count, first, first + count - 1, ms, no_destroy ? ", exiting without cleanup" : "");
    fflush(stdout);
    if (no_destroy) _exit(0);
    if (held) vkFreeMemory(dev, held, NULL);
    for (int i = 0; i < count; i++) vkDestroyPipeline(dev, pipes[i], NULL);
    for (int i = 0; i < app_caches; i++) vkDestroyPipelineCache(dev, caches[i], NULL);
    vkDestroyPipelineLayout(dev, pl, NULL);
    vkDestroyDescriptorSetLayout(dev, dsl, NULL);
    vkDestroyShaderModule(dev, sm, NULL);
    vkDestroyDevice(dev, NULL);
    vkDestroyInstance(inst, NULL);
    return 0;
}
