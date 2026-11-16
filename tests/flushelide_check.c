/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * flushelide_check: the upload pattern of Wine's WoW64 OpenGL buffers (dlls/opengl32/unix_wgl.c flush_buffer, 32-bit
 * games under WineD3D): host-visible memory mapped once, and each round the CPU writes a fresh pattern and flushes the
 * written range with vkFlushMappedMemoryRanges, the GPU fills one region and copies the CPU's pattern into another,
 * and the CPU invalidates and checks both (it reads every region before the GPU writes it, so a missing invalidate on
 * memory that needs one leaves stale lines to find). Every call must return VK_SUCCESS and every round read back right.
 * How many of the calls went to the server is the caller's to read (VKBRIDGE_STATS=1: the client's per-command counts
 * at exit; host/flushelide_test.sh).
 *   flushelide_check            a HOST_COHERENT type (the bridge's shared memory, coherent without flushes)
 *   flushelide_check noncoherent a host-visible type without HOST_COHERENT (one the self-test sent to flushes)
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

static void check(int ok, const char *what, const char *detail)
{
    checks++;
    if (!ok) failures++;
    printf("%s %s%s%s\n", ok ? "PASS" : "FAIL", what, detail && *detail ? ": " : "", detail ? detail : "");
    fflush(stdout);
}

#define SIZE (256u << 10)
#define ROUNDS 200

int main(int argc, char **argv)
{
    int want_coherent = !(argc > 1 && !strcmp(argv[1], "noncoherent"));
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
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "flushelide_check", 1, NULL, 0, VK_API_VERSION_1_1};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app};
    if (vkCreateInstance(&ici, NULL, &inst) != VK_SUCCESS) {
        printf("FAIL instance\n");
        return 1;
    }
    GI(vkEnumeratePhysicalDevices);
    GI(vkGetPhysicalDeviceMemoryProperties);
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
    GD(vkGetDeviceQueue);
    GD(vkCreateBuffer);
    GD(vkGetBufferMemoryRequirements);
    GD(vkAllocateMemory);
    GD(vkBindBufferMemory);
    GD(vkMapMemory);
    GD(vkFlushMappedMemoryRanges);
    GD(vkInvalidateMappedMemoryRanges);
    GD(vkCreateCommandPool);
    GD(vkAllocateCommandBuffers);
    GD(vkBeginCommandBuffer);
    GD(vkEndCommandBuffer);
    GD(vkResetCommandBuffer);
    GD(vkCmdCopyBuffer);
    GD(vkCmdFillBuffer);
    GD(vkCmdPipelineBarrier);
    GD(vkQueueSubmit);
    GD(vkQueueWaitIdle);
    GD(vkFreeMemory);
    GD(vkDestroyBuffer);
    GD(vkDestroyDevice);

    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, SIZE,
                              VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VkBuffer buf;
    vkCreateBuffer(dev, &bci, NULL, &buf);
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev, buf, &req);
    uint32_t type = UINT32_MAX;
    char d[256];
    int dn = 0;
    for (uint32_t k = 0; k < mp.memoryTypeCount; k++) {
        VkMemoryPropertyFlags f = mp.memoryTypes[k].propertyFlags;
        dn += snprintf(d + dn, sizeof(d) - (size_t)dn, "%s%u:0x%x", k ? " " : "", k, f);
        int coh = (f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
        if (type == UINT32_MAX && (req.memoryTypeBits & (1u << k)) && (f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) && coh == want_coherent)
            type = k;
    }
    check(type != UINT32_MAX, want_coherent ? "a HOST_COHERENT host-visible type for the buffer" : "a host-visible type without HOST_COHERENT", d);
    if (type == UINT32_MAX) {
        printf("FAILED: %d of %d checks passed\n", checks - failures, checks);
        return 1;
    }

    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size, type};
    VkDeviceMemory mem;
    VkResult r = vkAllocateMemory(dev, &ai, NULL, &mem);
    snprintf(d, sizeof(d), "type %u (flags 0x%x), %llu bytes: %d", type, mp.memoryTypes[type].propertyFlags,
             (unsigned long long)req.size, r);
    check(r == VK_SUCCESS, "allocation", d);
    if (r != VK_SUCCESS) return 1;
    vkBindBufferMemory(dev, buf, mem, 0);
    volatile uint32_t *p = NULL;
    r = vkMapMemory(dev, mem, 0, VK_WHOLE_SIZE, 0, (void **)&p);
    check(r == VK_SUCCESS && p, "map", "");
    if (!p) return 1;

    VkQueue q;
    vkGetDeviceQueue(dev, 0, 0, &q);
    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, 0};
    VkCommandPool pool;
    vkCreateCommandPool(dev, &pci, NULL, &pool);
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VkCommandBuffer cb;
    vkAllocateCommandBuffers(dev, &cai, &cb);
    VkMappedMemoryRange whole = {VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, NULL, mem, 0, VK_WHOLE_SIZE};

    /* Words: [0, Q) written by the CPU, [Q, 2Q) filled by the GPU, [2Q, 3Q) the GPU's copy of the
     * CPU's words. */
    const uint32_t Q = SIZE / 4 / 4;
    int bad_rounds = 0, flush_err = 0;
    char first_bad[160] = "";
    for (int round = 0; round < ROUNDS; round++) {
        uint32_t seed = 0x9e3779b9u * (uint32_t)(round + 1), fill = 0xC0DE0000u + (uint32_t)round;
        /* The CPU reads what the GPU is about to overwrite: stale lines for a missing invalidate. */
        uint32_t sink = 0;
        for (uint32_t i = Q; i < 3 * Q; i += 16) sink += p[i];
        __asm__ volatile("" : : "r"(sink));
        for (uint32_t i = 0; i < Q; i++) p[i] = seed ^ (i * 2654435761u);
        VkMappedMemoryRange written = {VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, NULL, mem, 0, (VkDeviceSize)Q * 4};
        if (vkFlushMappedMemoryRanges(dev, 1, &written) != VK_SUCCESS) flush_err++;
        VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        vkBeginCommandBuffer(cb, &bi);
        VkMemoryBarrier hb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT};
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &hb, 0, NULL, 0, NULL);
        vkCmdFillBuffer(cb, buf, (VkDeviceSize)Q * 4, (VkDeviceSize)Q * 4, fill);
        VkBufferCopy region = {0, (VkDeviceSize)Q * 8, (VkDeviceSize)Q * 4};
        vkCmdCopyBuffer(cb, buf, buf, 1, &region);
        VkMemoryBarrier rb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &rb, 0, NULL, 0, NULL);
        vkEndCommandBuffer(cb);
        VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cb};
        if (vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE) != VK_SUCCESS || vkQueueWaitIdle(q) != VK_SUCCESS) {
            check(0, "GPU submission", "");
            return 1;
        }
        if (vkInvalidateMappedMemoryRanges(dev, 1, &whole) != VK_SUCCESS) flush_err++;
        int bad = 0;
        for (uint32_t i = 0; i < Q && !bad; i++) {
            uint32_t got = p[Q + i];
            if (got != fill) {
                bad = 1;
                if (!*first_bad) snprintf(first_bad, sizeof(first_bad), "round %d: GPU fill word %u = 0x%08x, want 0x%08x", round, i, got, fill);
            }
        }
        for (uint32_t i = 0; i < Q && !bad; i++) {
            uint32_t got = p[2 * Q + i], w = seed ^ (i * 2654435761u);
            if (got != w) {
                bad = 1;
                if (!*first_bad) snprintf(first_bad, sizeof(first_bad), "round %d: GPU copy word %u = 0x%08x, want 0x%08x", round, i, got, w);
            }
        }
        bad_rounds += bad;
        vkResetCommandBuffer(cb, 0);
    }
    snprintf(d, sizeof(d), "%d rounds of %u KB%s%s", ROUNDS, SIZE >> 10, *first_bad ? "; first wrong: " : "", first_bad);
    check(bad_rounds == 0, "CPU writes reach the GPU and GPU writes reach the CPU (flush, invalidate)", d);
    snprintf(d, sizeof(d), "%d of %d calls", flush_err, 2 * ROUNDS);
    check(flush_err == 0, "every flush and invalidate returned VK_SUCCESS", d);

    vkFreeMemory(dev, mem, NULL);
    vkDestroyBuffer(dev, buf, NULL);
    vkDestroyDevice(dev, NULL);
    printf("%s: %d of %d checks passed (type %u flags 0x%x, %d rounds; %d flushes and %d invalidates made)\n", failures ? "FAILED" : "OK",
           checks - failures, checks, type, mp.memoryTypes[type].propertyFlags, ROUNDS, ROUNDS, ROUNDS);
    return failures != 0;
}
