/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * Surfaces and swapchains (backend-independent part).
 */
#define _GNU_SOURCE
#include "wsi.h"
#include "gen/vkb_client_gen.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

const VkSurfaceFormatKHR vkb_wsi_formats[] = {
    {VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
    {VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
    {VK_FORMAT_R8G8B8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
    {VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
};
const uint32_t vkb_wsi_format_count = sizeof(vkb_wsi_formats) / sizeof(vkb_wsi_formats[0]);

#define FOURCC(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))

uint32_t vkb_drm_format(VkFormat f)
{
    switch (f) {
    case VK_FORMAT_B8G8R8A8_SRGB:
    case VK_FORMAT_B8G8R8A8_UNORM: return FOURCC('A', 'R', '2', '4');
    case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_R8G8B8A8_UNORM: return FOURCC('A', 'B', '2', '4');
    default: return 0;
    }
}

static vkb_surface *surf(VkSurfaceKHR s) { return (vkb_surface *)(uintptr_t)s; }
static vkb_swapchain *chain(VkSwapchainKHR s) { return (vkb_swapchain *)(uintptr_t)s; }

/* ------------------------------------------------------------------ surfaces */

static VkResult new_surface(enum vkb_surface_kind kind, VkSurfaceKHR *out, vkb_surface **ps)
{
    vkb_surface *s = calloc(1, sizeof(*s));
    if (!s) return VK_ERROR_OUT_OF_HOST_MEMORY;
    s->kind = kind;
    *ps = s;
    *out = (VkSurfaceKHR)(uintptr_t)s;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkb_ep_vkCreateWaylandSurfaceKHR(VkInstance instance, const VkWaylandSurfaceCreateInfoKHR *pCreateInfo,
                                                               const VkAllocationCallbacks *pAllocator, VkSurfaceKHR *pSurface)
{
    (void)instance;
    (void)pAllocator;
    if (!vkb_wl_load()) return VK_ERROR_INITIALIZATION_FAILED;
    vkb_surface *s;
    VkResult r = new_surface(VKB_SURF_WAYLAND, pSurface, &s);
    if (r == VK_SUCCESS) {
        s->wl_display = pCreateInfo->display;
        s->wl_surface = pCreateInfo->surface;
    }
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL vkb_ep_vkCreateXcbSurfaceKHR(VkInstance instance, const VkXcbSurfaceCreateInfoKHR *pCreateInfo,
                                                           const VkAllocationCallbacks *pAllocator, VkSurfaceKHR *pSurface)
{
    (void)instance;
    (void)pAllocator;
    if (!vkb_xcb_load()) return VK_ERROR_INITIALIZATION_FAILED;
    vkb_surface *s;
    VkResult r = new_surface(VKB_SURF_XCB, pSurface, &s);
    if (r == VK_SUCCESS) {
        s->xcb_conn = pCreateInfo->connection;
        s->xcb_window = pCreateInfo->window;
    }
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL vkb_ep_vkCreateXlibSurfaceKHR(VkInstance instance, const VkXlibSurfaceCreateInfoKHR *pCreateInfo,
                                                            const VkAllocationCallbacks *pAllocator, VkSurfaceKHR *pSurface)
{
    (void)instance;
    (void)pAllocator;
    if (!vkb_xcb_load()) return VK_ERROR_INITIALIZATION_FAILED;
    void *conn = vkb_xlib_to_xcb(pCreateInfo->dpy);
    if (!conn) return VK_ERROR_INITIALIZATION_FAILED;
    vkb_surface *s;
    VkResult r = new_surface(VKB_SURF_XCB, pSurface, &s);
    if (r == VK_SUCCESS) {
        s->xcb_conn = conn;
        s->xcb_window = (uint32_t)pCreateInfo->window;
    }
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL vkb_ep_vkCreateHeadlessSurfaceEXT(VkInstance instance, const VkHeadlessSurfaceCreateInfoEXT *pCreateInfo,
                                                                const VkAllocationCallbacks *pAllocator, VkSurfaceKHR *pSurface)
{
    (void)instance;
    (void)pCreateInfo;
    (void)pAllocator;
    vkb_surface *s;
    return new_surface(VKB_SURF_HEADLESS, pSurface, &s);
}

VKAPI_ATTR void VKAPI_CALL vkb_ep_vkDestroySurfaceKHR(VkInstance instance, VkSurfaceKHR surface, const VkAllocationCallbacks *pAllocator)
{
    (void)instance;
    (void)pAllocator;
    free(surf(surface));
}

VKAPI_ATTR VkBool32 VKAPI_CALL vkb_ep_vkGetPhysicalDeviceWaylandPresentationSupportKHR(VkPhysicalDevice physicalDevice, uint32_t queueFamilyIndex,
                                                                                      struct wl_display *display)
{
    (void)physicalDevice;
    (void)queueFamilyIndex;
    (void)display;
    return vkb_wl_load() ? VK_TRUE : VK_FALSE;
}

VKAPI_ATTR VkBool32 VKAPI_CALL vkb_ep_vkGetPhysicalDeviceXcbPresentationSupportKHR(VkPhysicalDevice physicalDevice, uint32_t queueFamilyIndex,
                                                                                  xcb_connection_t *connection, xcb_visualid_t visual_id)
{
    (void)physicalDevice;
    (void)queueFamilyIndex;
    (void)connection;
    (void)visual_id;
    return vkb_xcb_load() ? VK_TRUE : VK_FALSE;
}

VKAPI_ATTR VkBool32 VKAPI_CALL vkb_ep_vkGetPhysicalDeviceXlibPresentationSupportKHR(VkPhysicalDevice physicalDevice, uint32_t queueFamilyIndex,
                                                                                   Display *dpy, VisualID visualID)
{
    (void)physicalDevice;
    (void)queueFamilyIndex;
    (void)dpy;
    (void)visualID;
    return vkb_xcb_load() ? VK_TRUE : VK_FALSE;
}

VKAPI_ATTR VkResult VKAPI_CALL vkb_ep_vkGetPhysicalDeviceSurfaceSupportKHR(VkPhysicalDevice physicalDevice, uint32_t queueFamilyIndex,
                                                                          VkSurfaceKHR surface, VkBool32 *pSupported)
{
    (void)surface;
    /* Presentation is a copy or a hand-over after the queue's work: any queue that can do
     * graphics or compute (or transfer) can present. */
    uint32_t n = 0;
    vkb_wire_vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &n, NULL);
    VkQueueFamilyProperties *qp = calloc(n ? n : 1, sizeof(*qp));
    vkb_wire_vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &n, qp);
    *pSupported = queueFamilyIndex < n &&
                  (qp[queueFamilyIndex].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT));
    free(qp);
    return VK_SUCCESS;
}

static VkResult surface_extent(vkb_surface *s, VkExtent2D *e)
{
    switch (s->kind) {
    case VKB_SURF_WAYLAND: return vkb_wl_surface_extent(s, e);
    case VKB_SURF_XCB: return vkb_xcb_surface_extent(s, e);
    default:
        e->width = e->height = 0xFFFFFFFFu;
        return VK_SUCCESS;
    }
}

static VkImageUsageFlags supported_usage(VkPhysicalDevice pd)
{
    VkImageUsageFlags u = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                          VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
    VkFormatProperties fp;
    vkb_wire_vkGetPhysicalDeviceFormatProperties(pd, VK_FORMAT_B8G8R8A8_UNORM, &fp);
    if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) u |= VK_IMAGE_USAGE_STORAGE_BIT;
    return u;
}

VKAPI_ATTR VkResult VKAPI_CALL vkb_ep_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface,
                                                                               VkSurfaceCapabilitiesKHR *c)
{
    vkb_surface *s = surf(surface);
    memset(c, 0, sizeof(*c));
    VkResult r = surface_extent(s, &c->currentExtent);
    if (r != VK_SUCCESS) return r;
    c->minImageCount = s->kind == VKB_SURF_WAYLAND ? 3 : 2;
    c->maxImageCount = 8;
    c->minImageExtent = (VkExtent2D){1, 1};
    c->maxImageExtent = (VkExtent2D){16384, 16384};
    if (c->currentExtent.width != 0xFFFFFFFFu) {
        c->minImageExtent = c->maxImageExtent = c->currentExtent;
        if (!c->currentExtent.width || !c->currentExtent.height) c->minImageExtent = (VkExtent2D){0, 0};
    }
    c->maxImageArrayLayers = 1;
    c->supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    c->currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    c->supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR | VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    if (s->kind == VKB_SURF_WAYLAND) c->supportedCompositeAlpha |= VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR;
    c->supportedUsageFlags = supported_usage(physicalDevice);
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkb_ep_vkGetPhysicalDeviceSurfaceCapabilities2KHR(VkPhysicalDevice physicalDevice,
                                                                                const VkPhysicalDeviceSurfaceInfo2KHR *pInfo,
                                                                                VkSurfaceCapabilities2KHR *pCaps)
{
    VkResult r = vkb_ep_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice, pInfo->surface, &pCaps->surfaceCapabilities);
    VkBool32 pw = vkb_wsi_present_wait_on(vkb_pd(physicalDevice)) ? VK_TRUE : VK_FALSE;
    for (VkBaseOutStructure *b = (VkBaseOutStructure *)pCaps->pNext; b; b = b->pNext) {
        if (b->sType == VK_STRUCTURE_TYPE_SURFACE_PROTECTED_CAPABILITIES_KHR)
            ((VkSurfaceProtectedCapabilitiesKHR *)b)->supportsProtected = VK_FALSE;
        /* every surface kind: what "shown" means on each is in the present wait section below */
        else if (b->sType == VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_PRESENT_ID_2_KHR)
            ((VkSurfaceCapabilitiesPresentId2KHR *)b)->presentId2Supported = pw;
        else if (b->sType == VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_PRESENT_WAIT_2_KHR)
            ((VkSurfaceCapabilitiesPresentWait2KHR *)b)->presentWait2Supported = pw;
    }
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL vkb_ep_vkGetPhysicalDeviceSurfaceFormatsKHR(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface,
                                                                          uint32_t *pCount, VkSurfaceFormatKHR *pFormats)
{
    (void)physicalDevice;
    (void)surface;
    if (!pFormats) {
        *pCount = vkb_wsi_format_count;
        return VK_SUCCESS;
    }
    uint32_t n = *pCount < vkb_wsi_format_count ? *pCount : vkb_wsi_format_count;
    memcpy(pFormats, vkb_wsi_formats, n * sizeof(*pFormats));
    *pCount = n;
    return n < vkb_wsi_format_count ? VK_INCOMPLETE : VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkb_ep_vkGetPhysicalDeviceSurfaceFormats2KHR(VkPhysicalDevice physicalDevice,
                                                                           const VkPhysicalDeviceSurfaceInfo2KHR *pInfo, uint32_t *pCount,
                                                                           VkSurfaceFormat2KHR *pFormats)
{
    (void)physicalDevice;
    (void)pInfo;
    if (!pFormats) {
        *pCount = vkb_wsi_format_count;
        return VK_SUCCESS;
    }
    uint32_t n = *pCount < vkb_wsi_format_count ? *pCount : vkb_wsi_format_count;
    for (uint32_t i = 0; i < n; i++) pFormats[i].surfaceFormat = vkb_wsi_formats[i];
    *pCount = n;
    return n < vkb_wsi_format_count ? VK_INCOMPLETE : VK_SUCCESS;
}

static const VkPresentModeKHR present_modes[] = {VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR};

VKAPI_ATTR VkResult VKAPI_CALL vkb_ep_vkGetPhysicalDeviceSurfacePresentModesKHR(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface,
                                                                               uint32_t *pCount, VkPresentModeKHR *pModes)
{
    (void)physicalDevice;
    (void)surface;
    uint32_t total = 3;
    if (!pModes) {
        *pCount = total;
        return VK_SUCCESS;
    }
    uint32_t n = *pCount < total ? *pCount : total;
    memcpy(pModes, present_modes, n * sizeof(*pModes));
    *pCount = n;
    return n < total ? VK_INCOMPLETE : VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkb_ep_vkGetPhysicalDevicePresentRectanglesKHR(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface,
                                                                             uint32_t *pRectCount, VkRect2D *pRects)
{
    (void)physicalDevice;
    if (!pRects) {
        *pRectCount = 1;
        return VK_SUCCESS;
    }
    if (*pRectCount < 1) return VK_INCOMPLETE;
    VkExtent2D e;
    surface_extent(surf(surface), &e);
    if (e.width == 0xFFFFFFFFu) e = (VkExtent2D){16384, 16384};
    pRects[0] = (VkRect2D){{0, 0}, e};
    *pRectCount = 1;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkb_ep_vkGetDeviceGroupPresentCapabilitiesKHR(VkDevice device, VkDeviceGroupPresentCapabilitiesKHR *pCaps)
{
    (void)device;
    memset(pCaps->presentMask, 0, sizeof(pCaps->presentMask));
    pCaps->presentMask[0] = 1;
    pCaps->modes = VK_DEVICE_GROUP_PRESENT_MODE_LOCAL_BIT_KHR;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkb_ep_vkGetDeviceGroupSurfacePresentModesKHR(VkDevice device, VkSurfaceKHR surface,
                                                                            VkDeviceGroupPresentModeFlagsKHR *pModes)
{
    (void)device;
    (void)surface;
    *pModes = VK_DEVICE_GROUP_PRESENT_MODE_LOCAL_BIT_KHR;
    return VK_SUCCESS;
}

/* ------------------------------------------------------------------ swapchain images */

static uint32_t pick_type(vkb_device *dev, uint32_t bits, VkMemoryPropertyFlags want)
{
    const VkPhysicalDeviceMemoryProperties *mp = &dev->pd->client_mem;
    for (uint32_t i = 0; i < mp->memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp->memoryTypes[i].propertyFlags & want) == want) return i;
    for (uint32_t i = 0; i < mp->memoryTypeCount; i++)
        if (bits & (1u << i)) return i;
    return UINT32_MAX;
}

static int pd_has_ext(vkb_physdev *pd, const char *name)
{
    for (uint32_t i = 0; i < pd->next; i++)
        if (!strcmp(pd->exts[i].extensionName, name)) return 1;
    return 0;
}

/* Gralloc swapchains (sc->ahb): on unless VKBRIDGE_WSI_AHB=0; off for good once the server says it
 * cannot allocate AHardwareBuffers at all (a desktop server). */
static int ahb_off = -1;

int vkb_wsi_ahb_disabled(void)
{
    if (ahb_off < 0) {
        const char *e = getenv("VKBRIDGE_WSI_AHB");
        ahb_off = e && !strcmp(e, "0");
    }
    return ahb_off;
}

/* The pages of a gralloc swapchain image (vkbAllocScanout): the server allocates the
 * AHardwareBuffer, writes its handle into one end of a socketpair (img->ahb_sock keeps the other,
 * for banner_ahb_v1.attach) and hands back its dma-buf (img->dmabuf_fd) and row pitch. */
static VkResult alloc_scanout(vkb_swapchain *sc, vkb_swap_image *img, uint32_t *pitch, uint64_t *size)
{
    vkb_device *dev = sc->dev;
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv) != 0) return VK_ERROR_INITIALIZATION_FAILED;
    vkb_call c;
    vkb_call_begin(&c, VKB_CMD_vkbAllocScanout, dev->obj.table);
    vkb_enc_u64(&c.e, dev->obj.remote);
    vkb_enc_u32(&c.e, sc->info.imageExtent.width);
    vkb_enc_u32(&c.e, sc->info.imageExtent.height);
    vkb_enc_u32(&c.e, (uint32_t)sc->info.imageFormat);
    vkb_enc_fd(&c.e, sv[1]);
    int ok = vkb_call_exec(&c);
    close(sv[1]);
    if (!ok) {
        close(sv[0]);
        return VK_ERROR_DEVICE_LOST;
    }
    VkResult r = (VkResult)vkb_dec_u32(&c.d);
    int fd = vkb_dec_fd(&c.d);
    *pitch = vkb_dec_u32(&c.d);
    *size = vkb_dec_u64(&c.d);
    (void)vkb_dec_u32(&c.d); /* the AHARDWAREBUFFER_FORMAT_* (the compositor reads it from the buffer) */
    vkb_call_end(&c);
    if (r != VK_SUCCESS || fd < 0) {
        if (fd >= 0) close(fd);
        close(sv[0]);
        if (r == VK_ERROR_FEATURE_NOT_PRESENT) {
            ahb_off = 1;
            VKB_INFO("Wayland: the server cannot allocate AHardwareBuffers; swapchains stay ordinary dma-bufs");
        }
        return r != VK_SUCCESS ? r : VK_ERROR_INITIALIZATION_FAILED;
    }
    img->dmabuf_fd = fd;
    img->ahb_sock = sv[0];
    return VK_SUCCESS;
}

/* One swapchain image: exported as a linear dma-buf when `dmabuf`, else an ordinary image. With
 * sc->ahb the dma-buf is a gralloc buffer's (alloc_scanout), imported with the buffer's own pitch. */
static VkResult create_image(vkb_swapchain *sc, vkb_swap_image *img, int dmabuf)
{
    vkb_device *dev = sc->dev;
    VkDevice device = (VkDevice)dev;
    const VkSwapchainCreateInfoKHR *ci = &sc->info;
    const int ahb = dmabuf && sc->ahb;
    uint32_t ahb_pitch = 0;
    uint64_t ahb_size = 0;
    img->dmabuf_fd = img->ahb_sock = -1;
    if (ahb) {
        VkResult r = alloc_scanout(sc, img, &ahb_pitch, &ahb_size);
        if (r != VK_SUCCESS) return r;
    }
    VkImageCreateInfo ici = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = ci->imageFormat;
    ici.extent = (VkExtent3D){ci->imageExtent.width, ci->imageExtent.height, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = ci->imageArrayLayers;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = ci->imageUsage | (sc->copy_present || !dmabuf ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0);
    ici.sharingMode = ci->imageSharingMode;
    ici.queueFamilyIndexCount = ci->queueFamilyIndexCount;
    ici.pQueueFamilyIndices = ci->pQueueFamilyIndices;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (ci->flags & VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR) ici.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    VkImageFormatListCreateInfo fl = {VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO, NULL, sc->view_format_count, sc->view_formats};
    VkExternalMemoryImageCreateInfo ext = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, NULL,
                                           VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    uint64_t linear = 0; /* DRM_FORMAT_MOD_LINEAR */
    VkImageDrmFormatModifierListCreateInfoEXT mods = {VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT, NULL, 1, &linear};
    VkSubresourceLayout plane = {0, 0, ahb_pitch, 0, 0};
    VkImageDrmFormatModifierExplicitCreateInfoEXT expl = {VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT, NULL, 0, 1,
                                                              &plane};
    const void *chain = NULL;
    if (sc->view_format_count) {
        fl.pNext = chain;
        chain = &fl;
    }
    int use_mods = 0;
    if (dmabuf) {
        ext.pNext = chain;
        chain = &ext;
        if (pd_has_ext(dev->pd, "VK_EXT_image_drm_format_modifier") && ahb) {
            expl.pNext = chain;
            chain = &expl;
            ici.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
            use_mods = 1;
        } else if (pd_has_ext(dev->pd, "VK_EXT_image_drm_format_modifier")) {
            mods.pNext = chain;
            chain = &mods;
            ici.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
            use_mods = 1;
        } else {
            ici.tiling = VK_IMAGE_TILING_LINEAR;
        }
    }
    ici.pNext = chain;
    VkResult r = vkb_wire_vkCreateImage(device, &ici, NULL, &img->image);
    if (r != VK_SUCCESS) return r;
    VkMemoryRequirements req;
    vkb_ep_vkGetImageMemoryRequirements(device, img->image, &req);
    uint32_t type = pick_type(dev, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == UINT32_MAX) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    VkMemoryDedicatedAllocateInfo ded = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, NULL, img->image, VK_NULL_HANDLE};
    VkExportMemoryAllocateInfo exp = {VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, &ded, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, dmabuf ? (const void *)&exp : (const void *)&ded, req.size, type};
    /* The gralloc buffer's dma-buf, imported whole; no dedicated struct (some Mali drivers reject
     * or crash on one with an imported dma-buf). */
    VkImportMemoryFdInfoKHR imp = {VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR, NULL, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, -1};
    if (ahb) {
        VkMemoryFdPropertiesKHR fp = {VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
        if (vkb_ep_vkGetMemoryFdPropertiesKHR(device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, img->dmabuf_fd, &fp) == VK_SUCCESS &&
            (fp.memoryTypeBits & req.memoryTypeBits))
            type = pick_type(dev, fp.memoryTypeBits & req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (req.size > ahb_size) {
            VKB_WARN("Wayland: the image needs %llu bytes, the gralloc buffer has %llu", (unsigned long long)req.size,
                     (unsigned long long)ahb_size);
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
        imp.fd = fcntl(img->dmabuf_fd, F_DUPFD_CLOEXEC, 0);
        if (imp.fd < 0) return VK_ERROR_TOO_MANY_OBJECTS;
        ai.pNext = &imp;
        ai.allocationSize = ahb_size;
        ai.memoryTypeIndex = type;
    }
    r = vkb_ep_vkAllocateMemory(device, &ai, NULL, &img->memory);
    if (r != VK_SUCCESS) {
        if (imp.fd >= 0) close(imp.fd); /* a refused import leaves the fd with us */
        return r;
    }
    r = vkb_wire_vkBindImageMemory(device, img->image, img->memory, 0);
    if (r != VK_SUCCESS) return r;
    if (ahb) {
        VkImageSubresource sub = {use_mods ? VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT : VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
        VkSubresourceLayout lay;
        vkb_wire_vkGetImageSubresourceLayout(device, img->image, &sub, &lay);
        if (lay.rowPitch != ahb_pitch || lay.offset != 0) {
            /* Without the modifier extension there is no explicit layout: the driver's own linear
             * pitch has to be gralloc's. */
            VKB_WARN("Wayland: the image's layout (pitch %llu, offset %llu) is not the gralloc buffer's (pitch %u)",
                     (unsigned long long)lay.rowPitch, (unsigned long long)lay.offset, ahb_pitch);
            return VK_ERROR_FORMAT_NOT_SUPPORTED;
        }
        img->stride = ahb_pitch;
        img->offset = 0;
        img->modifier = 0; /* DRM_FORMAT_MOD_LINEAR */
        return VK_SUCCESS;
    }
    if (dmabuf) {
        VkMemoryGetFdInfoKHR gi = {VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR, NULL, img->memory, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
        r = vkb_wire_vkGetMemoryFdKHR(device, &gi, &img->dmabuf_fd);
        if (r != VK_SUCCESS || img->dmabuf_fd < 0) return r != VK_SUCCESS ? r : VK_ERROR_INITIALIZATION_FAILED;
        VkImageSubresource sub = {use_mods ? VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT : VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
        VkSubresourceLayout lay;
        vkb_wire_vkGetImageSubresourceLayout(device, img->image, &sub, &lay);
        img->stride = (uint32_t)lay.rowPitch;
        img->offset = (uint32_t)lay.offset;
        img->modifier = 0;
        if (use_mods) {
            VkImageDrmFormatModifierPropertiesEXT mp = {VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT};
            if (vkb_wire_vkGetImageDrmFormatModifierPropertiesEXT(device, img->image, &mp) == VK_SUCCESS) img->modifier = mp.drmFormatModifier;
        }
    }
    return VK_SUCCESS;
}

static void destroy_image(vkb_swapchain *sc, vkb_swap_image *img)
{
    VkDevice device = (VkDevice)sc->dev;
    if (img->dmabuf_fd >= 0) close(img->dmabuf_fd);
    if (img->ahb_sock >= 0) close(img->ahb_sock);
    if (img->image) vkb_ep_vkDestroyImage(device, img->image, NULL);
    if (img->memory) vkb_ep_vkFreeMemory(device, img->memory, NULL);
}

static VkResult create_staging(vkb_swapchain *sc)
{
    vkb_device *dev = sc->dev;
    VkDevice device = (VkDevice)dev;
    VkDeviceSize size = (VkDeviceSize)sc->info.imageExtent.width * sc->info.imageExtent.height * 4;
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                              VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
    VkResult r = vkb_ep_vkCreateBuffer(device, &bci, NULL, &sc->staging);
    if (r != VK_SUCCESS) return r;
    VkMemoryRequirements req;
    vkb_ep_vkGetBufferMemoryRequirements(device, sc->staging, &req);
    /* Cached readback where the server offers it: the CPU reads each frame from cached pages
     * (Venus on the Mali-G715: 7-10 ms per frame from uncached memory, ~1.3 ms cached). */
    sc->staging_cached = 0;
    if (vkb_memory_alloc_readback(device, sc->staging, req.size, &sc->staging_mem) == VK_SUCCESS) {
        sc->staging_cached = 1;
        VKB_INFO("swapchain: frames read back through cached memory (server type %d)", dev->pd->readback_type);
        goto bind;
    }
    uint32_t type = pick_type(dev, req.memoryTypeBits,
                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    if (type == UINT32_MAX || !(dev->pd->client_mem.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
        type = pick_type(dev, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    if (type == UINT32_MAX || !(dev->pd->client_mem.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
        VKB_ERR("swapchain: no host-visible memory to read frames back into");
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size, type};
    r = vkb_ep_vkAllocateMemory(device, &ai, NULL, &sc->staging_mem);
    if (r != VK_SUCCESS) return r;
bind:
    r = vkb_wire_vkBindBufferMemory(device, sc->staging, sc->staging_mem, 0);
    if (r != VK_SUCCESS) return r;
    void *p = NULL;
    r = vkb_ep_vkMapMemory(device, sc->staging_mem, 0, VK_WHOLE_SIZE, 0, &p);
    sc->staging_ptr = p;
    sc->staging_size = size;
    return r;
}

static VkResult ensure_copy_cbs(vkb_swapchain *sc, uint32_t family)
{
    VkDevice device = (VkDevice)sc->dev;
    if (sc->pool && sc->pool_family == family) return VK_SUCCESS;
    if (sc->pool) {
        vkb_ep_vkDestroyCommandPool(device, sc->pool, NULL);
        sc->pool = VK_NULL_HANDLE;
    }
    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, 0, family};
    VkResult r = vkb_wire_vkCreateCommandPool(device, &pci, NULL, &sc->pool);
    if (r != VK_SUCCESS) return r;
    sc->pool_family = family;
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, sc->pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, sc->count};
    r = vkb_ep_vkAllocateCommandBuffers(device, &cai, sc->copy_cbs);
    if (r != VK_SUCCESS) return r;
    for (uint32_t i = 0; i < sc->count; i++) {
        VkCommandBuffer cb = sc->copy_cbs[i];
        VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, 0, NULL};
        vkb_ep_vkBeginCommandBuffer(cb, &bi);
        VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                                  VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_QUEUE_FAMILY_IGNORED,
                                  VK_QUEUE_FAMILY_IGNORED, sc->images[i].image, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        vkb_wire_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
        VkBufferImageCopy region = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0},
                                    {sc->info.imageExtent.width, sc->info.imageExtent.height, 1}};
        vkb_wire_vkCmdCopyImageToBuffer(cb, sc->images[i].image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sc->staging, 1, &region);
        b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        b.dstAccessMask = 0;
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        VkMemoryBarrier hb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
        vkb_wire_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 1,
                                      &hb, 0, NULL, 1, &b);
        r = vkb_ep_vkEndCommandBuffer(cb);
        if (r != VK_SUCCESS) return r;
    }
    return VK_SUCCESS;
}

/*
 * How long one frame may take to render before the swapchain calls the device lost. A frame that
 * is slow is not a lost device: while a game loads, the GPU can be busy for many seconds with the
 * frame queued behind it (Watch_Dogs on a Dimensity 8300: about 2,500 BC textures
 * converted on the GPU in 50 s with the CPU held at 1.4 GHz; the frame passed 10 s, the server
 * lost nothing, and the 10 s limit this replaced told DXVK the device was lost). A real loss still
 * ends the wait at once: the server's wait returns VK_ERROR_DEVICE_LOST.
 *
 * VKBRIDGE_PRESENT_TIMEOUT_S=<n> (0: never) in the guest's environment, else the server's value
 * (vkbQueryServer's present_timeout_s: the game's preset, <cache dir>/env, the server's
 * environment), else 120 s.
 */
#define PRESENT_TIMEOUT_DEFAULT_S 120u
#define PRESENT_WAIT_STEP_NS (10ull * 1000 * 1000 * 1000)

static uint64_t wsi_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* seconds; VKB_PRESENT_TIMEOUT_NONE: no limit */
static uint32_t present_timeout_s(vkb_swapchain *sc)
{
    const char *e = getenv("VKBRIDGE_PRESENT_TIMEOUT_S");
    if (e && *e) {
        char *end;
        unsigned long long v = strtoull(e, &end, 10);
        if (!*end && *e != '-') return !v || v >= VKB_PRESENT_TIMEOUT_NONE ? VKB_PRESENT_TIMEOUT_NONE : (uint32_t)v;
        VKB_ONCE(VKB_LOG_WARN, "VKBRIDGE_PRESENT_TIMEOUT_S=%.40s is not a number of seconds; not applied", e);
    }
    uint32_t srv = sc->dev && sc->dev->pd ? sc->dev->pd->info.present_timeout_s : 0;
    return srv ? srv : PRESENT_TIMEOUT_DEFAULT_S;
}

/* Waits for a frame's fence in 10 s steps, up to the limit above, and resets it. VK_SUCCESS, or
 * the wait's error (the server's VK_ERROR_DEVICE_LOST), or VK_ERROR_DEVICE_LOST past the limit.
 * t0: when the wait for this frame began (a late copy present's began in vkQueuePresentKHR). */
static VkResult wait_frame_from(vkb_swapchain *sc, VkFence fence, uint64_t t0)
{
    VkDevice device = (VkDevice)sc->dev;
    static int slow_said, late_said;
    uint32_t limit_s = present_timeout_s(sc);
    uint64_t limit = limit_s == VKB_PRESENT_TIMEOUT_NONE ? UINT64_MAX : (uint64_t)limit_s * 1000000000ull;
    uint64_t waited = wsi_now_ns() - t0;
    VkResult r;
    for (;;) {
        if (waited >= limit) {
            VKB_ERR("swapchain: a frame took more than %u s to render; the device is reported lost (VKBRIDGE_PRESENT_TIMEOUT_S, "
                    "0: no limit)", limit_s);
            r = VK_ERROR_DEVICE_LOST;
            break;
        }
        uint64_t step = limit - waited < PRESENT_WAIT_STEP_NS ? limit - waited : PRESENT_WAIT_STEP_NS;
        vkb_hitch_in_present = 1; /* the swapchain's wait, not the app's (hitch.c) */
        r = vkb_sm_wait_fences(sc->dev, 1, &fence, VK_TRUE, step); /* done already: no round trip (syncmirror.c) */
        vkb_hitch_in_present = 0;
        waited = wsi_now_ns() - t0;
        if (r != VK_TIMEOUT) break;
        if (waited >= limit) {
            VKB_ERR("swapchain: a frame took more than %u s to render; the device is reported lost (VKBRIDGE_PRESENT_TIMEOUT_S, "
                    "0: no limit)", limit_s);
            r = VK_ERROR_DEVICE_LOST;
            break;
        }
        if (!__atomic_exchange_n(&slow_said, 1, __ATOMIC_RELAXED)) {
            if (limit == UINT64_MAX)
                VKB_WARN("swapchain: a frame is taking more than 10 s to render (the GPU is busy, a game loading?); still waiting, "
                         "no limit");
            else
                VKB_WARN("swapchain: a frame is taking more than 10 s to render (the GPU is busy, a game loading?); still waiting, "
                         "up to %u s (VKBRIDGE_PRESENT_TIMEOUT_S)", limit_s);
        }
    }
    vkb_ep_vkResetFences(device, 1, &fence);
    if (r == VK_SUCCESS && waited >= PRESENT_WAIT_STEP_NS && !__atomic_exchange_n(&late_said, 1, __ATOMIC_RELAXED))
        VKB_WARN("swapchain: the slow frame rendered after %.1f s and was presented late", waited / 1e9);
    return r;
}

VkResult vkb_wsi_wait_frame(vkb_swapchain *sc, VkFence fence) { return wait_frame_from(sc, fence, wsi_now_ns()); }

/*
 * Copy presents (X11, wl_shm, headless) read the frame back, so they wait for it on the CPU. The
 * app's thread waits only VKBRIDGE_PRESENT_HOLD_MS (default 500 ms; 0: not at all) of that: a frame
 * not rendered by then is shown by the swapchain's own thread once it is, and vkQueuePresentKHR
 * returns. A queue may wait for a timeline value that is signalled later, by the host or by
 * another queue (wait-before-signal, D3D12's Wait before Signal), and the thread that will signal
 * it may be the one presenting: waiting for the frame there waited for itself, until the present
 * timeout called the device lost. Frames rendered within the hold go as before (the same pacing and
 * latency); a late frame is still handed over only after its GPU work. One late frame at a time
 * per swapchain (the read-back buffer and the fence are the swapchain's): the next present, or an
 * acquire that finds no free image, waits for it first. The dma-buf path (Wayland, every DroidDeck
 * game) never waited in the app's call: its presenting thread does (wsi_wayland.c).
 */
#define PRESENT_HOLD_DEFAULT_MS 500u

static uint64_t present_hold_ns(void)
{
    static uint64_t v = UINT64_MAX - 1;
    uint64_t c = __atomic_load_n(&v, __ATOMIC_RELAXED);
    if (c != UINT64_MAX - 1) return c;
    uint64_t ms = PRESENT_HOLD_DEFAULT_MS;
    const char *e = getenv("VKBRIDGE_PRESENT_HOLD_MS");
    if (e && *e) {
        char *end;
        unsigned long long n = strtoull(e, &end, 10);
        if (!*end && *e != '-') ms = n > 3600000ull ? 3600000ull : n;
        else VKB_ONCE(VKB_LOG_WARN, "VKBRIDGE_PRESENT_HOLD_MS=%.40s is not a number of milliseconds; %u used", e, PRESENT_HOLD_DEFAULT_MS);
    }
    c = ms * 1000000ull;
    __atomic_store_n(&v, c, __ATOMIC_RELAXED);
    return c;
}

static void dump_frame(vkb_swapchain *sc, uint32_t index);
static int dump_on(void);

/* The read-back frame is in the staging buffer: its CPU view brought up to date. */
static VkResult staging_ready(vkb_swapchain *sc)
{
    if (!sc->copy_present || !sc->staging_cached) return VK_SUCCESS;
    /* The copy is done: the CPU's stale lines of the staging buffer dropped before it is read
     * (server_memory.c: DMA_BUF_IOCTL_SYNC). */
    VkMappedMemoryRange whole = {VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, NULL, sc->staging_mem, 0, VK_WHOLE_SIZE};
    return vkb_ep_vkInvalidateMappedMemoryRanges((VkDevice)sc->dev, 1, &whole);
}

static void *late_thread(void *arg)
{
    vkb_swapchain *sc = arg;
    pthread_setname_np(pthread_self(), "vkb-present-cp");
    pthread_mutex_lock(&sc->lock);
    for (;;) {
        while (!sc->late_pending && !sc->late_quit) pthread_cond_wait(&sc->late_cond, &sc->lock);
        if (!sc->late_pending) break;
        uint32_t index = sc->late_index, lat = sc->late_lat;
        uint64_t pid = sc->late_pid, t0 = sc->late_t0;
        pthread_mutex_unlock(&sc->lock);
        VkResult r = wait_frame_from(sc, sc->fence, t0);
        if (r >= 0) {
            vkb_lat_stamp(lat, VKB_LAT_DONE, 0);
            r = staging_ready(sc);
        }
        if (r >= 0 && dump_on()) dump_frame(sc, index);
        if (r >= 0 && sc->status >= 0) {
            r = sc->backend->present(sc, index, NULL);
            if (r >= 0) vkb_wsi_pw_committed(sc, pid, 0);
        }
        vkb_lat_presented(lat, r >= 0);
        uint64_t ns = wsi_now_ns() - t0;
        pthread_mutex_lock(&sc->lock);
        sc->images[index].busy = 0;
        if (r < 0 && sc->status >= 0) sc->status = r;
        sc->late_frames++;
        if (ns > sc->late_max_ns) sc->late_max_ns = ns;
        sc->late_pending = 0;
        pthread_cond_broadcast(&sc->late_cond);
        pthread_cond_broadcast(&sc->pw_cond); /* a broken chain ends present waits */
    }
    pthread_mutex_unlock(&sc->lock);
    return NULL;
}

/* Waits until no late frame is pending (the next copy present reuses the read-back buffer and the
 * fence). deadline: CLOCK_MONOTONIC ns, UINT64_MAX: none. 1 if one was pending. */
static int late_wait(vkb_swapchain *sc, uint64_t deadline)
{
    pthread_mutex_lock(&sc->lock);
    int had = sc->late_pending;
    while (sc->late_pending) {
        if (deadline == UINT64_MAX) {
            pthread_cond_wait(&sc->late_cond, &sc->lock);
            continue;
        }
        if (wsi_now_ns() >= deadline) break;
        struct timespec ts = {(time_t)(deadline / 1000000000ull), (long)(deadline % 1000000000ull)};
        pthread_cond_timedwait(&sc->late_cond, &sc->lock, &ts);
    }
    pthread_mutex_unlock(&sc->lock);
    return had;
}

/* The frame in image `index` (its fence not yet signalled) to the swapchain's thread. 0 if that
 * thread cannot be started: the caller waits as before. */
static int late_hand_over(vkb_swapchain *sc, uint32_t index, uint64_t pid, uint64_t t0)
{
    static int said;
    pthread_mutex_lock(&sc->lock);
    if (!sc->late_on) {
        if (pthread_create(&sc->late_thread, NULL, late_thread, sc) != 0) {
            pthread_mutex_unlock(&sc->lock);
            return 0;
        }
        sc->late_on = 1;
    }
    if (!__atomic_exchange_n(&said, 1, __ATOMIC_RELAXED))
        VKB_INFO("swapchain: a frame not rendered within %llu ms is shown by the swapchain's thread once it is; the app's present "
                 "returns (VKBRIDGE_PRESENT_HOLD_MS)", (unsigned long long)(present_hold_ns() / 1000000ull));
    sc->images[index].acquired = 0;
    sc->images[index].busy = 1; /* not acquirable until shown */
    sc->late_index = index;
    sc->late_lat = sc->lat_n;
    sc->late_pid = pid;
    sc->late_t0 = t0;
    sc->late_pending = 1;
    pthread_cond_broadcast(&sc->late_cond);
    pthread_mutex_unlock(&sc->lock);
    return 1;
}

/* vkDestroySwapchainKHR: a late frame still pending is shown (or fails) first. */
static void late_stop(vkb_swapchain *sc)
{
    pthread_mutex_lock(&sc->lock);
    int on = sc->late_on;
    sc->late_quit = 1;
    pthread_cond_broadcast(&sc->late_cond);
    pthread_mutex_unlock(&sc->lock);
    if (on) pthread_join(sc->late_thread, NULL);
    sc->late_on = 0;
    if (sc->late_frames)
        VKB_INFO("swapchain: %llu frames not rendered within the hold were shown by the swapchain's thread, the longest %.1f ms after "
                 "its present", (unsigned long long)sc->late_frames, sc->late_max_ns / 1e6);
}

/* Waits (on the CPU) for the app's rendering into image `index`, reading it back when needed: up to
 * VKBRIDGE_PRESENT_HOLD_MS, then *late = 1 and the swapchain's thread finishes it (above). */
VkResult vkb_wsi_wait_rendering(vkb_swapchain *sc, VkQueue queue, uint32_t index, uint32_t nwait, const VkSemaphore *wait,
                                uint64_t pid, int *late)
{
    *late = 0;
    late_wait(sc, UINT64_MAX); /* an earlier late frame: the read-back buffer and the fence are free again */
    if (sc->status < 0) return sc->status;
    VkPipelineStageFlags *stages = alloca((nwait ? nwait : 1) * sizeof(VkPipelineStageFlags));
    for (uint32_t i = 0; i < nwait; i++) stages[i] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, nwait, wait, stages, 0, NULL, 0, NULL};
    if (sc->copy_present) {
        VkResult r = ensure_copy_cbs(sc, ((vkb_queue *)queue)->family);
        if (r != VK_SUCCESS) return r;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &sc->copy_cbs[index];
    }
    VkResult r = vkb_sm_queue_submit(queue, 1, &si, sc->fence);
    if (r != VK_SUCCESS) return r;
    uint64_t t0 = wsi_now_ns(), hold = present_hold_ns();
    vkb_hitch_in_present = 1; /* the swapchain's wait, not the app's (hitch.c) */
    r = vkb_sm_wait_fences(sc->dev, 1, &sc->fence, VK_TRUE, hold);
    vkb_hitch_in_present = 0;
    if (r == VK_TIMEOUT && late_hand_over(sc, index, pid, t0)) {
        *late = 1;
        return VK_SUCCESS;
    }
    if (r == VK_SUCCESS) vkb_ep_vkResetFences((VkDevice)sc->dev, 1, &sc->fence);
    else if (r == VK_TIMEOUT) r = wait_frame_from(sc, sc->fence, t0);
    if (r != VK_SUCCESS) return r;
    return staging_ready(sc);
}

/* ------------------------------------------------------------------ swapchain */

static void swapchain_free(vkb_swapchain *sc)
{
    VkDevice device = (VkDevice)sc->dev;
    if (sc->backend && sc->backend->destroy) sc->backend->destroy(sc);
    for (uint32_t i = 0; i < sc->count; i++) destroy_image(sc, &sc->images[i]);
    if (sc->pool) vkb_ep_vkDestroyCommandPool(device, sc->pool, NULL);
    if (sc->fence) vkb_ep_vkDestroyFence(device, sc->fence, NULL);
    if (sc->staging) vkb_ep_vkDestroyBuffer(device, sc->staging, NULL);
    if (sc->staging_mem) vkb_ep_vkFreeMemory(device, sc->staging_mem, NULL);
    free(sc->images);
    free(sc->copy_cbs);
    free(sc->view_formats);
    pthread_cond_destroy(&sc->pw_cond);
    pthread_cond_destroy(&sc->late_cond);
    pthread_mutex_destroy(&sc->lock);
    free(sc);
}

static VkResult swapchain_build(vkb_swapchain *sc, int dmabuf)
{
    VkDevice device = (VkDevice)sc->dev;
    sc->dmabuf = dmabuf;
    for (uint32_t i = 0; i < sc->count; i++) {
        VkResult r = create_image(sc, &sc->images[i], dmabuf);
        if (r != VK_SUCCESS) return r;
        sc->images[i].chain = sc;
    }
    if (sc->copy_present) {
        VkResult r = create_staging(sc);
        if (r != VK_SUCCESS) return r;
    }
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkResult r = vkb_ep_vkCreateFence(device, &fci, NULL, &sc->fence);
    if (r != VK_SUCCESS) return r;
    return sc->backend->init(sc);
}

static void swapchain_teardown(vkb_swapchain *sc)
{
    VkDevice device = (VkDevice)sc->dev;
    if (sc->backend && sc->backend->destroy) sc->backend->destroy(sc);
    sc->bdata = NULL;
    for (uint32_t i = 0; i < sc->count; i++) {
        destroy_image(sc, &sc->images[i]);
        memset(&sc->images[i], 0, sizeof(sc->images[i]));
        sc->images[i].dmabuf_fd = sc->images[i].ahb_sock = -1;
    }
    if (sc->fence) vkb_ep_vkDestroyFence(device, sc->fence, NULL);
    sc->fence = VK_NULL_HANDLE;
    if (sc->staging) vkb_ep_vkDestroyBuffer(device, sc->staging, NULL);
    if (sc->staging_mem) vkb_ep_vkFreeMemory(device, sc->staging_mem, NULL);
    sc->staging = VK_NULL_HANDLE;
    sc->staging_mem = VK_NULL_HANDLE;
    sc->staging_ptr = NULL;
}

VKAPI_ATTR VkResult VKAPI_CALL vkb_ep_vkCreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR *pCreateInfo,
                                                          const VkAllocationCallbacks *pAllocator, VkSwapchainKHR *pSwapchain)
{
    (void)pAllocator;
    vkb_frames_swapchain(); /* asked for (made or not): the process presents through the bridge (frames.c) */
    vkb_device *dev = vkb_dev(device);
    vkb_surface *s = surf(pCreateInfo->surface);
    if (pCreateInfo->oldSwapchain) {
        vkb_swapchain *old = chain(pCreateInfo->oldSwapchain);
        pthread_mutex_lock(&old->lock);
        old->retired = 1;
        pthread_cond_broadcast(&old->pw_cond); /* a wait for a present it will never make ends (out of date) */
        pthread_mutex_unlock(&old->lock);
    }
    vkb_swapchain *sc = calloc(1, sizeof(*sc));
    if (!sc) return VK_ERROR_OUT_OF_HOST_MEMORY;
    pthread_mutex_init(&sc->lock, NULL);
    pthread_condattr_t ca;
    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    pthread_cond_init(&sc->pw_cond, &ca);
    pthread_cond_init(&sc->late_cond, &ca);
    pthread_condattr_destroy(&ca);
    sc->dev = dev;
    sc->surface = s;
    sc->info = *pCreateInfo;
    sc->info.pNext = NULL;
    sc->info.pQueueFamilyIndices = NULL;
    sc->info.queueFamilyIndexCount = 0;
    sc->info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    for (const VkBaseInStructure *b = pCreateInfo->pNext; b; b = b->pNext) {
        if (b->sType == VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO) {
            const VkImageFormatListCreateInfo *fl = (const void *)b;
            sc->view_formats = calloc(fl->viewFormatCount + 1, sizeof(VkFormat));
            memcpy(sc->view_formats, fl->pViewFormats, fl->viewFormatCount * sizeof(VkFormat));
            sc->view_format_count = fl->viewFormatCount;
        }
    }
    sc->count = pCreateInfo->minImageCount < 2 ? 2 : pCreateInfo->minImageCount;
    if (s->kind == VKB_SURF_WAYLAND && sc->count < 3) sc->count = 3;
    sc->images = calloc(sc->count, sizeof(vkb_swap_image));
    sc->copy_cbs = calloc(sc->count, sizeof(VkCommandBuffer));
    for (uint32_t i = 0; i < sc->count; i++) sc->images[i].dmabuf_fd = sc->images[i].ahb_sock = -1;
    VkResult r;
    switch (s->kind) {
    case VKB_SURF_WAYLAND:
        sc->backend = &vkb_wsi_wayland_backend;
        sc->copy_present = getenv("VKBRIDGE_WSI_SHM") != NULL;
        /* Gralloc images when DroidDeck's compositor wants them (zero-copy on): the display shows
         * the game's own buffers. Anything going wrong falls back to ordinary dma-bufs. */
        sc->ahb = !sc->copy_present && !vkb_wsi_ahb_disabled() && vkb_wl_ahb_wanted(sc);
        r = swapchain_build(sc, !sc->copy_present);
        if (r != VK_SUCCESS && sc->ahb) {
            static int told;
            if (__atomic_fetch_add(&told, 1, __ATOMIC_RELAXED) < 3)
                VKB_WARN("Wayland: no gralloc swapchain (%d); presenting ordinary dma-bufs (the compositor copies them)", r);
            swapchain_teardown(sc);
            sc->ahb = 0;
            r = swapchain_build(sc, 1);
        }
        if (r != VK_SUCCESS && !sc->copy_present) {
            VKB_WARN("Wayland: no dma-buf presentation (%d); falling back to copies through shared memory", r);
            swapchain_teardown(sc);
            sc->copy_present = 1;
            r = swapchain_build(sc, 0);
        }
        break;
    case VKB_SURF_XCB:
        sc->backend = &vkb_wsi_x11_backend;
        sc->copy_present = 1;
        r = swapchain_build(sc, 0);
        break;
    default:
        sc->backend = &vkb_wsi_headless_backend;
        r = swapchain_build(sc, 0);
        break;
    }
    if (r != VK_SUCCESS) {
        VKB_ERR("swapchain creation failed: %d", r);
        swapchain_free(sc);
        return r;
    }
    VKB_INFO("swapchain %ux%u format %d, %u images, %s", sc->info.imageExtent.width, sc->info.imageExtent.height, sc->info.imageFormat,
             sc->count, sc->copy_present ? "presented by copy" : sc->ahb ? "presented as gralloc dma-bufs (zero-copy layer)"
                        : sc->dmabuf ? "presented as dma-bufs" : "headless");
    *pSwapchain = (VkSwapchainKHR)(uintptr_t)sc;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkb_ep_vkDestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks *pAllocator)
{
    (void)pAllocator;
    if (!swapchain) return;
    vkb_swapchain *sc = chain(swapchain);
    /* Threads still waiting for a present of it are let go first (out of date). */
    pthread_mutex_lock(&sc->lock);
    sc->pw_closing = 1;
    pthread_cond_broadcast(&sc->pw_cond);
    while (sc->pw_waiters) pthread_cond_wait(&sc->pw_cond, &sc->lock);
    pthread_mutex_unlock(&sc->lock);
    if (sc->pw_waits)
        VKB_INFO("present wait: %llu waits, %llu timed out, %.1f ms on average, longest %.1f ms; %llu frames counted shown without the "
                 "compositor's word", (unsigned long long)sc->pw_waits, (unsigned long long)sc->pw_timeouts,
                 sc->pw_wait_ns / 1e6 / (double)sc->pw_waits, sc->pw_wait_max_ns / 1e6, (unsigned long long)sc->pw_unanswered);
    late_stop(sc); /* a late copy present is shown first (it waits for its frame, up to the present timeout) */
    /* Images may still be in use by the GPU (present waits are CPU-side, so not by us). */
    vkb_wire_vkDeviceWaitIdle(device);
    swapchain_free(sc);
    vkb_lat_flush(); /* the latency log's last records (latency.c) */
}

VKAPI_ATTR VkResult VKAPI_CALL vkb_ep_vkGetSwapchainImagesKHR(VkDevice device, VkSwapchainKHR swapchain, uint32_t *pCount, VkImage *pImages)
{
    (void)device;
    vkb_swapchain *sc = chain(swapchain);
    if (!pImages) {
        *pCount = sc->count;
        return VK_SUCCESS;
    }
    uint32_t n = *pCount < sc->count ? *pCount : sc->count;
    for (uint32_t i = 0; i < n; i++) pImages[i] = sc->images[i].image;
    *pCount = n;
    return n < sc->count ? VK_INCOMPLETE : VK_SUCCESS;
}

/* Signals the app's semaphore/fence for an acquired image (no GPU work is involved). */
static VkResult signal_acquire(vkb_device *dev, VkSemaphore sem, VkFence fence)
{
    VkDevice device = (VkDevice)dev;
    if (fence) vkb_sm_fence_unknown(dev, fence); /* imported or signalled by an empty submit: asked of the server */
    if (sem) {
        VkImportSemaphoreFdInfoKHR si = {VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR, NULL, sem, VK_SEMAPHORE_IMPORT_TEMPORARY_BIT,
                                         VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT, -1};
        VkResult r = vkb_wire_vkImportSemaphoreFdKHR(device, &si);
        if (r != VK_SUCCESS) {
            /* No sync-fd import: signal it with an empty submit. */
            if (!dev->queues) return VK_ERROR_SURFACE_LOST_KHR;
            VkSubmitInfo s = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 0, NULL, 1, &sem};
            r = vkb_wire_vkQueueSubmit((VkQueue)dev->queues, 1, &s, fence);
            return r;
        }
    }
    if (fence) {
        VkImportFenceFdInfoKHR fi = {VK_STRUCTURE_TYPE_IMPORT_FENCE_FD_INFO_KHR, NULL, fence, VK_FENCE_IMPORT_TEMPORARY_BIT,
                                     VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT, -1};
        VkResult r = vkb_wire_vkImportFenceFdKHR(device, &fi);
        if (r != VK_SUCCESS) {
            if (!dev->queues) return VK_ERROR_SURFACE_LOST_KHR;
            r = vkb_wire_vkQueueSubmit((VkQueue)dev->queues, 0, NULL, fence);
            return r;
        }
    }
    return VK_SUCCESS;
}

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* A gralloc image may come back (wl_buffer.release) while the display still scans it out: the
 * compositor puts the display's release fence into the dma-buf instead of waiting for it (see
 * banner-ahb-v1.xml), and the server's GPU does not wait for a dma-buf's fences. Waiting here for
 * every fence on the dma-buf (POLLOUT) keeps the next frame from being drawn into the picture on
 * screen; it is normally signalled already. Bounded, so a lost fence costs a tear, not a hang. */
static void wait_display_release(const vkb_swap_image *img)
{
    if (img->dmabuf_fd < 0) return;
    struct pollfd p = {img->dmabuf_fd, POLLOUT, 0};
    int r;
    do {
        r = poll(&p, 1, 100);
    } while (r < 0 && errno == EINTR);
    static int told;
    if (r == 0 && !__atomic_fetch_add(&told, 1, __ATOMIC_RELAXED))
        VKB_WARN("Wayland: the display held a gralloc image for more than 100 ms after its release; drawing into it anyway");
}

static VkResult acquire(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout, VkSemaphore semaphore, VkFence fence,
                        uint32_t *pImageIndex)
{
    vkb_swapchain *sc = chain(swapchain);
    if (sc->retired) return VK_ERROR_OUT_OF_DATE_KHR;
    if (sc->status != VK_SUCCESS) return sc->status;
    uint64_t deadline = timeout == UINT64_MAX ? UINT64_MAX : now_ns() + timeout;
    for (;;) {
        pthread_mutex_lock(&sc->lock);
        for (uint32_t i = 0; i < sc->count; i++) {
            if (!sc->images[i].busy && !sc->images[i].acquired) {
                sc->images[i].acquired = 1;
                pthread_mutex_unlock(&sc->lock);
                if (sc->ahb) wait_display_release(&sc->images[i]);
                VkResult r = signal_acquire(vkb_dev(device), semaphore, fence);
                if (r != VK_SUCCESS) return r;
                *pImageIndex = i;
                return sc->status;
            }
        }
        pthread_mutex_unlock(&sc->lock);
        uint64_t now = now_ns();
        if (timeout == 0) return VK_NOT_READY;
        if (now >= deadline) return VK_TIMEOUT;
        /* a late copy present holds an image until shown: wait for it, not for the window system */
        if (late_wait(sc, deadline)) continue;
        uint64_t ht0 = vkb_hitch_active ? now_ns() : 0;
        VkResult r = sc->backend->wait_free(sc, deadline == UINT64_MAX ? UINT64_MAX : deadline - now);
        if (ht0) vkb_hitch_waited(VKB_HITCH_W_PRESENT, VKB_CMD_COUNT, now_ns() - ht0);
        if (r < 0) {
            sc->status = r;
            return r;
        }
    }
}

/* An error from an acquire or a present is logged the first time (vkb_first_error). */
VKAPI_ATTR VkResult VKAPI_CALL vkb_ep_vkAcquireNextImageKHR(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout, VkSemaphore semaphore,
                                                           VkFence fence, uint32_t *pImageIndex)
{
    return vkb_note_error("vkAcquireNextImageKHR", acquire(device, swapchain, timeout, semaphore, fence, pImageIndex));
}

VKAPI_ATTR VkResult VKAPI_CALL vkb_ep_vkAcquireNextImage2KHR(VkDevice device, const VkAcquireNextImageInfoKHR *pInfo, uint32_t *pImageIndex)
{
    return vkb_ep_vkAcquireNextImageKHR(device, pInfo->swapchain, pInfo->timeout, pInfo->semaphore, pInfo->fence, pImageIndex);
}

/* VKBRIDGE_WSI_DUMP=/path/prefix: writes presented frame 60 as a PPM (debugging aid). */
static void dump_frame(vkb_swapchain *sc, uint32_t index)
{
    /* The 60th present, or every VKBRIDGE_WSI_DUMP_EVERY-th (watching a game get somewhere). */
    static long every = -1;
    if (every < 0) every = getenv("VKBRIDGE_WSI_DUMP_EVERY") ? atol(getenv("VKBRIDGE_WSI_DUMP_EVERY")) : 0;
    if (every > 0 ? sc->presents % every != 0 : sc->presents != 60) return;
    uint32_t w = sc->info.imageExtent.width, h = sc->info.imageExtent.height, stride = w * 4;
    const uint8_t *px = NULL;
    void *map = NULL;
    size_t maplen = 0;
    if (sc->copy_present) {
        px = sc->staging_ptr;
    } else if (sc->images[index].dmabuf_fd >= 0) {
        stride = sc->images[index].stride;
        maplen = (size_t)sc->images[index].offset + (size_t)stride * h;
        map = mmap(NULL, maplen, PROT_READ, MAP_SHARED, sc->images[index].dmabuf_fd, 0);
        if (map == MAP_FAILED) return;
        px = (const uint8_t *)map + sc->images[index].offset;
    }
    if (!px) return;
    char path[512];
    if (every > 0)
        snprintf(path, sizeof(path), "%s-%d-%06u.ppm", getenv("VKBRIDGE_WSI_DUMP"), (int)getpid(), (unsigned)sc->presents);
    else
        snprintf(path, sizeof(path), "%s-%d.ppm", getenv("VKBRIDGE_WSI_DUMP"), (int)getpid());
    FILE *f = fopen(path, "wb");
    if (f) {
        int bgr = sc->info.imageFormat == VK_FORMAT_B8G8R8A8_UNORM || sc->info.imageFormat == VK_FORMAT_B8G8R8A8_SRGB;
        fprintf(f, "P6 %u %u 255\n", w, h);
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 0; x < w; x++) {
                const uint8_t *p = px + (size_t)y * stride + x * 4;
                uint8_t rgb[3] = {p[bgr ? 2 : 0], p[1], p[bgr ? 0 : 2]};
                fwrite(rgb, 1, 3, f);
            }
        fclose(f);
        VKB_INFO("dumped frame to %s", path);
    }
    if (map) munmap(map, maplen);
}

static int dump_on(void)
{
    static int v = -1;
    if (v < 0) v = getenv("VKBRIDGE_WSI_DUMP") != NULL;
    return v;
}

/* VKBRIDGE_FRAME_QUEUE=N (1-8; a per-game preset, <cache dir>/env or the server's environment, or
 * the guest's environment, which wins; 0 or unset: off). With presents that go back to the app at
 * once (dma-buf images on Wayland: the presenting thread shows them), a game can get several
 * frames ahead of the GPU, and each frame queued is a frame of input lag when the GPU is the limit.
 * On, vkQueuePresentKHR returns only once at most N - 1 presented frames, its own included, are
 * still rendering (1: once its own frame is rendered; 2: once every earlier one is). DXVK and vkd3d-proton present from a thread of
 * their own, one frame behind the game, so for them N is D3D's maximum frame latency N
 * (dxgi.maxFrameLatency): 1 measured the same as DXVK's own setting on the host, about 1% fps. A
 * game that presents from its render thread (native Vulkan) loses its CPU/GPU overlap at 1; 2 is
 * its "one frame ahead". DXVK's own setting is better for DXVK games (it also counts frames DXVK
 * holds before presenting): with a maxFrameLatency in DXVK_CONFIG this switch stands aside (both
 * on measured about 1% slower than DXVK's alone, for the same latency). Copy presents (X11, wl_shm)
 * already wait for each frame: nothing changes there. The line at the swapchain's end says how often and how long presents waited. */
static uint32_t fq_server, fq_value;
static int fq_decided;

void vkb_wsi_frame_queue_server(uint32_t n) { __atomic_store_n(&fq_server, n > 8 ? 8 : n, __ATOMIC_RELAXED); }

static uint32_t frame_queue(void)
{
    if (__atomic_load_n(&fq_decided, __ATOMIC_ACQUIRE)) return fq_value;
    const char *e = getenv("VKBRIDGE_FRAME_QUEUE");
    uint32_t n = __atomic_load_n(&fq_server, __ATOMIC_RELAXED);
    if (e && *e) {
        char *end;
        unsigned long v = strtoul(e, &end, 10);
        n = *end || *e == '-' ? 0 : v > 8 ? 8 : (uint32_t)v;
    }
    if (n && vkb_dxvkcfg_own_latency()) {
        VKB_INFO("frame queue: VKBRIDGE_FRAME_QUEUE=%u not used: DXVK keeps its own queue short (maxFrameLatency in DXVK_CONFIG)", n);
        n = 0;
    }
    fq_value = n;
    __atomic_store_n(&fq_decided, 1, __ATOMIC_RELEASE);
    if (n)
        VKB_INFO("frame queue: a present returns once at most %u frame%s (its own included) %s still rendering (VKBRIDGE_FRAME_QUEUE=%u)",
                 n - 1, n - 1 == 1 ? "" : "s", n - 1 == 1 ? "is" : "are", n);
    return n;
}

VKAPI_ATTR VkResult VKAPI_CALL vkb_ep_vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *pPresentInfo)
{
    const VkPresentRegionsKHR *regions = NULL;
    const uint64_t *ids = NULL; /* VkPresentIdKHR / VkPresentId2KHR: one per swapchain, 0 = none */
    uint32_t nids = 0;
    vkb_frames_present(); /* for the server's session report (frames.c) */
    vkb_hitch_present();  /* the per-hitch line (hitch.c) */
    vkb_dxvkcfg_check();  /* once: what DXVK was given (dxvkcfg.c) */
    uint32_t lat = vkb_lat_present(); /* the latency log (latency.c): before any of this present reaches the server */
    for (const VkBaseInStructure *b = pPresentInfo->pNext; b; b = b->pNext)
        if (b->sType == VK_STRUCTURE_TYPE_PRESENT_REGIONS_KHR) regions = (const void *)b;
        else if (b->sType == VK_STRUCTURE_TYPE_PRESENT_ID_KHR || b->sType == VK_STRUCTURE_TYPE_PRESENT_ID_2_KHR) {
            /* the two structs have the same layout */
            const VkPresentIdKHR *pid = (const void *)b;
            ids = pid->pPresentIds;
            nids = ids ? pid->swapchainCount : 0;
        }
    VkResult worst = VK_SUCCESS;
    for (uint32_t i = 0; i < pPresentInfo->swapchainCount; i++) {
        vkb_swapchain *sc = chain(pPresentInfo->pSwapchains[i]);
        uint32_t index = pPresentInfo->pImageIndices[i];
        VkResult r = sc->status;
        if (index >= sc->count) r = VK_ERROR_OUT_OF_DATE_KHR;
        uint32_t nwait = i == 0 ? pPresentInfo->waitSemaphoreCount : 0;
        uint64_t pid = i < nids ? ids[i] : 0;
        if (pid && r >= 0) {
            pthread_mutex_lock(&sc->lock);
            if (pid > sc->pw_queued) sc->pw_queued = pid;
            pthread_mutex_unlock(&sc->lock);
        }
        if (r >= 0 && sc->backend->present_async && sc->dmabuf && !sc->copy_present && !dump_on()) {
            /* The presenting thread goes back to the game at once: the GPU's work and the next
             * frame's overlap (Hades' Vulkan renderer presents from its render thread; waiting
             * here held it to ~30 fps on the tablet, 2026-10-11). */
            pthread_mutex_lock(&sc->lock);
            sc->images[index].acquired = 0;
            sc->images[index].busy = 1;
            pthread_mutex_unlock(&sc->lock);
            /* the presenting thread stamps it from here on; one swapchain's (the first) per present */
            sc->lat_n = i == 0 ? lat : 0;
            sc->pw_cur = pid; /* the presenting thread says when it is shown */
            vkb_lat_flag(sc->lat_n, VKB_LAT_F_ASYNC);
            r = sc->backend->present_async(sc, queue, index, nwait, pPresentInfo->pWaitSemaphores);
            if (r < 0) vkb_lat_presented(sc->lat_n, 0);
            sc->lat_n = 0;
            sc->pw_cur = 0;
            sc->presents++;
            uint32_t fq = r >= 0 && sc->backend->wait_queue ? frame_queue() : 0;
            if (fq) {
                uint64_t ht0 = vkb_hitch_active ? now_ns() : 0;
                sc->backend->wait_queue(sc, fq - 1);
                if (ht0) vkb_hitch_waited(VKB_HITCH_W_PRESENT, VKB_CMD_COUNT, now_ns() - ht0);
            }
            goto done;
        }
        sc->lat_n = i == 0 ? lat : 0;
        if (r >= 0) {
            int late = 0;
            r = vkb_wsi_wait_rendering(sc, queue, index, nwait, pPresentInfo->pWaitSemaphores, pid, &late);
            if (late) {
                /* not rendered within the hold: the swapchain's thread shows it (and stamps it) */
                sc->presents++;
                sc->lat_n = 0;
                goto done;
            }
            if (r >= 0) vkb_lat_stamp(sc->lat_n, VKB_LAT_DONE, 0);
        }
        pthread_mutex_lock(&sc->lock);
        if (index < sc->count) {
            sc->images[index].acquired = 0;
            sc->images[index].busy = sc->dmabuf && !sc->copy_present;
        }
        pthread_mutex_unlock(&sc->lock);
        if (r >= 0 && dump_on()) dump_frame(sc, index);
        if (r >= 0) {
            r = sc->backend->present(sc, index, regions && i < regions->swapchainCount ? &regions->pRegions[i] : NULL);
            sc->presents++;
            /* a copy present (X11, wl_shm), headless: shown when handed over */
            if (r >= 0) vkb_wsi_pw_committed(sc, pid, 0);
        }
        vkb_lat_presented(sc->lat_n, r >= 0);
        sc->lat_n = 0;
    done:
        if (r < 0) {
            pthread_mutex_lock(&sc->lock);
            if (index < sc->count) sc->images[index].busy = 0;
            pthread_mutex_unlock(&sc->lock);
            if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_ERROR_SURFACE_LOST_KHR) sc->status = r;
        }
        if (r == VK_SUCCESS && sc->status == VK_SUBOPTIMAL_KHR) r = VK_SUBOPTIMAL_KHR;
        if (pPresentInfo->pResults) pPresentInfo->pResults[i] = r;
        if (r < 0 && (worst >= 0 || r == VK_ERROR_DEVICE_LOST)) worst = r;
        else if (worst == VK_SUCCESS && r == VK_SUBOPTIMAL_KHR) worst = r;
    }
    return vkb_note_error("vkQueuePresentKHR", worst);
}

/* ------------------------------------------------------------------ present ids and waits */

/*
 * VK_KHR_present_id + VK_KHR_present_wait, and their successors VK_KHR_present_id2 +
 * VK_KHR_present_wait2: the app numbers its presents (VkPresentIdKHR / VkPresentId2KHR) and waits
 * until the frame with a number is shown (vkWaitForPresentKHR / vkWaitForPresent2KHR). vkd3d-proton
 * limits a D3D12 game's frame latency this way and has no limit of its own without them ("supports
 * neither present_wait1 or present_wait2. Latency will increase"); DXVK counts its frame latency to
 * the shown frame with them, to the rendered one without.
 *
 * All the swapchain's own, nothing goes to the server. "Shown" on each path:
 *  - Wayland, dma-buf images (the presenting thread; gamescope, DroidDeck's compositor): when the
 *    compositor says the frame was presented, or that a later frame replaced it (wp_presentation
 *    feedback). A compositor without wp_presentation: when the frame was committed to it. A frame
 *    the compositor has not answered for 500 ms after its commit, or by the time 4 later frames
 *    with ids were committed, counts as shown (said once, counted in the line at the end), so a
 *    compositor that never answers costs a game at most that, not a hang.
 *  - Wayland wl_shm, X11, headless (copy presents, the app's own call): when the frame was handed
 *    to the window system (the end of vkQueuePresentKHR; these presents wait for the rendering).
 * A wait returns VK_SUCCESS once the swapchain's highest shown id is at least the one asked (the
 * spec's rule: a later frame shown completes earlier ids), VK_TIMEOUT at the app's timeout,
 * the swapchain's error once it is broken (VK_ERROR_OUT_OF_DATE_KHR, SURFACE_LOST, DEVICE_LOST),
 * VK_ERROR_OUT_OF_DATE_KHR when the swapchain is being destroyed, or is retired and the id was
 * never presented on it.
 *
 * Offered by default to every app but DXVK (engine "DXVK" in its VkApplicationInfo: it keeps its
 * own frame latency without them, and measured no gain with them, see FEAT-present-wait).
 * VKBRIDGE_PRESENT_WAIT=1 (the guest's environment, or a preset / <cache dir>/env through the
 * server): offered to DXVK too. =0: the four extensions are not offered, their features read false. =commit: offered, but
 * a frame counts as shown once committed to the compositor (no wp_presentation asked), for a
 * compositor whose answers come late or in bursts (the game's pacing follows them).
 */
#define PW_GRACE_NS (500ull * 1000 * 1000)
#define PW_GRACE_COMMITS 4
#define PW_SLICE_NS (50ull * 1000 * 1000)

int vkb_wsi_present_wait_on(const vkb_physdev *pd)
{
    const char *e = getenv("VKBRIDGE_PRESENT_WAIT");
    uint32_t srv = pd ? pd->info.present_wait : 0;
    int on;
    if (e && *e) on = strcmp(e, "0") != 0;
    else if (srv) on = srv != VKB_PRESENT_WAIT_OFF;
    else if (pd && pd->instance && pd->instance->dxvk) {
        /* DXVK keeps its frame latency without them; with them it measured the same Present() ->
         * shown and about 1.4% fewer frames on the host (gamescope, GPU-bound). */
        VKB_ONCE(VKB_LOG_INFO, "present wait: not offered to DXVK (it limits its frame latency without; VKBRIDGE_PRESENT_WAIT=1 "
                               "offers it)");
        return 0;
    } else on = 1;
    if (!on)
        VKB_ONCE(VKB_LOG_INFO, "present wait: off (VKBRIDGE_PRESENT_WAIT=0): VK_KHR_present_id, present_wait, present_id2 and "
                               "present_wait2 not offered");
    return on;
}

int vkb_wsi_present_wait_commit(const vkb_physdev *pd)
{
    const char *e = getenv("VKBRIDGE_PRESENT_WAIT");
    int c = e && *e ? !strcmp(e, "commit") : pd && pd->info.present_wait == VKB_PRESENT_WAIT_COMMIT;
    if (c) VKB_ONCE(VKB_LOG_INFO, "present wait: a frame counts as shown once committed to the compositor (VKBRIDGE_PRESENT_WAIT=commit)");
    return c;
}

void vkb_wsi_patch_features(const vkb_physdev *pd, VkBaseOutStructure *b)
{
    VkBool32 on;
    switch ((int)b->sType) {
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR:
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR:
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_2_FEATURES_KHR:
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_2_FEATURES_KHR: break;
    default: return;
    }
    on = vkb_wsi_present_wait_on(pd) ? VK_TRUE : VK_FALSE;
    /* each is { sType, pNext, VkBool32 } */
    ((VkPhysicalDevicePresentIdFeaturesKHR *)b)->presentId = on;
}

/* lock held */
static void pw_done_locked(vkb_swapchain *sc, uint64_t id)
{
    if (id <= sc->pw_done) return;
    sc->pw_done = id;
    uint32_t k = 0;
    for (uint32_t i = 0; i < sc->pw_npending; i++)
        if (sc->pw_pending[i].id > id) sc->pw_pending[k++] = sc->pw_pending[i];
    sc->pw_npending = k;
    pthread_cond_broadcast(&sc->pw_cond);
}

/* lock held: frames the compositor has not answered for in time count as shown */
static void pw_unanswered_locked(vkb_swapchain *sc, uint64_t now)
{
    uint64_t upto = 0;
    for (uint32_t i = 0; i < sc->pw_npending; i++)
        if (now - sc->pw_pending[i].t >= PW_GRACE_NS || sc->pw_commits - sc->pw_pending[i].commit >= PW_GRACE_COMMITS)
            if (sc->pw_pending[i].id > upto) upto = sc->pw_pending[i].id;
    if (!upto || upto <= sc->pw_done) return;
    sc->pw_unanswered++;
    VKB_ONCE(VKB_LOG_INFO, "present wait: the compositor did not say a frame was shown (within %llu ms, or %d frames later); such "
                           "frames count as shown", PW_GRACE_NS / 1000000, PW_GRACE_COMMITS);
    pw_done_locked(sc, upto);
}

void vkb_wsi_pw_committed(vkb_swapchain *sc, uint64_t id, int feedback)
{
    if (!id) return;
    uint64_t now = now_ns();
    pthread_mutex_lock(&sc->lock);
    sc->pw_commits++;
    if (!feedback) {
        pw_done_locked(sc, id);
    } else if (id > sc->pw_done) {
        if (sc->pw_npending == sizeof(sc->pw_pending) / sizeof(sc->pw_pending[0])) {
            pw_done_locked(sc, sc->pw_pending[0].id); /* the oldest: counts as shown */
            sc->pw_unanswered++;
        }
        sc->pw_pending[sc->pw_npending].id = id;
        sc->pw_pending[sc->pw_npending].t = now;
        sc->pw_pending[sc->pw_npending].commit = sc->pw_commits;
        sc->pw_npending++;
    }
    pw_unanswered_locked(sc, now);
    pthread_mutex_unlock(&sc->lock);
}

void vkb_wsi_pw_shown(vkb_swapchain *sc, uint64_t id)
{
    if (!id) return;
    pthread_mutex_lock(&sc->lock);
    pw_done_locked(sc, id);
    pthread_mutex_unlock(&sc->lock);
}

static VkResult wait_for_present(VkDevice device, VkSwapchainKHR swapchain, uint64_t id, uint64_t timeout)
{
    (void)device;
    if (!swapchain) return VK_ERROR_OUT_OF_DATE_KHR;
    vkb_swapchain *sc = chain(swapchain);
    uint64_t t0 = now_ns();
    uint64_t deadline = timeout >= UINT64_MAX - t0 ? UINT64_MAX : t0 + timeout;
    VkResult r;
    int waited = 0;
    pthread_mutex_lock(&sc->lock);
    sc->pw_waiters++;
    for (;;) {
        uint64_t now = now_ns();
        pw_unanswered_locked(sc, now);
        if (sc->pw_closing) {
            r = VK_ERROR_OUT_OF_DATE_KHR;
            break;
        }
        if (sc->pw_done >= id) {
            r = sc->status == VK_SUBOPTIMAL_KHR ? VK_SUBOPTIMAL_KHR : VK_SUCCESS;
            break;
        }
        if (sc->status < 0) {
            r = sc->status;
            break;
        }
        if (sc->retired && id > sc->pw_queued) {
            r = VK_ERROR_OUT_OF_DATE_KHR; /* never presented, and it cannot be any more */
            break;
        }
        if (now >= deadline) {
            r = VK_TIMEOUT;
            break;
        }
        /* in slices: a status set elsewhere, a compositor's silence are seen without a signal */
        uint64_t until = deadline - now > PW_SLICE_NS ? now + PW_SLICE_NS : deadline;
        struct timespec ts = {(time_t)(until / 1000000000ull), (long)(until % 1000000000ull)};
        waited = 1;
        pthread_cond_timedwait(&sc->pw_cond, &sc->lock, &ts);
    }
    uint64_t ns = now_ns() - t0;
    sc->pw_waits++;
    if (r == VK_TIMEOUT) sc->pw_timeouts++;
    sc->pw_wait_ns += ns;
    if (ns > sc->pw_wait_max_ns) sc->pw_wait_max_ns = ns;
    if (--sc->pw_waiters == 0 && sc->pw_closing) pthread_cond_broadcast(&sc->pw_cond);
    pthread_mutex_unlock(&sc->lock);
    if (waited && vkb_hitch_active) vkb_hitch_waited(VKB_HITCH_W_PRESENT, VKB_CMD_COUNT, ns); /* the per-hitch line (hitch.c) */
    return r;
}

VKAPI_ATTR VkResult VKAPI_CALL vkb_ep_vkWaitForPresentKHR(VkDevice device, VkSwapchainKHR swapchain, uint64_t presentId, uint64_t timeout)
{
    return vkb_note_error("vkWaitForPresentKHR", wait_for_present(device, swapchain, presentId, timeout));
}

VKAPI_ATTR VkResult VKAPI_CALL vkb_ep_vkWaitForPresent2KHR(VkDevice device, VkSwapchainKHR swapchain, const VkPresentWait2InfoKHR *pPresentWait2Info)
{
    return vkb_note_error("vkWaitForPresent2KHR", wait_for_present(device, swapchain, pPresentWait2Info->presentId, pPresentWait2Info->timeout));
}

/* ------------------------------------------------------------------ headless */

static VkResult hl_init(vkb_swapchain *sc) { (void)sc; return VK_SUCCESS; }
static VkResult hl_present(vkb_swapchain *sc, uint32_t i, const VkPresentRegionKHR *r) { (void)sc; (void)i; (void)r; return VK_SUCCESS; }
static VkResult hl_wait(vkb_swapchain *sc, uint64_t t) { (void)sc; (void)t; return VK_TIMEOUT; }
static void hl_destroy(vkb_swapchain *sc) { (void)sc; }
const vkb_wsi_backend vkb_wsi_headless_backend = {hl_init, hl_present, hl_wait, hl_destroy};
