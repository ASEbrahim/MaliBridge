/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * dmabufsize_check: gamescope's import of a client's dma-buf on a driver without VK_EXT_image_drm_format_modifier
 * (Xiaomi 17T, Mali-G720 r49p1; Steam's window never showed). The exporter (Steam's Chromium) makes a
 * LINEAR BGRA8 image with DMA_BUF and exports its memory; the importer (gamescope) names no modifier: an OPTIMAL
 * image, sampled, with DMA_BUF, bound to the imported dma-buf. Mali sizes the OPTIMAL image larger than the LINEAR
 * dma-buf (1564x720: 5193728 bytes against 4558848) and the import fails with VK_ERROR_INVALID_EXTERNAL_HANDLE.
 * Host imitation: VKBRIDGE_FAKE_MISSING=dmabuflinear (the driver refuses dma-buf images at plain tiling, as r49p1
 * does) and optpad (OPTIMAL images ask 14% more), with the export emulation on (dmabufexport,
 * VKBRIDGE_TEST_EXPORT_PAGES=driver).
 *   A  1564x720: the exporter LINEAR, red; the importer's memory requirement fits in the dma-buf, the import binds,
 *      and red is read out of it (an OPTIMAL image over LINEAR texels reads them tiled on a desktop GPU)
 *   B  800x600: an OPTIMAL export of that shape is live beside the LINEAR one: the importer of the OPTIMAL
 *      export keeps its OPTIMAL image (it fits in that dma-buf, red is read out of it): imports that work are left alone
 * Exit status 0 only if all pass.
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
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "dmabufsize_check", 1, NULL, 0, VK_API_VERSION_1_2};
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

static int submit(ctx *c, VkCommandBuffer cb)
{
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cb, 0, NULL};
    return D(vkQueueSubmit)(c->queue, 1, &si, VK_NULL_HANDLE) == VK_SUCCESS && D(vkQueueWaitIdle)(c->queue) == VK_SUCCESS;
}


static VkImageUsageFlags EXP_USAGE = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

typedef struct {
    VkImage img;
    VkDeviceMemory mem;
    int fd;
    uint32_t w, h;
} xport;

/* gamescope's client: an image of the tiling asked, DMA_BUF, dedicated exportable memory, cleared to red, exported. */
static int export_image(ctx *c, xport *x, uint32_t w, uint32_t h, VkImageTiling tiling, VkCommandBuffer cb)
{
    char d[200], what[160];
    const char *tn = tiling == VK_IMAGE_TILING_LINEAR ? "LINEAR" : "OPTIMAL";
    memset(x, 0, sizeof(*x));
    x->fd = -1;
    x->w = w;
    x->h = h;
    VkExternalMemoryImageCreateInfo ext = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, NULL, DMABUF};
    VkImageCreateInfo ici = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &ext, 0, VK_IMAGE_TYPE_2D, VK_FORMAT_B8G8R8A8_UNORM, {w, h, 1}, 1, 1,
                             VK_SAMPLE_COUNT_1_BIT, tiling, EXP_USAGE, VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
    VkResult r = D(vkCreateImage)(c->dev, &ici, NULL, &x->img);
    snprintf(d, sizeof(d), "%d", r);
    snprintf(what, sizeof(what), "%ux%u exporter: create %s with DMA_BUF", w, h, tn);
    check(r == VK_SUCCESS, what, d);
    if (r != VK_SUCCESS) return 0;
    VkMemoryRequirements req;
    D(vkGetImageMemoryRequirements)(c->dev, x->img, &req);
    VkMemoryDedicatedAllocateInfo ded = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, NULL, x->img, VK_NULL_HANDLE};
    VkExportMemoryAllocateInfo exp = {VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, &ded, DMABUF};
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &exp, req.size, pick_type(c, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    r = D(vkAllocateMemory)(c->dev, &ai, NULL, &x->mem);
    if (r == VK_SUCCESS) r = D(vkBindImageMemory)(c->dev, x->img, x->mem, 0);
    snprintf(d, sizeof(d), "%d (%llu bytes)", r, (unsigned long long)req.size);
    snprintf(what, sizeof(what), "%ux%u exporter: dedicated exportable %s memory bound", w, h, tn);
    check(r == VK_SUCCESS, what, d);
    if (r != VK_SUCCESS) return 0;
    VkCommandBufferBeginInfo cbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
    D(vkResetCommandBuffer)(cb, 0);
    D(vkBeginCommandBuffer)(cb, &cbi);
    barrier(c, cb, x->img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkClearColorValue red = {.float32 = {1.0f, 0.0f, 0.0f, 1.0f}};
    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    D(vkCmdClearColorImage)(cb, x->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &red, 1, &range);
    barrier(c, cb, x->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
    D(vkEndCommandBuffer)(cb);
    int ok = submit(c, cb);
    VkMemoryGetFdInfoKHR gi = {VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR, NULL, x->mem, DMABUF};
    r = D(vkGetMemoryFdKHR)(c->dev, &gi, &x->fd);
    snprintf(d, sizeof(d), "%d (fd %d), cleared %s", r, x->fd, ok ? "ok" : "failed");
    snprintf(what, sizeof(what), "%ux%u exporter: red %s image exported as a dma-buf", w, h, tn);
    check(r == VK_SUCCESS && x->fd >= 0 && ok, what, d);
    return r == VK_SUCCESS && x->fd >= 0 && ok;
}

static void release(ctx *c, xport *x)
{
    if (x->fd >= 0) close(x->fd);
    if (x->img) D(vkDestroyImage)(c->dev, x->img, NULL);
    if (x->mem) D(vkFreeMemory)(c->dev, x->mem, NULL);
}

/* gamescope's import: OPTIMAL, sampled, DMA_BUF, no modifier, dedicated memory from the dma-buf; red read out of it. */
static void import_image(ctx *c, const xport *x, const char *label, VkCommandBuffer cb)
{
    char d[200], what[160];
    uint32_t w = x->w, h = x->h;
    VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    VkExternalMemoryImageCreateInfo ext = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, NULL, DMABUF};
    VkImageCreateInfo ici = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &ext, 0, VK_IMAGE_TYPE_2D, VK_FORMAT_B8G8R8A8_UNORM, {w, h, 1}, 1, 1,
                             VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL, usage, VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
    VkImage img = VK_NULL_HANDLE;
    VkResult r = D(vkCreateImage)(c->dev, &ici, NULL, &img);
    snprintf(d, sizeof(d), "%d", r);
    snprintf(what, sizeof(what), "%ux%u %s: importer's OPTIMAL image with DMA_BUF made", w, h, label);
    check(r == VK_SUCCESS, what, d);
    if (r != VK_SUCCESS) return;
    VkMemoryRequirements req;
    D(vkGetImageMemoryRequirements)(c->dev, img, &req);
    off_t len = lseek(x->fd, 0, SEEK_END);
    lseek(x->fd, 0, SEEK_SET);
    snprintf(d, sizeof(d), "the image asks %llu bytes, the dma-buf has %lld", (unsigned long long)req.size, (long long)len);
    snprintf(what, sizeof(what), "%ux%u %s: the importer's memory requirement fits in the dma-buf", w, h, label);
    check(len > 0 && req.size <= (VkDeviceSize)len, what, d);
    VkMemoryFdPropertiesKHR fp = {VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
    D(vkGetMemoryFdPropertiesKHR)(c->dev, DMABUF, x->fd, &fp);
    int ifd = dup(x->fd);
    VkImportMemoryFdInfoKHR imp = {VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR, NULL, DMABUF, ifd};
    VkMemoryDedicatedAllocateInfo ded = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, &imp, img, VK_NULL_HANDLE};
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &ded, req.size,
                               pick_type(c, req.memoryTypeBits & (fp.memoryTypeBits ? fp.memoryTypeBits : ~0u), 0)};
    VkDeviceMemory mem = VK_NULL_HANDLE;
    r = D(vkAllocateMemory)(c->dev, &ai, NULL, &mem);
    if (r != VK_SUCCESS) close(ifd);
    if (r == VK_SUCCESS) r = D(vkBindImageMemory)(c->dev, img, mem, 0);
    snprintf(d, sizeof(d), "%d (%llu bytes, type %u)", r, (unsigned long long)req.size, ai.memoryTypeIndex);
    snprintf(what, sizeof(what), "%ux%u %s: dedicated dma-buf import bound", w, h, label);
    check(r == VK_SUCCESS, what, d);
    if (r == VK_SUCCESS) {
        VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, (VkDeviceSize)w * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                  VK_SHARING_MODE_EXCLUSIVE};
        VkBuffer hb;
        D(vkCreateBuffer)(c->dev, &bci, NULL, &hb);
        VkMemoryRequirements hreq;
        D(vkGetBufferMemoryRequirements)(c->dev, hb, &hreq);
        VkMemoryAllocateInfo hai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, hreq.size,
                                    pick_type(c, hreq.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
        VkDeviceMemory hmem;
        D(vkAllocateMemory)(c->dev, &hai, NULL, &hmem);
        D(vkBindBufferMemory)(c->dev, hb, hmem, 0);
        VkCommandBufferBeginInfo cbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
        D(vkResetCommandBuffer)(cb, 0);
        D(vkBeginCommandBuffer)(cb, &cbi);
        /* the exporter's texels are already there: a foreign-queue acquire, not UNDEFINED */
        VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, 0, VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
                                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, img,
                                  {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        D(vkCmdPipelineBarrier)(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
        VkBufferImageCopy bic = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {w, h, 1}};
        D(vkCmdCopyImageToBuffer)(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, hb, 1, &bic);
        VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
        D(vkCmdPipelineBarrier)(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
        D(vkEndCommandBuffer)(cb);
        int ok = submit(c, cb);
        const uint32_t *p;
        D(vkMapMemory)(c->dev, hmem, 0, VK_WHOLE_SIZE, 0, (void **)&p);
        uint32_t bad = 0, first = 0;
        for (uint32_t i = 0; ok && i < w * h; i++)
            if (p[i] != 0xffff0000u) {
                if (!bad) first = p[i];
                bad++;
            }
        snprintf(d, sizeof(d), "submit %s, %u of %u texels not red", ok ? "ok" : "failed", bad, w * h);
        if (bad) snprintf(d + strlen(d), sizeof(d) - strlen(d), ", first 0x%08x", first);
        snprintf(what, sizeof(what), "%ux%u %s: red read out of the imported image", w, h, label);
        check(ok && !bad, what, d);
        D(vkUnmapMemory)(c->dev, hmem);
        D(vkDestroyBuffer)(c->dev, hb, NULL);
        D(vkFreeMemory)(c->dev, hmem, NULL);
    }
    D(vkDestroyImage)(c->dev, img, NULL);
    if (mem) D(vkFreeMemory)(c->dev, mem, NULL);
}

int main(void)
{
    ctx cc, *c = &cc;
    if (!setup(c)) {
        check(0, "Vulkan setup (VK_EXT_external_memory_dma_buf)", NULL);
        return 1;
    }
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, c->pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VkCommandBuffer cb;
    D(vkAllocateCommandBuffers)(c->dev, &cai, &cb);
    xport lin, opt;
    /* A: Steam's window of the a player log session */
    if (export_image(c, &lin, 1564, 720, VK_IMAGE_TILING_LINEAR, cb)) import_image(c, &lin, "import of a LINEAR export", cb);
    release(c, &lin);
    /* B: a LINEAR and an OPTIMAL export of one shape live: the OPTIMAL one's importer keeps its OPTIMAL image */
    xport lin2;
    int a = export_image(c, &lin2, 800, 600, VK_IMAGE_TILING_LINEAR, cb);
    int b = export_image(c, &opt, 800, 600, VK_IMAGE_TILING_OPTIMAL, cb);
    if (a && b) import_image(c, &opt, "import of an OPTIMAL export beside a LINEAR one", cb);
    release(c, &opt);
    release(c, &lin2);
    D(vkDestroyCommandPool)(c->dev, c->pool, NULL);
    D(vkDestroyDevice)(c->dev, NULL);
    I(vkDestroyInstance)(c->inst, NULL);
    printf("%s %d/%d checks\n", failures ? "FAILED:" : "OK:", checks - failures, checks);
    return failures ? 1 : 0;
}
