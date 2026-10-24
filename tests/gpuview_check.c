/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * gpuview_check: the GPU as an app sees it - identity (vkGetPhysicalDeviceProperties and
 * Properties2 with VkPhysicalDeviceDriverProperties) and memory heaps (MemoryProperties and
 * MemoryProperties2 with VK_EXT_memory_budget when the device has it). For VKBRIDGE_HEAP_MAX_MB and
 * VKBRIDGE_SPOOF_GPU (client/physdev.c); host/heapcap_test.sh reads the lines.
 *
 * Prints, for the first physical device:
 *   gpu vendor=0x.... device=0x.... name=<deviceName>
 *   driver id=<driverID> name=<driverName>
 *   heap <i> size_mb=<n> local=<0|1> budget_mb=<n|-> usage_mb=<n|->
 * and checks that the two forms of each query agree and that no budget is above its heap's size.
 * Exit status 0 only if they do.
 */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib) {
        printf("FAIL loader: %s\n", dlerror());
        return 1;
    }
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    VkInstance inst = NULL;
#define GI(name) PFN_##name name = (PFN_##name)gipa(inst, #name)
    GI(vkCreateInstance);
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "gpuview_check", 1, NULL, 0, VK_API_VERSION_1_2};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app};
    if (vkCreateInstance(&ici, NULL, &inst) != VK_SUCCESS) {
        printf("FAIL instance\n");
        return 1;
    }
    GI(vkEnumeratePhysicalDevices);
    GI(vkGetPhysicalDeviceProperties);
    GI(vkGetPhysicalDeviceProperties2);
    GI(vkGetPhysicalDeviceMemoryProperties);
    GI(vkGetPhysicalDeviceMemoryProperties2);
    GI(vkEnumerateDeviceExtensionProperties);
    GI(vkDestroyInstance);
    uint32_t n = 1;
    VkPhysicalDevice pd;
    if (vkEnumeratePhysicalDevices(inst, &n, &pd) < 0 || !n) {
        printf("FAIL no physical device\n");
        return 1;
    }
    int bad = 0;

    VkPhysicalDeviceProperties p1;
    vkGetPhysicalDeviceProperties(pd, &p1);
    VkPhysicalDeviceDriverProperties drv = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &drv};
    vkGetPhysicalDeviceProperties2(pd, &p2);
    printf("gpu vendor=0x%04x device=0x%04x name=%s\n", p1.vendorID, p1.deviceID, p1.deviceName);
    printf("driver id=%u name=%s\n", drv.driverID, drv.driverName);
    if (p1.vendorID != p2.properties.vendorID || p1.deviceID != p2.properties.deviceID || strcmp(p1.deviceName, p2.properties.deviceName)) {
        printf("FAIL Properties2 says vendor=0x%04x device=0x%04x name=%s\n", p2.properties.vendorID, p2.properties.deviceID,
               p2.properties.deviceName);
        bad++;
    }

    uint32_t ne = 0;
    vkEnumerateDeviceExtensionProperties(pd, NULL, &ne, NULL);
    VkExtensionProperties ext[512];
    if (ne > 512) ne = 512;
    vkEnumerateDeviceExtensionProperties(pd, NULL, &ne, ext);
    int has_budget = 0;
    for (uint32_t i = 0; i < ne; i++) has_budget |= !strcmp(ext[i].extensionName, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);

    VkPhysicalDeviceMemoryProperties m1;
    vkGetPhysicalDeviceMemoryProperties(pd, &m1);
    VkPhysicalDeviceMemoryBudgetPropertiesEXT bud = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
    VkPhysicalDeviceMemoryProperties2 m2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2, has_budget ? &bud : NULL};
    vkGetPhysicalDeviceMemoryProperties2(pd, &m2);
    if (m1.memoryHeapCount != m2.memoryProperties.memoryHeapCount) {
        printf("FAIL heap counts %u and %u\n", m1.memoryHeapCount, m2.memoryProperties.memoryHeapCount);
        bad++;
    }
    for (uint32_t h = 0; h < m1.memoryHeapCount; h++) {
        const VkMemoryHeap *a = &m1.memoryHeaps[h], *b = &m2.memoryProperties.memoryHeaps[h];
        char bs[32] = "-", us[32] = "-";
        if (has_budget) {
            snprintf(bs, sizeof(bs), "%llu", (unsigned long long)(bud.heapBudget[h] >> 20));
            snprintf(us, sizeof(us), "%llu", (unsigned long long)(bud.heapUsage[h] >> 20));
        }
        printf("heap %u size_mb=%llu local=%d budget_mb=%s usage_mb=%s\n", h, (unsigned long long)(a->size >> 20),
               !!(a->flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT), bs, us);
        if (a->size != b->size || a->flags != b->flags) {
            printf("FAIL heap %u: MemoryProperties2 says %llu bytes, flags 0x%x\n", h, (unsigned long long)b->size, b->flags);
            bad++;
        }
        if (has_budget && bud.heapBudget[h] > a->size) {
            printf("FAIL heap %u: budget %llu above its size %llu\n", h, (unsigned long long)bud.heapBudget[h], (unsigned long long)a->size);
            bad++;
        }
        if (has_budget && !bud.heapBudget[h]) {
            printf("FAIL heap %u: no budget\n", h);
            bad++;
        }
    }
    printf("budget extension: %s\n", has_budget ? "yes" : "no");
    vkDestroyInstance(inst, NULL);
    printf("%s: %u heaps\n", bad ? "FAILED" : "OK", m1.memoryHeapCount);
    return bad ? 1 : 0;
}
