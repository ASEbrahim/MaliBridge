/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * OPAQUE_FD external memory on dma-bufs (VKB_EMU_OPAQUE_FD).
 *
 * Mali r44 has no opaque fds ("external buffers, opaque fd: features 0x0"), and DXVK implements
 * D3D11 shared resources with them (Wine turns the NT/KMT handles into opaque fds): OPAQUE_FD
 * missing makes DXVK report "D3D11DeviceFeatures: Some formats not supported for resource sharing",
 * and Chromium-based games lose their GPU process (OMORI, 2026-10-11: white window). Every exporter
 * and importer of an opaque fd is a client of this one server, on the same driver, so the fd only
 * has to come back as the same memory: an opaque fd handed out here is a dma-buf of the memory,
 * imported back as a dma-buf. Opaque-fd rules make this sound: the importer must use the same
 * device, allocation size and an identically created resource, so the driver lays it out the same.
 *   - queries: OPAQUE_FD answers are the driver's DMA_BUF answers (exportable when the driver or
 *     the dma-heap export emulation exports), compatible with OPAQUE_FD only;
 *   - resources: OPAQUE_FD in VkExternalMemory{Buffer,Image}CreateInfo becomes DMA_BUF; an image
 *     the driver will not make dma-buf importable at its tiling is made without external info
 *     (same layout in both processes all the same; its memory is still a dma-buf);
 *   - allocation: export OPAQUE_FD becomes export DMA_BUF (the driver's, or the dma-heap emulation
 *     in server_memory.c); an imported opaque fd is imported as a dma-buf, into a memory type the
 *     dma-buf takes, with the dedicated struct dropped if the driver refuses it;
 *   - vkGetMemoryFdKHR(OPAQUE_FD) gives the dma-buf (server_memory.c's export hook).
 * VKBRIDGE_FAKE_MISSING=opaquefd turns it on for a driver with opaque fds (host tests; RADV takes
 * the plain-image path, as Mali presumably will). Plain images bound to imported dma-buf pages are
 * outside the letter of the spec (the image does not name the handle type); in practice the driver
 * sees an ordinary image on ordinary pages, laid out the same in both processes.
 * VKBRIDGE_EMU_OPAQUEFD=0 turns it off.
 */
#define _GNU_SOURCE
#include "vkb_server.h"
#include "server_emu.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define OPAQUE VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT
#define DMABUF VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT
#define BOTH_WAYS (VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT | VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT)

/* VkExternalMemoryBufferCreateInfo, VkExternalMemoryImageCreateInfo and VkExportMemoryAllocateInfo
 * all hold handleTypes right after the header. */
typedef struct {
    VkStructureType sType;
    const void *pNext;
    VkExternalMemoryHandleTypeFlags handleTypes;
} ext_handles;

static void *chain_find(const void *head, VkStructureType t)
{
    for (const VkBaseInStructure *b = ((const VkBaseInStructure *)head)->pNext; b; b = b->pNext)
        if (b->sType == t) return (void *)b;
    return NULL;
}

static void tell_once(int *flag, const char *what)
{
    if (!__atomic_exchange_n(flag, 1, __ATOMIC_RELAXED)) VKB_INFO("opaque fd emulation: %s", what);
}

static int pd_on(const vkb_dispatch *real, VkPhysicalDevice pd)
{
    const vkb_pd_knowledge *k = vkb_pd_lookup(real, pd);
    return k && (k->info.emu & VKB_EMU_OPAQUE_FD);
}

static int dev_on(const vkb_srv_table *t) { return t && t->pd && (t->pd->info.emu & VKB_EMU_OPAQUE_FD); }

/* ------------------------------------------------------------------ detection */

static VkExternalMemoryFeatureFlags buffer_features(const vkb_dispatch *idt, VkPhysicalDevice pd, uint32_t type)
{
    VkPhysicalDeviceExternalBufferInfo bi = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO, NULL, 0,
                                             VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                             (VkExternalMemoryHandleTypeFlagBits)type};
    VkExternalBufferProperties ep = {VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES};
    idt->vkGetPhysicalDeviceExternalBufferProperties(pd, &bi, &ep);
    return ep.externalMemoryProperties.externalMemoryFeatures;
}

int vkb_opaque_wanted(const vkb_dispatch *idt, VkPhysicalDevice pd, uint32_t emu)
{
    if (!idt->vkGetPhysicalDeviceExternalBufferProperties) return 0;
    VkExternalMemoryFeatureFlags opaque = buffer_features(idt, pd, OPAQUE);
    if (vkb_emu_faked("opaquefd")) opaque = 0;
    if ((opaque & BOTH_WAYS) == BOTH_WAYS) return 0;
    VkExternalMemoryFeatureFlags dmabuf = buffer_features(idt, pd, DMABUF);
    /* Imports must work; exports come from the driver or from the dma-heap emulation. */
    int import = (dmabuf & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) || (emu & VKB_EMU_DMABUF_EXPORT);
    int export = (dmabuf & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT) || (emu & VKB_EMU_DMABUF_EXPORT);
    VKB_INFO("opaque fd emulation: driver opaque fd features 0x%x, dma-buf 0x%x%s -> %s", opaque, dmabuf,
             emu & VKB_EMU_DMABUF_EXPORT ? " (+ emulated export)" : "", import && export ? "on" : "not possible");
    return import && export;
}

/* ------------------------------------------------------------------ images: which way */

enum { IMG_NO, IMG_DMABUF, IMG_PLAIN };

/* How an OPAQUE_FD image of this description is made: as a dma-buf image (the driver imports
 * dma-bufs at this tiling), plain (it does not: RADV refuses dma-buf optimal images with
 * VK_ERROR_FORMAT_NOT_SUPPORTED, and Mali answers dma-buf questions only for DRM modifiers -
 * server_emu.c), or not at all. *ded: the driver wants dedicated memory for it. */
static int image_way(const vkb_dispatch *real, VkPhysicalDevice pd, VkFormat format, VkImageType type, VkImageTiling tiling,
                     VkImageUsageFlags usage, VkImageCreateFlags flags, const void *chain, VkImageFormatProperties2 *out, int *ded)
{
    *ded = 1;
    if (!real->vkGetPhysicalDeviceImageFormatProperties2) return IMG_NO;
    VkPhysicalDeviceExternalImageFormatInfo ei = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO, chain, DMABUF};
    VkPhysicalDeviceImageFormatInfo2 ii = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2, &ei, format, type, tiling, usage, flags};
    VkExternalImageFormatProperties ep = {VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES, out ? out->pNext : NULL};
    VkImageFormatProperties2 ip = {VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, &ep};
    if (real->vkGetPhysicalDeviceImageFormatProperties2(pd, &ii, &ip) == VK_SUCCESS &&
        (ep.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT)) {
        *ded = (ep.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT) != 0;
        if (out) out->imageFormatProperties = ip.imageFormatProperties;
        return IMG_DMABUF;
    }
    if (tiling == VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT) return IMG_NO;
    ii.pNext = chain;
    ip.pNext = out ? out->pNext : NULL;
    if (real->vkGetPhysicalDeviceImageFormatProperties2(pd, &ii, &ip) != VK_SUCCESS) return IMG_NO;
    if (out) out->imageFormatProperties = ip.imageFormatProperties;
    return IMG_PLAIN;
}

/* ------------------------------------------------------------------ physical-device queries */

static void answer(VkExternalMemoryProperties *p, VkExternalMemoryFeatureFlags dedicated)
{
    p->externalMemoryFeatures = BOTH_WAYS | dedicated;
    p->exportFromImportedHandleTypes = OPAQUE;
    p->compatibleHandleTypes = OPAQUE;
}

/*
 * Mali r32p1 (Mali-G77 MC9, Retroid Pocket 4 Pro, Android 13) answers VK_ERROR_FORMAT_NOT_SUPPORTED
 * to vkGetPhysicalDeviceImageFormatProperties2 for an external (dma-buf) image at DRM-modifier
 * tiling as soon as VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT is set - with or without the
 * VkImageFormatListCreateInfo - while the same image without the flag is supported (importable),
 * and vkCreateImage itself takes the flag, the format list, the modifier list and the external
 * handle type, exports the memory and makes the sRGB view (tools/vkcreate.py, 2026-10-26).
 * gamescope sets the flag on every flippable image whose sRGB twin differs (XR24/AR24:
 * CVulkanTexture::BInit), found no modifier it could use and ended the session at its
 * "modifiers.size() > 0" assertion: Steam mode stopped at "Starting display and audio"
 * (MaliBridge#10). The refused question is asked again without the flag and the list; a
 * supported answer is the answer.
 */
static VkResult mutable_query_retry(const vkb_dispatch *real, VkPhysicalDevice pd, const VkPhysicalDeviceImageFormatInfo2 *info,
                                    VkImageFormatProperties2 *props)
{
    VkPhysicalDeviceImageFormatInfo2 *ii = (VkPhysicalDeviceImageFormatInfo2 *)info;
    VkImageCreateFlags flags = ii->flags;
    ii->flags &= ~(VkImageCreateFlags)VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    VkImageFormatListCreateInfo *list = vkb_emu_chain_take(ii, VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO);
    VkResult r = real->vkGetPhysicalDeviceImageFormatProperties2(pd, ii, props);
    if (list) {
        list->pNext = ii->pNext;
        ii->pNext = list;
    }
    ii->flags = flags;
    static int told;
    if (__atomic_fetch_add(&told, 1, __ATOMIC_RELAXED) < 4) {
        const VkExternalImageFormatProperties *ep = props ? chain_find(props, VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES) : NULL;
        VKB_INFO("image format query: the driver refuses MUTABLE_FORMAT on an external image (format %u tiling %d usage 0x%x, %s); "
                 "asked without it -> %d (external features 0x%x) - the image itself is made with the flag (Mali r32p1)",
                 info->format, (int)info->tiling, info->usage, list ? "with a format list" : "no format list", r,
                 ep ? ep->externalMemoryProperties.externalMemoryFeatures : 0);
    }
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL op_GetPhysicalDeviceImageFormatProperties2(VkPhysicalDevice pd,
                                                                                const VkPhysicalDeviceImageFormatInfo2 *info,
                                                                                VkImageFormatProperties2 *props)
{
    const vkb_dispatch *real = &vkb_srv_current_table()->real;
    VkPhysicalDeviceExternalImageFormatInfo *ei = chain_find(info, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO);
    if (!ei || ei->handleType != OPAQUE || !pd_on(real, pd)) {
        VkResult r = real->vkGetPhysicalDeviceImageFormatProperties2(pd, info, props);
        if (r == VK_ERROR_FORMAT_NOT_SUPPORTED && ei && (info->flags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT))
            r = mutable_query_retry(real, pd, info, props);
        if (vkb_verbose && ei) {
            /* RP4 Pro investigation: what the driver was asked and what it said (vkb_verbose only) */
            char chain[200] = "";
            int n = 0;
            const VkPhysicalDeviceImageDrmFormatModifierInfoEXT *mi = NULL;
            for (const VkBaseInStructure *b = info->pNext; b; b = b->pNext) {
                n += snprintf(chain + n, n < (int)sizeof(chain) ? sizeof(chain) - (size_t)n : 0, " %d", (int)b->sType);
                if (b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT) mi = (const void *)b;
            }
            VkExternalImageFormatProperties *ep = props ? chain_find(props, VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES) : NULL;
            VKB_INFO("image format query: format %u type %u tiling %u usage 0x%x flags 0x%x handle 0x%x chain [%s]%s mod 0x%llx sharing %u qfc %u -> %d, ext features 0x%x",
                     info->format, info->type, info->tiling, info->usage, info->flags, ei->handleType, chain, mi ? "" : " (no modifier info)",
                     mi ? (unsigned long long)mi->drmFormatModifier : 0ull, mi ? mi->sharingMode : 0, mi ? mi->queueFamilyIndexCount : 0, r,
                     ep ? ep->externalMemoryProperties.externalMemoryFeatures : 0);
            if (mi && r != VK_SUCCESS) {
                /* the same question built here, to tell a wire problem from a driver answer */
                VkPhysicalDeviceImageDrmFormatModifierInfoEXT m2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT, NULL,
                                                                    mi->drmFormatModifier, VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
                VkPhysicalDeviceExternalImageFormatInfo e2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO, &m2, ei->handleType};
                VkPhysicalDeviceImageFormatInfo2 i2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2, &e2, info->format, info->type,
                                                       info->tiling, info->usage, info->flags};
                VkExternalImageFormatProperties p2e = {VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
                VkImageFormatProperties2 p2 = {VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, &p2e};
                VkResult r2 = real->vkGetPhysicalDeviceImageFormatProperties2(pd, &i2, &p2);
                VKB_INFO("image format query: rebuilt on the server -> %d, ext features 0x%x", r2, p2e.externalMemoryProperties.externalMemoryFeatures);
            }
        }
        return r;
    }
    /* The rest of the request's chain (format lists, usage) still applies; the external struct
     * and the external answer are ours. */
    VkPhysicalDeviceImageFormatInfo2 *ii = (VkPhysicalDeviceImageFormatInfo2 *)info;
    vkb_emu_chain_take(ii, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO);
    VkExternalImageFormatProperties *ep = vkb_emu_chain_take(props, VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES);
    int ded;
    int way = image_way(real, pd, ii->format, ii->type, ii->tiling, ii->usage, ii->flags, ii->pNext, props, &ded);
    if (ep) {
        ep->pNext = props->pNext;
        props->pNext = ep;
        memset(&ep->externalMemoryProperties, 0, sizeof(ep->externalMemoryProperties));
        if (way != IMG_NO) answer(&ep->externalMemoryProperties, ded ? VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT : 0);
    }
    if (way == IMG_NO) {
        memset(&props->imageFormatProperties, 0, sizeof(props->imageFormatProperties));
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    }
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL op_GetPhysicalDeviceExternalBufferProperties(VkPhysicalDevice pd,
                                                                             const VkPhysicalDeviceExternalBufferInfo *info,
                                                                             VkExternalBufferProperties *props)
{
    const vkb_dispatch *real = &vkb_srv_current_table()->real;
    if (info->handleType != OPAQUE || !pd_on(real, pd)) {
        real->vkGetPhysicalDeviceExternalBufferProperties(pd, info, props);
        return;
    }
    VkPhysicalDeviceExternalBufferInfo bi = *info;
    bi.handleType = DMABUF;
    real->vkGetPhysicalDeviceExternalBufferProperties(pd, &bi, props);
    VkExternalMemoryProperties *p = &props->externalMemoryProperties;
    VkExternalMemoryFeatureFlags f = p->externalMemoryFeatures;
    memset(p, 0, sizeof(*p));
    /* Not the driver's dma-buf features as they are: vkb_opaque_wanted decided the path works
     * (exports may be the dma-heap emulation's, which the driver does not know of). */
    answer(p, f & VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT);
}

void vkb_opaque_instance_install(vkb_srv_table *t)
{
    if (t->real.vkGetPhysicalDeviceImageFormatProperties2) t->dt.vkGetPhysicalDeviceImageFormatProperties2 = op_GetPhysicalDeviceImageFormatProperties2;
    if (t->real.vkGetPhysicalDeviceExternalBufferProperties)
        t->dt.vkGetPhysicalDeviceExternalBufferProperties = op_GetPhysicalDeviceExternalBufferProperties;
}

/* ------------------------------------------------------------------ resources */

int vkb_opaque_buffer(vkb_emu_device *e, VkBufferCreateInfo *ci)
{
    if (!(e->flags & VKB_EMU_OPAQUE_FD)) return 0;
    ext_handles *h = chain_find(ci, VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO);
    if (!h || !(h->handleTypes & OPAQUE)) return 0;
    h->handleTypes = (h->handleTypes & ~(VkExternalMemoryHandleTypeFlags)OPAQUE) | DMABUF;
    return 1;
}

int vkb_opaque_image(vkb_emu_device *e, VkImageCreateInfo *ci)
{
    static int told_dmabuf, told_plain;
    if (!(e->flags & VKB_EMU_OPAQUE_FD)) return 0;
    ext_handles *h = chain_find(ci, VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO);
    if (!h || !(h->handleTypes & OPAQUE)) return 0;
    h->handleTypes &= ~(VkExternalMemoryHandleTypeFlags)OPAQUE;
    vkb_srv_table *t = e->dev;
    int ded;
    int way = ci->tiling == VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT
                  ? IMG_DMABUF
                  : image_way(&t->real, t->physical, ci->format, ci->imageType, ci->tiling, ci->usage, ci->flags, NULL, NULL, &ded);
    if (way == IMG_DMABUF) {
        h->handleTypes |= DMABUF;
        tell_once(&told_dmabuf, "shared images are made as dma-buf images");
    } else {
        /* IMG_NO too: the request goes on without the opaque type, as before this emulation. */
        if (!h->handleTypes) vkb_emu_chain_take(ci, VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO);
        tell_once(&told_plain, "shared images the driver will not import dma-bufs into are made without external info "
                               "(their memory is still a dma-buf; both processes get the same layout)");
    }
    return 1;
}

/* A dma-buf resource the driver refused: made as the app's resource without external memory
 * (its export will fail), as before this emulation. */
int vkb_opaque_retry(const void *ci, VkStructureType t)
{
    ext_handles *h = chain_find(ci, t);
    if (!h || !(h->handleTypes & DMABUF)) return 0;
    h->handleTypes &= ~(VkExternalMemoryHandleTypeFlags)DMABUF;
    if (!h->handleTypes) vkb_emu_chain_take(ci, t);
    static int told;
    tell_once(&told, "the driver refused a dma-buf resource; made without (its export will fail)");
    return 1;
}

/* ------------------------------------------------------------------ memory */

/* The memory types an allocation for `ai`'s dedicated resource may use (all if none). */
static uint32_t dedicated_bits(vkb_srv_table *t, const VkMemoryAllocateInfo *ai)
{
    const VkMemoryDedicatedAllocateInfo *d = chain_find(ai, VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO);
    VkMemoryRequirements req;
    if (d && d->image) {
        t->real.vkGetImageMemoryRequirements(t->device, d->image, &req);
        return req.memoryTypeBits;
    }
    if (d && d->buffer) {
        t->real.vkGetBufferMemoryRequirements(t->device, d->buffer, &req);
        return req.memoryTypeBits;
    }
    return UINT32_MAX;
}

int vkb_opaque_alloc_info(vkb_srv_table *t, VkMemoryAllocateInfo *ai)
{
    static int told_export, told_import;
    if (!dev_on(t)) return 0;
    int done = 0;
    ext_handles *exp = chain_find(ai, VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO);
    if (exp && (exp->handleTypes & OPAQUE)) {
        exp->handleTypes = (exp->handleTypes & ~(VkExternalMemoryHandleTypeFlags)OPAQUE) | DMABUF;
        tell_once(&told_export, "exportable memory is made as dma-buf exportable memory");
        done = 1;
    }
    VkImportMemoryFdInfoKHR *imp = chain_find(ai, VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR);
    if (imp && imp->handleType == OPAQUE) {
        imp->handleType = DMABUF;
        /* The exporter's memory type may have been changed to one the dma-buf takes (dma-heap
         * pages land in Mali's type 1): the importer's is changed the same way. */
        VkMemoryFdPropertiesKHR fp = {VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
        if (t->real.vkGetMemoryFdPropertiesKHR && t->real.vkGetMemoryFdPropertiesKHR(t->device, DMABUF, imp->fd, &fp) == VK_SUCCESS) {
            uint32_t bits = fp.memoryTypeBits & dedicated_bits(t, ai);
            if (!bits) bits = fp.memoryTypeBits;
            if (bits && !(bits & (1u << ai->memoryTypeIndex))) ai->memoryTypeIndex = (uint32_t)__builtin_ctz(bits);
        }
        tell_once(&told_import, "opaque fds are imported as dma-bufs");
        done = 1;
    }
    return done;
}

VkResult vkb_opaque_alloc_retry(vkb_srv_table *t, VkMemoryAllocateInfo *ai, vkb_srv_mem *m)
{
    /* Some Mali drivers reject a dedicated struct with a dma-buf import (server_memory.c). */
    const VkImportMemoryFdInfoKHR *imp = chain_find(ai, VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR);
    if (!imp || !chain_find(ai, VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO)) return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    int keep = fcntl(imp->fd, F_DUPFD_CLOEXEC, 0);
    vkb_emu_chain_take(ai, VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO);
    /* With the dma-heap export emulation on, a later export gives back these pages. */
    if (t->pd->info.emu & VKB_EMU_DMABUF_EXPORT) vkb_emu_chain_take(ai, VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO);
    VkResult r = t->dt.vkAllocateMemory(t->device, ai, NULL, &m->mem);
    VKB_INFO("opaque fd emulation: import of %llu bytes without the dedicated struct: %d", (unsigned long long)ai->allocationSize, r);
    if (r == VK_SUCCESS && keep >= 0 && !m->export_fd1 && (t->pd->info.emu & VKB_EMU_DMABUF_EXPORT)) m->export_fd1 = keep + 1;
    else if (keep >= 0) close(keep);
    return r;
}

/* Host-visible (shared) memory asking for an opaque export: the shared strategy's own dma-buf is
 * the export (no export info reaches the driver). 1 = asked. */
int vkb_opaque_shared_info(vkb_srv_table *t, VkMemoryAllocateInfo *ai)
{
    if (!dev_on(t)) return 0;
    ext_handles *exp = chain_find(ai, VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO);
    if (!exp || !(exp->handleTypes & OPAQUE)) return 0;
    exp->handleTypes &= ~(VkExternalMemoryHandleTypeFlags)OPAQUE;
    if (!exp->handleTypes) vkb_emu_chain_take(ai, VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO);
    return 1;
}

void vkb_opaque_shared_done(vkb_srv_mem *m, int out_fd)
{
    if (!m || m->export_fd1) return;
    int d = out_fd >= 0 ? fcntl(out_fd, F_DUPFD_CLOEXEC, 0) : -1;
    if (d >= 0) m->export_fd1 = d + 1;
    else VKB_WARN("opaque fd emulation: host-visible memory with no dma-buf behind it (hostptr sharing); its export will fail");
}

/* vkGetMemoryFdKHR(OPAQUE_FD): the dma-buf. NULL when gi is left as it is. */
const VkMemoryGetFdInfoKHR *vkb_opaque_get_fd_info(vkb_srv_table *t, const VkMemoryGetFdInfoKHR *gi, VkMemoryGetFdInfoKHR *tmp)
{
    if (!gi || gi->handleType != OPAQUE || !dev_on(t)) return gi;
    *tmp = *gi;
    tmp->handleType = DMABUF;
    return tmp;
}
