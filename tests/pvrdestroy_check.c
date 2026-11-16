/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * pvrdestroy_check: a process that ends its device with render objects still alive in it (a player log,
 * Pixel 10 PowerVR: the self-test's vkDestroyDevice never returned with its images, views, render
 * passes and framebuffers alive, and gamescope's vkCreateDevice then waited behind it for good).
 * It makes an image with memory, an image view, a render pass (and one through vkCreateRenderPass2
 * where the device has it), a framebuffer, a sampler, a buffer with a texel view, a fence, a
 * semaphore and an event, then
 *   pvrdestroy_check leak    vkDestroyDevice with all of it alive (as tests/smoke.c does)
 *   pvrdestroy_check exit    _exit(0) with all of it alive (the server's process cleanup)
 *   pvrdestroy_check kill    prints "made" and waits to be killed (SIGKILL mid-session; SIGALRM ends it after 60 s)
 *   pvrdestroy_check clean   destroys all of it, then the device, as an app should
 * Prints "made: ..." once everything is made and "OK: ..." when it returns from vkDestroyDevice.
 * The suite (host/pvrdestroy_test.sh) runs it against the server with the PowerVR imitation on.
 */
#define _GNU_SOURCE
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "leak";
    alarm(60); /* never outlives its test, whatever the test does (the "kill" mode waits to be killed) */
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib) return printf("FAILED: loader: %s\n", dlerror()), 2;
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    VkInstance inst;
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "vkbridge-pvrdestroy-check", 1, "vkbridge", 1, VK_API_VERSION_1_2};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 0, NULL};
    if (((PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance"))(&ici, NULL, &inst)) return printf("FAILED: vkCreateInstance\n"), 1;
    uint32_t n = 1;
    VkPhysicalDevice pd;
    if (((PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices"))(inst, &n, &pd) < 0 || !n)
        return printf("FAILED: no physical device\n"), 1;
    VkPhysicalDeviceProperties props;
    ((PFN_vkGetPhysicalDeviceProperties)gipa(inst, "vkGetPhysicalDeviceProperties"))(pd, &props);
    VkPhysicalDeviceMemoryProperties mp;
    ((PFN_vkGetPhysicalDeviceMemoryProperties)gipa(inst, "vkGetPhysicalDeviceMemoryProperties"))(pd, &mp);
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, 0, 1, &prio};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &qci, 0, NULL, 0, NULL, NULL};
    VkDevice dev;
    if (((PFN_vkCreateDevice)gipa(inst, "vkCreateDevice"))(pd, &dci, NULL, &dev)) return printf("FAILED: vkCreateDevice\n"), 1;
    PFN_vkGetDeviceProcAddr gdpa = (PFN_vkGetDeviceProcAddr)gipa(inst, "vkGetDeviceProcAddr");
#define G(name) PFN_##name name = (PFN_##name)gdpa(dev, #name)
    G(vkCreateImage); G(vkDestroyImage); G(vkGetImageMemoryRequirements); G(vkAllocateMemory); G(vkFreeMemory); G(vkBindImageMemory);
    G(vkCreateImageView); G(vkDestroyImageView); G(vkCreateRenderPass); G(vkCreateRenderPass2); G(vkDestroyRenderPass);
    G(vkCreateFramebuffer); G(vkDestroyFramebuffer); G(vkCreateSampler); G(vkDestroySampler); G(vkCreateBuffer); G(vkDestroyBuffer);
    G(vkGetBufferMemoryRequirements); G(vkBindBufferMemory); G(vkCreateBufferView); G(vkDestroyBufferView); G(vkCreateFence);
    G(vkDestroyFence); G(vkCreateSemaphore); G(vkDestroySemaphore); G(vkCreateEvent); G(vkDestroyEvent); G(vkDestroyDevice);
    G(vkDeviceWaitIdle);
#undef G
    PFN_vkDestroyInstance vkDestroyInstance = (PFN_vkDestroyInstance)gipa(inst, "vkDestroyInstance");
#define MEMFOR(mr, out)                                                                                 \
    do {                                                                                              \
        uint32_t ti = 0;                                                                              \
        while (ti < mp.memoryTypeCount && !((mr).memoryTypeBits & (1u << ti))) ti++;                  \
        VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, (mr).size, ti};    \
        if (vkAllocateMemory(dev, &mai, NULL, &(out))) return printf("FAILED: memory\n"), 1;          \
    } while (0)

    VkImageCreateInfo imci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {64, 64, 1}, 1, 1,
                              VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL,
                              VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL,
                              VK_IMAGE_LAYOUT_UNDEFINED};
    VkImage img;
    VkDeviceMemory imem, bmem;
    VkMemoryRequirements mr;
    if (vkCreateImage(dev, &imci, NULL, &img)) return printf("FAILED: image\n"), 1;
    vkGetImageMemoryRequirements(dev, img, &mr);
    MEMFOR(mr, imem);
    if (vkBindImageMemory(dev, img, imem, 0)) return printf("FAILED: bind image\n"), 1;
    VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, img, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM,
                                 {0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkImageView view;
    if (vkCreateImageView(dev, &vci, NULL, &view)) return printf("FAILED: view\n"), 1;
    VkAttachmentDescription ad = {0, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
                                  VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED,
                                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkAttachmentReference ar = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sd = {0, VK_PIPELINE_BIND_POINT_GRAPHICS, 0, NULL, 1, &ar, NULL, NULL, 0, NULL};
    VkRenderPassCreateInfo rpci = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, NULL, 0, 1, &ad, 1, &sd, 0, NULL};
    VkRenderPass rp, rp2 = VK_NULL_HANDLE;
    if (vkCreateRenderPass(dev, &rpci, NULL, &rp)) return printf("FAILED: render pass\n"), 1;
    if (vkCreateRenderPass2 && props.apiVersion >= VK_API_VERSION_1_2) {
        VkAttachmentDescription2 ad2 = {VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2, NULL, 0, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT,
                                        VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                        VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkAttachmentReference2 ar2 = {VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2, NULL, 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                      VK_IMAGE_ASPECT_COLOR_BIT};
        VkSubpassDescription2 sd2 = {VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2, NULL, 0, VK_PIPELINE_BIND_POINT_GRAPHICS, 0, 0, NULL, 1, &ar2};
        VkRenderPassCreateInfo2 rpci2 = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2, NULL, 0, 1, &ad2, 1, &sd2};
        if (vkCreateRenderPass2(dev, &rpci2, NULL, &rp2)) rp2 = VK_NULL_HANDLE;
    }
    VkFramebufferCreateInfo fbci = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, NULL, 0, rp, 1, &view, 64, 64, 1};
    VkFramebuffer fb;
    if (vkCreateFramebuffer(dev, &fbci, NULL, &fb)) return printf("FAILED: framebuffer\n"), 1;
    VkSamplerCreateInfo sci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    VkSampler smp;
    if (vkCreateSampler(dev, &sci, NULL, &smp)) return printf("FAILED: sampler\n"), 1;
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, 65536,
                              VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 0, 0, NULL};
    VkBuffer buf;
    if (vkCreateBuffer(dev, &bci, NULL, &buf)) return printf("FAILED: buffer\n"), 1;
    vkGetBufferMemoryRequirements(dev, buf, &mr);
    MEMFOR(mr, bmem);
    if (vkBindBufferMemory(dev, buf, bmem, 0)) return printf("FAILED: bind buffer\n"), 1;
    VkBufferViewCreateInfo bvci = {VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO, NULL, 0, buf, VK_FORMAT_R32_UINT, 0, VK_WHOLE_SIZE};
    VkBufferView bv;
    if (vkCreateBufferView(dev, &bvci, NULL, &bv)) bv = VK_NULL_HANDLE;
    VkFence fence;
    VkSemaphore sem;
    VkEvent ev;
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkSemaphoreCreateInfo semci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkEventCreateInfo eci = {VK_STRUCTURE_TYPE_EVENT_CREATE_INFO};
    if (vkCreateFence(dev, &fci, NULL, &fence) || vkCreateSemaphore(dev, &semci, NULL, &sem) || vkCreateEvent(dev, &eci, NULL, &ev))
        return printf("FAILED: sync objects\n"), 1;
    printf("made: image, view, render pass%s, framebuffer, sampler, buffer%s, fence, semaphore, event (mode %s)\n", rp2 ? " x2" : "",
           bv ? " + texel view" : "", mode);
    fflush(stdout);
    if (!strcmp(mode, "exit")) _exit(0);
    if (!strcmp(mode, "kill"))
        for (;;) pause();
    if (!strcmp(mode, "clean")) {
        vkDeviceWaitIdle(dev);
        vkDestroyEvent(dev, ev, NULL), vkDestroySemaphore(dev, sem, NULL), vkDestroyFence(dev, fence, NULL);
        if (bv) vkDestroyBufferView(dev, bv, NULL);
        vkDestroyBuffer(dev, buf, NULL);
        vkDestroySampler(dev, smp, NULL);
        vkDestroyFramebuffer(dev, fb, NULL);
        if (rp2) vkDestroyRenderPass(dev, rp2, NULL);
        vkDestroyRenderPass(dev, rp, NULL);
        vkDestroyImageView(dev, view, NULL);
        vkDestroyImage(dev, img, NULL);
        vkFreeMemory(dev, bmem, NULL), vkFreeMemory(dev, imem, NULL);
    }
    vkDestroyDevice(dev, NULL);
    vkDestroyInstance(inst, NULL);
    printf("OK: device destroyed (%s)\n", mode);
    return 0;
}
