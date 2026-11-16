/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * pvmutable_check: PV-02 (PowerVR: the image-format query for a LINEAR, dma-buf shareable,
 * mutable-format RGBA8 / BGRA8 image is refused though the image is made). The query as gamescope
 * asks it (DRM format modifier tiling, the LINEAR modifier, DMA_BUF external memory, a UNORM / sRGB
 * view format list) for both formats, the image made the same way (vkCreateImage with the modifier
 * list), and two controls: the query without the mutable flag, and a mutable query for a format
 * outside the pattern (RGBA16F), whose answers must not change whatever the fix does.
 *   pvmutable_check --expect yes|no   the pattern queries must succeed (yes) / be refused (no)
 *   pvmutable_check --gamescope       gamescope's own flippable-image query as the Pixel 10 Pro XL's
 *                                     session showed it (a player log session.log:99-100): B8G8R8A8_UNORM, 2D,
 *                                     DRM-modifier tiling, usage 0x6, MUTABLE_FORMAT, a 2-format view list,
 *                                     DMA_BUF, once per modifier of {LINEAR, 0x00ffffffffffffff}; the modifiers
 *                                     answered yes are kept (CVulkanTexture::BInit) and the image is made with
 *                                     them. No modifier kept = gamescope's "modifiers.size() > 0" abort: FAIL.
 * host/pvmutable_test.sh runs it through the bridge with PowerVR's refusal imitated
 * (VKBRIDGE_FAKE_MISSING=pvmutable) and with the fix over it.
 */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FAIL(...) (printf("FAIL: " __VA_ARGS__), printf("\n"), 1)

int main(int argc, char **argv)
{
    int expect = -1, gamescope = 0;
    for (int i = 1; i < argc; i++) {
        if (i + 1 < argc && !strcmp(argv[i], "--expect")) expect = !strcmp(argv[i + 1], "yes");
        if (!strcmp(argv[i], "--gamescope")) gamescope = 1;
    }
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib) return FAIL("no libvulkan.so.1");
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
#define GI(n) PFN_##n n = (PFN_##n)gipa(inst, #n)
    VkInstance inst = VK_NULL_HANDLE;
    GI(vkCreateInstance);
    VkApplicationInfo ai = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "pvmutable_check", 1, NULL, 0, VK_API_VERSION_1_2};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &ai};
    if (vkCreateInstance(&ici, NULL, &inst)) return FAIL("instance");
    GI(vkEnumeratePhysicalDevices);
    GI(vkEnumerateDeviceExtensionProperties);
    GI(vkGetPhysicalDeviceImageFormatProperties2);
    GI(vkCreateDevice);
    GI(vkGetDeviceProcAddr);
    uint32_t n = 1;
    VkPhysicalDevice pd;
    if (vkEnumeratePhysicalDevices(inst, &n, &pd) < 0 || !n) return FAIL("no physical device");
    const char *exts[3] = {"VK_EXT_image_drm_format_modifier", "VK_KHR_external_memory_fd", "VK_EXT_external_memory_dma_buf"};
    uint32_t ne = 0, have = 0;
    vkEnumerateDeviceExtensionProperties(pd, NULL, &ne, NULL);
    VkExtensionProperties *ep = calloc(ne ? ne : 1, sizeof(*ep));
    vkEnumerateDeviceExtensionProperties(pd, NULL, &ne, ep);
    for (uint32_t i = 0; i < ne; i++)
        for (int k = 0; k < 3; k++) have += !strcmp(ep[i].extensionName, exts[k]);
    free(ep);
    if (have < 3) {
        printf("SKIP: no DRM format modifiers with dma-bufs\n");
        return 0;
    }
    float prio = 1;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, 0, 1, &prio};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &qci, 0, NULL, 3, exts};
    VkDevice dev;
    if (vkCreateDevice(pd, &dci, NULL, &dev)) return FAIL("device");
    PFN_vkCreateImage vkCreateImage = (PFN_vkCreateImage)vkGetDeviceProcAddr(dev, "vkCreateImage");
    PFN_vkDestroyImage vkDestroyImage = (PFN_vkDestroyImage)vkGetDeviceProcAddr(dev, "vkDestroyImage");
    PFN_vkDestroyDevice vkDestroyDevice = (PFN_vkDestroyDevice)vkGetDeviceProcAddr(dev, "vkDestroyDevice");

    if (gamescope) {
        /* gamescope 3.16 CVulkanTexture::BInit for a flippable image: every modifier of the format's list is
         * asked with the image's own create info; those answered yes are kept, then asserted non-empty */
        const VkFormat list[2] = {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_SRGB};
        const uint64_t mods[2] = {0 /* DRM_FORMAT_MOD_LINEAR */, 0x00ffffffffffffffull};
        uint64_t kept[2];
        uint32_t nkept = 0;
        char gl[200] = "";
        for (int m = 0; m < 2; m++) {
            VkImageFormatListCreateInfo fl = {VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO, NULL, 2, list};
            VkPhysicalDeviceImageDrmFormatModifierInfoEXT mi = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT, &fl, mods[m],
                                                                VK_SHARING_MODE_EXCLUSIVE};
            VkPhysicalDeviceExternalImageFormatInfo ei = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO, &mi,
                                                          VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
            VkPhysicalDeviceImageFormatInfo2 ii = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2, &ei, VK_FORMAT_B8G8R8A8_UNORM,
                                                   VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
                                                   VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT /* 0x6 */,
                                                   VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT};
            VkExternalImageFormatProperties xp = {VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
            VkImageFormatProperties2 ip = {VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, &xp};
            VkResult q = vkGetPhysicalDeviceImageFormatProperties2(pd, &ii, &ip);
            snprintf(gl + strlen(gl), sizeof(gl) - strlen(gl), "modifier 0x%llx query %d; ", (unsigned long long)mods[m], q);
            if (q == VK_SUCCESS) kept[nkept++] = mods[m];
        }
        if (!nkept) return FAIL("%sno modifier kept: gamescope aborts (\"modifiers.size() > 0\", rendervulkan.cpp:2272)", gl);
        VkImageFormatListCreateInfo fl = {VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO, NULL, 2, list};
        VkImageDrmFormatModifierListCreateInfoEXT ml = {VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT, &fl, nkept, kept};
        VkExternalMemoryImageCreateInfo emi = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, &ml, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
        VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &emi, VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT, VK_IMAGE_TYPE_2D, VK_FORMAT_B8G8R8A8_UNORM,
                                {64, 64, 1}, 1, 1, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
                                VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
        VkImage img;
        VkResult cr = vkCreateImage(dev, &ci, NULL, &img);
        if (cr != VK_SUCCESS) return FAIL("%s%u modifier(s) kept, image %d", gl, nkept, cr);
        vkDestroyImage(dev, img, NULL);
        vkDestroyDevice(dev, NULL);
        printf("PASS: %s%u modifier(s) kept, image made\n", gl, nkept);
        return 0;
    }

    static const struct {
        VkFormat f, view;
        const char *what;
    } cases[2] = {{VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB, "RGBA8"}, {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_SRGB, "BGRA8"}};
    const VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    int bad = 0;
    char line[256] = "";
    for (int k = 0; k < 2; k++) {
        const VkFormat list[2] = {cases[k].f, cases[k].view};
        const uint64_t linear = 0;
        VkImageFormatListCreateInfo fl = {VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO, NULL, 2, list};
        VkPhysicalDeviceImageDrmFormatModifierInfoEXT mi = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT, &fl, 0,
                                                            VK_SHARING_MODE_EXCLUSIVE};
        VkPhysicalDeviceExternalImageFormatInfo ei = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO, &mi,
                                                      VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
        VkPhysicalDeviceImageFormatInfo2 ii = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2, &ei, cases[k].f, VK_IMAGE_TYPE_2D,
                                               VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT, usage, VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT};
        VkExternalImageFormatProperties xp = {VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
        VkImageFormatProperties2 ip = {VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, &xp};
        VkResult q = vkGetPhysicalDeviceImageFormatProperties2(pd, &ii, &ip);
        VkImageDrmFormatModifierListCreateInfoEXT ml = {VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT, &fl, 1, &linear};
        VkExternalMemoryImageCreateInfo emi = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, &ml, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
        VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &emi, VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT, VK_IMAGE_TYPE_2D, cases[k].f,
                                {16, 16, 1}, 1, 1, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT, usage, VK_SHARING_MODE_EXCLUSIVE, 0,
                                NULL, VK_IMAGE_LAYOUT_UNDEFINED};
        VkImage img;
        VkResult cr = vkCreateImage(dev, &ci, NULL, &img);
        snprintf(line + strlen(line), sizeof(line) - strlen(line), "%s query %d, image %d; ", cases[k].what, q, cr);
        if (expect >= 0 && (q == VK_SUCCESS) != expect) bad++;
        if (cr != VK_SUCCESS) bad++; /* the image must be made whatever the query says */
        if (q == VK_SUCCESS && !(xp.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT)) bad++;
        /* control: the same query without the mutable flag (the base format's answer) */
        ii.flags = 0;
        VkResult base = vkGetPhysicalDeviceImageFormatProperties2(pd, &ii, &ip);
        if (base != VK_SUCCESS) bad++, snprintf(line + strlen(line), sizeof(line) - strlen(line), "base query %d; ", base);
    }
    /* control: a mutable query outside the pattern (RGBA16F): whatever the driver says, the fix keeps */
    {
        VkPhysicalDeviceImageFormatInfo2 ii = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2, NULL, VK_FORMAT_R16G16B16A16_SFLOAT,
                                               VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, usage, VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT};
        VkImageFormatProperties2 ip = {VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
        VkResult o = vkGetPhysicalDeviceImageFormatProperties2(pd, &ii, &ip);
        snprintf(line + strlen(line), sizeof(line) - strlen(line), "RGBA16F mutable query %d", o);
        if (o != VK_SUCCESS) bad++;
    }
    if (bad) return FAIL("%s", line);
    printf("PASS: %s\n", line);
    return 0;
}
