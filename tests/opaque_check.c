/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * opaque_check: OPAQUE_FD external memory shared between two processes, as DXVK's D3D11 shared
 * resources use it through Wine (OMORI's Chromium: "Some formats not supported for resource
 * sharing" when the driver has none). Process A creates an exportable buffer and an exportable
 * optimal-tiled B8G8R8A8 image on dedicated memory, fills them, and sends the opaque fds over a
 * Unix socket; process B creates the same resources, imports the fds and reads them back through a
 * host-visible buffer. Also checks what the format queries say. Exit status 0 only if all pass.
 *   cc -O2 -Wall -Ithird_party/Vulkan-Headers-1.4.341/include -Icommon -Icommon/gen -Itests \
 *      -o host/out/opaque_check tests/opaque_check.c -ldl
 */
#define _GNU_SOURCE
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#define OPAQUE VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT
#define BOTH_WAYS (VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT | VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT)
#define BUF_SIZE (64u * 1024u)
#define IMG_W 256u
#define IMG_H 200u
#define IMG_FORMAT VK_FORMAT_B8G8R8A8_UNORM

static int failures, checks;
static const char *who = "A";

static void check(int ok, const char *what, const char *detail)
{
    checks++;
    if (!ok) failures++;
    printf("%s [%s] %s%s%s\n", ok ? "PASS" : "FAIL", who, what, detail && *detail ? ": " : "", detail ? detail : "");
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
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "opaque_check", 1, NULL, 0, VK_API_VERSION_1_1};
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
    const char *exts[] = {"VK_KHR_external_memory_fd"};
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, c->qf, 1, &prio};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &qci, 0, NULL, 1, exts, NULL};
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
    int ok = D(vkQueueSubmit)(c->queue, 1, &si, VK_NULL_HANDLE) == VK_SUCCESS && D(vkQueueWaitIdle)(c->queue) == VK_SUCCESS;
    D(vkFreeCommandBuffers)(c->dev, c->pool, 1, &cb);
    return ok;
}

/* A host-visible buffer, mapped. */
static VkBuffer host_buffer(ctx *c, VkDeviceSize size, VkDeviceMemory *mem, void **map)
{
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, size,
                              VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE};
    VkBuffer b;
    D(vkCreateBuffer)(c->dev, &bci, NULL, &b);
    VkMemoryRequirements req;
    D(vkGetBufferMemoryRequirements)(c->dev, b, &req);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size,
                               pick_type(c, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    D(vkAllocateMemory)(c->dev, &ai, NULL, mem);
    D(vkBindBufferMemory)(c->dev, b, *mem, 0);
    D(vkMapMemory)(c->dev, *mem, 0, VK_WHOLE_SIZE, 0, map);
    return b;
}

/* The shared resources, created the same way in both processes (opaque-fd rules). */
static VkBuffer shared_buffer(ctx *c)
{
    VkExternalMemoryBufferCreateInfo ext = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO, NULL, OPAQUE};
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &ext, 0, BUF_SIZE,
                              VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE};
    VkBuffer b = VK_NULL_HANDLE;
    VkResult r = D(vkCreateBuffer)(c->dev, &bci, NULL, &b);
    char d[32];
    snprintf(d, sizeof(d), "%d", r);
    check(r == VK_SUCCESS, "create buffer with OPAQUE_FD", d);
    return b;
}

static VkImage shared_image(ctx *c)
{
    /* DXVK's shared textures: 2D, optimal, sampled + render target + copies. */
    VkExternalMemoryImageCreateInfo ext = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, NULL, OPAQUE};
    VkImageCreateInfo ici = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &ext, 0, VK_IMAGE_TYPE_2D, IMG_FORMAT, {IMG_W, IMG_H, 1}, 1, 1,
                             VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL,
                             VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                             VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
    VkImage img = VK_NULL_HANDLE;
    VkResult r = D(vkCreateImage)(c->dev, &ici, NULL, &img);
    char d[32];
    snprintf(d, sizeof(d), "%d", r);
    check(r == VK_SUCCESS, "create optimal B8G8R8A8 image with OPAQUE_FD", d);
    return img;
}

static void image_barrier(ctx *c, VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to, uint32_t src_qf, uint32_t dst_qf)
{
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, VK_ACCESS_MEMORY_WRITE_BIT,
                              VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, from, to, src_qf, dst_qf, img,
                              {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    D(vkCmdPipelineBarrier)(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}

static uint32_t pattern(uint32_t i, uint32_t salt) { return (i * 2654435761u) ^ salt; }

/* What the queries say about OPAQUE_FD (DXVK asks for its shared formats this way). */
static void queries(ctx *c)
{
    VkPhysicalDeviceExternalBufferInfo bi = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO, NULL, 0,
                                             VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, OPAQUE};
    VkExternalBufferProperties ep = {VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES};
    I(vkGetPhysicalDeviceExternalBufferProperties)(c->pd, &bi, &ep);
    char d[128];
    const VkExternalMemoryProperties *p = &ep.externalMemoryProperties;
    snprintf(d, sizeof(d), "features 0x%x compatible 0x%x", p->externalMemoryFeatures, p->compatibleHandleTypes);
    check((p->externalMemoryFeatures & BOTH_WAYS) == BOTH_WAYS && (p->compatibleHandleTypes & OPAQUE), "buffers: OPAQUE_FD exportable+importable", d);
    VkFormat fmts[] = {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_SRGB};
    for (int i = 0; i < 4; i++) {
        VkPhysicalDeviceExternalImageFormatInfo ei = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO, NULL, OPAQUE};
        VkPhysicalDeviceImageFormatInfo2 ii = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2, &ei, fmts[i], VK_IMAGE_TYPE_2D,
                                               VK_IMAGE_TILING_OPTIMAL,
                                               VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                                   VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                                               0};
        VkExternalImageFormatProperties xp = {VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
        VkImageFormatProperties2 ip = {VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, &xp};
        VkResult r = I(vkGetPhysicalDeviceImageFormatProperties2)(c->pd, &ii, &ip);
        p = &xp.externalMemoryProperties;
        snprintf(d, sizeof(d), "format %d: %d, features 0x%x compatible 0x%x, max %ux%u", fmts[i], r, p->externalMemoryFeatures,
                 p->compatibleHandleTypes, ip.imageFormatProperties.maxExtent.width, ip.imageFormatProperties.maxExtent.height);
        check(r == VK_SUCCESS && (p->externalMemoryFeatures & BOTH_WAYS) == BOTH_WAYS && (p->compatibleHandleTypes & OPAQUE) &&
                  ip.imageFormatProperties.maxExtent.width >= IMG_W,
              "images: OPAQUE_FD exportable+importable", d);
    }
}

typedef struct {
    uint64_t buf_size, img_size;
    uint32_t buf_type, img_type;
} share_msg;

static int send_fds(int sock, const share_msg *m, int fd0, int fd1)
{
    char cbuf[CMSG_SPACE(2 * sizeof(int))];
    memset(cbuf, 0, sizeof(cbuf));
    struct iovec iov = {(void *)m, sizeof(*m)};
    struct msghdr mh = {NULL, 0, &iov, 1, cbuf, sizeof(cbuf), 0};
    struct cmsghdr *cm = CMSG_FIRSTHDR(&mh);
    cm->cmsg_level = SOL_SOCKET;
    cm->cmsg_type = SCM_RIGHTS;
    cm->cmsg_len = CMSG_LEN(2 * sizeof(int));
    int fds[2] = {fd0, fd1};
    memcpy(CMSG_DATA(cm), fds, sizeof(fds));
    return sendmsg(sock, &mh, 0) == (ssize_t)sizeof(*m);
}

static int recv_fds(int sock, share_msg *m, int *fd0, int *fd1)
{
    char cbuf[CMSG_SPACE(2 * sizeof(int))];
    struct iovec iov = {m, sizeof(*m)};
    struct msghdr mh = {NULL, 0, &iov, 1, cbuf, sizeof(cbuf), 0};
    if (recvmsg(sock, &mh, 0) != (ssize_t)sizeof(*m)) return 0;
    struct cmsghdr *cm = CMSG_FIRSTHDR(&mh);
    if (!cm || cm->cmsg_type != SCM_RIGHTS || cm->cmsg_len != CMSG_LEN(2 * sizeof(int))) return 0;
    int fds[2];
    memcpy(fds, CMSG_DATA(cm), sizeof(fds));
    *fd0 = fds[0];
    *fd1 = fds[1];
    return 1;
}

/* Dedicated memory for buffer or image: exported (import_fd < 0) or imported. */
static VkDeviceMemory shared_memory(ctx *c, VkBuffer b, VkImage img, int import_fd, VkDeviceSize *size, uint32_t *type)
{
    VkMemoryRequirements req;
    if (b) D(vkGetBufferMemoryRequirements)(c->dev, b, &req);
    else D(vkGetImageMemoryRequirements)(c->dev, img, &req);
    VkMemoryDedicatedAllocateInfo ded = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, NULL, img, b};
    VkExportMemoryAllocateInfo exp = {VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, &ded, OPAQUE};
    VkImportMemoryFdInfoKHR imp = {VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR, &ded, OPAQUE, import_fd};
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, import_fd < 0 ? (void *)&exp : (void *)&imp, req.size,
                               pick_type(c, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    if (import_fd >= 0) {
        /* Opaque fds: same size and type as the exporter's. */
        if (*size < req.size) printf("note [%s] exporter's size %llu < requirement %llu\n", who, (unsigned long long)*size,
                                     (unsigned long long)req.size);
        ai.allocationSize = *size;
        ai.memoryTypeIndex = *type;
    }
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkResult r = D(vkAllocateMemory)(c->dev, &ai, NULL, &mem);
    char d[64];
    snprintf(d, sizeof(d), "%d (%llu bytes, type %u)", r, (unsigned long long)ai.allocationSize, ai.memoryTypeIndex);
    check(r == VK_SUCCESS, import_fd < 0 ? (b ? "allocate exportable buffer memory" : "allocate exportable image memory")
                                         : (b ? "import buffer memory" : "import image memory"), d);
    if (r != VK_SUCCESS) {
        if (import_fd >= 0) close(import_fd);
        return VK_NULL_HANDLE;
    }
    r = b ? D(vkBindBufferMemory)(c->dev, b, mem, 0) : D(vkBindImageMemory)(c->dev, img, mem, 0);
    snprintf(d, sizeof(d), "%d", r);
    check(r == VK_SUCCESS, b ? "bind buffer" : "bind image", d);
    *size = ai.allocationSize;
    *type = ai.memoryTypeIndex;
    return mem;
}

static int exporter(int sock)
{
    ctx cc, *c = &cc;
    if (!setup(c)) {
        check(0, "Vulkan setup", NULL);
        return 1;
    }
    queries(c);
    VkBuffer buf = shared_buffer(c);
    VkImage img = shared_image(c);
    if (!buf || !img) return 1;
    share_msg m = {0};
    VkDeviceMemory bmem = shared_memory(c, buf, VK_NULL_HANDLE, -1, &m.buf_size, &m.buf_type);
    VkDeviceMemory imem = shared_memory(c, VK_NULL_HANDLE, img, -1, &m.img_size, &m.img_type);
    if (!bmem || !imem) return 1;
    /* Fill both from a staging buffer. */
    VkDeviceMemory smem;
    uint32_t *sp;
    VkBuffer staging = host_buffer(c, BUF_SIZE + IMG_W * IMG_H * 4, &smem, (void **)&sp);
    for (uint32_t i = 0; i < BUF_SIZE / 4; i++) sp[i] = pattern(i, 0xB0B0B0B0u);
    for (uint32_t i = 0; i < IMG_W * IMG_H; i++) sp[BUF_SIZE / 4 + i] = pattern(i, 0x1A1A1A1Au);
    VkCommandBuffer cb = begin(c);
    VkBufferCopy bc = {0, 0, BUF_SIZE};
    D(vkCmdCopyBuffer)(cb, staging, buf, 1, &bc);
    image_barrier(c, cb, img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED);
    VkBufferImageCopy ic = {BUF_SIZE, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {IMG_W, IMG_H, 1}};
    D(vkCmdCopyBufferToImage)(cb, staging, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &ic);
    /* Handed to the other process: released to the external queue family, layout GENERAL. */
    image_barrier(c, cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, c->qf, VK_QUEUE_FAMILY_EXTERNAL);
    check(submit(c, cb), "fill shared buffer and image", NULL);
    VkMemoryGetFdInfoKHR gb = {VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR, NULL, bmem, OPAQUE};
    VkMemoryGetFdInfoKHR gi = {VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR, NULL, imem, OPAQUE};
    int fb = -1, fi = -1;
    VkResult r1 = D(vkGetMemoryFdKHR)(c->dev, &gb, &fb);
    VkResult r2 = D(vkGetMemoryFdKHR)(c->dev, &gi, &fi);
    char d[64];
    snprintf(d, sizeof(d), "buffer %d (fd %d), image %d (fd %d)", r1, fb, r2, fi);
    check(r1 == VK_SUCCESS && r2 == VK_SUCCESS && fb >= 0 && fi >= 0, "vkGetMemoryFdKHR(OPAQUE_FD)", d);
    if (fb < 0 || fi < 0) return 1;
    /* What the fd is: a dma-buf when the bridge emulates opaque fds (RADV's own are dma-bufs too). */
    char path[64], link[128];
    snprintf(path, sizeof(path), "/proc/self/fd/%d", fi);
    ssize_t ln = readlink(path, link, sizeof(link) - 1);
    link[ln > 0 ? ln : 0] = 0;
    printf("note [A] the image's opaque fd is %s\n", link);
    check(send_fds(sock, &m, fb, fi), "send fds", NULL);
    close(fb);
    close(fi);
    /* The memory stays alive until B is done. */
    char done = 0;
    if (read(sock, &done, 1) != 1) done = 0;
    D(vkDeviceWaitIdle)(c->dev);
    return 0;
}

static int importer(int sock)
{
    who = "B";
    ctx cc, *c = &cc;
    if (!setup(c)) {
        check(0, "Vulkan setup", NULL);
        return 1;
    }
    share_msg m;
    int fb, fi;
    if (!recv_fds(sock, &m, &fb, &fi)) {
        check(0, "receive fds", NULL);
        return 1;
    }
    VkBuffer buf = shared_buffer(c);
    VkImage img = shared_image(c);
    if (!buf || !img) return 1;
    VkDeviceMemory bmem = shared_memory(c, buf, VK_NULL_HANDLE, fb, &m.buf_size, &m.buf_type);
    VkDeviceMemory imem = shared_memory(c, VK_NULL_HANDLE, img, fi, &m.img_size, &m.img_type);
    if (!bmem || !imem) return 1;
    VkDeviceMemory rmem;
    uint32_t *rp;
    VkBuffer rb = host_buffer(c, BUF_SIZE + IMG_W * IMG_H * 4, &rmem, (void **)&rp);
    memset(rp, 0, BUF_SIZE + IMG_W * IMG_H * 4);
    VkCommandBuffer cb = begin(c);
    VkBufferCopy bc = {0, 0, BUF_SIZE};
    D(vkCmdCopyBuffer)(cb, buf, rb, 1, &bc);
    image_barrier(c, cb, img, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_QUEUE_FAMILY_EXTERNAL, c->qf);
    VkBufferImageCopy ic = {BUF_SIZE, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {IMG_W, IMG_H, 1}};
    D(vkCmdCopyImageToBuffer)(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb, 1, &ic);
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
    D(vkCmdPipelineBarrier)(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    check(submit(c, cb), "read back shared buffer and image", NULL);
    uint32_t bad = 0, first = UINT32_MAX;
    for (uint32_t i = 0; i < BUF_SIZE / 4; i++)
        if (rp[i] != pattern(i, 0xB0B0B0B0u) && !bad++) first = i;
    char d[96];
    snprintf(d, sizeof(d), "%u of %u words differ (first at %u: 0x%08x)", bad, BUF_SIZE / 4, first, first < BUF_SIZE / 4 ? rp[first] : 0);
    check(!bad, "buffer contents seen by the importer", bad ? d : NULL);
    bad = 0;
    first = UINT32_MAX;
    for (uint32_t i = 0; i < IMG_W * IMG_H; i++)
        if (rp[BUF_SIZE / 4 + i] != pattern(i, 0x1A1A1A1Au) && !bad++) first = i;
    snprintf(d, sizeof(d), "%u of %u texels differ (first at %u: 0x%08x)", bad, IMG_W * IMG_H, first,
             first < IMG_W * IMG_H ? rp[BUF_SIZE / 4 + first] : 0);
    check(!bad, "image contents seen by the importer", bad ? d : NULL);
    D(vkDeviceWaitIdle)(c->dev);
    return 0;
}

int main(void)
{
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) return 1;
    pid_t pid = fork();
    if (pid == 0) {
        close(sv[0]);
        importer(sv[1]);
        char done = 1;
        if (write(sv[1], &done, 1) != 1) failures++;
        printf("[B] %d of %d checks failed\n", failures, checks);
        fflush(stdout);
        _exit(failures ? 1 : 0);
    }
    close(sv[1]);
    exporter(sv[0]);
    close(sv[0]); /* if A stopped early, B's receive ends here */
    int st = 0;
    waitpid(pid, &st, 0);
    int b_ok = WIFEXITED(st) && WEXITSTATUS(st) == 0;
    printf("%s %d/%d checks in A, importer %s\n", !failures && b_ok ? "OK:" : "FAILED:", checks - failures, checks, b_ok ? "passed" : "failed");
    return !failures && b_ok ? 0 : 1;
}
