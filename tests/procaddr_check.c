/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * procaddr_check: what vkGetInstanceProcAddr and vkGetDeviceProcAddr give for extension commands,
 * and that calling what they give is safe. Lee Gao's finding (Droid-Deck/DroidDeck#165): on a Mali
 * r49p1 setup vkGetInstanceProcAddr gave a non-NULL vkGetPhysicalDeviceCalibrateableTimeDomainsKHR
 * for a device without the extension, the call crashed, and so did Steam (through Zink).
 *   1  physical-device commands of device extensions the device does not have (VK_ARM_tensors,
 *      VK_ARM_performance_counters_by_region; and calibrated timestamps where the server hides or
 *      lacks them): vkGetInstanceProcAddr may give them (the loader-ICD interface), and a call must
 *      not crash, not report a lost device and leave nothing undefined in its outputs
 *   2  vkGetDeviceProcAddr: NULL for commands of extensions not enabled, non-NULL for those of the
 *      enabled ones under the name of the extension enabled (VK_KHR_calibrated_timestamps alone:
 *      vkGetCalibratedTimestampsKHR, not the EXT name), NULL for core commands newer than the device
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

static int failures, checks;

static void check(int ok, const char *what, const char *detail)
{
    checks++;
    if (!ok) failures++;
    printf("%s %s%s%s\n", ok ? "PASS" : "FAIL", what, detail && *detail ? ": " : "", detail ? detail : "");
    fflush(stdout);
}

static int has_ext(const VkExtensionProperties *e, uint32_t n, const char *name)
{
    for (uint32_t i = 0; i < n; i++)
        if (!strcmp(e[i].extensionName, name)) return 1;
    return 0;
}

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
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "procaddr_check", 1, NULL, 0, VK_API_VERSION_1_1};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app};
    if (vkCreateInstance(&ici, NULL, &inst) != VK_SUCCESS) {
        printf("FAIL instance\n");
        return 1;
    }
    GI(vkEnumeratePhysicalDevices);
    GI(vkEnumerateDeviceExtensionProperties);
    GI(vkGetPhysicalDeviceProperties);
    GI(vkCreateDevice);
    GI(vkGetDeviceProcAddr);
    uint32_t n = 1;
    VkPhysicalDevice pd;
    vkEnumeratePhysicalDevices(inst, &n, &pd);
    uint32_t ne = 0;
    vkEnumerateDeviceExtensionProperties(pd, NULL, &ne, NULL);
    VkExtensionProperties *ex = calloc(ne + 1, sizeof(*ex));
    vkEnumerateDeviceExtensionProperties(pd, NULL, &ne, ex);
    int cal_khr = has_ext(ex, ne, "VK_KHR_calibrated_timestamps"), cal_ext = has_ext(ex, ne, "VK_EXT_calibrated_timestamps");
    printf("device: VK_KHR_calibrated_timestamps %s, VK_EXT_calibrated_timestamps %s\n", cal_khr ? "yes" : "no", cal_ext ? "yes" : "no");
    char d[200];

    /* 1: physical-device commands of extensions the device lacks */
    if (!has_ext(ex, ne, "VK_ARM_tensors")) {
        PFN_vkGetPhysicalDeviceExternalTensorPropertiesARM f =
            (PFN_vkGetPhysicalDeviceExternalTensorPropertiesARM)gipa(inst, "vkGetPhysicalDeviceExternalTensorPropertiesARM");
        if (f) {
            VkTensorDescriptionARM td = {VK_STRUCTURE_TYPE_TENSOR_DESCRIPTION_ARM, NULL, VK_TENSOR_TILING_LINEAR_ARM, VK_FORMAT_R8_UNORM, 1,
                                         (const int64_t[]){16}, NULL, VK_TENSOR_USAGE_SHADER_BIT_ARM};
            VkPhysicalDeviceExternalTensorInfoARM ti = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_TENSOR_INFO_ARM, NULL, 0, &td,
                                                        VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
            VkExternalTensorPropertiesARM tp;
            memset(&tp, 0xcd, sizeof(tp));
            tp.sType = VK_STRUCTURE_TYPE_EXTERNAL_TENSOR_PROPERTIES_ARM;
            tp.pNext = NULL;
            f(pd, &ti, &tp);
            snprintf(d, sizeof(d), "features 0x%x, export-from 0x%x, compatible 0x%x", tp.externalMemoryProperties.externalMemoryFeatures,
                     tp.externalMemoryProperties.exportFromImportedHandleTypes, tp.externalMemoryProperties.compatibleHandleTypes);
            check(!tp.externalMemoryProperties.externalMemoryFeatures && !tp.externalMemoryProperties.exportFromImportedHandleTypes &&
                      !tp.externalMemoryProperties.compatibleHandleTypes,
                  "vkGetPhysicalDeviceExternalTensorPropertiesARM without the extension: nothing supported", d);
        }
    }
    if (!has_ext(ex, ne, "VK_ARM_performance_counters_by_region")) {
        PFN_vkEnumeratePhysicalDeviceQueueFamilyPerformanceCountersByRegionARM f =
            (PFN_vkEnumeratePhysicalDeviceQueueFamilyPerformanceCountersByRegionARM)gipa(
                inst, "vkEnumeratePhysicalDeviceQueueFamilyPerformanceCountersByRegionARM");
        if (f) {
            uint32_t cnt = 77;
            VkResult r = f(pd, 0, &cnt, NULL, NULL);
            snprintf(d, sizeof(d), "VkResult %d, count %u", r, cnt);
            check(r != VK_ERROR_DEVICE_LOST && (r < 0 || cnt == 0),
                  "vkEnumeratePhysicalDeviceQueueFamilyPerformanceCountersByRegionARM without the extension: none", d);
        }
    }
    {
        PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsKHR f =
            (PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsKHR)gipa(inst, "vkGetPhysicalDeviceCalibrateableTimeDomainsKHR");
        if (f) {
            uint32_t cnt = 77;
            VkResult r = f(pd, &cnt, NULL);
            snprintf(d, sizeof(d), "VkResult %d, count %u (extension %s)", r, cnt, cal_khr || cal_ext ? "offered" : "not offered");
            check(r != VK_ERROR_DEVICE_LOST && (r < 0 || cnt != 77), "vkGetPhysicalDeviceCalibrateableTimeDomainsKHR can be called", d);
        }
    }

    /* 2: vkGetDeviceProcAddr and the enabled extensions */
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(pd, &props);
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, 0, 1, &prio};
    const char *one[1];
    uint32_t nx = 0;
    if (cal_khr) one[nx++] = "VK_KHR_calibrated_timestamps";
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &qci, 0, NULL, nx, one};
    VkDevice dev;
    VkResult r = vkCreateDevice(pd, &dci, NULL, &dev);
    if (r != VK_SUCCESS) {
        snprintf(d, sizeof(d), "VkResult %d", r);
        check(0, "vkCreateDevice", d);
        return 1;
    }
    PFN_vkVoidFunction p;
    if (cal_khr) {
        p = vkGetDeviceProcAddr(dev, "vkGetCalibratedTimestampsKHR");
        check(p != NULL, "vkGetDeviceProcAddr(vkGetCalibratedTimestampsKHR), VK_KHR_calibrated_timestamps enabled: non-NULL", "");
        p = vkGetDeviceProcAddr(dev, "vkGetCalibratedTimestampsEXT");
        check(p == NULL, "vkGetDeviceProcAddr(vkGetCalibratedTimestampsEXT), only the KHR extension enabled: NULL", "");
    } else {
        p = vkGetDeviceProcAddr(dev, "vkGetCalibratedTimestampsKHR");
        check(p == NULL, "vkGetDeviceProcAddr(vkGetCalibratedTimestampsKHR), not enabled: NULL", "");
    }
    static const char *const not_enabled[] = {"vkCmdDrawMultiEXT", "vkCmdPushDescriptorSetKHR", "vkCmdSetLineStippleEXT", "vkGetMemoryFdKHR",
                                              "vkCmdBeginTransformFeedbackEXT", "vkCreateSwapchainKHR"};
    for (size_t i = 0; i < sizeof(not_enabled) / sizeof(not_enabled[0]); i++) {
        p = vkGetDeviceProcAddr(dev, not_enabled[i]);
        snprintf(d, sizeof(d), "vkGetDeviceProcAddr(%s), extension not enabled: NULL", not_enabled[i]);
        check(p == NULL, d, "");
    }
    p = vkGetDeviceProcAddr(dev, "vkCmdSetCullMode");
    check(p == NULL, "vkGetDeviceProcAddr(vkCmdSetCullMode), Vulkan 1.3 core, app asked for 1.1: NULL", "");
    p = vkGetDeviceProcAddr(dev, "vkGetPhysicalDeviceCalibrateableTimeDomainsKHR");
    check(p == NULL, "vkGetDeviceProcAddr of a physical-device command: NULL", "");
    PFN_vkDestroyDevice destroy = (PFN_vkDestroyDevice)vkGetDeviceProcAddr(dev, "vkDestroyDevice");
    destroy(dev, NULL);
    printf("%s: %d of %d checks passed\n", failures ? "FAILED" : "OK", checks - failures, checks);
    return failures ? 1 : 0;
}
