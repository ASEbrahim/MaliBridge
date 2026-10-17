/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * begin_check: vkBeginCommandBuffer's begin info as the server decodes it. Records a primary
 * command buffer and a secondary one with inheritance info (both single-element pointers on the
 * wire), each begun, filled and ended three times, and submits the primary, which executes the
 * secondary.
 *
 *   begin_check ok        every vkEndCommandBuffer and the submit succeed, the buffer is filled
 *   begin_check refused   (with VKBRIDGE_SYNC=1 VKBRIDGE_TEST_BEGIN_NOCOUNT=1: the begin info
 *                         encoded without its count, as a bridge copy of 2026-10-10 did) every
 *                         vkEndCommandBuffer fails; nothing is submitted
 * One line per step, "OK: ..." / "FAILED: ..." last. The server log says the rest
 * (begin_encoding_test.sh).
 */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

static PFN_vkGetInstanceProcAddr gipa;
static VkInstance inst;
static VkDevice dev;
static int failures, checks;

#define I(name) static PFN_##name name
I(vkCreateInstance); I(vkEnumeratePhysicalDevices); I(vkGetPhysicalDeviceQueueFamilyProperties); I(vkGetPhysicalDeviceMemoryProperties);
I(vkCreateDevice); I(vkGetDeviceProcAddr); I(vkDestroyInstance); I(vkGetDeviceQueue); I(vkCreateCommandPool); I(vkDestroyCommandPool);
I(vkAllocateCommandBuffers); I(vkBeginCommandBuffer); I(vkEndCommandBuffer); I(vkQueueSubmit); I(vkCreateFence); I(vkDestroyFence);
I(vkWaitForFences); I(vkCreateBuffer); I(vkDestroyBuffer); I(vkGetBufferMemoryRequirements); I(vkAllocateMemory); I(vkFreeMemory);
I(vkBindBufferMemory); I(vkMapMemory); I(vkCmdFillBuffer); I(vkCmdExecuteCommands); I(vkCmdPipelineBarrier); I(vkDeviceWaitIdle);
I(vkDestroyDevice);
#undef I

static void check(int ok, const char *what, int r)
{
    checks++;
    if (!ok) failures++;
    printf("%s %s (%d)\n", ok ? "PASS" : "FAIL", what, r);
}

int main(int argc, char **argv)
{
    int refused = argc > 1 && !strcmp(argv[1], "refused");
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib) {
        printf("FAILED: loader: %s\n", dlerror());
        return 2;
    }
    gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
#define G(n) n = (PFN_##n)gipa(inst, #n)
    G(vkCreateInstance);
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "vkbridge-begin-check", 1, "vkbridge", 1, VK_API_VERSION_1_1};
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
        if (qp[qfam].queueFlags & VK_QUEUE_GRAPHICS_BIT) break;
    if (qfam == nq) {
        printf("FAILED: no graphics queue\n");
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
    G(vkEndCommandBuffer); G(vkQueueSubmit); G(vkCreateFence); G(vkDestroyFence); G(vkWaitForFences); G(vkCreateBuffer);
    G(vkDestroyBuffer); G(vkGetBufferMemoryRequirements); G(vkAllocateMemory); G(vkFreeMemory); G(vkBindBufferMemory); G(vkMapMemory);
    G(vkCmdFillBuffer); G(vkCmdExecuteCommands); G(vkCmdPipelineBarrier); G(vkDeviceWaitIdle); G(vkDestroyDevice);
#undef G
    VkQueue queue;
    vkGetDeviceQueue(dev, qfam, 0, &queue);

    /* A host-visible buffer the command buffers fill: the primary one half, the secondary the other. */
    const VkDeviceSize size = 4096;
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
    VkBuffer buf;
    vkCreateBuffer(dev, &bci, NULL, &buf);
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(dev, buf, &mr);
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    uint32_t mt = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount && mt == UINT32_MAX; i++)
        if ((mr.memoryTypeBits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
            mt = i;
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, mt};
    VkDeviceMemory mem;
    void *ptr = NULL;
    if (mt == UINT32_MAX || vkAllocateMemory(dev, &mai, NULL, &mem) || vkBindBufferMemory(dev, buf, mem, 0) ||
        vkMapMemory(dev, mem, 0, VK_WHOLE_SIZE, 0, &ptr)) {
        printf("FAILED: setup: host-visible buffer\n");
        return 1;
    }
    memset(ptr, 0, size);

    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, qfam};
    VkCommandPool pool;
    vkCreateCommandPool(dev, &pci, NULL, &pool);
    VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VkCommandBuffer prim, sec;
    vkAllocateCommandBuffers(dev, &ai, &prim);
    ai.level = VK_COMMAND_BUFFER_LEVEL_SECONDARY;
    vkAllocateCommandBuffers(dev, &ai, &sec);

    VkResult r_sec = VK_SUCCESS, r_prim = VK_SUCCESS;
    for (int round = 0; round < 3; round++) {
        VkCommandBufferInheritanceInfo inh = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO, NULL, VK_NULL_HANDLE, 0, VK_NULL_HANDLE,
                                              VK_FALSE, 0, 0};
        VkCommandBufferBeginInfo sbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, 0, &inh};
        vkBeginCommandBuffer(sec, &sbi);
        vkCmdFillBuffer(sec, buf, size / 2, size / 2, 0x5ec0a7d0u + (uint32_t)round);
        r_sec = vkEndCommandBuffer(sec);
        VkCommandBufferBeginInfo pbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
        vkBeginCommandBuffer(prim, &pbi);
        vkCmdFillBuffer(prim, buf, 0, size / 2, 0x0b1a5e00u + (uint32_t)round);
        vkCmdExecuteCommands(prim, 1, &sec);
        VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
        vkCmdPipelineBarrier(prim, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
        r_prim = vkEndCommandBuffer(prim);
        if (refused) {
            check(r_sec != VK_SUCCESS, "secondary with inheritance info: vkEndCommandBuffer fails", r_sec);
            check(r_prim != VK_SUCCESS, "primary: vkEndCommandBuffer fails", r_prim);
            continue;
        }
        check(r_sec == VK_SUCCESS, "secondary with inheritance info: vkEndCommandBuffer", r_sec);
        check(r_prim == VK_SUCCESS, "primary: vkEndCommandBuffer", r_prim);
        VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VkFence f;
        vkCreateFence(dev, &fci, NULL, &f);
        VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &prim, 0, NULL};
        VkResult r = vkQueueSubmit(queue, 1, &si, f);
        if (r == VK_SUCCESS) r = vkWaitForFences(dev, 1, &f, VK_TRUE, 5000000000ull);
        check(r == VK_SUCCESS, "submit and wait", r);
        vkDestroyFence(dev, f, NULL);
        const uint32_t *w = ptr;
        int filled = w[0] == 0x0b1a5e00u + (uint32_t)round && w[size / 4 - 1] == 0x5ec0a7d0u + (uint32_t)round;
        check(filled, "both halves filled", (int)round);
    }
    vkDeviceWaitIdle(dev);
    vkDestroyCommandPool(dev, pool, NULL);
    vkDestroyBuffer(dev, buf, NULL);
    vkFreeMemory(dev, mem, NULL);
    vkDestroyDevice(dev, NULL);
    vkDestroyInstance(inst, NULL);
    if (failures) printf("FAILED: %d of %d checks (%s)\n", failures, checks, refused ? "refused" : "ok");
    else printf("OK: %d checks (%s)\n", checks, refused ? "refused" : "ok");
    return failures ? 1 : 0;
}
