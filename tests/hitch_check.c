/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * hitch_check: frames made slow on purpose, one cause each, for the per-hitch line
 * (VKBRIDGE_HITCH_MS; server/server_hitch.c, client/hitch.c). host/hitch_test.sh runs it against a
 * server of its own and checks that each long frame's line in the server's log names its cause.
 *
 *   hitch_check [--engine NAME] [--ms N] action...
 *
 * Three ordinary frames (an acquire, a barrier, a present on a headless surface), then for each
 * action one frame that does it before its present, then three ordinary frames again. Actions:
 *   compile  a compute pipeline whose stage carries the test marker (the server compiles it slowly
 *            with VKBRIDGE_TEST_CRASH_PIPELINE=slow, VKBRIDGE_TEST_SLOW_S)
 *   bc       a BC1 texture of 256x256 uploaded and waited for (with VKBRIDGE_FAKE_MISSING=bc the
 *            server converts it; VKBRIDGE_TEST_HITCH_DELAY_MS makes that slow)
 *   alloc    128 MB of device-local memory (slow with VKBRIDGE_TEST_HITCH_DELAY_MS)
 *   gate     a submit that waits for a timeline semaphore another thread signals N ms later
 *            (--ms, default 400), waited for: the GPU queue held, nothing converted
 *   gatebc   the same with a BC upload in the held submit: the GPU's wait is the conversion's
 *   sleep    N ms on the CPU, nothing else (the game's own work)
 * Prints "action <name> frame <n>" (n: the present that ends the slow frame, counted from 1, as
 * the hitch line's frame=) and "OK: ..." / "FAILED: ..." last. --engine names the instance's
 * engine (DXVK: the client's DXVK config check runs at the first present).
 */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "smoke_spv.h"

#define TEST_CRASH_SPEC_ID 0x7E57C0DEu /* server_guard.c */

static PFN_vkGetInstanceProcAddr gipa;
static VkInstance inst;
static VkPhysicalDevice pd;
static VkDevice dev;
static VkQueue queue;
static VkPhysicalDeviceMemoryProperties mp;

#define I(name) static PFN_##name name
I(vkCreateInstance); I(vkEnumeratePhysicalDevices); I(vkGetPhysicalDeviceQueueFamilyProperties); I(vkCreateDevice);
I(vkGetDeviceQueue); I(vkCreateCommandPool); I(vkAllocateCommandBuffers); I(vkBeginCommandBuffer); I(vkEndCommandBuffer);
I(vkCmdPipelineBarrier); I(vkQueueSubmit); I(vkCreateSemaphore); I(vkSignalSemaphore); I(vkQueueWaitIdle); I(vkDeviceWaitIdle);
I(vkCreateHeadlessSurfaceEXT); I(vkGetPhysicalDeviceSurfaceCapabilitiesKHR); I(vkGetPhysicalDeviceSurfaceSupportKHR);
I(vkCreateSwapchainKHR); I(vkGetSwapchainImagesKHR); I(vkAcquireNextImageKHR); I(vkQueuePresentKHR); I(vkCreateFence);
I(vkWaitForFences); I(vkResetFences); I(vkCreateShaderModule); I(vkCreateDescriptorSetLayout); I(vkCreatePipelineLayout);
I(vkCreateComputePipelines); I(vkDestroyPipeline); I(vkGetPhysicalDeviceMemoryProperties); I(vkAllocateMemory); I(vkFreeMemory);
I(vkCreateImage); I(vkGetImageMemoryRequirements); I(vkBindImageMemory); I(vkCreateBuffer); I(vkGetBufferMemoryRequirements);
I(vkBindBufferMemory); I(vkMapMemory); I(vkCmdCopyBufferToImage); I(vkResetCommandBuffer); I(vkDestroyImage); I(vkDestroyBuffer);
#undef I

static uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want)
{
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

/* ------------------------------------------------------------------ the gate (a held queue) */

static VkSemaphore gate;
static uint64_t gate_value;
static int hold_ms = 400;

static void *signal_later(void *arg)
{
    uint64_t v = (uint64_t)(uintptr_t)arg;
    usleep((useconds_t)hold_ms * 1000);
    VkSemaphoreSignalInfo si = {VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, NULL, gate, v};
    vkSignalSemaphore(dev, &si);
    return NULL;
}

/* ------------------------------------------------------------------ the actions */

static VkCommandPool pool;
static VkCommandBuffer work_cb;
static VkFence work_fence;

static int compile_slow(void)
{
    VkDescriptorSetLayoutBinding binds[2] = {{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
                                             {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
    VkDescriptorSetLayoutCreateInfo lci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 2, binds};
    VkDescriptorSetLayout dsl;
    VkPipelineLayout pl;
    vkCreateDescriptorSetLayout(dev, &lci, NULL, &dsl);
    VkPushConstantRange pcr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 8};
    VkPipelineLayoutCreateInfo plci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &dsl, 1, &pcr};
    vkCreatePipelineLayout(dev, &plci, NULL, &pl);
    VkShaderModuleCreateInfo mci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof(spv_smoke_comp), spv_smoke_comp};
    VkShaderModule m;
    if (vkCreateShaderModule(dev, &mci, NULL, &m)) return 0;
    static const VkSpecializationMapEntry e = {TEST_CRASH_SPEC_ID, 0, 4};
    static const uint32_t one = 1;
    VkSpecializationInfo spec = {1, &e, 4, &one};
    VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = (VkPipelineShaderStageCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_COMPUTE_BIT, m,
                                                 "main", &spec};
    ci.layout = pl;
    VkPipeline p;
    if (vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &ci, NULL, &p)) return 0;
    vkDestroyPipeline(dev, p, NULL);
    return 1;
}

/* Records a BC1 upload of 256x256 into work_cb (begun). */
static int record_bc(void)
{
    VkImageCreateInfo ici = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, VK_FORMAT_BC1_RGBA_UNORM_BLOCK, {256, 256, 1}, 1,
                             1, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                             VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
    VkImage img;
    if (vkCreateImage(dev, &ici, NULL, &img)) return 0;
    VkMemoryRequirements r;
    vkGetImageMemoryRequirements(dev, img, &r);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, r.size, mem_type(r.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    VkDeviceMemory im;
    if (ai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(dev, &ai, NULL, &im) || vkBindImageMemory(dev, img, im, 0)) return 0;
    const VkDeviceSize bytes = 64 * 64 * 8;
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_SHARING_MODE_EXCLUSIVE};
    VkBuffer buf;
    if (vkCreateBuffer(dev, &bci, NULL, &buf)) return 0;
    vkGetBufferMemoryRequirements(dev, buf, &r);
    ai = (VkMemoryAllocateInfo){VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, r.size,
                                mem_type(r.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    VkDeviceMemory bm;
    void *ptr;
    if (ai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(dev, &ai, NULL, &bm) || vkBindBufferMemory(dev, buf, bm, 0) ||
        vkMapMemory(dev, bm, 0, bytes, 0, &ptr))
        return 0;
    for (VkDeviceSize i = 0; i < bytes; i++) ((unsigned char *)ptr)[i] = (unsigned char)(i * 37);
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, img,
                              {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(work_cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    VkBufferImageCopy c = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {256, 256, 1}};
    vkCmdCopyBufferToImage(work_cb, buf, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    return 1; /* the image and buffer stay (a few KB each, the test is short) */
}

/* A submit of work_cb (bc: with a BC upload; gated: held until the gate opens), waited for. */
static int submit_wait(int bc, int gated)
{
    vkResetCommandBuffer(work_cb, 0);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(work_cb, &bi);
    if (bc && !record_bc()) return 0;
    vkEndCommandBuffer(work_cb);
    uint64_t v = ++gate_value;
    VkPipelineStageFlags st = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkTimelineSemaphoreSubmitInfo ts = {VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO, NULL, 1, &v, 0, NULL};
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, gated ? &ts : NULL, gated ? 1 : 0, gated ? &gate : NULL, &st, 1, &work_cb, 0, NULL};
    pthread_t th;
    if (vkQueueSubmit(queue, 1, &si, work_fence)) return 0;
    int th_on = gated && !pthread_create(&th, NULL, signal_later, (void *)(uintptr_t)v);
    if (gated && !th_on) return 0;
    VkResult r = vkWaitForFences(dev, 1, &work_fence, VK_TRUE, 30000000000ull);
    vkResetFences(dev, 1, &work_fence);
    if (th_on) pthread_join(th, NULL);
    return r == VK_SUCCESS;
}

static int alloc_big(void)
{
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, 128ull << 20, mem_type(~0u, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    VkDeviceMemory m;
    if (ai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(dev, &ai, NULL, &m)) return 0;
    vkFreeMemory(dev, m, NULL);
    return 1;
}

static int act(const char *a)
{
    if (!strcmp(a, "compile")) return compile_slow();
    if (!strcmp(a, "bc")) return submit_wait(1, 0);
    if (!strcmp(a, "alloc")) return alloc_big();
    if (!strcmp(a, "gate")) return submit_wait(0, 1);
    if (!strcmp(a, "gatebc")) return submit_wait(1, 1);
    if (!strcmp(a, "sleep")) return usleep((useconds_t)hold_ms * 1000), 1;
    printf("FAILED: unknown action %s\n", a);
    return 0;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    const char *engine = "vkbridge";
    int first = 1;
    while (first < argc && argv[first][0] == '-') {
        if (!strcmp(argv[first], "--engine") && first + 1 < argc) engine = argv[++first];
        else if (!strcmp(argv[first], "--ms") && first + 1 < argc) hold_ms = atoi(argv[++first]);
        first++;
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
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "vkbridge-hitch-check", 1, engine, VK_MAKE_VERSION(2, 7, 1),
                             VK_API_VERSION_1_2};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 2, iexts};
    if (vkCreateInstance(&ici, NULL, &inst)) {
        printf("FAILED: setup: vkCreateInstance\n");
        return 2;
    }
    G(vkEnumeratePhysicalDevices); G(vkGetPhysicalDeviceQueueFamilyProperties); G(vkCreateDevice); G(vkGetDeviceQueue);
    G(vkCreateCommandPool); G(vkAllocateCommandBuffers); G(vkBeginCommandBuffer); G(vkEndCommandBuffer); G(vkCmdPipelineBarrier);
    G(vkQueueSubmit); G(vkCreateSemaphore); G(vkSignalSemaphore); G(vkQueueWaitIdle); G(vkDeviceWaitIdle);
    G(vkCreateHeadlessSurfaceEXT); G(vkGetPhysicalDeviceSurfaceCapabilitiesKHR); G(vkGetPhysicalDeviceSurfaceSupportKHR);
    G(vkCreateSwapchainKHR); G(vkGetSwapchainImagesKHR); G(vkAcquireNextImageKHR); G(vkQueuePresentKHR); G(vkCreateFence);
    G(vkWaitForFences); G(vkResetFences); G(vkCreateShaderModule); G(vkCreateDescriptorSetLayout); G(vkCreatePipelineLayout);
    G(vkCreateComputePipelines); G(vkDestroyPipeline); G(vkGetPhysicalDeviceMemoryProperties); G(vkAllocateMemory); G(vkFreeMemory);
    G(vkCreateImage); G(vkGetImageMemoryRequirements); G(vkBindImageMemory); G(vkCreateBuffer); G(vkGetBufferMemoryRequirements);
    G(vkBindBufferMemory); G(vkMapMemory); G(vkCmdCopyBufferToImage); G(vkResetCommandBuffer); G(vkDestroyImage); G(vkDestroyBuffer);
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
    VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    f12.timelineSemaphore = VK_TRUE;
    VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f12};
    f2.features.textureCompressionBC = VK_TRUE; /* emulated by the server where the GPU lacks it */
    const char *dexts[1] = {"VK_KHR_swapchain"};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f2, 0, 1, &qci, 0, NULL, 1, dexts, NULL};
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
    vkCreateCommandPool(dev, &pci, NULL, &pool);
    VkCommandBuffer cbs[9];
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, nimg + 1};
    vkAllocateCommandBuffers(dev, &cai, cbs);
    work_cb = cbs[nimg];
    for (uint32_t i = 0; i < nimg; i++) {
        VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        vkBeginCommandBuffer(cbs[i], &bi);
        VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, 0, 0, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                                  VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, imgs[i], {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        vkCmdPipelineBarrier(cbs[i], VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
        vkEndCommandBuffer(cbs[i]);
    }
    VkSemaphoreTypeCreateInfo ti = {VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, NULL, VK_SEMAPHORE_TYPE_TIMELINE, 0};
    VkSemaphoreCreateInfo tci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &ti};
    vkCreateSemaphore(dev, &tci, NULL, &gate);
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence acq_fence;
    vkCreateFence(dev, &fci, NULL, &acq_fence);
    vkCreateFence(dev, &fci, NULL, &work_fence);

    /* the plan: 3 ordinary frames, then per action one slow frame and 3 ordinary ones */
    int nact = argc - first, total = 3 + nact * 4, presents = 0, failed = 0;
    for (int f = 0; f < total && !failed; f++) {
        uint32_t idx = 0;
        VkResult r = vkAcquireNextImageKHR(dev, sc, 5000000000ull, VK_NULL_HANDLE, acq_fence, &idx);
        if (r < 0) {
            printf("FAILED: acquire %d at frame %d\n", r, f);
            return 1;
        }
        vkWaitForFences(dev, 1, &acq_fence, VK_TRUE, 5000000000ull);
        vkResetFences(dev, 1, &acq_fence);
        int k = f - 3;
        if (k >= 0 && k % 4 == 0) {
            const char *a = argv[first + k / 4];
            if (!act(a)) {
                printf("FAILED: action %s\n", a);
                failed = 1;
                break;
            }
            printf("action %s frame %d\n", a, presents + 1);
        }
        VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cbs[idx], 0, NULL};
        vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
        VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, NULL, 0, NULL, 1, &sc, &idx, NULL};
        r = vkQueuePresentKHR(queue, &pi);
        presents++;
        if (r < 0) {
            printf("FAILED: present %d at frame %d\n", r, f);
            return 1;
        }
        usleep(5000); /* ordinary frames: a few ms */
    }
    vkDeviceWaitIdle(dev);
    if (failed) return 1;
    printf("OK: %d frames, %d slow on purpose\n", presents, nact);
    return 0;
}
