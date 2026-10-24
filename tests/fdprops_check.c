/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * fdprops_check: a dma-buf imported the way gamescope's DMA-BUF patch for other GPUs does it
 * (Droid-Deck's 0122: the memory type from vkGetMemoryFdPropertiesKHR's memoryTypeBits AND the
 * resource's VkMemoryRequirements.memoryTypeBits, nothing else tried). Through the bridge on Mali
 * every such import failed: the driver says a dma-buf goes only in its host-visible cached type,
 * which the client offers only as a shared type, so the fd's client bits came out 0 ("No
 * compatible image memory type (image=0x1, fd=0x0)") and Steam never drew.
 * A buffer's memory is exported as a dma-buf and filled on the GPU; then, for
 *   1  an optimal-tiled image made without external memory info (with the dedicated struct),
 *   2  a buffer made with VkExternalMemoryBufferCreateInfo(DMA_BUF) (with the dedicated struct),
 * the fd's memoryTypeBits AND the resource's must be nonzero, and the import in a type from that
 * AND and the bind must succeed; the buffer's import is copied on the GPU into a host-visible
 * buffer and must read back as exported.
 * Needs VK_KHR_external_memory_fd and VK_EXT_external_memory_dma_buf (SKIP otherwise).
 */
#define _GNU_SOURCE
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define N 65536u
#define DMABUF VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT
#define PATTERN 0x6a5e0122u

static PFN_vkGetInstanceProcAddr gipa;
static VkInstance inst;
static VkPhysicalDevice pd;
static VkDevice dev;
static VkQueue queue;
static int failures, checks;

#define I(name) static PFN_##name name
I(vkCreateInstance); I(vkEnumeratePhysicalDevices); I(vkGetPhysicalDeviceQueueFamilyProperties); I(vkEnumerateDeviceExtensionProperties);
I(vkGetPhysicalDeviceMemoryProperties); I(vkCreateDevice); I(vkGetDeviceProcAddr); I(vkGetDeviceQueue); I(vkCreateCommandPool);
I(vkAllocateCommandBuffers); I(vkBeginCommandBuffer); I(vkEndCommandBuffer); I(vkQueueSubmit); I(vkQueueWaitIdle);
I(vkCreateBuffer); I(vkGetBufferMemoryRequirements); I(vkAllocateMemory); I(vkBindBufferMemory); I(vkMapMemory);
I(vkCmdCopyBuffer); I(vkCmdFillBuffer); I(vkCreateImage); I(vkGetImageMemoryRequirements); I(vkBindImageMemory); I(vkCmdPipelineBarrier);
I(vkGetMemoryFdKHR); I(vkGetMemoryFdPropertiesKHR);
#undef I

static void check(int ok, const char *what, const char *detail)
{
    checks++;
    if (!ok) failures++;
    printf("%s %s%s%s\n", ok ? "PASS" : "FAIL", what, detail && *detail ? ": " : "", detail ? detail : "");
    fflush(stdout);
}

static VkPhysicalDeviceMemoryProperties mp;

static uint32_t find_type(uint32_t bits, VkMemoryPropertyFlags want)
{
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

/* gamescope's DmabufMemoryType: a type in both masks with the preferred flags, else any in both */
static uint32_t dmabuf_type(uint32_t res_bits, uint32_t fd_bits)
{
    uint32_t t = find_type(res_bits & fd_bits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    return t != UINT32_MAX ? t : find_type(res_bits & fd_bits, 0);
}

static VkBuffer buffer(VkBufferUsageFlags usage, int external)
{
    VkExternalMemoryBufferCreateInfo x = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO, NULL, DMABUF};
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, external ? &x : NULL, 0, N * 4, usage, VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
    VkBuffer b = VK_NULL_HANDLE;
    vkCreateBuffer(dev, &bci, NULL, &b);
    return b;
}

static VkCommandPool cpool;

/* Records src -> dst (or a fill of dst when src is null), submits and waits */
static int gpu_copy(VkBuffer src, VkBuffer dst)
{
    VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, cpool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VkCommandBuffer cb;
    if (vkAllocateCommandBuffers(dev, &ai, &cb)) return 0;
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(cb, &bi);
    if (src) {
        VkBufferCopy bc = {0, 0, N * 4};
        vkCmdCopyBuffer(cb, src, dst, 1, &bc);
    } else
        vkCmdFillBuffer(cb, dst, 0, N * 4, PATTERN);
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    vkEndCommandBuffer(cb);
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cb, 0, NULL};
    return !vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE) && !vkQueueWaitIdle(queue);
}

int main(void)
{
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib) { printf("FAIL no libvulkan\n"); return 1; }
    gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    vkCreateInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "fdprops_check", 1, NULL, 0, VK_API_VERSION_1_1};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 0, NULL};
    if (vkCreateInstance(&ici, NULL, &inst)) { printf("FAIL vkCreateInstance\n"); return 1; }
#define L(name) name = (PFN_##name)gipa(inst, #name)
    L(vkEnumeratePhysicalDevices); L(vkGetPhysicalDeviceQueueFamilyProperties); L(vkEnumerateDeviceExtensionProperties);
    L(vkGetPhysicalDeviceMemoryProperties); L(vkCreateDevice); L(vkGetDeviceProcAddr);
#undef L
    uint32_t n = 1;
    if (vkEnumeratePhysicalDevices(inst, &n, &pd) < 0 || !n) { printf("FAIL no GPU\n"); return 1; }
    const char *want[] = {"VK_KHR_external_memory_fd", "VK_EXT_external_memory_dma_buf"};
    uint32_t ne = 0;
    vkEnumerateDeviceExtensionProperties(pd, NULL, &ne, NULL);
    VkExtensionProperties *ext = calloc(ne ? ne : 1, sizeof(*ext));
    vkEnumerateDeviceExtensionProperties(pd, NULL, &ne, ext);
    int have = 0;
    for (uint32_t i = 0; i < ne; i++)
        for (int k = 0; k < 2; k++) have += !strcmp(ext[i].extensionName, want[k]);
    if (have < 2) { printf("SKIP no dma-buf export/import\nOK: 0 of 0 checks passed\n"); return 0; }
    uint32_t nq = 8;
    VkQueueFamilyProperties qf[8];
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qf);
    uint32_t qfam = 0;
    while (qfam < nq && !(qf[qfam].queueFlags & VK_QUEUE_GRAPHICS_BIT)) qfam++;
    float prio = 1;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, qfam, 1, &prio};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &qci, 0, NULL, 2, want, NULL};
    if (vkCreateDevice(pd, &dci, NULL, &dev)) { printf("FAIL vkCreateDevice\n"); return 1; }
#define D(name) name = (PFN_##name)vkGetDeviceProcAddr(dev, #name)
    D(vkGetDeviceQueue); D(vkCreateCommandPool); D(vkAllocateCommandBuffers); D(vkBeginCommandBuffer); D(vkEndCommandBuffer);
    D(vkQueueSubmit); D(vkQueueWaitIdle); D(vkCreateBuffer); D(vkGetBufferMemoryRequirements); D(vkAllocateMemory);
    D(vkBindBufferMemory); D(vkMapMemory); D(vkCmdCopyBuffer); D(vkCmdFillBuffer); D(vkCreateImage); D(vkGetImageMemoryRequirements);
    D(vkBindImageMemory); D(vkCmdPipelineBarrier); D(vkGetMemoryFdKHR); D(vkGetMemoryFdPropertiesKHR);
#undef D
    vkGetDeviceQueue(dev, qfam, 0, &queue);
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    VkCommandPoolCreateInfo cpci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, 0, qfam};
    vkCreateCommandPool(dev, &cpci, NULL, &cpool);
    char msg[200];
    VkResult r;

    /* the readback buffer */
    VkBuffer rbuf = buffer(VK_BUFFER_USAGE_TRANSFER_DST_BIT, 0);
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(dev, rbuf, &mr);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size,
                               find_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    VkDeviceMemory rmem;
    uint32_t *rmap = NULL;
    if (ai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(dev, &ai, NULL, &rmem) || vkBindBufferMemory(dev, rbuf, rmem, 0) ||
        vkMapMemory(dev, rmem, 0, VK_WHOLE_SIZE, 0, (void **)&rmap)) {
        printf("FAIL setup: readback buffer\n");
        return 1;
    }

    /* the exporter: device-local memory exporting a dma-buf, filled on the GPU - in the first
     * device-local type the buffer allows that takes an export (on the XRing O3's imitated table
     * the first is backed by type 8, which no dma-buf goes in; on the phone the server's export
     * emulation moves the memory into one that takes it, which the imitation beneath the
     * desktop's own export cannot) */
    VkBuffer src = buffer(VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, 1);
    vkGetBufferMemoryRequirements(dev, src, &mr);
    VkExportMemoryAllocateInfo exp = {VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, NULL, DMABUF};
    VkDeviceSize fd_size = mr.size;
    VkDeviceMemory smem = VK_NULL_HANDLE;
    r = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    for (uint32_t left = mr.memoryTypeBits, t; r && (t = find_type(left, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) != UINT32_MAX;
         left &= ~(1u << t)) {
        ai = (VkMemoryAllocateInfo){VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &exp, mr.size, t};
        r = vkAllocateMemory(dev, &ai, NULL, &smem);
    }
    if (!r) r = vkBindBufferMemory(dev, src, smem, 0);
    int fd = -1;
    VkMemoryGetFdInfoKHR gi = {VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR, NULL, smem, DMABUF};
    if (!r) r = vkGetMemoryFdKHR(dev, &gi, &fd);
    snprintf(msg, sizeof(msg), "%d (fd %d)", r, fd);
    check(!r && fd >= 0 && gpu_copy(VK_NULL_HANDLE, src), "exportable memory filled, its dma-buf exported", msg);
    if (r || fd < 0) goto out;
    VkMemoryFdPropertiesKHR fp = {VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
    r = vkGetMemoryFdPropertiesKHR(dev, DMABUF, fd, &fp);
    snprintf(msg, sizeof(msg), "%d, memory types 0x%x", r, fp.memoryTypeBits);
    check(!r && fp.memoryTypeBits, "vkGetMemoryFdPropertiesKHR on the dma-buf", msg);
    printf("memory types:");
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) printf(" %u:0x%x", i, mp.memoryTypes[i].propertyFlags);
    printf("\n");

    /* 1: an optimal image made without external memory info (gamescope's DMA-BUF images are this
     * resource on the client's side: their requirements list only the device-local type) */
    {
        VkImageCreateInfo ic = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {64, 64, 1}, 1, 1,
                                VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
        VkImage img = VK_NULL_HANDLE;
        r = vkCreateImage(dev, &ic, NULL, &img);
        if (r) {
            snprintf(msg, sizeof(msg), "vkCreateImage %d", r);
            check(0, "setup: the image", msg);
        } else {
            vkGetImageMemoryRequirements(dev, img, &mr);
            uint32_t t = dmabuf_type(mr.memoryTypeBits, fp.memoryTypeBits);
            snprintf(msg, sizeof(msg), "image 0x%x, fd 0x%x", mr.memoryTypeBits, fp.memoryTypeBits);
            check(t != UINT32_MAX, "a memory type both the image and the dma-buf allow", msg);
            if (mr.size > fd_size) {
                printf("SKIP the image needs %llu bytes, the dma-buf has %llu\n", (unsigned long long)mr.size, (unsigned long long)fd_size);
            } else if (t != UINT32_MAX) {
                VkMemoryDedicatedAllocateInfo ded = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, NULL, img, VK_NULL_HANDLE};
                VkImportMemoryFdInfoKHR imp = {VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR, &ded, DMABUF, dup(fd)};
                ai = (VkMemoryAllocateInfo){VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &imp, mr.size, t};
                VkDeviceMemory m = VK_NULL_HANDLE;
                r = vkAllocateMemory(dev, &ai, NULL, &m);
                if (r) close(imp.fd);
                if (!r) r = vkBindImageMemory(dev, img, m, 0);
                snprintf(msg, sizeof(msg), "type %u (flags 0x%x): VkResult %d", t, mp.memoryTypes[t].propertyFlags, r);
                check(!r, "dma-buf imported into the image in that type, bound", msg);
            }
        }
    }
    /* 2: a buffer made for dma-bufs; the import read back */
    {
        VkBuffer b = buffer(VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 1);
        vkGetBufferMemoryRequirements(dev, b, &mr);
        uint32_t t = dmabuf_type(mr.memoryTypeBits, fp.memoryTypeBits);
        snprintf(msg, sizeof(msg), "buffer 0x%x, fd 0x%x", mr.memoryTypeBits, fp.memoryTypeBits);
        check(t != UINT32_MAX, "a memory type both the buffer and the dma-buf allow", msg);
        if (t != UINT32_MAX) {
            VkMemoryDedicatedAllocateInfo ded = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, NULL, VK_NULL_HANDLE, b};
            VkImportMemoryFdInfoKHR imp = {VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR, &ded, DMABUF, dup(fd)};
            ai = (VkMemoryAllocateInfo){VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &imp, mr.size, t};
            VkDeviceMemory m = VK_NULL_HANDLE;
            r = vkAllocateMemory(dev, &ai, NULL, &m);
            if (r) close(imp.fd);
            if (!r) r = vkBindBufferMemory(dev, b, m, 0);
            int bad = -2;
            if (!r) {
                memset(rmap, 0, N * 4);
                bad = gpu_copy(b, rbuf) ? -1 : -3;
                for (uint32_t i = 0; i < N && bad == -1; i++)
                    if (rmap[i] != PATTERN) bad = (int)i;
            }
            if (bad == -1)
                snprintf(msg, sizeof(msg), "type %u (flags 0x%x): %u words as exported", t, mp.memoryTypes[t].propertyFlags, N);
            else if (bad >= 0)
                snprintf(msg, sizeof(msg), "type %u: word %d is 0x%08x", t, bad, rmap[bad]);
            else
                snprintf(msg, sizeof(msg), "type %u: %s %d", t, bad == -3 ? "copy failed" : "VkResult", r);
            check(bad == -1, "dma-buf imported into the buffer in that type, read back", msg);
        }
    }
    close(fd);
out:
    if (failures)
        printf("FAIL: %d of %d checks failed\n", failures, checks);
    else
        printf("OK: %d of %d checks passed\n", checks, checks);
    return failures ? 1 : 0;
}
