/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * dmabufsync_check: Zink's hand-over of a frame to another process, the way its flush does it: a
 * batch that draws into an exported dma-buf and signals a semaphore, then, on the same thread, a
 * sync-file export of that semaphore (vkGetSemaphoreFdKHR) and a dma-buf export of the memory
 * (vkGetMemoryFdKHR), which Zink uses to attach the sync file to the dma-buf. On a kernel without
 * dma-buf sync files (Linux before 6.0) that attach does nothing and the reader takes the buffer
 * while the GPU still draws it (client/dmabuf_sync.c). The batch here is long GPU work (fills of a
 * large buffer, sized to take a few hundred ms).
 *
 *   dmabufsync_check wait|nowait|timeout
 *     wait:    (VKBRIDGE_DMABUF_SYNC_FAKE_OLD=1) the dma-buf export returns only once the batch is
 *              done (its fence signalled when the call returns); the app's sync-file export is fd -1
 *              (a 5.x kernel's attach finds nothing); a sync-file export with a submit after it, and
 *              a dma-buf export with no sync-file export before it, do not wait;
 *     nowait:  (off, or a kernel with the ioctls) the dma-buf export returns while the batch still
 *              runs, the sync-file export is a real fd;
 *     timeout: (VKBRIDGE_DMABUF_SYNC_MS=50, FAKE_OLD) the export returns after about 50 ms, the batch
 *              still running.
 * One line per step, "OK: ..." / "FAILED: ..." / "SKIP: ..." last.
 */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static PFN_vkGetInstanceProcAddr gipa;
static VkInstance inst;
static VkDevice dev;
static VkQueue queue;
static int failures, checks;

#define I(name) static PFN_##name name
I(vkCreateInstance); I(vkEnumeratePhysicalDevices); I(vkGetPhysicalDeviceProperties); I(vkGetPhysicalDeviceQueueFamilyProperties);
I(vkGetPhysicalDeviceExternalSemaphoreProperties); I(vkGetPhysicalDeviceMemoryProperties); I(vkCreateDevice); I(vkGetDeviceProcAddr);
I(vkGetDeviceQueue); I(vkQueueSubmit); I(vkCreateSemaphore); I(vkDestroySemaphore); I(vkDeviceWaitIdle); I(vkDestroyDevice);
I(vkGetSemaphoreFdKHR); I(vkGetMemoryFdKHR); I(vkCreateBuffer); I(vkDestroyBuffer); I(vkGetBufferMemoryRequirements);
I(vkAllocateMemory); I(vkFreeMemory); I(vkBindBufferMemory); I(vkCreateCommandPool); I(vkDestroyCommandPool);
I(vkAllocateCommandBuffers); I(vkBeginCommandBuffer); I(vkEndCommandBuffer); I(vkCmdFillBuffer); I(vkCreateFence);
I(vkDestroyFence); I(vkWaitForFences); I(vkResetFences); I(vkGetFenceStatus);
#undef I

static VkCommandBuffer cmd;
static VkFence fence;
static VkBuffer big, shared;
static VkDeviceMemory big_mem, shared_mem;

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static void check(int ok, const char *what)
{
    checks++;
    if (!ok) failures++;
    printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
}

static VkSemaphore exportable(void)
{
    VkExportSemaphoreCreateInfo ex = {VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO, NULL, VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT};
    VkSemaphoreCreateInfo ci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &ex, 0};
    VkSemaphore s = VK_NULL_HANDLE;
    vkCreateSemaphore(dev, &ci, NULL, &s);
    return s;
}

static int memtype(uint32_t bits, VkMemoryPropertyFlags want, VkPhysicalDevice pd)
{
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return (int)i;
    return -1;
}

/* Records `fills` fills of the big buffer, then one of the shared one (the "frame"). */
static void record(uint32_t fills)
{
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(cmd, &bi);
    for (uint32_t i = 0; i < fills; i++) vkCmdFillBuffer(cmd, big, 0, VK_WHOLE_SIZE, i);
    vkCmdFillBuffer(cmd, shared, 0, VK_WHOLE_SIZE, 0x600d);
    vkEndCommandBuffer(cmd);
}

static VkResult submit(VkSemaphore signal, int with_cmd)
{
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, with_cmd ? 1 : 0, &cmd, signal ? 1 : 0, &signal};
    return vkQueueSubmit(queue, 1, &si, with_cmd ? fence : VK_NULL_HANDLE);
}

static int sem_fd(VkSemaphore s)
{
    VkSemaphoreGetFdInfoKHR gi = {VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR, NULL, s, VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT};
    int fd = -2;
    VkResult r = vkGetSemaphoreFdKHR(dev, &gi, &fd);
    return r == VK_SUCCESS ? fd : -3;
}

static int mem_fd(void)
{
    VkMemoryGetFdInfoKHR gi = {VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR, NULL, shared_mem, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    int fd = -2;
    VkResult r = vkGetMemoryFdKHR(dev, &gi, &fd);
    return r == VK_SUCCESS ? fd : -3;
}

static void settle(void)
{
    vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX);
    vkResetFences(dev, 1, &fence);
}

int main(int argc, char **argv)
{
    const char *expect = argc > 1 ? argv[1] : "wait";
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib) {
        printf("FAILED: loader: %s\n", dlerror());
        return 2;
    }
    gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
#define G(n) n = (PFN_##n)gipa(inst, #n)
    G(vkCreateInstance);
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "vkbridge-dmabufsync-check", 1, "vkbridge", 1, VK_API_VERSION_1_1};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 0, NULL};
    if (vkCreateInstance(&ici, NULL, &inst)) {
        printf("FAILED: vkCreateInstance\n");
        return 1;
    }
    G(vkEnumeratePhysicalDevices); G(vkGetPhysicalDeviceProperties); G(vkGetPhysicalDeviceQueueFamilyProperties);
    G(vkGetPhysicalDeviceExternalSemaphoreProperties); G(vkGetPhysicalDeviceMemoryProperties); G(vkCreateDevice);
    G(vkGetDeviceProcAddr);
#undef G
    uint32_t n = 1;
    VkPhysicalDevice pd;
    if (vkEnumeratePhysicalDevices(inst, &n, &pd) < 0 || !n) {
        printf("FAILED: no physical device\n");
        return 1;
    }
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(pd, &props);
    VkPhysicalDeviceExternalSemaphoreInfo ei = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO, NULL,
                                                VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT};
    VkExternalSemaphoreProperties ep = {VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES};
    vkGetPhysicalDeviceExternalSemaphoreProperties(pd, &ei, &ep);
    if (!(ep.externalSemaphoreFeatures & VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT)) {
        printf("SKIP: %s exports no sync fds\n", props.deviceName);
        return 0;
    }
    uint32_t nq = 0, qfam;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, NULL);
    VkQueueFamilyProperties qp[16];
    if (nq > 16) nq = 16;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qp);
    for (qfam = 0; qfam < nq; qfam++)
        if (qp[qfam].queueFlags & VK_QUEUE_GRAPHICS_BIT) break;
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, qfam, 1, &prio};
    const char *exts[] = {"VK_KHR_external_semaphore_fd", "VK_KHR_external_memory_fd", "VK_EXT_external_memory_dma_buf"};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &qci, 0, NULL, 3, exts, NULL};
    if (qfam == nq || vkCreateDevice(pd, &dci, NULL, &dev)) {
        printf("SKIP: no device with dma-buf export\n");
        return 0;
    }
#define G(n) n = (PFN_##n)vkGetDeviceProcAddr(dev, #n)
    G(vkGetDeviceQueue); G(vkQueueSubmit); G(vkCreateSemaphore); G(vkDestroySemaphore); G(vkDeviceWaitIdle); G(vkDestroyDevice);
    G(vkGetSemaphoreFdKHR); G(vkGetMemoryFdKHR); G(vkCreateBuffer); G(vkDestroyBuffer); G(vkGetBufferMemoryRequirements);
    G(vkAllocateMemory); G(vkFreeMemory); G(vkBindBufferMemory); G(vkCreateCommandPool); G(vkDestroyCommandPool);
    G(vkAllocateCommandBuffers); G(vkBeginCommandBuffer); G(vkEndCommandBuffer); G(vkCmdFillBuffer); G(vkCreateFence);
    G(vkDestroyFence); G(vkWaitForFences); G(vkResetFences); G(vkGetFenceStatus);
#undef G
    vkGetDeviceQueue(dev, qfam, 0, &queue);
    printf("device: %s (expecting %s)\n", props.deviceName, expect);

    /* the big buffer: device-local; the shared one: exported as a dma-buf */
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, 256u << 20, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                              VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
    VkMemoryRequirements mr;
    if (vkCreateBuffer(dev, &bci, NULL, &big)) { printf("FAILED: big buffer\n"); return 1; }
    vkGetBufferMemoryRequirements(dev, big, &mr);
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size,
                                (uint32_t)memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, pd)};
    if (vkAllocateMemory(dev, &mai, NULL, &big_mem) || vkBindBufferMemory(dev, big, big_mem, 0)) { printf("FAILED: big memory\n"); return 1; }
    VkExternalMemoryBufferCreateInfo ebi = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO, NULL,
                                            VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    VkBufferCreateInfo sci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &ebi, 0, 1u << 20, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                              VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
    if (vkCreateBuffer(dev, &sci, NULL, &shared)) { printf("SKIP: no dma-buf buffer\n"); return 0; }
    vkGetBufferMemoryRequirements(dev, shared, &mr);
    VkExportMemoryAllocateInfo emi = {VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, NULL, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    VkMemoryAllocateInfo smai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &emi, mr.size, (uint32_t)memtype(mr.memoryTypeBits, 0, pd)};
    if (vkAllocateMemory(dev, &smai, NULL, &shared_mem) || vkBindBufferMemory(dev, shared, shared_mem, 0)) {
        printf("SKIP: no exportable dma-buf memory\n");
        return 0;
    }
    int probe = mem_fd();
    if (probe < 0) { printf("SKIP: vkGetMemoryFdKHR gives no dma-buf (%d)\n", probe); return 0; }
    close(probe);
    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, qfam};
    VkCommandPool pool;
    vkCreateCommandPool(dev, &pci, NULL, &pool);
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    vkAllocateCommandBuffers(dev, &cai, &cmd);
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, NULL, 0};
    vkCreateFence(dev, &fci, NULL, &fence);

    /* Size the batch: at least 300 ms of GPU work. */
    uint32_t fills = 4;
    double batch = 0;
    for (int i = 0; i < 12; i++) {
        record(fills);
        double t0 = now_ms();
        submit(VK_NULL_HANDLE, 1);
        settle();
        batch = now_ms() - t0;
        if (batch >= 300) break;
        uint32_t next = batch > 1 ? (uint32_t)(fills * 400.0 / batch) + 1 : fills * 8;
        fills = next > 20000 ? 20000 : next;
    }
    printf("  the batch: %u fills of 256 MB, %.0f ms\n", fills, batch);
    if (batch < 150) {
        printf("SKIP: the GPU work is too short to tell (%.0f ms)\n", batch);
        return 0;
    }
    record(fills);

    /* 1: Zink's hand-over: submit, sync-file export, dma-buf export */
    VkSemaphore s = exportable();
    double t0 = now_ms();
    submit(s, 1);
    int sfd = sem_fd(s);
    VkResult before = vkGetFenceStatus(dev, fence);
    int mfd = mem_fd();
    double took = now_ms() - t0;
    VkResult fs = vkGetFenceStatus(dev, fence);
    /* The fence is signalled after the submit's last batch, a moment after the frame's semaphore (the
     * sync mirror's slot comes in a batch of its own after it): done within 50 ms of the return, the
     * batch itself took 300 ms or more, so a return that did not wait for it still fails. */
    if (!strcmp(expect, "wait") && fs == VK_NOT_READY) fs = vkWaitForFences(dev, 1, &fence, VK_TRUE, 50000000ull);
    char msg[300];
    if (!strcmp(expect, "wait")) {
        snprintf(msg, sizeof(msg), "hand-over: the batch running when the dma-buf export was called (fence %d), done when it "
                 "returned after %.0f ms (fence %d); the app's sync file fd %d (-1: nothing for its attach)", before, took, fs, sfd);
        check(before == VK_NOT_READY && fs == VK_SUCCESS && mfd >= 0 && sfd == -1, msg);
    } else if (!strcmp(expect, "timeout")) {
        snprintf(msg, sizeof(msg), "hand-over: the dma-buf export gave up after %.0f ms (bound 50 ms), the batch still running (fence %d)",
                 took, fs);
        check(before == VK_NOT_READY && fs == VK_NOT_READY && mfd >= 0 && took >= 40 && took < 250, msg);
    } else {
        snprintf(msg, sizeof(msg), "hand-over: the dma-buf export returned after %.0f ms, the batch still running (fence %d), sync file fd %d",
                 took, fs, sfd);
        check(fs == VK_NOT_READY && mfd >= 0 && sfd >= 0 && took < 100, msg);
    }
    if (sfd >= 0) close(sfd);
    if (mfd >= 0) close(mfd);
    settle();
    vkDestroySemaphore(dev, s, NULL);

    if (!strcmp(expect, "wait")) {
        /* 2: a sync-file export the app keeps for itself: a submit comes next, then a dma-buf export */
        s = exportable();
        submit(s, 1);
        sfd = sem_fd(s);
        submit(VK_NULL_HANDLE, 0);
        t0 = now_ms();
        mfd = mem_fd();
        took = now_ms() - t0;
        fs = vkGetFenceStatus(dev, fence);
        snprintf(msg, sizeof(msg), "a sync-file export with a submit after it: the dma-buf export does not wait (%.0f ms, fence %d)", took, fs);
        check(fs == VK_NOT_READY && took < 100, msg);
        if (sfd >= 0) close(sfd);
        if (mfd >= 0) close(mfd);
        settle();
        vkDestroySemaphore(dev, s, NULL);

        /* 3: a dma-buf export alone */
        submit(VK_NULL_HANDLE, 1);
        t0 = now_ms();
        mfd = mem_fd();
        took = now_ms() - t0;
        fs = vkGetFenceStatus(dev, fence);
        snprintf(msg, sizeof(msg), "a dma-buf export with no sync-file export before it does not wait (%.0f ms, fence %d)", took, fs);
        check(fs == VK_NOT_READY && took < 100, msg);
        if (mfd >= 0) close(mfd);
        settle();
    }

    vkDestroyFence(dev, fence, NULL);
    vkDestroyCommandPool(dev, pool, NULL);
    vkDestroyBuffer(dev, shared, NULL);
    vkDestroyBuffer(dev, big, NULL);
    vkFreeMemory(dev, shared_mem, NULL);
    vkFreeMemory(dev, big_mem, NULL);
    vkDestroyDevice(dev, NULL);
    if (failures) printf("FAILED: %d of %d\n", failures, checks);
    else printf("OK: %d checks\n", checks);
    return failures ? 1 : 0;
}
