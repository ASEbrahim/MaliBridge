/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * pvsusp_check: a dynamic rendering suspended and resumed across command buffers of one submit, as vkd3d-proton 3.1 records
 * it on tilers (PowerVR's driver faults on these chains: server_pvsusp.c). One 64x64 RGBA8 colour image, one vkQueueSubmit2
 * of three command buffers:
 *   cb1  layout to COLOR_ATTACHMENT; rendering SUSPENDING, loadOp CLEAR red, storeOp DONT_CARE (a suspending part's store
 *        op means nothing), no command inside
 *   cb2  rendering RESUMING|SUSPENDING, loadOp CLEAR green (a resuming part's load op means nothing: green must never show),
 *        vkCmdClearAttachments blue on the left half (x 0..31)
 *   cb3  rendering RESUMING, loadOp CLEAR green, storeOp STORE, nothing inside (the empty last part vkd3d-proton ends with);
 *        layout to TRANSFER_SRC, copy to a buffer
 * Prints "pixels left <rgba> right <rgba>" and "OK: ..." when left is blue (0,0,255,255) and right red (255,0,0,255).
 * Needs Vulkan 1.3 dynamic rendering and synchronization2 (lavapipe has both).
 */
#define _GNU_SOURCE
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define W 64
#define H 64

int main(void)
{
    alarm(60);
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib) return printf("FAILED: loader: %s\n", dlerror()), 2;
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    VkInstance inst;
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "vkbridge-pvsusp-check", 1, "vkd3d", 1, VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 0, NULL};
    if (((PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance"))(&ici, NULL, &inst)) return printf("FAILED: vkCreateInstance\n"), 1;
    uint32_t n = 1;
    VkPhysicalDevice pd;
    if (((PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices"))(inst, &n, &pd) < 0 || !n)
        return printf("FAILED: no physical device\n"), 1;
    VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f13};
    ((PFN_vkGetPhysicalDeviceFeatures2)gipa(inst, "vkGetPhysicalDeviceFeatures2"))(pd, &f2);
    if (!f13.dynamicRendering || !f13.synchronization2) return printf("SKIP: no dynamic rendering / synchronization2\n"), 0;
    VkPhysicalDeviceVulkan13Features want = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    want.dynamicRendering = VK_TRUE;
    want.synchronization2 = VK_TRUE;
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, 0, 1, &prio};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &want, 0, 1, &qci, 0, NULL, 0, NULL, NULL};
    VkDevice dev;
    if (((PFN_vkCreateDevice)gipa(inst, "vkCreateDevice"))(pd, &dci, NULL, &dev)) return printf("FAILED: vkCreateDevice\n"), 1;
    PFN_vkGetDeviceProcAddr gdpa = (PFN_vkGetDeviceProcAddr)gipa(inst, "vkGetDeviceProcAddr");
#define G(name) PFN_##name name = (PFN_##name)gdpa(dev, #name)
    G(vkCreateImage); G(vkDestroyImage); G(vkGetImageMemoryRequirements); G(vkBindImageMemory); G(vkCreateImageView); G(vkDestroyImageView);
    G(vkCreateBuffer); G(vkDestroyBuffer); G(vkGetBufferMemoryRequirements); G(vkAllocateMemory); G(vkFreeMemory); G(vkBindBufferMemory);
    G(vkMapMemory); G(vkInvalidateMappedMemoryRanges); G(vkCreateCommandPool); G(vkDestroyCommandPool); G(vkAllocateCommandBuffers);
    G(vkBeginCommandBuffer); G(vkEndCommandBuffer); G(vkCmdBeginRendering); G(vkCmdEndRendering); G(vkCmdClearAttachments);
    G(vkCmdPipelineBarrier2); G(vkCmdCopyImageToBuffer); G(vkGetDeviceQueue); G(vkQueueSubmit2); G(vkQueueWaitIdle); G(vkDestroyDevice);
#undef G
    PFN_vkGetPhysicalDeviceMemoryProperties gmp =
        (PFN_vkGetPhysicalDeviceMemoryProperties)gipa(inst, "vkGetPhysicalDeviceMemoryProperties");
    VkPhysicalDeviceMemoryProperties mp;
    gmp(pd, &mp);

    VkImageCreateInfo imci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {W, H, 1}, 1, 1,
                              VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL,
                              VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL,
                              VK_IMAGE_LAYOUT_UNDEFINED};
    VkImage img;
    if (vkCreateImage(dev, &imci, NULL, &img)) return printf("FAILED: vkCreateImage\n"), 1;
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(dev, img, &mr);
    uint32_t it = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount && it == UINT32_MAX; i++)
        if (mr.memoryTypeBits >> i & 1) it = i;
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, it};
    VkDeviceMemory imem;
    if (vkAllocateMemory(dev, &mai, NULL, &imem) || vkBindImageMemory(dev, img, imem, 0)) return printf("FAILED: image memory\n"), 1;
    VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, img, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM,
                                 {0, 0, 0, 0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkImageView view;
    if (vkCreateImageView(dev, &vci, NULL, &view)) return printf("FAILED: vkCreateImageView\n"), 1;
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, W * H * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE};
    VkBuffer buf;
    if (vkCreateBuffer(dev, &bci, NULL, &buf)) return printf("FAILED: vkCreateBuffer\n"), 1;
    vkGetBufferMemoryRequirements(dev, buf, &mr);
    uint32_t bt = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount && bt == UINT32_MAX; i++)
        if ((mr.memoryTypeBits >> i & 1) && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) bt = i;
    VkMemoryAllocateInfo bai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, bt};
    VkDeviceMemory bmem;
    if (bt == UINT32_MAX || vkAllocateMemory(dev, &bai, NULL, &bmem) || vkBindBufferMemory(dev, buf, bmem, 0)) return printf("FAILED: buffer memory\n"), 1;

    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, 0, 0};
    VkCommandPool pool;
    if (vkCreateCommandPool(dev, &pci, NULL, &pool)) return printf("FAILED: vkCreateCommandPool\n"), 1;
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 3};
    VkCommandBuffer cb[3];
    if (vkAllocateCommandBuffers(dev, &cai, cb)) return printf("FAILED: vkAllocateCommandBuffers\n"), 1;
    VkCommandBufferBeginInfo cbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};

    VkRenderingAttachmentInfo att = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    att.imageView = view;
    att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea.extent.width = W;
    ri.renderArea.extent.height = H;
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &att;

    /* cb1: to COLOR_ATTACHMENT, suspended part with the clear to red */
    vkBeginCommandBuffer(cb[0], &cbi);
    VkImageMemoryBarrier2 ib = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    ib.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
    ib.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    ib.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    ib.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ib.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ib.srcQueueFamilyIndex = ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    ib.image = img;
    ib.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &ib;
    vkCmdPipelineBarrier2(cb[0], &di);
    att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.clearValue.color = (VkClearColorValue){{1.0f, 0.0f, 0.0f, 1.0f}};
    ri.flags = VK_RENDERING_SUSPENDING_BIT;
    vkCmdBeginRendering(cb[0], &ri);
    vkCmdEndRendering(cb[0]);
    if (vkEndCommandBuffer(cb[0])) return printf("FAILED: end cb1\n"), 1;

    /* cb2: resumed and suspended again, blue on the left half; its CLEAR green must not apply */
    vkBeginCommandBuffer(cb[1], &cbi);
    att.clearValue.color = (VkClearColorValue){{0.0f, 1.0f, 0.0f, 1.0f}};
    ri.flags = VK_RENDERING_RESUMING_BIT | VK_RENDERING_SUSPENDING_BIT;
    vkCmdBeginRendering(cb[1], &ri);
    VkClearAttachment ca = {VK_IMAGE_ASPECT_COLOR_BIT, 0, {.color = {{0.0f, 0.0f, 1.0f, 1.0f}}}};
    VkClearRect cr = {{{0, 0}, {W / 2, H}}, 0, 1};
    vkCmdClearAttachments(cb[1], 1, &ca, 1, &cr);
    vkCmdEndRendering(cb[1]);
    if (vkEndCommandBuffer(cb[1])) return printf("FAILED: end cb2\n"), 1;

    /* cb3: the empty last part, stored; then the copy out */
    vkBeginCommandBuffer(cb[2], &cbi);
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    ri.flags = VK_RENDERING_RESUMING_BIT;
    vkCmdBeginRendering(cb[2], &ri);
    vkCmdEndRendering(cb[2]);
    ib.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    ib.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    ib.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    ib.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    ib.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ib.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    vkCmdPipelineBarrier2(cb[2], &di);
    VkBufferImageCopy bic = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {W, H, 1}};
    vkCmdCopyImageToBuffer(cb[2], img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &bic);
    if (vkEndCommandBuffer(cb[2])) return printf("FAILED: end cb3\n"), 1;

    VkQueue q;
    vkGetDeviceQueue(dev, 0, 0, &q);
    VkCommandBufferSubmitInfo cs[3];
    for (int i = 0; i < 3; i++) cs[i] = (VkCommandBufferSubmitInfo){VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, NULL, cb[i], 0};
    VkSubmitInfo2 si = {VK_STRUCTURE_TYPE_SUBMIT_INFO_2, NULL, 0, 0, NULL, 3, cs, 0, NULL};
    if (vkQueueSubmit2(q, 1, &si, VK_NULL_HANDLE) || vkQueueWaitIdle(q)) return printf("FAILED: submit\n"), 1;
    uint8_t *p = NULL;
    if (vkMapMemory(dev, bmem, 0, VK_WHOLE_SIZE, 0, (void **)&p) || !p) return printf("FAILED: map\n"), 1;
    VkMappedMemoryRange rg = {VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, NULL, bmem, 0, VK_WHOLE_SIZE};
    vkInvalidateMappedMemoryRanges(dev, 1, &rg);
    const uint8_t *l = p + (H / 2 * W + 4) * 4, *r = p + (H / 2 * W + W - 4) * 4;
    printf("pixels left %u,%u,%u,%u right %u,%u,%u,%u\n", l[0], l[1], l[2], l[3], r[0], r[1], r[2], r[3]);
    int ok = l[0] == 0 && l[1] == 0 && l[2] == 255 && l[3] == 255 && r[0] == 255 && r[1] == 0 && r[2] == 0 && r[3] == 255;
    vkDestroyCommandPool(dev, pool, NULL);
    vkDestroyImageView(dev, view, NULL);
    vkDestroyImage(dev, img, NULL);
    vkDestroyBuffer(dev, buf, NULL);
    vkFreeMemory(dev, imem, NULL);
    vkFreeMemory(dev, bmem, NULL);
    vkDestroyDevice(dev, NULL);
    if (!ok) return printf("FAILED: wrong pixels\n"), 1;
    printf("OK: suspended and resumed rendering over three command buffers drew right\n");
    return 0;
}
