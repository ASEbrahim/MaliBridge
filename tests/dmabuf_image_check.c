/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * dmabuf_image_check: dma-buf images at LINEAR tiling, as Zink makes its shared linear resources
 * (VkExternalMemoryImageCreateInfo with DMA_BUF, no DRM modifier). Mali r44 refuses them with
 * VK_ERROR_INVALID_DRM_FORMAT_MODIFIER_PLANE_LAYOUT_EXT, and Skyrim Special Edition's process gave
 * up right after its 4096x1 R32_SFLOAT 1D one was refused (2026-10-14); the host imitation is
 * VKBRIDGE_FAKE_MISSING=dmabuflinear. Each image is created, given dedicated memory that exports a
 * dma-buf, filled and read back through copies, and its memory exported. What the format query
 * says is printed. Exit status 0 only if all pass.
 *   cc -O2 -Wall -Ithird_party/Vulkan-Headers-1.4.341/include -o host/out/dmabuf_image_check tests/dmabuf_image_check.c -ldl
 */
#define _GNU_SOURCE
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DMABUF VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT

static int failures, checks;

static void check(int ok, const char *what, const char *detail)
{
    checks++;
    if (!ok) failures++;
    printf("%s %s%s%s\n", ok ? "PASS" : "FAIL", what, detail && *detail ? ": " : "", detail ? detail : "");
    fflush(stdout);
}

typedef struct {
    VkInstance inst;
    VkPhysicalDevice pd;
    VkDevice dev;
    VkQueue queue;
    VkCommandPool pool;
    uint32_t qf;
    VkPhysicalDeviceMemoryProperties mp;
    PFN_vkGetInstanceProcAddr gipa;
    PFN_vkGetDeviceProcAddr gdpa;
} ctx;

#define I(name) ((PFN_##name)c->gipa(c->inst, #name))
#define D(name) ((PFN_##name)c->gdpa(c->dev, #name))

static int setup(ctx *c)
{
    memset(c, 0, sizeof(*c));
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib) return 0;
    c->gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "dmabuf_image_check", 1, NULL, 0, VK_API_VERSION_1_1};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app};
    if (I(vkCreateInstance)(&ici, NULL, &c->inst) != VK_SUCCESS) return 0;
    uint32_t n = 1;
    I(vkEnumeratePhysicalDevices)(c->inst, &n, &c->pd);
    if (!n) return 0;
    I(vkGetPhysicalDeviceMemoryProperties)(c->pd, &c->mp);
    uint32_t nq = 8;
    VkQueueFamilyProperties qp[8];
    I(vkGetPhysicalDeviceQueueFamilyProperties)(c->pd, &nq, qp);
    for (uint32_t i = 0; i < nq; i++)
        if (qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            c->qf = i;
            break;
        }
    float prio = 1.0f;
    const char *exts[] = {"VK_KHR_external_memory_fd", "VK_EXT_external_memory_dma_buf"};
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, c->qf, 1, &prio};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &qci, 0, NULL, 2, exts, NULL};
    if (I(vkCreateDevice)(c->pd, &dci, NULL, &c->dev) != VK_SUCCESS) return 0;
    c->gdpa = (PFN_vkGetDeviceProcAddr)c->gipa(c->inst, "vkGetDeviceProcAddr");
    D(vkGetDeviceQueue)(c->dev, c->qf, 0, &c->queue);
    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, 0, c->qf};
    return D(vkCreateCommandPool)(c->dev, &pci, NULL, &c->pool) == VK_SUCCESS;
}

static uint32_t pick_type(ctx *c, uint32_t bits, VkMemoryPropertyFlags want)
{
    for (uint32_t i = 0; i < c->mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (c->mp.memoryTypes[i].propertyFlags & want) == want) return i;
    for (uint32_t i = 0; i < c->mp.memoryTypeCount; i++)
        if (bits & (1u << i)) return i;
    return UINT32_MAX;
}

static void barrier(ctx *c, VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to)
{
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, VK_ACCESS_MEMORY_WRITE_BIT,
                              VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, from, to, VK_QUEUE_FAMILY_IGNORED,
                              VK_QUEUE_FAMILY_IGNORED, img, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    D(vkCmdPipelineBarrier)(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}

typedef struct {
    const char *name;
    VkImageType type;
    VkFormat format;
    uint32_t w, h;
    VkImageUsageFlags usage;
} shape;

static void one(ctx *c, const shape *s)
{
    char d[192], what[128];
    /* The query, as an app that checks would ask it. */
    VkPhysicalDeviceExternalImageFormatInfo ei = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO, NULL, DMABUF};
    VkPhysicalDeviceImageFormatInfo2 ii = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2, &ei, s->format, s->type,
                                           VK_IMAGE_TILING_LINEAR, s->usage, 0};
    VkExternalImageFormatProperties xp = {VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
    VkImageFormatProperties2 ip = {VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, &xp};
    VkResult qr = I(vkGetPhysicalDeviceImageFormatProperties2)(c->pd, &ii, &ip);
    printf("note %s: dma-buf query %d, features 0x%x compatible 0x%x\n", s->name, qr, xp.externalMemoryProperties.externalMemoryFeatures,
           xp.externalMemoryProperties.compatibleHandleTypes);

    VkExternalMemoryImageCreateInfo ext = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, NULL, DMABUF};
    VkImageCreateInfo ici = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &ext, 0, s->type, s->format, {s->w, s->h, 1}, 1, 1,
                             VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_LINEAR, s->usage, VK_SHARING_MODE_EXCLUSIVE, 0, NULL,
                             VK_IMAGE_LAYOUT_UNDEFINED};
    VkImage img = VK_NULL_HANDLE;
    VkResult r = D(vkCreateImage)(c->dev, &ici, NULL, &img);
    snprintf(d, sizeof(d), "%d", r);
    snprintf(what, sizeof(what), "%s: create LINEAR image with DMA_BUF external memory", s->name);
    check(r == VK_SUCCESS, what, d);
    if (r != VK_SUCCESS) return;

    VkMemoryRequirements req;
    D(vkGetImageMemoryRequirements)(c->dev, img, &req);
    VkMemoryDedicatedAllocateInfo ded = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, NULL, img, VK_NULL_HANDLE};
    VkExportMemoryAllocateInfo exp = {VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, &ded, DMABUF};
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &exp, req.size,
                               pick_type(c, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    VkDeviceMemory mem = VK_NULL_HANDLE;
    r = D(vkAllocateMemory)(c->dev, &ai, NULL, &mem);
    if (r == VK_SUCCESS) r = D(vkBindImageMemory)(c->dev, img, mem, 0);
    snprintf(d, sizeof(d), "%d (%llu bytes, type %u)", r, (unsigned long long)req.size, ai.memoryTypeIndex);
    snprintf(what, sizeof(what), "%s: dedicated exportable memory bound", s->name);
    check(r == VK_SUCCESS, what, d);
    if (r != VK_SUCCESS) return;

    VkImageSubresource sub = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
    VkSubresourceLayout lay;
    D(vkGetImageSubresourceLayout)(c->dev, img, &sub, &lay);
    snprintf(d, sizeof(d), "offset %llu size %llu rowPitch %llu", (unsigned long long)lay.offset, (unsigned long long)lay.size,
             (unsigned long long)lay.rowPitch);
    snprintf(what, sizeof(what), "%s: linear layout", s->name);
    check(lay.size >= (VkDeviceSize)s->w * s->h * 4 && lay.rowPitch >= (VkDeviceSize)s->w * 4, what, d);

    /* Filled from one host buffer and read back into another. */
    VkDeviceSize bytes = (VkDeviceSize)s->w * s->h * 4;
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, bytes * 2,
                              VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE};
    VkBuffer buf;
    D(vkCreateBuffer)(c->dev, &bci, NULL, &buf);
    VkMemoryRequirements breq;
    D(vkGetBufferMemoryRequirements)(c->dev, buf, &breq);
    VkMemoryAllocateInfo bai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, breq.size,
                                pick_type(c, breq.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    VkDeviceMemory bmem;
    D(vkAllocateMemory)(c->dev, &bai, NULL, &bmem);
    D(vkBindBufferMemory)(c->dev, buf, bmem, 0);
    uint32_t *p;
    D(vkMapMemory)(c->dev, bmem, 0, VK_WHOLE_SIZE, 0, (void **)&p);
    uint32_t texels = s->w * s->h;
    for (uint32_t i = 0; i < texels; i++) p[i] = (i * 2654435761u) & 0x3f7fffffu; /* finite floats for R32_SFLOAT */
    memset(p + texels, 0, bytes);
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, c->pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VkCommandBuffer cb;
    D(vkAllocateCommandBuffers)(c->dev, &cai, &cb);
    VkCommandBufferBeginInfo cbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
    D(vkBeginCommandBuffer)(cb, &cbi);
    barrier(c, cb, img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy in = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {s->w, s->h, 1}};
    D(vkCmdCopyBufferToImage)(cb, buf, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &in);
    barrier(c, cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy out = in;
    out.bufferOffset = bytes;
    D(vkCmdCopyImageToBuffer)(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &out);
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
    D(vkCmdPipelineBarrier)(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    D(vkEndCommandBuffer)(cb);
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cb, 0, NULL};
    int ok = D(vkQueueSubmit)(c->queue, 1, &si, VK_NULL_HANDLE) == VK_SUCCESS && D(vkQueueWaitIdle)(c->queue) == VK_SUCCESS;
    uint32_t bad = 0;
    for (uint32_t i = 0; ok && i < texels; i++) bad += p[texels + i] != p[i];
    snprintf(d, sizeof(d), "submit %s, %u of %u texels differ", ok ? "ok" : "failed", bad, texels);
    snprintf(what, sizeof(what), "%s: contents through the image", s->name);
    check(ok && !bad, what, d);

    VkMemoryGetFdInfoKHR gi = {VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR, NULL, mem, DMABUF};
    int fd = -1;
    r = D(vkGetMemoryFdKHR)(c->dev, &gi, &fd);
    snprintf(d, sizeof(d), "%d (fd %d)", r, fd);
    snprintf(what, sizeof(what), "%s: vkGetMemoryFdKHR(DMA_BUF)", s->name);
    check(r == VK_SUCCESS && fd >= 0, what, d);
    if (fd >= 0) close(fd);

    D(vkFreeCommandBuffers)(c->dev, c->pool, 1, &cb);
    D(vkDestroyBuffer)(c->dev, buf, NULL);
    D(vkFreeMemory)(c->dev, bmem, NULL);
    D(vkDestroyImage)(c->dev, img, NULL);
    D(vkFreeMemory)(c->dev, mem, NULL);
}

int main(void)
{
    ctx cc, *c = &cc;
    if (!setup(c)) {
        check(0, "Vulkan setup (VK_EXT_external_memory_dma_buf)", NULL);
        return 1;
    }
    /* Skyrim SE's (Zink's) 1D image from the tablet log, and the 2D one refused earlier in it. */
    const shape shapes[] = {
        {"1D 4096 R32_SFLOAT", VK_IMAGE_TYPE_1D, VK_FORMAT_R32_SFLOAT, 4096, 1,
         VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT},
        {"2D 8x8 R8G8B8A8_UNORM", VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, 8, 8,
         VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT},
    };
    for (unsigned i = 0; i < sizeof(shapes) / sizeof(shapes[0]); i++) one(c, &shapes[i]);
    D(vkDestroyCommandPool)(c->dev, c->pool, NULL);
    D(vkDestroyDevice)(c->dev, NULL);
    I(vkDestroyInstance)(c->inst, NULL);
    printf("%s %d/%d checks\n", failures ? "FAILED:" : "OK:", checks - failures, checks);
    return failures ? 1 : 0;
}
