/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * robust2_check: robustness2 as DXVK 3 and vkd3d-proton take it. The features are queried (printed
 * as "robustness2: buffer B image I null N"), then a device is made the way those layers make
 * one: VK_EXT_robustness2 (or KHR) with every robustness2 feature the query gave, and core
 * robustBufferAccess. With nullDescriptor a storage buffer descriptor is written null. On the
 * Mali-G715 r54p3 the driver has nullDescriptor only (host: VKBRIDGE_FAKE_MISSING=robust2access);
 * the bridge reports the other two and takes them off before the driver (host/robust2_test.sh).
 *   cc ... -o host/out/robust2_check tests/robust2_check.c -ldl          (host/Makefile)
 * Prints OK when the device is made and the null write goes through, FAILED otherwise; SKIP
 * without the extension.
 */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib) return printf("FAILED: no libvulkan.so.1\n"), 1;
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    PFN_vkCreateInstance vkCreateInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "robust2_check", 1, NULL, 0, VK_API_VERSION_1_1};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 0, NULL};
    VkInstance inst;
    if (vkCreateInstance(&ici, NULL, &inst)) return printf("FAILED: vkCreateInstance\n"), 1;
#define L(name) PFN_##name name = (PFN_##name)gipa(inst, #name)
    L(vkEnumeratePhysicalDevices);
    L(vkEnumerateDeviceExtensionProperties);
    L(vkGetPhysicalDeviceFeatures2);
    L(vkGetPhysicalDeviceQueueFamilyProperties);
    L(vkCreateDevice);
    L(vkGetDeviceProcAddr);
#undef L
    VkPhysicalDevice pd;
    uint32_t n = 1;
    if (vkEnumeratePhysicalDevices(inst, &n, &pd) < 0 || !n) return printf("FAILED: no physical device\n"), 1;
    uint32_t ne = 0;
    vkEnumerateDeviceExtensionProperties(pd, NULL, &ne, NULL);
    VkExtensionProperties ext[512];
    if (ne > 512) ne = 512;
    vkEnumerateDeviceExtensionProperties(pd, NULL, &ne, ext);
    const char *r2name = NULL;
    for (uint32_t i = 0; i < ne && !r2name; i++)
        if (!strcmp(ext[i].extensionName, "VK_EXT_robustness2") || !strcmp(ext[i].extensionName, "VK_KHR_robustness2"))
            r2name = ext[i].extensionName;
    if (!r2name) return printf("SKIP: no robustness2\n"), 0;

    VkPhysicalDeviceRobustness2FeaturesEXT r2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT};
    VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &r2};
    vkGetPhysicalDeviceFeatures2(pd, &f2);
    printf("robustness2: buffer %u image %u null %u (%s); robustBufferAccess %u\n", r2.robustBufferAccess2, r2.robustImageAccess2,
           r2.nullDescriptor, r2name, f2.features.robustBufferAccess);

    /* the device as DXVK makes it: what the query gave, all of it */
    VkPhysicalDeviceRobustness2FeaturesEXT er2 = r2;
    er2.pNext = NULL;
    VkPhysicalDeviceFeatures2 ef = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &er2};
    ef.features.robustBufferAccess = f2.features.robustBufferAccess;
    uint32_t nq = 8;
    VkQueueFamilyProperties qf[8];
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qf);
    uint32_t qfam = 0;
    while (qfam < nq && !(qf[qfam].queueFlags & VK_QUEUE_GRAPHICS_BIT)) qfam++;
    float prio = 1;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, qfam, 1, &prio};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &ef, 0, 1, &qci, 0, NULL, 1, &r2name, NULL};
    VkDevice dev;
    VkResult r = vkCreateDevice(pd, &dci, NULL, &dev);
    if (r) return printf("FAILED: vkCreateDevice with robustness2 as reported: %d\n", r), 1;

#define D(name) PFN_##name name = (PFN_##name)vkGetDeviceProcAddr(dev, #name)
    D(vkCreateDescriptorSetLayout);
    D(vkCreateDescriptorPool);
    D(vkAllocateDescriptorSets);
    D(vkUpdateDescriptorSets);
    D(vkDeviceWaitIdle);
    D(vkDestroyDevice);
#undef D
    if (r2.nullDescriptor) {
        VkDescriptorSetLayoutBinding b = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
        VkDescriptorSetLayoutCreateInfo lci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 1, &b};
        VkDescriptorSetLayout dsl;
        VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
        VkDescriptorPoolCreateInfo pci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 1, 1, &ps};
        VkDescriptorPool pool;
        VkDescriptorSet set;
        if (vkCreateDescriptorSetLayout(dev, &lci, NULL, &dsl) || vkCreateDescriptorPool(dev, &pci, NULL, &pool))
            return printf("FAILED: descriptor layout or pool\n"), 1;
        VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, pool, 1, &dsl};
        if (vkAllocateDescriptorSets(dev, &ai, &set)) return printf("FAILED: descriptor set\n"), 1;
        VkDescriptorBufferInfo bi = {VK_NULL_HANDLE, 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &bi, NULL};
        vkUpdateDescriptorSets(dev, 1, &w, 0, NULL);
    }
    r = vkDeviceWaitIdle(dev);
    vkDestroyDevice(dev, NULL);
    if (r) return printf("FAILED: vkDeviceWaitIdle %d\n", r), 1;
    printf("OK: device made with robustness2 as reported%s\n", r2.nullDescriptor ? ", a null storage buffer written" : "");
    return 0;
}
