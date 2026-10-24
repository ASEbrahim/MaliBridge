/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * memclass_check: every kind of resource an app makes has a memory type, as an app looks for one
 * (the first type in its memoryTypeBits with the flags it wants). On the XRing O3 (Mali-G2-Ultra,
 * driver 56, issue #1) images allow one set of the driver's types and buffers others, by usage;
 * the v0.5 client had one device-local type, backed by a type the images allow, and one shared
 * host-visible type: buffers came out with no device-local type ("a resource allowing server types
 * 0x3 gets no client type"), indirect buffers with no host-visible one (the self-test's draws:
 * "no memory type for the buffer (types 0x1 allowed)"), and gamescope stopped at findMemoryType.
 *   images     colour (sampled, attachment), storage, depth, depth-stencil: DEVICE_LOCAL
 *   buffers    vertex/index, indirect, uniform, storage, texel, staging, readback: DEVICE_LOCAL,
 *              and HOST_VISIBLE | HOST_COHERENT (mapped, written, read back)
 * Each is allocated in that type and bound. The client's memory types are printed first.
 * Exit status 0 only if every check passes.
 */
#define _GNU_SOURCE
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures, checks;
static VkPhysicalDeviceMemoryProperties mp;

static void check(int ok, const char *what, const char *detail)
{
    checks++;
    if (!ok) failures++;
    printf("%s %s%s%s\n", ok ? "PASS" : "FAIL", what, detail && *detail ? ": " : "", detail ? detail : "");
    fflush(stdout);
}

static uint32_t find_type(uint32_t bits, VkMemoryPropertyFlags want)
{
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

int main(void)
{
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib) {
        printf("FAIL loader: %s\n", dlerror());
        return 1;
    }
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
#define GI(name) PFN_##name name = (PFN_##name)gipa(inst, #name)
#define GD(name) PFN_##name name = (PFN_##name)gdpa(dev, #name)
    VkInstance inst = NULL;
    GI(vkCreateInstance);
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "memclass_check", 1, NULL, 0, VK_API_VERSION_1_1};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app};
    if (vkCreateInstance(&ici, NULL, &inst) != VK_SUCCESS) {
        printf("FAIL instance\n");
        return 1;
    }
    GI(vkEnumeratePhysicalDevices);
    GI(vkGetPhysicalDeviceMemoryProperties);
    GI(vkGetPhysicalDeviceImageFormatProperties);
    GI(vkCreateDevice);
    GI(vkGetDeviceProcAddr);
    PFN_vkGetDeviceProcAddr gdpa = vkGetDeviceProcAddr;
    uint32_t n = 1;
    VkPhysicalDevice pd;
    vkEnumeratePhysicalDevices(inst, &n, &pd);
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, 0, 1, &prio};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &qci};
    VkDevice dev;
    if (vkCreateDevice(pd, &dci, NULL, &dev) != VK_SUCCESS) {
        check(0, "device", "");
        return 1;
    }
    GD(vkCreateBuffer);
    GD(vkDestroyBuffer);
    GD(vkGetBufferMemoryRequirements);
    GD(vkCreateImage);
    GD(vkDestroyImage);
    GD(vkGetImageMemoryRequirements);
    GD(vkAllocateMemory);
    GD(vkFreeMemory);
    GD(vkBindBufferMemory);
    GD(vkBindImageMemory);
    GD(vkMapMemory);
    GD(vkUnmapMemory);
    GD(vkDestroyDevice);

    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    char line[512];
    int ln = 0;
    for (uint32_t i = 0; i < mp.memoryTypeCount && ln < (int)sizeof(line) - 24; i++)
        ln += snprintf(line + ln, sizeof(line) - (size_t)ln, " %u:0x%x%s", i, mp.memoryTypes[i].propertyFlags,
                       mp.memoryTypes[i].heapIndex ? "/h1" : "");
    printf("memory types:%s\n", line);

    static const struct {
        const char *name;
        VkFormat fmt;
        VkImageUsageFlags usage;
    } imgs[] = {
        {"colour image", VK_FORMAT_R8G8B8A8_UNORM,
         VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT},
        {"storage image", VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT},
        {"depth image", VK_FORMAT_D16_UNORM, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT},
        {"depth-stencil image", VK_FORMAT_D24_UNORM_S8_UINT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT},
        {"depth-stencil image", VK_FORMAT_D32_SFLOAT_S8_UINT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT},
    };
    int ds_done = 0;
    for (size_t i = 0; i < sizeof(imgs) / sizeof(imgs[0]); i++) {
        VkImageFormatProperties ifp;
        if (imgs[i].usage == VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT &&
            (ds_done || vkGetPhysicalDeviceImageFormatProperties(pd, imgs[i].fmt, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, imgs[i].usage, 0,
                                                                  &ifp) != VK_SUCCESS))
            continue;
        if (imgs[i].usage == VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) ds_done = 1;
        VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, imgs[i].fmt, {256, 256, 1}, 1, 1,
                                VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL, imgs[i].usage, VK_SHARING_MODE_EXCLUSIVE, 0, NULL,
                                VK_IMAGE_LAYOUT_UNDEFINED};
        VkImage img;
        char d[160];
        VkResult r = vkCreateImage(dev, &ci, NULL, &img);
        if (r != VK_SUCCESS) {
            snprintf(d, sizeof(d), "vkCreateImage %d", r);
            check(0, imgs[i].name, d);
            continue;
        }
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(dev, img, &req);
        uint32_t t = find_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VkDeviceMemory mem = VK_NULL_HANDLE;
        if (t == UINT32_MAX) {
            snprintf(d, sizeof(d), "no DEVICE_LOCAL type (types 0x%x allowed)", req.memoryTypeBits);
        } else {
            VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size, t};
            r = vkAllocateMemory(dev, &ai, NULL, &mem);
            if (r == VK_SUCCESS) r = vkBindImageMemory(dev, img, mem, 0);
            snprintf(d, sizeof(d), "type %u of 0x%x: %d", t, req.memoryTypeBits, r);
        }
        check(t != UINT32_MAX && r == VK_SUCCESS, imgs[i].name, d);
        if (mem) vkFreeMemory(dev, mem, NULL);
        vkDestroyImage(dev, img, NULL);
    }

    static const struct {
        const char *name;
        VkBufferUsageFlags usage;
    } bufs[] = {
        {"vertex/index buffer", VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT},
        {"indirect buffer", VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT},
        {"uniform buffer", VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT},
        {"storage buffer", VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT},
        {"texel buffer", VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT},
        {"staging buffer", VK_BUFFER_USAGE_TRANSFER_SRC_BIT},
        {"readback buffer", VK_BUFFER_USAGE_TRANSFER_DST_BIT},
    };
    for (size_t i = 0; i < sizeof(bufs) / sizeof(bufs[0]); i++)
        for (int hv = 0; hv < 2; hv++) {
            VkMemoryPropertyFlags want = hv ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                                            : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
            char what[96], d[160];
            snprintf(what, sizeof(what), "%s, %s", bufs[i].name, hv ? "host-visible" : "device-local");
            VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, 65536, bufs[i].usage, VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
            VkBuffer b;
            VkResult r = vkCreateBuffer(dev, &bci, NULL, &b);
            if (r != VK_SUCCESS) {
                snprintf(d, sizeof(d), "vkCreateBuffer %d", r);
                check(0, what, d);
                continue;
            }
            VkMemoryRequirements req;
            vkGetBufferMemoryRequirements(dev, b, &req);
            uint32_t t = find_type(req.memoryTypeBits, want);
            VkDeviceMemory mem = VK_NULL_HANDLE;
            int ok = 0;
            if (t == UINT32_MAX) {
                snprintf(d, sizeof(d), "no type with flags 0x%x (types 0x%x allowed)", want, req.memoryTypeBits);
            } else {
                VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size, t};
                r = vkAllocateMemory(dev, &ai, NULL, &mem);
                if (r == VK_SUCCESS) r = vkBindBufferMemory(dev, b, mem, 0);
                ok = r == VK_SUCCESS;
                if (ok && hv) {
                    volatile uint32_t *p = NULL;
                    r = vkMapMemory(dev, mem, 0, VK_WHOLE_SIZE, 0, (void **)&p);
                    ok = r == VK_SUCCESS && p;
                    if (ok) {
                        for (uint32_t k = 0; k < 16384; k += 61) p[k] = 0x5eed0000u + k;
                        for (uint32_t k = 0; k < 16384 && ok; k += 61) ok = p[k] == 0x5eed0000u + k;
                        vkUnmapMemory(dev, mem);
                    }
                }
                snprintf(d, sizeof(d), "type %u (flags 0x%x) of 0x%x: %d", t, mp.memoryTypes[t].propertyFlags, req.memoryTypeBits, r);
            }
            check(ok, what, d);
            if (mem) vkFreeMemory(dev, mem, NULL);
            vkDestroyBuffer(dev, b, NULL);
        }
    vkDestroyDevice(dev, NULL);
    printf("%s: %d of %d checks passed\n", failures ? "FAILED" : "OK", checks - failures, checks);
    return failures ? 1 : 0;
}
