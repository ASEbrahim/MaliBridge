/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * dedimport_check: dma-buf imports with a dedicated-allocation struct (MK-39, dedicated_import).
 * Older Mali drivers fail or crash on an import (dma-buf, AHardwareBuffer, host pointer) that
 * carries a VkMemoryDedicatedAllocateInfo when the dedicated resource was not made with that
 * handle type - which the bridge's shared-memory strategies do for plain resources, and apps do.
 * A buffer's memory is exported as a dma-buf and filled; the dma-buf is then imported, with the
 * dedicated struct, into
 *   1  a buffer made with VkExternalMemoryBufferCreateInfo(DMA_BUF): the struct stays (that works);
 *      the import is copied on the GPU into a host-visible buffer and read back;
 *   2  an optimal-tiled image made without external memory info: the erratum's pattern (the fix
 *      takes the struct off); the import and the bind must succeed.
 * (The bridge's client gives every buffer and linear image its sharing handle type, so through the
 * bridge only optimal images are made without one.)
 * Host imitation (server): VKBRIDGE_ERRATA_IMITATE=dedicated_import fails pattern 2's imports.
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

#define N 4096u
#define DMABUF VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT
#define PATTERN 0x5eed1234u

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
I(vkCmdCopyBuffer); I(vkCmdFillBuffer); I(vkCreateImage); I(vkGetImageMemoryRequirements); I(vkBindImageMemory); I(vkCmdPipelineBarrier); I(vkGetMemoryFdKHR); I(vkGetMemoryFdPropertiesKHR);
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
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "dedimport_check", 1, NULL, 0, VK_API_VERSION_1_1};
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
    D(vkBindBufferMemory); D(vkMapMemory); D(vkCmdCopyBuffer); D(vkCmdFillBuffer); D(vkCreateImage); D(vkGetImageMemoryRequirements); D(vkBindImageMemory); D(vkCmdPipelineBarrier); D(vkGetMemoryFdKHR);
    D(vkGetMemoryFdPropertiesKHR);
#undef D
    vkGetDeviceQueue(dev, qfam, 0, &queue);
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    VkCommandPoolCreateInfo cpci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, 0, qfam};
    vkCreateCommandPool(dev, &cpci, NULL, &cpool);
    char msg[160];
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

    /* the exporter: device-local memory exporting a dma-buf, filled on the GPU */
    VkBuffer src = buffer(VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, 1);
    vkGetBufferMemoryRequirements(dev, src, &mr);
    VkExportMemoryAllocateInfo exp = {VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, NULL, DMABUF};
    ai = (VkMemoryAllocateInfo){VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &exp, mr.size, find_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    VkDeviceMemory smem;
    r = vkAllocateMemory(dev, &ai, NULL, &smem);
    if (!r) r = vkBindBufferMemory(dev, src, smem, 0);
    int fd = -1;
    VkMemoryGetFdInfoKHR gi = {VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR, NULL, smem, DMABUF};
    if (!r) r = vkGetMemoryFdKHR(dev, &gi, &fd);
    snprintf(msg, sizeof(msg), "%d (fd %d)", r, fd);
    check(!r && fd >= 0 && gpu_copy(VK_NULL_HANDLE, src), "exportable memory filled, its dma-buf exported", msg);
    if (r || fd < 0) goto out;
    VkMemoryFdPropertiesKHR fp = {VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
    if (vkGetMemoryFdPropertiesKHR(dev, DMABUF, fd, &fp)) fp.memoryTypeBits = ~0u;

    /* 1: a buffer made for dma-bufs */
    {
        VkBuffer b = buffer(VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 1);
        vkGetBufferMemoryRequirements(dev, b, &mr);
        VkMemoryDedicatedAllocateInfo ded = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, NULL, VK_NULL_HANDLE, b};
        VkImportMemoryFdInfoKHR imp = {VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR, &ded, DMABUF, dup(fd)};
        uint32_t t = find_type(mr.memoryTypeBits & fp.memoryTypeBits, 0);
        ai = (VkMemoryAllocateInfo){VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &imp, mr.size, t};
        VkDeviceMemory m = VK_NULL_HANDLE;
        r = t == UINT32_MAX ? VK_ERROR_FEATURE_NOT_PRESENT : vkAllocateMemory(dev, &ai, NULL, &m);
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
            snprintf(msg, sizeof(msg), "%u words as exported", N);
        else if (bad >= 0)
            snprintf(msg, sizeof(msg), "word %d is 0x%08x", bad, rmap[bad]);
        else
            snprintf(msg, sizeof(msg), "%s %d", bad == -3 ? "copy failed" : "VkResult", r);
        check(bad == -1, "dma-buf imported with a dedicated struct into a buffer made for dma-bufs", msg);
    }
    /* 2: an optimal image made without external memory info */
    {
        VkImageCreateInfo ic = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {32, 32, 1}, 1, 1,
                                VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
        VkImage img = VK_NULL_HANDLE;
        r = vkCreateImage(dev, &ic, NULL, &img);
        if (!r) {
            vkGetImageMemoryRequirements(dev, img, &mr);
            uint32_t t = find_type(mr.memoryTypeBits & fp.memoryTypeBits, 0);
            if (mr.size > N * 4 || t == UINT32_MAX) {
                printf("SKIP the image needs %llu bytes in types 0x%x; the dma-buf has %u in types 0x%x\n", (unsigned long long)mr.size,
                       mr.memoryTypeBits, N * 4, fp.memoryTypeBits);
            } else {
                VkMemoryDedicatedAllocateInfo ded = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, NULL, img, VK_NULL_HANDLE};
                VkImportMemoryFdInfoKHR imp = {VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR, &ded, DMABUF, dup(fd)};
                ai = (VkMemoryAllocateInfo){VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &imp, mr.size, t};
                VkDeviceMemory m = VK_NULL_HANDLE;
                r = vkAllocateMemory(dev, &ai, NULL, &m);
                if (r) close(imp.fd);
                if (!r) r = vkBindImageMemory(dev, img, m, 0);
                snprintf(msg, sizeof(msg), "VkResult %d", r);
                check(!r, "dma-buf imported with a dedicated struct into an optimal image made without external memory info", msg);
            }
        } else {
            snprintf(msg, sizeof(msg), "vkCreateImage %d", r);
            check(0, "setup", msg);
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
