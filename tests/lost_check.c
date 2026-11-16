/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 * lost_check: a program whose device is lost goes on as a game does after it (a player log: WoW Classic
 * through vkd3d-proton submitted four more times after its vkWaitForFences returned
 * VK_ERROR_DEVICE_LOST). Submits with a fence and waits, until a wait says the device is lost
 * (the server's VKBRIDGE_TEST_LOST imitation loses it), then submits <after> more times, waits
 * on a fence, waits for the device, and tears down. The host suite runs it against servers with
 * and without the lost-device latch (host/lost_test.sh), and checks the server lives on.
 * Usage: lost_check [rounds] [after] [pause_ms] [transient]: pause_ms (300) between the loss and the
 * submissions after it (a player log's game took 51 ms), and again before the teardown. Prints what it saw; "OK: ..." when the device was lost and
 * every call after it answered so, "FAILED: ..." otherwise. "transient" (a lost answer the driver takes
 * back, VKBRIDGE_TEST_LOST=<n>:blip): after the lost answer, <after> more rounds of submit + fence wait
 * must all answer VK_SUCCESS (the loss was not made permanent). "again" (VKBRIDGE_TEST_LOST=<n>:blip:again, a slow
 * job-manager driver's transient twice, pause_ms apart, nothing finished in between): after the pause one more
 * submit + fence wait meets the driver's second transient (-4 or VK_SUCCESS to the app: the fence did signal),
 * then the <after> rounds and the device wait must answer VK_SUCCESS. "cb": every round's submission carries a
 * command buffer (a vkCmdFillBuffer and a barrier) instead of none, so the server's record of what the lost
 * device was given has command buffers to name (host/lostcause_test.sh). "exit": the program ends at the first
 * lost answer (destroys and exits, no call after it), as a game that quits on its first VK_ERROR_DEVICE_LOST.
 */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define I(name) static PFN_##name name
I(vkCreateInstance); I(vkEnumeratePhysicalDevices); I(vkGetPhysicalDeviceQueueFamilyProperties); I(vkCreateDevice);
I(vkGetDeviceProcAddr); I(vkDestroyInstance); I(vkGetDeviceQueue); I(vkQueueSubmit); I(vkCreateFence); I(vkDestroyFence);
I(vkWaitForFences); I(vkResetFences); I(vkDeviceWaitIdle); I(vkDestroyDevice);
I(vkGetPhysicalDeviceMemoryProperties); I(vkCreateBuffer); I(vkDestroyBuffer); I(vkGetBufferMemoryRequirements); I(vkAllocateMemory);
I(vkFreeMemory); I(vkBindBufferMemory); I(vkCreateCommandPool); I(vkDestroyCommandPool); I(vkAllocateCommandBuffers);
I(vkBeginCommandBuffer); I(vkEndCommandBuffer); I(vkCmdFillBuffer); I(vkCmdPipelineBarrier);
#undef I

int main(int argc, char **argv)
{
    int rounds = argc > 1 ? atoi(argv[1]) : 20, after = argc > 2 ? atoi(argv[2]) : 4, pause_ms = argc > 3 ? atoi(argv[3]) : 300;
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib) {
        printf("FAILED: loader: %s\n", dlerror());
        return 2;
    }
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    VkInstance inst = VK_NULL_HANDLE;
#define G(n) n = (PFN_##n)gipa(inst, #n)
    G(vkCreateInstance);
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "vkbridge-lost-check", 1, "vkbridge", 1, VK_API_VERSION_1_1};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 0, NULL};
    if (vkCreateInstance(&ici, NULL, &inst)) {
        printf("FAILED: vkCreateInstance\n");
        return 1;
    }
    G(vkEnumeratePhysicalDevices); G(vkGetPhysicalDeviceQueueFamilyProperties); G(vkCreateDevice); G(vkGetDeviceProcAddr);
    G(vkDestroyInstance); G(vkGetPhysicalDeviceMemoryProperties);
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
        if (qp[qfam].queueFlags & VK_QUEUE_GRAPHICS_BIT) break;
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, qfam, 1, &prio};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &qci, 0, NULL, 0, NULL, NULL};
    VkDevice dev;
    if (qfam == nq || vkCreateDevice(pd, &dci, NULL, &dev)) {
        printf("FAILED: vkCreateDevice\n");
        return 1;
    }
#define G(n) n = (PFN_##n)vkGetDeviceProcAddr(dev, #n)
    G(vkGetDeviceQueue); G(vkQueueSubmit); G(vkCreateFence); G(vkDestroyFence); G(vkWaitForFences); G(vkResetFences);
    G(vkDeviceWaitIdle); G(vkDestroyDevice);
    G(vkCreateBuffer); G(vkDestroyBuffer); G(vkGetBufferMemoryRequirements); G(vkAllocateMemory); G(vkFreeMemory);
    G(vkBindBufferMemory); G(vkCreateCommandPool); G(vkDestroyCommandPool); G(vkAllocateCommandBuffers); G(vkBeginCommandBuffer);
    G(vkEndCommandBuffer); G(vkCmdFillBuffer); G(vkCmdPipelineBarrier);
#undef G
    VkQueue q;
    vkGetDeviceQueue(dev, qfam, 0, &q);
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence f;
    vkCreateFence(dev, &fci, NULL, &f);
    const char *mode = argc > 4 ? argv[4] : "";
    int with_cb = !strcmp(mode, "cb"), exit_at_loss = !strcmp(mode, "exit");
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    if (with_cb) {
        VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, 4096, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
        VkMemoryRequirements req;
        VkPhysicalDeviceMemoryProperties mp;
        vkGetPhysicalDeviceMemoryProperties(pd, &mp);
        if (vkCreateBuffer(dev, &bci, NULL, &buf)) {
            printf("FAILED: vkCreateBuffer\n");
            return 1;
        }
        vkGetBufferMemoryRequirements(dev, buf, &req);
        uint32_t type = 0;
        while (type < mp.memoryTypeCount && !(req.memoryTypeBits & (1u << type))) type++;
        VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size, type};
        VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, qfam};
        VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, VK_NULL_HANDLE, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
        if (type == mp.memoryTypeCount || vkAllocateMemory(dev, &mai, NULL, &mem) || vkBindBufferMemory(dev, buf, mem, 0) ||
            vkCreateCommandPool(dev, &pci, NULL, &pool) || (cai.commandPool = pool, vkAllocateCommandBuffers(dev, &cai, &cb))) {
            printf("FAILED: the command buffer's setup\n");
            return 1;
        }
    }
    int lost_at = -1;
    for (int i = 0; i < rounds && lost_at < 0; i++) {
        VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cb, 0, NULL};
        if (with_cb) {
            VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
            VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
            vkBeginCommandBuffer(cb, &bi);
            vkCmdFillBuffer(cb, buf, 0, 4096, (uint32_t)i);
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
            vkEndCommandBuffer(cb);
        }
        VkResult s = vkQueueSubmit(q, with_cb ? 1 : 0, with_cb ? &si : NULL, f);
        VkResult w = s == VK_SUCCESS ? vkWaitForFences(dev, 1, &f, VK_TRUE, 2000000000ull) : s;
        if (w == VK_ERROR_DEVICE_LOST) lost_at = i;
        else if (w != VK_SUCCESS) {
            printf("FAILED: round %d: submit %d, wait %d\n", i, s, w);
            return 1;
        } else
            vkResetFences(dev, 1, &f);
    }
    if (lost_at < 0) {
        printf("FAILED: the device was never lost in %d rounds\n", rounds);
        return 1;
    }
    printf("device lost at round %d\n", lost_at);
    if (exit_at_loss) {
        vkDestroyFence(dev, f, NULL);
        vkDestroyDevice(dev, NULL);
        vkDestroyInstance(inst, NULL);
        printf("OK: lost at round %d, the program ended at once\n", lost_at);
        return 0;
    }
    struct timespec ts = {pause_ms / 1000, (long)(pause_ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
    int bad = 0;
    int again = argc > 4 && !strcmp(argv[4], "again");
    if (again) {
        vkResetFences(dev, 1, &f);
        VkResult s = vkQueueSubmit(q, 0, NULL, f);
        VkResult w = s == VK_SUCCESS ? vkWaitForFences(dev, 1, &f, VK_TRUE, 2000000000ull) : s;
        /* the driver answers this wait -4 again (the server log says so); the app may still get VK_SUCCESS: the
         * fence did signal, and the client's own fence state can answer */
        printf("second transient %d ms later: submit %d, wait %d\n", pause_ms, s, w);
        if (w != VK_ERROR_DEVICE_LOST && w != VK_SUCCESS) {
            printf("FAILED: the second transient's wait answered %d\n", w);
            return 1;
        }
    }
    if (argc > 4 && (!strcmp(argv[4], "transient") || again)) {
        vkResetFences(dev, 1, &f);
        for (int i = 0; i < after; i++) {
            VkResult s = vkQueueSubmit(q, 0, NULL, f);
            VkResult w = s == VK_SUCCESS ? vkWaitForFences(dev, 1, &f, VK_TRUE, 2000000000ull) : s;
            if (w != VK_SUCCESS) {
                printf("FAILED: round %d after the lost answer: submit %d, wait %d (the loss was made permanent)\n", i, s, w);
                bad++;
                break;
            }
            vkResetFences(dev, 1, &f);
        }
        VkResult idle = vkDeviceWaitIdle(dev);
        if (!bad && idle != VK_SUCCESS) {
            printf("FAILED: device wait %d after the lost answer\n", idle);
            bad++;
        }
        vkDestroyFence(dev, f, NULL);
        vkDestroyDevice(dev, NULL);
        vkDestroyInstance(inst, NULL);
        if (!bad) printf("OK: a lost answer at round %d, then %d rounds fine\n", lost_at, after);
        return bad ? 1 : 0;
    }
    /* as the game: more submissions after the loss (asynchronous here: the client answered them) */
    for (int i = 0; i < after; i++) vkQueueSubmit(q, 0, NULL, VK_NULL_HANDLE);
    nanosleep(&ts, NULL); /* the game goes on for a while before it tears down */
    /* unsignaled, so that the wait asks the server (a loss first seen at a wait leaves the fence signaled by the driver) */
    vkResetFences(dev, 1, &f);
    VkResult w = vkWaitForFences(dev, 1, &f, VK_TRUE, 2000000000ull);
    VkResult idle = vkDeviceWaitIdle(dev);
    printf("after %d more submission(s): fence wait %d, device wait %d\n", after, w, idle);
    if (w != VK_ERROR_DEVICE_LOST || idle != VK_ERROR_DEVICE_LOST) bad++;
    if (pool) vkDestroyCommandPool(dev, pool, NULL);
    if (buf) vkDestroyBuffer(dev, buf, NULL);
    if (mem) vkFreeMemory(dev, mem, NULL);
    vkDestroyFence(dev, f, NULL);
    vkDestroyDevice(dev, NULL);
    vkDestroyInstance(inst, NULL);
    if (bad) printf("FAILED: a call after the loss did not answer VK_ERROR_DEVICE_LOST\n");
    else printf("OK: lost at round %d, %d submission(s) after it\n", lost_at, after);
    return bad ? 1 : 0;
}
