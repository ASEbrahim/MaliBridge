/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * reqcache_check: the answers the client keeps instead of asking the server again - image memory
 * requirements by create info (not for BC formats), format properties (v1 and v2) and image format
 * properties - are the ones the server gives: images of one create info get the requirements
 * vkGetDeviceImageMemoryRequirements gives for it, other sizes others, and repeated queries the
 * same answers, refusals included.
 * One line per check, "OK: ..." / "FAILED: ..." last.
 */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static PFN_vkGetInstanceProcAddr gipa;
static VkInstance inst;
static VkDevice dev;
static VkQueue queue;
static VkCommandPool cpool;
static int failures, checks;

#define I(name) static PFN_##name name
I(vkCreateInstance); I(vkEnumeratePhysicalDevices); I(vkGetPhysicalDeviceProperties); I(vkGetPhysicalDeviceQueueFamilyProperties);
I(vkCreateDevice); I(vkGetDeviceProcAddr); I(vkDestroyInstance); I(vkGetDeviceQueue); I(vkCreateCommandPool); I(vkDestroyCommandPool);
I(vkAllocateCommandBuffers); I(vkBeginCommandBuffer); I(vkEndCommandBuffer); I(vkQueueSubmit); I(vkCreateFence);
I(vkDestroyFence); I(vkWaitForFences); I(vkGetFenceStatus); I(vkCreateSemaphore); I(vkDestroySemaphore); I(vkWaitSemaphores);
I(vkQueueWaitIdle); I(vkDeviceWaitIdle); I(vkCreateQueryPool); I(vkDestroyQueryPool); I(vkCmdResetQueryPool);
I(vkCmdWriteTimestamp); I(vkGetQueryPoolResults); I(vkDestroyDevice); I(vkCreateEvent); I(vkDestroyEvent);
I(vkSetEvent); I(vkResetEvent); I(vkGetEventStatus); I(vkResetFences); I(vkGetSemaphoreCounterValue);
I(vkCreateImage); I(vkDestroyImage); I(vkGetImageMemoryRequirements); I(vkGetImageMemoryRequirements2);
I(vkGetDeviceImageMemoryRequirements); I(vkGetPhysicalDeviceFormatProperties); I(vkGetPhysicalDeviceFormatProperties2);
I(vkGetPhysicalDeviceImageFormatProperties); I(vkGetPhysicalDeviceImageFormatProperties2);
#undef I

static void judge(const char *what, int ok, const char *fmt, ...)
{
    char extra[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(extra, sizeof(extra), fmt, ap);
    va_end(ap);
    checks++;
    if (!ok) failures++;
    printf("%s %s: %s\n", ok ? "PASS" : "FAIL", what, extra);
    fflush(stdout);
}


static VkImageCreateInfo image_ci(VkFormat f, uint32_t w, uint32_t h)
{
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = f;
    ci.extent = (VkExtent3D){w, h, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    return ci;
}

static int same_req(const VkMemoryRequirements *a, const VkMemoryRequirements *b)
{
    return a->size == b->size && a->alignment == b->alignment && a->memoryTypeBits == b->memoryTypeBits;
}

/* n images of one create info: v1, v2 (with VkMemoryDedicatedRequirements) and the device query
 * agree for every one, asked twice each (the second from the client's cache). 0 = all agree. */
static int images_agree(VkFormat f, uint32_t w, uint32_t h, int n, VkDeviceSize *size)
{
    VkImageCreateInfo ci = image_ci(f, w, h);
    VkDeviceImageMemoryRequirements di = {VK_STRUCTURE_TYPE_DEVICE_IMAGE_MEMORY_REQUIREMENTS, NULL, &ci, 0};
    VkMemoryRequirements2 want = {VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
    vkGetDeviceImageMemoryRequirements(dev, &di, &want);
    *size = want.memoryRequirements.size;
    int bad = 0;
    for (int i = 0; i < n; i++) {
        VkImage img = VK_NULL_HANDLE;
        if (vkCreateImage(dev, &ci, NULL, &img)) return -1;
        for (int round = 0; round < 2; round++) {
            VkMemoryRequirements r1;
            vkGetImageMemoryRequirements(dev, img, &r1);
            VkMemoryDedicatedRequirements ded = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
            VkMemoryRequirements2 r2 = {VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, &ded};
            VkImageMemoryRequirementsInfo2 ri = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2, NULL, img};
            vkGetImageMemoryRequirements2(dev, &ri, &r2);
            if (!same_req(&r1, &want.memoryRequirements) || !same_req(&r2.memoryRequirements, &want.memoryRequirements) || !r1.size ||
                ded.requiresDedicatedAllocation)
                bad++;
        }
        vkDestroyImage(dev, img, NULL);
    }
    return bad;
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib) {
        printf("FAILED: loader: %s\n", dlerror());
        return 2;
    }
    gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
#define G(n) n = (PFN_##n)gipa(inst, #n)
    G(vkCreateInstance);
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "vkbridge-reqcache-check", 1, "vkbridge", 1, VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 0, NULL};
    if (vkCreateInstance(&ici, NULL, &inst)) {
        printf("FAILED: vkCreateInstance\n");
        return 1;
    }
    G(vkEnumeratePhysicalDevices); G(vkGetPhysicalDeviceProperties); G(vkGetPhysicalDeviceQueueFamilyProperties);
    G(vkCreateDevice); G(vkGetDeviceProcAddr); G(vkDestroyInstance); G(vkGetPhysicalDeviceFormatProperties);
    G(vkGetPhysicalDeviceFormatProperties2); G(vkGetPhysicalDeviceImageFormatProperties); G(vkGetPhysicalDeviceImageFormatProperties2);
#undef G
    uint32_t n = 1;
    VkPhysicalDevice pd;
    if (vkEnumeratePhysicalDevices(inst, &n, &pd) < 0 || !n) {
        printf("FAILED: no physical device\n");
        return 1;
    }
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(pd, &props);
    uint32_t nq = 0, qfam;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, NULL);
    VkQueueFamilyProperties qp[16];
    if (nq > 16) nq = 16;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qp);
    for (qfam = 0; qfam < nq; qfam++)
        if (qp[qfam].queueFlags & VK_QUEUE_GRAPHICS_BIT) break;
    if (qfam == nq || props.apiVersion < VK_API_VERSION_1_3) {
        printf("FAILED: %s: no graphics queue or not Vulkan 1.3\n", props.deviceName);
        return 1;
    }
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, qfam, 1, &prio};
    VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    f12.timelineSemaphore = VK_TRUE;
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f12, 0, 1, &qci, 0, NULL, 0, NULL, NULL};
    if (vkCreateDevice(pd, &dci, NULL, &dev)) {
        printf("FAILED: vkCreateDevice\n");
        return 1;
    }
#define G(n) n = (PFN_##n)vkGetDeviceProcAddr(dev, #n)
    G(vkGetDeviceQueue); G(vkCreateCommandPool); G(vkDestroyCommandPool); G(vkAllocateCommandBuffers); G(vkBeginCommandBuffer); G(vkEndCommandBuffer);
    G(vkQueueSubmit); G(vkCreateFence); G(vkDestroyFence); G(vkWaitForFences); G(vkGetFenceStatus); G(vkCreateSemaphore);
    G(vkDestroySemaphore); G(vkWaitSemaphores); G(vkQueueWaitIdle); G(vkDeviceWaitIdle); G(vkCreateQueryPool);
    G(vkDestroyQueryPool); G(vkCmdResetQueryPool); G(vkCmdWriteTimestamp); G(vkGetQueryPoolResults); G(vkDestroyDevice);
    G(vkCreateEvent); G(vkDestroyEvent); G(vkSetEvent); G(vkResetEvent); G(vkGetEventStatus); G(vkResetFences);
    G(vkGetSemaphoreCounterValue);
    G(vkCreateImage); G(vkDestroyImage); G(vkGetImageMemoryRequirements); G(vkGetImageMemoryRequirements2);
    G(vkGetDeviceImageMemoryRequirements);
#undef G
    vkGetDeviceQueue(dev, qfam, 0, &queue);
    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, 0, qfam};
    vkCreateCommandPool(dev, &pci, NULL, &cpool);
    printf("device: %s\n", props.deviceName);

    {
        VkDeviceSize s64, s1024;
        int a = images_agree(VK_FORMAT_R8G8B8A8_UNORM, 64, 64, 40, &s64), b = images_agree(VK_FORMAT_R8G8B8A8_UNORM, 1024, 1024, 40, &s1024);
        judge("image requirements, 40 images each of 64x64 and 1024x1024 RGBA8", !a && !b && s1024 > s64,
              "%d and %d disagreeing answers, %llu and %llu bytes", a, b, (unsigned long long)s64, (unsigned long long)s1024);
    }
    {
        VkFormatProperties bc;
        vkGetPhysicalDeviceFormatProperties(pd, VK_FORMAT_BC1_RGBA_UNORM_BLOCK, &bc);
        if (bc.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) {
            VkDeviceSize s;
            int a = images_agree(VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 256, 256, 8, &s);
            judge("image requirements, BC1 256x256 (not kept)", !a && s, "%d disagreeing answers, %llu bytes", a, (unsigned long long)s);
        }
    }
    {
        static const VkFormat fmts[] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_D32_SFLOAT, VK_FORMAT_BC1_RGB_UNORM_BLOCK,
                                        VK_FORMAT_BC3_UNORM_BLOCK, VK_FORMAT_BC7_SRGB_BLOCK, VK_FORMAT_A2B10G10R10_UNORM_PACK32,
                                        VK_FORMAT_B10G11R11_UFLOAT_PACK32, VK_FORMAT_R16G16B16A16_SFLOAT};
        int bad = 0;
        for (unsigned i = 0; i < sizeof(fmts) / sizeof(fmts[0]); i++)
            for (int round = 0; round < 3; round++) {
                VkFormatProperties p1;
                vkGetPhysicalDeviceFormatProperties(pd, fmts[i], &p1);
                VkFormatProperties3 p3 = {VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3};
                VkFormatProperties2 p2 = {VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, &p3};
                vkGetPhysicalDeviceFormatProperties2(pd, fmts[i], &p2);
                if (memcmp(&p1, &p2.formatProperties, sizeof(p1)) ||
                    (p3.optimalTilingFeatures & 0xffffffffu & p1.optimalTilingFeatures) != p1.optimalTilingFeatures)
                    bad++;
            }
        judge("format properties, v1 and v2 agree, asked 3 times (9 formats, BC among them)", !bad, "%d disagreeing answers", bad);
    }
    {
        int bad = 0;
        static const VkFormat fmts[] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_BC1_RGBA_UNORM_BLOCK, VK_FORMAT_BC7_UNORM_BLOCK, VK_FORMAT_D24_UNORM_S8_UINT};
        for (unsigned i = 0; i < sizeof(fmts) / sizeof(fmts[0]); i++) {
            VkImageFormatProperties first = {0};
            VkResult r0 = VK_SUCCESS;
            for (int round = 0; round < 3; round++) {
                VkImageFormatProperties p1;
                VkResult r1 = vkGetPhysicalDeviceImageFormatProperties(pd, fmts[i], VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
                                                                       VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, 0, &p1);
                VkPhysicalDeviceExternalImageFormatInfo ei = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO, NULL, 0};
                VkPhysicalDeviceImageFormatInfo2 info = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2, &ei, fmts[i], VK_IMAGE_TYPE_2D,
                                                         VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, 0};
                VkExternalImageFormatProperties ep = {VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
                VkImageFormatProperties2 p2 = {VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, &ep};
                VkResult r2 = vkGetPhysicalDeviceImageFormatProperties2(pd, &info, &p2);
                if (!round) first = p1, r0 = r1;
                if (r1 != r2 || r1 != r0 || (r1 == VK_SUCCESS && (memcmp(&p1, &p2.imageFormatProperties, sizeof(p1)) || memcmp(&p1, &first, sizeof(p1)))))
                    bad++;
            }
            /* an impossible one: storage on a depth format with linear tiling is refused, every time */
        }
        VkResult rz[3];
        for (int round = 0; round < 3; round++) {
            VkImageFormatProperties p;
            rz[round] = vkGetPhysicalDeviceImageFormatProperties(pd, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_IMAGE_TYPE_3D, VK_IMAGE_TILING_LINEAR,
                                                                 VK_IMAGE_USAGE_STORAGE_BIT, 0, &p);
        }
        if (rz[0] != rz[1] || rz[1] != rz[2] || rz[0] == VK_SUCCESS) bad++;
        judge("image format properties, v1 and v2 agree, asked 3 times; a refused one refused each time", !bad, "%d disagreeing answers", bad);
    }
    vkDestroyCommandPool(dev, cpool, NULL);
    vkDestroyDevice(dev, NULL);
    vkDestroyInstance(inst, NULL);
    if (failures) printf("FAILED: %d of %d check(s)\n", failures, checks);
    else printf("OK: %d of %d checks passed\n", checks, checks);
    return failures ? 1 : 0;
}
