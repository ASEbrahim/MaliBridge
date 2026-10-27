/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * gsexport_check: gamescope's export images, asked and made as its nested (Wayland) backend does
 * (rendervulkan.cpp, CVulkanTexture::BInit and getModifierProps). For each flippable image it asks
 * vkGetPhysicalDeviceImageFormatProperties2 at DRM-modifier tiling, the LINEAR modifier (the only one
 * DroidDeck's compositor advertises), DMA_BUF external memory, the image's usage and MUTABLE_FORMAT
 * with its UNORM / SRGB view list, and keeps the modifier only when the answer is VK_SUCCESS with
 * EXPORTABLE; with none kept it aborts on "modifiers.size() > 0" (MaliBridge#10, Mali-G77 r32p1,
 * where the export emulation was on). Then it creates the image with that modifier list, gives it
 * dedicated memory that exports a dma-buf, reads the modifier and plane 0's layout, and exports.
 *
 * Here: the two image shapes' queries (the output image: BGRA8, SAMPLED | STORAGE | TRANSFER_SRC;
 * the flat texture: BGRA8, SAMPLED | TRANSFER_DST), then one image made as gamescope makes it,
 * cleared to red, exported, and the dma-buf imported into a buffer and read at the layout it
 * reported (what the compositor would see), and the dma-buf imported as gamescope imports a client's
 * (an image at the explicit LINEAR modifier, its memory requirement within the dma-buf, red read
 * out of it). Host imitation of r32p1: VKBRIDGE_FAKE_MISSING=dmabufmod
 * with the export emulation on (dmabufexport, VKBRIDGE_TEST_EXPORT_PAGES=driver). Exit status 0
 * only if all pass.
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
#define MOD_LINEAR 0ull

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
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "gsexport_check", 1, NULL, 0, VK_API_VERSION_1_2};
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
    const char *exts[] = {"VK_KHR_external_memory_fd", "VK_EXT_external_memory_dma_buf", "VK_EXT_image_drm_format_modifier"};
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, c->qf, 1, &prio};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &qci, 0, NULL, 3, exts, NULL};
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

static const VkFormat views[2] = {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_SRGB};

/* getModifierProps, as gamescope asks it. */
static VkResult modifier_props(ctx *c, VkImageUsageFlags usage, VkExternalImageFormatProperties *xp)
{
    VkPhysicalDeviceImageDrmFormatModifierInfoEXT mi = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT, NULL, MOD_LINEAR,
                                                        VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
    VkPhysicalDeviceExternalImageFormatInfo ei = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO, &mi, DMABUF};
    VkImageFormatListCreateInfo fl = {VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO, &ei, 2, views};
    VkPhysicalDeviceImageFormatInfo2 ii = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2, &fl, VK_FORMAT_B8G8R8A8_UNORM,
                                           VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT, usage,
                                           VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT};
    memset(xp, 0, sizeof(*xp));
    xp->sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES;
    VkImageFormatProperties2 ip = {VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, xp};
    return I(vkGetPhysicalDeviceImageFormatProperties2)(c->pd, &ii, &ip);
}

static int query(ctx *c, const char *name, VkImageUsageFlags usage)
{
    VkExternalImageFormatProperties xp;
    VkResult r = modifier_props(c, usage, &xp);
    char d[160], what[160];
    snprintf(d, sizeof(d), "result %d, external features 0x%x, compatible 0x%x", r, xp.externalMemoryProperties.externalMemoryFeatures,
             xp.externalMemoryProperties.compatibleHandleTypes);
    snprintf(what, sizeof(what), "%s (usage 0x%x, MUTABLE_FORMAT): LINEAR modifier exportable as a dma-buf", name, usage);
    int ok = r == VK_SUCCESS && (xp.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT);
    check(ok, what, d);
    return ok;
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

/* gamescope's import of a client's LINEAR dma-buf (BInit with a wlr_dmabuf_attributes): its query
 * at the LINEAR modifier (sampled, MUTABLE_FORMAT with the view list) must be IMPORTABLE, or it makes
 * an OPTIMAL image with an implicit layout (MaliBridge#10 on the S10+ under dmabufmod: 3993600 bytes
 * asked of a 3719168-byte dma-buf, VK_ERROR_INVALID_EXTERNAL_HANDLE). Then the image at the explicit
 * LINEAR modifier with the exporter's plane layout: its memory requirement must fit in the dma-buf,
 * the import must bind, and a copy out of it must read red at the exporter's layout. */
static void imported(ctx *c, uint32_t w, uint32_t h, int fd, const VkSubresourceLayout *lay, VkCommandBuffer cb)
{
    char d[200], what[160];
    VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    VkExternalImageFormatProperties xp;
    VkResult r = modifier_props(c, usage, &xp);
    snprintf(d, sizeof(d), "result %d, external features 0x%x", r, xp.externalMemoryProperties.externalMemoryFeatures);
    snprintf(what, sizeof(what), "%ux%u import: LINEAR modifier importable as a dma-buf (usage 0x%x, MUTABLE_FORMAT)", w, h, usage);
    check(r == VK_SUCCESS && (xp.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT), what, d);
    if (r != VK_SUCCESS) return;
    VkSubresourceLayout plane = *lay;
    VkImageDrmFormatModifierExplicitCreateInfoEXT xi = {VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT, NULL, MOD_LINEAR,
                                                        1, &plane};
    plane.size = 0; /* as gamescope passes it */
    VkImageFormatListCreateInfo fl = {VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO, &xi, 2, views};
    VkExternalMemoryImageCreateInfo ext = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, &fl, DMABUF};
    VkImageCreateInfo ici = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &ext, VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT, VK_IMAGE_TYPE_2D,
                             VK_FORMAT_B8G8R8A8_UNORM, {w, h, 1}, 1, 1, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT, usage,
                             VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
    VkImage img = VK_NULL_HANDLE;
    r = D(vkCreateImage)(c->dev, &ici, NULL, &img);
    snprintf(d, sizeof(d), "%d (row pitch %llu)", r, (unsigned long long)plane.rowPitch);
    snprintf(what, sizeof(what), "%ux%u import: create at the explicit LINEAR modifier with the exporter's layout", w, h);
    check(r == VK_SUCCESS, what, d);
    if (r != VK_SUCCESS) return;
    VkMemoryRequirements req;
    D(vkGetImageMemoryRequirements)(c->dev, img, &req);
    off_t len = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    snprintf(d, sizeof(d), "the image asks %llu bytes, the dma-buf has %lld", (unsigned long long)req.size, (long long)len);
    snprintf(what, sizeof(what), "%ux%u import: the image's memory requirement fits in the dma-buf", w, h);
    check(len > 0 && req.size <= (VkDeviceSize)len, what, d);
    VkMemoryFdPropertiesKHR fp = {VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
    D(vkGetMemoryFdPropertiesKHR)(c->dev, DMABUF, fd, &fp);
    int ifd = dup(fd);
    VkImportMemoryFdInfoKHR imp = {VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR, NULL, DMABUF, ifd};
    VkMemoryDedicatedAllocateInfo ded = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, &imp, img, VK_NULL_HANDLE};
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &ded, req.size,
                               pick_type(c, req.memoryTypeBits & (fp.memoryTypeBits ? fp.memoryTypeBits : ~0u), 0)};
    VkDeviceMemory mem = VK_NULL_HANDLE;
    r = D(vkAllocateMemory)(c->dev, &ai, NULL, &mem);
    if (r != VK_SUCCESS) close(ifd);
    if (r == VK_SUCCESS) r = D(vkBindImageMemory)(c->dev, img, mem, 0);
    snprintf(d, sizeof(d), "%d (%llu bytes, type %u)", r, (unsigned long long)req.size, ai.memoryTypeIndex);
    snprintf(what, sizeof(what), "%ux%u import: dedicated dma-buf import bound", w, h);
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
        /* The exporter's texels are already there: a foreign-queue acquire, not UNDEFINED. */
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
        snprintf(what, sizeof(what), "%ux%u import: red read out of the imported image", w, h);
        check(ok && !bad, what, d);
        D(vkUnmapMemory)(c->dev, hmem);
        D(vkDestroyBuffer)(c->dev, hb, NULL);
        D(vkFreeMemory)(c->dev, hmem, NULL);
    }
    D(vkDestroyImage)(c->dev, img, NULL);
    if (mem) D(vkFreeMemory)(c->dev, mem, NULL);
}

/* An image as BInit makes a flippable one, cleared to red, exported and read through the dma-buf. */
static void made(ctx *c, uint32_t w, uint32_t h)
{
    char d[200], what[160];
    VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (!query(c, "the image made below", usage)) return;
    uint64_t mods[1] = {MOD_LINEAR};
    VkImageFormatListCreateInfo fl = {VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO, NULL, 2, views};
    VkImageDrmFormatModifierListCreateInfoEXT ml = {VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT, &fl, 1, mods};
    VkExternalMemoryImageCreateInfo ext = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, &ml, DMABUF};
    VkImageCreateInfo ici = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &ext, VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT, VK_IMAGE_TYPE_2D,
                             VK_FORMAT_B8G8R8A8_UNORM, {w, h, 1}, 1, 1, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT, usage,
                             VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
    VkImage img = VK_NULL_HANDLE;
    VkResult r = D(vkCreateImage)(c->dev, &ici, NULL, &img);
    snprintf(d, sizeof(d), "%d", r);
    snprintf(what, sizeof(what), "%ux%u: create with the modifier list {LINEAR} and DMA_BUF", w, h);
    check(r == VK_SUCCESS, what, d);
    if (r != VK_SUCCESS) return;

    VkImageDrmFormatModifierPropertiesEXT mp = {VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT};
    r = D(vkGetImageDrmFormatModifierPropertiesEXT)(c->dev, img, &mp);
    snprintf(d, sizeof(d), "%d, modifier 0x%llx", r, (unsigned long long)mp.drmFormatModifier);
    snprintf(what, sizeof(what), "%ux%u: the image's modifier is LINEAR", w, h);
    check(r == VK_SUCCESS && mp.drmFormatModifier == MOD_LINEAR, what, d);

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
    snprintf(what, sizeof(what), "%ux%u: dedicated exportable memory bound", w, h);
    check(r == VK_SUCCESS, what, d);
    if (r != VK_SUCCESS) {
        D(vkDestroyImage)(c->dev, img, NULL);
        return;
    }

    VkImageSubresource sub = {VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT, 0, 0};
    VkSubresourceLayout lay = {0};
    D(vkGetImageSubresourceLayout)(c->dev, img, &sub, &lay);
    snprintf(d, sizeof(d), "offset %llu size %llu rowPitch %llu", (unsigned long long)lay.offset, (unsigned long long)lay.size,
             (unsigned long long)lay.rowPitch);
    snprintf(what, sizeof(what), "%ux%u: memory plane 0's layout", w, h);
    int lay_ok = lay.rowPitch >= (VkDeviceSize)w * 4 && lay.offset + lay.rowPitch * (h - 1) + (VkDeviceSize)w * 4 <= req.size;
    check(lay_ok, what, d);

    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, c->pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VkCommandBuffer cb;
    D(vkAllocateCommandBuffers)(c->dev, &cai, &cb);
    VkCommandBufferBeginInfo cbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
    D(vkBeginCommandBuffer)(cb, &cbi);
    barrier(c, cb, img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkClearColorValue red = {.float32 = {1.0f, 0.0f, 0.0f, 1.0f}};
    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    D(vkCmdClearColorImage)(cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &red, 1, &range);
    barrier(c, cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
    D(vkEndCommandBuffer)(cb);
    int ok = submit(c, cb);

    VkMemoryGetFdInfoKHR gi = {VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR, NULL, mem, DMABUF};
    int fd = -1;
    r = D(vkGetMemoryFdKHR)(c->dev, &gi, &fd);
    snprintf(d, sizeof(d), "%d (fd %d)", r, fd);
    snprintf(what, sizeof(what), "%ux%u: vkGetMemoryFdKHR(DMA_BUF)", w, h);
    check(r == VK_SUCCESS && fd >= 0, what, d);

    /* The compositor's side, as gamescope imports it: an image at the explicit LINEAR modifier. */
    if (fd >= 0 && lay_ok && ok) imported(c, w, h, fd, &lay, cb);
    /* The dma-buf imported into a buffer and read at the layout. */
    if (fd >= 0 && lay_ok) {
        VkExternalMemoryBufferCreateInfo xb = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO, NULL, DMABUF};
        VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &xb, 0, req.size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                  VK_SHARING_MODE_EXCLUSIVE};
        VkBuffer ib, hb;
        D(vkCreateBuffer)(c->dev, &bci, NULL, &ib);
        VkMemoryRequirements ireq;
        D(vkGetBufferMemoryRequirements)(c->dev, ib, &ireq);
        VkMemoryFdPropertiesKHR fp = {VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
        D(vkGetMemoryFdPropertiesKHR)(c->dev, DMABUF, fd, &fp);
        VkImportMemoryFdInfoKHR imp = {VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR, NULL, DMABUF, fd};
        VkMemoryAllocateInfo iai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &imp, ireq.size,
                                    pick_type(c, ireq.memoryTypeBits & (fp.memoryTypeBits ? fp.memoryTypeBits : ~0u), 0)};
        VkDeviceMemory imem = VK_NULL_HANDLE;
        r = D(vkAllocateMemory)(c->dev, &iai, NULL, &imem);
        if (r == VK_SUCCESS) {
            fd = -1; /* the import owns it */
            r = D(vkBindBufferMemory)(c->dev, ib, imem, 0);
        }
        snprintf(d, sizeof(d), "%d (type %u, the fd's types 0x%x)", r, iai.memoryTypeIndex, fp.memoryTypeBits);
        snprintf(what, sizeof(what), "%ux%u: the exported dma-buf imported", w, h);
        check(r == VK_SUCCESS, what, d);
        if (r == VK_SUCCESS) {
            bci.pNext = NULL;
            bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            D(vkCreateBuffer)(c->dev, &bci, NULL, &hb);
            VkMemoryRequirements hreq;
            D(vkGetBufferMemoryRequirements)(c->dev, hb, &hreq);
            VkMemoryAllocateInfo hai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, hreq.size,
                                        pick_type(c, hreq.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
            VkDeviceMemory hmem;
            D(vkAllocateMemory)(c->dev, &hai, NULL, &hmem);
            D(vkBindBufferMemory)(c->dev, hb, hmem, 0);
            D(vkResetCommandBuffer)(cb, 0);
            D(vkBeginCommandBuffer)(cb, &cbi);
            VkBufferCopy bc = {0, 0, req.size};
            D(vkCmdCopyBuffer)(cb, ib, hb, 1, &bc);
            VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
            D(vkCmdPipelineBarrier)(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
            D(vkEndCommandBuffer)(cb);
            ok = ok && submit(c, cb);
            const unsigned char *p;
            D(vkMapMemory)(c->dev, hmem, 0, VK_WHOLE_SIZE, 0, (void **)&p);
            uint32_t bad = 0, first = UINT32_MAX;
            for (uint32_t y = 0; ok && y < h; y++)
                for (uint32_t x = 0; x < w; x++) {
                    const unsigned char *t = p + lay.offset + y * lay.rowPitch + x * 4;
                    uint32_t v = (uint32_t)t[0] | (uint32_t)t[1] << 8 | (uint32_t)t[2] << 16 | (uint32_t)t[3] << 24;
                    if (v != 0xffff0000u) {
                        if (!bad) first = v;
                        bad++;
                    }
                }
            snprintf(d, sizeof(d), "submits %s, %u of %u texels not red (BGRA 00 00 ff ff)%s", ok ? "ok" : "failed", bad, w * h,
                     bad ? "" : "");
            if (bad) snprintf(d + strlen(d), sizeof(d) - strlen(d), ", first 0x%08x", first);
            snprintf(what, sizeof(what), "%ux%u: red through the dma-buf at the reported layout", w, h);
            check(ok && !bad, what, d);
            D(vkUnmapMemory)(c->dev, hmem);
            D(vkDestroyBuffer)(c->dev, hb, NULL);
            D(vkFreeMemory)(c->dev, hmem, NULL);
        }
        D(vkDestroyBuffer)(c->dev, ib, NULL);
        if (imem) D(vkFreeMemory)(c->dev, imem, NULL);
    }
    if (fd >= 0) close(fd);
    D(vkFreeCommandBuffers)(c->dev, c->pool, 1, &cb);
    D(vkDestroyImage)(c->dev, img, NULL);
    D(vkFreeMemory)(c->dev, mem, NULL);
}

int main(void)
{
    ctx cc, *c = &cc;
    if (!setup(c)) {
        check(0, "Vulkan setup (VK_EXT_external_memory_dma_buf, VK_EXT_image_drm_format_modifier)", NULL);
        return 1;
    }
    /* gamescope's two kinds of flippable image (XR24 / AR24 -> B8G8R8A8) */
    query(c, "output image", VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    query(c, "flat texture", VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    made(c, 64, 48);
    made(c, 333, 1); /* one row: the driver's single-row pitch (Mali answers 0) */
    made(c, 1280, 720); /* Steam's windows on the S10+ (MaliBridge#10 under dmabufmod) */
    made(c, 1279, 721); /* odd sizes: the row pitch and size alignment */
    D(vkDestroyCommandPool)(c->dev, c->pool, NULL);
    D(vkDestroyDevice)(c->dev, NULL);
    I(vkDestroyInstance)(c->inst, NULL);
    printf("%s %d/%d checks\n", failures ? "FAILED:" : "OK:", checks - failures, checks);
    return failures ? 1 : 0;
}
