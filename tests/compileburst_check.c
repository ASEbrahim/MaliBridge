/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * compileburst_check: frames that each create new graphics pipelines at their draws, as DXVK does
 * on a driver without pipeline libraries, presented on a headless surface, for the asynchronous
 * compile's deferred mode (server_async.c, VKBRIDGE_ASYNC_COMPILE=3) and the per-hitch line
 * (VKBRIDGE_HITCH_MS). host/compileburst_test.sh runs it with a slow compiler imitated on the
 * server (VKBRIDGE_ASYNC_COMPILE_IMITATE_MS) and compares the modes.
 *
 *   compileburst_check [--small K] [--small-frames F] [--burst N] [--odd-fullscreen 0|1] [--tag T]
 *
 * Frames (each: acquire, one command buffer drawing into a 4x4 RGBA8 target, read back, present):
 *   3 ordinary frames (one pipeline made at the start);
 *   F frames (default 5) that each create K new pipelines (default 2), create - bind - draw one
 *   after another in the same command buffer;
 *   3 ordinary frames;
 *   1 frame that creates N new pipelines (default 8; at most 15);
 *   3 ordinary frames.
 * Pipeline i of a frame draws only pixel i (its own static scissor) in its own colour; every odd
 * one has no vertex input (a full-screen pass, as DXVK's blits and resolves; --odd-fullscreen 0: none), so the colour-only
 * rules of VKBRIDGE_ASYNC_COMPILE=1 keep those synchronous. After each frame every pixel is checked:
 * a draw that was skipped leaves the clear colour.
 * Prints one line per frame ("frame <n> <kind> ms=<present to present> create_ms=<creates>
 * drawn=<pixels right>/<expected>") with n counted from 1 as the hitch line's frame=, a summary
 * line per kind, and "OK: ..." / "FAILED: ..." last (FAILED only for setup errors or a frame whose
 * pixels are wrong: neither the draw's colour nor the clear).
 */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static const uint32_t spv_vert[] = {
#include "cullmode_vert.inc"
};
static const uint32_t spv_frag[] = {
#include "dynstate_frag.inc"
};

#define W 4

static PFN_vkGetInstanceProcAddr gipa;
static VkInstance inst;
static VkPhysicalDevice pd;
static VkDevice dev;
static VkQueue queue;
static VkPhysicalDeviceMemoryProperties mp;

#define I(name) static PFN_##name name
I(vkCreateInstance); I(vkEnumeratePhysicalDevices); I(vkGetPhysicalDeviceQueueFamilyProperties); I(vkCreateDevice);
I(vkGetDeviceQueue); I(vkCreateCommandPool); I(vkAllocateCommandBuffers); I(vkBeginCommandBuffer); I(vkEndCommandBuffer);
I(vkCmdPipelineBarrier); I(vkQueueSubmit); I(vkDeviceWaitIdle); I(vkCreateHeadlessSurfaceEXT);
I(vkGetPhysicalDeviceSurfaceCapabilitiesKHR); I(vkGetPhysicalDeviceSurfaceSupportKHR); I(vkCreateSwapchainKHR);
I(vkGetSwapchainImagesKHR); I(vkAcquireNextImageKHR); I(vkQueuePresentKHR); I(vkCreateFence); I(vkWaitForFences); I(vkResetFences);
I(vkCreateShaderModule); I(vkCreatePipelineLayout); I(vkCreateGraphicsPipelines); I(vkDestroyPipeline);
I(vkGetPhysicalDeviceMemoryProperties); I(vkAllocateMemory); I(vkCreateImage); I(vkGetImageMemoryRequirements); I(vkBindImageMemory);
I(vkCreateBuffer); I(vkGetBufferMemoryRequirements); I(vkBindBufferMemory); I(vkMapMemory); I(vkResetCommandBuffer);
I(vkCreateImageView); I(vkCreateRenderPass); I(vkCreateFramebuffer); I(vkCmdBeginRenderPass); I(vkCmdEndRenderPass);
I(vkCmdBindPipeline); I(vkCmdPushConstants); I(vkCmdDraw); I(vkCmdCopyImageToBuffer);
#undef I

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec * 1e-6;
}

static uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want)
{
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

static VkShaderModule vs, fs;
static VkPipelineLayout layout;
static VkRenderPass rp;
static VkFramebuffer fb;
static VkImage target;
static VkBuffer rbuf;
static uint8_t *rb;
static unsigned serial; /* makes every pipeline's create info differ (its depth bias, unused) */
static int odd_fullscreen = 1;

/* A pipeline drawing pixel `px` only; odd ones without vertex input. */
static VkPipeline make_pipeline(int px, double *ms)
{
    VkPipelineShaderStageCreateInfo st[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", NULL},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", NULL},
    };
    VkVertexInputBindingDescription vb = {0, 16, VK_VERTEX_INPUT_RATE_VERTEX};
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, NULL, 0, !(odd_fullscreen && (px & 1)), &vb, 0,
                                               NULL};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0,
                                                 VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 0};
    VkViewport vp = {0, 0, W, W, 0, 1};
    VkRect2D sc = {{px % W, px / W}, {1, 1}};
    VkPipelineViewportStateCreateInfo vps = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL, 0, 1, &vp, 1, &sc};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.0f;
    rs.depthBiasConstantFactor = (float)(++serial); /* unused (bias off): a create info of its own */
    VkPipelineMultisampleStateCreateInfo ms_ = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, NULL, 0, VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState cba = {0};
    cba.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cbs = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, NULL, 0, 0, 0, 1, &cba, {0}};
    VkGraphicsPipelineCreateInfo ci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, NULL, 0, 2, st, &vi, &ia, NULL, &vps, &rs, &ms_,
                                       NULL, &cbs, NULL, layout, rp, 0, VK_NULL_HANDLE, -1};
    VkPipeline p = VK_NULL_HANDLE;
    double t0 = now_ms();
    VkResult r = vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &ci, NULL, &p);
    *ms += now_ms() - t0;
    return r ? VK_NULL_HANDLE : p;
}

static uint32_t color_of(int px) { return (uint32_t)((px + 1) * 15) | 0xFF000000u; } /* RGBA8 little-endian: R, A */

/* Records the frame's pass into cb: `fresh` new pipelines (created here, at their draws) or the
 * ordinary pipeline on pixel 0. */
static int record(VkCommandBuffer cb, VkPipeline ordinary, int fresh, VkPipeline *made, double *create_ms)
{
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(cb, &bi);
    VkClearValue clear = {.color = {.float32 = {0, 0, 0, 0}}};
    VkRenderPassBeginInfo rbi = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, NULL, rp, fb, {{0, 0}, {W, W}}, 1, &clear};
    vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    int n = fresh ? fresh : 1;
    for (int i = 0; i < n; i++) {
        VkPipeline p = ordinary;
        if (fresh) {
            p = made[i] = make_pipeline(i, create_ms);
            if (!p) return 0;
        }
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
        uint32_t c = color_of(i);
        float pc[8] = {(c & 0xFF) / 255.0f, 0, 0, 1, 0, 0, 0, 0};
        vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), pc);
        vkCmdDraw(cb, 3, 1, 0, 0);
    }
    vkCmdEndRenderPass(cb);
    VkBufferImageCopy cp = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {W, W, 1}};
    vkCmdCopyImageToBuffer(cb, target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rbuf, 1, &cp);
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    return vkEndCommandBuffer(cb) == VK_SUCCESS;
}

/* Pixels right after the frame: drawn ones counted, a wrong value (neither colour nor clear) -1. */
static int drawn(int n)
{
    int ok = 0;
    for (int i = 0; i < W * W; i++) {
        uint32_t v;
        memcpy(&v, rb + i * 4, 4);
        uint32_t want = i < n ? color_of(i) : 0;
        if (v == want) ok += i < n;
        else if (v != 0) return -1;
    }
    return ok;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    int small = 2, small_frames = 5, burst = 8;
    const char *tag = "run";
    for (int i = 1; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "--small")) small = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--small-frames")) small_frames = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--burst")) burst = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--tag")) tag = argv[i + 1];
        else if (!strcmp(argv[i], "--odd-fullscreen")) odd_fullscreen = atoi(argv[i + 1]);
    }
    if (small < 1 || small > W * W - 1 || burst < 1 || burst > W * W - 1) {
        printf("FAILED: setup: at most %d pipelines a frame\n", W * W - 1);
        return 2;
    }
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib) {
        printf("FAILED: loader: %s\n", dlerror());
        return 2;
    }
    gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
#define G(n) n = (PFN_##n)gipa(inst, #n)
    G(vkCreateInstance);
    const char *iexts[2] = {"VK_KHR_surface", "VK_EXT_headless_surface"};
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "compileburst_check", 1, "DXVK", VK_MAKE_VERSION(2, 7, 1),
                             VK_API_VERSION_1_1};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 2, iexts};
    if (vkCreateInstance(&ici, NULL, &inst)) {
        printf("FAILED: setup: vkCreateInstance\n");
        return 2;
    }
    G(vkEnumeratePhysicalDevices); G(vkGetPhysicalDeviceQueueFamilyProperties); G(vkCreateDevice); G(vkGetDeviceQueue);
    G(vkCreateCommandPool); G(vkAllocateCommandBuffers); G(vkBeginCommandBuffer); G(vkEndCommandBuffer); G(vkCmdPipelineBarrier);
    G(vkQueueSubmit); G(vkDeviceWaitIdle); G(vkCreateHeadlessSurfaceEXT); G(vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
    G(vkGetPhysicalDeviceSurfaceSupportKHR); G(vkCreateSwapchainKHR); G(vkGetSwapchainImagesKHR); G(vkAcquireNextImageKHR);
    G(vkQueuePresentKHR); G(vkCreateFence); G(vkWaitForFences); G(vkResetFences); G(vkCreateShaderModule); G(vkCreatePipelineLayout);
    G(vkCreateGraphicsPipelines); G(vkDestroyPipeline); G(vkGetPhysicalDeviceMemoryProperties); G(vkAllocateMemory); G(vkCreateImage);
    G(vkGetImageMemoryRequirements); G(vkBindImageMemory); G(vkCreateBuffer); G(vkGetBufferMemoryRequirements); G(vkBindBufferMemory);
    G(vkMapMemory); G(vkResetCommandBuffer); G(vkCreateImageView); G(vkCreateRenderPass); G(vkCreateFramebuffer);
    G(vkCmdBeginRenderPass); G(vkCmdEndRenderPass); G(vkCmdBindPipeline); G(vkCmdPushConstants); G(vkCmdDraw); G(vkCmdCopyImageToBuffer);
#undef G
    uint32_t n = 1;
    vkEnumeratePhysicalDevices(inst, &n, &pd);
    if (!n) {
        printf("FAILED: setup: no GPU\n");
        return 2;
    }
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    VkSurfaceKHR surface;
    VkHeadlessSurfaceCreateInfoEXT hci = {VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT};
    if (!vkCreateHeadlessSurfaceEXT || vkCreateHeadlessSurfaceEXT(inst, &hci, NULL, &surface)) {
        printf("FAILED: setup: vkCreateHeadlessSurfaceEXT\n");
        return 2;
    }
    uint32_t nq = 16;
    VkQueueFamilyProperties qp[16];
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qp);
    uint32_t family = UINT32_MAX;
    for (uint32_t i = 0; i < nq && family == UINT32_MAX; i++) {
        VkBool32 ok = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(pd, i, surface, &ok);
        if ((qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && ok) family = i;
    }
    if (family == UINT32_MAX) {
        printf("FAILED: setup: no queue can present\n");
        return 2;
    }
    float prio = 1;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, family, 1, &prio};
    const char *dexts[1] = {"VK_KHR_swapchain"};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &qci, 0, NULL, 1, dexts, NULL};
    if (vkCreateDevice(pd, &dci, NULL, &dev)) {
        printf("FAILED: setup: vkCreateDevice\n");
        return 2;
    }
    vkGetDeviceQueue(dev, family, 0, &queue);
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surface, &caps);
    uint32_t want = caps.minImageCount > 3 ? caps.minImageCount : 3;
    if (caps.maxImageCount && want > caps.maxImageCount) want = caps.maxImageCount;
    VkExtent2D ext = caps.currentExtent.width == 0xFFFFFFFFu ? (VkExtent2D){64, 64} : caps.currentExtent;
    VkSwapchainCreateInfoKHR sci = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, NULL, 0, surface, want, VK_FORMAT_B8G8R8A8_UNORM,
                                    VK_COLOR_SPACE_SRGB_NONLINEAR_KHR, ext, 1, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                    VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
                                    VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_PRESENT_MODE_FIFO_KHR, VK_TRUE, VK_NULL_HANDLE};
    VkSwapchainKHR sc;
    if (vkCreateSwapchainKHR(dev, &sci, NULL, &sc)) {
        printf("FAILED: setup: vkCreateSwapchainKHR\n");
        return 2;
    }
    uint32_t nimg = 8;
    VkImage imgs[8];
    vkGetSwapchainImagesKHR(dev, sc, &nimg, imgs);
    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, family};
    VkCommandPool pool;
    vkCreateCommandPool(dev, &pci, NULL, &pool);
    VkCommandBuffer cbs[9];
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, nimg + 1};
    vkAllocateCommandBuffers(dev, &cai, cbs);
    VkCommandBuffer work = cbs[nimg];
    for (uint32_t i = 0; i < nimg; i++) {
        VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        vkBeginCommandBuffer(cbs[i], &bi);
        VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, 0, 0, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                                  VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, imgs[i], {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        vkCmdPipelineBarrier(cbs[i], VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
        vkEndCommandBuffer(cbs[i]);
    }
    /* the 4x4 target, its pass, the readback */
    {
        VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {W, W, 1}, 1, 1,
                                VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL,
                                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL,
                                VK_IMAGE_LAYOUT_UNDEFINED};
        VkMemoryRequirements mr;
        VkDeviceMemory m;
        if (vkCreateImage(dev, &ii, NULL, &target)) goto setup_failed;
        vkGetImageMemoryRequirements(dev, target, &mr);
        VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, mem_type(mr.memoryTypeBits, 0)};
        if (vkAllocateMemory(dev, &mai, NULL, &m) || vkBindImageMemory(dev, target, m, 0)) goto setup_failed;
        VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, target, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM,
                                     {0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        VkImageView view;
        if (vkCreateImageView(dev, &vci, NULL, &view)) goto setup_failed;
        VkAttachmentDescription att = {0, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR,
                                       VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE,
                                       VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL};
        VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub = {0, VK_PIPELINE_BIND_POINT_GRAPHICS, 0, NULL, 1, &ref, NULL, NULL, 0, NULL};
        VkSubpassDependency dep = {0, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                   VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, 0};
        VkRenderPassCreateInfo rpci = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, NULL, 0, 1, &att, 1, &sub, 1, &dep};
        if (vkCreateRenderPass(dev, &rpci, NULL, &rp)) goto setup_failed;
        VkFramebufferCreateInfo fci = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, NULL, 0, rp, 1, &view, W, W, 1};
        if (vkCreateFramebuffer(dev, &fci, NULL, &fb)) goto setup_failed;
        VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, W * W * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                  VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
        if (vkCreateBuffer(dev, &bci, NULL, &rbuf)) goto setup_failed;
        vkGetBufferMemoryRequirements(dev, rbuf, &mr);
        mai = (VkMemoryAllocateInfo){VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size,
                                     mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
        if (mai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(dev, &mai, NULL, &m) || vkBindBufferMemory(dev, rbuf, m, 0) ||
            vkMapMemory(dev, m, 0, VK_WHOLE_SIZE, 0, (void **)&rb))
            goto setup_failed;
        VkShaderModuleCreateInfo smci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof(spv_vert), spv_vert};
        if (vkCreateShaderModule(dev, &smci, NULL, &vs)) goto setup_failed;
        smci.codeSize = sizeof(spv_frag), smci.pCode = spv_frag;
        if (vkCreateShaderModule(dev, &smci, NULL, &fs)) goto setup_failed;
        VkPushConstantRange pcr = {VK_SHADER_STAGE_VERTEX_BIT, 0, 32};
        VkPipelineLayoutCreateInfo plci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 0, NULL, 1, &pcr};
        if (vkCreatePipelineLayout(dev, &plci, NULL, &layout)) goto setup_failed;
    }
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence acq_fence, work_fence;
    vkCreateFence(dev, &fci, NULL, &acq_fence);
    vkCreateFence(dev, &fci, NULL, &work_fence);
    double ignore = 0;
    VkPipeline ordinary = make_pipeline(0, &ignore); /* at the start, before any timing */
    if (!ordinary) goto setup_failed;

    /* the plan */
    int kinds[64], nk = 0;
    for (int i = 0; i < 3; i++) kinds[nk++] = 0;
    for (int i = 0; i < small_frames && nk < 40; i++) kinds[nk++] = small;
    for (int i = 0; i < 3; i++) kinds[nk++] = 0;
    kinds[nk++] = -burst;
    for (int i = 0; i < 3; i++) kinds[nk++] = 0;

    double last_present = now_ms();
    double sum_ms[3] = {0}, max_ms[3] = {0};
    int frames_k[3] = {0}, drawn_k[3] = {0}, want_k[3] = {0}, wrong = 0;
    static const char *const names[3] = {"ordinary", "small", "burst"};
    for (int f = 0; f < nk; f++) {
        int fresh = kinds[f] < 0 ? -kinds[f] : kinds[f], k = kinds[f] < 0 ? 2 : kinds[f] > 0 ? 1 : 0;
        uint32_t idx = 0;
        VkResult r = vkAcquireNextImageKHR(dev, sc, 5000000000ull, VK_NULL_HANDLE, acq_fence, &idx);
        if (r < 0) {
            printf("FAILED: acquire %d at frame %d\n", r, f + 1);
            return 1;
        }
        vkWaitForFences(dev, 1, &acq_fence, VK_TRUE, 5000000000ull);
        vkResetFences(dev, 1, &acq_fence);
        VkPipeline made[W * W];
        double create_ms = 0;
        vkResetCommandBuffer(work, 0);
        if (!record(work, ordinary, fresh, made, &create_ms)) {
            printf("FAILED: frame %d: a pipeline was not created or the command buffer not recorded\n", f + 1);
            return 1;
        }
        memset(rb, 0x55, W * W * 4);
        VkCommandBuffer sub[2] = {work, cbs[idx]};
        VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 2, sub, 0, NULL};
        vkQueueSubmit(queue, 1, &si, work_fence);
        VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, NULL, 0, NULL, 1, &sc, &idx, NULL};
        r = vkQueuePresentKHR(queue, &pi);
        double t = now_ms(), ms = t - last_present;
        last_present = t;
        if (r < 0) {
            printf("FAILED: present %d at frame %d\n", r, f + 1);
            return 1;
        }
        vkWaitForFences(dev, 1, &work_fence, VK_TRUE, 30000000000ull);
        vkResetFences(dev, 1, &work_fence);
        int want_px = fresh ? fresh : 1, got = drawn(want_px);
        if (got < 0) wrong++;
        printf("frame %d %s ms=%.0f create_ms=%.0f drawn=%d/%d\n", f + 1, names[k], ms, create_ms, got, want_px);
        if (fresh)
            for (int i = 0; i < fresh; i++) vkDestroyPipeline(dev, made[i], NULL);
        if (f == 0) continue; /* its pipeline was made just before (and in the background may still compile) */
        frames_k[k]++;
        sum_ms[k] += ms;
        if (ms > max_ms[k]) max_ms[k] = ms;
        drawn_k[k] += got > 0 ? got : 0;
        want_k[k] += want_px;
        double t1 = now_ms();
        while (now_ms() - t1 < 5) usleep(1000); /* a few ms of the game's own work */
    }
    vkDeviceWaitIdle(dev);
    for (int k = 0; k < 3; k++)
        if (frames_k[k])
            printf("summary %s %s frames=%d mean_ms=%.0f max_ms=%.0f drawn=%d/%d\n", tag, names[k], frames_k[k], sum_ms[k] / frames_k[k], max_ms[k],
                   drawn_k[k], want_k[k]);
    if (wrong) {
        printf("FAILED: %d frame(s) with pixels neither drawn nor cleared\n", wrong);
        return 1;
    }
    printf("OK: %d frames\n", nk);
    return 0;
setup_failed:
    printf("FAILED: setup\n");
    return 2;
}
