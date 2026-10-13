/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Feature emulation: entry point. What the GPU lacks and the bridge can make up for is decided
 * here per physical device; the client reports those features as present, the server strips
 * them from vkCreateDevice and installs wrappers in the device's dispatch table.
 */
#define _GNU_SOURCE
#include "vkb_server.h"
#include "server_emu.h"

#include <stdlib.h>
#include <string.h>

static int token(const char *list, const char *t)
{
    size_t n = strlen(t);
    for (const char *p = list; p && (p = strstr(p, t)); p += n)
        if ((p == list || p[-1] == ',') && (p[n] == ',' || p[n] == 0)) return 1;
    return 0;
}

int vkb_emu_faked(const char *t)
{
    const char *fake = getenv("VKBRIDGE_FAKE_MISSING");
    return fake && token(fake, t);
}

static int env_off(const char *name)
{
    const char *v = getenv(name);
    return v && !strcmp(v, "0");
}

uint32_t vkb_emu_detect(const vkb_dispatch *idt, VkPhysicalDevice pd, uint32_t *missing)
{
    VkPhysicalDeviceFeatures f;
    idt->vkGetPhysicalDeviceFeatures(pd, &f);
    uint32_t n = 0;
    idt->vkEnumerateDeviceExtensionProperties(pd, NULL, &n, NULL);
    VkExtensionProperties *e = calloc(n ? n : 1, sizeof(*e));
    idt->vkEnumerateDeviceExtensionProperties(pd, NULL, &n, e);
    int divisor = 0, depth_clip = 0, maint5 = 0, robust2 = 0, maint6 = 0, lson_khr = 0, lson_ext = 0, push = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (!strcmp(e[i].extensionName, "VK_EXT_vertex_attribute_divisor") ||
            !strcmp(e[i].extensionName, "VK_KHR_vertex_attribute_divisor"))
            divisor = 1;
        if (!strcmp(e[i].extensionName, "VK_EXT_depth_clip_enable")) depth_clip = 1;
        if (!strcmp(e[i].extensionName, "VK_KHR_maintenance5")) maint5 = 1;
        if (!strcmp(e[i].extensionName, "VK_EXT_robustness2") || !strcmp(e[i].extensionName, "VK_KHR_robustness2")) robust2 = 1;
        if (!strcmp(e[i].extensionName, "VK_KHR_maintenance6")) maint6 = 1;
        if (!strcmp(e[i].extensionName, "VK_KHR_push_descriptor")) push = 1;
        if (!strcmp(e[i].extensionName, "VK_KHR_load_store_op_none")) lson_khr = 1;
        if (!strcmp(e[i].extensionName, "VK_EXT_load_store_op_none")) lson_ext = 1;
    }
    free(e);
    /* VKBRIDGE_FAKE_MISSING=bc,divisor,clip,cull,depthclip pretends the GPU lacks them (host tests). */
    const char *fake = getenv("VKBRIDGE_FAKE_MISSING");
    if (fake) {
        if (token(fake, "bc")) f.textureCompressionBC = VK_FALSE;
        if (token(fake, "divisor")) divisor = 0;
        if (token(fake, "clip")) f.shaderClipDistance = VK_FALSE;
        if (token(fake, "cull")) f.shaderCullDistance = VK_FALSE;
        if (token(fake, "depthclip")) depth_clip = 0;
        if (token(fake, "maint5")) maint5 = 0;
        if (token(fake, "robustness2")) robust2 = 0;
        if (token(fake, "dualsrc")) f.dualSrcBlend = VK_FALSE;
        if (token(fake, "fillmode")) f.fillModeNonSolid = VK_FALSE;
        if (token(fake, "multiviewport")) f.multiViewport = VK_FALSE;
        if (token(fake, "maint6")) maint6 = 0;
        if (token(fake, "push")) push = 0;
    }
    uint32_t emu = 0;
    VkPhysicalDeviceProperties props;
    idt->vkGetPhysicalDeviceProperties(pd, &props);
    /* Vulkan 1.4 has maintenance5 in core. */
    if (!maint5 && (props.apiVersion < VK_API_VERSION_1_4 || (fake && token(fake, "maint5"))) && !env_off("VKBRIDGE_EMU_MAINT5"))
        emu |= VKB_EMU_MAINT5;
    if (!f.textureCompressionBC && vkb_emu_bcn_supported(idt, pd) && !env_off("VKBRIDGE_EMU_BCN")) emu |= VKB_EMU_BCN;
    if (!divisor && !env_off("VKBRIDGE_EMU_DIVISOR")) emu |= VKB_EMU_DIVISOR;
    if (!f.shaderClipDistance && !env_off("VKBRIDGE_EMU_CLIP")) emu |= VKB_EMU_CLIP_DISTANCE;
    if (!f.shaderCullDistance && !env_off("VKBRIDGE_EMU_CULL")) emu |= VKB_EMU_CULL_DISTANCE;
    if (!depth_clip && f.depthClamp && !env_off("VKBRIDGE_EMU_DEPTHCLIP")) emu |= VKB_EMU_DEPTH_CLIP;
    /* DXVK will not start without robustness2. Mali r44 lacks it; GameNative's wrapper reports it
     * (robustBufferAccess2, robustImageAccess2, nullDescriptor) without enforcing it, and DXVK
     * games run - out-of-bounds reads and null descriptors are then up to the driver. */
    if (!robust2 && f.robustBufferAccess && !env_off("VKBRIDGE_EMU_ROBUSTNESS2")) emu |= VKB_EMU_ROBUSTNESS2;
    /* dma-buf export of images (gamescope's nested backend needs it): emulated with dma-heap pages
     * where the driver imports dma-bufs into images but exports none (Mali). */
    if (!env_off("VKBRIDGE_EMU_DMABUF_EXPORT") && vkb_mem_dmaheap_available()) {
        int can_export = 0, can_import = 0;
        if (idt->vkGetPhysicalDeviceImageFormatProperties2) {
            /* Asked as gamescope asks: a linear DRM modifier (Mali answers import questions only so). */
            VkPhysicalDeviceImageDrmFormatModifierInfoEXT mi = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT, NULL,
                                                                0 /* DRM_FORMAT_MOD_LINEAR */, VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
            VkPhysicalDeviceExternalImageFormatInfo ei = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO, &mi,
                                                          VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
            VkPhysicalDeviceImageFormatInfo2 ii = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2, &ei, VK_FORMAT_R8G8B8A8_UNORM,
                                                   VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
                                                   VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, 0};
            VkExternalImageFormatProperties ep = {VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
            VkImageFormatProperties2 ip = {VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, &ep};
            if (idt->vkGetPhysicalDeviceImageFormatProperties2(pd, &ii, &ip) == VK_SUCCESS) {
                VkExternalMemoryFeatureFlags ef = ep.externalMemoryProperties.externalMemoryFeatures;
                can_export = (ef & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT) != 0;
                can_import = (ef & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) != 0;
            }
        }
        if (fake && token(fake, "dmabufexport")) can_export = 0;
        if (!can_export && can_import) emu |= VKB_EMU_DMABUF_EXPORT;
    }
    /* OPAQUE_FD external memory on dma-bufs (server_opaque.c): DXVK's D3D11 shared resources. */
    if (!env_off("VKBRIDGE_EMU_OPAQUEFD") && vkb_opaque_wanted(idt, pd, emu)) emu |= VKB_EMU_OPAQUE_FD;
    /* What DXVK 3 requires and the GPU lacks (server_emu_compat.c). Vulkan 1.4 has maintenance6. */
    if (!f.dualSrcBlend && !env_off("VKBRIDGE_EMU_DUALSRC")) emu |= VKB_EMU_DUAL_SRC;
    if (!f.fillModeNonSolid && !env_off("VKBRIDGE_EMU_FILLMODE")) emu |= VKB_EMU_FILL_MODE;
    if (!f.multiViewport && !env_off("VKBRIDGE_EMU_MULTIVIEWPORT")) emu |= VKB_EMU_MULTI_VIEWPORT;
    if (!maint6 && (props.apiVersion < VK_API_VERSION_1_4 || (fake && token(fake, "maint6"))) && !env_off("VKBRIDGE_EMU_MAINT6"))
        emu |= VKB_EMU_MAINT6;
    if (!lson_khr && lson_ext) emu |= VKB_EMU_LOADSTORE_NONE;
    /* vkd3d-proton (every D3D12 game) refuses a device without push descriptors (Vulkan 1.4 has them). */
    if (!push && (props.apiVersion < VK_API_VERSION_1_4 || (fake && token(fake, "push"))) && !env_off("VKBRIDGE_EMU_PUSH")) emu |= VKB_EMU_PUSH_DESC;
    /* vkd3d-proton 3 refuses a device whose storage texel buffers need more than texel alignment
     * (Deep Rock Galactic DX12, 2026-10-11: "Lacking support for single texel alignment"). Mali
     * r44 wants 64 bytes for storage (uniform is fine). Reported as supported; D3D12 views mostly
     * start at 0 or on large boundaries, and a misaligned one is logged (server_emu_layer.c). */
    int texel_single = 1;
    if (idt->vkGetPhysicalDeviceProperties2 && props.apiVersion >= VK_API_VERSION_1_3) {
        VkPhysicalDeviceTexelBufferAlignmentProperties ta = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TEXEL_BUFFER_ALIGNMENT_PROPERTIES};
        VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &ta};
        idt->vkGetPhysicalDeviceProperties2(pd, &p2);
        texel_single = ta.storageTexelBufferOffsetSingleTexelAlignment || ta.storageTexelBufferOffsetAlignmentBytes == 1;
    }
    if (fake && token(fake, "texelalign")) texel_single = 0;
    if (!texel_single && !env_off("VKBRIDGE_EMU_TEXELALIGN")) emu |= VKB_EMU_TEXEL_ALIGN;
    /* vkd3d-proton's bindless path without descriptor buffers wants 1,000,000 sampled images,
     * storage images and storage buffers per stage after bind (Deep Rock Galactic DX12,
     * 2026-10-11: "Insufficient descriptor indexing support"); Mali r44 allows 500,000. Reported;
     * what reaches the driver is cut to its limits (server_emu_desc.c). */
    int desc_short = 0;
    if (props.apiVersion >= VK_API_VERSION_1_2) {
        uint32_t dl[2][3];
        vkb_emu_desc_limits_get(idt, pd, dl);
        for (int i = 0; i < 3; i++) desc_short |= dl[1][i] && dl[1][i] < 1000000;
    }
    if (desc_short && !env_off("VKBRIDGE_EMU_DESCLIMITS")) emu |= VKB_EMU_DESC_LIMITS;
    /* PointSize in geometry/tessellation stages: stripped when the feature is missing. */
    if (!f.shaderTessellationAndGeometryPointSize && (f.geometryShader || f.tessellationShader)) emu |= VKB_EMU_POINT_SIZE;
    uint32_t miss = 0;
    if (!f.textureCompressionBC) miss |= VKB_EMU_BCN;
    if (!divisor) miss |= VKB_EMU_DIVISOR;
    if (!f.shaderClipDistance) miss |= VKB_EMU_CLIP_DISTANCE;
    if (!f.shaderCullDistance) miss |= VKB_EMU_CULL_DISTANCE;
    if (!depth_clip) miss |= VKB_EMU_DEPTH_CLIP;
    if (emu & VKB_EMU_MAINT5) miss |= VKB_EMU_MAINT5;
    if (!robust2) miss |= VKB_EMU_ROBUSTNESS2;
    if (!f.dualSrcBlend) miss |= VKB_EMU_DUAL_SRC;
    if (!f.fillModeNonSolid) miss |= VKB_EMU_FILL_MODE;
    if (!f.multiViewport) miss |= VKB_EMU_MULTI_VIEWPORT;
    if (emu & VKB_EMU_MAINT6) miss |= VKB_EMU_MAINT6;
    if (emu & VKB_EMU_PUSH_DESC) miss |= VKB_EMU_PUSH_DESC;
    if (!texel_single) miss |= VKB_EMU_TEXEL_ALIGN;
    if (desc_short) miss |= VKB_EMU_DESC_LIMITS;
    *missing = miss;
    return emu & VKB_EMU_IMPLEMENTED;
}

/* Remove a structure from a pNext chain (the chain lives in the request's arena). */
static void chain_remove(VkDeviceCreateInfo *ci, VkStructureType t)
{
    VkBaseOutStructure *prev = (VkBaseOutStructure *)ci;
    while (prev->pNext) {
        if (prev->pNext->sType == t) prev->pNext = prev->pNext->pNext;
        else prev = prev->pNext;
    }
}

static VkPhysicalDeviceFeatures *find_features(VkDeviceCreateInfo *ci)
{
    if (ci->pEnabledFeatures) return (VkPhysicalDeviceFeatures *)ci->pEnabledFeatures;
    for (VkBaseOutStructure *b = (VkBaseOutStructure *)ci->pNext; b; b = b->pNext)
        if (b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2) return &((VkPhysicalDeviceFeatures2 *)b)->features;
    return NULL;
}

void vkb_emu_device_create_info(vkb_srv_table *inst, VkPhysicalDevice pd, uint32_t emu, VkDeviceCreateInfo *ci, vkb_arena *a)
{
    VkPhysicalDeviceFeatures real;
    inst->real.vkGetPhysicalDeviceFeatures(pd, &real);
    VkPhysicalDeviceFeatures *f = find_features(ci);
    if ((emu & (VKB_EMU_BCN | VKB_EMU_DEPTH_CLIP)) && !f) {
        /* The BC decoder writes R8/RG8 storage images: it needs this feature on, so the request
         * gets a features structure if it had none. */
        f = vkb_arena_alloc(a, sizeof(*f));
        ci->pEnabledFeatures = f;
    }
    if (f) {
        /* pEnabledFeatures came from the request: it is ours to change. Anything the client asked
         * for that the GPU does not have is either emulated or would fail the call. */
        VkBool32 *want = (VkBool32 *)f;
        const VkBool32 *have = (const VkBool32 *)&real;
        for (size_t i = 0; i < sizeof(*f) / sizeof(VkBool32); i++) {
            if (want[i] && !have[i]) want[i] = VK_FALSE;
        }
        if (emu & VKB_EMU_BCN) f->shaderStorageImageExtendedFormats = VK_TRUE;
        if (emu & VKB_EMU_DEPTH_CLIP) f->depthClamp = VK_TRUE;
    }
    if (emu & VKB_EMU_DIVISOR) chain_remove(ci, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT);
    if (emu & VKB_EMU_DEPTH_CLIP) chain_remove(ci, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT);
    if (emu & VKB_EMU_MAINT5) chain_remove(ci, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES);
    if (emu & VKB_EMU_MAINT6) chain_remove(ci, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_6_FEATURES);
    if (emu & VKB_EMU_ROBUSTNESS2) {
        chain_remove(ci, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT);
        /* What robustness the GPU has stays on; robustBufferAccess2 asks for at least that. */
        VkPhysicalDeviceFeatures *rf = find_features(ci);
        const char *nr = vkb_srv_env("VKBRIDGE_NO_FORCED_ROBUSTNESS");
        if (rf && !(nr && *nr && strcmp(nr, "0"))) rf->robustBufferAccess = VK_TRUE;
    }
}

void vkb_emu_device_init(vkb_srv_table *dev, uint32_t emu)
{
    if (!emu) return;
    vkb_emu_device *e = calloc(1, sizeof(*e));
    e->flags = emu;
    e->dev = dev;
    pthread_mutex_init(&e->lock, NULL);
    dev->emu = e;
    vkb_emu_install(dev);
}

void vkb_emu_device_destroy(vkb_srv_table *dev)
{
    if (!dev->emu) return;
    vkb_emu_uninstall(dev);
    pthread_mutex_destroy(&dev->emu->lock);
    free(dev->emu);
    dev->emu = NULL;
}
