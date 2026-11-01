/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * sparse_check [vkd3d|other]: what vkd3d-proton does with a D3D12 reserved buffer, on a driver
 * without sparse memory (host: VKBRIDGE_FAKE_MISSING=sparse on the server). As engine "vkd3d":
 *   - no sparse feature reported, a graphics queue family with VK_QUEUE_SPARSE_BINDING_BIT;
 *   - a 3 MB buffer created with the three sparse flags: 64 KiB alignment, a device address;
 *   - its memory is real: a fill and a copy back read 0xabcd1234;
 *   - vkQueueBindSparse (one buffer bind) signals its timeline value and its fence.
 * As another engine: no sparse queue flag either.
 * One line per check, "OK: ..." / "FAILED: ..." last.
 */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

static int failures, checks;

static void check(int ok, const char *what, long long v)
{
    checks++;
    if (!ok) failures++;
    printf("%s %s (%lld)\n", ok ? "PASS" : "FAIL", what, v);
}

int main(int argc, char **argv)
{
    int vkd3d = argc < 2 || !strcmp(argv[1], "vkd3d");
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib) {
        printf("FAILED: loader: %s\n", dlerror());
        return 2;
    }
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "vkbridge-sparse-check", 1, vkd3d ? "vkd3d" : "other", 1,
                             VK_API_VERSION_1_2};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 0, NULL};
    VkInstance inst;
    if (((PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance"))(&ici, NULL, &inst)) {
        printf("FAILED: vkCreateInstance\n");
        return 1;
    }
#define GI(name) PFN_##name name = (PFN_##name)gipa(inst, #name)
    GI(vkEnumeratePhysicalDevices); GI(vkGetPhysicalDeviceFeatures); GI(vkGetPhysicalDeviceQueueFamilyProperties);
    GI(vkGetPhysicalDeviceMemoryProperties); GI(vkCreateDevice); GI(vkGetDeviceProcAddr); GI(vkDestroyInstance);
#undef GI
    uint32_t n = 1;
    VkPhysicalDevice pd;
    if (vkEnumeratePhysicalDevices(inst, &n, &pd) < 0 || !n) {
        printf("FAILED: no physical device\n");
        return 1;
    }
    VkPhysicalDeviceFeatures f;
    vkGetPhysicalDeviceFeatures(pd, &f);
    check(!f.sparseBinding && !f.sparseResidencyBuffer && !f.sparseResidencyImage2D, "no sparse feature reported", f.sparseBinding);
    VkQueueFamilyProperties qf[16];
    uint32_t nq = 16;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qf);
    uint32_t fam = UINT32_MAX;
    for (uint32_t i = 0; i < nq && fam == UINT32_MAX; i++)
        if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) fam = i;
    if (fam == UINT32_MAX) {
        printf("FAILED: no graphics queue\n");
        return 1;
    }
    int sparse_q = !!(qf[fam].queueFlags & VK_QUEUE_SPARSE_BINDING_BIT);
    if (!vkd3d) {
        check(!sparse_q, "another engine: no sparse queue flag", qf[fam].queueFlags);
        vkDestroyInstance(inst, NULL);
        if (failures) printf("FAILED: %d of %d checks\n", failures, checks);
        else printf("OK: %d checks (other engine)\n", checks);
        return failures ? 1 : 0;
    }
    check(sparse_q, "vkd3d: the graphics family says sparse binding", qf[fam].queueFlags);

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, fam, 1, &prio};
    VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    f12.bufferDeviceAddress = VK_TRUE;
    f12.timelineSemaphore = VK_TRUE;
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f12, 0, 1, &qci, 0, NULL, 0, NULL, NULL};
    VkDevice dev;
    VkResult r = vkCreateDevice(pd, &dci, NULL, &dev);
    if (r) {
        printf("FAILED: vkCreateDevice (%d)\n", r);
        return 1;
    }
#define G(name) PFN_##name name = (PFN_##name)vkGetDeviceProcAddr(dev, #name)
    G(vkGetDeviceQueue); G(vkCreateBuffer); G(vkDestroyBuffer); G(vkGetBufferMemoryRequirements); G(vkGetBufferDeviceAddress);
    G(vkAllocateMemory); G(vkFreeMemory); G(vkBindBufferMemory); G(vkMapMemory); G(vkQueueBindSparse); G(vkCreateSemaphore);
    G(vkDestroySemaphore); G(vkWaitSemaphores); G(vkCreateFence); G(vkDestroyFence); G(vkWaitForFences); G(vkCreateCommandPool);
    G(vkDestroyCommandPool); G(vkAllocateCommandBuffers); G(vkBeginCommandBuffer); G(vkEndCommandBuffer); G(vkCmdFillBuffer);
    G(vkCmdCopyBuffer); G(vkCmdPipelineBarrier); G(vkQueueSubmit); G(vkDeviceWaitIdle); G(vkDestroyDevice);
#undef G
    VkQueue q;
    vkGetDeviceQueue(dev, fam, 0, &q);

    /* the reserved buffer, as vkd3d-proton makes it */
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL,
                              VK_BUFFER_CREATE_SPARSE_BINDING_BIT | VK_BUFFER_CREATE_SPARSE_RESIDENCY_BIT | VK_BUFFER_CREATE_SPARSE_ALIASED_BIT,
                              3u << 20,
                              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                  VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                              VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
    VkBuffer sb = VK_NULL_HANDLE;
    r = vkCreateBuffer(dev, &bci, NULL, &sb);
    check(r == VK_SUCCESS, "reserved buffer created", r);
    if (r != VK_SUCCESS) { /* over the budget (sparse_test.sh step 3): nothing more to check */
        vkDestroyDevice(dev, NULL);
        vkDestroyInstance(inst, NULL);
        printf("FAILED: %d of %d checks\n", failures, checks);
        return 1;
    }
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(dev, sb, &mr);
    check(mr.alignment >= 65536, "its alignment is the 64 KiB tile", (long long)mr.alignment);
    VkBufferDeviceAddressInfo bai = {VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, NULL, sb};
    VkDeviceAddress va = vkGetBufferDeviceAddress(dev, &bai);
    check(va != 0, "it has a device address at creation", (long long)va);

    /* a host-visible buffer to read it back */
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    VkBufferCreateInfo rci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, 4096, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
    VkBuffer rb;
    vkCreateBuffer(dev, &rci, NULL, &rb);
    VkMemoryRequirements rr;
    vkGetBufferMemoryRequirements(dev, rb, &rr);
    uint32_t ht = 0;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((rr.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                                                   (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            ht = i;
            break;
        }
    VkMemoryAllocateInfo rai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, rr.size, ht};
    VkDeviceMemory rm;
    vkAllocateMemory(dev, &rai, NULL, &rm);
    vkBindBufferMemory(dev, rb, rm, 0);
    uint32_t *host = NULL;
    vkMapMemory(dev, rm, 0, VK_WHOLE_SIZE, 0, (void **)&host);

    /* a bind, as UpdateTileMappings makes one: a heap page into the first tile, timeline + fence */
    VkMemoryAllocateInfo hai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, 65536, 0};
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) {
            hai.memoryTypeIndex = i;
            break;
        }
    VkDeviceMemory heap;
    vkAllocateMemory(dev, &hai, NULL, &heap);
    VkSemaphoreTypeCreateInfo stc = {VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, NULL, VK_SEMAPHORE_TYPE_TIMELINE, 0};
    VkSemaphoreCreateInfo sci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &stc, 0};
    VkSemaphore tl;
    vkCreateSemaphore(dev, &sci, NULL, &tl);
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence;
    vkCreateFence(dev, &fci, NULL, &fence);
    VkSparseMemoryBind mb = {0, 65536, heap, 0, 0};
    VkSparseBufferMemoryBindInfo bb = {sb, 1, &mb};
    uint64_t val = 5;
    VkTimelineSemaphoreSubmitInfo tsi = {VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO, NULL, 0, NULL, 1, &val};
    VkBindSparseInfo bi = {VK_STRUCTURE_TYPE_BIND_SPARSE_INFO, &tsi, 0, NULL, 1, &bb, 0, NULL, 0, NULL, 1, &tl};
    r = vkQueueBindSparse(q, 1, &bi, fence);
    check(r == VK_SUCCESS, "vkQueueBindSparse", r);
    VkSemaphoreWaitInfo wi = {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, NULL, 0, 1, &tl, &val};
    r = vkWaitSemaphores(dev, &wi, 5000000000ull);
    check(r == VK_SUCCESS, "its timeline value is signalled", r);
    r = vkWaitForFences(dev, 1, &fence, VK_TRUE, 5000000000ull);
    check(r == VK_SUCCESS, "its fence is signalled", r);

    /* the memory behind the buffer is real */
    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, 0, fam};
    VkCommandPool pool;
    vkCreateCommandPool(dev, &pci, NULL, &pool);
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VkCommandBuffer cb;
    vkAllocateCommandBuffers(dev, &cai, &cb);
    VkCommandBufferBeginInfo cbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
    vkBeginCommandBuffer(cb, &cbi);
    vkCmdFillBuffer(cb, sb, (2u << 20) + 4096, 4096, 0xabcd1234u);
    VkMemoryBarrier mbar = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mbar, 0, NULL, 0, NULL);
    VkBufferCopy cp = {(2u << 20) + 4096, 0, 4096};
    vkCmdCopyBuffer(cb, sb, rb, 1, &cp);
    vkEndCommandBuffer(cb);
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cb, 0, NULL};
    vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE);
    vkDeviceWaitIdle(dev);
    check(host && host[0] == 0xabcd1234u && host[1023] == 0xabcd1234u, "a fill of a tile far from the bound one reads back",
          host ? (long long)host[0] : -1);

    vkDestroyCommandPool(dev, pool, NULL);
    vkDestroyFence(dev, fence, NULL);
    vkDestroySemaphore(dev, tl, NULL);
    vkDestroyBuffer(dev, sb, NULL);
    vkDestroyBuffer(dev, rb, NULL);
    vkFreeMemory(dev, rm, NULL);
    vkFreeMemory(dev, heap, NULL);
    vkDestroyDevice(dev, NULL);
    vkDestroyInstance(inst, NULL);
    if (failures) printf("FAILED: %d of %d checks\n", failures, checks);
    else printf("OK: %d checks\n", checks);
    return failures ? 1 : 0;
}
