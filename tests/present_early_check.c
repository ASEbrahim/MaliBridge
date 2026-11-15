/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * present_early_check: what VKBRIDGE_PRESENT_EARLY (client/wsi_wayland.c) rests on, on a real dma-buf (udmabuf):
 * a submit signals a sync-fd exportable binary semaphore, the semaphore is exported as a sync file, client/dmabuf_attach.c puts
 * it into the dma-buf as a write fence, and a reader polling the dma-buf (gamescope's way) is not woken before the submit's
 * GPU work is done. The submit waits for a timeline value the test signals later, so "not done yet" is certain.
 *   1  the export returns a sync fd (the semaphore is one the submit signals together with a fence of the app's)
 *   2  it attaches   3  (soft) the buffer is normally not readable right after   4  (hard) readable => the work is done; 3 rounds
 * (Mesa defers a submit that waits on an unsignalled timeline, so its export would block: the work here is simply long.)
 * Exits 77 (skip) without /dev/udmabuf, sync-fd export or the ioctl.
 */
#define _GNU_SOURCE
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/udmabuf.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

int vkb_dmabuf_attach_write_fence(int dmabuf_fd, int sync_fd);

static int fails;
#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        if (c) { printf("ok   "); } else { printf("FAIL "); fails++; }                                                 \
        printf(__VA_ARGS__);                                                                                           \
        printf("\n");                                                                                                  \
    } while (0)
#define SKIP(...) do { printf("SKIP: "); printf(__VA_ARGS__); printf("\n"); return 77; } while (0)

static int make_dmabuf(void)
{
    int dev = open("/dev/udmabuf", O_RDWR | O_CLOEXEC);
    if (dev < 0) return -1;
    int mfd = memfd_create("present_early", MFD_ALLOW_SEALING);
    if (mfd < 0 || ftruncate(mfd, 1 << 20) < 0) { close(dev); return -1; }
    fcntl(mfd, F_ADD_SEALS, F_SEAL_SHRINK);
    struct udmabuf_create c = {mfd, 0, 0, 1 << 20};
    int fd = ioctl(dev, UDMABUF_CREATE, &c);
    close(dev);
    close(mfd);
    return fd;
}

/* 1: readable (a write fence, if any, has signalled) within ms */
static int readable(int fd, int ms)
{
    struct pollfd p = {fd, POLLIN, 0};
    return poll(&p, 1, ms) == 1 && (p.revents & POLLIN);
}

int main(void)
{
    int dbuf = make_dmabuf();
    if (dbuf < 0) SKIP("no /dev/udmabuf access (%s)", strerror(errno));
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib) { printf("FAIL loader: %s\n", dlerror()); return 1; }
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    VkInstance inst = NULL;
#define GI(name) PFN_##name name = (PFN_##name)gipa(inst, #name)
    GI(vkCreateInstance);
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "present_early_check", 1, "none", 1, VK_API_VERSION_1_2};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app};
    if (vkCreateInstance(&ici, NULL, &inst) != VK_SUCCESS) { printf("FAIL instance\n"); return 1; }
    GI(vkEnumeratePhysicalDevices);
    GI(vkEnumerateDeviceExtensionProperties);
    GI(vkGetPhysicalDeviceExternalSemaphoreProperties);
    GI(vkCreateDevice);
    PFN_vkGetDeviceProcAddr gdpa = (PFN_vkGetDeviceProcAddr)gipa(inst, "vkGetDeviceProcAddr");
    uint32_t n = 1;
    VkPhysicalDevice pd = VK_NULL_HANDLE;
    if (vkEnumeratePhysicalDevices(inst, &n, &pd) < 0 || !n || !pd) { printf("FAIL no GPU\n"); return 1; }
    uint32_t en = 0;
    vkEnumerateDeviceExtensionProperties(pd, NULL, &en, NULL);
    VkExtensionProperties *e = calloc(en ? en : 1, sizeof(*e));
    vkEnumerateDeviceExtensionProperties(pd, NULL, &en, e);
    int sfd = 0;
    for (uint32_t i = 0; i < en; i++) sfd |= !strcmp(e[i].extensionName, "VK_KHR_external_semaphore_fd");
    free(e);
    if (!sfd) SKIP("no VK_KHR_external_semaphore_fd");
    VkPhysicalDeviceExternalSemaphoreInfo xi = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO, NULL,
                                                VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT};
    VkExternalSemaphoreProperties xp = {VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES};
    vkGetPhysicalDeviceExternalSemaphoreProperties(pd, &xi, &xp);
    if (!(xp.externalSemaphoreFeatures & VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT)) SKIP("SYNC_FD semaphores not exportable");

    float prio = 1;
    VkDeviceQueueCreateInfo q = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, 0, 1, &prio};
    const char *exts[] = {"VK_KHR_external_semaphore_fd"};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &q, 0, NULL, 1, exts};
    VkDevice dev;
    if (vkCreateDevice(pd, &dci, NULL, &dev) != VK_SUCCESS) { printf("FAIL device\n"); return 1; }
#define GD(name) PFN_##name name = (PFN_##name)gdpa(dev, #name)
    GD(vkGetDeviceQueue); GD(vkQueueSubmit); GD(vkCreateSemaphore); GD(vkGetSemaphoreFdKHR); GD(vkDeviceWaitIdle);
    GD(vkDestroySemaphore); GD(vkCreateFence); GD(vkGetFenceStatus); GD(vkResetFences); GD(vkCreateCommandPool);
    GD(vkAllocateCommandBuffers); GD(vkBeginCommandBuffer); GD(vkEndCommandBuffer); GD(vkCmdFillBuffer); GD(vkCreateBuffer);
    GD(vkGetBufferMemoryRequirements); GD(vkAllocateMemory); GD(vkBindBufferMemory);
    GI(vkGetPhysicalDeviceMemoryProperties);
    if (!vkGetSemaphoreFdKHR) { printf("FAIL vkGetSemaphoreFdKHR missing\n"); return 1; }
    VkQueue queue;
    vkGetDeviceQueue(dev, 0, 0, &queue);

    /* the "frame's rendering": a lot of fills of a big buffer, in one command buffer */
    VkBuffer big;
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, 64u << 20, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE};
    vkCreateBuffer(dev, &bi, NULL, &big);
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(dev, big, &mr);
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    uint32_t t = 0;
    while (t < mp.memoryTypeCount && !((mr.memoryTypeBits >> t) & 1)) t++;
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, t};
    VkDeviceMemory bm;
    vkAllocateMemory(dev, &mai, NULL, &bm);
    vkBindBufferMemory(dev, big, bm, 0);
    VkCommandPool pool;
    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, 0, 0};
    vkCreateCommandPool(dev, &pci, NULL, &pool);
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VkCommandBuffer cb;
    vkAllocateCommandBuffers(dev, &cai, &cb);
    VkCommandBufferBeginInfo cbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vkBeginCommandBuffer(cb, &cbi);
    for (int i = 0; i < 2000; i++) vkCmdFillBuffer(cb, big, 0, 64u << 20, (uint32_t)i);
    vkEndCommandBuffer(cb);

    VkSemaphore bin;
    VkExportSemaphoreCreateInfo ex = {VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO, NULL, VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT};
    VkSemaphoreCreateInfo sci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &ex, 0};
    VkFence fence;
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (vkCreateSemaphore(dev, &sci, NULL, &bin) || vkCreateFence(dev, &fci, NULL, &fence)) { printf("FAIL semaphore/fence\n"); return 1; }

    CHECK(readable(dbuf, 0), "0 a fresh dma-buf is readable");
    for (int round = 1; round <= 3; round++) {
        /* the present submit: signals the exportable semaphore and the bridge's own fence (the semaphore is the one exported) */
        VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cb, 1, &bin};
        VkResult r = vkQueueSubmit(queue, 1, &si, fence);
        int fd = -1;
        VkSemaphoreGetFdInfoKHR gi = {VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR, NULL, bin, VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT};
        VkResult g = vkGetSemaphoreFdKHR(dev, &gi, &fd);
        CHECK(r == VK_SUCCESS && g == VK_SUCCESS && fd >= 0, "%d.1 submit %d, sync fd export %d, fd %d", round, r, g, fd);
        if (fd < 0) return 1;
        int err = vkb_dmabuf_attach_write_fence(dbuf, fd);
        close(fd);
        if (err == ENOTTY || err == EPERM || err == EACCES) SKIP("the kernel/host does not allow IMPORT_SYNC_FILE (%s)", strerror(err));
        CHECK(!err, "%d.2 fence attached to the dma-buf (%s)", round, strerror(err));
        /* soft: the work is long, so the buffer is normally not readable yet (only reported) */
        int early = readable(dbuf, 0);
        int done_at_poll = vkGetFenceStatus(dev, fence) == VK_SUCCESS;
        printf("     %d.3 right after the attach: dma-buf %s, fence %s%s\n", round, early ? "readable" : "not readable",
               done_at_poll ? "signalled" : "pending", (early && !done_at_poll) ? "  <-- readable before the work was done?" : "");
        /* hard: once the dma-buf is readable, the rendering is done */
        int rd = readable(dbuf, 20000);
        /* the app's fence and the semaphore are two sync objects of one batch: the fence may trail by a few ms, not by the work */
        VkResult fs = vkGetFenceStatus(dev, fence);
        for (int k = 0; k < 50 && fs != VK_SUCCESS; k++) { usleep(1000); fs = vkGetFenceStatus(dev, fence); }
        CHECK(rd && fs == VK_SUCCESS, "%d.4 dma-buf readable (%d) with the work done (fence within 50 ms): status %d", round, rd, fs);
        vkResetFences(dev, 1, &fence);
    }
    vkDeviceWaitIdle(dev);
    vkDestroySemaphore(dev, bin, NULL);
    printf(fails ? "FAILED: %d failure(s)\n" : "OK: 0 failure(s)\n", fails);
    return fails ? 1 : 0;
}
