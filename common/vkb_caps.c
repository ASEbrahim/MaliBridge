/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include "vkb_caps.h"
#include "vkb_emu_formats.h"
#include "vkb_ext_alias.h"
#include "vkb_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CAP_EMU(c, f)     (((c)->emu & (f)) != 0)
#define CAP_MISSING(c, f) (((c)->missing & (f)) != 0 && !CAP_EMU(c, f))

#define DESC_LIMIT (1u << 20)
#define RAISE(x, v) do { if ((x) < (v)) (x) = (v); } while (0)

#define raise_uab(d)                                                                        \
    do {                                                                                    \
        RAISE((d)->maxPerStageDescriptorUpdateAfterBindSampledImages, DESC_LIMIT);          \
        RAISE((d)->maxPerStageDescriptorUpdateAfterBindStorageImages, DESC_LIMIT);          \
        RAISE((d)->maxPerStageDescriptorUpdateAfterBindStorageBuffers, DESC_LIMIT);         \
        RAISE((d)->maxDescriptorSetUpdateAfterBindSampledImages, DESC_LIMIT);               \
        RAISE((d)->maxDescriptorSetUpdateAfterBindStorageImages, DESC_LIMIT);               \
        RAISE((d)->maxDescriptorSetUpdateAfterBindStorageBuffers, DESC_LIMIT);              \
        RAISE((d)->maxPerStageUpdateAfterBindResources, 4 * DESC_LIMIT);                    \
    } while (0)

static int ext_in_list(const VkExtensionProperties *list, uint32_t count, const char *name)
{
    for (uint32_t i = 0; i < count; i++)
        if (!strcmp(list[i].extensionName, name))
            return 1;
    return 0;
}

static int name_in_list(const char *const *list, uint32_t count, const char *name)
{
    for (uint32_t i = 0; i < count; i++)
        if (!strcmp(list[i], name))
            return 1;
    return 0;
}

uint32_t vkb_caps_spoof_gpu(const char *value, int *ok)
{
    if (ok) *ok = 1;
    if (!value || !*value) return 0;
    if (!strcmp(value, "nvidia")) return VKB_SPOOF_NVIDIA;
    if (!strcmp(value, "amd")) return VKB_SPOOF_AMD;
    if (!strcmp(value, "off") || !strcmp(value, "0")) return 0;
    if (ok) *ok = 0;
    return 0;
}

uint32_t vkb_caps_heap_max_mb(const char *value, int *ok)
{
    if (ok) *ok = 1;
    if (!value || !*value) return 0;
    char *end;
    unsigned long long mb = strtoull(value, &end, 10);
    if (*end || *value == '-') {
        if (ok) *ok = 0;
        return 0;
    }
    return (uint32_t)mb;
}

const VkExtensionProperties *vkb_caps_emu_extensions(const vkb_pd_caps *caps, uint32_t *n)
{
    static __thread VkExtensionProperties list[16];
    uint32_t c = 0;
    if (CAP_EMU(caps, VKB_EMU_MAINT5))
        list[c++] = (VkExtensionProperties){"VK_KHR_maintenance5", 1};
    if (CAP_EMU(caps, VKB_EMU_DIVISOR)) {
        list[c++] = (VkExtensionProperties){"VK_EXT_vertex_attribute_divisor", 3};
        list[c++] = (VkExtensionProperties){"VK_KHR_vertex_attribute_divisor", 1};
    }
    if (CAP_EMU(caps, VKB_EMU_DEPTH_CLIP))
        list[c++] = (VkExtensionProperties){"VK_EXT_depth_clip_enable", 1};
    if (CAP_EMU(caps, VKB_EMU_ROBUSTNESS2))
        list[c++] = (VkExtensionProperties){"VK_EXT_robustness2", 1};
    if (CAP_EMU(caps, VKB_EMU_MAINT6))
        list[c++] = (VkExtensionProperties){"VK_KHR_maintenance6", 1};
    if (CAP_EMU(caps, VKB_EMU_LOADSTORE_NONE))
        list[c++] = (VkExtensionProperties){"VK_KHR_load_store_op_none", 1};
    if (CAP_EMU(caps, VKB_EMU_PUSH_DESC))
        list[c++] = (VkExtensionProperties){"VK_KHR_push_descriptor", 2};
    if (CAP_EMU(caps, VKB_EMU_DYN_RENDER))
        list[c++] = (VkExtensionProperties){"VK_KHR_dynamic_rendering", 1};
    *n = c;
    return list;
}

uint32_t vkb_caps_compute_aliased_mask(const VkExtensionProperties *avail, uint32_t avail_count)
{
    uint32_t mask = 0;
    for (uint32_t i = 0; i < VKB_EXT_ALIAS_COUNT; i++) {
        int have_app    = ext_in_list(avail, avail_count, vkb_ext_aliases[i].app);
        int have_driver = ext_in_list(avail, avail_count, vkb_ext_aliases[i].driver);
        if (!have_app && have_driver)
            mask |= (1u << i);
    }
    return mask;
}

uint32_t vkb_caps_append_device_extensions(
    const vkb_pd_caps *caps,
    VkExtensionProperties *exts, uint32_t count, uint32_t max_cap)
{
    /* If VKB_EMU_CLIP_DISTANCE is active, remove GPL and shader_module_identifier
     * so vertex and fragment stages are compiled together */
    if (CAP_EMU(caps, VKB_EMU_CLIP_DISTANCE)) {
        uint32_t w = 0;
        for (uint32_t i = 0; i < count; i++) {
            if (!strcmp(exts[i].extensionName, "VK_EXT_graphics_pipeline_library") ||
                !strcmp(exts[i].extensionName, "VK_EXT_shader_module_identifier"))
                continue;
            exts[w++] = exts[i];
        }
        count = w;
    }

    uint32_t nemu = 0;
    const VkExtensionProperties *emu = vkb_caps_emu_extensions(caps, &nemu);
    for (uint32_t i = 0; i < nemu && count < max_cap; i++) {
        if (!ext_in_list(exts, count, emu[i].extensionName))
            exts[count++] = emu[i];
    }

    for (uint32_t i = 0; i < VKB_EXT_ALIAS_COUNT && count < max_cap; i++) {
        if (!(caps->aliased & (1u << i)))
            continue;
        if (ext_in_list(exts, count, vkb_ext_aliases[i].app))
            continue;
        VkExtensionProperties *e = &exts[count++];
        snprintf(e->extensionName, sizeof(e->extensionName), "%s", vkb_ext_aliases[i].app);
        e->specVersion = vkb_ext_aliases[i].spec;
    }
    return count;
}

uint32_t vkb_caps_filter_device_extensions(
    uint32_t emu,
    const VkExtensionProperties *avail, uint32_t avail_count,
    const char *const *requested, uint32_t requested_count,
    const char **out, uint32_t max_out,
    vkb_caps_ext_faked_fn faked, void *user,
    char *not_passed, size_t not_passed_size)
{
    uint32_t n = 0;
    if (not_passed && not_passed_size) not_passed[0] = 0;
    for (uint32_t i = 0; i < requested_count; i++) {
        const char *req = requested[i];
        int missing = !ext_in_list(avail, avail_count, req) ||
                      (faked && faked(req, user));
        if (!missing) {
            if (!name_in_list(out, n, req) && n < max_out) out[n++] = req;
            continue;
        }
        /* An EXT name the driver has only as KHR (vkb_ext_alias.h): the KHR one in its place. */
        const char *drv = NULL;
        for (uint32_t a = 0; a < VKB_EXT_ALIAS_COUNT && !drv; a++)
            if (!strcmp(vkb_ext_aliases[a].app, req) && ext_in_list(avail, avail_count, vkb_ext_aliases[a].driver))
                drv = vkb_ext_aliases[a].driver;
        if (drv) {
            VKB_INFO("device extension %s is enabled as %s", req, drv);
            if (!name_in_list(out, n, drv) && n < max_out) out[n++] = drv;
            continue;
        }
        /* Emulated or client-side: the driver must not see it. */
        VKB_DBG("device extension %s is emulated or client-side; not passed to the driver", req);
        if (not_passed && not_passed_size) {
            size_t nl = strlen(not_passed);
            if (nl + strlen(req) + 2 < not_passed_size)
                snprintf(not_passed + nl, not_passed_size - nl, "%s%s", nl ? " " : "", req);
        }
    }
    /* The BC decoder binds its buffer and image with push descriptors. */
    if ((emu & VKB_EMU_BCN) && ext_in_list(avail, avail_count, "VK_KHR_push_descriptor") &&
        !name_in_list(out, n, "VK_KHR_push_descriptor") && n < max_out)
        out[n++] = "VK_KHR_push_descriptor";
    return n;
}

void vkb_caps_filter_device_create_extensions(
    uint32_t emu,
    const VkExtensionProperties *avail, uint32_t avail_count,
    VkDeviceCreateInfo *ci, vkb_arena *arena,
    vkb_caps_ext_faked_fn faked, void *user)
{
    uint32_t max_out = ci->enabledExtensionCount + 2;
    const char **out = vkb_arena_alloc(arena, max_out * sizeof(const char *));
    if (!out) return;
    uint32_t n = vkb_caps_filter_device_extensions(emu, avail, avail_count,
                                                   ci->ppEnabledExtensionNames, ci->enabledExtensionCount,
                                                   out, max_out, faked, user, NULL, 0);
    ci->enabledExtensionCount = n;
    ci->ppEnabledExtensionNames = out;
}

void vkb_caps_patch_properties(const vkb_pd_caps *caps, VkPhysicalDeviceProperties *p,
                               uint32_t max_api)
{
    if (max_api && p->apiVersion > max_api)
        p->apiVersion = max_api;

    if (caps->spoof_gpu == VKB_SPOOF_NVIDIA) {
        p->vendorID = 0x10de;
        p->deviceID = 0x1c03;
        snprintf(p->deviceName, sizeof(p->deviceName), "NVIDIA GeForce GTX 1060 6GB");
    } else if (caps->spoof_gpu == VKB_SPOOF_AMD) {
        p->vendorID = 0x1002;
        p->deviceID = 0x67df;
        snprintf(p->deviceName, sizeof(p->deviceName), "AMD Radeon RX 480 Graphics");
    }

    if (CAP_EMU(caps, VKB_EMU_DESC_LIMITS)) {
        VkPhysicalDeviceLimits *l = &p->limits;
        RAISE(l->maxPerStageDescriptorSampledImages, DESC_LIMIT);
        RAISE(l->maxPerStageDescriptorStorageImages, DESC_LIMIT);
        RAISE(l->maxPerStageDescriptorStorageBuffers, DESC_LIMIT);
        RAISE(l->maxDescriptorSetSampledImages, DESC_LIMIT);
        RAISE(l->maxDescriptorSetStorageImages, DESC_LIMIT);
        RAISE(l->maxDescriptorSetStorageBuffers, DESC_LIMIT);
        RAISE(l->maxPerStageResources, 4 * DESC_LIMIT);
    }
    if (CAP_EMU(caps, VKB_EMU_MULTI_VIEWPORT) && p->limits.maxViewports < 16)
        p->limits.maxViewports = 16;
    if (CAP_EMU(caps, VKB_EMU_DUAL_SRC) && p->limits.maxFragmentDualSrcAttachments < 1)
        p->limits.maxFragmentDualSrcAttachments = 1;
    if (CAP_EMU(caps, VKB_EMU_MULTI_DRAW))
        p->limits.maxDrawIndirectCount = UINT32_MAX;
}

static int is_aliased(const vkb_pd_caps *caps, const char *app)
{
    for (uint32_t i = 0; i < VKB_EXT_ALIAS_COUNT; i++)
        if ((caps->aliased & (1u << i)) && !strcmp(vkb_ext_aliases[i].app, app))
            return 1;
    return 0;
}

void vkb_caps_patch_properties_chain(const vkb_pd_caps *caps, VkPhysicalDevice pd,
                                     VkBaseOutStructure *b,
                                     vkb_caps_get_props2_fn get_props2, void *user)
{
    if (b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_PROPERTIES_EXT)
        ((VkPhysicalDeviceMapMemoryPlacedPropertiesEXT *)b)->minPlacedMemoryMapAlignment =
            (VkDeviceSize)sysconf(_SC_PAGESIZE);

    if (CAP_EMU(caps, VKB_EMU_DESC_LIMITS)) {
        if (b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES)
            raise_uab((VkPhysicalDeviceVulkan12Properties *)b);
        if (b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES)
            raise_uab((VkPhysicalDeviceDescriptorIndexingProperties *)b);
    }
    if (CAP_EMU(caps, VKB_EMU_PUSH_DESC) &&
        b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PUSH_DESCRIPTOR_PROPERTIES) {
        ((VkPhysicalDevicePushDescriptorProperties *)b)->maxPushDescriptors = 32;
    }
    if (CAP_EMU(caps, VKB_EMU_TEXEL_ALIGN)) {
        if (b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES)
            ((VkPhysicalDeviceVulkan13Properties *)b)->storageTexelBufferOffsetSingleTexelAlignment = VK_TRUE;
        if (b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TEXEL_BUFFER_ALIGNMENT_PROPERTIES)
            ((VkPhysicalDeviceTexelBufferAlignmentProperties *)b)->storageTexelBufferOffsetSingleTexelAlignment = VK_TRUE;
    }
    if (CAP_EMU(caps, VKB_EMU_MAINT6) &&
        b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_6_PROPERTIES) {
        VkPhysicalDeviceMaintenance6Properties *m = (void *)b;
        m->blockTexelViewCompatibleMultipleLayers = VK_FALSE;
        m->maxCombinedImageSamplerDescriptorCount = 3;
        m->fragmentShadingRateClampCombinerInputs = VK_FALSE;
    }
    if (CAP_EMU(caps, VKB_EMU_ROBUSTNESS2) &&
        b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_PROPERTIES_EXT) {
        VkPhysicalDeviceRobustness2PropertiesEXT *r = (void *)b;
        r->robustStorageBufferAccessSizeAlignment = 4;
        r->robustUniformBufferAccessSizeAlignment = 256;
    }
    if (CAP_EMU(caps, VKB_EMU_DIVISOR)) {
        if (b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_PROPERTIES_EXT)
            ((VkPhysicalDeviceVertexAttributeDivisorPropertiesEXT *)b)->maxVertexAttribDivisor = UINT32_MAX;
        if (b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_PROPERTIES) {
            VkPhysicalDeviceVertexAttributeDivisorProperties *p = (void *)b;
            p->maxVertexAttribDivisor = UINT32_MAX;
            p->supportsNonZeroFirstInstance = VK_TRUE;
        }
    } else if (b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_PROPERTIES_EXT &&
               is_aliased(caps, "VK_EXT_vertex_attribute_divisor") && get_props2) {
        VkPhysicalDeviceVertexAttributeDivisorProperties k = {
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_PROPERTIES};
        VkPhysicalDeviceProperties2 p2 = {
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &k};
        get_props2(pd, &p2, user);
        ((VkPhysicalDeviceVertexAttributeDivisorPropertiesEXT *)b)->maxVertexAttribDivisor =
            k.maxVertexAttribDivisor;
    }
}

static void set_feature(const vkb_pd_caps *caps, uint32_t flag, VkBool32 *f)
{
    if (CAP_EMU(caps, flag)) *f = VK_TRUE;
    else if (CAP_MISSING(caps, flag)) *f = VK_FALSE;
}

void vkb_caps_patch_features(const vkb_pd_caps *caps, VkPhysicalDeviceFeatures *f)
{
    set_feature(caps, VKB_EMU_BCN, &f->textureCompressionBC);
    set_feature(caps, VKB_EMU_CLIP_DISTANCE, &f->shaderClipDistance);
    set_feature(caps, VKB_EMU_CULL_DISTANCE, &f->shaderCullDistance);
    set_feature(caps, VKB_EMU_DUAL_SRC, &f->dualSrcBlend);
    set_feature(caps, VKB_EMU_FILL_MODE, &f->fillModeNonSolid);
    set_feature(caps, VKB_EMU_MULTI_VIEWPORT, &f->multiViewport);
    set_feature(caps, VKB_EMU_MULTI_DRAW, &f->multiDrawIndirect);
}

void vkb_caps_patch_features_chain(const vkb_pd_caps *caps, VkBaseOutStructure *b,
                                   int is_remote_bridge, bool no_sharing)
{
    switch ((int)b->sType) {
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES:
        if (CAP_EMU(caps, VKB_EMU_MAINT5))
            ((VkPhysicalDeviceMaintenance5Features *)b)->maintenance5 = VK_TRUE;
        break;
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_6_FEATURES:
        if (CAP_EMU(caps, VKB_EMU_MAINT6))
            ((VkPhysicalDeviceMaintenance6Features *)b)->maintenance6 = VK_TRUE;
        break;
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES: {
        VkPhysicalDeviceVertexAttributeDivisorFeatures *d = (void *)b;
        set_feature(caps, VKB_EMU_DIVISOR, &d->vertexAttributeInstanceRateDivisor);
        set_feature(caps, VKB_EMU_DIVISOR, &d->vertexAttributeInstanceRateZeroDivisor);
        break;
    }
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT:
        if (CAP_MISSING(caps, VKB_EMU_DEPTH_CLIP) || CAP_EMU(caps, VKB_EMU_DEPTH_CLIP)) {
            VkPhysicalDeviceExtendedDynamicState3FeaturesEXT *d3 = (void *)b;
            d3->extendedDynamicState3DepthClipEnable = VK_FALSE;
            if (CAP_EMU(caps, VKB_EMU_DEPTH_CLIP))
                d3->extendedDynamicState3DepthClampEnable = VK_FALSE;
        }
        break;
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT:
        if (CAP_EMU(caps, VKB_EMU_ROBUSTNESS2)) {
            VkPhysicalDeviceRobustness2FeaturesEXT *r = (void *)b;
            r->robustBufferAccess2 = r->robustImageAccess2 = r->nullDescriptor = VK_TRUE;
        } else if (CAP_EMU(caps, VKB_EMU_ROBUST2_ACCESS)) {
            /* the driver's null descriptors, its core robustness for the rest (server_emu.c) */
            VkPhysicalDeviceRobustness2FeaturesEXT *r = (void *)b;
            r->robustBufferAccess2 = r->robustImageAccess2 = VK_TRUE;
        }
        break;
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES:
        set_feature(caps, VKB_EMU_DYN_RENDER,
                    &((VkPhysicalDeviceDynamicRenderingFeatures *)b)->dynamicRendering);
        break;
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES:
        set_feature(caps, VKB_EMU_DYN_RENDER,
                    &((VkPhysicalDeviceVulkan13Features *)b)->dynamicRendering);
        break;
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT:
        set_feature(caps, VKB_EMU_DEPTH_CLIP,
                    &((VkPhysicalDeviceDepthClipEnableFeaturesEXT *)b)->depthClipEnable);
        break;
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES: {
        VkPhysicalDeviceVulkan14Features *v = (void *)b;
        if (is_remote_bridge)
            v->hostImageCopy = VK_FALSE;
        if (CAP_EMU(caps, VKB_EMU_MAINT6))
            v->maintenance6 = VK_TRUE;
        set_feature(caps, VKB_EMU_DIVISOR, &v->vertexAttributeInstanceRateDivisor);
        set_feature(caps, VKB_EMU_DIVISOR, &v->vertexAttributeInstanceRateZeroDivisor);
        break;
    }
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_FEATURES_EXT: {
        /* Whole allocations only: memory.c moves the client's mapping to the address. */
        VkPhysicalDeviceMapMemoryPlacedFeaturesEXT *m = (void *)b;
        m->memoryMapPlaced = m->memoryUnmapReserve = no_sharing;
        m->memoryMapRangePlaced = VK_FALSE;
        return;
    }
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_IMAGE_COPY_FEATURES:
        if (is_remote_bridge)
            ((VkPhysicalDeviceHostImageCopyFeatures *)b)->hostImageCopy = VK_FALSE;
        break;
    default:
        break;
    }
}

int vkb_caps_format_properties2(const vkb_pd_caps *caps, VkPhysicalDevice pd,
                                VkFormat format, VkFormatProperties *fp,
                                VkFormatProperties2 *fp2,
                                vkb_caps_get_fmt_fn get_fmt,
                                vkb_caps_get_fmt2_fn get_fmt2, void *user)
{
    if ((vkb_bgr10_rgba(format) != VK_FORMAT_UNDEFINED && CAP_EMU(caps, VKB_EMU_BGR10_VTX)) ||
        (format == VK_FORMAT_B10G11R11_UFLOAT_PACK32 && CAP_EMU(caps, VKB_EMU_UF11_VTX))) {
        /* Vertex fetch: done as the A2B10G10R10 format with red and blue swapped, or as R32_UINT
         * unpacked, in the vertex shader (server_emu_shader.c), exact also where the driver has it;
         * everything else as the driver says. */
        if (fp2 && get_fmt2) get_fmt2(pd, format, fp2, user);
        else if (get_fmt) get_fmt(pd, format, fp, user);
        fp->bufferFeatures |= VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT;
        for (VkBaseOutStructure *b = fp2 ? (VkBaseOutStructure *)fp2->pNext : NULL; b; b = b->pNext)
            if (b->sType == VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3)
                ((VkFormatProperties3 *)b)->bufferFeatures |= VK_FORMAT_FEATURE_2_VERTEX_BUFFER_BIT;
        return 1;
    }
    if (!vkb_is_bc(format) || !(caps->missing & VKB_EMU_BCN))
        return 0;
    memset(fp, 0, sizeof(*fp));
    if (CAP_EMU(caps, VKB_EMU_BCN) && get_fmt) {
        VkFormatProperties real = {0};
        get_fmt(pd, vkb_bc_view_format(format), &real, user);
        fp->optimalTilingFeatures = real.optimalTilingFeatures &
            (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
             VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
             VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
             VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
             VK_FORMAT_FEATURE_BLIT_SRC_BIT |
             VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_MINMAX_BIT);
    }
    if (fp2) {
        for (VkBaseOutStructure *b = (VkBaseOutStructure *)fp2->pNext; b; b = b->pNext) {
            if (b->sType == VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3) {
                VkFormatProperties3 *p3 = (void *)b;
                p3->linearTilingFeatures = 0;
                p3->optimalTilingFeatures = fp->optimalTilingFeatures;
                p3->bufferFeatures = 0;
            } else if (b->sType == VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT) {
                ((VkDrmFormatModifierPropertiesListEXT *)b)->drmFormatModifierCount = 0;
            } else if (b->sType == VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_2_EXT) {
                ((VkDrmFormatModifierPropertiesList2EXT *)b)->drmFormatModifierCount = 0;
            }
        }
    }
    return 1;
}

int vkb_caps_format_properties(const vkb_pd_caps *caps, VkPhysicalDevice pd,
                               VkFormat format, VkFormatProperties *fp,
                               VkFormatProperties2 *fp2,
                               vkb_caps_get_fmt_fn get_fmt, void *user)
{
    return vkb_caps_format_properties2(caps, pd, format, fp, fp2, get_fmt, NULL, user);
}

/* ------------------------------------------------------------------ multi-planar extended usage */

/*
 * The Mali-G925 driver refuses VK_IMAGE_CREATE_EXTENDED_USAGE_BIT on multi-planar (YCbCr) formats:
 * vkGetPhysicalDeviceImageFormatProperties(2) answers VK_ERROR_FORMAT_NOT_SUPPORTED for an NV12
 * (G8_B8R8_2PLANE_420) image that is MUTABLE_FORMAT | EXTENDED_USAGE, even when every usage bit
 * asked for is one the format itself supports, and gives the same image without EXTENDED_USAGE.
 * The flag only relaxes the rules (usage may include bits only a view format supports), so with
 * usage the format covers it changes nothing. DXVK sets both flags on every multi-planar D3D11
 * texture (its plane views are R8 / R8G8), so each NV12 texture failed: "D3D11: Cannot create
 * texture: Format 103", Media Foundation's H.264 decoder could not allocate its output samples
 * ("Failed to initialize allocator, hr 0x80070057"), and Risk of Rain Returns waited for its intro
 * video's first frame for ever (2026-10-19).
 *
 * Such a query or image goes to the driver without EXTENDED_USAGE when the usage is all supported
 * by the format at that tiling (an image whose usage needs a view format's features keeps the flag,
 * and the driver's answer). VKBRIDGE_PLANAR_EXT_FIX=0 turns it off.
 */
int vkb_caps_planar_ext_usage(const vkb_pd_caps *caps, VkPhysicalDevice pd,
                              VkFormat format, VkImageTiling tiling,
                              VkImageUsageFlags usage, VkImageCreateFlags *flags,
                              vkb_caps_get_fmt_fn get_fmt, void *user)
{
    /* The capability model does not gate this one: the check is on the format's own features,
     * which the driver itself answers. The parameter keeps the shape of the other entry points. */
    (void)caps;
    static int off = -1;
    if (off < 0) {
        const char *v = getenv("VKBRIDGE_PLANAR_EXT_FIX");
        off = v && !strcmp(v, "0");
    }
    if (off || !(*flags & VK_IMAGE_CREATE_EXTENDED_USAGE_BIT) || !vkb_is_multiplanar(format)) return 0;
    if (tiling != VK_IMAGE_TILING_OPTIMAL && tiling != VK_IMAGE_TILING_LINEAR) return 0;
    if (!get_fmt) return 0;
    VkFormatProperties fp = {0};
    get_fmt(pd, format, &fp, user);
    VkFormatFeatureFlags have = tiling == VK_IMAGE_TILING_OPTIMAL ? fp.optimalTilingFeatures : fp.linearTilingFeatures;
    static const struct { VkImageUsageFlags u; VkFormatFeatureFlags f; } need[] = {
        {VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_FORMAT_FEATURE_TRANSFER_SRC_BIT},
        {VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_FORMAT_FEATURE_TRANSFER_DST_BIT},
        {VK_IMAGE_USAGE_SAMPLED_BIT, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT},
        {VK_IMAGE_USAGE_STORAGE_BIT, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT},
        {VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT},
    };
    VkImageUsageFlags left = usage;
    for (unsigned i = 0; i < sizeof(need) / sizeof(need[0]); i++) {
        if (!(usage & need[i].u)) continue;
        if (!(have & need[i].f)) return 0; /* a view format's feature: the flag is needed */
        left &= ~need[i].u;
    }
    if (left) return 0; /* a usage this check does not know */
    *flags &= ~(VkImageCreateFlags)VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
    return 1;
}

void vkb_caps_patch_memory_heaps(const vkb_pd_caps *caps,
                                 VkPhysicalDeviceMemoryProperties *mem,
                                 VkPhysicalDeviceMemoryBudgetPropertiesEXT *budget)
{
    if (!caps->heap_max_mb)
        return;
    VkDeviceSize cap = (VkDeviceSize)caps->heap_max_mb << 20;
    for (uint32_t h = 0; h < mem->memoryHeapCount; h++) {
        if ((mem->memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) &&
            mem->memoryHeaps[h].size > cap) {
            mem->memoryHeaps[h].size = cap;
        }
        if (budget && budget->heapBudget[h] > mem->memoryHeaps[h].size) {
            budget->heapBudget[h] = mem->memoryHeaps[h].size;
        }
    }
}
