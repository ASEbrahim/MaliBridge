/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * dma-buf external memory carried by AHardwareBuffers (VKB_EMU_DMABUF_AHB).
 *
 * gamescope enables VK_KHR_external_memory_fd and VK_EXT_external_memory_dma_buf unconditionally
 * and stops without them. Kirin's Mali drivers (a player log Kirin 9000E G78 r23p0 G76 r18p0,
 * a player log G76 r19p0 G52), the Maleoon 910 and ChromeOS's Venus have neither, no
 * VK_EXT_image_drm_format_modifier and no /dev/dma_heap, but they import AHardwareBuffers (the
 * ahb_share probe passed on a player log). Every exporter and importer of those dma-bufs is a client of
 * this one server (gamescope, Xwayland and Steam through Zink, the bridge's own swapchains) or
 * DroidDeck's compositor in the same process, so a dma-buf only has to come back here as the same
 * pages:
 *   - clients are offered the two extensions and VK_EXT_image_drm_format_modifier with the LINEAR
 *     modifier only (vkb_caps.c); the driver's own three are not shown. A LINEAR-modifier image is a
 *     plain LINEAR one (server_emu_layer.c): Zink exports Xwayland's pixmaps only through the
 *     modifier extension, and without it no X11 window reached gamescope;
 *   - memory allocated to be exported as a dma-buf gets its pages from a BLOB AHardwareBuffer of
 *     its size, imported into the driver (VkImportAndroidHardwareBufferInfoANDROID, not dedicated),
 *     and the resource is bound to it as to any memory. vkGetMemoryFdKHR hands out the buffer's own
 *     dma-buf (a dup); the register (server_dab_reg.c) keeps the pages with the image they were made
 *     for;
 *   - an imported dma-buf is looked up there by its open file and the same buffer imported again;
 *     an fd the bridge did not hand out is refused (VK_ERROR_INVALID_EXTERNAL_HANDLE), or goes to
 *     the driver where it has dma-buf import of its own (the force switch on a capable GPU);
 *   - an image made with the dma-buf handle type is made without it, LINEAR where the driver can
 *     (Zink and the bridge's swapchains export LINEAR images without modifiers, gamescope imports
 *     them as OPTIMAL: both then lay them out alike); an import whose image is laid out unlike the
 *     exporter's (row pitch, size, extent) is refused rather than shown as garbage;
 *   - DroidDeck's compositor asks vkb_dmabuf_ahb_lookup for the buffer and the exporter's image
 *     and makes the same image over it (vkb_dmabuf_ahb.h).
 * Images bound to BLOB AHardwareBuffer memory are outside the letter of the spec (they are not made
 * with the AHB handle type). The start's check (vkb_dab_selfcheck) runs exactly that - one buffer
 * imported twice, a LINEAR and an OPTIMAL image on each, a clear through one read back through the
 * other - and the path is offered only where it passed (LINEAR images only where LINEAR passed).
 * VKBRIDGE_DMABUF_VIA_AHB=1 takes this path on a GPU with dma-bufs of its own (to check it on a
 * capable tablet), =0 never takes it; also from <cache dir>/env, read once per server start.
 * Host tests: VKBRIDGE_TEST_EXPORT_PAGES=driver - the pages are the desktop driver's own dma-bufs
 * (made, exported, freed: the fd keeps them), imported as dma-bufs; the rest is as on Android.
 * VKBRIDGE_FAKE_MISSING=dmabufext imitates a driver without dma-buf memory for clients.
 */
#define _GNU_SOURCE
#include "vkb_server.h"
#include "server_emu.h"
#include "server_dab.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __ANDROID__
#include <vulkan/vulkan_android.h>
#endif

#define DMABUF VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT
#define BOTH_WAYS (VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT | VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT)

static int dab_on;          /* decided at the self-test (vkb_dab_wanted) */
static int dab_forced;      /* VKBRIDGE_DMABUF_VIA_AHB=1 on a driver with dma-bufs of its own */
static int check_ok = -1;   /* the start's check: -1 not run, 0 failed, 1 passed */
static int check_linear;    /* LINEAR images passed it too */
static char check_text[400];

/* ------------------------------------------------------------------ the pages */

#ifdef __ANDROID__
typedef struct {
    uint32_t width, height, layers, format;
    uint64_t usage;
    uint32_t stride, rfu0;
    uint64_t rfu1;
} dab_ahb_desc;
typedef struct {
    int version, numFds, numInts;
    int data[0];
} dab_native_handle;
static struct {
    int state;
    int (*allocate)(const dab_ahb_desc *, struct AHardwareBuffer **);
    void (*acquire)(struct AHardwareBuffer *);
    void (*release)(struct AHardwareBuffer *);
    const dab_native_handle *(*handle)(const struct AHardwareBuffer *);
} ahbf;
#define AHB_FORMAT_BLOB 0x21
#define AHB_CPU_RARELY (2ull | (2ull << 4))
#define AHB_CPU_OFTEN (3ull | (3ull << 4))
#define AHB_GPU_DATA_BUFFER (1ull << 24)
static uint64_t ahb_cpu = AHB_CPU_RARELY;

static void ahb_acquire_cb(void *a) { ahbf.acquire((struct AHardwareBuffer *)a); }
static void ahb_release_cb(void *a) { ahbf.release((struct AHardwareBuffer *)a); }

static int ahb_ok(void)
{
    if (ahbf.state) return ahbf.state > 0;
    ahbf.state = -1;
    void *h = dlopen("libnativewindow.so", RTLD_NOW);
    if (!h) h = dlopen("libandroid.so", RTLD_NOW);
    if (!h) return 0;
    *(void **)&ahbf.allocate = dlsym(h, "AHardwareBuffer_allocate");
    *(void **)&ahbf.acquire = dlsym(h, "AHardwareBuffer_acquire");
    *(void **)&ahbf.release = dlsym(h, "AHardwareBuffer_release");
    *(void **)&ahbf.handle = dlsym(h, "AHardwareBuffer_getNativeHandle");
    if (ahbf.allocate && ahbf.acquire && ahbf.release && ahbf.handle) ahbf.state = 1;
    if (ahbf.state > 0) dab_reg_set_ahb_ops(ahb_acquire_cb, ahb_release_cb);
    return ahbf.state > 0;
}
#endif

#ifndef __ANDROID__
/* Host tests: the desktop driver's own dma-bufs stand in for the AHardwareBuffers. */
static int host_pages(void)
{
    static int on = -1;
    if (on < 0) {
        const char *v = vkb_srv_env_global("VKBRIDGE_TEST_EXPORT_PAGES");
        on = v && !strcmp(v, "driver");
    }
    return on;
}
#endif

typedef struct {
    void *ahb; /* Android: the buffer (one reference) */
    int fd;    /* its dma-buf (owned) */
    uint64_t bytes;
} dab_pages;

/* What a device needs to take the pages in. */
typedef struct {
    VkDevice dev;
    const vkb_dispatch *dt;
    VkPhysicalDevice pd;
    const vkb_dispatch *idt;
#ifdef __ANDROID__
    PFN_vkGetAndroidHardwareBufferPropertiesANDROID ahb_props;
#endif
} dab_dev_fns;

static int pages_new(const dab_dev_fns *f, uint64_t bytes, uint32_t type, dab_pages *p)
{
    memset(p, 0, sizeof(*p));
    p->fd = -1;
    p->bytes = bytes;
#ifdef __ANDROID__
    (void)f;
    (void)type;
    if (!ahb_ok() || bytes > 0xFFFFFFFFull) return 0;
    dab_ahb_desc d = {(uint32_t)bytes, 1, 1, AHB_FORMAT_BLOB, ahb_cpu | AHB_GPU_DATA_BUFFER, 0, 0, 0};
    struct AHardwareBuffer *ahb = NULL;
    int ar = ahbf.allocate(&d, &ahb);
    if (ar != 0 || !ahb) {
        VKB_ERR("dma-buf over AHardwareBuffer: allocating %llu bytes failed: %d", (unsigned long long)bytes, ar);
        return 0;
    }
    const dab_native_handle *nh = ahbf.handle(ahb);
    int pick = -1;
    for (int i = 0; nh && i < nh->numFds && pick < 0; i++) {
        off_t end = lseek(nh->data[i], 0, SEEK_END);
        if (end >= (off_t)bytes) pick = nh->data[i];
    }
    p->fd = pick >= 0 ? fcntl(pick, F_DUPFD_CLOEXEC, 0) : -1;
    if (p->fd < 0) {
        VKB_ERR("dma-buf over AHardwareBuffer: no dma-buf of %llu bytes among the buffer's %d fd(s)", (unsigned long long)bytes,
                nh ? nh->numFds : -1);
        ahbf.release(ahb);
        return 0;
    }
    p->ahb = ahb;
    return 1;
#else
    if (!host_pages() || !f->dt->vkGetMemoryFdKHR) return 0;
    VkExportMemoryAllocateInfo ex = {VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, NULL, DMABUF};
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &ex, bytes, type};
    VkDeviceMemory mem;
    if (f->dt->vkAllocateMemory(f->dev, &ai, NULL, &mem) != VK_SUCCESS) return 0;
    VkMemoryGetFdInfoKHR gi = {VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR, NULL, mem, DMABUF};
    if (f->dt->vkGetMemoryFdKHR(f->dev, &gi, &p->fd) != VK_SUCCESS) p->fd = -1;
    f->dt->vkFreeMemory(f->dev, mem, NULL);
    return p->fd >= 0;
#endif
}

static void pages_free(dab_pages *p)
{
#ifdef __ANDROID__
    if (p->ahb) ahbf.release((struct AHardwareBuffer *)p->ahb);
#endif
    if (p->fd >= 0) close(p->fd);
    p->ahb = NULL;
    p->fd = -1;
}

/* The memory types and the allocation size an import of the pages takes. */
static VkResult pages_props(const dab_dev_fns *f, void *ahb, int fd, uint64_t bytes, uint32_t *bits, uint64_t *size)
{
#ifdef __ANDROID__
    (void)fd;
    (void)bytes;
    if (!f->ahb_props) return VK_ERROR_EXTENSION_NOT_PRESENT;
    VkAndroidHardwareBufferPropertiesANDROID hp = {VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID};
    VkResult r = f->ahb_props(f->dev, (struct AHardwareBuffer *)ahb, &hp);
    *bits = hp.memoryTypeBits;
    *size = hp.allocationSize;
    return r;
#else
    (void)ahb;
    if (!f->dt->vkGetMemoryFdPropertiesKHR) return VK_ERROR_EXTENSION_NOT_PRESENT;
    VkMemoryFdPropertiesKHR fp = {VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
    VkResult r = f->dt->vkGetMemoryFdPropertiesKHR(f->dev, DMABUF, fd, &fp);
    *bits = fp.memoryTypeBits;
    *size = bytes;
    return r;
#endif
}

static uint32_t ordinary_types(const dab_dev_fns *f)
{
    VkPhysicalDeviceMemoryProperties mp;
    f->idt->vkGetPhysicalDeviceMemoryProperties(f->pd, &mp);
    uint32_t bits = 0;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if (!(mp.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT | VK_MEMORY_PROPERTY_PROTECTED_BIT))) bits |= 1u << i;
    return bits;
}

/* The pages imported into the device as memory a resource of `want` types and `need` bytes binds
 * to. chain: what else the allocation carries (flags, priority), without dedicated/export/import
 * structures. *type: asked for, then used. */
static VkResult pages_import(const dab_dev_fns *f, void *ahb, int fd, uint64_t bytes, uint32_t want, uint64_t need, const void *chain,
                             uint32_t *type, VkDeviceMemory *mem, char *why, size_t cap)
{
    uint32_t pbits = 0;
    uint64_t size = 0;
    VkResult r = pages_props(f, ahb, fd, bytes, &pbits, &size);
    if (r != VK_SUCCESS) {
        snprintf(why, cap, "the driver will not describe the buffer (%d)", r);
        return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    }
    uint32_t bits = pbits & want & ordinary_types(f);
    if (!bits) {
        snprintf(why, cap, "no memory type takes both the buffer (0x%x) and the resource (0x%x)", pbits, want);
        return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    }
    if (need > size) {
        snprintf(why, cap, "the resource needs %llu bytes, the buffer has %llu", (unsigned long long)need, (unsigned long long)size);
        return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    }
    if (*type >= 32 || !(bits & (1u << *type))) *type = (uint32_t)__builtin_ctz(bits);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, size, *type};
#ifdef __ANDROID__
    (void)fd;
    VkImportAndroidHardwareBufferInfoANDROID imp = {VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID, chain,
                                                    (struct AHardwareBuffer *)ahb};
    ai.pNext = &imp;
    r = f->dt->vkAllocateMemory(f->dev, &ai, NULL, mem);
#else
    (void)ahb;
    VkImportMemoryFdInfoKHR imp = {VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR, chain, DMABUF, fcntl(fd, F_DUPFD_CLOEXEC, 0)};
    ai.pNext = &imp;
    r = imp.fd >= 0 ? f->dt->vkAllocateMemory(f->dev, &ai, NULL, mem) : VK_ERROR_TOO_MANY_OBJECTS;
    if (r != VK_SUCCESS && imp.fd >= 0) close(imp.fd);
#endif
    if (r != VK_SUCCESS) snprintf(why, cap, "the driver refused the import (%d, type %u, %llu bytes)", r, *type, (unsigned long long)size);
    return r;
}

/* ------------------------------------------------------------------ the start's check */

#define CHK_W 64u
#define CHK_H 64u
#define CHK_FMT VK_FORMAT_B8G8R8A8_UNORM

static VkCommandBuffer chk_begin(const dab_dev_fns *f, VkCommandPool pool)
{
    VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VkCommandBuffer cb = VK_NULL_HANDLE;
    if (f->dt->vkAllocateCommandBuffers(f->dev, &ai, &cb) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
    f->dt->vkBeginCommandBuffer(cb, &bi);
    return cb;
}

static VkResult chk_submit(const dab_dev_fns *f, VkQueue q, VkCommandPool pool, VkCommandBuffer cb)
{
    f->dt->vkEndCommandBuffer(cb);
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cb, 0, NULL};
    VkResult r = f->dt->vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE);
    if (r == VK_SUCCESS) r = f->dt->vkQueueWaitIdle(q);
    f->dt->vkFreeCommandBuffers(f->dev, pool, 1, &cb);
    return r;
}

static void chk_barrier(const dab_dev_fns *f, VkCommandBuffer cb, VkImage i, VkImageLayout from, VkImageLayout to, VkAccessFlags src,
                        VkAccessFlags dst)
{
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, src, dst, from, to, VK_QUEUE_FAMILY_IGNORED,
                              VK_QUEUE_FAMILY_IGNORED, i, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    f->dt->vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}

/* One tiling: a buffer imported twice, an image on each; a clear through the first read back
 * through the second. 1 passed, 0 failed, -1 the driver has no such image (why says). */
static int chk_tiling(const dab_dev_fns *f, VkQueue q, VkCommandPool pool, VkImageTiling tiling, char *why, size_t cap)
{
    VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    VkImageFormatProperties fp;
    if (f->idt->vkGetPhysicalDeviceImageFormatProperties(f->pd, CHK_FMT, VK_IMAGE_TYPE_2D, tiling, usage, 0, &fp) != VK_SUCCESS) {
        snprintf(why, cap, "no such image");
        return -1;
    }
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, CHK_FMT, {CHK_W, CHK_H, 1}, 1, 1,
                            VK_SAMPLE_COUNT_1_BIT, tiling, usage, VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
    VkImage img[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkDeviceMemory mem[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkBuffer rb = VK_NULL_HANDLE;
    VkDeviceMemory rbm = VK_NULL_HANDLE;
    dab_pages p = {NULL, -1, 0};
    int ok = 0;
    VkMemoryRequirements req;
    for (int k = 0; k < 2; k++)
        if (f->dt->vkCreateImage(f->dev, &ci, NULL, &img[k]) != VK_SUCCESS) {
            img[k] = VK_NULL_HANDLE;
            snprintf(why, cap, "image refused");
            goto out;
        }
    f->dt->vkGetImageMemoryRequirements(f->dev, img[0], &req);
    if (!pages_new(f, (req.size + 4095) & ~4095ull, (uint32_t)__builtin_ctz(req.memoryTypeBits ? req.memoryTypeBits : 1), &p)) {
        snprintf(why, cap, "no buffer of %llu bytes", (unsigned long long)req.size);
        goto out;
    }
    for (int k = 0; k < 2; k++) {
        uint32_t type = UINT32_MAX;
        VkResult r = pages_import(f, p.ahb, p.fd, p.bytes, req.memoryTypeBits, req.size, NULL, &type, &mem[k], why, cap);
        if (r != VK_SUCCESS) {
            mem[k] = VK_NULL_HANDLE;
            goto out;
        }
        if ((r = f->dt->vkBindImageMemory(f->dev, img[k], mem[k], 0)) != VK_SUCCESS) {
            snprintf(why, cap, "binding refused (%d)", r);
            goto out;
        }
    }
    if (tiling == VK_IMAGE_TILING_LINEAR) {
        VkImageSubresource sub = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
        VkSubresourceLayout l0, l1;
        f->dt->vkGetImageSubresourceLayout(f->dev, img[0], &sub, &l0);
        f->dt->vkGetImageSubresourceLayout(f->dev, img[1], &sub, &l1);
        if (l0.rowPitch != l1.rowPitch || l0.offset != l1.offset) {
            snprintf(why, cap, "the two images are laid out apart (pitch %llu, %llu)", (unsigned long long)l0.rowPitch,
                     (unsigned long long)l1.rowPitch);
            goto out;
        }
    }
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE};
    if (f->dt->vkCreateBuffer(f->dev, &bci, NULL, &rb) != VK_SUCCESS) {
        rb = VK_NULL_HANDLE;
        snprintf(why, cap, "no readback buffer");
        goto out;
    }
    VkMemoryRequirements breq;
    f->dt->vkGetBufferMemoryRequirements(f->dev, rb, &breq);
    VkPhysicalDeviceMemoryProperties mp;
    f->idt->vkGetPhysicalDeviceMemoryProperties(f->pd, &mp);
    uint32_t ht = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount && ht == UINT32_MAX; i++)
        if ((breq.memoryTypeBits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
            ht = i;
    VkMemoryAllocateInfo bai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, breq.size, ht};
    void *map = NULL;
    if (ht == UINT32_MAX || f->dt->vkAllocateMemory(f->dev, &bai, NULL, &rbm) != VK_SUCCESS) {
        rbm = VK_NULL_HANDLE;
        snprintf(why, cap, "no readback memory");
        goto out;
    }
    f->dt->vkBindBufferMemory(f->dev, rb, rbm, 0);
    f->dt->vkMapMemory(f->dev, rbm, 0, VK_WHOLE_SIZE, 0, &map);
    /* the first image cleared and left in GENERAL, as an exporter hands its image over */
    VkCommandBuffer cb = chk_begin(f, pool);
    if (!cb) goto out;
    chk_barrier(f, cb, img[0], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkClearColorValue col = {.float32 = {0.25f, 0.5f, 0.75f, 1.0f}};
    VkImageSubresourceRange rng = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    f->dt->vkCmdClearColorImage(cb, img[0], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &col, 1, &rng);
    chk_barrier(f, cb, img[0], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_MEMORY_READ_BIT);
    VkResult r = chk_submit(f, q, pool, cb);
    /* the second read from GENERAL, as an importer takes it */
    cb = r == VK_SUCCESS ? chk_begin(f, pool) : VK_NULL_HANDLE;
    if (!cb) {
        snprintf(why, cap, "the clear failed (%d)", r);
        goto out;
    }
    chk_barrier(f, cb, img[1], VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_MEMORY_WRITE_BIT,
                VK_ACCESS_TRANSFER_READ_BIT);
    VkBufferImageCopy reg = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {CHK_W / 2, CHK_H / 2, 0}, {1, 1, 1}};
    f->dt->vkCmdCopyImageToBuffer(cb, img[1], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb, 1, &reg);
    VkMemoryBarrier hb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
    f->dt->vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hb, 0, NULL, 0, NULL);
    r = chk_submit(f, q, pool, cb);
    uint32_t got = r == VK_SUCCESS && map ? *(volatile uint32_t *)map : 0;
    const uint32_t want = 0xff4080bfu; /* B8G8R8A8 (0.25, 0.5, 0.75, 1): bytes bf 80 40 ff */
    ok = 1;
    for (int k = 0; k < 4; k++) {
        int a = (int)((got >> (8 * k)) & 0xff), b = (int)((want >> (8 * k)) & 0xff);
        if (a - b > 1 || b - a > 1) ok = 0;
    }
    snprintf(why, cap, "texel 0x%08x%s", got, ok ? "" : r != VK_SUCCESS ? " (the copy failed)" : " WRONG");
out:
    if (rb) f->dt->vkDestroyBuffer(f->dev, rb, NULL);
    if (rbm) f->dt->vkFreeMemory(f->dev, rbm, NULL);
    for (int k = 0; k < 2; k++) {
        if (img[k]) f->dt->vkDestroyImage(f->dev, img[k], NULL);
        if (mem[k]) f->dt->vkFreeMemory(f->dev, mem[k], NULL);
    }
    pages_free(&p);
    return ok;
}

/* Two buffers told apart, and one told from itself under another fd: the register's identity test
 * on this kernel (a shared inode is noted for the register). */
static int chk_identity(const dab_dev_fns *f, char *why, size_t cap)
{
    dab_pages a, b;
    if (!pages_new(f, 65536, 0, &a)) {
        snprintf(why, cap, "no buffer");
        return 0;
    }
    if (!pages_new(f, 65536, 0, &b)) {
        pages_free(&a);
        snprintf(why, cap, "no second buffer");
        return 0;
    }
    struct stat sa, sb;
    int shared = fstat(a.fd, &sa) == 0 && fstat(b.fd, &sb) == 0 && sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino;
    if (shared) dab_reg_note_shared_inode(1);
    int d = fcntl(a.fd, F_DUPFD_CLOEXEC, 0);
    int self = d >= 0 && dab_same_file(a.fd, d);
    int other = dab_same_file(a.fd, b.fd);
    if (d >= 0) close(d);
    pages_free(&a);
    pages_free(&b);
    snprintf(why, cap, "%s; a dup %s, another buffer %s", shared ? "every dma-buf on one inode (told apart by flock)" : "an inode per dma-buf",
             self ? "matched" : "NOT matched", other ? "MATCHED" : "told apart");
    return self && !other;
}

static int driver_has(const vkb_dispatch *idt, VkPhysicalDevice pd, const char *name)
{
    uint32_t n = 0;
    idt->vkEnumerateDeviceExtensionProperties(pd, NULL, &n, NULL);
    VkExtensionProperties *e = calloc(n ? n : 1, sizeof(*e));
    idt->vkEnumerateDeviceExtensionProperties(pd, NULL, &n, e);
    int has = 0;
    for (uint32_t i = 0; i < n && !has; i++) has = !strcmp(e[i].extensionName, name);
    free(e);
    return has;
}

/* The switch: -1 automatic, 0 off, 1 forced. */
static int switch_value(void)
{
    const char *v = vkb_srv_env_global("VKBRIDGE_DMABUF_VIA_AHB");
    if (v && !strcmp(v, "0")) return 0;
    if (v && !strcmp(v, "1")) return 1;
    return -1;
}

/* Whether this driver needs the path (it lacks dma-buf memory) and the pages can be had here. */
static int candidate(const vkb_dispatch *idt, VkPhysicalDevice pd, int *needed, const char **why)
{
    VkPhysicalDeviceProperties props;
    idt->vkGetPhysicalDeviceProperties(pd, &props);
    /* VKBRIDGE_FAKE_MISSING=dmabufext (host tests): as a driver without dma-buf memory for its clients,
     * the server's own memory sharing left as it is (the Kirin phones share through AHBs instead) */
    int fake = vkb_emu_faked("dmabufext");
    int fd = driver_has(idt, pd, "VK_KHR_external_memory_fd") && !vkb_emu_faked("VK_KHR_external_memory_fd") && !fake;
    int dmabuf = driver_has(idt, pd, "VK_EXT_external_memory_dma_buf") && !vkb_emu_faked("VK_EXT_external_memory_dma_buf") && !fake;
    *needed = !(fd && dmabuf);
    *why = "";
    if (props.apiVersion < VK_API_VERSION_1_1) return *why = "a Vulkan 1.0 driver", 0;
#ifdef __ANDROID__
    if (!driver_has(idt, pd, "VK_ANDROID_external_memory_android_hardware_buffer")) return *why = "no AHardwareBuffer import in the driver", 0;
    if (!driver_has(idt, pd, "VK_EXT_queue_family_foreign")) return *why = "no VK_EXT_queue_family_foreign in the driver", 0;
    if (!ahb_ok()) return *why = "no AHardwareBuffer functions", 0;
#else
    if (!host_pages()) return *why = "no AHardwareBuffers on this system (host tests: VKBRIDGE_TEST_EXPORT_PAGES=driver)", 0;
    if (!driver_has(idt, pd, "VK_KHR_external_memory_fd") || !driver_has(idt, pd, "VK_EXT_external_memory_dma_buf"))
        return *why = "host pages need the driver's dma-bufs", 0;
#endif
    return 1;
}

const char *const *vkb_dab_selftest_exts(uint32_t *n)
{
#ifdef __ANDROID__
    static const char *const e[] = {"VK_ANDROID_external_memory_android_hardware_buffer", "VK_EXT_queue_family_foreign"};
    *n = 2;
    return e;
#else
    static const char *const e[] = {"VK_KHR_external_memory_fd", "VK_EXT_external_memory_dma_buf"};
    *n = 2;
    return e;
#endif
}

void vkb_dab_selfcheck(const vkb_dispatch *idt, VkPhysicalDevice pd, VkDevice dev, const vkb_dispatch *dt, VkQueue q, uint32_t qf)
{
    int sw = switch_value(), needed;
    const char *why;
    if (sw == 0 || !candidate(idt, pd, &needed, &why) || (!needed && sw != 1)) return;
    dab_dev_fns f = {dev, dt, pd, idt};
#ifdef __ANDROID__
    f.ahb_props = vkb_mem_gdpa ? (PFN_vkGetAndroidHardwareBufferPropertiesANDROID)vkb_mem_gdpa(dev, "vkGetAndroidHardwareBufferPropertiesANDROID")
                               : NULL;
#endif
    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, 0, qf};
    VkCommandPool pool = VK_NULL_HANDLE;
    if (dt->vkCreateCommandPool(dev, &pci, NULL, &pool) != VK_SUCCESS) return;
    char lin[120], opt[120], ident[160], cpu[60] = "";
    int l = chk_tiling(&f, q, pool, VK_IMAGE_TILING_LINEAR, lin, sizeof(lin));
    int o = chk_tiling(&f, q, pool, VK_IMAGE_TILING_OPTIMAL, opt, sizeof(opt));
#ifdef __ANDROID__
    if (o <= 0 && ahb_cpu == AHB_CPU_RARELY) {
        /* some grallocs refuse BLOB buffers with CPU usage "rarely" (server_memory.c's export check) */
        ahb_cpu = AHB_CPU_OFTEN;
        l = chk_tiling(&f, q, pool, VK_IMAGE_TILING_LINEAR, lin, sizeof(lin));
        o = chk_tiling(&f, q, pool, VK_IMAGE_TILING_OPTIMAL, opt, sizeof(opt));
        snprintf(cpu, sizeof(cpu), "; CPU usage often");
        if (o <= 0) ahb_cpu = AHB_CPU_RARELY;
    }
#endif
    int id = chk_identity(&f, ident, sizeof(ident));
    dt->vkDestroyCommandPool(dev, pool, NULL);
    check_linear = l == 1;
    check_ok = o == 1 && id;
    snprintf(check_text, sizeof(check_text), "OPTIMAL %s: %s; LINEAR %s: %s; %s%s", o == 1 ? "passed" : "FAILED", opt,
             l == 1 ? "passed" : l < 0 ? "not offered" : "FAILED", lin, ident, cpu);
    if (check_ok) VKB_INFO("dma-buf over AHardwareBuffer: the start's check passed (%s)", check_text);
    else VKB_WARN("dma-buf over AHardwareBuffer: the start's check FAILED (%s); not offered", check_text);
}

const char *vkb_dab_check_text(void)
{
    return check_text;
}

int vkb_dab_wanted(const vkb_dispatch *idt, VkPhysicalDevice pd)
{
    int sw = switch_value(), needed;
    const char *why;
    if (sw == 0) {
        VKB_ONCE(VKB_LOG_INFO, "dma-buf over AHardwareBuffer: off (VKBRIDGE_DMABUF_VIA_AHB=0)");
        return 0;
    }
    if (!candidate(idt, pd, &needed, &why)) {
        if (needed || sw == 1) VKB_ONCE(VKB_LOG_INFO, "dma-buf over AHardwareBuffer: not possible (%s)", why);
        return 0;
    }
    if (!needed && sw != 1) return 0;
    if (check_ok != 1) {
        VKB_ONCE(VKB_LOG_WARN, "dma-buf over AHardwareBuffer: wanted (%s) but the start's check %s; gamescope will find no dma-buf memory",
                 needed ? "the driver has no dma-buf memory" : "VKBRIDGE_DMABUF_VIA_AHB=1", check_ok < 0 ? "did not run" : "failed");
        return 0;
    }
    __atomic_store_n(&dab_forced, !needed, __ATOMIC_RELAXED);
    __atomic_store_n(&dab_on, 1, __ATOMIC_RELEASE);
    VKB_ONCE(VKB_LOG_INFO, "dma-buf over AHardwareBuffer: on (%s): clients get VK_KHR_external_memory_fd, "
                           "VK_EXT_external_memory_dma_buf and the modifier extension (LINEAR %s); dma-buf images made %s",
             needed ? "the driver has no dma-buf memory" : "VKBRIDGE_DMABUF_VIA_AHB=1 on a driver with its own",
             check_linear ? "only" : "refused: LINEAR did not pass the check",
             check_linear ? "LINEAR where the driver can" : "as asked (LINEAR did not pass the check)");
    return 1;
}

int vkb_dab_on(void)
{
    return __atomic_load_n(&dab_on, __ATOMIC_ACQUIRE);
}

int vkb_dab_hides(const char *name)
{
    return !strcmp(name, "VK_KHR_external_memory_fd") || !strcmp(name, "VK_EXT_external_memory_dma_buf") ||
           !strcmp(name, "VK_EXT_image_drm_format_modifier");
}

const char *const *vkb_dab_driver_exts(uint32_t *n)
{
#ifdef __ANDROID__
    static const char *const e[] = {"VK_ANDROID_external_memory_android_hardware_buffer", "VK_EXT_queue_family_foreign"};
    *n = 2;
#else
    static const char *const e[] = {"VK_KHR_external_memory_fd", "VK_EXT_external_memory_dma_buf"};
    *n = 2;
#endif
    return e;
}

/* ------------------------------------------------------------------ images */

static int pd_on(const vkb_dispatch *real, VkPhysicalDevice pd)
{
    if (!vkb_dab_on()) return 0;
    const vkb_pd_knowledge *k = vkb_pd_lookup(real, pd);
    return k && (k->info.emu & VKB_EMU_DMABUF_AHB);
}

/* The tiling a dma-buf image of this description is made with: LINEAR where the check passed it
 * and the driver makes such an image, else as asked. */
static VkImageTiling made_tiling(const vkb_dispatch *real, VkPhysicalDevice pd, VkFormat format, VkImageType type, VkImageTiling tiling,
                                 VkImageUsageFlags usage, VkImageCreateFlags flags, const VkExtent3D *extent, uint32_t mips, uint32_t layers,
                                 VkSampleCountFlagBits samples)
{
    if (tiling != VK_IMAGE_TILING_OPTIMAL || !check_linear || type != VK_IMAGE_TYPE_2D) return tiling;
    VkImageFormatProperties p;
    if (real->vkGetPhysicalDeviceImageFormatProperties(pd, format, type, VK_IMAGE_TILING_LINEAR, usage, flags, &p) != VK_SUCCESS)
        return tiling;
    if (extent && (extent->width > p.maxExtent.width || extent->height > p.maxExtent.height)) return tiling;
    if (mips > p.maxMipLevels || layers > p.maxArrayLayers || !(p.sampleCounts & samples)) return tiling;
    return VK_IMAGE_TILING_LINEAR;
}

typedef struct dab_img {
    VkImage image;
    vkb_dmabuf_ahb_info d;
    int asked_dmabuf;
    struct dab_img *next;
} dab_img;

#define IMG_BUCKETS 256
typedef struct vkb_dab_dev {
    pthread_mutex_t lock;
    dab_img *images[IMG_BUCKETS];
    dab_dev_fns f;
    int native;                 /* the driver imports dma-bufs itself (forced on a capable GPU) */
    unsigned exports, imports, refused;
} vkb_dab_dev;

static unsigned img_hash(VkImage i)
{
    uint64_t v = (uint64_t)(uintptr_t)i;
    return (unsigned)((v >> 4) ^ (v >> 16)) & (IMG_BUCKETS - 1);
}

static int img_find(vkb_dab_dev *d, VkImage image, dab_img *out)
{
    int found = 0;
    pthread_mutex_lock(&d->lock);
    for (dab_img *i = d->images[img_hash(image)]; i && !found; i = i->next)
        if (i->image == image) *out = *i, found = 1;
    pthread_mutex_unlock(&d->lock);
    return found;
}

void vkb_dab_image_create(vkb_emu_device *e, VkImageCreateInfo *ci, vkb_dab_note *n)
{
    memset(n, 0, sizeof(*n));
    if (!(e->flags & VKB_EMU_DMABUF_AHB) || !e->dev->dab) return;
    VkExternalMemoryImageCreateInfo *x = NULL;
    for (VkBaseOutStructure *b = (VkBaseOutStructure *)ci->pNext; b && !x; b = b->pNext)
        if (b->sType == VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO) x = (VkExternalMemoryImageCreateInfo *)b;
    if (!x || !(x->handleTypes & DMABUF)) return;
    n->dmabuf = 1;
    n->asked_tiling = ci->tiling;
    x->handleTypes &= ~(VkExternalMemoryHandleTypeFlags)DMABUF;
    if (!x->handleTypes) vkb_emu_chain_take(ci, VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO);
    vkb_srv_table *t = e->dev;
    ci->tiling = made_tiling(&t->real, t->physical, ci->format, ci->imageType, ci->tiling, ci->usage, ci->flags, &ci->extent, ci->mipLevels,
                             ci->arrayLayers, ci->samples);
}

void vkb_dab_image_created(vkb_emu_device *e, VkImage image, const VkImageCreateInfo *ci, const vkb_dab_note *n)
{
    vkb_srv_table *t = e->dev;
    vkb_dab_dev *d = t->dab;
    if (!d || !(e->flags & VKB_EMU_DMABUF_AHB) || ci->imageType != VK_IMAGE_TYPE_2D) return;
    dab_img *i = calloc(1, sizeof(*i));
    if (!i) return;
    i->image = image;
    i->asked_dmabuf = n->dmabuf;
    vkb_dmabuf_ahb_info *x = &i->d;
    x->has_image = 1;
    x->format = (uint32_t)ci->format;
    x->width = ci->extent.width;
    x->height = ci->extent.height;
    x->tiling = (uint32_t)ci->tiling;
    x->usage = ci->usage;
    x->flags = ci->flags;
    x->mip_levels = ci->mipLevels;
    x->array_layers = ci->arrayLayers;
    x->samples = (uint32_t)ci->samples;
    for (const VkBaseInStructure *b = ci->pNext; b; b = b->pNext)
        if (b->sType == VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO) {
            const VkImageFormatListCreateInfo *fl = (const void *)b;
            for (uint32_t k = 0; k < fl->viewFormatCount && x->view_format_count < 8; k++) x->view_formats[x->view_format_count++] = fl->pViewFormats[k];
        }
    pthread_mutex_lock(&d->lock);
    unsigned h = img_hash(image);
    i->next = d->images[h];
    d->images[h] = i;
    pthread_mutex_unlock(&d->lock);
    if (n->dmabuf) {
        static int told;
        if (__atomic_fetch_add(&told, 1, __ATOMIC_RELAXED) < 6)
            VKB_INFO("dma-buf over AHardwareBuffer: dma-buf image %llx (pid %d): format %d %ux%u, tiling %d made %d, usage 0x%x flags 0x%x",
                     (unsigned long long)(uintptr_t)image, t->proc ? t->proc->pid : 0, ci->format, ci->extent.width, ci->extent.height,
                     n->asked_tiling, ci->tiling, ci->usage, ci->flags);
    }
}

void vkb_dab_image_destroyed(vkb_emu_device *e, VkImage image)
{
    vkb_dab_dev *d = e ? e->dev->dab : NULL;
    if (!d || !image) return;
    pthread_mutex_lock(&d->lock);
    for (dab_img **pp = &d->images[img_hash(image)]; *pp; pp = &(*pp)->next)
        if ((*pp)->image == image) {
            dab_img *i = *pp;
            *pp = i->next;
            free(i);
            break;
        }
    pthread_mutex_unlock(&d->lock);
}

/* ------------------------------------------------------------------ physical-device queries */

/* VK_EXT_image_drm_format_modifier is the server's on this path, with the LINEAR modifier only: a
 * LINEAR-modifier image is a plain LINEAR image (server_emu_layer.c dab_linear_mod), offered only where
 * LINEAR passed the start's check. Without the extension Zink cannot export an image it did not make
 * exportable (zink_resource_get_handle), which is every pixmap Xwayland hands to gamescope: Xwayland
 * then had no buffer for any window ("Error getting buffer") and Steam never showed (S11 Ultra, forced). */
static int lin_mod_ok(VkFormat f)
{
    /* single-plane, uncompressed colour formats */
    if (f == VK_FORMAT_UNDEFINED || (f >= VK_FORMAT_D16_UNORM && f <= VK_FORMAT_D32_SFLOAT_S8_UINT)) return 0;
    if (f >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && f <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK) return 0;
    return f < 1000000000; /* the core 1.0 formats */
}

int vkb_dab_linear_modifier(void)
{
    return check_linear;
}

static VKAPI_ATTR void VKAPI_CALL dab_GetPhysicalDeviceFormatProperties2(VkPhysicalDevice pd, VkFormat format, VkFormatProperties2 *props)
{
    const vkb_dispatch *real = &vkb_srv_current_table()->real;
    if (!pd_on(real, pd)) {
        real->vkGetPhysicalDeviceFormatProperties2(pd, format, props);
        return;
    }
    vkb_emu_taken tl = vkb_emu_chain_take_at(props, VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT);
    vkb_emu_taken tl2 = vkb_emu_chain_take_at(props, VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_2_EXT);
    VkDrmFormatModifierPropertiesListEXT *l = (void *)tl.s;
    VkDrmFormatModifierPropertiesList2EXT *l2 = (void *)tl2.s;
    real->vkGetPhysicalDeviceFormatProperties2(pd, format, props);
    VkFormatFeatureFlags lin = props->formatProperties.linearTilingFeatures;
    VkFormatFeatureFlags2 lin2 = lin;
    for (const VkBaseInStructure *b = props->pNext; b; b = b->pNext)
        if (b->sType == VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3) lin2 = ((const VkFormatProperties3 *)b)->linearTilingFeatures;
    uint32_t n = check_linear && lin && lin_mod_ok(format) ? 1 : 0;
    if (l) {
        if (l->pDrmFormatModifierProperties && l->drmFormatModifierCount && n)
            l->pDrmFormatModifierProperties[0] = (VkDrmFormatModifierPropertiesEXT){0 /* DRM_FORMAT_MOD_LINEAR */, 1, lin};
        l->drmFormatModifierCount = l->pDrmFormatModifierProperties && l->drmFormatModifierCount < n ? l->drmFormatModifierCount : n;
    }
    if (l2) {
        if (l2->pDrmFormatModifierProperties && l2->drmFormatModifierCount && n)
            l2->pDrmFormatModifierProperties[0] = (VkDrmFormatModifierProperties2EXT){0 /* DRM_FORMAT_MOD_LINEAR */, 1, lin2};
        l2->drmFormatModifierCount = l2->pDrmFormatModifierProperties && l2->drmFormatModifierCount < n ? l2->drmFormatModifierCount : n;
    }
    /* back where they were: Zink chains VkFormatProperties3 first, and the client reads the answer in order */
    vkb_emu_chain_put_back(tl2);
    vkb_emu_chain_put_back(tl);
}

static VKAPI_ATTR VkResult VKAPI_CALL dab_GetPhysicalDeviceImageFormatProperties2(VkPhysicalDevice pd, const VkPhysicalDeviceImageFormatInfo2 *info,
                                                                                 VkImageFormatProperties2 *props)
{
    const vkb_dispatch *real = &vkb_srv_current_table()->real;
    const VkPhysicalDeviceExternalImageFormatInfo *ei = NULL;
    const VkPhysicalDeviceImageDrmFormatModifierInfoEXT *mi = NULL;
    for (const VkBaseInStructure *b = info->pNext; b; b = b->pNext) {
        if (b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO) ei = (const void *)b;
        if (b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT) mi = (const void *)b;
    }
    int dmabuf = ei && ei->handleType == DMABUF, mod = info->tiling == VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
    if ((!dmabuf && !mod) || !pd_on(real, pd)) return real->vkGetPhysicalDeviceImageFormatProperties2(pd, info, props);
    VkPhysicalDeviceImageFormatInfo2 *ii = (VkPhysicalDeviceImageFormatInfo2 *)info;
    VkBaseOutStructure *took_ei = dmabuf ? vkb_emu_chain_take(ii, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO) : NULL;
    VkBaseOutStructure *took_mi = mi ? vkb_emu_chain_take(ii, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT) : NULL;
    vkb_emu_taken tep = dmabuf ? vkb_emu_chain_take_at(props, VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES) : (vkb_emu_taken){0};
    VkExternalImageFormatProperties *ep = (void *)tep.s;
    VkResult r = VK_ERROR_FORMAT_NOT_SUPPORTED;
    VkImageTiling asked = ii->tiling;
    if (!mod) {
        ii->tiling = made_tiling(real, pd, ii->format, ii->type, ii->tiling, ii->usage, ii->flags, NULL, 1, 1, VK_SAMPLE_COUNT_1_BIT);
        r = real->vkGetPhysicalDeviceImageFormatProperties2(pd, ii, props);
    } else if (mi && mi->drmFormatModifier == 0 /* DRM_FORMAT_MOD_LINEAR */ && check_linear && lin_mod_ok(ii->format) &&
               ii->type == VK_IMAGE_TYPE_2D) {
        ii->tiling = VK_IMAGE_TILING_LINEAR; /* the LINEAR modifier: a plain LINEAR image */
        r = real->vkGetPhysicalDeviceImageFormatProperties2(pd, ii, props);
    }
    ii->tiling = asked;
    if (took_mi) {
        took_mi->pNext = (VkBaseOutStructure *)ii->pNext;
        ii->pNext = took_mi;
    }
    if (took_ei) {
        took_ei->pNext = (VkBaseOutStructure *)ii->pNext;
        ii->pNext = took_ei;
    }
    if (ep) {
        vkb_emu_chain_put_back(tep); /* in its place: the client reads the answer in order */
        memset(&ep->externalMemoryProperties, 0, sizeof(ep->externalMemoryProperties));
        if (r == VK_SUCCESS) {
            ep->externalMemoryProperties.externalMemoryFeatures = BOTH_WAYS;
            ep->externalMemoryProperties.exportFromImportedHandleTypes = DMABUF;
            ep->externalMemoryProperties.compatibleHandleTypes = DMABUF;
        }
    }
    if (r != VK_SUCCESS) memset(&props->imageFormatProperties, 0, sizeof(props->imageFormatProperties));
    return r;
}

static VKAPI_ATTR void VKAPI_CALL dab_GetPhysicalDeviceExternalBufferProperties(VkPhysicalDevice pd, const VkPhysicalDeviceExternalBufferInfo *info,
                                                                              VkExternalBufferProperties *props)
{
    const vkb_dispatch *real = &vkb_srv_current_table()->real;
    if (info->handleType != DMABUF || !pd_on(real, pd)) {
        real->vkGetPhysicalDeviceExternalBufferProperties(pd, info, props);
        return;
    }
    VkExternalMemoryProperties *p = &props->externalMemoryProperties;
    memset(p, 0, sizeof(*p));
    p->externalMemoryFeatures = BOTH_WAYS;
    p->exportFromImportedHandleTypes = DMABUF;
    p->compatibleHandleTypes = DMABUF;
}

void vkb_dab_instance_install(vkb_srv_table *t)
{
    if (!vkb_dab_on()) return;
    if (t->real.vkGetPhysicalDeviceImageFormatProperties2)
        t->dt.vkGetPhysicalDeviceImageFormatProperties2 = dab_GetPhysicalDeviceImageFormatProperties2;
    if (t->real.vkGetPhysicalDeviceFormatProperties2) t->dt.vkGetPhysicalDeviceFormatProperties2 = dab_GetPhysicalDeviceFormatProperties2;
    if (t->real.vkGetPhysicalDeviceExternalBufferProperties)
        t->dt.vkGetPhysicalDeviceExternalBufferProperties = dab_GetPhysicalDeviceExternalBufferProperties;
}

/* ------------------------------------------------------------------ memory */

static VKAPI_ATTR VkResult VKAPI_CALL dab_GetMemoryFdKHR(VkDevice device, const VkMemoryGetFdInfoKHR *gi, int *pFd)
{
    vkb_srv_table *t = vkb_srv_current_table();
    vkb_srv_mem *m = t && gi ? vkb_mem_find(t, gi->memory, 0) : NULL;
    if (m && m->dab && gi->handleType == DMABUF) {
        *pFd = fcntl(dab_reg_fd(m->dab), F_DUPFD_CLOEXEC, 0);
        return *pFd >= 0 ? VK_SUCCESS : VK_ERROR_TOO_MANY_OBJECTS;
    }
    if (t && t->real.vkGetMemoryFdKHR) return t->real.vkGetMemoryFdKHR(device, gi, pFd);
    static int told;
    if (__atomic_fetch_add(&told, 1, __ATOMIC_RELAXED) < 4)
        VKB_WARN("dma-buf over AHardwareBuffer: vkGetMemoryFdKHR (handle type 0x%x) of memory not made for export", gi ? gi->handleType : 0);
    return VK_ERROR_TOO_MANY_OBJECTS;
}

static VKAPI_ATTR VkResult VKAPI_CALL dab_GetMemoryFdPropertiesKHR(VkDevice device, VkExternalMemoryHandleTypeFlagBits type, int fd,
                                                                  VkMemoryFdPropertiesKHR *props)
{
    vkb_srv_table *t = vkb_srv_current_table();
    dab_entry *e = type == DMABUF ? dab_reg_find(fd) : NULL;
    if (e && t && t->dab) {
        uint64_t size;
        VkResult r = pages_props(&t->dab->f, dab_reg_ahb(e), dab_reg_fd(e), dab_reg_bytes(e), &props->memoryTypeBits, &size);
        props->memoryTypeBits &= ordinary_types(&t->dab->f);
        dab_reg_put(e);
        return r == VK_SUCCESS ? VK_SUCCESS : VK_ERROR_INVALID_EXTERNAL_HANDLE;
    }
    if (t && t->real.vkGetMemoryFdPropertiesKHR) return t->real.vkGetMemoryFdPropertiesKHR(device, type, fd, props);
    return VK_ERROR_INVALID_EXTERNAL_HANDLE;
}

void vkb_dab_device_init(vkb_srv_table *t, uint32_t emu)
{
    if (!(emu & VKB_EMU_DMABUF_AHB) || !vkb_dab_on()) return;
    vkb_dab_dev *d = calloc(1, sizeof(*d));
    if (!d) return;
    pthread_mutex_init(&d->lock, NULL);
    d->f.dev = t->device;
    d->f.dt = &t->real;
    d->f.pd = t->physical;
    d->f.idt = t->parent ? &t->parent->real : &t->real;
#ifdef __ANDROID__
    d->f.ahb_props = vkb_mem_gdpa ? (PFN_vkGetAndroidHardwareBufferPropertiesANDROID)vkb_mem_gdpa(t->device, "vkGetAndroidHardwareBufferPropertiesANDROID")
                                  : NULL;
    if (!d->f.ahb_props) VKB_WARN("dma-buf over AHardwareBuffer: vkGetAndroidHardwareBufferPropertiesANDROID not found for the device");
#endif
    d->native = __atomic_load_n(&dab_forced, __ATOMIC_RELAXED) && t->real.vkGetMemoryFdPropertiesKHR != NULL;
    t->dab = d;
    t->dt.vkGetMemoryFdKHR = dab_GetMemoryFdKHR;
    t->dt.vkGetMemoryFdPropertiesKHR = dab_GetMemoryFdPropertiesKHR;
}

void vkb_dab_device_destroy(vkb_srv_table *t)
{
    vkb_dab_dev *d = t->dab;
    if (!d) return;
    if (d->exports || d->imports || d->refused)
        VKB_INFO("dma-buf over AHardwareBuffer: device %u made %u export(s), %u import(s), refused %u", t->id, d->exports, d->imports,
                 d->refused);
    for (int i = 0; i < IMG_BUCKETS; i++)
        while (d->images[i]) {
            dab_img *n = d->images[i]->next;
            free(d->images[i]);
            d->images[i] = n;
        }
    pthread_mutex_destroy(&d->lock);
    free(d);
    t->dab = NULL;
}

static void *chain_find(const void *head, VkStructureType type)
{
    for (const VkBaseInStructure *b = ((const VkBaseInStructure *)head)->pNext; b; b = b->pNext)
        if (b->sType == type) return (void *)b;
    return NULL;
}

/* The exporter's and the importer's images laid out alike, or why not. */
static int same_layout(vkb_srv_table *t, VkImage image, const dab_img *mine, const vkb_dmabuf_ahb_info *theirs, uint64_t need, char *why, size_t cap)
{
    const vkb_dmabuf_ahb_info *m = &mine->d;
    if (m->width != theirs->width || m->height != theirs->height) {
        snprintf(why, cap, "%ux%u, the exporter's %ux%u", m->width, m->height, theirs->width, theirs->height);
        return 0;
    }
    if (m->tiling != theirs->tiling) {
        snprintf(why, cap, "tiling %u, the exporter's %u", m->tiling, theirs->tiling);
        return 0;
    }
    if (m->tiling == VK_IMAGE_TILING_LINEAR) {
        VkImageSubresource sub = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
        VkSubresourceLayout l;
        t->real.vkGetImageSubresourceLayout(t->device, image, &sub, &l);
        if (l.rowPitch != theirs->row_pitch || l.offset != 0) {
            snprintf(why, cap, "row pitch %llu, the exporter's %llu", (unsigned long long)l.rowPitch, (unsigned long long)theirs->row_pitch);
            return 0;
        }
        return 1;
    }
    if (need != theirs->image_bytes || m->format != theirs->format) {
        snprintf(why, cap, "format %u and %llu bytes, the exporter's %u and %llu", m->format, (unsigned long long)need, theirs->format,
                 (unsigned long long)theirs->image_bytes);
        return 0;
    }
    return 1;
}

static void told(unsigned *count, int warn, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void told(unsigned *count, int warn, const char *fmt, ...)
{
    if (__atomic_fetch_add(count, 1, __ATOMIC_RELAXED) >= 8) return;
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (warn) VKB_WARN("dma-buf over AHardwareBuffer: %s", line);
    else VKB_INFO("dma-buf over AHardwareBuffer: %s", line);
}

int vkb_dab_allocate(vkb_srv_table *t, VkMemoryAllocateInfo *ai, vkb_srv_mem *m, VkResult *res)
{
    vkb_dab_dev *d = t->dab;
    if (!d) return 0;
    VkExportMemoryAllocateInfo *ex = chain_find(ai, VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO);
    if (ex && !(ex->handleTypes & DMABUF)) ex = NULL;
    VkImportMemoryFdInfoKHR *im = chain_find(ai, VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR);
    if (im && im->handleType != DMABUF) im = NULL;
    if (!ex && !im) return 0;
    dab_entry *e = im ? dab_reg_find(im->fd) : NULL;
    if (im && !e && d->native) return 0; /* not one of ours: the driver's own dma-buf import (forced on a capable GPU) */
    const int pid = t->proc ? t->proc->pid : 0;
    const VkMemoryDedicatedAllocateInfo *ded = chain_find(ai, VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO);
    VkImage image = ded ? ded->image : VK_NULL_HANDLE;
    VkBuffer buffer = ded ? ded->buffer : VK_NULL_HANDLE;
    VkMemoryRequirements req = {ai->allocationSize, 1, UINT32_MAX};
    if (image) t->real.vkGetImageMemoryRequirements(t->device, image, &req);
    else if (buffer) t->real.vkGetBufferMemoryRequirements(t->device, buffer, &req);
    dab_img mine;
    int have_img = image && img_find(d, image, &mine);
    char why[256] = "";
    if (im && !e) {
        told(&d->refused, 1, "pid %d imported a dma-buf the bridge did not hand out (%llu bytes); refused", pid,
             (unsigned long long)ai->allocationSize);
        *res = VK_ERROR_INVALID_EXTERNAL_HANDLE;
        return 2;
    }
    if (im) {
        const vkb_dmabuf_ahb_info *theirs = dab_reg_image(e);
        if (have_img && theirs->has_image && !same_layout(t, image, &mine, theirs, req.size, why, sizeof(why))) {
            told(&d->refused, 1, "pid %d's image %llx is laid out unlike the exporter's (%s); import refused", pid,
                 (unsigned long long)(uintptr_t)image, why);
            dab_reg_put(e);
            *res = VK_ERROR_INVALID_EXTERNAL_HANDLE;
            return 2;
        }
        int fd = im->fd;
        vkb_emu_chain_take(ai, VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR);
        vkb_emu_chain_take(ai, VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO);
        vkb_emu_chain_take(ai, VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO);
        uint32_t type = ai->memoryTypeIndex;
        VkResult r = pages_import(&d->f, dab_reg_ahb(e), dab_reg_fd(e), dab_reg_bytes(e), req.memoryTypeBits, req.size, ai->pNext, &type,
                                  &m->mem, why, sizeof(why));
        if (fd >= 0) close(fd); /* never the driver's: the bridge's copy of the app's fd */
        ai->memoryTypeIndex = type;
        if (r != VK_SUCCESS) {
            told(&d->refused, 1, "pid %d's import of %llu bytes failed: %s", pid, (unsigned long long)dab_reg_bytes(e), why);
            dab_reg_put(e);
            *res = r;
            return 2;
        }
        m->dab = e;
        m->type = type;
        told(&d->imports, 0, "pid %d imported %llu bytes (%s %ux%u) for image %llx, type %u", pid, (unsigned long long)dab_reg_bytes(e),
             theirs->has_image ? (theirs->tiling == VK_IMAGE_TILING_LINEAR ? "LINEAR" : "OPTIMAL") : "no image", theirs->width,
             theirs->height, (unsigned long long)(uintptr_t)image, type);
        *res = VK_SUCCESS;
        return 2;
    }
    /* export: fresh pages */
    uint64_t bytes = ((ai->allocationSize > req.size ? ai->allocationSize : req.size) + 4095) & ~4095ull;
    dab_pages p;
    if (!pages_new(&d->f, bytes, ai->memoryTypeIndex, &p)) {
        told(&d->refused, 1, "pid %d: no buffer of %llu bytes for an export", pid, (unsigned long long)bytes);
        *res = VK_ERROR_OUT_OF_DEVICE_MEMORY;
        return 1;
    }
    vkb_dmabuf_ahb_info desc;
    memset(&desc, 0, sizeof(desc));
    if (have_img) {
        desc = mine.d;
        desc.image_bytes = req.size;
        if (desc.tiling == VK_IMAGE_TILING_LINEAR) {
            VkImageSubresource sub = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
            VkSubresourceLayout l;
            t->real.vkGetImageSubresourceLayout(t->device, image, &sub, &l);
            desc.row_pitch = l.rowPitch;
        }
    }
    dab_entry *ne = dab_reg_add(p.ahb, p.fd, bytes, &desc);
    if (!ne) {
        pages_free(&p);
        *res = VK_ERROR_OUT_OF_HOST_MEMORY;
        return 1;
    }
    vkb_emu_chain_take(ai, VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO);
    vkb_emu_chain_take(ai, VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO);
    uint32_t type = ai->memoryTypeIndex;
    VkResult r = pages_import(&d->f, p.ahb, p.fd, bytes, req.memoryTypeBits, req.size, ai->pNext, &type, &m->mem, why, sizeof(why));
    ai->memoryTypeIndex = type;
    if (r != VK_SUCCESS) {
        told(&d->refused, 1, "pid %d: an export of %llu bytes failed: %s", pid, (unsigned long long)bytes, why);
        dab_reg_put(ne);
        *res = r;
        return 1;
    }
    m->dab = ne;
    m->type = type;
    struct stat st;
    uint64_t handle = image ? (uint64_t)(uintptr_t)image : (uint64_t)(uintptr_t)buffer;
    told(&d->exports, 0, "pid %d exports %llu bytes (dma-buf ino %lu%s) for %s %llx%s%s, type %u", pid, (unsigned long long)bytes,
         fstat(p.fd, &st) == 0 ? (unsigned long)st.st_ino : 0UL, dab_reg_shared_inode() ? ", one inode for all" : "",
         image ? "image" : buffer ? "buffer" : "memory", (unsigned long long)handle,
         have_img ? (desc.tiling == VK_IMAGE_TILING_LINEAR ? " (LINEAR)" : " (OPTIMAL)") : "",
         have_img && !mine.asked_dmabuf ? ", made without the dma-buf handle type" : "", type);
    *res = VK_SUCCESS;
    return 1;
}

void vkb_dab_mem_release(vkb_srv_mem *m)
{
    if (!m || !m->dab) return;
    dab_reg_put(m->dab);
    m->dab = NULL;
}
