/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * zinkname_check [engine name]: makes an instance whose engine name is argv[1] (default "mesa zink",
 * what Zink uses) and prints what the first GPU offers of the features PowerVR's "mesa zink" profile
 * hides (pv_zink_name, PV-08: server_errata_zink.c):
 *   features: sparseBinding, vertexInputDynamicState, graphicsPipelineLibrary
 *   engine: the name the instance was made with
 * for host/zinkname_test.sh. Exit status 0 when the queries ran.
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
    const char *engine = argc > 1 ? argv[1] : "mesa zink";
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib) {
        printf("FAIL loader: %s\n", dlerror());
        return 1;
    }
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    VkInstance inst = NULL;
    PFN_vkCreateInstance create = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "zinkname_check", 1, engine, 1, VK_API_VERSION_1_1};
    VkInstanceCreateInfo ci = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 0, NULL};
    if (create(&ci, NULL, &inst) != VK_SUCCESS) {
        printf("FAIL vkCreateInstance\n");
        return 1;
    }
    PFN_vkEnumeratePhysicalDevices enumpd = (PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices");
    PFN_vkGetPhysicalDeviceFeatures2 feats2 = (PFN_vkGetPhysicalDeviceFeatures2)gipa(inst, "vkGetPhysicalDeviceFeatures2");
    PFN_vkEnumerateDeviceExtensionProperties exts = (PFN_vkEnumerateDeviceExtensionProperties)gipa(inst, "vkEnumerateDeviceExtensionProperties");
    uint32_t n = 1;
    VkPhysicalDevice pd;
    VkResult r = enumpd(inst, &n, &pd);
    if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || !n) {
        printf("FAIL no physical device\n");
        return 1;
    }
    uint32_t ne = 0;
    exts(pd, NULL, &ne, NULL);
    VkExtensionProperties *e = calloc(ne ? ne : 1, sizeof(*e));
    exts(pd, NULL, &ne, e);
    int vi_ext = 0, gpl_ext = 0;
    for (uint32_t i = 0; i < ne; i++) {
        vi_ext |= !strcmp(e[i].extensionName, "VK_EXT_vertex_input_dynamic_state");
        gpl_ext |= !strcmp(e[i].extensionName, "VK_EXT_graphics_pipeline_library");
    }
    VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT vi = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT};
    VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT gp = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT};
    VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    if (vi_ext) vi.pNext = f2.pNext, f2.pNext = &vi;
    if (gpl_ext) gp.pNext = f2.pNext, f2.pNext = &gp;
    feats2(pd, &f2);
    printf("engine: %s\n", engine);
    printf("features: sparseBinding=%u vertexInputDynamicState=%u graphicsPipelineLibrary=%u extVI=%d extGPL=%d\n", f2.features.sparseBinding,
           vi.vertexInputDynamicState, gp.graphicsPipelineLibrary, vi_ext, gpl_ext);
    return 0;
}
