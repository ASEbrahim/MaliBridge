/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * bitwise_check: MK-03 (integer vector bitwise ops on a swizzle, Mali r18p0-r53p0). A compute
 * shader (shaders/bitwise.comp, glslc -O0 so the shuffles and composites reach the driver) works
 * 21 values out of push constants - x = 0x10, so nothing folds - with ops on swizzles, lane-built
 * vectors, constant composites, a bitcast of a shuffle, a cascade, and one op on loaded vectors
 * that is not the pattern; the results are compared with the CPU's. host/bitwise_test.sh runs it
 * through the bridge with the bug imitated (VKBRIDGE_ERRATA_IMITATE=bitwise_scalar: fails), the
 * fix over it (passes), and the fix alone.
 *   bitwise_check [--inline]   --inline: the module chained to the pipeline's stage
 *                              (maintenance5), not made with vkCreateShaderModule
 */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint32_t comp_spv[] = {
#include "bitwise_comp.inc"
};

#define N 21

typedef struct {
    uint32_t x;
    float f;
    uint32_t v[2], m[2];
} push;

static void expected(const push *p, uint32_t *o)
{
    uint32_t x = p->x;
    o[0] = x | 1u, o[1] = 2u | 1u;
    uint32_t b[4] = {x + 3u, x + 2u, x + 1u, x}, bm[4] = {0xF0u, 0x0Fu, 0xFFu, 0x01u};
    for (int i = 0; i < 4; i++) o[2 + i] = b[i] & bm[i];
    o[6] = (x + 1u) ^ 3u, o[7] = x ^ 3u;
    o[8] = ~2u, o[9] = ~1u, o[10] = ~x;
    o[11] = (uint32_t)((int32_t)x >> 1), o[12] = (uint32_t)(-(int32_t)x >> 2);
    o[13] = (x + 5u) << 1, o[14] = x << 3;
    o[15] = (x & 0xFFu) | 4u, o[16] = (1u & 0xFFu) | (x + 7u);
    uint32_t one, fb;
    float fone = 1.0f;
    memcpy(&one, &fone, 4);
    memcpy(&fb, &p->f, 4);
    o[17] = one & 0x7F800000u, o[18] = fb & 0x7F800000u;
    o[19] = p->v[0] & p->m[0], o[20] = p->v[1] & p->m[1];
}

int main(int argc, char **argv)
{
    int inline_mod = argc > 1 && !strcmp(argv[1], "--inline");
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib) {
        printf("FAIL: no libvulkan.so.1\n");
        return 1;
    }
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
#define GI(n) PFN_##n n = (PFN_##n)gipa(inst, #n)
    VkInstance inst = VK_NULL_HANDLE;
    GI(vkCreateInstance);
    VkApplicationInfo ai = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "bitwise_check", 1, NULL, 0, VK_API_VERSION_1_1};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &ai};
    if (vkCreateInstance(&ici, NULL, &inst)) {
        printf("FAIL: instance\n");
        return 1;
    }
    GI(vkEnumeratePhysicalDevices);
    GI(vkGetPhysicalDeviceMemoryProperties);
    GI(vkEnumerateDeviceExtensionProperties);
    GI(vkCreateDevice);
    GI(vkGetDeviceProcAddr);
    GI(vkDestroyInstance);
    uint32_t n = 1;
    VkPhysicalDevice pd;
    if (vkEnumeratePhysicalDevices(inst, &n, &pd) < 0 || !n) {
        printf("FAIL: no physical device\n");
        return 1;
    }
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
    VkShaderModule sm = VK_NULL_HANDLE;
    VkDescriptorSetLayoutBinding bind = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
    VkDescriptorSetLayoutCreateInfo lci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 1, &bind};
    VkDescriptorSetLayout dsl;
    VkPushConstantRange pcr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push)};
    VkPipelineLayoutCreateInfo plci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &dsl, 1, &pcr};
    VkPipelineLayout pl;
    if ((!inline_mod && vkCreateShaderModule(dev, &smci, NULL, &sm)) || vkCreateDescriptorSetLayout(dev, &lci, NULL, &dsl) ||
        vkCreatePipelineLayout(dev, &plci, NULL, &pl)) {
        printf("FAIL: setup\n");
        return 1;
    }
    VkComputePipelineCreateInfo cpci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpci.stage = (VkPipelineShaderStageCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, inline_mod ? &smci : NULL, 0,
                                                   VK_SHADER_STAGE_COMPUTE_BIT, sm, "main", NULL};
    cpci.layout = pl;
    VkPipeline pipe;
    if (vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, NULL, &pipe)) {
        printf("FAIL: pipeline\n");
        return 1;
    }

    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, N * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
    VkBuffer buf;
    VkMemoryRequirements mr;
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    if (vkCreateBuffer(dev, &bci, NULL, &buf)) {
        printf("FAIL: buffer\n");
        return 1;
    }
    vkGetBufferMemoryRequirements(dev, buf, &mr);
    uint32_t type = UINT32_MAX;
    VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t i = 0; i < mp.memoryTypeCount && type == UINT32_MAX; i++)
        if ((mr.memoryTypeBits >> i & 1) && (mp.memoryTypes[i].propertyFlags & want) == want) type = i;
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, type};
    VkDeviceMemory mem;
    uint32_t *map;
    if (type == UINT32_MAX || vkAllocateMemory(dev, &mai, NULL, &mem) || vkBindBufferMemory(dev, buf, mem, 0) ||
        vkMapMemory(dev, mem, 0, VK_WHOLE_SIZE, 0, (void **)&map)) {
        printf("FAIL: memory\n");
        return 1;
    }
    memset(map, 0xAB, N * 4);
    VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
    VkDescriptorPoolCreateInfo dpci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 1, 1, &ps};
    VkDescriptorPool dp;
    VkDescriptorSet set;
    VkDescriptorSetAllocateInfo dsai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, 0, 1, &dsl};
    if (vkCreateDescriptorPool(dev, &dpci, NULL, &dp) || (dsai.descriptorPool = dp, vkAllocateDescriptorSets(dev, &dsai, &set))) {
        printf("FAIL: descriptors\n");
        return 1;
    }
    VkDescriptorBufferInfo dbi = {buf, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet wr = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &dbi};
    vkUpdateDescriptorSets(dev, 1, &wr, 0, NULL);
    VkCommandPoolCreateInfo cpi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, 0, 0};
    VkCommandPool cp;
    VkCommandBuffer cb;
    VkCommandBufferAllocateInfo cbai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, 0, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    if (vkCreateCommandPool(dev, &cpi, NULL, &cp) || (cbai.commandPool = cp, vkAllocateCommandBuffers(dev, &cbai, &cb))) {
        printf("FAIL: command buffer\n");
        return 1;
    }
    push p = {0x10u, 4.0f, {0xF0F0u, 0x1234u}, {0xFF00u, 0x0FF0u}};
    VkCommandBufferBeginInfo cbbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(cb, &cbbi);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &set, 0, NULL);
    vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
    vkCmdDispatch(cb, 1, 1, 1);
    vkEndCommandBuffer(cb);
    VkQueue q;
    vkGetDeviceQueue(dev, 0, 0, &q);
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cb};
    if (vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE) || vkQueueWaitIdle(q)) {
        printf("FAIL: submit\n");
        return 1;
    }
    uint32_t want_o[N];
    expected(&p, want_o);
    int bad = 0;
    char first[160] = "";
    for (int i = 0; i < N; i++)
        if (map[i] != want_o[i]) {
            if (!bad) snprintf(first, sizeof(first), "; first o[%d] = 0x%x, want 0x%x", i, map[i], want_o[i]);
            bad++;
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
    if (bad) {
        printf("FAIL: %d of %d values wrong%s (%s module)\n", bad, N, first, inline_mod ? "inline" : "vkCreateShaderModule");
        return 1;
    }
    printf("PASS: %d values as the CPU's (%s module)\n", N, inline_mod ? "inline" : "vkCreateShaderModule");
    return 0;
}
