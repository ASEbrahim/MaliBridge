/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * cached_check: cached readback as an app sees it (server_memory.c, VKBRIDGE_CACHED_READBACK=apps):
 * a HOST_VISIBLE | HOST_CACHED memory type without HOST_COHERENT, used the way the spec says for
 * such memory. Each round the CPU writes a fresh pattern and flushes, the GPU fills one region and
 * copies the CPU's pattern into another, and the CPU invalidates and checks both - with the CPU
 * reading every region before the GPU writes it, so that a missing invalidate leaves stale lines
 * to be found. Then the read speed of the mapping (the reason for the cached pages).
 *
 *   cached_check expect   the type must be offered, and every round must read back right
 *   cached_check none     the type must not be offered (cached readback off, or for the client's
 *                         frame readback only)
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
#include <time.h>

static int failures, checks;

static void check(int ok, const char *what, const char *detail)
{
    checks++;
    if (!ok) failures++;
    printf("%s %s%s%s\n", ok ? "PASS" : "FAIL", what, detail && *detail ? ": " : "", detail ? detail : "");
    fflush(stdout);
}

static double now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

#define SIZE (4u << 20)
#define ROUNDS 40

int main(int argc, char **argv)
{
    int expect = argc > 1 && !strcmp(argv[1], "expect");
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
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "cached_check", 1, NULL, 0, VK_API_VERSION_1_1};
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
    const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    uint32_t type = UINT32_MAX;
    char d[256];
    int dn = 0;
    for (uint32_t k = 0; k < mp.memoryTypeCount; k++) {
        VkMemoryPropertyFlags f = mp.memoryTypes[k].propertyFlags;
        dn += snprintf(d + dn, sizeof(d) - (size_t)dn, "%s%u:0x%x", k ? " " : "", k, f);
        if (type == UINT32_MAX && (req.memoryTypeBits & (1u << k)) && (f & want) == want && !(f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
            type = k;
    }
    if (!expect) {
        check(type == UINT32_MAX, "no cached type without HOST_COHERENT offered", d);
        vkDestroyBuffer(dev, buf, NULL);
        vkDestroyDevice(dev, NULL);
        printf("%s: %d of %d checks passed (types %s)\n", failures ? "FAILED" : "OK", checks - failures, checks, d);
        return failures != 0;
    }
    check(type != UINT32_MAX, "a cached type without HOST_COHERENT offered", d);
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
    int bad_rounds = 0;
    char first_bad[160] = "";
    for (int round = 0; round < ROUNDS; round++) {
        uint32_t seed = 0x9e3779b9u * (uint32_t)(round + 1), fill = 0xC0DE0000u + (uint32_t)round;
        /* The CPU reads what the GPU is about to overwrite: stale lines for a missing invalidate. */
        uint32_t sink = 0;
        for (uint32_t i = Q; i < 3 * Q; i += 16) sink += p[i];
        __asm__ volatile("" : : "r"(sink));
        for (uint32_t i = 0; i < Q; i++) p[i] = seed ^ (i * 2654435761u);
        vkFlushMappedMemoryRanges(dev, 1, &whole);
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
        vkInvalidateMappedMemoryRanges(dev, 1, &whole);
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

    /* The mapping's read speed, after an invalidate (what the frame readback pays per frame). */
    uint8_t *volatile dst = malloc(SIZE);
    double best = 1e9;
    uint32_t sum = 0;
    for (int k = 0; k < 5; k++) {
        vkInvalidateMappedMemoryRanges(dev, 1, &whole);
        double t0 = now_ms();
        memcpy(dst, (const void *)p, SIZE);
        double t = now_ms() - t0;
        sum += dst[(k * 4099u) % SIZE];
        if (t < best) best = t;
    }
    __asm__ volatile("" : : "r"(sum));
    free(dst);
    snprintf(d, sizeof(d), "%.2f ms for %u KB (%.0f MB/s)", best, SIZE >> 10, (SIZE / 1048576.0) / (best / 1e3));
    check(1, "read speed", d);

    vkFreeMemory(dev, mem, NULL);
    vkDestroyBuffer(dev, buf, NULL);
    vkDestroyDevice(dev, NULL);
    printf("%s: %d of %d checks passed (%s)\n", failures ? "FAILED" : "OK", checks - failures, checks, d);
    return failures != 0;
}
