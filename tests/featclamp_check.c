/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * featclamp_check: a device made the way DXVK 1.x (1.10.3) makes one at feature level 10_0 and up,
 * for host/featclamp_test.sh. A Vulkan 1.1 instance; transform feedback's features queried and
 * printed ("features: transformFeedback=N geometryStreams=N"); then vkCreateDevice with
 * pEnabledFeatures (not VkPhysicalDeviceFeatures2) and VK_EXT_transform_feedback, transformFeedback
 * and geometryStreams both on whatever the query said, as DXVK 1.x does (its adapter check looks at
 * transformFeedback only). The result is printed ("device: N").
 * Argument (issue #32, DXVK 1.10.3 also asks for shaderDrawParameters without checking it):
 *   drawparams  VkPhysicalDeviceShaderDrawParametersFeatures queried and printed
 *               ("features: shaderDrawParameters=N") and turned on in the device, with transform
 *               feedback as above when the GPU has it
 *   v11         a Vulkan 1.2 instance; VkPhysicalDeviceVulkan11Features queried and printed
 *               ("features: v11 shaderDrawParameters=N multiview=N"), the device made with only
 *               shaderDrawParameters turned on in it and multiview as the query said
 * Exit status: 0 the device was made, 1 it was refused, 77 the GPU has no VK_EXT_transform_feedback
 * (no argument only).
 */
#define _GNU_SOURCE
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "";
    int dp = !strcmp(mode, "drawparams"), v11 = !strcmp(mode, "v11");
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib) {
        printf("FAIL loader: %s\n", dlerror());
        return 1;
    }
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    VkInstance inst = NULL;
#define GI(name) PFN_##name name = (PFN_##name)gipa(inst, #name)
    GI(vkCreateInstance);
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "featclamp_check", 1, "DXVK", 1, v11 ? VK_API_VERSION_1_2 : VK_API_VERSION_1_1};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app};
    if (vkCreateInstance(&ici, NULL, &inst) != VK_SUCCESS) {
        printf("FAIL instance\n");
        return 1;
    }
    GI(vkEnumeratePhysicalDevices);
    GI(vkEnumerateDeviceExtensionProperties);
    GI(vkGetPhysicalDeviceFeatures2);
    GI(vkCreateDevice);
    GI(vkDestroyInstance);
    uint32_t n = 1;
    VkPhysicalDevice pd = VK_NULL_HANDLE;
    if (vkEnumeratePhysicalDevices(inst, &n, &pd) < 0 || !n || !pd) {
        printf("FAIL no GPU\n");
        return 1;
    }
    uint32_t en = 0;
    vkEnumerateDeviceExtensionProperties(pd, NULL, &en, NULL);
    VkExtensionProperties *e = calloc(en ? en : 1, sizeof(*e));
    vkEnumerateDeviceExtensionProperties(pd, NULL, &en, e);
    int xfb_ext = 0;
    for (uint32_t i = 0; i < en; i++) xfb_ext |= !strcmp(e[i].extensionName, VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME);
    free(e);
    if (!xfb_ext && !dp && !v11) {
        printf("SKIP: no VK_EXT_transform_feedback\n");
        vkDestroyInstance(inst, NULL);
        return 77;
    }

    VkPhysicalDeviceTransformFeedbackFeaturesEXT xfb = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT};
    VkPhysicalDeviceShaderDrawParametersFeatures sdp = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES};
    VkPhysicalDeviceVulkan11Features f11 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, v11 ? (void *)&f11 : dp ? (void *)&sdp : (void *)&xfb};
    if (dp && xfb_ext) sdp.pNext = &xfb;
    vkGetPhysicalDeviceFeatures2(pd, &f2);
    if (v11) printf("features: v11 shaderDrawParameters=%u multiview=%u\n", f11.shaderDrawParameters, f11.multiview);
    else if (dp) printf("features: shaderDrawParameters=%u\n", sdp.shaderDrawParameters);
    if (!v11 && xfb_ext) printf("features: transformFeedback=%u geometryStreams=%u\n", xfb.transformFeedback, xfb.geometryStreams);

    float prio = 1;
    VkDeviceQueueCreateInfo q = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, 0, 1, &prio};
    VkPhysicalDeviceFeatures want = {0};
    want.geometryShader = f2.features.geometryShader;
    want.robustBufferAccess = f2.features.robustBufferAccess;
    VkPhysicalDeviceTransformFeedbackFeaturesEXT exf = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT, NULL, VK_TRUE, VK_TRUE};
    VkPhysicalDeviceShaderDrawParametersFeatures esdp = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES, NULL, VK_TRUE};
    VkPhysicalDeviceVulkan11Features e11 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    e11.shaderDrawParameters = VK_TRUE;
    e11.multiview = f11.multiview;
    const char *ext[] = {VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME};
    void *chain = &exf;
    uint32_t next = xfb_ext;
    if (dp) {
        esdp.pNext = xfb_ext ? (void *)&exf : NULL;
        chain = &esdp;
    }
    if (v11) {
        chain = &e11;
        next = 0;
    }
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, chain, 0, 1, &q, 0, NULL, next, ext, &want};
    VkDevice dev = VK_NULL_HANDLE;
    VkResult r = vkCreateDevice(pd, &dci, NULL, &dev);
    printf("device: %d\n", r);
    if (r == VK_SUCCESS) {
        PFN_vkDestroyDevice vkDestroyDevice = (PFN_vkDestroyDevice)gipa(inst, "vkDestroyDevice");
        vkDestroyDevice(dev, NULL);
    }
    vkDestroyInstance(inst, NULL);
    return r == VK_SUCCESS ? 0 : 1;
}
