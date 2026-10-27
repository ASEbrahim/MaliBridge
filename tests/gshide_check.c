/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * gshide_check: what the first GPU offers of geometry shaders (gs_hide, issue #11:
 * server_errata_gs.c), printed for host/gshide_test.sh to compare:
 *   features: geometryShader (1.0 and Features2), transform feedback's geometryStreams
 *   limits: maxGeometryOutputVertices, maxGeometryShaderInvocations (1.0 and Properties2)
 *   stages: whether the subgroup stages list the geometry stage (Vulkan 1.1 properties)
 *   device: a device made with geometryShader enabled whatever was offered - an app that enables
 *           it anyway (the server takes it off where it is hidden; the result is printed)
 * The process name (comm) is what the server matches against VKBRIDGE_GS_HIDE_PROCS.
 * Exit status 0 when the queries ran and the device was made.
 */
#define _GNU_SOURCE
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
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "gshide_check", 1, NULL, 0, VK_API_VERSION_1_2};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app};
    if (vkCreateInstance(&ici, NULL, &inst) != VK_SUCCESS) {
        printf("FAIL instance\n");
        return 1;
    }
    GI(vkEnumeratePhysicalDevices);
    GI(vkGetPhysicalDeviceFeatures);
    GI(vkGetPhysicalDeviceFeatures2);
    GI(vkGetPhysicalDeviceProperties);
    GI(vkGetPhysicalDeviceProperties2);
    GI(vkCreateDevice);
    GI(vkDestroyInstance);
    uint32_t n = 1;
    VkPhysicalDevice pd = VK_NULL_HANDLE;
    if (vkEnumeratePhysicalDevices(inst, &n, &pd) < 0 || !n || !pd) {
        printf("FAIL no GPU\n");
        return 1;
    }
    VkPhysicalDeviceFeatures f1;
    vkGetPhysicalDeviceFeatures(pd, &f1);
    VkPhysicalDeviceTransformFeedbackFeaturesEXT xfb = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT};
    VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &xfb};
    vkGetPhysicalDeviceFeatures2(pd, &f2);
    printf("features: v10.geometryShader=%u f2.geometryShader=%u geometryStreams=%u tessellationShader=%u\n", f1.geometryShader,
           f2.features.geometryShader, xfb.geometryStreams, f2.features.tessellationShader);

    VkPhysicalDeviceProperties p1;
    vkGetPhysicalDeviceProperties(pd, &p1);
    VkPhysicalDeviceVulkan11Properties v11 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES};
    VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &v11};
    vkGetPhysicalDeviceProperties2(pd, &p2);
    printf("limits: v10.maxGeometryOutputVertices=%u p2.maxGeometryOutputVertices=%u maxGeometryShaderInvocations=%u\n",
           p1.limits.maxGeometryOutputVertices, p2.properties.limits.maxGeometryOutputVertices,
           p2.properties.limits.maxGeometryShaderInvocations);
    printf("stages: subgroupGeometry=%u subgroupVertex=%u\n", !!(v11.subgroupSupportedStages & VK_SHADER_STAGE_GEOMETRY_BIT),
           !!(v11.subgroupSupportedStages & VK_SHADER_STAGE_VERTEX_BIT));

    float prio = 1;
    VkDeviceQueueCreateInfo q = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, 0, 1, &prio};
    VkPhysicalDeviceFeatures want = {0};
    want.geometryShader = VK_TRUE;
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &q};
    dci.pEnabledFeatures = &want;
    VkDevice dev = VK_NULL_HANDLE;
    VkResult r = vkCreateDevice(pd, &dci, NULL, &dev);
    printf("device: geometryShader enabled: %d\n", r);
    if (r == VK_SUCCESS) {
        PFN_vkDestroyDevice vkDestroyDevice = (PFN_vkDestroyDevice)gipa(inst, "vkDestroyDevice");
        vkDestroyDevice(dev, NULL);
    }
    vkDestroyInstance(inst, NULL);
    return r == VK_SUCCESS ? 0 : 1;
}
