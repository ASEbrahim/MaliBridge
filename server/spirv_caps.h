/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 ASEbrahim and MaliBridge contributors */
#ifndef VKB_SPIRV_CAPS_H
#define VKB_SPIRV_CAPS_H

#include <stddef.h>
#include <stdint.h>

/* What a SPIR-V module asks of the device beyond Vulkan 1.0's required features, as far as the
 * driver's VkPhysicalDeviceFeatures tells (server_guard.c's capability check): one bit per
 * feature, from its OpCapability declarations and - for clip and cull distances - its BuiltIn
 * decorations too (ULTRAKILL's Unity vertex shader declared gl_ClipDistance in gl_PerVertex
 * without the ClipDistance capability, and Mali's compiler died on it). */
enum {
    VKB_SPVCAP_CLIP = 1u << 0,         /* ClipDistance: shaderClipDistance */
    VKB_SPVCAP_CULL = 1u << 1,         /* CullDistance: shaderCullDistance */
    VKB_SPVCAP_F64 = 1u << 2,          /* Float64: shaderFloat64 */
    VKB_SPVCAP_I64 = 1u << 3,          /* Int64: shaderInt64 */
    VKB_SPVCAP_I16 = 1u << 4,          /* Int16: shaderInt16 */
    VKB_SPVCAP_GEOM = 1u << 5,         /* Geometry: geometryShader */
    VKB_SPVCAP_TESS = 1u << 6,         /* Tessellation: tessellationShader */
    VKB_SPVCAP_CUBE_ARRAY = 1u << 7,   /* ImageCubeArray / SampledCubeArray: imageCubeArray */
    VKB_SPVCAP_SAMPLE_RATE = 1u << 8,  /* SampleRateShading / InterpolationFunction: sampleRateShading */
    VKB_SPVCAP_GATHER_EXT = 1u << 9,   /* ImageGatherExtended: shaderImageGatherExtended */
    VKB_SPVCAP_MS_STORAGE = 1u << 10,  /* StorageImageMultisample: shaderStorageImageMultisample */
    VKB_SPVCAP_EXT_FORMATS = 1u << 11, /* StorageImageExtendedFormats: shaderStorageImageExtendedFormats */
    VKB_SPVCAP_READ_NOFMT = 1u << 12,  /* StorageImageReadWithoutFormat: shaderStorageImageReadWithoutFormat */
    VKB_SPVCAP_WRITE_NOFMT = 1u << 13, /* StorageImageWriteWithoutFormat: shaderStorageImageWriteWithoutFormat */
    VKB_SPVCAP_COUNT = 14
};

/* The bits a module needs (0 for anything that is not SPIR-V). Reads the module up to its first
 * type declaration: capabilities and decorations come before. */
uint32_t vkb_spv_caps(const uint32_t *code, size_t bytes);
/* The SPIR-V name and the VkPhysicalDeviceFeatures member of bit index i (0..VKB_SPVCAP_COUNT-1). */
const char *vkb_spv_cap_name(int i);
const char *vkb_spv_cap_feature(int i);
/* Bit names of a mask as "ClipDistance (shaderClipDistance), ..." into out. */
void vkb_spv_caps_text(uint32_t mask, char *out, size_t cap);

#endif
