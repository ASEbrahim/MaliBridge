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
 *   --lose-when FILE      after the hold, once FILE exists, submit + wait on a fence until the device is lost (the
 *                         server's VKBRIDGE_TEST_LOST imitation, VKBRIDGE_TEST_LOST_COMM=pcache_check), print
 *                         "lost at round N", then compile --count more pipelines (after the first ones) into the same
 *                         cache, as RE Village went on compiling after its loss (a player log); host/losthygiene_test.sh
 *                         (not lost in 20 rounds: "not lost in 20 rounds", and the same follows)
 *   --flood K             ... and then K empty submissions (asynchronous: the client answers them), again right
 *                         before the teardown
 *   --after-hold S        ... and wait S seconds between the two floods (periodic saves happen meanwhile)
 *   --rate R              compile R pipelines a second (spread over the run, as Steam's replayer does), not all at once
 *   --reset-every K       with --app-caches: destroy the first cache and make a new one after every K pipelines, as Steam's
 *                         fossilize_replay does with its "memory context" caches (to keep the driver's memory down)
 *   --drop                destroy each pipeline right after it is made (the replayer keeps none: what stays is the caches)
 *   --mark                every pipeline carries the compile watchdog test's marker (VKBRIDGE_TEST_CRASH_PIPELINE=spin:
 *                         each spins until it is interrupted; host/spinheap_test.sh)
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
    int first = 0, count = 32, app_caches = 0, ext_sync = 0, hold = 0, no_destroy = 0, mem_mb = 0, flood = 0, after_hold = 0, rate = 0, mark = 0, reset_every = 0, drop = 0;
    const char *lose_when = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--first") && i + 1 < argc) first = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--count") && i + 1 < argc) count = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--app-caches") && i + 1 < argc) app_caches = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--hold") && i + 1 < argc) hold = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--mem") && i + 1 < argc) mem_mb = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--lose-when") && i + 1 < argc) lose_when = argv[++i];
        else if (!strcmp(argv[i], "--flood") && i + 1 < argc) flood = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--after-hold") && i + 1 < argc) after_hold = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--rate") && i + 1 < argc) rate = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--ext-sync")) ext_sync = 1;
        else if (!strcmp(argv[i], "--mark")) mark = 1;
        else if (!strcmp(argv[i], "--drop")) drop = 1;
        else if (!strcmp(argv[i], "--reset-every") && i + 1 < argc) reset_every = atoi(argv[++i]);
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
    GD(vkGetDeviceQueue);
    GD(vkQueueSubmit);
    GD(vkCreateFence);
    GD(vkDestroyFence);
    GD(vkWaitForFences);
    GD(vkResetFences);

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
    int total = lose_when ? 2 * count : count;
    VkPipeline *pipes = calloc(total > 0 ? total : 1, sizeof(*pipes));
    double t0 = now_ms();
    for (int i = 0; i < count; i++) {
        /* Constant id 1000 is not in the shader: the map changes the pipeline's key, not its code. */
        uint32_t value = (uint32_t)(first + i);
        VkSpecializationMapEntry me[2] = {{1000, 0, 4}, {0x7E57C0DEu, 0, 4}}; /* the second: server_guard.c TEST_CRASH_SPEC_ID */
        VkSpecializationInfo si = {mark ? 2 : 1, me, 4, &value};
        VkComputePipelineCreateInfo cpci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        cpci.stage = (VkPipelineShaderStageCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_COMPUTE_BIT,
                                                       sm, "main", &si};
        cpci.layout = pl;
        VkResult pr = vkCreateComputePipelines(dev, app_caches ? caches[0] : VK_NULL_HANDLE, 1, &cpci, NULL, &pipes[i]);
        if (pr && mark) {
            pipes[i] = VK_NULL_HANDLE; /* interrupted by the watchdog: failed, as it should */
        } else if (pr) {
            printf("FAIL: pipeline %d\n", first + i);
            return 1;
        }
        if (drop && pipes[i]) {
            vkDestroyPipeline(dev, pipes[i], NULL);
            pipes[i] = VK_NULL_HANDLE;
        }
        if (rate > 0) usleep(1000000 / rate);
        if (reset_every > 0 && app_caches && (i + 1) % reset_every == 0) {
            VkPipelineCacheCreateInfo pcci = {VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
            vkDestroyPipelineCache(dev, caches[0], NULL);
            if (vkCreatePipelineCache(dev, &pcci, NULL, &caches[0])) {
                printf("FAIL: pipeline cache again after %d\n", i + 1);
                return 1;
            }
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
    if (lose_when) {
        VkQueue q;
        vkGetDeviceQueue(dev, 0, 0, &q);
        VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VkFence f;
        if (vkCreateFence(dev, &fci, NULL, &f)) {
            printf("FAIL: fence\n");
            return 1;
        }
        for (int w = 0; access(lose_when, F_OK) != 0; w++) {
            if (w > 600) {
                printf("FAIL: %s never appeared\n", lose_when);
                return 1;
            }
            usleep(100000);
        }
        int lost_at = -1;
        for (int i = 0; i < 20 && lost_at < 0; i++) {
            VkResult s = vkQueueSubmit(q, 0, NULL, f);
            VkResult w = s == VK_SUCCESS ? vkWaitForFences(dev, 1, &f, VK_TRUE, 2000000000ull) : s;
            if (w == VK_ERROR_DEVICE_LOST) lost_at = i;
            else if (w != VK_SUCCESS) {
                printf("FAIL: round %d: submit %d, wait %d\n", i, s, w);
                return 1;
            } else
                vkResetFences(dev, 1, &f);
        }
        /* not lost (the test's control): the same compiles, they must reach the file */
        if (lost_at < 0) printf("not lost in 20 rounds\n");
        else printf("lost at round %d\n", lost_at);
        fflush(stdout);
        /* the game goes on compiling after its loss: these must not reach the cache's file */
        for (int i = count; i < total; i++) {
            uint32_t value = (uint32_t)(first + i);
            VkSpecializationMapEntry me = {1000, 0, 4};
            VkSpecializationInfo si = {1, &me, 4, &value};
            VkComputePipelineCreateInfo cpci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            cpci.stage = (VkPipelineShaderStageCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_COMPUTE_BIT,
                                                           sm, "main", &si};
            cpci.layout = pl;
            if (vkCreateComputePipelines(dev, app_caches ? caches[0] : VK_NULL_HANDLE, 1, &cpci, NULL, &pipes[i])) {
                printf("FAIL: pipeline %d after the loss\n", first + i);
                return 1;
            }
        }
        for (int i = 0; i < flood; i++) vkQueueSubmit(q, 0, NULL, VK_NULL_HANDLE);
        if (after_hold > 0) sleep((unsigned)after_hold);
        for (int i = 0; i < flood; i++) vkQueueSubmit(q, 0, NULL, VK_NULL_HANDLE);
        vkResetFences(dev, 1, &f);
        VkResult w = vkWaitForFences(dev, 1, &f, VK_TRUE, 2000000000ull); /* asks the server: after the floods */
        vkDestroyFence(dev, f, NULL);
        printf("after it: %d more pipelines, 2 x %d submissions, fence wait %d\n", total - count, flood, w);
        count = total;
    }
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
