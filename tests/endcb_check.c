/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * endcb_check: the command buffer of Animal Well through vkd3d-proton, Mali-G2-Ultra r56p0
 * beta: vkd3d-proton's "Query Resets" command buffer (d3d12_command_list_batch_reset_query_pools)
 * is a begin, one vkCmdResetQueryPool per range and an end; the bridge's query mirror
 * (client/query.c) adds its barrier and vkCmdFillBuffer after the reset. The driver's
 * vkEndCommandBuffer of that stream faulted (address 0x18) and took the server with it.
 * Each round records that command buffer (a 160-query timestamp pool, as the game's 1280-byte
 * mirror), and a second one that fills a host-visible buffer, submits both and waits.
 *
 *   endcb_check ok     every end, submit and wait succeeds and the fill is seen, 3 rounds
 *   endcb_check lost   (the server with VKBRIDGE_TEST_CRASH_CMD=vkEndCommandBuffer: the driver's end
 *                      faults) the program sees its device lost (VK_ERROR_DEVICE_LOST from a submit,
 *                      a wait or a later call) and ends: no hang, no lost server
 * One line per step, "OK: ..." / "FAILED: ..." last.
 */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static PFN_vkGetInstanceProcAddr gipa;
static VkInstance inst;
static VkDevice dev;
static int failures, checks;

#define I(name) static PFN_##name name
I(vkCreateInstance); I(vkEnumeratePhysicalDevices); I(vkGetPhysicalDeviceQueueFamilyProperties); I(vkGetPhysicalDeviceMemoryProperties);
I(vkCreateDevice); I(vkGetDeviceProcAddr); I(vkDestroyInstance); I(vkGetDeviceQueue); I(vkCreateCommandPool); I(vkDestroyCommandPool);
I(vkAllocateCommandBuffers); I(vkBeginCommandBuffer); I(vkEndCommandBuffer); I(vkQueueSubmit); I(vkCreateFence); I(vkDestroyFence);
I(vkWaitForFences); I(vkResetFences); I(vkCreateBuffer); I(vkDestroyBuffer); I(vkGetBufferMemoryRequirements); I(vkAllocateMemory);
I(vkFreeMemory); I(vkBindBufferMemory); I(vkMapMemory); I(vkCmdFillBuffer); I(vkCmdPipelineBarrier); I(vkDeviceWaitIdle);
I(vkDestroyDevice); I(vkCreateQueryPool); I(vkDestroyQueryPool); I(vkCmdResetQueryPool); I(vkResetCommandBuffer);
#undef I

static void check(int ok, const char *what, int r)
{
    checks++;
    if (!ok) failures++;
    printf("%s %s (%d)\n", ok ? "PASS" : "FAIL", what, r);
    fflush(stdout);
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "ok";
    int lost_mode = !strcmp(mode, "lost");
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib) {
        printf("FAILED: loader: %s\n", dlerror());
        return 2;
    }
    gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
#define G(n) n = (PFN_##n)gipa(inst, #n)
    G(vkCreateInstance);
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "vkbridge-endcb-check", 1, "vkd3d", 1, VK_API_VERSION_1_1};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 0, NULL};
    if (vkCreateInstance(&ici, NULL, &inst)) {
        printf("FAILED: vkCreateInstance\n");
        return 1;
    }
    G(vkEnumeratePhysicalDevices); G(vkGetPhysicalDeviceQueueFamilyProperties); G(vkGetPhysicalDeviceMemoryProperties);
    G(vkCreateDevice); G(vkGetDeviceProcAddr); G(vkDestroyInstance);
#undef G
    uint32_t n = 1;
    VkPhysicalDevice pd;
    if (vkEnumeratePhysicalDevices(inst, &n, &pd) < 0 || !n) {
        printf("FAILED: no physical device\n");
        return 1;
    }
    uint32_t nq = 0, qfam;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, NULL);
    VkQueueFamilyProperties qp[16];
    if (nq > 16) nq = 16;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qp);
    for (qfam = 0; qfam < nq; qfam++)
        if ((qp[qfam].queueFlags & VK_QUEUE_GRAPHICS_BIT) && qp[qfam].timestampValidBits) break;
    if (qfam == nq) {
        printf("FAILED: no graphics queue with timestamps\n");
        return 1;
    }
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, qfam, 1, &prio};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &qci, 0, NULL, 0, NULL, NULL};
    if (vkCreateDevice(pd, &dci, NULL, &dev)) {
        printf("FAILED: vkCreateDevice\n");
        return 1;
    }
#define G(n) n = (PFN_##n)vkGetDeviceProcAddr(dev, #n)
    G(vkGetDeviceQueue); G(vkCreateCommandPool); G(vkDestroyCommandPool); G(vkAllocateCommandBuffers); G(vkBeginCommandBuffer);
    G(vkEndCommandBuffer); G(vkQueueSubmit); G(vkCreateFence); G(vkDestroyFence); G(vkWaitForFences); G(vkResetFences);
    G(vkCreateBuffer); G(vkDestroyBuffer); G(vkGetBufferMemoryRequirements); G(vkAllocateMemory); G(vkFreeMemory);
    G(vkBindBufferMemory); G(vkMapMemory); G(vkCmdFillBuffer); G(vkCmdPipelineBarrier); G(vkDeviceWaitIdle); G(vkDestroyDevice);
    G(vkCreateQueryPool); G(vkDestroyQueryPool); G(vkCmdResetQueryPool); G(vkResetCommandBuffer);
#undef G
    VkQueue queue;
    vkGetDeviceQueue(dev, qfam, 0, &queue);

    const VkDeviceSize size = 4096;
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
    VkBuffer buf;
    vkCreateBuffer(dev, &bci, NULL, &buf);
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(dev, buf, &mr);
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    uint32_t mt = UINT32_MAX;
    const VkMemoryPropertyFlags hv = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t i = 0; i < mp.memoryTypeCount && mt == UINT32_MAX; i++)
        if ((mr.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & hv) == hv) mt = i;
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, mt};
    VkDeviceMemory mem;
    void *ptr = NULL;
    if (mt == UINT32_MAX || vkAllocateMemory(dev, &mai, NULL, &mem) || vkBindBufferMemory(dev, buf, mem, 0) ||
        vkMapMemory(dev, mem, 0, VK_WHOLE_SIZE, 0, &ptr)) {
        printf("FAILED: setup: host-visible buffer\n");
        return 1;
    }
    memset(ptr, 0, size);

    /* A D3D12 timestamp query heap of 160 (vkd3d-proton makes its Vulkan pool the same size). */
    VkQueryPoolCreateInfo qpi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, NULL, 0, VK_QUERY_TYPE_TIMESTAMP, 160, 0};
    VkQueryPool qpool;
    if (vkCreateQueryPool(dev, &qpi, NULL, &qpool)) {
        printf("FAILED: setup: query pool\n");
        return 1;
    }

    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, qfam};
    VkCommandPool pool;
    vkCreateCommandPool(dev, &pci, NULL, &pool);
    VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 2};
    VkCommandBuffer cbs[2];
    vkAllocateCommandBuffers(dev, &ai, cbs);
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence f;
    vkCreateFence(dev, &fci, NULL, &f);

    int saw_lost = 0;
    for (int round = 0; round < 3 && !saw_lost; round++) {
        VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
        /* vkd3d-proton's query-reset command buffer */
        vkBeginCommandBuffer(cbs[0], &bi);
        vkCmdResetQueryPool(cbs[0], qpool, 0, 160);
        VkResult r_reset = vkEndCommandBuffer(cbs[0]);
        /* the frame's work: a fill the host reads back */
        VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
        vkBeginCommandBuffer(cbs[1], &bi);
        vkCmdFillBuffer(cbs[1], buf, 0, size, 0x0eddcb00u + (uint32_t)round);
        vkCmdPipelineBarrier(cbs[1], VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
        VkResult r_work = vkEndCommandBuffer(cbs[1]);
        if (!lost_mode) {
            check(r_reset == VK_SUCCESS, "query-reset command buffer: vkEndCommandBuffer", r_reset);
            check(r_work == VK_SUCCESS, "work command buffer: vkEndCommandBuffer", r_work);
        }
        VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 2, cbs, 0, NULL};
        VkResult r = vkQueueSubmit(queue, 1, &si, f);
        if (r == VK_SUCCESS) r = vkWaitForFences(dev, 1, &f, VK_TRUE, 5000000000ull);
        if (r == VK_SUCCESS) r = vkResetFences(dev, 1, &f);
        if (r == VK_ERROR_DEVICE_LOST) saw_lost = 1;
        if (lost_mode) {
            if (saw_lost) check(1, "the device is lost after the faulting end (submit/wait/reset answered VK_ERROR_DEVICE_LOST)", r);
            continue;
        }
        check(r == VK_SUCCESS, "submit and wait", r);
        const uint32_t *w = ptr;
        check(w[0] == 0x0eddcb00u + (uint32_t)round && w[size / 4 - 1] == 0x0eddcb00u + (uint32_t)round, "the work ran", round);
    }
    if (lost_mode && !saw_lost) check(0, "the device is lost after the faulting end", 0);
    /* Teardown as a game does after a loss; on a stopped device every call is refused, none hangs. */
    vkDeviceWaitIdle(dev);
    vkDestroyFence(dev, f, NULL);
    vkDestroyCommandPool(dev, pool, NULL);
    vkDestroyQueryPool(dev, qpool, NULL);
    vkDestroyBuffer(dev, buf, NULL);
    vkFreeMemory(dev, mem, NULL);
    vkDestroyDevice(dev, NULL);
    vkDestroyInstance(inst, NULL);
    if (failures) printf("FAILED: %d of %d checks (%s)\n", failures, checks, mode);
    else printf("OK: %d checks (%s)\n", checks, mode);
    return failures ? 1 : 0;
}
