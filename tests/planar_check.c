/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * planar_check: NV12 (G8_B8R8_2PLANE_420) textures as DXVK makes them for D3D11 - what Media
 * Foundation's H.264 decoder allocates its output samples as. DXVK creates every multi-planar
 * image MUTABLE_FORMAT | EXTENDED_USAGE (its plane views are R8 and R8G8) and checks it with
 * vkGetPhysicalDeviceImageFormatProperties2 first; the Mali-G925 refuses that flag on multi-planar
 * formats, so each texture failed ("D3D11: Cannot create texture: Format 103") and Risk of Rain
 * Returns waited for its intro video for ever. VKBRIDGE_FAKE_MISSING=planarext imitates the
 * refusal on the host; the client makes such images without the flag where the format covers the
 * usage (client/emu.c).
 *
 * Checks: the format queries DXVK sends (both entry points) succeed; an image whose usage the format
 * lacks (storage) keeps the flag, and the driver's answer; the image is created with DXVK's flags,
 * usage and view-format list, gets memory, R8 / R8G8 plane views, and both planes take a copy in
 * and give the same bytes back.
 *
 *   planar_check [--imitated]   (--imitated: the server imitates the refusal: the storage query
 *                                must be refused, as the flag stays on it)
 * Last line "OK: ..." or "FAILED: ..."; exit 0 only if all pass (77: the device has no NV12).
 */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FMT VK_FORMAT_G8_B8R8_2PLANE_420_UNORM
#define W 1920u
#define H 1088u

static int failures, checks;

static void check(int ok, const char *what, VkResult r)
{
    checks++;
    if (!ok) failures++;
    printf("%s %s (%d)\n", ok ? "PASS" : "FAIL", what, r);
    fflush(stdout);
}

static PFN_vkGetInstanceProcAddr gipa;
static VkInstance inst;
static VkPhysicalDevice pd;
static VkDevice dev;
static PFN_vkGetDeviceProcAddr gdpa;
#define I(name) ((PFN_##name)gipa(inst, #name))
#define D(name) ((PFN_##name)gdpa(dev, #name))

static uint32_t pick(const VkPhysicalDeviceMemoryProperties *mp, uint32_t bits, VkMemoryPropertyFlags want)
{
    for (uint32_t i = 0; i < mp->memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp->memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

int main(int argc, char **argv)
{
    int imitated = argc > 1 && !strcmp(argv[1], "--imitated");
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib) {
        printf("FAILED: no Vulkan loader\n");
        return 1;
    }
    gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "planar_check", 1, "DXVK", 1, VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app};
    if (((PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance"))(&ici, NULL, &inst) != VK_SUCCESS) {
        printf("FAILED: no instance\n");
        return 1;
    }
    uint32_t n = 1;
    I(vkEnumeratePhysicalDevices)(inst, &n, &pd);
    if (!n) {
        printf("FAILED: no device\n");
        return 1;
    }
    VkFormatProperties fp;
    I(vkGetPhysicalDeviceFormatProperties)(pd, FMT, &fp);
    const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    if ((fp.optimalTilingFeatures & need) != need) {
        printf("SKIP: the device does not sample and copy NV12 (features 0x%x)\n", fp.optimalTilingFeatures);
        return 77;
    }

    /* The queries DXVK sends: its CheckImageSupport asks with usage TRANSFER_SRC|DST only when the
     * image is EXTENDED_USAGE; its format limits ask with the image's usage. */
    const VkImageCreateFlags flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
    const VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    VkFormat views[] = {VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8_UNORM};
    VkImageFormatListCreateInfo fl = {VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO, NULL, 2, views};
    VkImageUsageFlags qusage[] = {VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, usage};
    const char *qname[] = {"vkGetPhysicalDeviceImageFormatProperties2, NV12 transfer, mutable + extended usage",
                           "vkGetPhysicalDeviceImageFormatProperties2, NV12 sampled + transfer, mutable + extended usage, R8/R8G8 list"};
    for (int i = 0; i < 2; i++) {
        VkPhysicalDeviceImageFormatInfo2 info = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2, i ? &fl : NULL, FMT, VK_IMAGE_TYPE_2D,
                                                 VK_IMAGE_TILING_OPTIMAL, qusage[i], flags};
        VkImageFormatProperties2 out = {VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
        VkResult r = I(vkGetPhysicalDeviceImageFormatProperties2)(pd, &info, &out);
        check(r == VK_SUCCESS && out.imageFormatProperties.maxExtent.width >= W && out.imageFormatProperties.maxExtent.height >= H, qname[i], r);
    }
    VkImageFormatProperties ip;
    VkResult r = I(vkGetPhysicalDeviceImageFormatProperties)(pd, FMT, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, usage, flags, &ip);
    check(r == VK_SUCCESS, "vkGetPhysicalDeviceImageFormatProperties, NV12 sampled + transfer, mutable + extended usage", r);
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) {
        /* Storage is a view format's feature here: the flag is needed and stays, with the driver's
         * answer (refused when imitated). */
        VkPhysicalDeviceImageFormatInfo2 info = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2, &fl, FMT, VK_IMAGE_TYPE_2D,
                                                 VK_IMAGE_TILING_OPTIMAL, usage | VK_IMAGE_USAGE_STORAGE_BIT, flags};
        VkImageFormatProperties2 out = {VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
        r = I(vkGetPhysicalDeviceImageFormatProperties2)(pd, &info, &out);
        if (imitated) check(r == VK_ERROR_FORMAT_NOT_SUPPORTED, "NV12 + storage keeps extended usage (refused, imitated)", r);
        else printf("info: NV12 + storage, mutable + extended usage: %d\n", r);
    }

    /* The device and DXVK's image. */
    uint32_t nq = 8, qf = 0;
    VkQueueFamilyProperties qp[8];
    I(vkGetPhysicalDeviceQueueFamilyProperties)(pd, &nq, qp);
    for (uint32_t i = 0; i < nq; i++)
        if (qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            qf = i;
            break;
        }
    float prio = 1.0f;
    VkPhysicalDeviceVulkan11Features f11 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    f11.samplerYcbcrConversion = VK_TRUE;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, qf, 1, &prio};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f11, 0, 1, &qci, 0, NULL, 0, NULL, NULL};
    r = I(vkCreateDevice)(pd, &dci, NULL, &dev);
    if (r != VK_SUCCESS) {
        printf("FAILED: vkCreateDevice %d\n", r);
        return 1;
    }
    gdpa = (PFN_vkGetDeviceProcAddr)gipa(inst, "vkGetDeviceProcAddr");
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &fl, flags, VK_IMAGE_TYPE_2D, FMT, {W, H, 1}, 1, 1,
                            VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL, usage, VK_SHARING_MODE_EXCLUSIVE, 0, NULL,
                            VK_IMAGE_LAYOUT_UNDEFINED};
    VkImage img = VK_NULL_HANDLE;
    r = D(vkCreateImage)(dev, &ci, NULL, &img);
    check(r == VK_SUCCESS, "vkCreateImage NV12 1920x1088, DXVK's flags and usage", r);
    if (r != VK_SUCCESS) goto out;
    VkPhysicalDeviceMemoryProperties mp;
    I(vkGetPhysicalDeviceMemoryProperties)(pd, &mp);
    VkMemoryRequirements req;
    D(vkGetImageMemoryRequirements)(dev, img, &req);
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size, pick(&mp, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    if (mai.memoryTypeIndex == UINT32_MAX) mai.memoryTypeIndex = pick(&mp, req.memoryTypeBits, 0);
    VkDeviceMemory imem;
    r = D(vkAllocateMemory)(dev, &mai, NULL, &imem);
    if (r == VK_SUCCESS) r = D(vkBindImageMemory)(dev, img, imem, 0);
    check(r == VK_SUCCESS, "image memory bound", r);
    if (r != VK_SUCCESS) goto out;
    for (int p = 0; p < 2; p++) {
        VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, img, VK_IMAGE_VIEW_TYPE_2D, views[p], {0},
                                     {p ? VK_IMAGE_ASPECT_PLANE_1_BIT : VK_IMAGE_ASPECT_PLANE_0_BIT, 0, 1, 0, 1}};
        VkImageView v = VK_NULL_HANDLE;
        r = D(vkCreateImageView)(dev, &vci, NULL, &v);
        check(r == VK_SUCCESS, p ? "R8G8 view of plane 1" : "R8 view of plane 0", r);
        if (v) D(vkDestroyImageView)(dev, v, NULL);
    }

    /* Both planes: a pattern copied in, copied back out to another buffer, compared. */
    {
        VkDeviceSize y = (VkDeviceSize)W * H, uv = (VkDeviceSize)W * H / 2, total = y + uv;
        VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, total * 2,
                                  VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE};
        VkBuffer buf;
        D(vkCreateBuffer)(dev, &bci, NULL, &buf);
        D(vkGetBufferMemoryRequirements)(dev, buf, &req);
        VkMemoryAllocateInfo bai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size,
                                    pick(&mp, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
        VkDeviceMemory bmem;
        D(vkAllocateMemory)(dev, &bai, NULL, &bmem);
        D(vkBindBufferMemory)(dev, buf, bmem, 0);
        uint8_t *map;
        D(vkMapMemory)(dev, bmem, 0, VK_WHOLE_SIZE, 0, (void **)&map);
        for (VkDeviceSize i = 0; i < total; i++) map[i] = (uint8_t)(i * 7 + (i >> 11));
        memset(map + total, 0, total);
        VkQueue q;
        D(vkGetDeviceQueue)(dev, qf, 0, &q);
        VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, 0, qf};
        VkCommandPool pool;
        D(vkCreateCommandPool)(dev, &pci, NULL, &pool);
        VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
        VkCommandBuffer cb;
        D(vkAllocateCommandBuffers)(dev, &cai, &cb);
        VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
        D(vkBeginCommandBuffer)(cb, &bi);
        VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                                  VK_IMAGE_LAYOUT_GENERAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, img,
                                  {VK_IMAGE_ASPECT_PLANE_0_BIT | VK_IMAGE_ASPECT_PLANE_1_BIT, 0, 1, 0, 1}};
        D(vkCmdPipelineBarrier)(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
        VkBufferImageCopy in[2] = {{0, 0, 0, {VK_IMAGE_ASPECT_PLANE_0_BIT, 0, 0, 1}, {0, 0, 0}, {W, H, 1}},
                                   {y, 0, 0, {VK_IMAGE_ASPECT_PLANE_1_BIT, 0, 0, 1}, {0, 0, 0}, {W / 2, H / 2, 1}}};
        D(vkCmdCopyBufferToImage)(cb, buf, img, VK_IMAGE_LAYOUT_GENERAL, 2, in);
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        D(vkCmdPipelineBarrier)(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
        VkBufferImageCopy outc[2] = {in[0], in[1]};
        outc[0].bufferOffset += total;
        outc[1].bufferOffset += total;
        D(vkCmdCopyImageToBuffer)(cb, img, VK_IMAGE_LAYOUT_GENERAL, buf, 2, outc);
        VkMemoryBarrier hb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
        D(vkCmdPipelineBarrier)(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hb, 0, NULL, 0, NULL);
        D(vkEndCommandBuffer)(cb);
        VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cb, 0, NULL};
        r = D(vkQueueSubmit)(q, 1, &si, VK_NULL_HANDLE);
        if (r == VK_SUCCESS) r = D(vkQueueWaitIdle)(q);
        int same = r == VK_SUCCESS && !memcmp(map, map + total, total);
        check(same, "both planes copied in and back out unchanged", r);
        D(vkDestroyCommandPool)(dev, pool, NULL);
        D(vkDestroyBuffer)(dev, buf, NULL);
        D(vkFreeMemory)(dev, bmem, NULL);
    }
    D(vkDestroyImage)(dev, img, NULL);
    D(vkFreeMemory)(dev, imem, NULL);
out:
    D(vkDestroyDevice)(dev, NULL);
    I(vkDestroyInstance)(inst, NULL);
    if (failures) printf("FAILED: %d of %d checks\n", failures, checks);
    else printf("OK: %d checks%s\n", checks, imitated ? " (refusal imitated)" : "");
    return failures ? 1 : 0;
}
