/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * membudget_check: VK_EXT_memory_budget as an app sees it through the bridge (host/membudget_test.sh).
 * The instance's engine name is MBC_ENGINE ("DXVK" by default, "-" for none). If the extension is
 * offered, a device is made with it enabled and MBC_ALLOC_MB of device-local memory allocated, then for
 * each MemAvailable in MBC_STEPS (MB, comma separated) the value is written to MBC_AVAIL_FILE (the
 * server's VKBRIDGE_FAKE_MEMAVAIL_FILE) and the budget read. Prints:
 *   ext=<yes|no>
 *   device=<ok|fail N|none>
 *   step avail=<MB> heap=<i> size_mb=<n> local=<0|1> budget_mb=<n> usage_mb=<n>
 *   rt_us=<average microseconds of one MemoryProperties2 query with the budget, 200 queries>
 * Exit status 0 unless a Vulkan call failed.
 */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
    const char *engine = getenv("MBC_ENGINE");
    if (!engine) engine = "DXVK";
    if (!strcmp(engine, "-")) engine = NULL;
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "membudget_check", 1, engine, 0, VK_API_VERSION_1_2};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app};
    if (vkCreateInstance(&ici, NULL, &inst) != VK_SUCCESS) {
        printf("FAIL instance\n");
        return 1;
    }
    GI(vkEnumeratePhysicalDevices);
    GI(vkGetPhysicalDeviceMemoryProperties);
    GI(vkGetPhysicalDeviceMemoryProperties2);
    GI(vkEnumerateDeviceExtensionProperties);
    GI(vkCreateDevice);
    GI(vkGetDeviceProcAddr);
    uint32_t n = 1;
    VkPhysicalDevice pd;
    if (vkEnumeratePhysicalDevices(inst, &n, &pd) < 0 || !n) {
        printf("FAIL no physical device\n");
        return 1;
    }
    uint32_t ne = 0;
    vkEnumerateDeviceExtensionProperties(pd, NULL, &ne, NULL);
    VkExtensionProperties ext[512];
    if (ne > 512) ne = 512;
    vkEnumerateDeviceExtensionProperties(pd, NULL, &ne, ext);
    int has = 0;
    for (uint32_t i = 0; i < ne; i++) has |= !strcmp(ext[i].extensionName, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
    printf("ext=%s\n", has ? "yes" : "no");
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    int bad = 0;

    VkDevice dev = NULL;
    if (has) {
        float prio = 1.0f;
        VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, 0, 1, &prio};
        const char *names[] = {VK_EXT_MEMORY_BUDGET_EXTENSION_NAME};
        VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &qci, 0, NULL, 1, names, NULL};
        VkResult r = vkCreateDevice(pd, &dci, NULL, &dev);
        if (r != VK_SUCCESS) {
            printf("device=fail %d\n", r);
            dev = NULL;
            bad++;
        } else {
            printf("device=ok\n");
        }
    } else {
        printf("device=none\n");
    }
    const char *am = getenv("MBC_ALLOC_MB");
    uint64_t alloc_mb = am ? strtoull(am, NULL, 10) : 0;
    if (dev && alloc_mb) {
        PFN_vkAllocateMemory alloc = (PFN_vkAllocateMemory)vkGetDeviceProcAddr(dev, "vkAllocateMemory");
        uint32_t type = 0;
        for (uint32_t t = 0; t < mp.memoryTypeCount; t++)
            if (mp.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) {
                type = t;
                break;
            }
        for (uint64_t i = 0; i < alloc_mb / 64; i++) {
            VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, 64ull << 20, type};
            VkDeviceMemory m;
            if (alloc(dev, &ai, NULL, &m) != VK_SUCCESS) {
                printf("FAIL alloc %llu\n", (unsigned long long)i);
                bad++;
                break;
            }
        }
    }

    VkPhysicalDeviceMemoryBudgetPropertiesEXT bud = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
    VkPhysicalDeviceMemoryProperties2 m2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2, &bud};
    const char *steps = getenv("MBC_STEPS"), *file = getenv("MBC_AVAIL_FILE");
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", steps && *steps ? steps : "0");
    for (char *tok = strtok(buf, ","); has && tok; tok = strtok(NULL, ",")) {
        if (file) {
            FILE *f = fopen(file, "w");
            if (f) {
                fprintf(f, "%s\n", tok);
                fclose(f);
            }
        }
        memset(bud.heapBudget, 0, sizeof(bud.heapBudget));
        memset(bud.heapUsage, 0, sizeof(bud.heapUsage));
        vkGetPhysicalDeviceMemoryProperties2(pd, &m2);
        for (uint32_t h = 0; h < m2.memoryProperties.memoryHeapCount; h++)
            printf("step avail=%s heap=%u size_mb=%llu local=%d budget_mb=%llu usage_mb=%llu\n", tok, h,
                   (unsigned long long)(m2.memoryProperties.memoryHeaps[h].size >> 20),
                   !!(m2.memoryProperties.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT),
                   (unsigned long long)(bud.heapBudget[h] >> 20), (unsigned long long)(bud.heapUsage[h] >> 20));
    }
    if (has) {
        struct timespec a, b;
        clock_gettime(CLOCK_MONOTONIC, &a);
        for (int i = 0; i < 200; i++) vkGetPhysicalDeviceMemoryProperties2(pd, &m2);
        clock_gettime(CLOCK_MONOTONIC, &b);
        printf("rt_us=%.1f\n", ((b.tv_sec - a.tv_sec) * 1e9 + (b.tv_nsec - a.tv_nsec)) / 200.0 / 1000.0);
    }
    /* the instance, the device and its memory go with the process (the server frees a closed connection's objects) */
    return bad ? 1 : 0;
}
