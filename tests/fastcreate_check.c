/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * fastcreate_check: vkCreateImage / vkCreateBuffer and the process's queued asynchronous work
 * (VKBRIDGE_FAST_CREATE, client/memory.c + server_conn.c). Run by host/fastcreate_test.sh against
 * a server that holds every replayed vkCmdFillBuffer VKBRIDGE_TEST_DELAY_MS (VKBRIDGE_TEST_DELAY_CMD,
 * _REPEAT=1), so a command buffer ended on one thread keeps the queue busy that long.
 *   fastcreate_check timing   thread A ends a held command buffer, thread B then creates: one line
 *                             "T <what> <ms>" per create (RGBA8, BC1, an external-memory image, a
 *                             buffer); then the ordering rounds: images destroyed behind the held
 *                             work, new ones created at once (ahead of those destroys), bound, filled
 *                             and read back - every texel right, no handle the same as a live one.
 *   fastcreate_check reqs     the requirements of many kinds of image, one "REQ ..." line each (v2
 *                             with and without VkMemoryDedicatedRequirements, v1), and 150 BC1
 *                             images created and asked (for the client's round-trip count).
 * One line per check, "OK: ..." / "FAILED: ..." last (the shell compares the T and REQ lines).
 */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static PFN_vkGetInstanceProcAddr gipa;
static VkInstance inst;
static VkPhysicalDevice pd;
static VkDevice dev;
static VkQueue queue;
static uint32_t qfam;
static int failures, checks;

#define I(name) static PFN_##name name
I(vkCreateInstance); I(vkEnumeratePhysicalDevices); I(vkGetPhysicalDeviceProperties); I(vkGetPhysicalDeviceQueueFamilyProperties);
I(vkGetPhysicalDeviceMemoryProperties); I(vkGetPhysicalDeviceFormatProperties); I(vkCreateDevice); I(vkGetDeviceProcAddr);
I(vkDestroyInstance); I(vkGetDeviceQueue); I(vkCreateCommandPool); I(vkDestroyCommandPool); I(vkAllocateCommandBuffers);
I(vkFreeCommandBuffers); I(vkBeginCommandBuffer); I(vkEndCommandBuffer); I(vkQueueSubmit); I(vkCreateFence); I(vkDestroyFence);
I(vkWaitForFences); I(vkDeviceWaitIdle); I(vkDestroyDevice); I(vkCreateImage); I(vkDestroyImage); I(vkGetImageMemoryRequirements);
I(vkGetImageMemoryRequirements2); I(vkCreateBuffer); I(vkDestroyBuffer); I(vkGetBufferMemoryRequirements); I(vkAllocateMemory);
I(vkFreeMemory); I(vkBindImageMemory); I(vkBindBufferMemory); I(vkMapMemory); I(vkUnmapMemory); I(vkCmdFillBuffer);
I(vkCmdPipelineBarrier); I(vkCmdCopyBufferToImage); I(vkCmdCopyImageToBuffer);
#undef I

static void judge(const char *what, int ok, const char *fmt, ...)
{
    char extra[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(extra, sizeof(extra), fmt, ap);
    va_end(ap);
    checks++;
    if (!ok) failures++;
    printf("%s %s: %s\n", ok ? "PASS" : "FAIL", what, extra);
    fflush(stdout);
}

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static VkImageCreateInfo image_ci(VkFormat f, uint32_t w, uint32_t h, uint32_t mips)
{
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = f;
    ci.extent = (VkExtent3D){w, h, 1};
    ci.mipLevels = mips;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    return ci;
}

static uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want)
{
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if (bits & (1u << i)) return i;
    return UINT32_MAX;
}

/* ------------------------------------------------------------------ the held work (thread A) */

static VkCommandPool pool_a, pool_b;
static VkBuffer fill_buf;
static VkDeviceMemory fill_mem;

/* records and ends a command buffer with one vkCmdFillBuffer: its replay is held by the server */
static VkCommandBuffer held_cb(void)
{
    VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool_a, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VkCommandBuffer cb = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(dev, &ai, &cb);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(cb, &bi);
    vkCmdFillBuffer(cb, fill_buf, 0, 256, 0x5a5a5a5au);
    vkEndCommandBuffer(cb); /* asynchronous: the replay (and its hold) runs on the server while we go on */
    return cb;
}

/* thread B: runs one job at a time */
static pthread_mutex_t mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static void (*job)(void *);
static void *job_arg;
static int job_done = 1, quit;

static void *worker(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&mx);
    for (;;) {
        while (job_done && !quit) pthread_cond_wait(&cv, &mx);
        if (quit) break;
        void (*f)(void *) = job;
        void *a = job_arg;
        pthread_mutex_unlock(&mx);
        f(a);
        pthread_mutex_lock(&mx);
        job_done = 1;
        pthread_cond_broadcast(&cv);
    }
    pthread_mutex_unlock(&mx);
    return NULL;
}

static void on_b(void (*f)(void *), void *a)
{
    pthread_mutex_lock(&mx);
    job = f, job_arg = a, job_done = 0;
    pthread_cond_broadcast(&cv);
    while (!job_done) pthread_cond_wait(&cv, &mx);
    pthread_mutex_unlock(&mx);
}

/* ------------------------------------------------------------------ timing: creates right after held work */

typedef struct {
    int kind; /* 0 RGBA8, 1 BC1, 2 external-memory RGBA8, 3 buffer */
    double ms;
    VkResult r;
    VkImage img;
    VkBuffer buf;
} create_job;

static void do_create(void *p)
{
    create_job *j = p;
    double t0 = now_ms();
    if (j->kind == 3) {
        VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, 65536,
                                  VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
        j->r = vkCreateBuffer(dev, &bci, NULL, &j->buf);
    } else {
        VkImageCreateInfo ci = image_ci(j->kind == 1 ? VK_FORMAT_BC1_RGBA_UNORM_BLOCK : VK_FORMAT_R8G8B8A8_UNORM, 256, 256, 9);
        VkExternalMemoryImageCreateInfo x = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, NULL, VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT};
        if (j->kind == 2) ci.pNext = &x;
        j->r = vkCreateImage(dev, &ci, NULL, &j->img);
    }
    j->ms = now_ms() - t0;
}

static void timing(void)
{
    static const char *const names[] = {"rgba8", "bc1", "external", "buffer"};
    for (int k = 0; k < 4; k++) {
        VkCommandBuffer cb = held_cb();
        create_job j = {k, 0, VK_ERROR_UNKNOWN, VK_NULL_HANDLE, VK_NULL_HANDLE};
        on_b(do_create, &j);
        printf("T %s %.1f result %d\n", names[k], j.ms, j.r);
        fflush(stdout);
        vkDeviceWaitIdle(dev); /* the hold is over */
        if (j.img) vkDestroyImage(dev, j.img, NULL);
        if (j.buf) vkDestroyBuffer(dev, j.buf, NULL);
        vkFreeCommandBuffers(dev, pool_a, 1, &cb);
    }
}

/* ------------------------------------------------------------------ ordering rounds */

#define OW 64
#define OH 64

typedef struct {
    VkImage live[4];   /* images alive (their destroys queued behind the held work, not run yet) */
    int nlive;
    VkImage img;
    VkDeviceMemory mem;
    VkResult r;
    int same_handle;
    double ms;
} round_job;

static void do_round_create(void *p)
{
    round_job *j = p;
    VkImageCreateInfo ci = image_ci(VK_FORMAT_R8G8B8A8_UNORM, OW, OH, 1);
    double t0 = now_ms();
    j->r = vkCreateImage(dev, &ci, NULL, &j->img);
    j->ms = now_ms() - t0;
    /* made ahead of the queued destroys (at once), its handle cannot be one of theirs: those images are still alive in the driver
     * (made after them, the driver may hand one out again) */
    for (int i = 0; i < j->nlive && j->ms < 150; i++) j->same_handle |= j->img == j->live[i];
    /* the requirements (answered from the create's reply when it carried them), memory, bind */
    VkMemoryDedicatedRequirements ded = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
    VkMemoryRequirements2 r2 = {VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, &ded};
    VkImageMemoryRequirementsInfo2 ri = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2, NULL, j->img};
    vkGetImageMemoryRequirements2(dev, &ri, &r2);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, r2.memoryRequirements.size,
                               mem_type(r2.memoryRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    if (vkAllocateMemory(dev, &ai, NULL, &j->mem) == VK_SUCCESS) vkBindImageMemory(dev, j->img, j->mem, 0);
}

static void ordering(void)
{
    /* a host-visible buffer: the texels in, the texels back */
    const VkDeviceSize bytes = OW * OH * 4;
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, bytes * 2,
                              VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
    VkBuffer io;
    vkCreateBuffer(dev, &bci, NULL, &io);
    VkMemoryRequirements br;
    vkGetBufferMemoryRequirements(dev, io, &br);
    VkMemoryAllocateInfo bai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, br.size,
                                mem_type(br.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    VkDeviceMemory iom;
    vkAllocateMemory(dev, &bai, NULL, &iom);
    vkBindBufferMemory(dev, io, iom, 0);
    uint32_t *map = NULL;
    vkMapMemory(dev, iom, 0, VK_WHOLE_SIZE, 0, (void **)&map);
    VkImage prev[4] = {0};
    VkDeviceMemory prevm[4] = {0};
    int nprev = 0, wrong = 0, same = 0, failed = 0;
    double worst = 0;
    for (int round = 0; round < 6; round++) {
        /* the images of the last round go, behind a held command buffer: their destroys wait in the queue */
        VkCommandBuffer cb = held_cb();
        round_job j = {{0}, 0, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_ERROR_UNKNOWN, 0, 0};
        for (int i = 0; i < nprev; i++) {
            j.live[j.nlive++] = prev[i];
            vkDestroyImage(dev, prev[i], NULL);
            vkFreeMemory(dev, prevm[i], NULL);
        }
        nprev = 0;
        on_b(do_round_create, &j);
        if (j.ms > worst) worst = j.ms;
        same += j.same_handle;
        if (j.r != VK_SUCCESS || !j.mem) {
            failed++;
            vkDeviceWaitIdle(dev);
            vkFreeCommandBuffers(dev, pool_a, 1, &cb);
            continue;
        }
        /* texels in through the new image and back: the round's own pattern */
        for (uint32_t i = 0; i < OW * OH; i++) map[i] = 0x01000000u * (uint32_t)round + i * 2654435761u;
        memset(map + OW * OH, 0, bytes);
        VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool_a, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
        VkCommandBuffer xb;
        vkAllocateCommandBuffers(dev, &ai, &xb);
        VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        vkBeginCommandBuffer(xb, &bi);
        VkImageMemoryBarrier b1 = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, j.img,
                                   {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        vkCmdPipelineBarrier(xb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b1);
        VkBufferImageCopy cp = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {OW, OH, 1}};
        vkCmdCopyBufferToImage(xb, io, j.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
        VkImageMemoryBarrier b2 = b1;
        b2.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, b2.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        b2.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, b2.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        vkCmdPipelineBarrier(xb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b2);
        VkBufferImageCopy back = cp;
        back.bufferOffset = bytes;
        vkCmdCopyImageToBuffer(xb, j.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, io, 1, &back);
        vkEndCommandBuffer(xb);
        VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VkFence f;
        vkCreateFence(dev, &fci, NULL, &f);
        VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &xb, 0, NULL};
        vkQueueSubmit(queue, 1, &si, f);
        vkWaitForFences(dev, 1, &f, VK_TRUE, 10000000000ull);
        vkDestroyFence(dev, f, NULL);
        for (uint32_t i = 0; i < OW * OH; i++)
            if (map[OW * OH + i] != 0x01000000u * (uint32_t)round + i * 2654435761u) {
                wrong++;
                break;
            }
        vkFreeCommandBuffers(dev, pool_a, 1, &xb);
        vkFreeCommandBuffers(dev, pool_a, 1, &cb);
        prev[nprev] = j.img, prevm[nprev++] = j.mem;
    }
    vkDeviceWaitIdle(dev);
    for (int i = 0; i < nprev; i++) vkDestroyImage(dev, prev[i], NULL), vkFreeMemory(dev, prevm[i], NULL);
    vkUnmapMemory(dev, iom);
    vkDestroyBuffer(dev, io, NULL);
    vkFreeMemory(dev, iom, NULL);
    printf("T order-worst %.1f\n", worst);
    judge("6 rounds: images created while the last round's destroys wait behind held work, filled and read back", !wrong && !same && !failed,
          "%d rounds read back wrong, %d new handles equal to a live one, %d creates or allocations failed; slowest create %.1f ms", wrong,
          same, failed, worst);
}

/* ------------------------------------------------------------------ requirements */

typedef struct {
    const char *name;
    VkImageCreateInfo ci;
    VkImageFormatListCreateInfo fl;
    VkFormat views[2];
} req_case;

static void print_req(const char *name, const VkImageCreateInfo *ci)
{
    VkImage img = VK_NULL_HANDLE;
    VkResult r = vkCreateImage(dev, ci, NULL, &img);
    if (r != VK_SUCCESS) {
        printf("REQ %s refused %d\n", name, r);
        return;
    }
    VkMemoryDedicatedRequirements ded = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
    VkMemoryRequirements2 r2 = {VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, &ded};
    VkImageMemoryRequirementsInfo2 ri = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2, NULL, img};
    vkGetImageMemoryRequirements2(dev, &ri, &r2);
    VkMemoryRequirements2 p2 = {VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, NULL};
    vkGetImageMemoryRequirements2(dev, &ri, &p2);
    VkMemoryRequirements r1;
    vkGetImageMemoryRequirements(dev, img, &r1);
    printf("REQ %s v2 %llu %llu 0x%x ded %u %u plain %llu %llu 0x%x v1 %llu %llu 0x%x\n", name, (unsigned long long)r2.memoryRequirements.size,
           (unsigned long long)r2.memoryRequirements.alignment, r2.memoryRequirements.memoryTypeBits, ded.prefersDedicatedAllocation,
           ded.requiresDedicatedAllocation, (unsigned long long)p2.memoryRequirements.size, (unsigned long long)p2.memoryRequirements.alignment,
           p2.memoryRequirements.memoryTypeBits, (unsigned long long)r1.size, (unsigned long long)r1.alignment, r1.memoryTypeBits);
    judge(name, r2.memoryRequirements.size && !memcmp(&r1, &r2.memoryRequirements, sizeof(r1)) &&
          !memcmp(&p2.memoryRequirements, &r2.memoryRequirements, sizeof(r1)), "v1, v2 and v2 with the dedicated struct agree");
    vkDestroyImage(dev, img, NULL);
}

static void reqs(void)
{
    VkImageCreateInfo c;
    print_req("rgba8-64", (c = image_ci(VK_FORMAT_R8G8B8A8_UNORM, 64, 64, 1), &c));
    print_req("rgba8-256-mips", (c = image_ci(VK_FORMAT_R8G8B8A8_UNORM, 256, 256, 9), &c));
    print_req("rgba8-1000x600", (c = image_ci(VK_FORMAT_R8G8B8A8_UNORM, 1000, 600, 1), &c));
    {
        VkFormat views[2] = {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_SRGB};
        VkImageFormatListCreateInfo fl = {VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO, NULL, 2, views};
        c = image_ci(VK_FORMAT_B8G8R8A8_SRGB, 512, 512, 10);
        c.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
        c.pNext = &fl;
        print_req("bgra8-mutable-list", &c);
    }
    c = image_ci(VK_FORMAT_R16G16B16A16_SFLOAT, 320, 200, 1);
    c.usage |= VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    print_req("rgba16f-storage-rt", &c);
    c = image_ci(VK_FORMAT_D32_SFLOAT, 1280, 720, 1);
    c.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    print_req("d32-depth", &c);
    c = image_ci(VK_FORMAT_R8G8B8A8_UNORM, 128, 128, 1);
    c.tiling = VK_IMAGE_TILING_LINEAR;
    c.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    print_req("rgba8-linear", &c);
    c = image_ci(VK_FORMAT_R8G8B8A8_UNORM, 32, 32, 1);
    c.imageType = VK_IMAGE_TYPE_3D;
    c.extent.depth = 32;
    print_req("rgba8-3d", &c);
    print_req("bc1-256-mips", (c = image_ci(VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 256, 256, 9), &c));
    print_req("bc3-256-mips", (c = image_ci(VK_FORMAT_BC3_UNORM_BLOCK, 256, 256, 9), &c));
    print_req("bc7-256-mips", (c = image_ci(VK_FORMAT_BC7_UNORM_BLOCK, 256, 256, 9), &c));
    print_req("bc7srgb-1024-mips", (c = image_ci(VK_FORMAT_BC7_SRGB_BLOCK, 1024, 1024, 11), &c));
    c = image_ci(VK_FORMAT_BC3_SRGB_BLOCK, 128, 128, 8);
    c.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    c.arrayLayers = 6;
    print_req("bc3-cube", &c);
    {
        VkFormat views[2] = {VK_FORMAT_BC1_RGBA_UNORM_BLOCK, VK_FORMAT_BC1_RGBA_SRGB_BLOCK};
        VkImageFormatListCreateInfo fl = {VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO, NULL, 2, views};
        c = image_ci(VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 512, 256, 10);
        c.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
        c.pNext = &fl;
        print_req("bc1-mutable-list", &c);
    }
    /* 150 BC1 textures, created and asked once each (BC answers are never kept by create info: each a round trip without
     * the create's reply) */
    int bad = 0;
    VkMemoryRequirements first = {0};
    for (int i = 0; i < 150; i++) {
        c = image_ci(VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 64, 64, 7);
        VkImage img;
        if (vkCreateImage(dev, &c, NULL, &img)) {
            bad++;
            continue;
        }
        VkMemoryDedicatedRequirements ded = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
        VkMemoryRequirements2 r2 = {VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, &ded};
        VkImageMemoryRequirementsInfo2 ri = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2, NULL, img};
        vkGetImageMemoryRequirements2(dev, &ri, &r2);
        if (!i) first = r2.memoryRequirements;
        else if (memcmp(&first, &r2.memoryRequirements, sizeof(first))) bad++;
        vkDestroyImage(dev, img, NULL);
    }
    printf("REQ bc1-64-x150 %llu %llu 0x%x\n", (unsigned long long)first.size, (unsigned long long)first.alignment, first.memoryTypeBits);
    judge("150 BC1 64x64 textures", !bad && first.size, "%d failed or answered differently", bad);
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "timing";
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib) {
        printf("FAILED: loader: %s\n", dlerror());
        return 2;
    }
    gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
#define G(n) n = (PFN_##n)gipa(inst, #n)
    G(vkCreateInstance);
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "vkbridge-fastcreate-check", 1, "vkbridge", 1, VK_API_VERSION_1_1};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 0, NULL};
    if (vkCreateInstance(&ici, NULL, &inst)) {
        printf("FAILED: vkCreateInstance\n");
        return 1;
    }
    G(vkEnumeratePhysicalDevices); G(vkGetPhysicalDeviceProperties); G(vkGetPhysicalDeviceQueueFamilyProperties);
    G(vkGetPhysicalDeviceMemoryProperties); G(vkGetPhysicalDeviceFormatProperties); G(vkCreateDevice); G(vkGetDeviceProcAddr);
    G(vkDestroyInstance);
#undef G
    uint32_t n = 1;
    if (vkEnumeratePhysicalDevices(inst, &n, &pd) < 0 || !n) {
        printf("FAILED: no physical device\n");
        return 1;
    }
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(pd, &props);
    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, NULL);
    VkQueueFamilyProperties qp[16];
    if (nq > 16) nq = 16;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qp);
    for (qfam = 0; qfam < nq; qfam++)
        if (qp[qfam].queueFlags & VK_QUEUE_GRAPHICS_BIT) break;
    if (qfam == nq) {
        printf("FAILED: %s: no graphics queue\n", props.deviceName);
        return 1;
    }
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, qfam, 1, &prio};
    VkPhysicalDeviceFeatures feat = {0};
    feat.textureCompressionBC = VK_TRUE;
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &qci, 0, NULL, 0, NULL, &feat};
    if (vkCreateDevice(pd, &dci, NULL, &dev)) {
        printf("FAILED: vkCreateDevice\n");
        return 1;
    }
#define G(n) n = (PFN_##n)vkGetDeviceProcAddr(dev, #n)
    G(vkGetDeviceQueue); G(vkCreateCommandPool); G(vkDestroyCommandPool); G(vkAllocateCommandBuffers); G(vkFreeCommandBuffers);
    G(vkBeginCommandBuffer); G(vkEndCommandBuffer); G(vkQueueSubmit); G(vkCreateFence); G(vkDestroyFence); G(vkWaitForFences);
    G(vkDeviceWaitIdle); G(vkDestroyDevice); G(vkCreateImage); G(vkDestroyImage); G(vkGetImageMemoryRequirements);
    G(vkGetImageMemoryRequirements2); G(vkCreateBuffer); G(vkDestroyBuffer); G(vkGetBufferMemoryRequirements); G(vkAllocateMemory);
    G(vkFreeMemory); G(vkBindImageMemory); G(vkBindBufferMemory); G(vkMapMemory); G(vkUnmapMemory); G(vkCmdFillBuffer);
    G(vkCmdPipelineBarrier); G(vkCmdCopyBufferToImage); G(vkCmdCopyImageToBuffer);
#undef G
    vkGetDeviceQueue(dev, qfam, 0, &queue);
    printf("device: %s\n", props.deviceName);
    if (!strcmp(mode, "reqs")) {
        reqs();
    } else {
        VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, qfam};
        vkCreateCommandPool(dev, &pci, NULL, &pool_a);
        vkCreateCommandPool(dev, &pci, NULL, &pool_b);
        VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, 4096, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
        vkCreateBuffer(dev, &bci, NULL, &fill_buf);
        VkMemoryRequirements br;
        vkGetBufferMemoryRequirements(dev, fill_buf, &br);
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, br.size, mem_type(br.memoryTypeBits, 0)};
        vkAllocateMemory(dev, &ai, NULL, &fill_mem);
        vkBindBufferMemory(dev, fill_buf, fill_mem, 0);
        pthread_t t;
        pthread_create(&t, NULL, worker, NULL);
        /* thread B's connection and first round trips before any hold */
        create_job warm = {0, 0, VK_ERROR_UNKNOWN, VK_NULL_HANDLE, VK_NULL_HANDLE};
        on_b(do_create, &warm);
        vkDeviceWaitIdle(dev);
        if (warm.img) vkDestroyImage(dev, warm.img, NULL);
        timing();
        ordering();
        pthread_mutex_lock(&mx);
        quit = 1;
        pthread_cond_broadcast(&cv);
        pthread_mutex_unlock(&mx);
        pthread_join(t, NULL);
        vkDeviceWaitIdle(dev);
        vkDestroyBuffer(dev, fill_buf, NULL);
        vkFreeMemory(dev, fill_mem, NULL);
        vkDestroyCommandPool(dev, pool_a, NULL);
        vkDestroyCommandPool(dev, pool_b, NULL);
    }
    vkDestroyDevice(dev, NULL);
    vkDestroyInstance(inst, NULL);
    if (failures) printf("FAILED: %d of %d check(s)\n", failures, checks);
    else printf("OK: %d of %d checks passed\n", checks, checks);
    return failures ? 1 : 0;
}
