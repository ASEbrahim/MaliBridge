/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * stencillayout_check: MK-11 (Arm erratum 4021056, r41p0-r54p0: a separate stencil layout in a
 * render pass ignored, the stencil discarded when the depth layout is read-only). A packed
 * depth/stencil image's stencil is cleared by the pass's load op with the depth aspect read-only
 * (DEPTH_READ_ONLY_OPTIMAL) and the stencil writable (STENCIL_ATTACHMENT_OPTIMAL, given with
 * VkAttachmentDescriptionStencilLayout / VkAttachmentReferenceStencilLayout), then copied out and
 * compared:
 *   render pass        vkCreateRenderPass2 with the stencil structs, stencil cleared to 7;
 *   dynamic rendering  the same layouts as vkCmdBeginRendering's depth and stencil attachments
 *                      on one view, cleared to 9 (when the device has dynamic rendering).
 * host/stencillayout_test.sh runs it through the bridge with the bug imitated
 * (VKBRIDGE_ERRATA_IMITATE=stencil_layout: the render pass case fails), the fix over it, the
 * version gate.
 */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 64
#define H 64

static PFN_vkGetDeviceProcAddr gdpa;
static VkDevice dev;
#define GD(n) PFN_##n n = (PFN_##n)gdpa(dev, #n)

static int fail(const char *what)
{
    printf("FAIL: %s\n", what);
    return 1;
}

static uint32_t mem_type(const VkPhysicalDeviceMemoryProperties *mp, uint32_t bits, VkMemoryPropertyFlags want)
{
    for (uint32_t i = 0; i < mp->memoryTypeCount; i++)
        if ((bits >> i & 1) && (mp->memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

typedef struct {
    VkImage img;
    VkImageView view;
    VkDeviceMemory mem;
} ds_image;

static int make_image(const VkPhysicalDeviceMemoryProperties *mp, VkFormat fmt, ds_image *o)
{
    GD(vkCreateImage); GD(vkGetImageMemoryRequirements); GD(vkAllocateMemory); GD(vkBindImageMemory); GD(vkCreateImageView);
    VkImageCreateInfo ici = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, fmt, {W, H, 1}, 1, 1, VK_SAMPLE_COUNT_1_BIT,
                             VK_IMAGE_TILING_OPTIMAL,
                             VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
    if (vkCreateImage(dev, &ici, NULL, &o->img)) return 0;
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(dev, o->img, &mr);
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size,
                                mem_type(mp, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    if (mai.memoryTypeIndex == UINT32_MAX) mai.memoryTypeIndex = mem_type(mp, mr.memoryTypeBits, 0);
    if (vkAllocateMemory(dev, &mai, NULL, &o->mem) || vkBindImageMemory(dev, o->img, o->mem, 0)) return 0;
    VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, o->img, VK_IMAGE_VIEW_TYPE_2D, fmt, {0},
                                 {VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1}};
    return !vkCreateImageView(dev, &vci, NULL, &o->view);
}

static void barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to, VkAccessFlags src, VkAccessFlags dst,
                    VkPipelineStageFlags ss, VkPipelineStageFlags ds)
{
    GD(vkCmdPipelineBarrier);
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, src, dst, from, to, VK_QUEUE_FAMILY_IGNORED,
                              VK_QUEUE_FAMILY_IGNORED, img, {VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(cb, ss, ds, 0, 0, NULL, 0, NULL, 1, &b);
}

static void copy_stencil(VkCommandBuffer cb, VkImage img, VkBuffer buf, VkDeviceSize off)
{
    GD(vkCmdCopyImageToBuffer);
    VkBufferImageCopy r = {off, 0, 0, {VK_IMAGE_ASPECT_STENCIL_BIT, 0, 0, 1}, {0, 0, 0}, {W, H, 1}};
    vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &r);
}

static int check(const char *what, const uint8_t *s, uint8_t want, int *bad)
{
    int wrong = 0;
    for (int i = 0; i < W * H; i++) wrong += s[i] != want;
    if (wrong) {
        printf("FAIL %s: %d of %d stencil values wrong (first %u, want %u)\n", what, wrong, W * H, s[0], want);
        ++*bad;
    } else
        printf("PASS %s: stencil %u everywhere\n", what, want);
    return 1;
}

int main(void)
{
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib) return fail("no libvulkan.so.1");
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
#define GI(n) PFN_##n n = (PFN_##n)gipa(inst, #n)
    VkInstance inst = VK_NULL_HANDLE;
    GI(vkCreateInstance);
    GI(vkEnumerateInstanceVersion);
    uint32_t iv = VK_API_VERSION_1_0;
    if (vkEnumerateInstanceVersion) vkEnumerateInstanceVersion(&iv);
    if (iv < VK_API_VERSION_1_2) {
        printf("SKIP: Vulkan 1.2 needed\n0 of 0 checks\n");
        return 0;
    }
    VkApplicationInfo ai = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "stencillayout_check", 1, NULL, 0, VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &ai};
    if (vkCreateInstance(&ici, NULL, &inst)) return fail("instance");
    GI(vkEnumeratePhysicalDevices);
    GI(vkGetPhysicalDeviceMemoryProperties);
    GI(vkGetPhysicalDeviceProperties);
    GI(vkGetPhysicalDeviceFeatures2);
    GI(vkGetPhysicalDeviceFormatProperties);
    GI(vkCreateDevice);
    GI(vkDestroyInstance);
    gdpa = (PFN_vkGetDeviceProcAddr)gipa(inst, "vkGetDeviceProcAddr");
    uint32_t n = 1;
    VkPhysicalDevice pd;
    if (vkEnumeratePhysicalDevices(inst, &n, &pd) < 0 || !n) return fail("no physical device");
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(pd, &props);
    VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                                            props.apiVersion >= VK_API_VERSION_1_3 ? &f13 : NULL};
    VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f12};
    if (props.apiVersion < VK_API_VERSION_1_2) {
        printf("SKIP: a Vulkan 1.2 device needed\n0 of 0 checks\n");
        return 0;
    }
    vkGetPhysicalDeviceFeatures2(pd, &f2);
    if (!f12.separateDepthStencilLayouts) {
        printf("SKIP: no separateDepthStencilLayouts\n0 of 0 checks\n");
        return 0;
    }
    int dyn = props.apiVersion >= VK_API_VERSION_1_3 && f13.dynamicRendering;
    VkFormat fmt = VK_FORMAT_UNDEFINED;
    const VkFormat cand[] = {VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D16_UNORM_S8_UINT};
    for (int i = 0; i < 3 && !fmt; i++) {
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(pd, cand[i], &fp);
        VkFormatFeatureFlags want = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
        if ((fp.optimalTilingFeatures & want) == want) fmt = cand[i];
    }
    if (!fmt) {
        printf("SKIP: no depth/stencil format\n0 of 0 checks\n");
        return 0;
    }
    /* only what the check uses */
    VkPhysicalDeviceVulkan13Features e13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features e12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, dyn ? &e13 : NULL};
    e12.separateDepthStencilLayouts = VK_TRUE;
    e13.dynamicRendering = VK_TRUE;
    float prio = 1;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, 0, 1, &prio};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &e12, 0, 1, &qci};
    if (vkCreateDevice(pd, &dci, NULL, &dev)) return fail("device");
    GD(vkDestroyDevice); GD(vkGetDeviceQueue); GD(vkCreateRenderPass2); GD(vkDestroyRenderPass); GD(vkCreateFramebuffer);
    GD(vkDestroyFramebuffer); GD(vkCreateBuffer); GD(vkDestroyBuffer); GD(vkGetBufferMemoryRequirements); GD(vkAllocateMemory);
    GD(vkFreeMemory); GD(vkBindBufferMemory); GD(vkMapMemory); GD(vkCreateCommandPool); GD(vkDestroyCommandPool);
    GD(vkAllocateCommandBuffers); GD(vkBeginCommandBuffer); GD(vkEndCommandBuffer); GD(vkCmdBeginRenderPass2);
    GD(vkCmdEndRenderPass2); GD(vkQueueSubmit); GD(vkQueueWaitIdle); GD(vkDestroyImage); GD(vkDestroyImageView);
    PFN_vkCmdBeginRendering vkCmdBeginRendering = dyn ? (PFN_vkCmdBeginRendering)gdpa(dev, "vkCmdBeginRendering") : NULL;
    PFN_vkCmdEndRendering vkCmdEndRendering = dyn ? (PFN_vkCmdEndRendering)gdpa(dev, "vkCmdEndRendering") : NULL;
    if (!vkCreateRenderPass2 || !vkCmdBeginRenderPass2) return fail("no vkCreateRenderPass2");
    dyn = dyn && vkCmdBeginRendering && vkCmdEndRendering;
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);

    ds_image a = {0}, b = {0};
    if (!make_image(&mp, fmt, &a) || (dyn && !make_image(&mp, fmt, &b))) return fail("image");

    VkAttachmentDescriptionStencilLayout asl = {VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_STENCIL_LAYOUT, NULL,
                                                VK_IMAGE_LAYOUT_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL};
    VkAttachmentDescription2 ad = {VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2, &asl, 0, fmt, VK_SAMPLE_COUNT_1_BIT,
                                   VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_NONE, VK_ATTACHMENT_LOAD_OP_CLEAR,
                                   VK_ATTACHMENT_STORE_OP_STORE, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL};
    VkAttachmentReferenceStencilLayout rsl = {VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_STENCIL_LAYOUT, NULL,
                                              VK_IMAGE_LAYOUT_STENCIL_ATTACHMENT_OPTIMAL};
    VkAttachmentReference2 ref = {VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2, &rsl, 0, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL,
                                  VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT};
    VkSubpassDescription2 sd = {VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2, NULL, 0, VK_PIPELINE_BIND_POINT_GRAPHICS};
    sd.pDepthStencilAttachment = &ref;
    VkSubpassDependency2 deps[2] = {
        {VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2, NULL, VK_SUBPASS_EXTERNAL, 0, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
         VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, 0,
         VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT},
        {VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2, NULL, 0, VK_SUBPASS_EXTERNAL,
         VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
         VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT}};
    VkRenderPassCreateInfo2 rpci = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2, NULL, 0, 1, &ad, 1, &sd, 2, deps};
    VkRenderPass rp;
    if (vkCreateRenderPass2(dev, &rpci, NULL, &rp)) return fail("render pass");
    VkFramebufferCreateInfo fci = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, NULL, 0, rp, 1, &a.view, W, H, 1};
    VkFramebuffer fb;
    if (vkCreateFramebuffer(dev, &fci, NULL, &fb)) return fail("framebuffer");

    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, 2 * W * H, VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VkBuffer buf;
    VkDeviceMemory bmem;
    uint8_t *map;
    if (vkCreateBuffer(dev, &bci, NULL, &buf)) return fail("buffer");
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(dev, buf, &mr);
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size,
                                mem_type(&mp, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    if (mai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(dev, &mai, NULL, &bmem) || vkBindBufferMemory(dev, buf, bmem, 0) ||
        vkMapMemory(dev, bmem, 0, VK_WHOLE_SIZE, 0, (void **)&map))
        return fail("buffer memory");
    memset(map, 0xEE, 2 * W * H);

    VkCommandPoolCreateInfo cpi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, 0, 0};
    VkCommandPool cp;
    VkCommandBuffer cb;
    VkCommandBufferAllocateInfo cbai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, 0, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    if (vkCreateCommandPool(dev, &cpi, NULL, &cp) || (cbai.commandPool = cp, vkAllocateCommandBuffers(dev, &cbai, &cb)))
        return fail("command buffer");
    VkCommandBufferBeginInfo cbbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(cb, &cbbi);
    const VkImageLayout ro_rw = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL;
    const VkPipelineStageFlags frag = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    const VkAccessFlags dsw = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    /* the render pass */
    barrier(cb, a.img, VK_IMAGE_LAYOUT_UNDEFINED, ro_rw, 0, dsw, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, frag);
    VkClearValue cv = {.depthStencil = {1.0f, 7}};
    VkRenderPassBeginInfo rbi = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, NULL, rp, fb, {{0, 0}, {W, H}}, 1, &cv};
    VkSubpassBeginInfo sbi = {VK_STRUCTURE_TYPE_SUBPASS_BEGIN_INFO, NULL, VK_SUBPASS_CONTENTS_INLINE};
    VkSubpassEndInfo sei = {VK_STRUCTURE_TYPE_SUBPASS_END_INFO};
    vkCmdBeginRenderPass2(cb, &rbi, &sbi);
    vkCmdEndRenderPass2(cb, &sei);
    copy_stencil(cb, a.img, buf, 0);
    /* dynamic rendering */
    if (dyn) {
        barrier(cb, b.img, VK_IMAGE_LAYOUT_UNDEFINED, ro_rw, 0, dsw, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, frag);
        VkRenderingAttachmentInfo da = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, NULL, b.view, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL};
        da.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, da.storeOp = VK_ATTACHMENT_STORE_OP_NONE;
        VkRenderingAttachmentInfo sa = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, NULL, b.view, VK_IMAGE_LAYOUT_STENCIL_ATTACHMENT_OPTIMAL};
        sa.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, sa.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        sa.clearValue.depthStencil.stencil = 9;
        VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO, NULL, 0, {{0, 0}, {W, H}}, 1, 0, 0, NULL, &da, &sa};
        vkCmdBeginRendering(cb, &ri);
        vkCmdEndRendering(cb);
        barrier(cb, b.img, ro_rw, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                VK_ACCESS_TRANSFER_READ_BIT, frag, VK_PIPELINE_STAGE_TRANSFER_BIT);
        copy_stencil(cb, b.img, buf, W * H);
    }
    vkEndCommandBuffer(cb);
    VkQueue q;
    vkGetDeviceQueue(dev, 0, 0, &q);
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cb};
    if (vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE) || vkQueueWaitIdle(q)) return fail("submit");
    int bad = 0, checks = 0;
    checks += check("render pass with separate stencil layouts (depth read-only, stencil cleared)", map, 7, &bad);
    if (dyn) checks += check("dynamic rendering with separate depth and stencil layouts (stencil cleared)", map + W * H, 9, &bad);
    else printf("SKIP dynamic rendering: not on this device\n");

    vkDestroyCommandPool(dev, cp, NULL);
    vkDestroyFramebuffer(dev, fb, NULL);
    vkDestroyRenderPass(dev, rp, NULL);
    vkDestroyBuffer(dev, buf, NULL);
    vkFreeMemory(dev, bmem, NULL);
    ds_image *imgs[2] = {&a, &b};
    for (int i = 0; i < 2; i++)
        if (imgs[i]->img) {
            vkDestroyImageView(dev, imgs[i]->view, NULL);
            vkDestroyImage(dev, imgs[i]->img, NULL);
            vkFreeMemory(dev, imgs[i]->mem, NULL);
        }
    vkDestroyDevice(dev, NULL);
    vkDestroyInstance(inst, NULL);
    if (bad) {
        printf("FAIL: %d of %d checks failed\n", bad, checks);
        return 1;
    }
    printf("OK: %d of %d checks passed\n", checks, checks);
    return 0;
}
