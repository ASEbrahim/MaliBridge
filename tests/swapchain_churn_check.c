/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * swapchain_churn_check: a program that makes a new VkSurfaceKHR and a new swapchain for every frame (a player log: KOTOR through
 * Zink on a POCO X8 Pro, ~6 per second, 8 images each = ~950 round trips a second) must not pay the image set's creation
 * each time: the client keeps a destroyed swapchain's idle images for a few seconds and gives them to the next swapchain of
 * the same device, extent, format, usage and count (client/wsi.c). Real resizes must still work.
 *
 *   swapchain_churn_check [--cycles N] [--phase A|B|C]
 *
 *   A  N x (create surface, swapchain 64x64, acquire, present, destroy swapchain, destroy surface)
 *   B  N x the same with the extent alternating 64x64 / 96x80 (a resize each cycle)
 *   C  one cycle at a new extent and count
 * The script (swapchain_churn_test.sh) counts the client's vkCreateImage round trips (VKBRIDGE_STATS) per phase.
 * One line per step, "OK: ..." / "FAILED: ..." last (exit 0 / 1).
 */
#define _GNU_SOURCE
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static PFN_vkGetInstanceProcAddr gipa;
static VkInstance inst;
static VkDevice dev;
#define I(name) static PFN_##name name
I(vkCreateInstance); I(vkEnumeratePhysicalDevices); I(vkGetPhysicalDeviceQueueFamilyProperties); I(vkCreateDevice);
I(vkGetDeviceQueue); I(vkCreateHeadlessSurfaceEXT); I(vkDestroySurfaceKHR); I(vkCreateSwapchainKHR); I(vkGetSwapchainImagesKHR);
I(vkAcquireNextImageKHR); I(vkQueuePresentKHR); I(vkDestroySwapchainKHR); I(vkCreateFence); I(vkWaitForFences); I(vkResetFences);
I(vkDeviceWaitIdle); I(vkDestroyDevice); I(vkDestroyFence);
#undef I

static int fails;
static void step(int ok, const char *what)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) fails++;
}

static VkPhysicalDevice pd;
static VkQueue queue;
static VkFence fence;

/* one frame: surface + swapchain of w x h, acquire, present, destroy both. 1 when everything succeeded. */
static int cycle(uint32_t w, uint32_t h, uint32_t want, uint32_t *got)
{
    VkSurfaceKHR s;
    VkHeadlessSurfaceCreateInfoEXT hci = {VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT};
    if (vkCreateHeadlessSurfaceEXT(inst, &hci, NULL, &s)) return 0;
    VkSwapchainKHR sc;
    VkSwapchainCreateInfoKHR sci = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, NULL, 0, s, want, VK_FORMAT_B8G8R8A8_UNORM,
                                    VK_COLOR_SPACE_SRGB_NONLINEAR_KHR, {w, h}, 1,
                                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE,
                                    0, NULL, VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR, VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
                                    VK_PRESENT_MODE_FIFO_KHR, VK_TRUE, VK_NULL_HANDLE};
    if (vkCreateSwapchainKHR(dev, &sci, NULL, &sc)) return 0;
    uint32_t n = 0;
    vkGetSwapchainImagesKHR(dev, sc, &n, NULL);
    *got = n;
    uint32_t idx = 0;
    VkResult r = vkAcquireNextImageKHR(dev, sc, 5000000000ull, VK_NULL_HANDLE, fence, &idx);
    if (r < 0) return 0;
    vkWaitForFences(dev, 1, &fence, VK_TRUE, 5000000000ull);
    vkResetFences(dev, 1, &fence);
    VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, NULL, 0, NULL, 1, &sc, &idx, NULL};
    r = vkQueuePresentKHR(queue, &pi);
    if (r < 0) return 0;
    vkDestroySwapchainKHR(dev, sc, NULL);
    vkDestroySurfaceKHR(inst, s, NULL);
    return 1;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    int cycles = 60;
    const char *only = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--cycles") && i + 1 < argc) cycles = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--phase") && i + 1 < argc) only = argv[++i];
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
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "swapchain-churn-check", 1, "vkbridge", 1, VK_API_VERSION_1_1};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 2, iexts};
    if (vkCreateInstance(&ici, NULL, &inst)) {
        printf("FAILED: setup: vkCreateInstance\n");
        return 2;
    }
    G(vkEnumeratePhysicalDevices); G(vkGetPhysicalDeviceQueueFamilyProperties); G(vkCreateDevice); G(vkGetDeviceQueue);
    G(vkCreateHeadlessSurfaceEXT); G(vkDestroySurfaceKHR); G(vkCreateSwapchainKHR); G(vkGetSwapchainImagesKHR);
    G(vkAcquireNextImageKHR); G(vkQueuePresentKHR); G(vkDestroySwapchainKHR); G(vkCreateFence); G(vkWaitForFences);
    G(vkResetFences); G(vkDeviceWaitIdle); G(vkDestroyDevice); G(vkDestroyFence);
#undef G
    uint32_t n = 1;
    vkEnumeratePhysicalDevices(inst, &n, &pd);
    if (!n) {
        printf("FAILED: setup: no GPU\n");
        return 2;
    }
    uint32_t nq = 16;
    VkQueueFamilyProperties qp[16];
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qp);
    uint32_t family = UINT32_MAX;
    for (uint32_t i = 0; i < nq && family == UINT32_MAX; i++)
        if (qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) family = i;
    float prio = 1;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, family, 1, &prio};
    const char *dexts[1] = {"VK_KHR_swapchain"};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &qci, 0, NULL, 1, dexts, NULL};
    if (vkCreateDevice(pd, &dci, NULL, &dev)) {
        printf("FAILED: setup: vkCreateDevice\n");
        return 2;
    }
    vkGetDeviceQueue(dev, family, 0, &queue);
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    vkCreateFence(dev, &fci, NULL, &fence);

    uint32_t got = 0;
    int ok = 1;
    if (!only || !strcmp(only, "A")) {
        for (int i = 0; i < cycles && ok; i++) ok = cycle(64, 64, 4, &got);
        step(ok && got >= 4, "A same-size swapchain per frame: every cycle created, presented, destroyed");
    }
    if (!only || !strcmp(only, "B")) {
        ok = 1;
        for (int i = 0; i < cycles && ok; i++) ok = (i & 1) ? cycle(96, 80, 4, &got) : cycle(64, 64, 4, &got);
        step(ok && got >= 4, "B extent alternating each frame (a resize each cycle): every cycle worked");
    }
    if (!only || !strcmp(only, "C")) {
        ok = cycle(128, 72, 3, &got);
        step(ok && got >= 3, "C a new extent and count: a fresh set, three images or more");
    }
    vkDeviceWaitIdle(dev);
    vkDestroyFence(dev, fence, NULL);
    vkDestroyDevice(dev, NULL);
    if (fails) {
        printf("FAILED: %d step(s)\n", fails);
        return 1;
    }
    printf("OK: %d cycles per phase done\n", cycles);
    return 0;
}
