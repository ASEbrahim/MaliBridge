/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * gpuprof_check: command buffers for the GPU profiler (server_gpuprof.c, VKBRIDGE_GPU_PROF), re-recorded and
 * submitted for GPUPROF_S seconds (default 6.5, past the profiler's 5 s report) so the server logs a report.
 *   gpuprof_check runs      one dispatch of pipeline A over GPUPROF_WG workgroups (default 16384), then pipeline B
 *                           dispatched GPUPROF_MANY times (default 600) over one workgroup, then a buffer copy;
 *                           no barriers in between. Under =2 A and B must show as spans of their own, B's 600
 *                           dispatches as one span (more stamps than the profiler's 512 would end its spans before
 *                           the copy), and the copy must still be seen.
 *   gpuprof_check alternate A and B every dispatch, 300 pairs: under =2 every dispatch is a span, more than the
 *                           512 stamps a command buffer gets; the report must say spans were cut short.
 * Two pipelines from one module (a specialization constant the shader does not have keeps them apart); the
 * report tells them apart by their workgroup counts. host/gpuprof_test.sh reads the server's lines.
 */
#define _GNU_SOURCE
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "smoke_spv.h"

static int envi(const char *n, int d)
{
    const char *v = getenv(n);
    return v && *v ? atoi(v) : d;
}

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "runs";
    int alternate = !strcmp(mode, "alternate");
    uint32_t wg = (uint32_t)envi("GPUPROF_WG", 16384), many = (uint32_t)envi("GPUPROF_MANY", 600);
    double secs = getenv("GPUPROF_S") ? atof(getenv("GPUPROF_S")) : 6.5;
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib) {
        printf("FAILED: loader: %s\n", dlerror());
        return 2;
    }
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    VkInstance inst;
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "vkbridge-gpuprof-check", 1, "vkbridge", 1, VK_API_VERSION_1_1};
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
    G(vkCreateDescriptorSetLayout); G(vkCreateDescriptorPool); G(vkAllocateDescriptorSets); G(vkUpdateDescriptorSets);
    G(vkCreatePipelineLayout); G(vkCreateShaderModule); G(vkCreateComputePipelines); G(vkCreateCommandPool);
    G(vkAllocateCommandBuffers); G(vkBeginCommandBuffer); G(vkEndCommandBuffer); G(vkCmdBindPipeline);
    G(vkCmdBindDescriptorSets); G(vkCmdDispatch); G(vkCmdPushConstants); G(vkCmdCopyBuffer); G(vkCreateFence);
    G(vkResetFences); G(vkWaitForFences); G(vkGetDeviceQueue); G(vkQueueSubmit); G(vkCreateBuffer);
    G(vkGetBufferMemoryRequirements); G(vkAllocateMemory); G(vkBindBufferMemory); G(vkDeviceWaitIdle); G(vkDestroyDevice);
#undef G

    VkDeviceSize size = (VkDeviceSize)wg * 64 * 4;
    VkBuffer buf[2];
    VkDeviceMemory mem[2];
    for (int i = 0; i < 2; i++) {
        VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, size,
                                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, 0, 0, NULL};
        VkMemoryRequirements mr;
        if (vkCreateBuffer(dev, &bci, NULL, &buf[i])) return printf("FAILED: buffer\n"), 1;
        vkGetBufferMemoryRequirements(dev, buf[i], &mr);
        uint32_t ti = 0;
        while (ti < mp.memoryTypeCount && !(mr.memoryTypeBits & (1u << ti))) ti++;
        VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, ti};
        if (vkAllocateMemory(dev, &mai, NULL, &mem[i]) || vkBindBufferMemory(dev, buf[i], mem[i], 0)) return printf("FAILED: memory\n"), 1;
    }
    VkDescriptorSetLayoutBinding pb[2] = {{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
                                          {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
    VkDescriptorSetLayoutCreateInfo lci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 2, pb};
    VkDescriptorSetLayout dsl;
    VkPushConstantRange pcr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 8};
    VkPipelineLayoutCreateInfo plci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &dsl, 1, &pcr};
    VkPipelineLayout pl;
    VkShaderModuleCreateInfo mci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof(spv_smoke_comp), spv_smoke_comp};
    VkShaderModule mod;
    if (vkCreateDescriptorSetLayout(dev, &lci, NULL, &dsl) || vkCreatePipelineLayout(dev, &plci, NULL, &pl) ||
        vkCreateShaderModule(dev, &mci, NULL, &mod))
        return printf("FAILED: setup\n"), 1;
    VkDescriptorPoolSize dps = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2};
    VkDescriptorPoolCreateInfo dpci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 1, 1, &dps};
    VkDescriptorPool pool;
    VkDescriptorSet set;
    if (vkCreateDescriptorPool(dev, &dpci, NULL, &pool)) return printf("FAILED: pool\n"), 1;
    VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, pool, 1, &dsl};
    if (vkAllocateDescriptorSets(dev, &ai, &set)) return printf("FAILED: set\n"), 1;
    VkDescriptorBufferInfo dbi[2] = {{buf[0], 0, VK_WHOLE_SIZE}, {buf[1], 0, VK_WHOLE_SIZE}};
    VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 0, 0, 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, dbi, NULL};
    vkUpdateDescriptorSets(dev, 1, &w, 0, NULL);
    VkPipeline pipe[2];
    for (int i = 0; i < 2; i++) {
        uint32_t spec_val = (uint32_t)i + 1;
        VkSpecializationMapEntry me = {777, 0, 4};
        VkSpecializationInfo spec = {1, &me, 4, &spec_val};
        VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage = (VkPipelineShaderStageCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_COMPUTE_BIT,
                                                     mod, "main", &spec};
        ci.layout = pl;
        if (vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &ci, NULL, &pipe[i])) return printf("FAILED: pipeline %d\n", i), 1;
    }
    VkCommandPoolCreateInfo cpci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, 0};
    VkCommandPool cp;
    VkCommandBuffer cb;
    if (vkCreateCommandPool(dev, &cpci, NULL, &cp)) return printf("FAILED: command pool\n"), 1;
    VkCommandBufferAllocateInfo cbai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, cp, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    if (vkAllocateCommandBuffers(dev, &cbai, &cb)) return printf("FAILED: command buffer\n"), 1;
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, NULL, 0};
    VkFence fence;
    vkCreateFence(dev, &fci, NULL, &fence);
    VkQueue q;
    vkGetDeviceQueue(dev, 0, 0, &q);
    uint32_t pc[2] = {3, 1};
    VkBufferCopy region = {0, 0, 4096};
    int rounds = 0;
    double start = now_s();
    /* Each Begin after the first reads the last run's stamps back; the run after GPUPROF_S lets the report out. */
    while (now_s() - start < secs || rounds < 2) {
        VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
        if (vkBeginCommandBuffer(cb, &bi)) return printf("FAILED: begin\n"), 1;
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &set, 0, NULL);
        vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 8, pc);
        if (alternate) {
            for (int k = 0; k < 300; k++) {
                vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe[0]);
                vkCmdDispatch(cb, 2, 1, 1);
                vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe[1]);
                vkCmdDispatch(cb, 1, 1, 1);
            }
        } else {
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe[0]);
            vkCmdDispatch(cb, wg, 1, 1);
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe[1]);
            for (uint32_t k = 0; k < many; k++) vkCmdDispatch(cb, 1, 1, 1);
        }
        vkCmdCopyBuffer(cb, buf[0], buf[1], 1, &region);
        if (vkEndCommandBuffer(cb)) return printf("FAILED: end\n"), 1;
        VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cb, 0, NULL};
        vkResetFences(dev, 1, &fence);
        if (vkQueueSubmit(q, 1, &si, fence) || vkWaitForFences(dev, 1, &fence, VK_TRUE, 10000000000ull))
            return printf("FAILED: submit/wait\n"), 1;
        rounds++;
    }
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, 0, NULL};
    vkBeginCommandBuffer(cb, &bi); /* reads the last run back */
    vkEndCommandBuffer(cb);
    vkDeviceWaitIdle(dev);
    vkDestroyDevice(dev, NULL);
    printf("OK: %s, %d runs in %.1f s\n", mode, rounds, now_s() - start);
    return 0;
}
