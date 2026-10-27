/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * Emulation wrappers installed into a device's dispatch table, like a Vulkan layer: the generated
 * handlers call c->dt, which for an emulating device points here first. The device is found
 * through the per-call table the connection loop sets (vkb_srv_current_table).
 *
 * VK_KHR_maintenance5 (Zink and DXVK 2.7 refuse a Vulkan 1.3 device without it):
 *   - 64-bit flags structures (pipeline create flags 2, buffer usage 2) folded into the 32-bit
 *     fields, which hold every bit a 1.3 driver knows;
 *   - shader modules given inline (VkShaderModuleCreateInfo chained to a stage) created and
 *     destroyed around the pipeline;
 *   - vkCmdBindIndexBuffer2, vkGetRenderingAreaGranularity, vkGetImageSubresourceLayout2 and
 *     vkGetDeviceImageSubresourceLayout on their 1.0 equivalents;
 *   - VK_REMAINING_ARRAY_LAYERS in copy regions and VK_WHOLE_SIZE in vertex buffer sizes, resolved
 *     from the image/buffer the server keeps a record of.
 */
#define _GNU_SOURCE
#include "server_emu.h"
#include "vkb_emu_formats.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ resource records */

static unsigned rh(uint64_t k)
{
    k ^= k >> 29;
    k *= 0xbf58476d1ce4e5b9ull;
    return (unsigned)(k >> 52) & (VKB_EMU_BUCKETS - 1);
}

void vkb_emu_track_image(vkb_emu_device *e, VkImage img, const VkImageCreateInfo *ci, VkFormat client_format)
{
    vkb_emu_image *r = calloc(1, sizeof(*r));
    r->image = img;
    r->type = ci->imageType;
    r->format = ci->format;
    r->client_format = client_format;
    r->extent = ci->extent;
    r->mips = ci->mipLevels;
    r->layers = ci->arrayLayers;
    r->plain_ci = *ci;
    r->plain_ci.pNext = NULL;
    for (const VkBaseInStructure *b = ci->pNext; b; b = b->pNext)
        if (b->sType == VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO &&
            (((const VkExternalMemoryImageCreateInfo *)b)->handleTypes & 0x400 /* ANDROID_HARDWARE_BUFFER_BIT_ANDROID */))
            r->ahb_external = 1;
    pthread_mutex_lock(&e->lock);
    unsigned h = rh((uint64_t)img);
    r->next = e->images[h];
    e->images[h] = r;
    pthread_mutex_unlock(&e->lock);
}

vkb_emu_image *vkb_emu_find_image(vkb_emu_device *e, VkImage img)
{
    pthread_mutex_lock(&e->lock);
    vkb_emu_image *r = e->images[rh((uint64_t)img)];
    while (r && r->image != img) r = r->next;
    pthread_mutex_unlock(&e->lock);
    return r;
}

static void image_unaccount(const vkb_emu_image *r);

static void untrack_image(vkb_emu_device *e, VkImage img)
{
    pthread_mutex_lock(&e->lock);
    vkb_emu_image **pp = &e->images[rh((uint64_t)img)];
    while (*pp && (*pp)->image != img) pp = &(*pp)->next;
    vkb_emu_image *r = *pp;
    if (r) *pp = r->next;
    pthread_mutex_unlock(&e->lock);
    if (r) {
        image_unaccount(r);
        vkb_texcap_destroyed(r);
        vkb_emu_image_release(e, r);
        free(r);
    }
}

static void track_buffer(vkb_emu_device *e, VkBuffer b, VkDeviceSize size)
{
    vkb_emu_buffer *r = calloc(1, sizeof(*r));
    r->buffer = b;
    r->size = size;
    pthread_mutex_lock(&e->lock);
    unsigned h = rh((uint64_t)b);
    r->next = e->buffers[h];
    e->buffers[h] = r;
    pthread_mutex_unlock(&e->lock);
}

VkDeviceSize vkb_emu_buffer_size(vkb_emu_device *e, VkBuffer b)
{
    pthread_mutex_lock(&e->lock);
    vkb_emu_buffer *r = e->buffers[rh((uint64_t)b)];
    while (r && r->buffer != b) r = r->next;
    VkDeviceSize s = r ? r->size : 0;
    pthread_mutex_unlock(&e->lock);
    return s;
}

static void untrack_buffer(vkb_emu_device *e, VkBuffer b)
{
    pthread_mutex_lock(&e->lock);
    vkb_emu_buffer **pp = &e->buffers[rh((uint64_t)b)];
    while (*pp && (*pp)->buffer != b) pp = &(*pp)->next;
    vkb_emu_buffer *r = *pp;
    if (r) *pp = r->next;
    pthread_mutex_unlock(&e->lock);
    if (r) vkb_memstat_buffer(-(int64_t)r->size);
    free(r);
}

vkb_emu_device *vkb_emu_cur(void)
{
    vkb_srv_table *t = vkb_srv_current_table();
    return t ? t->emu : NULL;
}

const vkb_dispatch *vkb_emu_real(void)
{
    return &vkb_srv_current_table()->real;
}

/* Removes the first structure of type t from a chain we own (decoded into the call's arena). */
void *vkb_emu_chain_take(const void *head_const, VkStructureType t)
{
    VkBaseOutStructure *prev = (VkBaseOutStructure *)head_const;
    while (prev->pNext) {
        if (prev->pNext->sType == t) {
            VkBaseOutStructure *found = prev->pNext;
            prev->pNext = found->pNext;
            return found;
        }
        prev = prev->pNext;
    }
    return NULL;
}

/* Opaque-fd external memory on a driver without it (Mali r44: "opaque fd: features 0x0"). Zink
 * asks for it on GL resources it may share (tablet run 9: every such image refused with
 * VK_ERROR_INVALID_EXTERNAL_HANDLE, and Steam's browser lost its GPU process over it). The opaque
 * handle type is taken out of the request - VkExternalMemoryImageCreateInfo,
 * VkExternalMemoryBufferCreateInfo and VkExportMemoryAllocateInfo all hold handleTypes right after
 * the header - so the resource exists; exporting it later fails (vkGetMemoryFdKHR, logged). */
typedef struct {
    VkStructureType sType;
    const void *pNext;
    VkExternalMemoryHandleTypeFlags handleTypes;
} ext_handles;

int vkb_emu_strip_opaque(const void *head, VkStructureType t)
{
    for (VkBaseOutStructure *b = (VkBaseOutStructure *)((const VkBaseInStructure *)head)->pNext; b; b = b->pNext) {
        if (b->sType != t) continue;
        ext_handles *h = (ext_handles *)b;
        if (!(h->handleTypes & VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT)) return 0;
        h->handleTypes &= ~(VkExternalMemoryHandleTypeFlags)VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        if (!h->handleTypes) vkb_emu_chain_take(head, t);
        static int told;
        if (!__atomic_fetch_add(&told, 1, __ATOMIC_RELAXED))
            VKB_WARN("external memory: the driver has no opaque fds; resources asking for them are made without (export would fail)");
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ resources */

static VKAPI_ATTR VkResult VKAPI_CALL emu_CreateBuffer(VkDevice device, const VkBufferCreateInfo *pCreateInfo,
                                                      const VkAllocationCallbacks *pAllocator, VkBuffer *pBuffer)
{
    vkb_emu_device *e = vkb_emu_cur();
    VkBufferCreateInfo *ci = (VkBufferCreateInfo *)pCreateInfo;
    if (e->flags & VKB_EMU_MAINT5) {
        VkBufferUsageFlags2CreateInfo *u = vkb_emu_chain_take(ci, VK_STRUCTURE_TYPE_BUFFER_USAGE_FLAGS_2_CREATE_INFO);
        if (u) ci->usage = (VkBufferUsageFlags)u->usage;
    }
    vkb_emu_bcn_buffer_usage(e, ci);
    vkb_emu_polygon_buffer_usage(e, ci);
    int opaque = vkb_opaque_buffer(e, ci);
    VkResult r = vkb_emu_real()->vkCreateBuffer(device, ci, pAllocator, pBuffer);
    if (r != VK_SUCCESS && opaque && vkb_opaque_retry(ci, VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO))
        r = vkb_emu_real()->vkCreateBuffer(device, ci, pAllocator, pBuffer);
    if (r == VK_ERROR_INVALID_EXTERNAL_HANDLE && vkb_emu_strip_opaque(ci, VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO))
        r = vkb_emu_real()->vkCreateBuffer(device, ci, pAllocator, pBuffer);
    if (r == VK_SUCCESS) {
        track_buffer(e, *pBuffer, ci->size);
        vkb_memstat_buffer((int64_t)ci->size);
    }
    return r;
}

static VKAPI_ATTR void VKAPI_CALL emu_DestroyBuffer(VkDevice device, VkBuffer buffer, const VkAllocationCallbacks *pAllocator)
{
    if (buffer) untrack_buffer(vkb_emu_cur(), buffer);
    vkb_emu_real()->vkDestroyBuffer(device, buffer, pAllocator);
}

static VKAPI_ATTR VkResult VKAPI_CALL emu_CreateBufferView(VkDevice device, const VkBufferViewCreateInfo *pCreateInfo,
                                                          const VkAllocationCallbacks *pAllocator, VkBufferView *pView)
{
    vkb_emu_device *e = vkb_emu_cur();
    if (e->flags & VKB_EMU_TEXEL_ALIGN && (pCreateInfo->offset & 63)) {
        /* Single-texel alignment is reported but the GPU wants 64 bytes for storage texel buffers;
         * the view goes through as asked. If a game draws wrong, these lines say where to look. */
        static int told;
        if (__atomic_fetch_add(&told, 1, __ATOMIC_RELAXED) < 8)
            VKB_WARN("texel buffer view at offset %llu (format %d, range %llu): not 64-byte aligned; right only if a uniform texel "
                     "buffer, or if the GPU takes it anyway", (unsigned long long)pCreateInfo->offset, pCreateInfo->format,
                     (unsigned long long)pCreateInfo->range);
    }
    if (e->flags & VKB_EMU_MAINT5) vkb_emu_chain_take(pCreateInfo, VK_STRUCTURE_TYPE_BUFFER_USAGE_FLAGS_2_CREATE_INFO);
    return vkb_emu_real()->vkCreateBufferView(device, pCreateInfo, pAllocator, pView);
}

/* Requirements for an image not created yet are asked of the image the server will create: BC
 * formats decoded to RGBA and opaque-fd sharing as in emu_CreateImage. Asked as the app had it,
 * Mali answered 0 bytes for a BC1 texture, and vkd3d-proton handed Deep Rock Galactic (D3D12) a
 * heap size of 0 (CreateHeap E_INVALIDARG, a crash at start). */
static VKAPI_ATTR void VKAPI_CALL emu_GetDeviceImageMemoryRequirements(VkDevice device, const VkDeviceImageMemoryRequirements *pInfo,
                                                                      VkMemoryRequirements2 *pReq)
{
    vkb_emu_device *e = vkb_emu_cur();
    VkImageCreateInfo ci = *pInfo->pCreateInfo;
    vkb_texcap_create_info(e, &ci);
    vkb_emu_image_create_info(e, &ci);
    vkb_opaque_image(e, &ci);
    VkDeviceImageMemoryRequirements info = *pInfo;
    info.pCreateInfo = &ci;
    vkb_emu_real()->vkGetDeviceImageMemoryRequirements(device, &info, pReq);
}

static VKAPI_ATTR void VKAPI_CALL emu_GetDeviceImageSparseMemoryRequirements(VkDevice device, const VkDeviceImageMemoryRequirements *pInfo,
                                                                            uint32_t *pCount, VkSparseImageMemoryRequirements2 *pReq)
{
    vkb_emu_device *e = vkb_emu_cur();
    VkImageCreateInfo ci = *pInfo->pCreateInfo;
    vkb_texcap_create_info(e, &ci);
    vkb_emu_image_create_info(e, &ci);
    VkDeviceImageMemoryRequirements info = *pInfo;
    info.pCreateInfo = &ci;
    vkb_emu_real()->vkGetDeviceImageSparseMemoryRequirements(device, &info, pCount, pReq);
}

/* Mali answers rowPitch 0 for a linear or modifier image of one row (its pitch never matters
 * there), and Chromium's GPU process aborts on a GBM buffer with stride 0: every 1xN GPU memory
 * buffer Steam's browser made killed it. Such layouts get the row's own size as the pitch
 * (fix_single_row_pitch), and when the same buffer comes back as an explicit layout and Mali
 * refuses that pitch, the import is retried with the 0 it reported. */
static int single_row_pitch_to_driver(VkImageCreateInfo *ci)
{
    if (ci->extent.height != 1 || ci->extent.depth != 1) return 0;
    for (const VkBaseInStructure *b = ci->pNext; b; b = b->pNext)
        if (b->sType == VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT) {
            const VkImageDrmFormatModifierExplicitCreateInfoEXT *x = (const void *)b;
            int changed = 0;
            for (uint32_t i = 0; i < x->drmFormatModifierPlaneCount; i++) {
                VkSubresourceLayout *l = (VkSubresourceLayout *)&x->pPlaneLayouts[i];
                if (l->rowPitch) {
                    l->rowPitch = 0;
                    changed = 1;
                }
            }
            return changed;
        }
    return 0;
}

static void fix_single_row_pitch(VkImage image, const VkImageSubresource *sub, VkSubresourceLayout *l)
{
    if (l->rowPitch || !l->size) return;
    vkb_emu_image *r = vkb_emu_find_image(vkb_emu_cur(), image);
    uint32_t mip = sub->aspectMask & (VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT | VK_IMAGE_ASPECT_MEMORY_PLANE_1_BIT_EXT |
                                      VK_IMAGE_ASPECT_MEMORY_PLANE_2_BIT_EXT | VK_IMAGE_ASPECT_MEMORY_PLANE_3_BIT_EXT)
                       ? 0 : sub->mipLevel;
    if (!r || r->type == VK_IMAGE_TYPE_3D || (r->extent.height >> mip) > 1) return;
    l->rowPitch = l->size;
}

/* A modifier-list image made plain LINEAR (linear_mod_plain): its memory plane 0 is its colour aspect. */
static const VkImageSubresource *linear_mod_sub(VkImage image, const VkImageSubresource *sub, VkImageSubresource *tmp)
{
    if (!(sub->aspectMask & VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT)) return sub;
    vkb_emu_image *r = vkb_emu_find_image(vkb_emu_cur(), image);
    if (!r || !r->linear_mod) return sub;
    *tmp = *sub;
    tmp->aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    return tmp;
}

/*
 * Images made with the AHardwareBuffer handle type: the client's shareable linear images under the
 * AHB memory strategy (client/memory.c, vkb_ep_vkCreateImage; Retroid Pocket 4 Pro: no dma-heap).
 * Vulkan leaves such an image's memory requirements undefined until an AHardwareBuffer is bound
 * (VUID-vkGetImageMemoryRequirements-image-04004), and Mali r32p1 answers size 0, alignment 0 -
 * the client then allocated nothing, Zink's mapped staging textures for wined3d (Ys Origin's
 * DirectDraw scratch surface) held garbage and the picture came out white - while
 * vkGetImageSubresourceLayout on the unbound image faulted in the driver (address 0x18) and took
 * the server down. Both are answered from a twin: the same VkImageCreateInfo without the pNext
 * chain, made for the question and destroyed (the layout's twin is bound to plain device memory
 * first). Once the image itself is bound the driver's layout answer is used, since that is how it
 * reads the memory. Newer drivers answer the unbound questions themselves; the twin is used only
 * when the driver's requirements come back empty, or for every layout question before binding.
 */
static int twin_requirements(VkDevice device, vkb_emu_image *r)
{
    if (r->twin_req_ok) return 1;
    VkImage twin = VK_NULL_HANDLE;
    if (vkb_emu_real()->vkCreateImage(device, &r->plain_ci, NULL, &twin) != VK_SUCCESS) return 0;
    vkb_emu_real()->vkGetImageMemoryRequirements(device, twin, &r->twin_req);
    vkb_emu_real()->vkDestroyImage(device, twin, NULL);
    r->twin_req_ok = r->twin_req.size != 0;
    return r->twin_req_ok;
}

static int twin_layout(VkDevice device, vkb_emu_image *r, const VkImageSubresource *sub)
{
    if (r->twin_layout_ok) return 1;
    if (!twin_requirements(device, r)) return 0;
    VkImage twin = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    int ok = 0;
    if (vkb_emu_real()->vkCreateImage(device, &r->plain_ci, NULL, &twin) != VK_SUCCESS) return 0;
    uint32_t type = 0;
    while (type < 32 && !(r->twin_req.memoryTypeBits >> type & 1)) type++;
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, r->twin_req.size, type};
    if (type < 32 && vkb_emu_real()->vkAllocateMemory(device, &ai, NULL, &mem) == VK_SUCCESS &&
        vkb_emu_real()->vkBindImageMemory(device, twin, mem, 0) == VK_SUCCESS) {
        vkb_emu_real()->vkGetImageSubresourceLayout(device, twin, sub, &r->twin_layout);
        ok = r->twin_layout_ok = 1;
    }
    vkb_emu_real()->vkDestroyImage(device, twin, NULL);
    if (mem) vkb_emu_real()->vkFreeMemory(device, mem, NULL);
    return ok;
}

static void ahb_image_requirements(VkDevice device, VkImage image, VkMemoryRequirements *req)
{
    vkb_emu_image *r = vkb_emu_find_image(vkb_emu_cur(), image);
    if (!r || !r->ahb_external || (req->size && req->memoryTypeBits)) return;
    if (!twin_requirements(device, r)) return;
    static int told;
    if (__atomic_fetch_add(&told, 1, __ATOMIC_RELAXED) < 4)
        VKB_INFO("AHardwareBuffer image %llx (format %d %ux%u tiling %d): the driver answers no memory requirements before a buffer is "
                 "bound (size %llu, types 0x%x); answered from a twin without the handle type: size %llu, alignment %llu, types 0x%x",
                 (unsigned long long)(uintptr_t)image, r->format, r->extent.width, r->extent.height, (int)r->plain_ci.tiling,
                 (unsigned long long)req->size, req->memoryTypeBits, (unsigned long long)r->twin_req.size,
                 (unsigned long long)r->twin_req.alignment, r->twin_req.memoryTypeBits);
    req->size = r->twin_req.size;
    req->alignment = r->twin_req.alignment;
    if (!req->memoryTypeBits) req->memoryTypeBits = r->twin_req.memoryTypeBits;
}

static VKAPI_ATTR void VKAPI_CALL emu_GetImageMemoryRequirements(VkDevice device, VkImage image, VkMemoryRequirements *req)
{
    vkb_emu_real()->vkGetImageMemoryRequirements(device, image, req);
    ahb_image_requirements(device, image, req);
}

static VKAPI_ATTR void VKAPI_CALL emu_GetImageMemoryRequirements2(VkDevice device, const VkImageMemoryRequirementsInfo2 *info,
                                                                 VkMemoryRequirements2 *req)
{
    vkb_emu_real()->vkGetImageMemoryRequirements2(device, info, req);
    ahb_image_requirements(device, info->image, &req->memoryRequirements);
}

static VKAPI_ATTR VkResult VKAPI_CALL emu_BindImageMemory(VkDevice device, VkImage image, VkDeviceMemory memory, VkDeviceSize offset)
{
    VkResult r = vkb_emu_real()->vkBindImageMemory(device, image, memory, offset);
    vkb_emu_image *rec = r == VK_SUCCESS ? vkb_emu_find_image(vkb_emu_cur(), image) : NULL;
    if (rec) rec->bound = 1;
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL emu_BindImageMemory2(VkDevice device, uint32_t n, const VkBindImageMemoryInfo *infos)
{
    VkResult r = vkb_emu_real()->vkBindImageMemory2(device, n, infos);
    if (r == VK_SUCCESS)
        for (uint32_t i = 0; i < n; i++) {
            vkb_emu_image *rec = vkb_emu_find_image(vkb_emu_cur(), infos[i].image);
            if (rec) rec->bound = 1;
        }
    return r;
}

static VKAPI_ATTR void VKAPI_CALL emu_GetImageSubresourceLayout(VkDevice device, VkImage image, const VkImageSubresource *sub,
                                                               VkSubresourceLayout *layout)
{
    VkImageSubresource tmp;
    sub = linear_mod_sub(image, sub, &tmp);
    vkb_emu_image *r = vkb_emu_find_image(vkb_emu_cur(), image);
    /* Before binding the driver faults; after binding a BLOB AHardwareBuffer it answers rowPitch 0
     * and four times the size (r32p1, 320x240 BGRA8: size 1228800): the twin's layout both times. */
    if (r && r->ahb_external) {
        if (twin_layout(device, r, sub)) {
            static int told;
            if (__atomic_fetch_add(&told, 1, __ATOMIC_RELAXED) < 4)
                VKB_INFO("AHardwareBuffer image %llx (%s): layout answered from a bound twin without the handle type: offset %llu size %llu "
                         "rowPitch %llu", (unsigned long long)(uintptr_t)image, r->bound ? "bound" : "not yet bound; the driver faults there",
                         (unsigned long long)r->twin_layout.offset, (unsigned long long)r->twin_layout.size,
                         (unsigned long long)r->twin_layout.rowPitch);
            *layout = r->twin_layout;
            fix_single_row_pitch(image, sub, layout);
            return;
        }
        VKB_WARN("AHardwareBuffer image %llx: no twin could answer its layout; asking the driver%s", (unsigned long long)(uintptr_t)image,
                 r->bound ? "" : " (not yet bound: it may fault)");
    }
    vkb_emu_real()->vkGetImageSubresourceLayout(device, image, sub, layout);
    fix_single_row_pitch(image, sub, layout);
}

static VKAPI_ATTR VkResult VKAPI_CALL emu_GetImageDrmFormatModifierPropertiesEXT(VkDevice device, VkImage image,
                                                                                VkImageDrmFormatModifierPropertiesEXT *props)
{
    vkb_emu_image *r = vkb_emu_find_image(vkb_emu_cur(), image);
    if (r && r->linear_mod) {
        props->drmFormatModifier = 0; /* DRM_FORMAT_MOD_LINEAR */
        return VK_SUCCESS;
    }
    const vkb_dispatch *real = vkb_emu_real();
    return real->vkGetImageDrmFormatModifierPropertiesEXT ? real->vkGetImageDrmFormatModifierPropertiesEXT(device, image, props)
                                                          : VK_ERROR_FEATURE_NOT_PRESENT;
}

/* Accounting (server_memory.c): what the driver asks for the image and, for an emulated BC image,
 * what the app's compressed one would have taken and what its decoded texels take. */
static uint32_t decoded_texel_bytes(VkFormat f)
{
    switch (f) {
    case VK_FORMAT_R8_UNORM: case VK_FORMAT_R8_SNORM: return 1;
    case VK_FORMAT_R8G8_UNORM: case VK_FORMAT_R8G8_SNORM: case VK_FORMAT_R5G6B5_UNORM_PACK16: case VK_FORMAT_A1R5G5B5_UNORM_PACK16: return 2;
    case VK_FORMAT_R16G16B16A16_SFLOAT: return 8;
    default: return 4;
    }
}

static void image_account(vkb_emu_device *e, VkDevice device, VkImage img, const VkImageCreateInfo *ci, const VkImageCreateInfo *app,
                          VkFormat client_format)
{
    vkb_emu_image *r = ci->flags & VK_IMAGE_CREATE_DISJOINT_BIT ? NULL : vkb_emu_find_image(e, img); /* disjoint: per plane */
    if (!r) return;
    VkMemoryRequirements req;
    vkb_emu_real()->vkGetImageMemoryRequirements(device, img, &req);
    int64_t src = 0, ideal = 0;
    if (client_format != ci->format && vkb_is_bc(client_format)) {
        int64_t block = vkb_bc_block_bytes(client_format), texel = decoded_texel_bytes(ci->format), tx = vkb_tx_block_bytes(ci->format);
        /* the BC data as the app made the image (all its levels), and what the driver holds of it */
        for (uint32_t m = 0; m < app->mipLevels; m++) {
            uint32_t w = app->extent.width >> m, h = app->extent.height >> m, d = app->extent.depth >> m;
            w = w ? w : 1, h = h ? h : 1, d = d ? d : 1;
            src += (int64_t)((w + 3) / 4) * ((h + 3) / 4) * d * block;
        }
        for (uint32_t m = 0; m < ci->mipLevels; m++) {
            uint32_t w = ci->extent.width >> m, h = ci->extent.height >> m, d = ci->extent.depth >> m;
            w = w ? w : 1, h = h ? h : 1, d = d ? d : 1;
            uint32_t n = vkb_tx_block_dim(ci->format);
            ideal += tx ? (int64_t)((w + n - 1) / n) * ((h + n - 1) / n) * d * tx : (int64_t)w * h * d * texel; /* transcoded: its blocks */
        }
        src *= ci->arrayLayers, ideal *= ci->arrayLayers;
    } else {
        client_format = 0;
    }
    r->stat_bytes = (int64_t)req.size;
    r->stat_bc_src = src;
    r->stat_ideal = ideal;
    if (r->stat_bytes || src) vkb_memstat_image(client_format, r->stat_bytes, src, ideal);
}

static void image_unaccount(const vkb_emu_image *r)
{
    if (r->stat_bytes || r->stat_bc_src)
        vkb_memstat_image(r->stat_bc_src ? r->client_format : 0, -r->stat_bytes, -r->stat_bc_src, -r->stat_ideal);
}

/* Mali r44 makes dma-buf images only at DRM-modifier tiling: VkExternalMemoryImageCreateInfo with
 * DMA_BUF on a LINEAR (or OPTIMAL) image is refused with
 * VK_ERROR_INVALID_DRM_FORMAT_MODIFIER_PLANE_LAYOUT_EXT. Zink asks for exactly that for its shared
 * linear resources (it checks the format without the external struct), and Skyrim Special
 * Edition's process gave up 50 ms after a refused 4096x1 R32_SFLOAT 1D one (2026-10-14). Such an
 * image is made without the dma-buf handle type, as the opaque-fd emulation makes the images the
 * driver will not import into (server_opaque.c): its memory still comes from the allocation that
 * asks to export (the dma-heap export emulation on Mali), the driver lays it out as the same
 * plain image in every process, and a linear image's layout is vkGetImageSubresourceLayout's.
 * A modifier image is left refused: its layout is what the importer was promised.
 * VKBRIDGE_FAKE_MISSING=dmabuflinear imitates the refusal on the host. */
static int dmabuf_plain_retry(VkImageCreateInfo *ci)
{
    if (ci->tiling == VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT) return 0;
    for (VkBaseOutStructure *b = (VkBaseOutStructure *)ci->pNext; b; b = b->pNext) {
        if (b->sType != VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO) continue;
        ext_handles *h = (ext_handles *)b;
        if (!(h->handleTypes & VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT)) return 0;
        static int told;
        if (__atomic_fetch_add(&told, 1, __ATOMIC_RELAXED) < 8)
            VKB_INFO("dma-buf image refused at tiling %d (type %d format %d %ux%ux%u usage 0x%x); made without the dma-buf "
                     "handle type (its memory is exported as allocated)", ci->tiling, ci->imageType, ci->format, ci->extent.width,
                     ci->extent.height, ci->extent.depth, ci->usage);
        h->handleTypes &= ~(VkExternalMemoryHandleTypeFlags)VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
        if (!h->handleTypes) vkb_emu_chain_take(ci, VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO);
        return 1;
    }
    return 0;
}

/* The export images of gamescope's nested backend (and any client exporting through a modifier list):
 * DRM-modifier tiling with VkImageDrmFormatModifierListCreateInfoEXT and DMA_BUF. Arm's r32p1
 * (Mali-G77 MC9, Retroid Pocket 4 Pro, MaliBridge#10) most likely refuses them at the LINEAR
 * modifier: it refuses dma-buf images at LINEAR tiling outright (the dmabuf_linear_image probe), and
 * gamescope found no exportable modifier for its output images although the export emulation was on.
 * Where the emulation is on and the list holds LINEAR, such an image is made as a plain LINEAR image
 * without the dma-buf handle type when the driver refuses its LINEAR-modifier dma-buf query (the
 * client then answered that query from the plain LINEAR one, client/physdev.c) or refuses to create
 * it. Its memory comes from the emulation's pages as for any image that asks to export; the image
 * reports the LINEAR modifier (emu_GetImageDrmFormatModifierPropertiesEXT) and its plane 0 layout
 * is its linear layout, with the single-row pitch patch. An image the driver makes itself is
 * left as it was. */
static int has_linear_mod_list(const VkImageCreateInfo *ci)
{
    if (ci->tiling != VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT) return 0;
    int list = 0, dmabuf = 0;
    for (const VkBaseInStructure *b = ci->pNext; b; b = b->pNext) {
        if (b->sType == VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT) {
            const VkImageDrmFormatModifierListCreateInfoEXT *l = (const void *)b;
            for (uint32_t i = 0; i < l->drmFormatModifierCount; i++) list |= l->pDrmFormatModifiers[i] == 0 /* DRM_FORMAT_MOD_LINEAR */;
        }
        if (b->sType == VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO)
            dmabuf |= (((const VkExternalMemoryImageCreateInfo *)b)->handleTypes & VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT) != 0;
    }
    return list && dmabuf && ci->imageType == VK_IMAGE_TYPE_2D && ci->mipLevels == 1 && ci->arrayLayers == 1 &&
           ci->samples == VK_SAMPLE_COUNT_1_BIT;
}

/* Whether the driver refuses (or will not import) the LINEAR-modifier dma-buf image ci describes. */
static int linear_mod_refused(const vkb_srv_table *t, const VkImageCreateInfo *ci)
{
    if (vkb_emu_dmabufmod_refused(ci->tiling, ci->usage, ci->flags, ci->pNext)) return 1; /* host imitation */
    const vkb_srv_table *inst = t ? t->parent : NULL;
    if (!inst || !inst->real.vkGetPhysicalDeviceImageFormatProperties2) return 0;
    VkPhysicalDeviceImageDrmFormatModifierInfoEXT mi = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT, NULL, 0,
                                                        ci->sharingMode, 0, NULL};
    VkPhysicalDeviceExternalImageFormatInfo ei = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO, &mi,
                                                  VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    VkImageFormatListCreateInfo fl;
    for (const VkBaseInStructure *b = ci->pNext; b; b = b->pNext)
        if (b->sType == VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO) {
            fl = *(const VkImageFormatListCreateInfo *)b;
            fl.pNext = ei.pNext;
            ei.pNext = &fl;
        }
    VkPhysicalDeviceImageFormatInfo2 ii = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2, &ei, ci->format, ci->imageType,
                                           VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT, ci->usage, ci->flags};
    VkExternalImageFormatProperties ep = {VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
    VkImageFormatProperties2 ip = {VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, &ep};
    VkResult r = inst->real.vkGetPhysicalDeviceImageFormatProperties2(t->physical, &ii, &ip);
    return r != VK_SUCCESS || !(ep.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT);
}

/* ci made plain LINEAR without the dma-buf handle type; why: VK_SUCCESS (the query refused) or what
 * vkCreateImage returned. Returns 1. */
static int linear_mod_plain(VkImageCreateInfo *ci, VkResult why)
{
    static int told;
    if (__atomic_fetch_add(&told, 1, __ATOMIC_RELAXED) < 8)
        VKB_INFO("dma-buf export image with a LINEAR modifier list %s (format %d %ux%u usage 0x%x flags 0x%x); made as a plain LINEAR "
                 "image without the dma-buf handle type, reported as the LINEAR modifier (its memory is exported as allocated)",
                 why == VK_SUCCESS ? "refused by the driver's format query" : "refused by vkCreateImage", ci->format, ci->extent.width,
                 ci->extent.height, ci->usage, ci->flags);
    ci->tiling = VK_IMAGE_TILING_LINEAR;
    vkb_emu_chain_take(ci, VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT);
    for (VkBaseOutStructure *b = (VkBaseOutStructure *)ci->pNext; b; b = b->pNext)
        if (b->sType == VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO) {
            ext_handles *h = (ext_handles *)b;
            h->handleTypes &= ~(VkExternalMemoryHandleTypeFlags)VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
            if (!h->handleTypes) vkb_emu_chain_take(ci, VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO);
            break;
        }
    return 1;
}

/* The compositor's side of the same images: gamescope imports a client's LINEAR dma-buf at
 * DRM-modifier tiling with VkImageDrmFormatModifierExplicitCreateInfoEXT (the LINEAR modifier, one
 * plane, the exporter's layout), sampled with a mutable format. Where the driver refuses that
 * query, the client answers it from plain LINEAR as importable (client/physdev.c): answered without
 * IMPORTABLE, gamescope made the import an OPTIMAL image with an implicit layout, which on Mali needs
 * more memory than the linear dma-buf has (S10+ under dmaheap,dmabufmod: 3993600 bytes against
 * 3719168, VK_ERROR_INVALID_EXTERNAL_HANDLE) and would read tiled texels from linear ones anyway.
 * Such an image is made as a plain LINEAR image without the dma-buf handle type (as
 * dmabuf_plain_retry does for LINEAR imports), and kept only if its own layout is the one the
 * importer was given; otherwise it is refused as before. Its memory is the imported dma-buf. */
static int has_linear_mod_explicit(const VkImageCreateInfo *ci, VkSubresourceLayout *plane)
{
    if (ci->tiling != VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT) return 0;
    int lin = 0, dmabuf = 0;
    for (const VkBaseInStructure *b = ci->pNext; b; b = b->pNext) {
        if (b->sType == VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT) {
            const VkImageDrmFormatModifierExplicitCreateInfoEXT *x = (const void *)b;
            lin = x->drmFormatModifier == 0 /* DRM_FORMAT_MOD_LINEAR */ && x->drmFormatModifierPlaneCount == 1 && x->pPlaneLayouts;
            if (lin) *plane = x->pPlaneLayouts[0];
        }
        if (b->sType == VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO)
            dmabuf |= (((const VkExternalMemoryImageCreateInfo *)b)->handleTypes & VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT) != 0;
    }
    return lin && dmabuf && ci->imageType == VK_IMAGE_TYPE_2D && ci->mipLevels == 1 && ci->arrayLayers == 1 &&
           ci->samples == VK_SAMPLE_COUNT_1_BIT;
}

/* ci (has_linear_mod_explicit) made plain LINEAR without the dma-buf handle type. Returns 1. */
static int linear_import_plain(VkImageCreateInfo *ci, VkResult why)
{
    static int told;
    if (__atomic_fetch_add(&told, 1, __ATOMIC_RELAXED) < 8)
        VKB_INFO("dma-buf import image at the LINEAR modifier %s (format %d %ux%u usage 0x%x flags 0x%x); made as a plain LINEAR image "
                 "without the dma-buf handle type, reported as the LINEAR modifier (its memory is the imported dma-buf)",
                 why == VK_SUCCESS ? "refused by the driver's format query" : "refused by vkCreateImage", ci->format, ci->extent.width,
                 ci->extent.height, ci->usage, ci->flags);
    ci->tiling = VK_IMAGE_TILING_LINEAR;
    vkb_emu_chain_take(ci, VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT);
    for (VkBaseOutStructure *b = (VkBaseOutStructure *)ci->pNext; b; b = b->pNext)
        if (b->sType == VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO) {
            ext_handles *h = (ext_handles *)b;
            h->handleTypes &= ~(VkExternalMemoryHandleTypeFlags)VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
            if (!h->handleTypes) vkb_emu_chain_take(ci, VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO);
            break;
        }
    return 1;
}

/* Whether the plain LINEAR image lays its texels out where the importer's plane says they are. */
static int linear_import_layout_ok(VkDevice device, VkImage image, const VkImageCreateInfo *ci, const VkSubresourceLayout *want)
{
    VkImageSubresource sub = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
    VkSubresourceLayout l = {0};
    vkb_emu_real()->vkGetImageSubresourceLayout(device, image, &sub, &l);
    int ok = l.offset == want->offset && (ci->extent.height <= 1 || l.rowPitch == want->rowPitch);
    if (!ok)
        VKB_WARN("dma-buf import image at the LINEAR modifier (format %d %ux%u): the plain LINEAR image's layout (offset %llu, row pitch "
                 "%llu) is not the dma-buf's (offset %llu, row pitch %llu); refused", ci->format, ci->extent.width, ci->extent.height,
                 (unsigned long long)l.offset, (unsigned long long)l.rowPitch, (unsigned long long)want->offset,
                 (unsigned long long)want->rowPitch);
    return ok;
}

static VkResult driver_create_image(VkDevice device, const VkImageCreateInfo *ci, const VkAllocationCallbacks *pAllocator, VkImage *pImage)
{
    static int fake = -1;
    if (fake < 0) fake = vkb_emu_faked("dmabuflinear");
    if (fake && ci->tiling != VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT)
        for (const VkBaseInStructure *b = ci->pNext; b; b = b->pNext)
            if (b->sType == VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO &&
                (((const VkExternalMemoryImageCreateInfo *)b)->handleTypes & VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT))
                return VK_ERROR_INVALID_DRM_FORMAT_MODIFIER_PLANE_LAYOUT_EXT; /* as Mali r44 answers */
    if (vkb_emu_planar_ext_refused(ci->format, ci->flags)) return VK_ERROR_FORMAT_NOT_SUPPORTED; /* host imitation */
    if (vkb_emu_dmabufmod_refused(ci->tiling, ci->usage, ci->flags, ci->pNext)) return VK_ERROR_FORMAT_NOT_SUPPORTED; /* host imitation */
    return vkb_emu_real()->vkCreateImage(device, ci, pAllocator, pImage);
}

/* VK_WHOLE_SIZE in vkCmdBindVertexBuffers2's pSizes is a VK_KHR_maintenance5 addition: a driver
 * without it need not take it. Mali-G720 r44p1 does not: a binding bound so reads zeros (MK-04's
 * re-sent strides showed it, 2026-10-17). The emulation above resolves the clients' calls; whatever
 * the bridge itself sends beneath it (the errata fixes, the divisor and polygon emulations) must not
 * send it either. VKBRIDGE_FAKE_MISSING=maint5 imitates such a driver on the host: at the driver
 * boundary, beneath every layer, a VK_WHOLE_SIZE size becomes 0 (the binding reads zeros, as on the
 * G720), and the server log says so. */
static struct {
    vkb_srv_table *t;
    PFN_vkCmdBindVertexBuffers2 next;
} imi_vb2[16];
static uint64_t imi_vb2_whole;

static VKAPI_ATTR void VKAPI_CALL imi_CmdBindVertexBuffers2(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkBuffer *buffers,
                                                           const VkDeviceSize *offsets, const VkDeviceSize *sizes,
                                                           const VkDeviceSize *strides)
{
    vkb_srv_table *t = vkb_srv_current_table();
    PFN_vkCmdBindVertexBuffers2 next = imi_vb2[0].next;
    for (int i = 0; i < 16 && imi_vb2[i].t; i++)
        if (imi_vb2[i].t == t) next = imi_vb2[i].next;
    VkDeviceSize s[32];
    uint32_t whole = 0;
    for (uint32_t i = 0; sizes && i < n && i < 32; i++) {
        s[i] = sizes[i];
        if (s[i] == VK_WHOLE_SIZE) s[i] = 0, whole++;
    }
    if (whole) {
        uint64_t k = __atomic_add_fetch(&imi_vb2_whole, whole, __ATOMIC_RELAXED);
        if (k == whole || !(k & (k - 1)))
            VKB_WARN("VK_WHOLE_SIZE vertex buffer size reached a driver without maintenance5 (imitated: bound with size 0, reads "
                     "zeros); %llu so far", (unsigned long long)k);
    }
    next(cb, first, n, buffers, offsets, whole && n <= 32 ? s : sizes, strides);
}

void vkb_emu_driver_imitate_init(vkb_srv_table *t)
{
    if (!vkb_emu_faked("maint5") || !t->real.vkCmdBindVertexBuffers2 || t->real.vkCmdBindVertexBuffers2 == imi_CmdBindVertexBuffers2)
        return;
    for (int i = 0; i < 16; i++)
        if (!imi_vb2[i].t || imi_vb2[i].t == t) {
            imi_vb2[i].next = t->real.vkCmdBindVertexBuffers2;
            __atomic_store_n(&imi_vb2[i].t, t, __ATOMIC_RELEASE);
            if (t->dt.vkCmdBindVertexBuffers2 == t->real.vkCmdBindVertexBuffers2) t->dt.vkCmdBindVertexBuffers2 = imi_CmdBindVertexBuffers2;
            t->real.vkCmdBindVertexBuffers2 = imi_CmdBindVertexBuffers2;
            return;
        }
}

static VKAPI_ATTR VkResult VKAPI_CALL emu_CreateImage(VkDevice device, const VkImageCreateInfo *pCreateInfo,
                                                     const VkAllocationCallbacks *pAllocator, VkImage *pImage)
{
    vkb_emu_device *e = vkb_emu_cur();
    VkImageCreateInfo *ci = (VkImageCreateInfo *)pCreateInfo;
    VkFormat client_format = ci->format;
    VkImageCreateInfo app = *ci;
    uint32_t drop = vkb_texcap_create_info(e, ci);
    vkb_emu_image_create_info(e, ci);
    int opaque = vkb_opaque_image(e, ci);
    int linear_mod = 0;
    int lm_candidate = (e->flags & VKB_EMU_DMABUF_EXPORT) && has_linear_mod_list(ci);
    if (lm_candidate && linear_mod_refused(e->dev, ci)) linear_mod = linear_mod_plain(ci, VK_SUCCESS);
    VkSubresourceLayout lm_plane = {0};
    int lm_import = !lm_candidate && (e->flags & VKB_EMU_DMABUF_EXPORT) && has_linear_mod_explicit(ci, &lm_plane);
    if (lm_import && linear_mod_refused(e->dev, ci)) linear_mod = linear_import_plain(ci, VK_SUCCESS);
    VkResult r = driver_create_image(device, ci, pAllocator, pImage);
    if (r != VK_SUCCESS && (lm_candidate || lm_import) && !linear_mod) {
        linear_mod = lm_candidate ? linear_mod_plain(ci, r) : linear_import_plain(ci, r);
        r = driver_create_image(device, ci, pAllocator, pImage);
    }
    if (r == VK_SUCCESS && lm_import && linear_mod && !linear_import_layout_ok(device, *pImage, ci, &lm_plane)) {
        vkb_emu_real()->vkDestroyImage(device, *pImage, pAllocator);
        *pImage = VK_NULL_HANDLE;
        return VK_ERROR_INVALID_DRM_FORMAT_MODIFIER_PLANE_LAYOUT_EXT;
    }
    if (r != VK_SUCCESS && opaque && vkb_opaque_retry(ci, VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO))
        r = driver_create_image(device, ci, pAllocator, pImage);
    if (r == VK_ERROR_INVALID_EXTERNAL_HANDLE && vkb_emu_strip_opaque(ci, VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO))
        r = driver_create_image(device, ci, pAllocator, pImage);
    if (r == VK_ERROR_INVALID_DRM_FORMAT_MODIFIER_PLANE_LAYOUT_EXT && single_row_pitch_to_driver(ci))
        r = driver_create_image(device, ci, pAllocator, pImage);
    if (r != VK_SUCCESS && dmabuf_plain_retry(ci)) r = driver_create_image(device, ci, pAllocator, pImage);
    if (r == VK_SUCCESS) {
        vkb_emu_track_image(e, *pImage, ci, client_format);
        if (linear_mod) {
            vkb_emu_image *ti = vkb_emu_find_image(e, *pImage);
            if (ti) ti->linear_mod = 1;
        }
        if (drop) vkb_texcap_created(e, vkb_emu_find_image(e, *pImage), drop, &app.extent, app.mipLevels);
        image_account(e, device, *pImage, ci, &app, client_format);
        for (const VkBaseInStructure *b = ci->pNext; b; b = b->pNext)
            if (b->sType == VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO &&
                (((const VkExternalMemoryImageCreateInfo *)b)->handleTypes & VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT)) {
                vkb_srv_table *t = vkb_srv_current_table();
                VKB_INFO("dma-buf image %llx (pid %d): format %d %ux%u tiling %d usage 0x%x flags 0x%x", (unsigned long long)(uintptr_t)*pImage,
                         t && t->proc ? t->proc->pid : 0, ci->format, ci->extent.width, ci->extent.height, ci->tiling, ci->usage, ci->flags);
                break;
            }
    } else {
        /* What the driver refused, in full (tablet run 9: Zink's shareable images in Steam's
         * browser got VK_ERROR_INVALID_EXTERNAL_HANDLE). Logged for the first few. */
        static int told;
        if (__atomic_fetch_add(&told, 1, __ATOMIC_RELAXED) < 8) {
            char chain[256];
            int len = 0;
            chain[0] = 0;
            for (const VkBaseInStructure *b = ci->pNext; b && len < (int)sizeof(chain) - 48; b = b->pNext) {
                len += snprintf(chain + len, sizeof(chain) - (size_t)len, " %d", (int)b->sType);
                if (b->sType == VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO)
                    len += snprintf(chain + len, sizeof(chain) - (size_t)len, "(handles 0x%x)",
                                    ((const VkExternalMemoryImageCreateInfo *)b)->handleTypes);
                if (b->sType == VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT)
                    len += snprintf(chain + len, sizeof(chain) - (size_t)len, "(%u modifiers)",
                                    ((const VkImageDrmFormatModifierListCreateInfoEXT *)b)->drmFormatModifierCount);
            }
            VKB_ERR("vkCreateImage refused (%d): type %d format %d %ux%ux%u mips %u layers %u samples %d tiling %d usage 0x%x "
                    "flags 0x%x sharing %d, chain:%s", r, ci->imageType, ci->format, ci->extent.width, ci->extent.height,
                    ci->extent.depth, ci->mipLevels, ci->arrayLayers, ci->samples, ci->tiling, ci->usage, ci->flags,
                    ci->sharingMode, chain);
        }
    }
    return r;
}

static VKAPI_ATTR void VKAPI_CALL emu_DestroyImage(VkDevice device, VkImage image, const VkAllocationCallbacks *pAllocator)
{
    if (image) untrack_image(vkb_emu_cur(), image);
    vkb_emu_real()->vkDestroyImage(device, image, pAllocator);
}

/* ------------------------------------------------------------------ pipelines */

/* Fold flags2 and create inline shader modules; temporaries are recorded in mods[]. */
static void prep_stage(VkDevice device, VkPipelineShaderStageCreateInfo *st, VkShaderModule *mods, uint32_t *nmods, uint32_t cap)
{
    if (st->module) return;
    VkShaderModuleCreateInfo *smci = vkb_emu_chain_take(st, VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
    if (!smci) return;
    VkShaderModule m = VK_NULL_HANDLE;
    smci->pNext = NULL;
    if (vkb_emu_driver_module(device, smci, &m) == VK_SUCCESS) {
        st->module = m;
        if (*nmods < cap) mods[(*nmods)++] = m;
    }
}

static VKAPI_ATTR VkResult VKAPI_CALL emu_CreateGraphicsPipelines(VkDevice device, VkPipelineCache cache, uint32_t n,
                                                                 const VkGraphicsPipelineCreateInfo *infos,
                                                                 const VkAllocationCallbacks *pAllocator, VkPipeline *pPipelines)
{
    vkb_emu_device *e = vkb_emu_cur();
    VkShaderModule mods[64];
    uint32_t nmods = 0;
    size_t dsz = vkb_emu_divisor_info_size(), psz = vkb_emu_polygon_info_size();
    uint8_t *divs = calloc(n ? n : 1, dsz), *polys = calloc(n ? n : 1, psz);
    for (uint32_t i = 0; i < n; i++) {
        VkGraphicsPipelineCreateInfo *ci = (VkGraphicsPipelineCreateInfo *)&infos[i];
        vkb_emu_divisor_pipeline(e, device, ci, (div_info *)(divs + i * dsz));
        vkb_emu_depth_clip_pipeline(e, ci);
        /* Shader rewrites first: they read inline SPIR-V before it becomes a module. */
        vkb_emu_shader_xfb_fold(e, device, ci, mods, &nmods, 64);
        vkb_emu_shader_pipeline(e, device, ci, mods, &nmods, 64);
        /* Before compat flattens a non-solid polygon mode: what the line/point variant needs. */
        vkb_emu_polygon_pipeline(e, ci, (poly_info *)(polys + i * psz));
        vkb_emu_compat_pipeline(e, device, ci, mods, &nmods, 64);
        /* Last of the shader rewrites: it builds on what the others made of the vertex stage. */
        vkb_emu_shader_bgr10(e, device, ci, mods, &nmods, 64);
        vkb_emu_shader_uf11(e, device, ci, mods, &nmods, 64);
        if (e->flags & VKB_EMU_MAINT5) {
            VkPipelineCreateFlags2CreateInfo *f2 = vkb_emu_chain_take(ci, VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO);
            if (f2) ci->flags = (VkPipelineCreateFlags)f2->flags;
            for (uint32_t s = 0; s < ci->stageCount; s++)
                prep_stage(device, (VkPipelineShaderStageCreateInfo *)&ci->pStages[s], mods, &nmods, 64);
        }
    }
    if (vkb_emu_dumping())
        for (uint32_t i = 0; i < n; i++) {
            const VkGraphicsPipelineCreateInfo *ci = &infos[i];
            char line[640];
            int len = snprintf(line, sizeof(line), "graphics pipeline %u/%u: flags 0x%x, layout %llx, renderPass %llx, subpass %u, chain:", i, n,
                               (unsigned)ci->flags, (unsigned long long)(uintptr_t)ci->layout, (unsigned long long)(uintptr_t)ci->renderPass, ci->subpass);
            for (const VkBaseInStructure *b = ci->pNext; b && len < (int)sizeof(line) - 80; b = b->pNext)
                len += snprintf(line + len, sizeof(line) - (size_t)len, " %d", (int)b->sType);
            len += snprintf(line + len, sizeof(line) - (size_t)len, ", stages:");
            for (uint32_t s = 0; s < ci->stageCount && len < (int)sizeof(line) - 80; s++)
                len += snprintf(line + len, sizeof(line) - (size_t)len, " 0x%x=%llx", (unsigned)ci->pStages[s].stage,
                                (unsigned long long)(uintptr_t)ci->pStages[s].module);
            VKB_INFO("%s", line);
        }
    VkResult r = vkb_emu_real()->vkCreateGraphicsPipelines(device, cache, n, infos, pAllocator, pPipelines);
    /* Before the temporary modules go: a divisor variant is built from the same stages. */
    for (uint32_t i = 0; i < n; i++) {
        VkPipeline p = r >= 0 ? pPipelines[i] : VK_NULL_HANDLE;
        vkb_emu_divisor_created(e, device, cache, &infos[i], p, (div_info *)(divs + i * dsz));
        vkb_emu_polygon_created(e, device, cache, &infos[i], p, (poly_info *)(polys + i * psz));
    }
    for (uint32_t i = 0; i < nmods; i++) vkb_emu_real()->vkDestroyShaderModule(device, mods[i], NULL);
    vkb_emu_shader_call_end();
    free(divs);
    free(polys);
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL emu_CreateComputePipelines(VkDevice device, VkPipelineCache cache, uint32_t n,
                                                                const VkComputePipelineCreateInfo *infos,
                                                                const VkAllocationCallbacks *pAllocator, VkPipeline *pPipelines)
{
    vkb_emu_device *e = vkb_emu_cur();
    VkShaderModule mods[64];
    uint32_t nmods = 0;
    for (uint32_t i = 0; i < n; i++) {
        VkComputePipelineCreateInfo *ci = (VkComputePipelineCreateInfo *)&infos[i];
        if (e->flags & VKB_EMU_MAINT5) {
            VkPipelineCreateFlags2CreateInfo *f2 = vkb_emu_chain_take(ci, VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO);
            if (f2) ci->flags = (VkPipelineCreateFlags)f2->flags;
            prep_stage(device, &ci->stage, mods, &nmods, 64);
        }
    }
    VkResult r = vkb_emu_real()->vkCreateComputePipelines(device, cache, n, infos, pAllocator, pPipelines);
    for (uint32_t i = 0; i < nmods; i++) vkb_emu_real()->vkDestroyShaderModule(device, mods[i], NULL);
    vkb_emu_shader_call_end();
    return r;
}

/* ------------------------------------------------------------------ maintenance5 entry points */

static VKAPI_ATTR void VKAPI_CALL emu_CmdBindIndexBuffer2(VkCommandBuffer cb, VkBuffer buffer, VkDeviceSize offset, VkDeviceSize size,
                                                         VkIndexType type)
{
    (void)size;
    vkb_emu_real()->vkCmdBindIndexBuffer(cb, buffer, offset, type);
}

static VKAPI_ATTR void VKAPI_CALL emu_GetRenderingAreaGranularity(VkDevice device, const VkRenderingAreaInfo *info, VkExtent2D *g)
{
    (void)device;
    (void)info;
    g->width = 1;
    g->height = 1;
}

static VKAPI_ATTR void VKAPI_CALL emu_GetImageSubresourceLayout2(VkDevice device, VkImage image, const VkImageSubresource2 *sub,
                                                                VkSubresourceLayout2 *layout)
{
    /* A chained output (VkImageCompressionPropertiesEXT, VkSubresourceHostMemcpySize) needs the
     * driver's own 2-form, which a driver with VK_EXT_image_compression_control has as
     * vkGetImageSubresourceLayout2EXT (loaded under this name): the 1.0 form answered the layout
     * and left the chain untouched, so "is this image compressed" went unanswered on Mali. */
    const vkb_dispatch *real = vkb_emu_real();
    if (layout->pNext && real->vkGetImageSubresourceLayout2)
        real->vkGetImageSubresourceLayout2(device, image, sub, layout);
    else {
        VkImageSubresource tmp;
        real->vkGetImageSubresourceLayout(device, image, linear_mod_sub(image, &sub->imageSubresource, &tmp), &layout->subresourceLayout);
    }
    fix_single_row_pitch(image, &sub->imageSubresource, &layout->subresourceLayout);
}

static VKAPI_ATTR void VKAPI_CALL emu_GetDeviceImageSubresourceLayout(VkDevice device, const VkDeviceImageSubresourceInfo *info,
                                                                     VkSubresourceLayout2 *layout)
{
    VkImage img;
    memset(&layout->subresourceLayout, 0, sizeof(layout->subresourceLayout));
    if (vkb_emu_real()->vkCreateImage(device, info->pCreateInfo, NULL, &img) != VK_SUCCESS) return;
    vkb_emu_real()->vkGetImageSubresourceLayout(device, img, &info->pSubresource->imageSubresource, &layout->subresourceLayout);
    vkb_emu_real()->vkDestroyImage(device, img, NULL);
    if (!layout->subresourceLayout.rowPitch && info->pCreateInfo->extent.height == 1 && info->pCreateInfo->imageType != VK_IMAGE_TYPE_3D)
        layout->subresourceLayout.rowPitch = layout->subresourceLayout.size;
}

/* VK_REMAINING_ARRAY_LAYERS in a copy region, resolved against the image. */
static void fix_layers(vkb_emu_device *e, VkImage img, VkImageSubresourceLayers *l)
{
    if (l->layerCount != VK_REMAINING_ARRAY_LAYERS) return;
    vkb_emu_image *r = vkb_emu_find_image(e, img);
    l->layerCount = r && r->layers > l->baseArrayLayer ? r->layers - l->baseArrayLayer : 1;
}

static VKAPI_ATTR void VKAPI_CALL emu_CmdCopyImage(VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkImage dst, VkImageLayout dl,
                                                  uint32_t n, const VkImageCopy *regions)
{
    vkb_emu_device *e = vkb_emu_cur();
    for (uint32_t i = 0; i < n; i++) {
        fix_layers(e, src, (VkImageSubresourceLayers *)&regions[i].srcSubresource);
        fix_layers(e, dst, (VkImageSubresourceLayers *)&regions[i].dstSubresource);
    }
    if (!(n = vkb_texcap_image_copy(e, src, dst, (VkImageCopy *)regions, n))) return;
    if (vkb_emu_copy_image(e, cb, src, sl, dst, dl, n, regions)) return;
    vkb_emu_real()->vkCmdCopyImage(cb, src, sl, dst, dl, n, regions);
}

static VKAPI_ATTR void VKAPI_CALL emu_CmdCopyImage2(VkCommandBuffer cb, const VkCopyImageInfo2 *info)
{
    vkb_emu_device *e = vkb_emu_cur();
    for (uint32_t i = 0; i < info->regionCount; i++) {
        fix_layers(e, info->srcImage, (VkImageSubresourceLayers *)&info->pRegions[i].srcSubresource);
        fix_layers(e, info->dstImage, (VkImageSubresourceLayers *)&info->pRegions[i].dstSubresource);
    }
    VkCopyImageInfo2 *w = (VkCopyImageInfo2 *)info;
    if (!(w->regionCount = vkb_texcap_image_copy2(e, info->srcImage, info->dstImage, (VkImageCopy2 *)info->pRegions, info->regionCount))) return;
    if (vkb_emu_copy_image2(e, cb, info)) return;
    vkb_emu_real()->vkCmdCopyImage2(cb, info);
}

static VKAPI_ATTR void VKAPI_CALL emu_CmdBlitImage(VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkImage dst, VkImageLayout dl,
                                                  uint32_t n, const VkImageBlit *regions, VkFilter filter)
{
    vkb_emu_device *e = vkb_emu_cur();
    for (uint32_t i = 0; i < n; i++) {
        fix_layers(e, src, (VkImageSubresourceLayers *)&regions[i].srcSubresource);
        fix_layers(e, dst, (VkImageSubresourceLayers *)&regions[i].dstSubresource);
    }
    if (!(n = vkb_texcap_blit(e, src, dst, (VkImageBlit *)regions, n))) return;
    vkb_emu_real()->vkCmdBlitImage(cb, src, sl, dst, dl, n, regions, filter);
}

static VKAPI_ATTR void VKAPI_CALL emu_CmdBlitImage2(VkCommandBuffer cb, const VkBlitImageInfo2 *info)
{
    vkb_emu_device *e = vkb_emu_cur();
    for (uint32_t i = 0; i < info->regionCount; i++) {
        fix_layers(e, info->srcImage, (VkImageSubresourceLayers *)&info->pRegions[i].srcSubresource);
        fix_layers(e, info->dstImage, (VkImageSubresourceLayers *)&info->pRegions[i].dstSubresource);
    }
    VkBlitImageInfo2 *w = (VkBlitImageInfo2 *)info;
    if (!(w->regionCount = vkb_texcap_blit2(e, info->srcImage, info->dstImage, (VkImageBlit2 *)info->pRegions, info->regionCount))) return;
    vkb_emu_real()->vkCmdBlitImage2(cb, info);
}

static VKAPI_ATTR void VKAPI_CALL emu_CmdResolveImage(VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkImage dst, VkImageLayout dl,
                                                     uint32_t n, const VkImageResolve *regions)
{
    vkb_emu_device *e = vkb_emu_cur();
    for (uint32_t i = 0; i < n; i++) {
        fix_layers(e, src, (VkImageSubresourceLayers *)&regions[i].srcSubresource);
        fix_layers(e, dst, (VkImageSubresourceLayers *)&regions[i].dstSubresource);
    }
    if (!(n = vkb_texcap_resolve(e, dst, (VkImageResolve *)regions, n))) return;
    vkb_emu_real()->vkCmdResolveImage(cb, src, sl, dst, dl, n, regions);
}

static VKAPI_ATTR void VKAPI_CALL emu_CmdResolveImage2(VkCommandBuffer cb, const VkResolveImageInfo2 *info)
{
    vkb_emu_device *e = vkb_emu_cur();
    for (uint32_t i = 0; i < info->regionCount; i++) {
        fix_layers(e, info->srcImage, (VkImageSubresourceLayers *)&info->pRegions[i].srcSubresource);
        fix_layers(e, info->dstImage, (VkImageSubresourceLayers *)&info->pRegions[i].dstSubresource);
    }
    VkResolveImageInfo2 *w = (VkResolveImageInfo2 *)info;
    if (!(w->regionCount = vkb_texcap_resolve2(e, info->dstImage, (VkImageResolve2 *)info->pRegions, info->regionCount))) return;
    vkb_emu_real()->vkCmdResolveImage2(cb, info);
}

static VKAPI_ATTR void VKAPI_CALL emu_CmdCopyBufferToImage(VkCommandBuffer cb, VkBuffer src, VkImage dst, VkImageLayout dl, uint32_t n,
                                                          const VkBufferImageCopy *regions)
{
    vkb_emu_device *e = vkb_emu_cur();
    for (uint32_t i = 0; i < n; i++) fix_layers(e, dst, (VkImageSubresourceLayers *)&regions[i].imageSubresource);
    if (!(n = vkb_texcap_buffer_image(e, dst, 0, (VkBufferImageCopy *)regions, n))) return;
    if (vkb_emu_copy_buffer_to_image(e, cb, src, dst, dl, n, regions)) return;
    vkb_emu_real()->vkCmdCopyBufferToImage(cb, src, dst, dl, n, regions);
}

static VKAPI_ATTR void VKAPI_CALL emu_CmdCopyBufferToImage2(VkCommandBuffer cb, const VkCopyBufferToImageInfo2 *info)
{
    vkb_emu_device *e = vkb_emu_cur();
    for (uint32_t i = 0; i < info->regionCount; i++)
        fix_layers(e, info->dstImage, (VkImageSubresourceLayers *)&info->pRegions[i].imageSubresource);
    VkCopyBufferToImageInfo2 *w = (VkCopyBufferToImageInfo2 *)info;
    if (!(w->regionCount = vkb_texcap_buffer_image2(e, info->dstImage, 0, (VkBufferImageCopy2 *)info->pRegions, info->regionCount))) return;
    if (vkb_emu_copy_buffer_to_image2(e, cb, info)) return;
    vkb_emu_real()->vkCmdCopyBufferToImage2(cb, info);
}

static VKAPI_ATTR void VKAPI_CALL emu_CmdCopyImageToBuffer(VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkBuffer dst, uint32_t n,
                                                          const VkBufferImageCopy *regions)
{
    vkb_emu_device *e = vkb_emu_cur();
    for (uint32_t i = 0; i < n; i++) fix_layers(e, src, (VkImageSubresourceLayers *)&regions[i].imageSubresource);
    if (vkb_emu_bcn_blocks_readback(e, src)) return;
    if (!(n = vkb_texcap_buffer_image(e, src, 1, (VkBufferImageCopy *)regions, n))) return;
    vkb_emu_real()->vkCmdCopyImageToBuffer(cb, src, sl, dst, n, regions);
}

static VKAPI_ATTR void VKAPI_CALL emu_CmdCopyImageToBuffer2(VkCommandBuffer cb, const VkCopyImageToBufferInfo2 *info)
{
    vkb_emu_device *e = vkb_emu_cur();
    for (uint32_t i = 0; i < info->regionCount; i++)
        fix_layers(e, info->srcImage, (VkImageSubresourceLayers *)&info->pRegions[i].imageSubresource);
    if (vkb_emu_bcn_blocks_readback(e, info->srcImage)) return;
    VkCopyImageToBufferInfo2 *w = (VkCopyImageToBufferInfo2 *)info;
    if (!(w->regionCount = vkb_texcap_buffer_image2(e, info->srcImage, 1, (VkBufferImageCopy2 *)info->pRegions, info->regionCount))) return;
    vkb_emu_real()->vkCmdCopyImageToBuffer2(cb, info);
}

/* VK_WHOLE_SIZE in vkCmdBindVertexBuffers2's pSizes (maintenance5) resolved in place from the buffer's
 * record, for a driver without maintenance5. Every wrapper of the client's vkCmdBindVertexBuffers2
 * calls it (the divisor emulation's replaces this file's in the table). */
void vkb_emu_vb_sizes_resolve(vkb_emu_device *e, uint32_t n, const VkBuffer *buffers, const VkDeviceSize *offsets, const VkDeviceSize *sizes)
{
    if (!sizes || !(e->flags & VKB_EMU_MAINT5)) return;
    VkDeviceSize *s = (VkDeviceSize *)sizes;
    for (uint32_t i = 0; i < n; i++) {
        if (s[i] != VK_WHOLE_SIZE || !buffers[i]) continue;
        VkDeviceSize total = vkb_emu_buffer_size(e, buffers[i]);
        s[i] = total > offsets[i] ? total - offsets[i] : 0;
    }
}

static VKAPI_ATTR void VKAPI_CALL emu_CmdBindVertexBuffers2(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkBuffer *buffers,
                                                           const VkDeviceSize *offsets, const VkDeviceSize *sizes,
                                                           const VkDeviceSize *strides)
{
    vkb_emu_vb_sizes_resolve(vkb_emu_cur(), n, buffers, offsets, sizes);
    vkb_emu_real()->vkCmdBindVertexBuffers2(cb, first, n, buffers, offsets, sizes, strides);
}

/* ------------------------------------------------------------------ install */

void vkb_emu_install(vkb_srv_table *dev)
{
    vkb_emu_device *e = dev->emu;
    vkb_dispatch *dt = &dev->dt;
    /* Resource records are needed by every emulation. */
    dt->vkCreateBuffer = emu_CreateBuffer;
    dt->vkDestroyBuffer = emu_DestroyBuffer;
    dt->vkCreateImage = emu_CreateImage;
    if (dev->real.vkGetDeviceImageMemoryRequirements) dt->vkGetDeviceImageMemoryRequirements = emu_GetDeviceImageMemoryRequirements;
    if (dev->real.vkGetDeviceImageSparseMemoryRequirements)
        dt->vkGetDeviceImageSparseMemoryRequirements = emu_GetDeviceImageSparseMemoryRequirements;
    dt->vkDestroyImage = emu_DestroyImage;
    dt->vkGetImageSubresourceLayout = emu_GetImageSubresourceLayout;
    if (e->flags & VKB_EMU_DMABUF_EXPORT) dt->vkGetImageDrmFormatModifierPropertiesEXT = emu_GetImageDrmFormatModifierPropertiesEXT;
    /* AHardwareBuffer-typed images: requirements and layouts from a twin (above) */
    dt->vkGetImageMemoryRequirements = emu_GetImageMemoryRequirements;
    if (dev->real.vkGetImageMemoryRequirements2) dt->vkGetImageMemoryRequirements2 = emu_GetImageMemoryRequirements2;
    dt->vkBindImageMemory = emu_BindImageMemory;
    if (dev->real.vkBindImageMemory2) dt->vkBindImageMemory2 = emu_BindImageMemory2;
    dt->vkCreateGraphicsPipelines = emu_CreateGraphicsPipelines;
    dt->vkCreateComputePipelines = emu_CreateComputePipelines;
    dt->vkCmdCopyImage = emu_CmdCopyImage;
    dt->vkCmdCopyBufferToImage = emu_CmdCopyBufferToImage;
    if (dev->real.vkCmdCopyImage2) dt->vkCmdCopyImage2 = emu_CmdCopyImage2;
    if (dev->real.vkCmdCopyBufferToImage2) dt->vkCmdCopyBufferToImage2 = emu_CmdCopyBufferToImage2;
    dt->vkCmdCopyImageToBuffer = emu_CmdCopyImageToBuffer;
    if (dev->real.vkCmdCopyImageToBuffer2) dt->vkCmdCopyImageToBuffer2 = emu_CmdCopyImageToBuffer2;
    if (e->flags & (VKB_EMU_MAINT5 | VKB_EMU_TEXEL_ALIGN)) dt->vkCreateBufferView = emu_CreateBufferView;
    if (e->flags & VKB_EMU_MAINT5) {
        dt->vkCmdBindIndexBuffer2 = emu_CmdBindIndexBuffer2;
        dt->vkGetRenderingAreaGranularity = emu_GetRenderingAreaGranularity;
        dt->vkGetImageSubresourceLayout2 = emu_GetImageSubresourceLayout2;
        dt->vkGetDeviceImageSubresourceLayout = emu_GetDeviceImageSubresourceLayout;
        dt->vkCmdBlitImage = emu_CmdBlitImage;
        dt->vkCmdResolveImage = emu_CmdResolveImage;
        if (dev->real.vkCmdBlitImage2) dt->vkCmdBlitImage2 = emu_CmdBlitImage2;
        if (dev->real.vkCmdResolveImage2) dt->vkCmdResolveImage2 = emu_CmdResolveImage2;
        if (dev->real.vkCmdBindVertexBuffers2) dt->vkCmdBindVertexBuffers2 = emu_CmdBindVertexBuffers2;
    }
    vkb_emu_install_features(dev);
    vkb_emu_depth_clip_install(dev);
    vkb_emu_compat_install(dev);
    vkb_emu_polygon_install(dev); /* over compat: its split indirect draws come through it */
    vkb_emu_desc_install(dev); /* beneath push descriptors: their layouts and templates come through it */
    vkb_emu_push_install(dev);
    vkb_emu_dynrend_install(dev); /* over the pipeline and command buffer wrappers: they see render passes */
    vkb_texcap_install(dev); /* over the BC views and the shader store: it renumbers levels before them */
    if (e->texcap) {
        /* blits from a capped texture and resolves into one (not only under maintenance5) */
        dt->vkCmdBlitImage = emu_CmdBlitImage;
        dt->vkCmdResolveImage = emu_CmdResolveImage;
        if (dev->real.vkCmdBlitImage2) dt->vkCmdBlitImage2 = emu_CmdBlitImage2;
        if (dev->real.vkCmdResolveImage2) dt->vkCmdResolveImage2 = emu_CmdResolveImage2;
    }
    VKB_INFO("device %u: emulation 0x%x installed", dev->id, e->flags);
}

void vkb_emu_uninstall(vkb_srv_table *dev)
{
    vkb_emu_device *e = dev->emu;
    vkb_texcap_uninstall(dev);
    vkb_emu_push_uninstall(dev);
    vkb_emu_desc_uninstall(dev);
    vkb_emu_polygon_uninstall(dev);
    vkb_emu_compat_uninstall(dev);
    vkb_emu_uninstall_features(dev);
    for (int i = 0; i < VKB_EMU_BUCKETS; i++) {
        while (e->images[i]) {
            vkb_emu_image *r = e->images[i];
            e->images[i] = r->next;
            image_unaccount(r);
            vkb_texcap_destroyed(r);
            vkb_emu_image_release(e, r);
            free(r);
        }
        while (e->buffers[i]) {
            vkb_emu_buffer *r = e->buffers[i];
            e->buffers[i] = r->next;
            vkb_memstat_buffer(-(int64_t)r->size);
            free(r);
        }
    }
    vkb_emu_dynrend_uninstall(dev); /* after the command buffers' records, which hold its framebuffers */
}

/* ------------------------------------------------------------------ depth clip enable */

/* VK_EXT_depth_clip_enable on a GPU without it. Without the extension Vulkan clips exactly when
 * depth clamp is off, so a pipeline's clamp becomes "not clip" (D3D's DepthClipEnable = FALSE is
 * clamping; clip + clamp only differs for fragments the clip removes anyway). The client hides
 * dynamic depth clip/clamp while this is emulated, so both are static pipeline state here. */
void vkb_emu_depth_clip_pipeline(vkb_emu_device *e, VkGraphicsPipelineCreateInfo *ci)
{
    if (!(e->flags & VKB_EMU_DEPTH_CLIP)) return;
    VkPipelineRasterizationStateCreateInfo *rs = (VkPipelineRasterizationStateCreateInfo *)ci->pRasterizationState;
    if (!rs) return;
    VkPipelineRasterizationDepthClipStateCreateInfoEXT *dc =
        vkb_emu_chain_take(rs, VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_DEPTH_CLIP_STATE_CREATE_INFO_EXT);
    if (dc) rs->depthClampEnable = dc->depthClipEnable ? VK_FALSE : VK_TRUE;
}

void vkb_emu_depth_clip_install(vkb_srv_table *dev)
{
    (void)dev;
}
