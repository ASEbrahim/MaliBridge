/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * flicker_check [frames] [draws]: descriptor traffic with DXVK's frame structure, which
 * desc_traffic_check (one frame at a time, one thread) does not have. Several frames in flight,
 * each with its own descriptor pool reset when its fence says the GPU is done and its sets
 * allocated again (the driver hands the same set handles back); command buffers recorded on one
 * thread and submitted from another; a third thread making synchronous calls all along; per draw
 * sets from the frame's pool (client-named), from a pool with FREE_DESCRIPTOR_SET (driver-named,
 * freed and reused), written by vkUpdateDescriptorSets (packed with VKBRIDGE_DESC_BATCH=1), by a
 * template, or by two calls with synchronous calls in between. Every draw's shader reads all its
 * descriptors back; a value from another draw or frame is what a game shows as a flickering frame.
 *
 *   make -C host; . host/mali_env.sh; VKBRIDGE_DESC_BATCH=1 host/out/flicker_check
 */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* glslangValidator -V of:
 *   #version 450
 *   layout(local_size_x = 1) in;
 *   layout(std140, binding = 0) uniform U { uvec4 u; };
 *   layout(binding = 1) uniform usamplerBuffer tb;
 *   layout(binding = 2) uniform usampler2D cis;
 *   layout(binding = 3) uniform utexture2D sep[2];
 *   layout(binding = 4) uniform sampler smp;
 *   layout(binding = 5, r32ui) uniform readonly uimage2D sto;
 *   layout(std430, binding = 6) writeonly buffer O { uint o[8]; };
 *   void main() {
 *       o[0] = u.x; o[1] = texelFetch(tb, 0).x; o[2] = texelFetch(cis, ivec2(0), 0).x;
 *       o[3] = texelFetch(usampler2D(sep[0], smp), ivec2(0), 0).x;
 *       o[4] = texelFetch(usampler2D(sep[1], smp), ivec2(0), 0).x;
 *       o[5] = imageLoad(sto, ivec2(0)).x; o[6] = 0x600du; o[7] = 0u;
 *   } */
static const uint32_t spv_desc_comp[] = {
    0x07230203,0x00010000,0x0008000b,0x00000057,
    0x00000000,0x00020011,0x00000001,0x00020011,
    0x0000002e,0x0006000b,0x00000001,0x4c534c47,
    0x6474732e,0x3035342e,0x00000000,0x0003000e,
    0x00000000,0x00000001,0x0005000f,0x00000005,
    0x00000004,0x6e69616d,0x00000000,0x00060010,
    0x00000004,0x00000011,0x00000001,0x00000001,
    0x00000001,0x00030003,0x00000002,0x000001c2,
    0x00040005,0x00000004,0x6e69616d,0x00000000,
    0x00030005,0x00000009,0x0000004f,0x00040006,
    0x00000009,0x00000000,0x0000006f,0x00030005,
    0x0000000b,0x00000000,0x00030005,0x0000000f,
    0x00000055,0x00040006,0x0000000f,0x00000000,
    0x00000075,0x00030005,0x00000011,0x00000000,
    0x00030005,0x0000001b,0x00006274,0x00030005,
    0x00000025,0x00736963,0x00030005,0x00000031,
    0x00706573,0x00030005,0x00000037,0x00706d73,
    0x00030005,0x0000004a,0x006f7473,0x00040047,
    0x00000008,0x00000006,0x00000004,0x00030047,
    0x00000009,0x00000003,0x00040048,0x00000009,
    0x00000000,0x00000019,0x00050048,0x00000009,
    0x00000000,0x00000023,0x00000000,0x00030047,
    0x0000000b,0x00000019,0x00040047,0x0000000b,
    0x00000021,0x00000006,0x00040047,0x0000000b,
    0x00000022,0x00000000,0x00030047,0x0000000f,
    0x00000002,0x00050048,0x0000000f,0x00000000,
    0x00000023,0x00000000,0x00040047,0x00000011,
    0x00000021,0x00000000,0x00040047,0x00000011,
    0x00000022,0x00000000,0x00040047,0x0000001b,
    0x00000021,0x00000001,0x00040047,0x0000001b,
    0x00000022,0x00000000,0x00040047,0x00000025,
    0x00000021,0x00000002,0x00040047,0x00000025,
    0x00000022,0x00000000,0x00040047,0x00000031,
    0x00000021,0x00000003,0x00040047,0x00000031,
    0x00000022,0x00000000,0x00040047,0x00000037,
    0x00000021,0x00000004,0x00040047,0x00000037,
    0x00000022,0x00000000,0x00030047,0x0000004a,
    0x00000018,0x00040047,0x0000004a,0x00000021,
    0x00000005,0x00040047,0x0000004a,0x00000022,
    0x00000000,0x00040047,0x00000056,0x0000000b,
    0x00000019,0x00020013,0x00000002,0x00030021,
    0x00000003,0x00000002,0x00040015,0x00000006,
    0x00000020,0x00000000,0x0004002b,0x00000006,
    0x00000007,0x00000008,0x0004001c,0x00000008,
    0x00000006,0x00000007,0x0003001e,0x00000009,
    0x00000008,0x00040020,0x0000000a,0x00000002,
    0x00000009,0x0004003b,0x0000000a,0x0000000b,
    0x00000002,0x00040015,0x0000000c,0x00000020,
    0x00000001,0x0004002b,0x0000000c,0x0000000d,
    0x00000000,0x00040017,0x0000000e,0x00000006,
    0x00000004,0x0003001e,0x0000000f,0x0000000e,
    0x00040020,0x00000010,0x00000002,0x0000000f,
    0x0004003b,0x00000010,0x00000011,0x00000002,
    0x0004002b,0x00000006,0x00000012,0x00000000,
    0x00040020,0x00000013,0x00000002,0x00000006,
    0x0004002b,0x0000000c,0x00000017,0x00000001,
    0x00090019,0x00000018,0x00000006,0x00000005,
    0x00000000,0x00000000,0x00000000,0x00000001,
    0x00000000,0x0003001b,0x00000019,0x00000018,
    0x00040020,0x0000001a,0x00000000,0x00000019,
    0x0004003b,0x0000001a,0x0000001b,0x00000000,
    0x0004002b,0x0000000c,0x00000021,0x00000002,
    0x00090019,0x00000022,0x00000006,0x00000001,
    0x00000000,0x00000000,0x00000000,0x00000001,
    0x00000000,0x0003001b,0x00000023,0x00000022,
    0x00040020,0x00000024,0x00000000,0x00000023,
    0x0004003b,0x00000024,0x00000025,0x00000000,
    0x00040017,0x00000027,0x0000000c,0x00000002,
    0x0005002c,0x00000027,0x00000028,0x0000000d,
    0x0000000d,0x0004002b,0x0000000c,0x0000002d,
    0x00000003,0x0004002b,0x00000006,0x0000002e,
    0x00000002,0x0004001c,0x0000002f,0x00000022,
    0x0000002e,0x00040020,0x00000030,0x00000000,
    0x0000002f,0x0004003b,0x00000030,0x00000031,
    0x00000000,0x00040020,0x00000032,0x00000000,
    0x00000022,0x0002001a,0x00000035,0x00040020,
    0x00000036,0x00000000,0x00000035,0x0004003b,
    0x00000036,0x00000037,0x00000000,0x0004002b,
    0x0000000c,0x0000003e,0x00000004,0x0004002b,
    0x0000000c,0x00000047,0x00000005,0x00090019,
    0x00000048,0x00000006,0x00000001,0x00000000,
    0x00000000,0x00000000,0x00000002,0x00000021,
    0x00040020,0x00000049,0x00000000,0x00000048,
    0x0004003b,0x00000049,0x0000004a,0x00000000,
    0x0004002b,0x0000000c,0x0000004f,0x00000006,
    0x0004002b,0x00000006,0x00000050,0x0000600d,
    0x0004002b,0x0000000c,0x00000052,0x00000007,
    0x00040017,0x00000054,0x00000006,0x00000003,
    0x0004002b,0x00000006,0x00000055,0x00000001,
    0x0006002c,0x00000054,0x00000056,0x00000055,
    0x00000055,0x00000055,0x00050036,0x00000002,
    0x00000004,0x00000000,0x00000003,0x000200f8,
    0x00000005,0x00060041,0x00000013,0x00000014,
    0x00000011,0x0000000d,0x00000012,0x0004003d,
    0x00000006,0x00000015,0x00000014,0x00060041,
    0x00000013,0x00000016,0x0000000b,0x0000000d,
    0x0000000d,0x0003003e,0x00000016,0x00000015,
    0x0004003d,0x00000019,0x0000001c,0x0000001b,
    0x00040064,0x00000018,0x0000001d,0x0000001c,
    0x0005005f,0x0000000e,0x0000001e,0x0000001d,
    0x0000000d,0x00050051,0x00000006,0x0000001f,
    0x0000001e,0x00000000,0x00060041,0x00000013,
    0x00000020,0x0000000b,0x0000000d,0x00000017,
    0x0003003e,0x00000020,0x0000001f,0x0004003d,
    0x00000023,0x00000026,0x00000025,0x00040064,
    0x00000022,0x00000029,0x00000026,0x0007005f,
    0x0000000e,0x0000002a,0x00000029,0x00000028,
    0x00000002,0x0000000d,0x00050051,0x00000006,
    0x0000002b,0x0000002a,0x00000000,0x00060041,
    0x00000013,0x0000002c,0x0000000b,0x0000000d,
    0x00000021,0x0003003e,0x0000002c,0x0000002b,
    0x00050041,0x00000032,0x00000033,0x00000031,
    0x0000000d,0x0004003d,0x00000022,0x00000034,
    0x00000033,0x0004003d,0x00000035,0x00000038,
    0x00000037,0x00050056,0x00000023,0x00000039,
    0x00000034,0x00000038,0x00040064,0x00000022,
    0x0000003a,0x00000039,0x0007005f,0x0000000e,
    0x0000003b,0x0000003a,0x00000028,0x00000002,
    0x0000000d,0x00050051,0x00000006,0x0000003c,
    0x0000003b,0x00000000,0x00060041,0x00000013,
    0x0000003d,0x0000000b,0x0000000d,0x0000002d,
    0x0003003e,0x0000003d,0x0000003c,0x00050041,
    0x00000032,0x0000003f,0x00000031,0x00000017,
    0x0004003d,0x00000022,0x00000040,0x0000003f,
    0x0004003d,0x00000035,0x00000041,0x00000037,
    0x00050056,0x00000023,0x00000042,0x00000040,
    0x00000041,0x00040064,0x00000022,0x00000043,
    0x00000042,0x0007005f,0x0000000e,0x00000044,
    0x00000043,0x00000028,0x00000002,0x0000000d,
    0x00050051,0x00000006,0x00000045,0x00000044,
    0x00000000,0x00060041,0x00000013,0x00000046,
    0x0000000b,0x0000000d,0x0000003e,0x0003003e,
    0x00000046,0x00000045,0x0004003d,0x00000048,
    0x0000004b,0x0000004a,0x00050062,0x0000000e,
    0x0000004c,0x0000004b,0x00000028,0x00050051,
    0x00000006,0x0000004d,0x0000004c,0x00000000,
    0x00060041,0x00000013,0x0000004e,0x0000000b,
    0x0000000d,0x00000047,0x0003003e,0x0000004e,
    0x0000004d,0x00060041,0x00000013,0x00000051,
    0x0000000b,0x0000000d,0x0000004f,0x0003003e,
    0x00000051,0x00000050,0x00060041,0x00000013,
    0x00000053,0x0000000b,0x0000000d,0x00000052,
    0x0003003e,0x00000053,0x00000012,0x000100fd,
    0x00010038
};

#define K 4     /* distinct values per resource kind, chosen per draw */
#define UBO_N 64 /* uniform buffer slots */

static VkDevice dev;
static VkQueue queue;
static VkPhysicalDeviceMemoryProperties mp;
static int failures, checks;

#define I(name) static PFN_##name name
I(vkCreateInstance); I(vkEnumeratePhysicalDevices); I(vkGetPhysicalDeviceProperties); I(vkGetPhysicalDeviceQueueFamilyProperties);
I(vkGetPhysicalDeviceMemoryProperties); I(vkCreateDevice); I(vkGetDeviceProcAddr); I(vkDestroyInstance); I(vkDestroyDevice);
I(vkGetDeviceQueue); I(vkCreateCommandPool); I(vkDestroyCommandPool); I(vkAllocateCommandBuffers); I(vkBeginCommandBuffer);
I(vkEndCommandBuffer); I(vkQueueSubmit); I(vkCreateBuffer); I(vkDestroyBuffer); I(vkGetBufferMemoryRequirements);
I(vkAllocateMemory); I(vkFreeMemory); I(vkBindBufferMemory); I(vkMapMemory); I(vkCmdPipelineBarrier); I(vkCreateShaderModule);
I(vkCreateDescriptorSetLayout); I(vkCreatePipelineLayout); I(vkCreateComputePipelines); I(vkCreateDescriptorPool);
I(vkResetDescriptorPool); I(vkAllocateDescriptorSets); I(vkUpdateDescriptorSets); I(vkCmdBindPipeline); I(vkCmdBindDescriptorSets);
I(vkCmdDispatch); I(vkCreateFence); I(vkWaitForFences); I(vkResetFences); I(vkCreateBufferView); I(vkFlushMappedMemoryRanges);
I(vkInvalidateMappedMemoryRanges); I(vkCreateImage); I(vkGetImageMemoryRequirements); I(vkBindImageMemory); I(vkCreateImageView);
I(vkCreateSampler); I(vkCmdClearColorImage); I(vkResetCommandBuffer);
#undef I

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

static uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want)
{
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

typedef struct {
    VkBuffer buf;
    VkDeviceMemory mem;
    uint32_t *ptr;
    int coherent;
} buffer;

static int make_buffer(buffer *b, VkDeviceSize size, VkBufferUsageFlags usage)
{
    VkBufferCreateInfo ci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, size, usage, VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
    if (vkCreateBuffer(dev, &ci, NULL, &b->buf)) return 0;
    VkMemoryRequirements r;
    vkGetBufferMemoryRequirements(dev, b->buf, &r);
    uint32_t t = mem_type(r.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    if (t == UINT32_MAX) return 0;
    b->coherent = (mp.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, r.size, t};
    if (vkAllocateMemory(dev, &ai, NULL, &b->mem) || vkBindBufferMemory(dev, b->buf, b->mem, 0)) return 0;
    return !vkMapMemory(dev, b->mem, 0, VK_WHOLE_SIZE, 0, (void **)&b->ptr);
}

static void range_sync(buffer *b, int to_gpu)
{
    if (b->coherent) return;
    VkMappedMemoryRange r = {VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, NULL, b->mem, 0, VK_WHOLE_SIZE};
    if (to_gpu) vkFlushMappedMemoryRanges(dev, 1, &r);
    else vkInvalidateMappedMemoryRanges(dev, 1, &r);
}

/* A 1x1 R32_UINT image holding `value`, in GENERAL layout, and its view. */
static int make_image(VkCommandBuffer cb, uint32_t value, VkImageView *view)
{
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, VK_FORMAT_R32_UINT, {1, 1, 1}, 1, 1,
                            VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL,
                            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                            VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
    VkImage img;
    if (vkCreateImage(dev, &ci, NULL, &img)) return 0;
    VkMemoryRequirements r;
    vkGetImageMemoryRequirements(dev, img, &r);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, r.size, mem_type(r.memoryTypeBits, 0)};
    VkDeviceMemory mem;
    if (vkAllocateMemory(dev, &ai, NULL, &mem) || vkBindImageMemory(dev, img, mem, 0)) return 0;
    VkImageSubresourceRange sr = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                              VK_IMAGE_LAYOUT_GENERAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, img, sr};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    VkClearColorValue c = {.uint32 = {value, 0, 0, 0}};
    vkCmdClearColorImage(cb, img, VK_IMAGE_LAYOUT_GENERAL, &c, 1, &sr);
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, img, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R32_UINT,
                                 {0}, sr};
    return !vkCreateImageView(dev, &vci, NULL, view);
}

static int submit_wait(VkCommandBuffer cb, VkFence f)
{
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cb, 0, NULL};
    int ok = !vkQueueSubmit(queue, 1, &si, f) && !vkWaitForFences(dev, 1, &f, VK_TRUE, 10000000000ull);
    vkResetFences(dev, 1, &f);
    return ok;
}


#include <pthread.h>
#include <stddef.h>

#define I(name) static PFN_##name name
I(vkFreeDescriptorSets); I(vkCreateDescriptorUpdateTemplate); I(vkUpdateDescriptorSetWithTemplate); I(vkGetFenceStatus);
I(vkDestroyImageView); I(vkDestroySampler); I(vkDestroyFence);
#undef I

#define FRAMES_IN_FLIGHT 3

typedef struct {
    VkCommandBuffer cb;
    VkFence fence;
    VkDescriptorPool pool;            /* reset per frame, client-named sets */
    VkDescriptorSet freeable[1024];   /* from the shared FREE_DESCRIPTOR_SET pool, freed when done */
    uint32_t nfree;
    int frame;                        /* the frame recorded in it, -1 none */
    int submitted;
} slot;

/* The submit thread: takes recorded slots in order and submits them. */
static pthread_mutex_t sq_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t sq_cond = PTHREAD_COND_INITIALIZER;
static slot *sq[64];
static unsigned sq_head, sq_tail;
static volatile int stop_all;

static void *submit_thread(void *arg)
{
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&sq_lock);
        while (sq_head == sq_tail && !stop_all) pthread_cond_wait(&sq_cond, &sq_lock);
        if (sq_head == sq_tail) {
            pthread_mutex_unlock(&sq_lock);
            return NULL;
        }
        slot *s = sq[sq_head % 64];
        pthread_mutex_unlock(&sq_lock);
        VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &s->cb, 0, NULL};
        if (vkQueueSubmit(queue, 1, &si, s->fence)) printf("FAIL submit\n");
        pthread_mutex_lock(&sq_lock);
        s->submitted = 1;
        sq_head++;
        pthread_cond_broadcast(&sq_cond);
        pthread_mutex_unlock(&sq_lock);
    }
}

/* Synchronous calls from another thread the whole time (DXVK's own threads, the game's). */
static VkFence busy_fence;
static void *busy_thread(void *arg)
{
    (void)arg;
    VkSamplerCreateInfo sci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    while (!stop_all) {
        VkSampler s;
        if (!vkCreateSampler(dev, &sci, NULL, &s)) vkDestroySampler(dev, s, NULL);
        vkGetFenceStatus(dev, busy_fence);
        struct timespec ts = {0, 300000};
        nanosleep(&ts, NULL);
    }
    return NULL;
}

int main(int argc, char **argv)
{
    int frames = argc > 1 ? atoi(argv[1]) : 300;
    int draws = argc > 2 ? atoi(argv[2]) : 512;
    if (draws < 1 || frames < 1 || draws > 1024) return 2;
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib) {
        printf("FAIL loader: %s\n", dlerror());
        return 2;
    }
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    VkInstance inst = VK_NULL_HANDLE;
#define G(n) n = (PFN_##n)gipa(inst, #n)
    G(vkCreateInstance);
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "vkbridge-flicker", 1, "vkbridge", 1, VK_API_VERSION_1_1};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 0, NULL};
    if (vkCreateInstance(&ici, NULL, &inst)) {
        check(0, "instance", NULL);
        return 1;
    }
    G(vkEnumeratePhysicalDevices); G(vkGetPhysicalDeviceProperties); G(vkGetPhysicalDeviceQueueFamilyProperties);
    G(vkGetPhysicalDeviceMemoryProperties); G(vkCreateDevice); G(vkGetDeviceProcAddr); G(vkDestroyInstance);
#undef G
    uint32_t n = 1;
    VkPhysicalDevice pd;
    vkEnumeratePhysicalDevices(inst, &n, &pd);
    if (!n) {
        check(0, "physical device", "none");
        return 1;
    }
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(pd, &props);
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    uint32_t nq = 16, qfam;
    VkQueueFamilyProperties qp[16];
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qp);
    for (qfam = 0; qfam < nq; qfam++)
        if (qp[qfam].queueFlags & VK_QUEUE_COMPUTE_BIT) break;
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, qfam, 1, &prio};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &qci, 0, NULL, 0, NULL, NULL};
    if (vkCreateDevice(pd, &dci, NULL, &dev)) {
        check(0, "device", NULL);
        return 1;
    }
#define G(n) n = (PFN_##n)vkGetDeviceProcAddr(dev, #n)
    G(vkGetDeviceQueue); G(vkCreateCommandPool); G(vkDestroyCommandPool); G(vkAllocateCommandBuffers); G(vkBeginCommandBuffer);
    G(vkEndCommandBuffer); G(vkQueueSubmit); G(vkCreateBuffer); G(vkDestroyBuffer); G(vkGetBufferMemoryRequirements);
    G(vkAllocateMemory); G(vkFreeMemory); G(vkBindBufferMemory); G(vkMapMemory); G(vkCmdPipelineBarrier); G(vkCreateShaderModule);
    G(vkCreateDescriptorSetLayout); G(vkCreatePipelineLayout); G(vkCreateComputePipelines); G(vkCreateDescriptorPool);
    G(vkResetDescriptorPool); G(vkAllocateDescriptorSets); G(vkUpdateDescriptorSets); G(vkCmdBindPipeline); G(vkCmdBindDescriptorSets);
    G(vkCmdDispatch); G(vkCreateFence); G(vkWaitForFences); G(vkResetFences); G(vkCreateBufferView); G(vkFlushMappedMemoryRanges);
    G(vkInvalidateMappedMemoryRanges); G(vkCreateImage); G(vkGetImageMemoryRequirements); G(vkBindImageMemory); G(vkCreateImageView);
    G(vkCreateSampler); G(vkCmdClearColorImage); G(vkDestroyDevice); G(vkResetCommandBuffer);
    G(vkFreeDescriptorSets); G(vkCreateDescriptorUpdateTemplate); G(vkUpdateDescriptorSetWithTemplate); G(vkGetFenceStatus);
    G(vkDestroyImageView); G(vkDestroySampler); G(vkDestroyFence);
#undef G
    vkGetDeviceQueue(dev, qfam, 0, &queue);
    char d[600];
    snprintf(d, sizeof(d), "%s, %d frames x %d draws, %d in flight", props.deviceName, frames, draws, FRAMES_IN_FLIGHT);
    check(1, "device", d);

    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, qfam};
    VkCommandPool cpool;
    vkCreateCommandPool(dev, &pci, NULL, &cpool);
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, cpool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkCommandBufferBeginInfo cbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
    VkCommandBuffer setup_cb;
    VkFence setup_fence;
    vkAllocateCommandBuffers(dev, &cai, &setup_cb);
    vkCreateFence(dev, &fci, NULL, &setup_fence);
    vkCreateFence(dev, &fci, NULL, &busy_fence);

    VkDeviceSize ua = props.limits.minUniformBufferOffsetAlignment < 256 ? 256 : props.limits.minUniformBufferOffsetAlignment;
    VkDeviceSize ta = props.limits.minTexelBufferOffsetAlignment < 256 ? 256 : props.limits.minTexelBufferOffsetAlignment;
    VkDeviceSize sa = props.limits.minStorageBufferOffsetAlignment < 32 ? 32 : props.limits.minStorageBufferOffsetAlignment;
    buffer ubo, tbo, out;
    if (!make_buffer(&ubo, ua * UBO_N, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT) ||
        !make_buffer(&tbo, ta * K, VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT) ||
        !make_buffer(&out, sa * (VkDeviceSize)draws * FRAMES_IN_FLIGHT, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)) {
        check(0, "buffers", "cannot create/map");
        return 1;
    }
    for (uint32_t i = 0; i < UBO_N; i++) ubo.ptr[i * ua / 4] = 0x10000 + i;
    for (uint32_t i = 0; i < K; i++) tbo.ptr[i * ta / 4] = 0x20000 + i;
    range_sync(&ubo, 1);
    range_sync(&tbo, 1);
    VkBufferView views[K];
    VkImageView images[K];
    vkBeginCommandBuffer(setup_cb, &cbi);
    for (uint32_t i = 0; i < K; i++) {
        VkBufferViewCreateInfo vci = {VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO, NULL, 0, tbo.buf, VK_FORMAT_R32_UINT, i * ta, 4};
        if (vkCreateBufferView(dev, &vci, NULL, &views[i]) || !make_image(setup_cb, 0x30000 + i, &images[i])) {
            check(0, "views and images", NULL);
            return 1;
        }
    }
    vkEndCommandBuffer(setup_cb);
    if (!submit_wait(setup_cb, setup_fence)) {
        check(0, "image clears", "submit failed");
        return 1;
    }
    VkSamplerCreateInfo sci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    VkSampler smp;
    vkCreateSampler(dev, &sci, NULL, &smp);

    VkDescriptorSetLayoutBinding binds[7] = {
        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 2, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {4, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {5, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
    VkDescriptorSetLayoutCreateInfo lci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 7, binds};
    VkDescriptorSetLayout dsl;
    VkPipelineLayout pl;
    VkShaderModule sm;
    VkPipeline pipe;
    VkPipelineLayoutCreateInfo plci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &dsl, 0, NULL};
    VkShaderModuleCreateInfo smci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof(spv_desc_comp), spv_desc_comp};
    VkComputePipelineCreateInfo cpci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    if (vkCreateDescriptorSetLayout(dev, &lci, NULL, &dsl) || vkCreatePipelineLayout(dev, &plci, NULL, &pl) ||
        vkCreateShaderModule(dev, &smci, NULL, &sm)) {
        check(0, "pipeline objects", NULL);
        return 1;
    }
    cpci.stage = (VkPipelineShaderStageCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_COMPUTE_BIT, sm,
                                                   "main", NULL};
    cpci.layout = pl;
    if (vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, NULL, &pipe)) {
        check(0, "compute pipeline", NULL);
        return 1;
    }
    /* A template laid out like DXVK's: the app's own struct, entries in binding order. */
    typedef struct {
        VkDescriptorBufferInfo ub;
        VkBufferView tv;
        VkDescriptorImageInfo cis, sep[2], sampler, sto;
        VkDescriptorBufferInfo sb;
    } tdata;
    VkDescriptorUpdateTemplateEntry te[7] = {
        {0, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, offsetof(tdata, ub), 0},
        {1, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, offsetof(tdata, tv), 0},
        {2, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, offsetof(tdata, cis), 0},
        {3, 0, 2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, offsetof(tdata, sep), sizeof(VkDescriptorImageInfo)},
        {4, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLER, offsetof(tdata, sampler), 0},
        {5, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, offsetof(tdata, sto), 0},
        {6, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, offsetof(tdata, sb), 0}};
    VkDescriptorUpdateTemplateCreateInfo tci = {VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO, NULL, 0, 7, te,
                                                VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET, dsl, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0};
    VkDescriptorUpdateTemplate tmpl;
    if (vkCreateDescriptorUpdateTemplate(dev, &tci, NULL, &tmpl)) {
        check(0, "descriptor update template", NULL);
        return 1;
    }

    VkDescriptorPoolSize ps[7] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, draws},     {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, draws},
                                  {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, draws}, {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 2 * draws},
                                  {VK_DESCRIPTOR_TYPE_SAMPLER, draws},            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, draws},
                                  {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, draws}};
    VkDescriptorPoolCreateInfo dpci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, (uint32_t)draws, 7, ps};
    slot slots[FRAMES_IN_FLIGHT];
    memset(slots, 0, sizeof(slots));
    for (int s = 0; s < FRAMES_IN_FLIGHT; s++) {
        slots[s].frame = -1;
        if (vkAllocateCommandBuffers(dev, &cai, &slots[s].cb) || vkCreateFence(dev, &fci, NULL, &slots[s].fence) ||
            vkCreateDescriptorPool(dev, &dpci, NULL, &slots[s].pool)) {
            check(0, "per-frame objects", NULL);
            return 1;
        }
    }
    /* The shared pool sets are freed back to: big enough for every frame in flight. */
    VkDescriptorPoolSize ps2[7];
    memcpy(ps2, ps, sizeof(ps));
    for (int i = 0; i < 7; i++) ps2[i].descriptorCount *= FRAMES_IN_FLIGHT;
    VkDescriptorPoolCreateInfo dpci2 = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
                                        (uint32_t)draws * FRAMES_IN_FLIGHT, 7, ps2};
    VkDescriptorPool shared_pool;
    if (vkCreateDescriptorPool(dev, &dpci2, NULL, &shared_pool)) {
        check(0, "shared descriptor pool", NULL);
        return 1;
    }

    pthread_t st, bt;
    pthread_create(&st, NULL, submit_thread, NULL);
    pthread_create(&bt, NULL, busy_thread, NULL);

    int bad_frames = 0, bad_draws = 0, checked = 0;
    char first_bad[300] = "";
    double t0 = now_ms();
    for (int f = 0; f < frames + FRAMES_IN_FLIGHT; f++) {
        int si = f % FRAMES_IN_FLIGHT;
        slot *s = &slots[si];
        uint32_t *o_base = out.ptr + (size_t)si * draws * sa / 4;
        if (s->frame >= 0) {
            /* Wait for the slot's submit to have been made, then for the GPU, as DXVK does. */
            pthread_mutex_lock(&sq_lock);
            while (!s->submitted) pthread_cond_wait(&sq_cond, &sq_lock);
            pthread_mutex_unlock(&sq_lock);
            if (vkWaitForFences(dev, 1, &s->fence, VK_TRUE, 10000000000ull)) {
                check(0, "frame fence", "timeout");
                return 1;
            }
            vkResetFences(dev, 1, &s->fence);
            range_sync(&out, 0);
            int bad = 0, pf = s->frame;
            for (int i = 0; i < draws; i++) {
                uint32_t k = (uint32_t)(i * 7 + pf * 3);
                const uint32_t *o = o_base + (size_t)i * sa / 4;
                uint32_t want[7] = {0x10000 + k % UBO_N, 0x20000 + (k / 2) % K, 0x30000 + k % K, 0x30000 + (k + 1) % K,
                                    0x30000 + (k + 2) % K, 0x30000 + (k + 3) % K, 0x600d};
                for (int j = 0; j < 7; j++)
                    if (o[j] != want[j]) {
                        if (!bad_draws && !bad)
                            snprintf(first_bad, sizeof(first_bad), "frame %d draw %d (%s): value %d is 0x%x, want 0x%x", pf, i,
                                     i % 4 == 0 ? "template" : i % 4 == 1 ? "split update" : i % 4 == 2 ? "freeable set" : "update",
                                     j, o[j], want[j]);
                        bad++;
                        break;
                    }
            }
            checked++;
            bad_draws += bad;
            bad_frames += bad != 0;
            if (s->nfree) vkFreeDescriptorSets(dev, shared_pool, s->nfree, s->freeable);
            s->nfree = 0;
            s->frame = -1;
        }
        if (f >= frames) continue;
        memset(o_base, 0, sa * (size_t)draws);
        range_sync(&out, 1);
        vkResetDescriptorPool(dev, s->pool, 0);
        vkResetCommandBuffer(s->cb, 0);
        vkBeginCommandBuffer(s->cb, &cbi);
        vkCmdBindPipeline(s->cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
        for (int i = 0; i < draws; i++) {
            uint32_t k = (uint32_t)(i * 7 + f * 3);
            VkDescriptorSet set;
            int kind = i % 4;
            VkDescriptorSetAllocateInfo dsai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, kind == 2 ? shared_pool : s->pool, 1, &dsl};
            if (vkAllocateDescriptorSets(dev, &dsai, &set)) {
                check(0, "descriptor set allocation", NULL);
                return 1;
            }
            if (kind == 2) s->freeable[s->nfree++] = set;
            tdata t;
            t.ub = (VkDescriptorBufferInfo){ubo.buf, (k % UBO_N) * ua, 16};
            t.sb = (VkDescriptorBufferInfo){out.buf, ((VkDeviceSize)si * draws + i) * sa, 32};
            t.cis = (VkDescriptorImageInfo){smp, images[k % K], VK_IMAGE_LAYOUT_GENERAL};
            t.sep[0] = (VkDescriptorImageInfo){VK_NULL_HANDLE, images[(k + 1) % K], VK_IMAGE_LAYOUT_GENERAL};
            t.sep[1] = (VkDescriptorImageInfo){VK_NULL_HANDLE, images[(k + 2) % K], VK_IMAGE_LAYOUT_GENERAL};
            t.sampler = (VkDescriptorImageInfo){smp, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
            t.sto = (VkDescriptorImageInfo){VK_NULL_HANDLE, images[(k + 3) % K], VK_IMAGE_LAYOUT_GENERAL};
            t.tv = views[(k / 2) % K];
            if (kind == 0) {
                vkUpdateDescriptorSetWithTemplate(dev, set, tmpl, &t);
            } else {
#define W(b, n, ty, img, buf, tex) {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, b, 0, n, ty, img, buf, tex}
                VkWriteDescriptorSet w[7] = {W(0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, NULL, &t.ub, NULL),
                                             W(1, 1, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, NULL, NULL, &t.tv),
                                             W(2, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &t.cis, NULL, NULL),
                                             W(3, 2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, t.sep, NULL, NULL),
                                             W(4, 1, VK_DESCRIPTOR_TYPE_SAMPLER, &t.sampler, NULL, NULL),
                                             W(5, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &t.sto, NULL, NULL),
                                             W(6, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &t.sb, NULL)};
#undef W
                if (kind == 1) {
                    /* Half, synchronous calls (a sampler made and dropped, a fence asked), the rest. */
                    vkUpdateDescriptorSets(dev, 3, w, 0, NULL);
                    VkSampler tmp;
                    if (!vkCreateSampler(dev, &sci, NULL, &tmp)) vkDestroySampler(dev, tmp, NULL);
                    vkGetFenceStatus(dev, busy_fence);
                    vkUpdateDescriptorSets(dev, 4, w + 3, 0, NULL);
                } else {
                    vkUpdateDescriptorSets(dev, 7, w, 0, NULL);
                }
            }
            vkCmdBindDescriptorSets(s->cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &set, 0, NULL);
            vkCmdDispatch(s->cb, 1, 1, 1);
        }
        VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
        vkCmdPipelineBarrier(s->cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
        vkEndCommandBuffer(s->cb);
        s->frame = f;
        s->submitted = 0;
        pthread_mutex_lock(&sq_lock);
        sq[sq_tail++ % 64] = s;
        pthread_cond_broadcast(&sq_cond);
        pthread_mutex_unlock(&sq_lock);
    }
    double ms = now_ms() - t0;
    stop_all = 1;
    pthread_mutex_lock(&sq_lock);
    pthread_cond_broadcast(&sq_cond);
    pthread_mutex_unlock(&sq_lock);
    pthread_join(st, NULL);
    pthread_join(bt, NULL);
    snprintf(d, sizeof(d), "%d frames checked, %d draws wrong in %d frames (%.2f ms per frame)%s%s", checked, bad_draws, bad_frames,
             ms / frames, first_bad[0] ? "; first: " : "", first_bad);
    check(!bad_draws && checked == frames, "every draw of every frame read its own descriptors", d);

    vkDestroyDevice(dev, NULL);
    vkDestroyInstance(inst, NULL);
    printf("%s: %d of %d checks passed\n", failures ? "FAILED" : "OK", checks - failures, checks);
    return failures ? 1 : 0;
}
