/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * importrace_check: the shape of a player's RE crashes (a player log, Mali-G615 r44p1): two programs on one
 * server, a compositor importing dma-bufs while a game makes and frees memory on its own device.
 *   importrace_check compositor N   N times: an exportable dma-buf buffer, its fd exported, imported into a second
 *                                   buffer (dedicated), bound, both freed; counts imports that failed
 *   importrace_check game N         N times: 4 plain allocations, a buffer bound to each, all freed
 *   importrace_check both N         one program, one device: the compositor's rounds on a second thread beside the
 *                                   game's on the first (a device importing while it allocates, as re8's own export
 *                                   imports beside its loads)
 * Prints "imports N ok M failed in T ms" / "allocations N ok M failed in T ms" (both: the two lines); exit 0 only if every
 * call worked.
 */
#define _GNU_SOURCE
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <pthread.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* dma-buf where the driver has it (a phone), else an opaque fd (lavapipe): the server sees an fd import either way */
static VkExternalMemoryHandleTypeFlagBits DMABUF = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

static PFN_vkGetInstanceProcAddr gipa;
static VkInstance inst;
static VkPhysicalDevice pd;
static VkDevice dev;
static VkPhysicalDeviceMemoryProperties mp;
#define I(name) ((PFN_##name)gipa(inst, #name))
#define D(name) ((PFN_##name)gipa(inst, #name))

static int setup(void)
{
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib) return 0;
    gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "importrace_check", 1, NULL, 0, VK_API_VERSION_1_2};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app};
    if (I(vkCreateInstance)(&ici, NULL, &inst) != VK_SUCCESS) return 0;
    uint32_t n = 1;
    I(vkEnumeratePhysicalDevices)(inst, &n, &pd);
    if (!n) return 0;
    I(vkGetPhysicalDeviceMemoryProperties)(pd, &mp);
    float prio = 1.0f;
    const char *exts[] = {"VK_KHR_external_memory_fd", "VK_EXT_external_memory_dma_buf"};
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, 0, 1, &prio};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &qci, 0, NULL, 2, exts, NULL};
    VkPhysicalDeviceExternalBufferInfo eb = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO, NULL, 0,
                                             VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    VkExternalBufferProperties ep = {VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES};
    I(vkGetPhysicalDeviceExternalBufferProperties)(pd, &eb, &ep);
    if (!(ep.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT)) {
        DMABUF = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        dci.enabledExtensionCount = 1;
    }
    printf("handle type 0x%x\n", DMABUF);
    return I(vkCreateDevice)(pd, &dci, NULL, &dev) == VK_SUCCESS;
}

static uint32_t first_type(uint32_t bits)
{
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if (bits & (1u << i)) return i;
    return 0;
}

static VkBuffer ext_buffer(VkDeviceSize size)
{
    VkExternalMemoryBufferCreateInfo x = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO, NULL, DMABUF};
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &x, 0, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                             VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
    VkBuffer b = VK_NULL_HANDLE;
    return D(vkCreateBuffer)(dev, &bi, NULL, &b) == VK_SUCCESS ? b : VK_NULL_HANDLE;
}

/* one export + import round, 0 when every call worked: a LINEAR BGRA image's exportable dedicated memory (the
 * window buffer), its dma-buf imported into a buffer (the compositor's import) */
static int import_round(void)
{
    VkExternalMemoryImageCreateInfo xi = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, NULL, DMABUF};
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &xi, 0, VK_IMAGE_TYPE_2D, VK_FORMAT_B8G8R8A8_UNORM, {320, 240, 1}, 1, 1,
                            VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_LINEAR, VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                            VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
    VkImage img = VK_NULL_HANDLE;
    if (D(vkCreateImage)(dev, &ci, NULL, &img) != VK_SUCCESS) return 1;
    VkBuffer dst = ext_buffer(307200);
    VkMemoryRequirements mr;
    D(vkGetImageMemoryRequirements)(dev, img, &mr);
    VkMemoryDedicatedAllocateInfo ded = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, NULL, img, VK_NULL_HANDLE};
    VkExportMemoryAllocateInfo ex = {VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, &ded, DMABUF};
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &ex, mr.size, first_type(mr.memoryTypeBits)};
    VkDeviceMemory m1 = VK_NULL_HANDLE, m2 = VK_NULL_HANDLE;
    int bad = !dst;
    if (!bad && D(vkAllocateMemory)(dev, &ai, NULL, &m1) != VK_SUCCESS) bad = 1;
    int fd = -1;
    if (!bad) {
        D(vkBindImageMemory)(dev, img, m1, 0);
        VkMemoryGetFdInfoKHR gi = {VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR, NULL, m1, DMABUF};
        if (D(vkGetMemoryFdKHR)(dev, &gi, &fd) != VK_SUCCESS || fd < 0) bad = 1;
    }
    if (!bad) {
        VkMemoryFdPropertiesKHR fp = {VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
        D(vkGetMemoryFdPropertiesKHR)(dev, DMABUF, fd, &fp);
        D(vkGetBufferMemoryRequirements)(dev, dst, &mr);
        VkImportMemoryFdInfoKHR imp = {VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR, NULL, DMABUF, fd};
        VkMemoryAllocateInfo ai2 = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &imp, mr.size,
                                    first_type(mr.memoryTypeBits & (fp.memoryTypeBits ? fp.memoryTypeBits : ~0u))};
        if (D(vkAllocateMemory)(dev, &ai2, NULL, &m2) != VK_SUCCESS) {
            bad = 1;
            close(fd);
        } else if (D(vkBindBufferMemory)(dev, dst, m2, 0) != VK_SUCCESS) {
            bad = 1;
        }
    }
    if (dst) D(vkDestroyBuffer)(dev, dst, NULL);
    D(vkDestroyImage)(dev, img, NULL);
    if (m2) D(vkFreeMemory)(dev, m2, NULL);
    if (m1) D(vkFreeMemory)(dev, m1, NULL);
    return bad;
}

static int game_round(void)
{
    int bad = 0;
    VkDeviceMemory m[4] = {0};
    for (int i = 0; i < 4; i++) {
        VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, 1 << 20, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 0, 0, NULL};
        VkBuffer b = VK_NULL_HANDLE;
        D(vkCreateBuffer)(dev, &bi, NULL, &b);
        VkMemoryRequirements mr;
        D(vkGetBufferMemoryRequirements)(dev, b, &mr);
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, first_type(mr.memoryTypeBits)};
        if (D(vkAllocateMemory)(dev, &ai, NULL, &m[i]) != VK_SUCCESS || D(vkBindBufferMemory)(dev, b, m[i], 0) != VK_SUCCESS) bad = 1;
        D(vkDestroyBuffer)(dev, b, NULL);
    }
    for (int i = 0; i < 4; i++)
        if (m[i]) D(vkFreeMemory)(dev, m[i], NULL);
    return bad;
}

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static int rounds(int comp, int n)
{
    double t0 = now_ms();
    int failed = 0;
    for (int i = 0; i < n; i++) failed += comp ? import_round() : game_round();
    printf("%s %d ok %d failed in %.0f ms\n", comp ? "imports" : "allocations", n - failed, failed, now_ms() - t0);
    fflush(stdout);
    return failed;
}

static int both_n, both_failed;
static void *comp_thread(void *arg)
{
    (void)arg;
    both_failed = rounds(1, both_n);
    return NULL;
}

int main(int argc, char **argv)
{
    const char *role = argc > 1 ? argv[1] : "compositor";
    int n = argc > 2 ? atoi(argv[2]) : 20;
    if (!setup()) {
        printf("FAIL setup\n");
        return 2;
    }
    if (!strcmp(role, "both")) {
        pthread_t th;
        both_n = n;
        if (pthread_create(&th, NULL, comp_thread, NULL)) return 2;
        int failed = rounds(0, n * 3);
        pthread_join(th, NULL);
        return failed || both_failed ? 1 : 0;
    }
    return rounds(!strcmp(role, "compositor"), n) ? 1 : 0;
}
