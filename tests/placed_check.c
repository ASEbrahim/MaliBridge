/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * placed_check: VK_EXT_map_memory_placed as Wine uses it for 32-bit programs (Celeste, x86 .NET/FNA,
 * crashed on a mapping that "does not fit 32-bit pointer"). Reserves an address range, maps an
 * allocation there, writes through it, has the GPU copy it, unmaps keeping the range reserved, and
 * maps again. Exit status 0 only if every check passes.
 */
#define _GNU_SOURCE
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static int failures, checks;

static void check(int ok, const char *what, const char *detail)
{
    checks++;
    if (!ok) failures++;
    printf("%s %s%s%s\n", ok ? "PASS" : "FAIL", what, detail && *detail ? ": " : "", detail ? detail : "");
    fflush(stdout);
}

/* 1 when [p, p+len) is mapped PROT_NONE according to /proc/self/maps. */
static int reserved(void *p, size_t len)
{
    FILE *f = fopen("/proc/self/maps", "r");
    char line[512];
    int ok = 0;
    while (f && fgets(line, sizeof(line), f)) {
        unsigned long a, b;
        char perms[8];
        if (sscanf(line, "%lx-%lx %7s", &a, &b, perms) != 3) continue;
        if (a <= (unsigned long)p && (unsigned long)p + len <= b) ok = !strncmp(perms, "---", 3);
    }
    if (f) fclose(f);
    return ok;
}

int main(void)
{
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
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "placed_check", 1, NULL, 0, VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app};
    if (vkCreateInstance(&ici, NULL, &inst) != VK_SUCCESS) {
        printf("FAIL instance\n");
        return 1;
    }
    GI(vkEnumeratePhysicalDevices);
    GI(vkEnumerateDeviceExtensionProperties);
    GI(vkGetPhysicalDeviceFeatures2);
    GI(vkGetPhysicalDeviceProperties2);
    GI(vkGetPhysicalDeviceMemoryProperties);
    GI(vkCreateDevice);
    GI(vkGetDeviceProcAddr);
    PFN_vkGetDeviceProcAddr gdpa = vkGetDeviceProcAddr;
    uint32_t n = 1;
    VkPhysicalDevice pd;
    vkEnumeratePhysicalDevices(inst, &n, &pd);
    uint32_t ne = 0;
    vkEnumerateDeviceExtensionProperties(pd, NULL, &ne, NULL);
    VkExtensionProperties *ex = calloc(ne, sizeof(*ex));
    vkEnumerateDeviceExtensionProperties(pd, NULL, &ne, ex);
    int have_placed = 0, have_map2 = 0;
    for (uint32_t i = 0; i < ne; i++) {
        have_placed |= !strcmp(ex[i].extensionName, VK_EXT_MAP_MEMORY_PLACED_EXTENSION_NAME);
        have_map2 |= !strcmp(ex[i].extensionName, VK_KHR_MAP_MEMORY_2_EXTENSION_NAME);
    }
    check(have_placed && have_map2, "extensions offered", have_placed ? "" : "no VK_EXT_map_memory_placed");
    if (!have_placed || !have_map2) return 1;

    VkPhysicalDeviceMapMemoryPlacedFeaturesEXT pf = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_FEATURES_EXT};
    VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &pf};
    vkGetPhysicalDeviceFeatures2(pd, &f2);
    VkPhysicalDeviceMapMemoryPlacedPropertiesEXT pp = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_PROPERTIES_EXT};
    VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &pp};
    vkGetPhysicalDeviceProperties2(pd, &p2);
    char d[160];
    snprintf(d, sizeof(d), "placed %u, unmap reserve %u, alignment %llu", pf.memoryMapPlaced, pf.memoryUnmapReserve,
             (unsigned long long)pp.minPlacedMemoryMapAlignment);
    check(pf.memoryMapPlaced && pf.memoryUnmapReserve && pp.minPlacedMemoryMapAlignment, "features", d);

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, 0, 1, &prio};
    const char *exts[] = {VK_KHR_MAP_MEMORY_2_EXTENSION_NAME, VK_EXT_MAP_MEMORY_PLACED_EXTENSION_NAME};
    VkPhysicalDeviceMapMemoryPlacedFeaturesEXT en = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_FEATURES_EXT, NULL, 1, 0, 1};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &en, 0, 1, &qci, 0, NULL, 2, exts};
    VkDevice dev;
    if (vkCreateDevice(pd, &dci, NULL, &dev) != VK_SUCCESS) {
        check(0, "device with the extensions", "");
        return 1;
    }
    GD(vkGetDeviceQueue);
    GD(vkCreateBuffer);
    GD(vkGetBufferMemoryRequirements);
    GD(vkAllocateMemory);
    GD(vkBindBufferMemory);
    GD(vkMapMemory2KHR);
    GD(vkUnmapMemory2KHR);
    GD(vkMapMemory);
    GD(vkCreateCommandPool);
    GD(vkAllocateCommandBuffers);
    GD(vkBeginCommandBuffer);
    GD(vkEndCommandBuffer);
    GD(vkCmdCopyBuffer);
    GD(vkQueueSubmit);
    GD(vkQueueWaitIdle);
    GD(vkFreeMemory);
    check(vkMapMemory2KHR && vkUnmapMemory2KHR, "vkMapMemory2KHR entry points", "");

    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    const VkDeviceSize size = 1 << 20;
    VkBuffer bufs[2];
    VkDeviceMemory mems[2];
    void *plain = NULL;
    for (int i = 0; i < 2; i++) {
        VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, size,
                                  VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        vkCreateBuffer(dev, &bci, NULL, &bufs[i]);
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(dev, bufs[i], &req);
        uint32_t t = UINT32_MAX;
        for (uint32_t k = 0; k < mp.memoryTypeCount && t == UINT32_MAX; k++)
            if ((req.memoryTypeBits & (1u << k)) &&
                (mp.memoryTypes[k].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                    (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
                t = k;
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size, t};
        vkAllocateMemory(dev, &ai, NULL, &mems[i]);
        vkBindBufferMemory(dev, bufs[i], mems[i], 0);
    }
    vkMapMemory(dev, mems[1], 0, VK_WHOLE_SIZE, 0, &plain);

    /* The app's range, as Wine reserves it (low addresses for 32-bit programs, any here). */
    size_t len = (size_t)size;
    void *where = mmap(NULL, len, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    VkMemoryMapPlacedInfoEXT pl = {VK_STRUCTURE_TYPE_MEMORY_MAP_PLACED_INFO_EXT, NULL, where};
    VkMemoryMapInfoKHR mi = {VK_STRUCTURE_TYPE_MEMORY_MAP_INFO_KHR, &pl, VK_MEMORY_MAP_PLACED_BIT_EXT, mems[0], 0, VK_WHOLE_SIZE};
    void *got = NULL;
    VkResult r = vkMapMemory2KHR(dev, &mi, &got);
    snprintf(d, sizeof(d), "result %d, asked %p, got %p", r, where, got);
    check(r == VK_SUCCESS && got == where, "placed map at the reserved address", d);
    if (r != VK_SUCCESS || got != where) return 1;
    for (size_t i = 0; i < len / 4; i++) ((uint32_t *)got)[i] = 0xC0DE0000u + (uint32_t)i;

    /* The GPU sees what was written through the placed mapping. */
    VkQueue q;
    vkGetDeviceQueue(dev, 0, 0, &q);
    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, 0, 0};
    VkCommandPool cp;
    vkCreateCommandPool(dev, &pci, NULL, &cp);
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, cp, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VkCommandBuffer cb;
    vkAllocateCommandBuffers(dev, &cai, &cb);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vkBeginCommandBuffer(cb, &bi);
    VkBufferCopy region = {0, 0, size};
    vkCmdCopyBuffer(cb, bufs[0], bufs[1], 1, &region);
    vkEndCommandBuffer(cb);
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cb};
    vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(q);
    int same = 1;
    for (size_t i = 0; i < len / 4 && same; i++) same = ((uint32_t *)plain)[i] == 0xC0DE0000u + (uint32_t)i;
    check(same, "GPU copy of what the placed mapping wrote", "");

    VkMemoryUnmapInfoKHR ui = {VK_STRUCTURE_TYPE_MEMORY_UNMAP_INFO_KHR, NULL, VK_MEMORY_UNMAP_RESERVE_BIT_EXT, mems[0]};
    vkUnmapMemory2KHR(dev, &ui);
    check(reserved(where, len), "unmap with reserve leaves the range reserved", "");

    got = NULL;
    r = vkMapMemory2KHR(dev, &mi, &got);
    same = r == VK_SUCCESS && got == where;
    for (size_t i = 0; i < len / 4 && same; i++) same = ((uint32_t *)got)[i] == 0xC0DE0000u + (uint32_t)i;
    check(same, "mapped again at the same address, contents kept", "");

    ui.flags = 0;
    vkUnmapMemory2KHR(dev, &ui);
    void *again = NULL;
    r = vkMapMemory(dev, mems[0], 0, VK_WHOLE_SIZE, 0, &again);
    same = r == VK_SUCCESS && again && again != where && ((uint32_t *)again)[7] == 0xC0DE0007u;
    check(same, "plain map after unplacing", "");
    vkFreeMemory(dev, mems[0], NULL);
    printf("%s: %d of %d checks passed\n", failures ? "FAILED" : "OK", checks - failures, checks);
    return failures ? 1 : 0;
}
