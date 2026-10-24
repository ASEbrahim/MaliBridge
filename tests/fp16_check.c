/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * fp16_check: what the first GPU offers of 16-bit floats (fp16_hide, MK-20: server_errata_fp16.c),
 * printed for host/fp16_test.sh to compare:
 *   features: the Vulkan 1.1 / 1.2 structures and the extension ones (16-bit storage,
 *             shaderFloat16 / shaderInt8), and shaderInt16 from the 1.0 ones
 *   properties: the 16-bit float controls (Vulkan 1.2 properties)
 *   device: with "enable", a device made with shaderFloat16 and storageBuffer16BitAccess on
 *           whatever was offered - an app that enables them anyway (the server takes them off
 *           where they are hidden; the result is printed)
 *   dxvk: with "dxvk", DXVK 3's device creation: storageBuffer16BitAccess is a required feature
 *         (dxvk_device_info.cpp: ENABLE_FEATURE(vk11, storageBuffer16BitAccess, true)), so an
 *         adapter without it is skipped; with it, a device made with it on and shaderFloat16 on
 *         only where offered (DXVK's optional feature)
 * Exit status 0 when the queries ran (and the device, with "enable" / "dxvk", was made).
 */
#define _GNU_SOURCE
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    int enable = argc > 1 && !strcmp(argv[1], "enable"), dxvk = argc > 1 && !strcmp(argv[1], "dxvk");
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib) {
        printf("FAIL loader: %s\n", dlerror());
        return 1;
    }
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    VkInstance inst = NULL;
#define GI(name) PFN_##name name = (PFN_##name)gipa(inst, #name)
    GI(vkCreateInstance);
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "fp16_check", 1, NULL, 0, VK_API_VERSION_1_2};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app};
    if (vkCreateInstance(&ici, NULL, &inst) != VK_SUCCESS) {
        printf("FAIL instance\n");
        return 1;
    }
    GI(vkEnumeratePhysicalDevices);
    GI(vkGetPhysicalDeviceFeatures2);
    GI(vkGetPhysicalDeviceProperties2);
    GI(vkGetPhysicalDeviceQueueFamilyProperties);
    GI(vkCreateDevice);
    GI(vkDestroyInstance);
    uint32_t n = 1;
    VkPhysicalDevice pd = VK_NULL_HANDLE;
    if (vkEnumeratePhysicalDevices(inst, &n, &pd) < 0 || !n || !pd) {
        printf("FAIL no GPU\n");
        return 1;
    }

    VkPhysicalDevice16BitStorageFeatures s16 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES};
    VkPhysicalDeviceShaderFloat16Int8Features f16 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES, &s16};
    VkPhysicalDeviceVulkan12Features v12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &f16};
    VkPhysicalDeviceVulkan11Features v11 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &v12};
    VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &v11};
    vkGetPhysicalDeviceFeatures2(pd, &f2);
    printf("features: v12.shaderFloat16=%u ext.shaderFloat16=%u v11.storageBuffer16BitAccess=%u v11.uniformAndStorageBuffer16BitAccess=%u "
           "v11.storagePushConstant16=%u ext.storageBuffer16BitAccess=%u v12.shaderInt8=%u ext.shaderInt8=%u shaderInt16=%u\n",
           v12.shaderFloat16, f16.shaderFloat16, v11.storageBuffer16BitAccess, v11.uniformAndStorageBuffer16BitAccess, v11.storagePushConstant16,
           s16.storageBuffer16BitAccess, v12.shaderInt8, f16.shaderInt8, f2.features.shaderInt16);

    VkPhysicalDeviceVulkan12Properties p12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES};
    VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &p12};
    vkGetPhysicalDeviceProperties2(pd, &p2);
    printf("properties: %s: shaderDenormPreserveFloat16=%u shaderRoundingModeRTEFloat16=%u shaderDenormPreserveFloat32=%u\n",
           p2.properties.deviceName, p12.shaderDenormPreserveFloat16, p12.shaderRoundingModeRTEFloat16, p12.shaderDenormPreserveFloat32);

    int ok = 1;
    if (dxvk) {
        uint32_t nq = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, NULL);
        VkResult r = VK_ERROR_FEATURE_NOT_PRESENT;
        if (v11.storageBuffer16BitAccess && nq) {
            float prio = 1.0f;
            VkDeviceQueueCreateInfo qi = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, 0, 1, &prio};
            VkPhysicalDeviceVulkan12Features e12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
            VkPhysicalDeviceVulkan11Features e11 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &e12};
            VkPhysicalDeviceFeatures2 ef = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &e11};
            e11.storageBuffer16BitAccess = VK_TRUE;
            e11.uniformAndStorageBuffer16BitAccess = v11.uniformAndStorageBuffer16BitAccess;
            e12.shaderFloat16 = v12.shaderFloat16;
            e12.shaderInt8 = v12.shaderInt8;
            ef.features.shaderInt16 = f2.features.shaderInt16;
            VkDeviceCreateInfo di = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &ef, 0, 1, &qi};
            VkDevice dev = VK_NULL_HANDLE;
            r = vkCreateDevice(pd, &di, NULL, &dev);
            if (dev) {
                PFN_vkDestroyDevice vkDestroyDevice = (PFN_vkDestroyDevice)gipa(inst, "vkDestroyDevice");
                vkDestroyDevice(dev, NULL);
            }
        }
        printf("dxvk: storageBuffer16BitAccess (required) %s, shaderFloat16 (optional) %s; device: %d\n",
               v11.storageBuffer16BitAccess ? "offered" : "MISSING: adapter skipped", v12.shaderFloat16 ? "on" : "off", r);
        ok = r == VK_SUCCESS;
    }
    if (enable) {
        uint32_t nq = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, NULL);
        float prio = 1.0f;
        VkDeviceQueueCreateInfo qi = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, 0, 1, &prio};
        VkPhysicalDeviceVulkan12Features e12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        VkPhysicalDeviceVulkan11Features e11 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &e12};
        VkPhysicalDeviceFeatures2 ef = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &e11};
        e12.shaderFloat16 = VK_TRUE;
        e11.storageBuffer16BitAccess = VK_TRUE;
        VkDeviceCreateInfo di = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &ef, 0, 1, &qi};
        VkDevice dev = VK_NULL_HANDLE;
        VkResult r = nq ? vkCreateDevice(pd, &di, NULL, &dev) : VK_ERROR_INITIALIZATION_FAILED;
        printf("device: shaderFloat16 and storageBuffer16BitAccess enabled: %d\n", r);
        ok = r == VK_SUCCESS;
        if (dev) {
            PFN_vkDestroyDevice vkDestroyDevice = (PFN_vkDestroyDevice)gipa(inst, "vkDestroyDevice");
            vkDestroyDevice(dev, NULL);
        }
    }
    vkDestroyInstance(inst, NULL);
    return ok ? 0 : 1;
}
