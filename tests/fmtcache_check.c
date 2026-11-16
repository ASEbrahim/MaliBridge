/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * fmtcache_check: the format properties the client asks the server for while it describes emulated formats
 * (BC without the driver's support; BGR10/UF11 vertex formats) - DXVK asks the v2 query for every BC image it
 * makes, ~1,370 round trips in 11 s of a Sekiro load - are asked once and answered from the client's cache
 * after: every answer of rounds 1.. equals round 0's, byte for byte, for the v1 query, v2 with no chain, v2
 * with VkFormatProperties3, and v2 with a chain naming a struct the cache does not know (never kept).
 * The round trips are counted by the script (VKBRIDGE_STATS: "stats at exit" lists commands over 100).
 * One line per check, "OK: ..." / "FAILED: ..." last.
 */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

static int failures;

static void judge(const char *what, int ok, int a, int b)
{
    if (!ok) failures++;
    printf("%s %s (%d, %d)\n", ok ? "PASS" : "FAIL", what, a, b);
    fflush(stdout);
}

int main(void)
{
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib) {
        printf("FAILED: loader: %s\n", dlerror());
        return 2;
    }
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    PFN_vkCreateInstance ci = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "vkbridge-fmtcache-check", 1, "vkbridge", 1, VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 0, NULL};
    VkInstance inst;
    if (ci(&ici, NULL, &inst)) {
        printf("FAILED: vkCreateInstance\n");
        return 1;
    }
    PFN_vkEnumeratePhysicalDevices enumpd = (PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices");
    PFN_vkGetPhysicalDeviceFormatProperties fp1 = (PFN_vkGetPhysicalDeviceFormatProperties)gipa(inst, "vkGetPhysicalDeviceFormatProperties");
    PFN_vkGetPhysicalDeviceFormatProperties2 fp2 = (PFN_vkGetPhysicalDeviceFormatProperties2)gipa(inst, "vkGetPhysicalDeviceFormatProperties2");
    uint32_t n = 1;
    VkPhysicalDevice pd;
    if (enumpd(inst, &n, &pd) < 0 || !n) {
        printf("FAILED: no physical device\n");
        return 1;
    }
    static const VkFormat fmts[] = {VK_FORMAT_BC1_RGBA_UNORM_BLOCK, VK_FORMAT_BC1_RGB_SRGB_BLOCK, VK_FORMAT_BC3_UNORM_BLOCK,
                                    VK_FORMAT_BC4_UNORM_BLOCK, VK_FORMAT_BC5_UNORM_BLOCK, VK_FORMAT_BC6H_UFLOAT_BLOCK,
                                    VK_FORMAT_BC7_UNORM_BLOCK, VK_FORMAT_BC7_SRGB_BLOCK, VK_FORMAT_R8G8B8A8_UNORM,
                                    VK_FORMAT_D32_SFLOAT, VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_FORMAT_B10G11R11_UFLOAT_PACK32};
    enum { NF = sizeof(fmts) / sizeof(fmts[0]), ROUNDS = 250 };
    static VkFormatProperties r1[NF];
    static VkFormatProperties2 r2[NF];
    static VkFormatProperties3 r3[NF], r3b[NF];
    static VkFormatProperties r2p[NF];
    int bad1 = 0, bad2 = 0, bad3 = 0, badu = 0, bc_optimal = 0;
    for (int round = 0; round < ROUNDS; round++)
        for (int i = 0; i < NF; i++) {
            VkFormatProperties a;
            memset(&a, 0xaa, sizeof(a));
            fp1(pd, fmts[i], &a);
            VkFormatProperties2 b = {VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2};
            memset(&b.formatProperties, 0xaa, sizeof(b.formatProperties));
            fp2(pd, fmts[i], &b);
            VkFormatProperties3 c = {VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3};
            VkFormatProperties2 cc = {VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, &c};
            memset(&cc.formatProperties, 0xaa, sizeof(cc.formatProperties));
            fp2(pd, fmts[i], &cc);
            /* a chain the cache does not know (VkDrmFormatModifierPropertiesListEXT with a zero count): always the server */
            VkDrmFormatModifierPropertiesListEXT dl = {VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT};
            VkFormatProperties3 d = {VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3, &dl};
            VkFormatProperties2 dd = {VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, &d};
            memset(&dd.formatProperties, 0xaa, sizeof(dd.formatProperties));
            fp2(pd, fmts[i], &dd);
            if (!round) {
                r1[i] = a;
                r2p[i] = b.formatProperties;
                r3[i] = c;
                r3[i].pNext = NULL;
                r3b[i] = d;
                r3b[i].pNext = NULL;
                if (i < 8 && (a.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) bc_optimal++;
                continue;
            }
            VkFormatProperties3 cx = c, dx = d;
            cx.pNext = dx.pNext = NULL;
            if (memcmp(&a, &r1[i], sizeof(a))) bad1++;
            if (memcmp(&b.formatProperties, &r2p[i], sizeof(a))) bad2++;
            if (memcmp(&cx, &r3[i], sizeof(cx)) || memcmp(&cc.formatProperties, &r2p[i], sizeof(a))) bad3++;
            if (memcmp(&dx, &r3b[i], sizeof(dx))) badu++;
        }
    (void)r2;
    judge("v1 answers of rounds 1.. = round 0", !bad1, bad1, ROUNDS);
    judge("v2 answers (no chain) of rounds 1.. = round 0", !bad2, bad2, ROUNDS);
    judge("v2 + FormatProperties3 answers of rounds 1.. = round 0", !bad3, bad3, ROUNDS);
    judge("v2 + unknown chain answers of rounds 1.. = round 0", !badu, badu, ROUNDS);
    judge("BC formats are offered (sampled), emulated or not", bc_optimal >= 6, bc_optimal, 8);
    printf("%s: fmtcache_check %d failures\n", failures ? "FAILED" : "OK", failures);
    return failures ? 1 : 0;
}
