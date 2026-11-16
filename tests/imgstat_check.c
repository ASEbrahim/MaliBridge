/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * imgstat_check: makes images of known shapes through the bridge and holds them, so the server's
 * memory line (server_memory.c) can be read while they are alive (ledger 357: a Unity game's
 * 6,583 uncompressed images filled a 7.2 GB phone and the line could not say what they were):
 *   - RGBA8 2048x2048 with 12 levels, sampled only (mipped, a side over 1024);
 *   - RGBA8 512x512 with 10 levels, sampled only (mipped, no side over 1024);
 *   - RGBA8 1024x1024 one level, sampled only (no mips);
 *   - RGBA8 1024x1024 one level, colour attachment (a target);
 *   - RGBA16F 256x256 one level, sampled only (a second format).
 * Prints each image's memory requirements size as "size <name> <bytes>", then holds them for
 * `imgstat_check <seconds>` (default 7), destroys them, holds as long again ("destroyed") and exits 0.
 * The host test compares the server's lines with these sizes, and the last line with none.
 */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static PFN_vkGetInstanceProcAddr gipa;
static PFN_vkGetDeviceProcAddr gdpa;
static PFN_vkCreateImage create_image;
static PFN_vkGetImageMemoryRequirements image_req;
static PFN_vkDestroyImage destroy_image;
static VkImage made[8];
static int nmade;
static VkDevice dev;

static VkDeviceSize image(const char *name, VkFormat f, uint32_t size, uint32_t mips, VkImageUsageFlags usage)
{
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, f, {size, size, 1}, mips, 1,
                            VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL, usage, VK_SHARING_MODE_EXCLUSIVE, 0, NULL,
                            VK_IMAGE_LAYOUT_UNDEFINED};
    VkImage img = VK_NULL_HANDLE;
    if (create_image(dev, &ci, NULL, &img)) {
        printf("FAIL vkCreateImage %s\n", name);
        exit(1);
    }
    made[nmade++] = img;
    VkMemoryRequirements r;
    image_req(dev, img, &r);
    printf("size %s %llu\n", name, (unsigned long long)r.size);
    return r.size;
}

int main(int argc, char **argv)
{
    int hold = argc > 1 ? atoi(argv[1]) : 7;
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib) { printf("FAIL no libvulkan\n"); return 1; }
    gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    PFN_vkCreateInstance ci_ = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "imgstat_check", 1, NULL, 0, VK_API_VERSION_1_1};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 0, NULL};
    VkInstance inst;
    if (ci_(&ici, NULL, &inst)) { printf("FAIL vkCreateInstance\n"); return 1; }
    PFN_vkEnumeratePhysicalDevices enum_pd = (PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices");
    PFN_vkCreateDevice create_dev = (PFN_vkCreateDevice)gipa(inst, "vkCreateDevice");
    gdpa = (PFN_vkGetDeviceProcAddr)gipa(inst, "vkGetDeviceProcAddr");
    VkPhysicalDevice pd;
    uint32_t n = 1;
    if (enum_pd(inst, &n, &pd) < 0 || !n) { printf("FAIL no GPU\n"); return 1; }
    float prio = 1;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, 0, 1, &prio};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &qci, 0, NULL, 0, NULL, NULL};
    if (create_dev(pd, &dci, NULL, &dev)) { printf("FAIL vkCreateDevice\n"); return 1; }
    create_image = (PFN_vkCreateImage)gdpa(dev, "vkCreateImage");
    image_req = (PFN_vkGetImageMemoryRequirements)gdpa(dev, "vkGetImageMemoryRequirements");
    destroy_image = (PFN_vkDestroyImage)gdpa(dev, "vkDestroyImage");
    const VkImageUsageFlags S = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    image("big_mipped", VK_FORMAT_R8G8B8A8_UNORM, 2048, 12, S);
    image("small_mipped", VK_FORMAT_R8G8B8A8_UNORM, 512, 10, S);
    image("flat", VK_FORMAT_R8G8B8A8_UNORM, 1024, 1, S);
    image("target", VK_FORMAT_R8G8B8A8_UNORM, 1024, 1, S | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    image("half", VK_FORMAT_R16G16B16A16_SFLOAT, 256, 1, S);
    fflush(stdout);
    sleep(hold > 0 ? hold : 7);
    printf("held %d s\n", hold);
    for (int i = 0; i < nmade; i++) destroy_image(dev, made[i], NULL);
    printf("destroyed\n");
    fflush(stdout);
    sleep(hold > 0 ? hold : 7);
    return 0;
}
