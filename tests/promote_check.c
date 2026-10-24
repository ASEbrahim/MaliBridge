/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * promote_check: a device offered at a Vulkan version above its driver's (server_promote.c), as an
 * app that wants that version uses it - core names, core structures, no extensions enabled.
 *   promote_check <1.x>   the apiVersion expected (the minor version: 1.1 .. 1.4)
 *   1  apiVersion is that version
 *   2  VkPhysicalDeviceVulkan11/12/13Features and ...Properties agree with the extensions' own
 *      structures, queried apart (the driver answers those itself)
 *   3  a device from VkPhysicalDeviceVulkan12Features / 13Features alone, with no extension
 *      enabled: timeline semaphores (signal, value, wait), vkResetQueryPool and
 *      vkGetBufferDeviceAddress under their core names
 *   4  a SPIR-V 1.5 compute shader (1.6 at 1.3) with a runtime descriptor array and a subgroup
 *      broadcast from a lane that is dynamically uniform, not a constant: right values
 *   5  at 1.3: dynamic rendering (a clear), synchronization2 barriers, vkCmdCopyImageToBuffer2 and
 *      vkQueueSubmit2 signalling the timeline semaphore: the cleared colour read back
 * Exit status 0 only if every check passes.
 */
#define _GNU_SOURCE
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint32_t comp15[] = {
#include "promote_comp15.inc"
};
static const uint32_t comp16[] = {
#include "promote_comp16.inc"
};

static int failures, checks;

static void check(int ok, const char *what, const char *detail)
{
    checks++;
    if (!ok) failures++;
    printf("%s %s%s%s\n", ok ? "PASS" : "FAIL", what, detail && *detail ? ": " : "", detail ? detail : "");
    fflush(stdout);
}

static PFN_vkGetInstanceProcAddr gipa;
static VkInstance inst;
static VkDevice dev;
static PFN_vkGetDeviceProcAddr gdpa;
#define GI(name) PFN_##name name = (PFN_##name)gipa(inst, #name)
#define GD(name) PFN_##name name = (PFN_##name)gdpa(dev, #name)

static uint32_t mem_type(const VkPhysicalDeviceMemoryProperties *mp, uint32_t bits, VkMemoryPropertyFlags want)
{
    for (uint32_t i = 0; i < mp->memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp->memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

int main(int argc, char **argv)
{
    unsigned want_minor = 3;
    if (argc > 1 && sscanf(argv[1], "1.%u", &want_minor) != 1) {
        fprintf(stderr, "usage: promote_check <1.x>\n");
        return 2;
    }
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib) {
        printf("FAIL loader: %s\n", dlerror());
        return 1;
    }
    gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    GI(vkCreateInstance);
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "promote_check", 1, NULL, 0, VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app};
    if (vkCreateInstance(&ici, NULL, &inst) != VK_SUCCESS) {
        printf("FAIL instance\n");
        return 1;
    }
    GI(vkEnumeratePhysicalDevices);
    GI(vkGetPhysicalDeviceProperties);
    GI(vkGetPhysicalDeviceProperties2);
    GI(vkGetPhysicalDeviceFeatures2);
    GI(vkGetPhysicalDeviceMemoryProperties);
    GI(vkCreateDevice);
    GI(vkGetDeviceProcAddr);
    gdpa = vkGetDeviceProcAddr;
    uint32_t n = 1;
    VkPhysicalDevice pd;
    vkEnumeratePhysicalDevices(inst, &n, &pd);
    char d[1024];

    /* 1 */
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(pd, &props);
    unsigned minor = VK_API_VERSION_MINOR(props.apiVersion);
    snprintf(d, sizeof(d), "%s, Vulkan %u.%u.%u", props.deviceName, VK_API_VERSION_MAJOR(props.apiVersion), minor,
             VK_API_VERSION_PATCH(props.apiVersion));
    check(minor == want_minor, "apiVersion is the one expected", d);
    if (minor < 2) {
        printf("%s: %d of %d checks passed\n", failures ? "FAILED" : "OK", checks - failures, checks);
        return failures ? 1 : 0;
    }
    int v13 = minor >= 3;

    /* 2: core structures against the extensions' */
    VkPhysicalDeviceVulkan11Features f11 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &f11};
    VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, &f12};
    VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, v13 ? (void *)&f13 : (void *)&f12};
    vkGetPhysicalDeviceFeatures2(pd, &f2);
    VkPhysicalDeviceTimelineSemaphoreFeatures tl = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
    VkPhysicalDeviceHostQueryResetFeatures hqr = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES, &tl};
    VkPhysicalDeviceBufferDeviceAddressFeatures bda = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES, &hqr};
    VkPhysicalDeviceDescriptorIndexingFeatures di = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES, &bda};
    VkPhysicalDevice16BitStorageFeatures s16 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES, &di};
    VkPhysicalDeviceDynamicRenderingFeatures dr = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES, &s16};
    VkPhysicalDeviceSynchronization2Features s2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES, &dr};
    VkPhysicalDeviceMaintenance4Features m4 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_FEATURES, &s2};
    VkPhysicalDeviceFeatures2 e2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, v13 ? (void *)&m4 : (void *)&s16};
    vkGetPhysicalDeviceFeatures2(pd, &e2);
    snprintf(d, sizeof(d), "timelineSemaphore %u/%u hostQueryReset %u/%u bufferDeviceAddress %u/%u runtimeDescriptorArray %u/%u "
             "storageBuffer16BitAccess %u/%u",
             f12.timelineSemaphore, tl.timelineSemaphore, f12.hostQueryReset, hqr.hostQueryReset, f12.bufferDeviceAddress,
             bda.bufferDeviceAddress, f12.runtimeDescriptorArray, di.runtimeDescriptorArray, f11.storageBuffer16BitAccess,
             s16.storageBuffer16BitAccess);
    check(f12.timelineSemaphore && f12.timelineSemaphore == tl.timelineSemaphore && f12.hostQueryReset == hqr.hostQueryReset &&
              f12.bufferDeviceAddress == bda.bufferDeviceAddress && f12.runtimeDescriptorArray == di.runtimeDescriptorArray &&
              f11.storageBuffer16BitAccess == s16.storageBuffer16BitAccess && f12.subgroupBroadcastDynamicId,
          "Vulkan 1.1 / 1.2 features as the extensions report them", d);
    if (v13) {
        snprintf(d, sizeof(d), "dynamicRendering %u/%u synchronization2 %u/%u maintenance4 %u/%u privateData %u", f13.dynamicRendering,
                 dr.dynamicRendering, f13.synchronization2, s2.synchronization2, f13.maintenance4, m4.maintenance4, f13.privateData);
        check(f13.dynamicRendering && f13.dynamicRendering == dr.dynamicRendering && f13.synchronization2 == s2.synchronization2 &&
                  f13.maintenance4 == m4.maintenance4 && f13.privateData,
              "Vulkan 1.3 features as the extensions report them", d);
    }
    VkPhysicalDeviceVulkan11Properties p11 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES};
    VkPhysicalDeviceVulkan12Properties p12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES, &p11};
    VkPhysicalDeviceVulkan13Properties p13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES, &p12};
    VkPhysicalDeviceProperties2 pp = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, v13 ? (void *)&p13 : (void *)&p12};
    vkGetPhysicalDeviceProperties2(pd, &pp);
    VkPhysicalDeviceIDProperties idp = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    VkPhysicalDeviceDriverProperties drv = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES, &idp};
    VkPhysicalDeviceTimelineSemaphoreProperties tlp = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_PROPERTIES, &drv};
    VkPhysicalDeviceDescriptorIndexingProperties dip = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES, &tlp};
    VkPhysicalDeviceMaintenance4Properties m4p = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_PROPERTIES, &dip};
    VkPhysicalDeviceSubgroupSizeControlProperties ssp = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES, &m4p};
    VkPhysicalDeviceProperties2 ep = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, v13 ? (void *)&ssp : (void *)&dip};
    vkGetPhysicalDeviceProperties2(pd, &ep);
    snprintf(d, sizeof(d), "driverID %u/%u, driverName \"%s\"/\"%s\", maxTimelineSemaphoreValueDifference %llu/%llu, "
             "maxUpdateAfterBindDescriptorsInAllPools %u/%u, deviceUUID %s",
             p12.driverID, drv.driverID, p12.driverName, drv.driverName, (unsigned long long)p12.maxTimelineSemaphoreValueDifference,
             (unsigned long long)tlp.maxTimelineSemaphoreValueDifference, p12.maxUpdateAfterBindDescriptorsInAllPools,
             dip.maxUpdateAfterBindDescriptorsInAllPools, memcmp(p11.deviceUUID, idp.deviceUUID, VK_UUID_SIZE) ? "differs" : "same");
    check(p12.driverID && p12.driverID == drv.driverID && !strcmp(p12.driverName, drv.driverName) &&
              p12.maxTimelineSemaphoreValueDifference == tlp.maxTimelineSemaphoreValueDifference &&
              p12.maxUpdateAfterBindDescriptorsInAllPools == dip.maxUpdateAfterBindDescriptorsInAllPools &&
              !memcmp(p11.deviceUUID, idp.deviceUUID, VK_UUID_SIZE) && p12.framebufferIntegerColorSampleCounts,
          "Vulkan 1.1 / 1.2 properties as the extensions report them", d);
    if (v13) {
        snprintf(d, sizeof(d), "maxBufferSize %llu/%llu, subgroup sizes %u-%u/%u-%u, maxInlineUniformTotalSize %u",
                 (unsigned long long)p13.maxBufferSize, (unsigned long long)m4p.maxBufferSize, p13.minSubgroupSize, p13.maxSubgroupSize,
                 ssp.minSubgroupSize, ssp.maxSubgroupSize, p13.maxInlineUniformTotalSize);
        check(p13.maxBufferSize && p13.maxBufferSize == m4p.maxBufferSize && p13.minSubgroupSize == ssp.minSubgroupSize &&
                  p13.maxSubgroupSize == ssp.maxSubgroupSize && p13.maxInlineUniformTotalSize >= 256,
              "Vulkan 1.3 properties as the extensions report them", d);
        snprintf(d, sizeof(d), "storage %llu (single texel %u), uniform %llu (single texel %u), 1.0 limit %llu",
                 (unsigned long long)p13.storageTexelBufferOffsetAlignmentBytes, p13.storageTexelBufferOffsetSingleTexelAlignment,
                 (unsigned long long)p13.uniformTexelBufferOffsetAlignmentBytes, p13.uniformTexelBufferOffsetSingleTexelAlignment,
                 (unsigned long long)pp.properties.limits.minTexelBufferOffsetAlignment);
        check(p13.storageTexelBufferOffsetAlignmentBytes && p13.uniformTexelBufferOffsetAlignmentBytes &&
                  p13.storageTexelBufferOffsetAlignmentBytes <= pp.properties.limits.minTexelBufferOffsetAlignment &&
                  p13.uniformTexelBufferOffsetAlignmentBytes <= pp.properties.limits.minTexelBufferOffsetAlignment,
              "texel alignments", d);
    }

    /* 3: a device from the core structures alone */
    VkPhysicalDeviceVulkan12Features e12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan13Features e13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, &e12};
    e12.timelineSemaphore = VK_TRUE;
    e12.hostQueryReset = f12.hostQueryReset;
    e12.bufferDeviceAddress = f12.bufferDeviceAddress;
    e12.runtimeDescriptorArray = f12.runtimeDescriptorArray;
    if (v13) e13.dynamicRendering = e13.synchronization2 = e13.maintenance4 = VK_TRUE;
    VkPhysicalDeviceFeatures2 ef = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, v13 ? (void *)&e13 : (void *)&e12};
    ef.features.shaderStorageBufferArrayDynamicIndexing = VK_TRUE;
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, 0, 1, &prio};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &ef, 0, 1, &qci, 0, NULL, 0, NULL};
    VkResult r = vkCreateDevice(pd, &dci, NULL, &dev);
    snprintf(d, sizeof(d), "VkResult %d", r);
    check(r == VK_SUCCESS, "vkCreateDevice with Vulkan 1.2/1.3 features and no extensions", d);
    if (r != VK_SUCCESS) return 1;
    GD(vkGetDeviceQueue);
    GD(vkCreateSemaphore);
    GD(vkSignalSemaphore);
    GD(vkGetSemaphoreCounterValue);
    GD(vkWaitSemaphores);
    GD(vkCreateQueryPool);
    GD(vkResetQueryPool);
    GD(vkCreateBuffer);
    GD(vkGetBufferMemoryRequirements);
    GD(vkAllocateMemory);
    GD(vkBindBufferMemory);
    GD(vkGetBufferDeviceAddress);
    GD(vkMapMemory);
    GD(vkCreateShaderModule);
    GD(vkCreateDescriptorSetLayout);
    GD(vkCreatePipelineLayout);
    GD(vkCreateComputePipelines);
    GD(vkCreateDescriptorPool);
    GD(vkAllocateDescriptorSets);
    GD(vkUpdateDescriptorSets);
    GD(vkCreateCommandPool);
    GD(vkAllocateCommandBuffers);
    GD(vkBeginCommandBuffer);
    GD(vkEndCommandBuffer);
    GD(vkCmdBindPipeline);
    GD(vkCmdBindDescriptorSets);
    GD(vkCmdDispatch);
    GD(vkQueueSubmit);
    GD(vkQueueWaitIdle);
    GD(vkDestroyDevice);
    snprintf(d, sizeof(d), "vkSignalSemaphore %p vkGetSemaphoreCounterValue %p vkWaitSemaphores %p vkResetQueryPool %p",
             (void *)vkSignalSemaphore, (void *)vkGetSemaphoreCounterValue, (void *)vkWaitSemaphores, (void *)vkResetQueryPool);
    check(vkSignalSemaphore && vkGetSemaphoreCounterValue && vkWaitSemaphores && (vkResetQueryPool || !f12.hostQueryReset),
          "Vulkan 1.2 core commands", d);
    VkQueue q;
    vkGetDeviceQueue(dev, 0, 0, &q);
    VkSemaphoreTypeCreateInfo sti = {VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, NULL, VK_SEMAPHORE_TYPE_TIMELINE, 5};
    VkSemaphoreCreateInfo sci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &sti};
    VkSemaphore sem = VK_NULL_HANDLE;
    uint64_t val = 0;
    VkResult rs = vkCreateSemaphore(dev, &sci, NULL, &sem), rg = VK_ERROR_UNKNOWN, rw = VK_ERROR_UNKNOWN, rsig = VK_ERROR_UNKNOWN;
    if (rs == VK_SUCCESS && vkSignalSemaphore) {
        VkSemaphoreSignalInfo si = {VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, NULL, sem, 7};
        rsig = vkSignalSemaphore(dev, &si);
        rg = vkGetSemaphoreCounterValue(dev, sem, &val);
        VkSemaphoreWaitInfo wi = {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, NULL, 0, 1, &sem, (uint64_t[]){7}};
        rw = vkWaitSemaphores(dev, &wi, 1000000000ull);
    }
    snprintf(d, sizeof(d), "create %d, signal %d, value %d (%llu), wait %d", rs, rsig, rg, (unsigned long long)val, rw);
    check(rs == VK_SUCCESS && rsig == VK_SUCCESS && rg == VK_SUCCESS && val == 7 && rw == VK_SUCCESS, "timeline semaphore: signal 7, read 7, wait 7", d);
    if (f12.hostQueryReset && vkResetQueryPool) {
        VkQueryPoolCreateInfo qpi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, NULL, 0, VK_QUERY_TYPE_TIMESTAMP, 4};
        VkQueryPool qp;
        r = vkCreateQueryPool(dev, &qpi, NULL, &qp);
        if (r == VK_SUCCESS) vkResetQueryPool(dev, qp, 0, 4);
        check(r == VK_SUCCESS, "vkResetQueryPool from the host", "");
    }
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    if (f12.bufferDeviceAddress && vkGetBufferDeviceAddress) {
        VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, 4096,
                                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT};
        VkBuffer b;
        VkDeviceAddress addr = 0;
        r = vkCreateBuffer(dev, &bi, NULL, &b);
        if (r == VK_SUCCESS) {
            VkMemoryRequirements mr;
            vkGetBufferMemoryRequirements(dev, b, &mr);
            VkMemoryAllocateFlagsInfo fi = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, NULL, VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT};
            VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &fi, mr.size,
                                       mem_type(&mp, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
            VkDeviceMemory m;
            r = vkAllocateMemory(dev, &ai, NULL, &m);
            if (r == VK_SUCCESS) r = vkBindBufferMemory(dev, b, m, 0);
            VkBufferDeviceAddressInfo ba = {VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, NULL, b};
            if (r == VK_SUCCESS) addr = vkGetBufferDeviceAddress(dev, &ba);
        }
        snprintf(d, sizeof(d), "VkResult %d, address 0x%llx", r, (unsigned long long)addr);
        check(r == VK_SUCCESS && addr, "vkGetBufferDeviceAddress", d);
    }

    /* 4: SPIR-V 1.5 / 1.6, a runtime descriptor array and a dynamic broadcast */
    VkBufferCreateInfo obi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, 65 * 4 + 64 * 64 * 4,
                              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VkBuffer ob;
    vkCreateBuffer(dev, &obi, NULL, &ob);
    VkMemoryRequirements omr;
    vkGetBufferMemoryRequirements(dev, ob, &omr);
    VkMemoryAllocateInfo oai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, omr.size,
                                mem_type(&mp, omr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    VkDeviceMemory om;
    uint32_t *out = NULL;
    r = vkAllocateMemory(dev, &oai, NULL, &om);
    if (r == VK_SUCCESS) r = vkBindBufferMemory(dev, ob, om, 0);
    if (r == VK_SUCCESS) r = vkMapMemory(dev, om, 0, VK_WHOLE_SIZE, 0, (void **)&out);
    if (r != VK_SUCCESS || !out) {
        snprintf(d, sizeof(d), "VkResult %d", r);
        check(0, "host-visible output buffer", d);
        return 1;
    }
    memset(out, 0, 65 * 4);
    VkShaderModuleCreateInfo smi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, v13 ? sizeof(comp16) : sizeof(comp15),
                                    v13 ? comp16 : comp15};
    VkShaderModule sm;
    r = vkCreateShaderModule(dev, &smi, NULL, &sm);
    VkDescriptorSetLayoutBinding bnd = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT};
    VkDescriptorSetLayoutCreateInfo dli = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 1, &bnd};
    VkDescriptorSetLayout dsl;
    vkCreateDescriptorSetLayout(dev, &dli, NULL, &dsl);
    VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &dsl};
    VkPipelineLayout pl;
    vkCreatePipelineLayout(dev, &pli, NULL, &pl);
    VkComputePipelineCreateInfo cpi = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, NULL, 0,
                                       {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_COMPUTE_BIT, sm, "main"},
                                       pl};
    VkPipeline pipe = VK_NULL_HANDLE;
    VkResult rp = r == VK_SUCCESS ? vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, NULL, &pipe) : r;
    VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
    VkDescriptorPoolCreateInfo dpi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 1, 1, &ps};
    VkDescriptorPool dp;
    vkCreateDescriptorPool(dev, &dpi, NULL, &dp);
    VkDescriptorSetAllocateInfo dsa = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, dp, 1, &dsl};
    VkDescriptorSet ds;
    vkAllocateDescriptorSets(dev, &dsa, &ds);
    VkDescriptorBufferInfo dbi = {ob, 0, 65 * 4};
    VkWriteDescriptorSet wr = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, ds, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &dbi};
    vkUpdateDescriptorSets(dev, 1, &wr, 0, NULL);
    VkCommandPoolCreateInfo cpci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, 0};
    VkCommandPool cp;
    vkCreateCommandPool(dev, &cpci, NULL, &cp);
    VkCommandBufferAllocateInfo cbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, cp, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VkCommandBuffer cb;
    vkAllocateCommandBuffers(dev, &cbi, &cb);
    VkCommandBufferBeginInfo bgi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    int good = 0;
    unsigned sgs = 0;
    if (rp == VK_SUCCESS) {
        vkBeginCommandBuffer(cb, &bgi);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, NULL);
        vkCmdDispatch(cb, 1, 1, 1);
        vkEndCommandBuffer(cb);
        VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cb};
        vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE);
        vkQueueWaitIdle(q);
        sgs = out[64];
        if (sgs && sgs <= 64)
            for (unsigned i = 0; i < 64; i++) good += out[i] == ((i / sgs) * sgs + 1 % sgs) * 3;
    }
    snprintf(d, sizeof(d), "module %d, pipeline %d, subgroup size %u, %d of 64 right (out[0] %u, out[1] %u)", r, rp, sgs, good, out[0], out[1]);
    check(rp == VK_SUCCESS && good == 64, v13 ? "SPIR-V 1.6 compute: runtime array, dynamic broadcast" : "SPIR-V 1.5 compute: runtime array, dynamic broadcast",
          d);

    /* 5: 1.3's commands */
    if (v13) {
        GD(vkCreateImage);
        GD(vkGetImageMemoryRequirements);
        GD(vkBindImageMemory);
        GD(vkCreateImageView);
        GD(vkCmdPipelineBarrier2);
        GD(vkCmdBeginRendering);
        GD(vkCmdEndRendering);
        GD(vkCmdCopyImageToBuffer2);
        GD(vkQueueSubmit2);
        GD(vkResetCommandBuffer);
        snprintf(d, sizeof(d), "vkCmdPipelineBarrier2 %p vkCmdBeginRendering %p vkCmdCopyImageToBuffer2 %p vkQueueSubmit2 %p",
                 (void *)vkCmdPipelineBarrier2, (void *)vkCmdBeginRendering, (void *)vkCmdCopyImageToBuffer2, (void *)vkQueueSubmit2);
        int have = vkCmdPipelineBarrier2 && vkCmdBeginRendering && vkCmdEndRendering && vkCmdCopyImageToBuffer2 && vkQueueSubmit2;
        check(have, "Vulkan 1.3 core commands", d);
        if (have) {
            VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {64, 64, 1}, 1, 1,
                                    VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL,
                                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
            VkImage img;
            r = vkCreateImage(dev, &ii, NULL, &img);
            VkMemoryRequirements imr;
            vkGetImageMemoryRequirements(dev, img, &imr);
            VkMemoryAllocateInfo iai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, imr.size,
                                        mem_type(&mp, imr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
            VkDeviceMemory im;
            if (r == VK_SUCCESS) r = vkAllocateMemory(dev, &iai, NULL, &im);
            if (r == VK_SUCCESS) r = vkBindImageMemory(dev, img, im, 0);
            VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, img, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM,
                                        {0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
            VkImageView view;
            if (r == VK_SUCCESS) r = vkCreateImageView(dev, &vi, NULL, &view);
            if (r == VK_SUCCESS) {
                vkResetCommandBuffer(cb, 0);
                vkBeginCommandBuffer(cb, &bgi);
                VkImageMemoryBarrier2 b1 = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, NULL, VK_PIPELINE_STAGE_2_NONE, 0,
                                            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                                            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_QUEUE_FAMILY_IGNORED,
                                            VK_QUEUE_FAMILY_IGNORED, img, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
                VkDependencyInfo dep = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, NULL, 0, 0, NULL, 0, NULL, 1, &b1};
                vkCmdPipelineBarrier2(cb, &dep);
                VkRenderingAttachmentInfo ca = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, NULL, view, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                                VK_RESOLVE_MODE_NONE, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED, VK_ATTACHMENT_LOAD_OP_CLEAR,
                                                VK_ATTACHMENT_STORE_OP_STORE};
                ca.clearValue.color.float32[0] = 1.0f;
                ca.clearValue.color.float32[1] = 0.0f;
                ca.clearValue.color.float32[2] = 1.0f;
                ca.clearValue.color.float32[3] = 1.0f;
                VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO, NULL, 0, {{0, 0}, {64, 64}}, 1, 0, 1, &ca};
                vkCmdBeginRendering(cb, &ri);
                vkCmdEndRendering(cb);
                VkImageMemoryBarrier2 b2 = b1;
                b2.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
                b2.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
                b2.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
                b2.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
                b2.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                b2.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                dep.pImageMemoryBarriers = &b2;
                vkCmdPipelineBarrier2(cb, &dep);
                VkBufferImageCopy2 reg = {VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2, NULL, 65 * 4, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                          {0, 0, 0}, {64, 64, 1}};
                VkCopyImageToBufferInfo2 cii = {VK_STRUCTURE_TYPE_COPY_IMAGE_TO_BUFFER_INFO_2, NULL, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, ob,
                                                1, &reg};
                vkCmdCopyImageToBuffer2(cb, &cii);
                VkMemoryBarrier2 mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, NULL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                       VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT};
                VkDependencyInfo dep2 = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, NULL, 0, 1, &mb};
                vkCmdPipelineBarrier2(cb, &dep2);
                vkEndCommandBuffer(cb);
                VkCommandBufferSubmitInfo cbs = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, NULL, cb};
                VkSemaphoreSubmitInfo ss = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, NULL, sem, 8, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
                VkSubmitInfo2 s2i = {VK_STRUCTURE_TYPE_SUBMIT_INFO_2, NULL, 0, 0, NULL, 1, &cbs, 1, &ss};
                r = vkQueueSubmit2(q, 1, &s2i, VK_NULL_HANDLE);
                VkSemaphoreWaitInfo wi = {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, NULL, 0, 1, &sem, (uint64_t[]){8}};
                if (r == VK_SUCCESS) r = vkWaitSemaphores(dev, &wi, 5000000000ull);
                vkQueueWaitIdle(q);
            }
            uint32_t px = out[65], last = out[65 + 64 * 64 - 1];
            snprintf(d, sizeof(d), "VkResult %d, first texel 0x%08x, last 0x%08x (want 0xffff00ff)", r, px, last);
            check(r == VK_SUCCESS && px == 0xffff00ffu && last == 0xffff00ffu,
                  "dynamic rendering clear, sync2 barriers, copy2, submit2 signalling the timeline", d);
        }
    }
    vkDestroyDevice(dev, NULL);
    printf("%s: %d of %d checks passed\n", failures ? "FAILED" : "OK", checks - failures, checks);
    return failures ? 1 : 0;
}
