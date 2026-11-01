/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * dmabufahb_check: dma-buf memory as gamescope, Xwayland and Zink use it, on a bridge whose driver has
 * none (server_dmabuf_ahb.c: the pages are AHardwareBuffers on Android, the desktop driver's own
 * dma-bufs in host tests). Process A exports:
 *   - gamescope's flippable image: OPTIMAL B8G8R8A8, no external info, dedicated memory exported
 *     as a dma-buf, cleared to one colour;
 *   - Zink's shared image (no modifiers): LINEAR B8G8R8A8 with the dma-buf handle type, cleared to
 *     another;
 * and sends the fds over a socket. Process B imports Zink's as gamescope does (OPTIMAL, the handle
 * type, modifier invalid) and reads the colour back; imports gamescope's into an image laid out
 * otherwise and must be refused, not shown garbage; asks vkGetMemoryFdPropertiesKHR about both and
 * a foreign fd, and imports the foreign fd (refused where the driver has no dma-bufs: argument
 * "foreign-refused").
 * Where VK_EXT_image_drm_format_modifier is offered, Xwayland's pixmap as Zink makes it exportable
 * after the fact: a DRM-modifier image from a list holding LINEAR, exported, its modifier and plane
 * asked; process B imports it as gamescope does (the explicit LINEAR modifier at that plane) and
 * reads its colour back. Argument "ahb" (the bridge's path): the format's modifiers are LINEAR alone
 * and an explicit import at another row pitch is refused.
 * Usage: dmabufahb_check [foreign-refused] [ahb]  - exit 77 when the device offers no dma-buf memory,
 * prints PASS/FAIL lines and "OK: 0 failures" / "FAILED: n failures".
 */
#define _GNU_SOURCE
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#define DMABUF VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT
#define W 320u
#define H 200u
#define FMT VK_FORMAT_B8G8R8A8_UNORM

static int failures;
static const char *who = "A";
static void check(int ok, const char *what, const char *detail)
{
    if (!ok) failures++;
    printf("%s [%s] %s%s%s\n", ok ? "PASS" : "FAIL", who, what, detail && *detail ? ": " : "", detail ? detail : "");
    fflush(stdout);
}

typedef struct {
    VkInstance inst;
    VkPhysicalDevice pd;
    VkDevice dev;
    VkQueue q;
    VkCommandPool pool;
    uint32_t qf;
    VkPhysicalDeviceMemoryProperties mp;
    int mods;
    PFN_vkGetInstanceProcAddr gipa;
    PFN_vkGetDeviceProcAddr gdpa;
} ctx;
#define I(n) ((PFN_##n)c->gipa(c->inst, #n))
#define D(n) ((PFN_##n)c->gdpa(c->dev, #n))

static int has_ext(ctx *c, const char *name)
{
    uint32_t n = 0;
    I(vkEnumerateDeviceExtensionProperties)(c->pd, NULL, &n, NULL);
    VkExtensionProperties *e = calloc(n + 1, sizeof(*e));
    I(vkEnumerateDeviceExtensionProperties)(c->pd, NULL, &n, e);
    int has = 0;
    for (uint32_t i = 0; i < n; i++) has |= !strcmp(e[i].extensionName, name);
    free(e);
    return has;
}

/* 0: no instance; 1: ready; 77: no dma-buf memory offered */
static int setup(ctx *c)
{
    memset(c, 0, sizeof(*c));
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib) return 0;
    c->gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "dmabufahb_check", 1, NULL, 0, VK_API_VERSION_1_1};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app};
    if (I(vkCreateInstance)(&ici, NULL, &c->inst) != VK_SUCCESS) return 0;
    uint32_t n = 1;
    I(vkEnumeratePhysicalDevices)(c->inst, &n, &c->pd);
    if (!n) return 0;
    if (!has_ext(c, "VK_KHR_external_memory_fd") || !has_ext(c, "VK_EXT_external_memory_dma_buf")) return 77;
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
    const char *exts[4] = {"VK_KHR_external_memory_fd", "VK_EXT_external_memory_dma_buf"};
    uint32_t ne = 2;
    c->mods = has_ext(c, "VK_EXT_image_drm_format_modifier");
    if (c->mods) {
        exts[ne++] = "VK_EXT_image_drm_format_modifier";
        if (has_ext(c, "VK_KHR_image_format_list")) exts[ne++] = "VK_KHR_image_format_list";
    }
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, c->qf, 1, &prio};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &qci, 0, NULL, ne, exts, NULL};
    if (I(vkCreateDevice)(c->pd, &dci, NULL, &c->dev) != VK_SUCCESS) return 0;
    c->gdpa = (PFN_vkGetDeviceProcAddr)c->gipa(c->inst, "vkGetDeviceProcAddr");
    D(vkGetDeviceQueue)(c->dev, c->qf, 0, &c->q);
    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, 0, c->qf};
    return D(vkCreateCommandPool)(c->dev, &pci, NULL, &c->pool) == VK_SUCCESS;
}

static uint32_t pick(ctx *c, uint32_t bits, VkMemoryPropertyFlags want)
{
    for (uint32_t i = 0; i < c->mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (c->mp.memoryTypes[i].propertyFlags & want) == want) return i;
    for (uint32_t i = 0; i < c->mp.memoryTypeCount; i++)
        if (bits & (1u << i)) return i;
    return UINT32_MAX;
}

static VkCommandBuffer begin(ctx *c)
{
    VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, c->pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VkCommandBuffer cb;
    D(vkAllocateCommandBuffers)(c->dev, &ai, &cb);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
    D(vkBeginCommandBuffer)(cb, &bi);
    return cb;
}

static int submit(ctx *c, VkCommandBuffer cb)
{
    D(vkEndCommandBuffer)(cb);
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cb, 0, NULL};
    int ok = D(vkQueueSubmit)(c->q, 1, &si, VK_NULL_HANDLE) == VK_SUCCESS && D(vkQueueWaitIdle)(c->q) == VK_SUCCESS;
    D(vkFreeCommandBuffers)(c->dev, c->pool, 1, &cb);
    return ok;
}

static void barrier(ctx *c, VkCommandBuffer cb, VkImage i, VkImageLayout from, VkImageLayout to, uint32_t src_qf, uint32_t dst_qf)
{
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, VK_ACCESS_MEMORY_WRITE_BIT,
                              VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, from, to, src_qf, dst_qf, i,
                              {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    D(vkCmdPipelineBarrier)(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}

/* An image as the clients make them: ext = with the dma-buf handle type. */
static VkImage image(ctx *c, VkImageTiling tiling, int ext, VkImageUsageFlags usage)
{
    VkExternalMemoryImageCreateInfo x = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, NULL, DMABUF};
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, ext ? &x : NULL, 0, VK_IMAGE_TYPE_2D, FMT, {W, H, 1}, 1, 1,
                            VK_SAMPLE_COUNT_1_BIT, tiling, usage, VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
    VkImage i = VK_NULL_HANDLE;
    if (D(vkCreateImage)(c->dev, &ci, NULL, &i) != VK_SUCCESS) return VK_NULL_HANDLE;
    return i;
}

/* A DRM-modifier dma-buf image: from a list holding LINEAR (pitch 0), or the explicit LINEAR modifier at pitch. */
static VkImage mod_image(ctx *c, VkDeviceSize pitch, VkResult *res)
{
    uint64_t lin = 0; /* DRM_FORMAT_MOD_LINEAR */
    VkImageDrmFormatModifierListCreateInfoEXT ml = {VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT, NULL, 1, &lin};
    VkSubresourceLayout plane = {0, 0, pitch, 0, 0};
    VkImageDrmFormatModifierExplicitCreateInfoEXT mx = {VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT, NULL, 0, 1, &plane};
    VkExternalMemoryImageCreateInfo x = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, pitch ? (const void *)&mx : (const void *)&ml,
                                         DMABUF};
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &x, 0, VK_IMAGE_TYPE_2D, FMT, {W, H, 1}, 1, 1,
                            VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
                            VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                            VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
    VkImage i = VK_NULL_HANDLE;
    *res = D(vkCreateImage)(c->dev, &ci, NULL, &i);
    return *res == VK_SUCCESS ? i : VK_NULL_HANDLE;
}

static VkResult export_image(ctx *c, VkImage i, VkDeviceMemory *mem, int *fd)
{
    VkMemoryRequirements req;
    D(vkGetImageMemoryRequirements)(c->dev, i, &req);
    VkMemoryDedicatedAllocateInfo ded = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, NULL, i, VK_NULL_HANDLE};
    VkExportMemoryAllocateInfo ex = {VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, &ded, DMABUF};
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &ex, req.size, pick(c, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    VkResult r = D(vkAllocateMemory)(c->dev, &ai, NULL, mem);
    if (r != VK_SUCCESS) return r;
    if ((r = D(vkBindImageMemory)(c->dev, i, *mem, 0)) != VK_SUCCESS) return r;
    VkMemoryGetFdInfoKHR gi = {VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR, NULL, *mem, DMABUF};
    return ((PFN_vkGetMemoryFdKHR)c->gdpa(c->dev, "vkGetMemoryFdKHR"))(c->dev, &gi, fd);
}

static VkResult import_image(ctx *c, VkImage i, int fd, VkDeviceMemory *mem)
{
    VkMemoryRequirements req;
    D(vkGetImageMemoryRequirements)(c->dev, i, &req);
    VkMemoryFdPropertiesKHR fp = {VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
    ((PFN_vkGetMemoryFdPropertiesKHR)c->gdpa(c->dev, "vkGetMemoryFdPropertiesKHR"))(c->dev, DMABUF, fd, &fp);
    uint32_t bits = req.memoryTypeBits & (fp.memoryTypeBits ? fp.memoryTypeBits : ~0u);
    VkMemoryDedicatedAllocateInfo ded = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, NULL, i, VK_NULL_HANDLE};
    VkImportMemoryFdInfoKHR imp = {VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR, &ded, DMABUF, dup(fd)};
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &imp, req.size, pick(c, bits ? bits : req.memoryTypeBits, 0)};
    VkResult r = D(vkAllocateMemory)(c->dev, &ai, NULL, mem);
    if (r != VK_SUCCESS) {
        close(imp.fd);
        *mem = VK_NULL_HANDLE;
        return r;
    }
    return D(vkBindImageMemory)(c->dev, i, *mem, 0);
}

static int clear(ctx *c, VkImage i, float r, float g, float b)
{
    VkCommandBuffer cb = begin(c);
    barrier(c, cb, i, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED);
    VkClearColorValue col = {.float32 = {r, g, b, 1.0f}};
    VkImageSubresourceRange rng = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    D(vkCmdClearColorImage)(cb, i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &col, 1, &rng);
    /* handed over as gamescope and Zink do */
    barrier(c, cb, i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, c->qf, VK_QUEUE_FAMILY_EXTERNAL);
    return submit(c, cb);
}

static uint32_t read_texel(ctx *c, VkImage i, uint32_t x, uint32_t y)
{
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE};
    VkBuffer b;
    D(vkCreateBuffer)(c->dev, &bci, NULL, &b);
    VkMemoryRequirements req;
    D(vkGetBufferMemoryRequirements)(c->dev, b, &req);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size,
                               pick(c, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    VkDeviceMemory m;
    D(vkAllocateMemory)(c->dev, &ai, NULL, &m);
    D(vkBindBufferMemory)(c->dev, b, m, 0);
    void *p = NULL;
    D(vkMapMemory)(c->dev, m, 0, VK_WHOLE_SIZE, 0, &p);
    VkCommandBuffer cb = begin(c);
    barrier(c, cb, i, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_QUEUE_FAMILY_EXTERNAL, c->qf);
    VkBufferImageCopy reg = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {(int32_t)x, (int32_t)y, 0}, {1, 1, 1}};
    D(vkCmdCopyImageToBuffer)(cb, i, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, b, 1, &reg);
    VkMemoryBarrier hb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
    D(vkCmdPipelineBarrier)(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hb, 0, NULL, 0, NULL);
    uint32_t v = submit(c, cb) ? *(volatile uint32_t *)p : 0xdeadbeefu;
    D(vkDestroyBuffer)(c->dev, b, NULL);
    D(vkFreeMemory)(c->dev, m, NULL);
    return v;
}

static int near(uint32_t got, uint32_t want)
{
    for (int k = 0; k < 4; k++) {
        int a = (int)((got >> (8 * k)) & 0xff), b = (int)((want >> (8 * k)) & 0xff);
        if (a - b > 1 || b - a > 1) return 0;
    }
    return 1;
}

static void send_fds(int sock, int *fds, int n)
{
    char data = 'f', cbuf[CMSG_SPACE(sizeof(int) * 4)];
    struct iovec iov = {&data, 1};
    struct msghdr mh = {0};
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    mh.msg_control = cbuf;
    mh.msg_controllen = CMSG_SPACE(sizeof(int) * (size_t)n);
    struct cmsghdr *cm = CMSG_FIRSTHDR(&mh);
    cm->cmsg_level = SOL_SOCKET;
    cm->cmsg_type = SCM_RIGHTS;
    cm->cmsg_len = CMSG_LEN(sizeof(int) * (size_t)n);
    memcpy(CMSG_DATA(cm), fds, sizeof(int) * (size_t)n);
    sendmsg(sock, &mh, 0);
}

static int recv_fds(int sock, int *fds, int n)
{
    char data, cbuf[CMSG_SPACE(sizeof(int) * 4)];
    struct iovec iov = {&data, 1};
    struct msghdr mh = {0};
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    mh.msg_control = cbuf;
    mh.msg_controllen = sizeof(cbuf);
    if (recvmsg(sock, &mh, 0) <= 0) return 0;
    struct cmsghdr *cm = CMSG_FIRSTHDR(&mh);
    if (!cm || cm->cmsg_len != CMSG_LEN(sizeof(int) * (size_t)n)) return 0;
    memcpy(fds, CMSG_DATA(cm), sizeof(int) * (size_t)n);
    return 1;
}

/* B8G8R8A8 of (r, g, b, 1) as a little-endian word */
#define BGRA(r, g, b) ((uint32_t)(b) | ((uint32_t)(g) << 8) | ((uint32_t)(r) << 16) | 0xff000000u)

static int importer(int sock, int foreign_refused, int ahb)
{
    who = "B";
    ctx cc, *c = &cc;
    int s = setup(c);
    if (s != 1) {
        check(0, "importer setup", NULL);
        return 1;
    }
    int fds[3] = {-1, -1, -1};
    uint64_t pitch = 0;
    if (!recv_fds(sock, fds, c->mods ? 3 : 2) || (c->mods && read(sock, &pitch, sizeof(pitch)) != sizeof(pitch))) {
        check(0, "fds received", NULL);
        return 1;
    }
    char d[160];
    if (c->mods) {
        /* Xwayland's pixmap imported as gamescope imports it: the explicit LINEAR modifier at its plane */
        VkResult r;
        VkImage xi = mod_image(c, pitch, &r);
        VkDeviceMemory xm = VK_NULL_HANDLE;
        if (xi) r = import_image(c, xi, fds[2], &xm);
        snprintf(d, sizeof(d), "%d, row pitch %llu", r, (unsigned long long)pitch);
        check(r == VK_SUCCESS, "the LINEAR-modifier dma-buf imported at its explicit layout", d);
        if (r == VK_SUCCESS) {
            uint32_t t = read_texel(c, xi, W / 2, H / 2);
            snprintf(d, sizeof(d), "texel 0x%08x, want 0x%08x", t, BGRA(0xbf, 0xbf, 0x40));
            check(near(t, BGRA(0xbf, 0xbf, 0x40)), "its colour read back", d);
        }
        if (xm) D(vkFreeMemory)(c->dev, xm, NULL);
        if (xi) D(vkDestroyImage)(c->dev, xi, NULL);
        if (ahb) {
            VkImage wi = mod_image(c, pitch + 256, &r);
            VkDeviceMemory wm = VK_NULL_HANDLE;
            if (wi) r = import_image(c, wi, fds[2], &wm);
            snprintf(d, sizeof(d), "%d", r);
            check(r != VK_SUCCESS, "an explicit LINEAR import at another row pitch refused", d);
            if (wm) D(vkFreeMemory)(c->dev, wm, NULL);
            if (wi) D(vkDestroyImage)(c->dev, wi, NULL);
        }
        close(fds[2]);
    }
    /* Zink's LINEAR image imported as gamescope imports one (OPTIMAL, modifier invalid) */
    VkImage gi = image(c, VK_IMAGE_TILING_OPTIMAL, 1, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    VkDeviceMemory gm = VK_NULL_HANDLE;
    VkResult r = gi ? import_image(c, gi, fds[1], &gm) : VK_ERROR_INITIALIZATION_FAILED;
    snprintf(d, sizeof(d), "%d", r);
    check(r == VK_SUCCESS, "Zink's LINEAR dma-buf imported into gamescope's OPTIMAL image", d);
    if (r == VK_SUCCESS) {
        uint32_t t = read_texel(c, gi, W / 2, H / 2);
        snprintf(d, sizeof(d), "texel 0x%08x, want 0x%08x", t, BGRA(0x40, 0xbf, 0x80));
        check(near(t, BGRA(0x40, 0xbf, 0x80)), "its colour read back", d);
    }
    /* gamescope's OPTIMAL export into a LINEAR image: laid out otherwise, refused */
    VkImage li = image(c, VK_IMAGE_TILING_LINEAR, 0, VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    VkDeviceMemory lm = VK_NULL_HANDLE;
    r = li ? import_image(c, li, fds[0], &lm) : VK_ERROR_INITIALIZATION_FAILED;
    snprintf(d, sizeof(d), "%d", r);
    check(r == VK_ERROR_INVALID_EXTERNAL_HANDLE, "an import laid out unlike its exporter is refused", d);
    /* fd properties */
    PFN_vkGetMemoryFdPropertiesKHR props = (PFN_vkGetMemoryFdPropertiesKHR)c->gdpa(c->dev, "vkGetMemoryFdPropertiesKHR");
    VkMemoryFdPropertiesKHR fp = {VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
    r = props(c->dev, DMABUF, fds[0], &fp);
    snprintf(d, sizeof(d), "%d, types 0x%x", r, fp.memoryTypeBits);
    check(r == VK_SUCCESS && fp.memoryTypeBits, "vkGetMemoryFdPropertiesKHR of the bridge's dma-buf", d);
    int foreign = memfd_create("not-a-dmabuf", MFD_CLOEXEC);
    if (ftruncate(foreign, 1 << 20) != 0) foreign = -1;
    if (foreign_refused) {
        fp.memoryTypeBits = 0;
        r = props(c->dev, DMABUF, foreign, &fp);
        snprintf(d, sizeof(d), "%d", r);
        check(r == VK_ERROR_INVALID_EXTERNAL_HANDLE, "vkGetMemoryFdPropertiesKHR of a foreign fd refused", d);
        VkImage fi = image(c, VK_IMAGE_TILING_OPTIMAL, 1, VK_IMAGE_USAGE_SAMPLED_BIT);
        VkDeviceMemory fm = VK_NULL_HANDLE;
        r = fi ? import_image(c, fi, foreign, &fm) : VK_ERROR_INITIALIZATION_FAILED;
        snprintf(d, sizeof(d), "%d", r);
        check(r == VK_ERROR_INVALID_EXTERNAL_HANDLE, "a foreign fd's import refused", d);
        if (fi) D(vkDestroyImage)(c->dev, fi, NULL);
    }
    if (gm) D(vkFreeMemory)(c->dev, gm, NULL);
    if (lm) D(vkFreeMemory)(c->dev, lm, NULL);
    D(vkDestroyImage)(c->dev, gi, NULL);
    if (li) D(vkDestroyImage)(c->dev, li, NULL);
    char ok = 1;
    if (write(sock, &ok, 1) != 1) {}
    D(vkDestroyCommandPool)(c->dev, c->pool, NULL);
    D(vkDestroyDevice)(c->dev, NULL);
    return failures != 0;
}

int main(int argc, char **argv)
{
    int foreign_refused = 0, ahb = 0;
    for (int a = 1; a < argc; a++) {
        foreign_refused |= !strcmp(argv[a], "foreign-refused");
        ahb |= !strcmp(argv[a], "ahb");
    }
    ctx cc, *c = &cc;
    int s = setup(c);
    if (s == 77) {
        printf("SKIP: no VK_KHR_external_memory_fd / VK_EXT_external_memory_dma_buf offered\n");
        return 77;
    }
    if (s != 1) {
        printf("FAILED: no device\n");
        return 1;
    }
    char d[160];
    snprintf(d, sizeof(d), "%s", has_ext(c, "VK_EXT_image_drm_format_modifier") ? "offered" : "not offered");
    printf("INFO [A] VK_EXT_image_drm_format_modifier %s\n", d);
    /* the queries gamescope and Zink ask */
    VkPhysicalDeviceExternalImageFormatInfo ei = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO, NULL, DMABUF};
    VkPhysicalDeviceImageFormatInfo2 ii = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2, &ei, FMT, VK_IMAGE_TYPE_2D,
                                           VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_SAMPLED_BIT, 0};
    VkExternalImageFormatProperties ep = {VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
    /* another answer first in the chain: the external one must come back in its place */
    VkSamplerYcbcrConversionImageFormatProperties yp = {VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_IMAGE_FORMAT_PROPERTIES, &ep};
    VkImageFormatProperties2 ip = {VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, &yp};
    VkResult r = I(vkGetPhysicalDeviceImageFormatProperties2)(c->pd, &ii, &ip);
    VkExternalMemoryFeatureFlags f = ep.externalMemoryProperties.externalMemoryFeatures;
    snprintf(d, sizeof(d), "%d, features 0x%x", r, f);
    check(r == VK_SUCCESS && (f & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT) && (f & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT),
          "a dma-buf image query: exportable and importable", d);
    VkPhysicalDeviceExternalBufferInfo bi = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO, NULL, 0, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                             DMABUF};
    VkExternalBufferProperties bp = {VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES};
    I(vkGetPhysicalDeviceExternalBufferProperties)(c->pd, &bi, &bp);
    snprintf(d, sizeof(d), "features 0x%x", bp.externalMemoryProperties.externalMemoryFeatures);
    check((bp.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT) != 0,
          "a dma-buf buffer query: exportable", d);
    /* gamescope's flippable image and Zink's shared one */
    VkImage gs = image(c, VK_IMAGE_TILING_OPTIMAL, 0,
                       VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    VkImage zk = image(c, VK_IMAGE_TILING_LINEAR, 1, VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    VkDeviceMemory gm = VK_NULL_HANDLE, zm = VK_NULL_HANDLE;
    int fds[3] = {-1, -1, -1};
    r = gs ? export_image(c, gs, &gm, &fds[0]) : VK_ERROR_INITIALIZATION_FAILED;
    snprintf(d, sizeof(d), "%d, fd %d", r, fds[0]);
    check(r == VK_SUCCESS && fds[0] >= 0, "gamescope's flippable image (no handle type) exported", d);
    r = zk ? export_image(c, zk, &zm, &fds[1]) : VK_ERROR_INITIALIZATION_FAILED;
    snprintf(d, sizeof(d), "%d, fd %d", r, fds[1]);
    check(r == VK_SUCCESS && fds[1] >= 0, "Zink's LINEAR dma-buf image exported", d);
    if (fds[0] < 0 || fds[1] < 0) {
        printf("FAILED: %d failures\n", failures);
        return 1;
    }
    /* Xwayland's pixmap through Zink: a DRM-modifier image from a list holding LINEAR, exported */
    VkImage xp = VK_NULL_HANDLE;
    VkDeviceMemory xm = VK_NULL_HANDLE;
    uint64_t pitch = 0;
    if (c->mods) {
        VkDrmFormatModifierPropertiesEXT mp[16];
        VkDrmFormatModifierPropertiesListEXT ml = {VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT, NULL, 16, mp};
        VkFormatProperties2 fp = {VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, &ml};
        I(vkGetPhysicalDeviceFormatProperties2)(c->pd, FMT, &fp);
        int lin = 0;
        for (uint32_t k = 0; k < ml.drmFormatModifierCount && k < 16; k++)
            lin |= mp[k].drmFormatModifier == 0 && mp[k].drmFormatModifierPlaneCount == 1 && mp[k].drmFormatModifierTilingFeatures;
        snprintf(d, sizeof(d), "%u modifier(s), LINEAR %s", ml.drmFormatModifierCount, lin ? "with features" : "missing");
        check(lin && (!ahb || ml.drmFormatModifierCount == 1), ahb ? "the format's modifiers: LINEAR alone" : "the format's modifiers hold LINEAR", d);
        /* Zink's own chain: VkFormatProperties3 first, the modifier list after it, the count asked, then the list. The
         * server took the list out for the driver's call; put back elsewhere in the chain the client read the answer as
         * malformed (S11 Ultra: Xwayland and Steam, "vkGetPhysicalDeviceFormatProperties2: malformed reply", no glamor). */
        VkDrmFormatModifierPropertiesListEXT zl = {VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT, NULL, 0, NULL};
        VkFormatProperties3 z3 = {VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3, &zl};
        VkFormatProperties2 zf = {VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, &z3};
        I(vkGetPhysicalDeviceFormatProperties2)(c->pd, FMT, &zf);
        const uint32_t zc = zl.drmFormatModifierCount;
        VkDrmFormatModifierPropertiesEXT zp[16];
        memset(zp, 0, sizeof(zp));
        zl.drmFormatModifierCount = zc < 16 ? zc : 16;
        zl.pDrmFormatModifierProperties = zp;
        I(vkGetPhysicalDeviceFormatProperties2)(c->pd, FMT, &zf);
        int zlin = 0;
        for (uint32_t k = 0; k < zl.drmFormatModifierCount && k < 16; k++) zlin |= zp[k].drmFormatModifier == 0 && zp[k].drmFormatModifierTilingFeatures;
        snprintf(d, sizeof(d), "count %u, then %u, LINEAR %s, linear features 0x%llx", zc, zl.drmFormatModifierCount, zlin ? "with features" : "missing",
                 (unsigned long long)z3.linearTilingFeatures);
        check(zc >= 1 && zlin && z3.linearTilingFeatures, "Zink's chain (format properties 3, then the modifier list) answered", d);
        VkPhysicalDeviceImageDrmFormatModifierInfoEXT mi = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT, NULL, 0,
                                                            VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
        VkPhysicalDeviceExternalImageFormatInfo mei = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO, &mi, DMABUF};
        VkPhysicalDeviceImageFormatInfo2 mii = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2, &mei, FMT, VK_IMAGE_TYPE_2D,
                                                VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, 0};
        VkExternalImageFormatProperties mep = {VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
        VkImageFormatProperties2 mip = {VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, &mep};
        r = I(vkGetPhysicalDeviceImageFormatProperties2)(c->pd, &mii, &mip);
        f = mep.externalMemoryProperties.externalMemoryFeatures;
        snprintf(d, sizeof(d), "%d, features 0x%x, max %ux%u", r, f, mip.imageFormatProperties.maxExtent.width,
                 mip.imageFormatProperties.maxExtent.height);
        check(r == VK_SUCCESS && (f & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) && mip.imageFormatProperties.maxExtent.width >= W,
              "a LINEAR-modifier dma-buf image query", d);
        xp = mod_image(c, 0, &r);
        if (xp) r = export_image(c, xp, &xm, &fds[2]);
        snprintf(d, sizeof(d), "%d, fd %d", r, fds[2]);
        check(r == VK_SUCCESS && fds[2] >= 0, "Xwayland's pixmap (a modifier list holding LINEAR) exported", d);
        VkImageDrmFormatModifierPropertiesEXT mpr = {VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT, NULL, ~0ull};
        r = xp ? ((PFN_vkGetImageDrmFormatModifierPropertiesEXT)c->gdpa(c->dev, "vkGetImageDrmFormatModifierPropertiesEXT"))(c->dev, xp, &mpr)
               : VK_ERROR_INITIALIZATION_FAILED;
        VkImageSubresource sub = {VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT, 0, 0};
        VkSubresourceLayout lay = {0};
        if (xp) D(vkGetImageSubresourceLayout)(c->dev, xp, &sub, &lay);
        pitch = lay.rowPitch;
        snprintf(d, sizeof(d), "%d, modifier 0x%llx, plane 0 offset %llu pitch %llu", r, (unsigned long long)mpr.drmFormatModifier,
                 (unsigned long long)lay.offset, (unsigned long long)lay.rowPitch);
        check(r == VK_SUCCESS && mpr.drmFormatModifier == 0 && lay.rowPitch >= W * 4, "its modifier LINEAR and its plane's layout", d);
        if (fds[2] < 0) {
            printf("FAILED: %d failures\n", failures);
            return 1;
        }
        clear(c, xp, 0.75f, 0.75f, 0.25f);
    }
    int again = -1;
    VkMemoryGetFdInfoKHR gfi = {VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR, NULL, zm, DMABUF};
    ((PFN_vkGetMemoryFdKHR)c->gdpa(c->dev, "vkGetMemoryFdKHR"))(c->dev, &gfi, &again);
    check(again >= 0 && lseek(again, 0, SEEK_END) >= (off_t)(W * H * 4), "a second export of the same memory: a dma-buf of its size", NULL);
    if (again >= 0) close(again);
    clear(c, gs, 0.75f, 0.25f, 0.5f);
    clear(c, zk, 0.25f, 0.75f, 0.5f);
    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        close(sv[0]);
        _exit(importer(sv[1], foreign_refused, ahb));
    }
    close(sv[1]);
    send_fds(sv[0], fds, c->mods ? 3 : 2);
    if (c->mods && write(sv[0], &pitch, sizeof(pitch)) != sizeof(pitch)) failures++;
    char done;
    if (read(sv[0], &done, 1) != 1) {}
    int st = 0;
    waitpid(pid, &st, 0);
    if (!WIFEXITED(st) || WEXITSTATUS(st)) failures++;
    /* the exporter frees after the importer: nothing left behind */
    D(vkFreeMemory)(c->dev, gm, NULL);
    D(vkFreeMemory)(c->dev, zm, NULL);
    D(vkDestroyImage)(c->dev, gs, NULL);
    D(vkDestroyImage)(c->dev, zk, NULL);
    if (xm) D(vkFreeMemory)(c->dev, xm, NULL);
    if (xp) D(vkDestroyImage)(c->dev, xp, NULL);
    close(fds[0]);
    close(fds[1]);
    if (fds[2] >= 0) close(fds[2]);
    D(vkDestroyCommandPool)(c->dev, c->pool, NULL);
    D(vkDestroyDevice)(c->dev, NULL);
    I(vkDestroyInstance)(c->inst, NULL);
    if (failures) printf("FAILED: %d failures\n", failures);
    else printf("OK: 0 failures\n");
    return failures != 0;
}
