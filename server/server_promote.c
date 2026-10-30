/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * Vulkan version promotion: a device offered at Vulkan 1.2 or 1.3 on a driver below it, when every
 * piece that version made core is there - as the driver's extension, or done by the bridge.
 *
 * Older Mali drivers are Vulkan 1.1 (Mali-G710 r32p1, OnePlus Ace 2V: 1.1.177) and DXVK 2 and 3 ask
 * for 1.3: Half-Life 2 and every D3D game left at once ("apiVersion is too old"). Most of what 1.2
 * and 1.3 made core such a driver has as extensions (71 on that one: timeline semaphores, buffer
 * device address, descriptor indexing, ...). What the app sees then:
 *   - apiVersion: the highest version whose every piece is present (pieces[] below), or the
 *     driver's; VKBRIDGE_PROMOTE_API=0 leaves the driver's (and installs nothing), =1.2 stops at 1.2.
 *     The switch never raises the version past what is complete: "auto" (the default) and "1.3"
 *     both give 1.3 only with every piece of it.
 *   - VkPhysicalDeviceVulkan11/12/13Features and ...Properties: a driver below their version does
 *     not know them, so they are taken out of the query and filled from the extensions' own
 *     structures (feats[] / props[]), which the bridge chains in instead.
 *   - vkCreateDevice: the same structures turned into the extensions' ones, and the extensions the
 *     promoted version's core commands come from enabled; the core names then resolve to the
 *     extensions' (vkb_dispatch_load_device tries every alias), and the features 1.3 has without a
 *     bit of their own (extended dynamic state, texel buffer alignment, ...) are switched on.
 *   - SPIR-V above what the driver takes is lowered to it (spirv_downgrade.c), beneath every layer.
 *   - privateData and the tooling info are the client's own (descriptor.c, physdev.c).
 * Every decision is logged at start ("promotion: ..."), with what is missing for the next version.
 *
 * Host tests: VKBRIDGE_FAKE_API=1.1 has the server take the desktop driver as a 1.1 one: the
 * version it reports, the 1.2/1.3 structures it may be given, and the core 1.2/1.3 commands of its
 * devices (fake_gdpa: only the extensions' names resolve, and only when enabled).
 *
 * Our own code from the Vulkan specification's promotion lists (no other implementation looked at).
 */
#define _GNU_SOURCE
#include "vkb_server.h"
#include "server_emu.h"
#include "spirv_downgrade.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define V11 VK_API_VERSION_1_1
#define V12 VK_API_VERSION_1_2
#define V13 VK_API_VERSION_1_3
#define VMINOR(v) VK_API_VERSION_MINOR(v)
#define BELOW(a, b) (VK_API_VERSION_MINOR(a) < VK_API_VERSION_MINOR(b))

/* ------------------------------------------------------------------ the extensions' structures */

/* Feature structures of what 1.1, 1.2 and 1.3 made core. ext NULL: core in 1.1 (every driver here
 * has it); sType 0: an extension without a structure (its feature is "the extension is there"). */
enum {
    EF_16BIT, EF_MULTIVIEW, EF_VARPTR, EF_PROTECTED, EF_YCBCR, EF_DRAWPARAMS, /* 1.1 */
    EF_MIRROR, EF_DIC, EF_8BIT, EF_ATOMIC64, EF_F16I8, EF_DI_EXT, EF_DI, EF_MINMAX, EF_SCALAR, EF_IMAGELESS, EF_UBOSTD,
    EF_SGEXT, EF_SEPDS, EF_HQR, EF_TIMELINE, EF_BDA, EF_VMM, EF_VPLAYER, /* 1.2 */
    EF_IMGROBUST, EF_IUB, EF_PCCC, EF_PRIVATE, EF_DEMOTE, EF_TERMINATE, EF_SGSIZE, EF_SYNC2, EF_ASTCHDR, EF_ZEROWG,
    EF_DYNREND, EF_DOT, EF_MAINT4, /* 1.3, in VkPhysicalDeviceVulkan13Features */
    EF_EDS, EF_EDS2, EF_TEXALIGN, EF_4444, EF_YCBCR444, /* 1.3, features without a core bit (on with the version) */
    EF_COUNT
};

typedef struct ext_struct {
    VkStructureType stype;
    uint32_t size;
    const char *ext;
    uint32_t version; /* the version that made it core */
} ext_struct;

#define S(t, T) (VkStructureType)VK_STRUCTURE_TYPE_##t, (uint32_t)sizeof(T)
static const ext_struct feat_structs[EF_COUNT] = {
    [EF_16BIT] = {S(PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES, VkPhysicalDevice16BitStorageFeatures), NULL, V11},
    [EF_MULTIVIEW] = {S(PHYSICAL_DEVICE_MULTIVIEW_FEATURES, VkPhysicalDeviceMultiviewFeatures), NULL, V11},
    [EF_VARPTR] = {S(PHYSICAL_DEVICE_VARIABLE_POINTERS_FEATURES, VkPhysicalDeviceVariablePointersFeatures), NULL, V11},
    [EF_PROTECTED] = {S(PHYSICAL_DEVICE_PROTECTED_MEMORY_FEATURES, VkPhysicalDeviceProtectedMemoryFeatures), NULL, V11},
    [EF_YCBCR] = {S(PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES, VkPhysicalDeviceSamplerYcbcrConversionFeatures), NULL, V11},
    [EF_DRAWPARAMS] = {S(PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES, VkPhysicalDeviceShaderDrawParametersFeatures), NULL, V11},
    [EF_MIRROR] = {0, 0, "VK_KHR_sampler_mirror_clamp_to_edge", V12},
    [EF_DIC] = {0, 0, "VK_KHR_draw_indirect_count", V12},
    [EF_8BIT] = {S(PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES, VkPhysicalDevice8BitStorageFeatures), "VK_KHR_8bit_storage", V12},
    [EF_ATOMIC64] = {S(PHYSICAL_DEVICE_SHADER_ATOMIC_INT64_FEATURES, VkPhysicalDeviceShaderAtomicInt64Features), "VK_KHR_shader_atomic_int64", V12},
    [EF_F16I8] = {S(PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES, VkPhysicalDeviceShaderFloat16Int8Features), "VK_KHR_shader_float16_int8", V12},
    [EF_DI_EXT] = {0, 0, "VK_EXT_descriptor_indexing", V12},
    [EF_DI] = {S(PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES, VkPhysicalDeviceDescriptorIndexingFeatures), "VK_EXT_descriptor_indexing", V12},
    [EF_MINMAX] = {0, 0, "VK_EXT_sampler_filter_minmax", V12},
    [EF_SCALAR] = {S(PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES, VkPhysicalDeviceScalarBlockLayoutFeatures), "VK_EXT_scalar_block_layout", V12},
    [EF_IMAGELESS] = {S(PHYSICAL_DEVICE_IMAGELESS_FRAMEBUFFER_FEATURES, VkPhysicalDeviceImagelessFramebufferFeatures), "VK_KHR_imageless_framebuffer", V12},
    [EF_UBOSTD] = {S(PHYSICAL_DEVICE_UNIFORM_BUFFER_STANDARD_LAYOUT_FEATURES, VkPhysicalDeviceUniformBufferStandardLayoutFeatures),
                   "VK_KHR_uniform_buffer_standard_layout", V12},
    [EF_SGEXT] = {S(PHYSICAL_DEVICE_SHADER_SUBGROUP_EXTENDED_TYPES_FEATURES, VkPhysicalDeviceShaderSubgroupExtendedTypesFeatures),
                  "VK_KHR_shader_subgroup_extended_types", V12},
    [EF_SEPDS] = {S(PHYSICAL_DEVICE_SEPARATE_DEPTH_STENCIL_LAYOUTS_FEATURES, VkPhysicalDeviceSeparateDepthStencilLayoutsFeatures),
                  "VK_KHR_separate_depth_stencil_layouts", V12},
    [EF_HQR] = {S(PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES, VkPhysicalDeviceHostQueryResetFeatures), "VK_EXT_host_query_reset", V12},
    [EF_TIMELINE] = {S(PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES, VkPhysicalDeviceTimelineSemaphoreFeatures), "VK_KHR_timeline_semaphore", V12},
    [EF_BDA] = {S(PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES, VkPhysicalDeviceBufferDeviceAddressFeatures), "VK_KHR_buffer_device_address", V12},
    [EF_VMM] = {S(PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES, VkPhysicalDeviceVulkanMemoryModelFeatures), "VK_KHR_vulkan_memory_model", V12},
    [EF_VPLAYER] = {0, 0, "VK_EXT_shader_viewport_index_layer", V12},
    [EF_IMGROBUST] = {S(PHYSICAL_DEVICE_IMAGE_ROBUSTNESS_FEATURES, VkPhysicalDeviceImageRobustnessFeatures), "VK_EXT_image_robustness", V13},
    [EF_IUB] = {S(PHYSICAL_DEVICE_INLINE_UNIFORM_BLOCK_FEATURES, VkPhysicalDeviceInlineUniformBlockFeatures), "VK_EXT_inline_uniform_block", V13},
    [EF_PCCC] = {S(PHYSICAL_DEVICE_PIPELINE_CREATION_CACHE_CONTROL_FEATURES, VkPhysicalDevicePipelineCreationCacheControlFeatures),
                 "VK_EXT_pipeline_creation_cache_control", V13},
    [EF_PRIVATE] = {S(PHYSICAL_DEVICE_PRIVATE_DATA_FEATURES, VkPhysicalDevicePrivateDataFeatures), "VK_EXT_private_data", V13},
    [EF_DEMOTE] = {S(PHYSICAL_DEVICE_SHADER_DEMOTE_TO_HELPER_INVOCATION_FEATURES, VkPhysicalDeviceShaderDemoteToHelperInvocationFeatures),
                   "VK_EXT_shader_demote_to_helper_invocation", V13},
    [EF_TERMINATE] = {S(PHYSICAL_DEVICE_SHADER_TERMINATE_INVOCATION_FEATURES, VkPhysicalDeviceShaderTerminateInvocationFeatures),
                      "VK_KHR_shader_terminate_invocation", V13},
    [EF_SGSIZE] = {S(PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES, VkPhysicalDeviceSubgroupSizeControlFeatures), "VK_EXT_subgroup_size_control", V13},
    [EF_SYNC2] = {S(PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES, VkPhysicalDeviceSynchronization2Features), "VK_KHR_synchronization2", V13},
    [EF_ASTCHDR] = {S(PHYSICAL_DEVICE_TEXTURE_COMPRESSION_ASTC_HDR_FEATURES, VkPhysicalDeviceTextureCompressionASTCHDRFeatures),
                    "VK_EXT_texture_compression_astc_hdr", V13},
    [EF_ZEROWG] = {S(PHYSICAL_DEVICE_ZERO_INITIALIZE_WORKGROUP_MEMORY_FEATURES, VkPhysicalDeviceZeroInitializeWorkgroupMemoryFeatures),
                   "VK_KHR_zero_initialize_workgroup_memory", V13},
    [EF_DYNREND] = {S(PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES, VkPhysicalDeviceDynamicRenderingFeatures), "VK_KHR_dynamic_rendering", V13},
    [EF_DOT] = {S(PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_FEATURES, VkPhysicalDeviceShaderIntegerDotProductFeatures),
                "VK_KHR_shader_integer_dot_product", V13},
    [EF_MAINT4] = {S(PHYSICAL_DEVICE_MAINTENANCE_4_FEATURES, VkPhysicalDeviceMaintenance4Features), "VK_KHR_maintenance4", V13},
    [EF_EDS] = {S(PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT, VkPhysicalDeviceExtendedDynamicStateFeaturesEXT),
                "VK_EXT_extended_dynamic_state", V13},
    [EF_EDS2] = {S(PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT, VkPhysicalDeviceExtendedDynamicState2FeaturesEXT),
                 "VK_EXT_extended_dynamic_state2", V13},
    [EF_TEXALIGN] = {S(PHYSICAL_DEVICE_TEXEL_BUFFER_ALIGNMENT_FEATURES_EXT, VkPhysicalDeviceTexelBufferAlignmentFeaturesEXT),
                     "VK_EXT_texel_buffer_alignment", V13},
    [EF_4444] = {S(PHYSICAL_DEVICE_4444_FORMATS_FEATURES_EXT, VkPhysicalDevice4444FormatsFeaturesEXT), "VK_EXT_4444_formats", V13},
    [EF_YCBCR444] = {S(PHYSICAL_DEVICE_YCBCR_2_PLANE_444_FORMATS_FEATURES_EXT, VkPhysicalDeviceYcbcr2Plane444FormatsFeaturesEXT),
                     "VK_EXT_ycbcr_2plane_444_formats", V13},
};

/* Members of VkPhysicalDeviceVulkan1xFeatures and where they come from. ext_off NONE: the
 * extension's presence. */
#define NONE 0xffffu
typedef struct feat_row {
    uint8_t core; /* 11, 12, 13 */
    uint8_t ef;
    uint16_t core_off, ext_off;
} feat_row;
#define F11(m, ef, T, em) {11, ef, offsetof(VkPhysicalDeviceVulkan11Features, m), offsetof(T, em)}
#define F12(m, ef, T, em) {12, ef, offsetof(VkPhysicalDeviceVulkan12Features, m), offsetof(T, em)}
#define F13(m, ef, T, em) {13, ef, offsetof(VkPhysicalDeviceVulkan13Features, m), offsetof(T, em)}
#define P12(m, ef) {12, ef, offsetof(VkPhysicalDeviceVulkan12Features, m), NONE}
#define DI(m) F12(m, EF_DI, VkPhysicalDeviceDescriptorIndexingFeatures, m)
static const feat_row feats[] = {
    F11(storageBuffer16BitAccess, EF_16BIT, VkPhysicalDevice16BitStorageFeatures, storageBuffer16BitAccess),
    F11(uniformAndStorageBuffer16BitAccess, EF_16BIT, VkPhysicalDevice16BitStorageFeatures, uniformAndStorageBuffer16BitAccess),
    F11(storagePushConstant16, EF_16BIT, VkPhysicalDevice16BitStorageFeatures, storagePushConstant16),
    F11(storageInputOutput16, EF_16BIT, VkPhysicalDevice16BitStorageFeatures, storageInputOutput16),
    F11(multiview, EF_MULTIVIEW, VkPhysicalDeviceMultiviewFeatures, multiview),
    F11(multiviewGeometryShader, EF_MULTIVIEW, VkPhysicalDeviceMultiviewFeatures, multiviewGeometryShader),
    F11(multiviewTessellationShader, EF_MULTIVIEW, VkPhysicalDeviceMultiviewFeatures, multiviewTessellationShader),
    F11(variablePointersStorageBuffer, EF_VARPTR, VkPhysicalDeviceVariablePointersFeatures, variablePointersStorageBuffer),
    F11(variablePointers, EF_VARPTR, VkPhysicalDeviceVariablePointersFeatures, variablePointers),
    F11(protectedMemory, EF_PROTECTED, VkPhysicalDeviceProtectedMemoryFeatures, protectedMemory),
    F11(samplerYcbcrConversion, EF_YCBCR, VkPhysicalDeviceSamplerYcbcrConversionFeatures, samplerYcbcrConversion),
    F11(shaderDrawParameters, EF_DRAWPARAMS, VkPhysicalDeviceShaderDrawParametersFeatures, shaderDrawParameters),
    P12(samplerMirrorClampToEdge, EF_MIRROR),
    P12(drawIndirectCount, EF_DIC),
    F12(storageBuffer8BitAccess, EF_8BIT, VkPhysicalDevice8BitStorageFeatures, storageBuffer8BitAccess),
    F12(uniformAndStorageBuffer8BitAccess, EF_8BIT, VkPhysicalDevice8BitStorageFeatures, uniformAndStorageBuffer8BitAccess),
    F12(storagePushConstant8, EF_8BIT, VkPhysicalDevice8BitStorageFeatures, storagePushConstant8),
    F12(shaderBufferInt64Atomics, EF_ATOMIC64, VkPhysicalDeviceShaderAtomicInt64Features, shaderBufferInt64Atomics),
    F12(shaderSharedInt64Atomics, EF_ATOMIC64, VkPhysicalDeviceShaderAtomicInt64Features, shaderSharedInt64Atomics),
    F12(shaderFloat16, EF_F16I8, VkPhysicalDeviceShaderFloat16Int8Features, shaderFloat16),
    F12(shaderInt8, EF_F16I8, VkPhysicalDeviceShaderFloat16Int8Features, shaderInt8),
    P12(descriptorIndexing, EF_DI_EXT),
    DI(shaderInputAttachmentArrayDynamicIndexing),
    DI(shaderUniformTexelBufferArrayDynamicIndexing),
    DI(shaderStorageTexelBufferArrayDynamicIndexing),
    DI(shaderUniformBufferArrayNonUniformIndexing),
    DI(shaderSampledImageArrayNonUniformIndexing),
    DI(shaderStorageBufferArrayNonUniformIndexing),
    DI(shaderStorageImageArrayNonUniformIndexing),
    DI(shaderInputAttachmentArrayNonUniformIndexing),
    DI(shaderUniformTexelBufferArrayNonUniformIndexing),
    DI(shaderStorageTexelBufferArrayNonUniformIndexing),
    DI(descriptorBindingUniformBufferUpdateAfterBind),
    DI(descriptorBindingSampledImageUpdateAfterBind),
    DI(descriptorBindingStorageImageUpdateAfterBind),
    DI(descriptorBindingStorageBufferUpdateAfterBind),
    DI(descriptorBindingUniformTexelBufferUpdateAfterBind),
    DI(descriptorBindingStorageTexelBufferUpdateAfterBind),
    DI(descriptorBindingUpdateUnusedWhilePending),
    DI(descriptorBindingPartiallyBound),
    DI(descriptorBindingVariableDescriptorCount),
    DI(runtimeDescriptorArray),
    P12(samplerFilterMinmax, EF_MINMAX),
    F12(scalarBlockLayout, EF_SCALAR, VkPhysicalDeviceScalarBlockLayoutFeatures, scalarBlockLayout),
    F12(imagelessFramebuffer, EF_IMAGELESS, VkPhysicalDeviceImagelessFramebufferFeatures, imagelessFramebuffer),
    F12(uniformBufferStandardLayout, EF_UBOSTD, VkPhysicalDeviceUniformBufferStandardLayoutFeatures, uniformBufferStandardLayout),
    F12(shaderSubgroupExtendedTypes, EF_SGEXT, VkPhysicalDeviceShaderSubgroupExtendedTypesFeatures, shaderSubgroupExtendedTypes),
    F12(separateDepthStencilLayouts, EF_SEPDS, VkPhysicalDeviceSeparateDepthStencilLayoutsFeatures, separateDepthStencilLayouts),
    F12(hostQueryReset, EF_HQR, VkPhysicalDeviceHostQueryResetFeatures, hostQueryReset),
    F12(timelineSemaphore, EF_TIMELINE, VkPhysicalDeviceTimelineSemaphoreFeatures, timelineSemaphore),
    F12(bufferDeviceAddress, EF_BDA, VkPhysicalDeviceBufferDeviceAddressFeatures, bufferDeviceAddress),
    F12(bufferDeviceAddressCaptureReplay, EF_BDA, VkPhysicalDeviceBufferDeviceAddressFeatures, bufferDeviceAddressCaptureReplay),
    F12(bufferDeviceAddressMultiDevice, EF_BDA, VkPhysicalDeviceBufferDeviceAddressFeatures, bufferDeviceAddressMultiDevice),
    F12(vulkanMemoryModel, EF_VMM, VkPhysicalDeviceVulkanMemoryModelFeatures, vulkanMemoryModel),
    F12(vulkanMemoryModelDeviceScope, EF_VMM, VkPhysicalDeviceVulkanMemoryModelFeatures, vulkanMemoryModelDeviceScope),
    F12(vulkanMemoryModelAvailabilityVisibilityChains, EF_VMM, VkPhysicalDeviceVulkanMemoryModelFeatures,
        vulkanMemoryModelAvailabilityVisibilityChains),
    P12(shaderOutputViewportIndex, EF_VPLAYER),
    P12(shaderOutputLayer, EF_VPLAYER),
    /* subgroupBroadcastDynamicId: the SPIR-V lowering's (fill_core) */
    F13(robustImageAccess, EF_IMGROBUST, VkPhysicalDeviceImageRobustnessFeatures, robustImageAccess),
    F13(inlineUniformBlock, EF_IUB, VkPhysicalDeviceInlineUniformBlockFeatures, inlineUniformBlock),
    F13(descriptorBindingInlineUniformBlockUpdateAfterBind, EF_IUB, VkPhysicalDeviceInlineUniformBlockFeatures,
        descriptorBindingInlineUniformBlockUpdateAfterBind),
    F13(pipelineCreationCacheControl, EF_PCCC, VkPhysicalDevicePipelineCreationCacheControlFeatures, pipelineCreationCacheControl),
    F13(privateData, EF_PRIVATE, VkPhysicalDevicePrivateDataFeatures, privateData),
    F13(shaderDemoteToHelperInvocation, EF_DEMOTE, VkPhysicalDeviceShaderDemoteToHelperInvocationFeatures, shaderDemoteToHelperInvocation),
    F13(shaderTerminateInvocation, EF_TERMINATE, VkPhysicalDeviceShaderTerminateInvocationFeatures, shaderTerminateInvocation),
    F13(subgroupSizeControl, EF_SGSIZE, VkPhysicalDeviceSubgroupSizeControlFeatures, subgroupSizeControl),
    F13(computeFullSubgroups, EF_SGSIZE, VkPhysicalDeviceSubgroupSizeControlFeatures, computeFullSubgroups),
    F13(synchronization2, EF_SYNC2, VkPhysicalDeviceSynchronization2Features, synchronization2),
    F13(textureCompressionASTC_HDR, EF_ASTCHDR, VkPhysicalDeviceTextureCompressionASTCHDRFeatures, textureCompressionASTC_HDR),
    F13(shaderZeroInitializeWorkgroupMemory, EF_ZEROWG, VkPhysicalDeviceZeroInitializeWorkgroupMemoryFeatures,
        shaderZeroInitializeWorkgroupMemory),
    F13(dynamicRendering, EF_DYNREND, VkPhysicalDeviceDynamicRenderingFeatures, dynamicRendering),
    F13(shaderIntegerDotProduct, EF_DOT, VkPhysicalDeviceShaderIntegerDotProductFeatures, shaderIntegerDotProduct),
    F13(maintenance4, EF_MAINT4, VkPhysicalDeviceMaintenance4Features, maintenance4),
};
#define NFEATS (sizeof(feats) / sizeof(feats[0]))

/* Property structures, and ranges of VkPhysicalDeviceVulkan1xProperties copied from them (the
 * members in the same order and of the same types: the lengths are checked once, check_rows). */
enum {
    EP_ID, EP_SUBGROUP, EP_POINTCLIP, EP_MULTIVIEW, EP_PROTECTED, EP_MAINT3, /* 1.1 */
    EP_DRIVER, EP_FLOATCTL, EP_DI, EP_DSRESOLVE, EP_MINMAX, EP_TIMELINE, /* 1.2 */
    EP_SGSIZE, EP_IUB, EP_DOT, EP_TEXALIGN, EP_MAINT4, /* 1.3 */
    EP_COUNT
};
static const ext_struct prop_structs[EP_COUNT] = {
    [EP_ID] = {S(PHYSICAL_DEVICE_ID_PROPERTIES, VkPhysicalDeviceIDProperties), NULL, V11},
    [EP_SUBGROUP] = {S(PHYSICAL_DEVICE_SUBGROUP_PROPERTIES, VkPhysicalDeviceSubgroupProperties), NULL, V11},
    [EP_POINTCLIP] = {S(PHYSICAL_DEVICE_POINT_CLIPPING_PROPERTIES, VkPhysicalDevicePointClippingProperties), NULL, V11},
    [EP_MULTIVIEW] = {S(PHYSICAL_DEVICE_MULTIVIEW_PROPERTIES, VkPhysicalDeviceMultiviewProperties), NULL, V11},
    [EP_PROTECTED] = {S(PHYSICAL_DEVICE_PROTECTED_MEMORY_PROPERTIES, VkPhysicalDeviceProtectedMemoryProperties), NULL, V11},
    [EP_MAINT3] = {S(PHYSICAL_DEVICE_MAINTENANCE_3_PROPERTIES, VkPhysicalDeviceMaintenance3Properties), NULL, V11},
    [EP_DRIVER] = {S(PHYSICAL_DEVICE_DRIVER_PROPERTIES, VkPhysicalDeviceDriverProperties), "VK_KHR_driver_properties", V12},
    [EP_FLOATCTL] = {S(PHYSICAL_DEVICE_FLOAT_CONTROLS_PROPERTIES, VkPhysicalDeviceFloatControlsProperties), "VK_KHR_shader_float_controls", V12},
    [EP_DI] = {S(PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES, VkPhysicalDeviceDescriptorIndexingProperties), "VK_EXT_descriptor_indexing", V12},
    [EP_DSRESOLVE] = {S(PHYSICAL_DEVICE_DEPTH_STENCIL_RESOLVE_PROPERTIES, VkPhysicalDeviceDepthStencilResolveProperties),
                      "VK_KHR_depth_stencil_resolve", V12},
    [EP_MINMAX] = {S(PHYSICAL_DEVICE_SAMPLER_FILTER_MINMAX_PROPERTIES, VkPhysicalDeviceSamplerFilterMinmaxProperties),
                   "VK_EXT_sampler_filter_minmax", V12},
    [EP_TIMELINE] = {S(PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_PROPERTIES, VkPhysicalDeviceTimelineSemaphoreProperties), "VK_KHR_timeline_semaphore", V12},
    [EP_SGSIZE] = {S(PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES, VkPhysicalDeviceSubgroupSizeControlProperties),
                   "VK_EXT_subgroup_size_control", V13},
    [EP_IUB] = {S(PHYSICAL_DEVICE_INLINE_UNIFORM_BLOCK_PROPERTIES, VkPhysicalDeviceInlineUniformBlockProperties), "VK_EXT_inline_uniform_block", V13},
    [EP_DOT] = {S(PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_PROPERTIES, VkPhysicalDeviceShaderIntegerDotProductProperties),
                "VK_KHR_shader_integer_dot_product", V13},
    [EP_TEXALIGN] = {S(PHYSICAL_DEVICE_TEXEL_BUFFER_ALIGNMENT_PROPERTIES, VkPhysicalDeviceTexelBufferAlignmentProperties),
                     "VK_EXT_texel_buffer_alignment", V13},
    [EP_MAINT4] = {S(PHYSICAL_DEVICE_MAINTENANCE_4_PROPERTIES, VkPhysicalDeviceMaintenance4Properties), "VK_KHR_maintenance4", V13},
};

typedef struct prop_row {
    uint8_t core, ep;
    uint16_t core_off, ext_off, len, ext_len;
} prop_row;
#define END(T, m) (offsetof(T, m) + sizeof(((T *)0)->m))
#define R(cv, CT, cf, cl, ep, ET, ef, el) {cv, ep, offsetof(CT, cf), offsetof(ET, ef), END(CT, cl) - offsetof(CT, cf), END(ET, el) - offsetof(ET, ef)}
#define R11(cf, cl, ep, ET, ef, el) R(11, VkPhysicalDeviceVulkan11Properties, cf, cl, ep, ET, ef, el)
#define R12(cf, cl, ep, ET, ef, el) R(12, VkPhysicalDeviceVulkan12Properties, cf, cl, ep, ET, ef, el)
#define R13(cf, cl, ep, ET, ef, el) R(13, VkPhysicalDeviceVulkan13Properties, cf, cl, ep, ET, ef, el)
#define M13(m, ep, ET, em) R13(m, m, ep, ET, em, em)
static const prop_row props[] = {
    R11(deviceUUID, deviceLUIDValid, EP_ID, VkPhysicalDeviceIDProperties, deviceUUID, deviceLUIDValid),
    R11(subgroupSize, subgroupQuadOperationsInAllStages, EP_SUBGROUP, VkPhysicalDeviceSubgroupProperties, subgroupSize, quadOperationsInAllStages),
    R11(pointClippingBehavior, pointClippingBehavior, EP_POINTCLIP, VkPhysicalDevicePointClippingProperties, pointClippingBehavior, pointClippingBehavior),
    R11(maxMultiviewViewCount, maxMultiviewInstanceIndex, EP_MULTIVIEW, VkPhysicalDeviceMultiviewProperties, maxMultiviewViewCount,
        maxMultiviewInstanceIndex),
    R11(protectedNoFault, protectedNoFault, EP_PROTECTED, VkPhysicalDeviceProtectedMemoryProperties, protectedNoFault, protectedNoFault),
    R11(maxPerSetDescriptors, maxPerSetDescriptors, EP_MAINT3, VkPhysicalDeviceMaintenance3Properties, maxPerSetDescriptors, maxPerSetDescriptors),
    R11(maxMemoryAllocationSize, maxMemoryAllocationSize, EP_MAINT3, VkPhysicalDeviceMaintenance3Properties, maxMemoryAllocationSize,
        maxMemoryAllocationSize),
    R12(driverID, conformanceVersion, EP_DRIVER, VkPhysicalDeviceDriverProperties, driverID, conformanceVersion),
    R12(denormBehaviorIndependence, shaderRoundingModeRTZFloat64, EP_FLOATCTL, VkPhysicalDeviceFloatControlsProperties, denormBehaviorIndependence,
        shaderRoundingModeRTZFloat64),
    R12(maxUpdateAfterBindDescriptorsInAllPools, maxDescriptorSetUpdateAfterBindInputAttachments, EP_DI, VkPhysicalDeviceDescriptorIndexingProperties,
        maxUpdateAfterBindDescriptorsInAllPools, maxDescriptorSetUpdateAfterBindInputAttachments),
    R12(supportedDepthResolveModes, independentResolve, EP_DSRESOLVE, VkPhysicalDeviceDepthStencilResolveProperties, supportedDepthResolveModes,
        independentResolve),
    R12(filterMinmaxSingleComponentFormats, filterMinmaxImageComponentMapping, EP_MINMAX, VkPhysicalDeviceSamplerFilterMinmaxProperties,
        filterMinmaxSingleComponentFormats, filterMinmaxImageComponentMapping),
    R12(maxTimelineSemaphoreValueDifference, maxTimelineSemaphoreValueDifference, EP_TIMELINE, VkPhysicalDeviceTimelineSemaphoreProperties,
        maxTimelineSemaphoreValueDifference, maxTimelineSemaphoreValueDifference),
    R13(minSubgroupSize, requiredSubgroupSizeStages, EP_SGSIZE, VkPhysicalDeviceSubgroupSizeControlProperties, minSubgroupSize,
        requiredSubgroupSizeStages),
    R13(maxInlineUniformBlockSize, maxDescriptorSetUpdateAfterBindInlineUniformBlocks, EP_IUB, VkPhysicalDeviceInlineUniformBlockProperties,
        maxInlineUniformBlockSize, maxDescriptorSetUpdateAfterBindInlineUniformBlocks),
    R13(integerDotProduct8BitUnsignedAccelerated, integerDotProductAccumulatingSaturating64BitMixedSignednessAccelerated, EP_DOT,
        VkPhysicalDeviceShaderIntegerDotProductProperties, integerDotProduct8BitUnsignedAccelerated,
        integerDotProductAccumulatingSaturating64BitMixedSignednessAccelerated),
    M13(storageTexelBufferOffsetAlignmentBytes, EP_TEXALIGN, VkPhysicalDeviceTexelBufferAlignmentProperties, storageTexelBufferOffsetAlignmentBytes),
    M13(storageTexelBufferOffsetSingleTexelAlignment, EP_TEXALIGN, VkPhysicalDeviceTexelBufferAlignmentProperties,
        storageTexelBufferOffsetSingleTexelAlignment),
    M13(uniformTexelBufferOffsetAlignmentBytes, EP_TEXALIGN, VkPhysicalDeviceTexelBufferAlignmentProperties, uniformTexelBufferOffsetAlignmentBytes),
    M13(uniformTexelBufferOffsetSingleTexelAlignment, EP_TEXALIGN, VkPhysicalDeviceTexelBufferAlignmentProperties,
        uniformTexelBufferOffsetSingleTexelAlignment),
    M13(maxBufferSize, EP_MAINT4, VkPhysicalDeviceMaintenance4Properties, maxBufferSize),
};
#define NPROPS (sizeof(props) / sizeof(props[0]))

/* Extensions a version made core that have no structure above (their commands, enumerants or
 * properties only). need: that version is not offered without it. */
static const struct {
    uint32_t version;
    const char *ext;
    int need;
} plain_exts[] = {
    {V12, "VK_KHR_create_renderpass2", 1},
    {V12, "VK_KHR_depth_stencil_resolve", 1},
    {V12, "VK_KHR_driver_properties", 1},
    {V12, "VK_KHR_image_format_list", 1},
    {V12, "VK_KHR_shader_float_controls", 1},
    {V12, "VK_KHR_spirv_1_4", 1}, /* what SPIR-V 1.5 and 1.6 are lowered to */
    {V12, "VK_EXT_separate_stencil_usage", 1},
    {V12, "VK_EXT_sampler_filter_minmax", 0},
    {V13, "VK_KHR_copy_commands2", 1},
    {V13, "VK_KHR_format_feature_flags2", 1},
    {V13, "VK_EXT_pipeline_creation_feedback", 1},
    {V13, "VK_KHR_shader_non_semantic_info", 1},
    {V13, "VK_EXT_tooling_info", 0}, /* the client's (physdev.c) */
};
#define NPLAIN (sizeof(plain_exts) / sizeof(plain_exts[0]))

/* What a version needs as a feature (the specification's feature requirements). */
typedef struct need_row {
    uint32_t version;
    uint8_t core;
    uint16_t off;
    const char *name;
} need_row;
#define N12(m) {V12, 12, offsetof(VkPhysicalDeviceVulkan12Features, m), #m}
#define N13(m) {V13, 13, offsetof(VkPhysicalDeviceVulkan13Features, m), #m}
#define N13_12(m) {V13, 12, offsetof(VkPhysicalDeviceVulkan12Features, m), #m}
static const need_row needs[] = {
    N12(uniformBufferStandardLayout), N12(subgroupBroadcastDynamicId), N12(imagelessFramebuffer), N12(separateDepthStencilLayouts),
    N12(hostQueryReset), N12(timelineSemaphore), N12(shaderSubgroupExtendedTypes),
    N13(robustImageAccess), N13(inlineUniformBlock), N13(pipelineCreationCacheControl), N13(privateData),
    N13(shaderDemoteToHelperInvocation), N13(shaderTerminateInvocation), N13(subgroupSizeControl), N13(computeFullSubgroups),
    N13(synchronization2), N13(shaderZeroInitializeWorkgroupMemory), N13(dynamicRendering), N13(shaderIntegerDotProduct),
    N13(maintenance4), N13_12(bufferDeviceAddress), N13_12(vulkanMemoryModel), N13_12(vulkanMemoryModelDeviceScope),
};
#define NNEEDS (sizeof(needs) / sizeof(needs[0]))

/* 1.3's features without a core bit: on with the version, when the driver's extension has them. */
static const struct {
    uint8_t ef;
    uint16_t off;
} implicit13[] = {
    {EF_EDS, offsetof(VkPhysicalDeviceExtendedDynamicStateFeaturesEXT, extendedDynamicState)},
    {EF_EDS2, offsetof(VkPhysicalDeviceExtendedDynamicState2FeaturesEXT, extendedDynamicState2)},
    {EF_TEXALIGN, offsetof(VkPhysicalDeviceTexelBufferAlignmentFeaturesEXT, texelBufferAlignment)},
    {EF_4444, offsetof(VkPhysicalDevice4444FormatsFeaturesEXT, formatA4R4G4B4)},
    {EF_4444, offsetof(VkPhysicalDevice4444FormatsFeaturesEXT, formatA4B4G4R4)},
    {EF_YCBCR444, offsetof(VkPhysicalDeviceYcbcr2Plane444FormatsFeaturesEXT, ycbcr2plane444Formats)},
};
#define NIMPLICIT (sizeof(implicit13) / sizeof(implicit13[0]))

/* ------------------------------------------------------------------ switches */

static uint32_t parse_version(const char *v, uint32_t dflt)
{
    unsigned ma, mi;
    if (v && sscanf(v, "%u.%u", &ma, &mi) == 2 && ma == 1) return VK_MAKE_API_VERSION(0, 1, mi, 0);
    return dflt;
}

/* VKBRIDGE_FAKE_API=1.x (host tests): the driver taken as that version. 0: off. One snapshot. */
static uint32_t fake_api(void)
{
    static uint32_t v;
    static int taken;
    if (!__atomic_load_n(&taken, __ATOMIC_ACQUIRE)) {
        v = parse_version(vkb_srv_env("VKBRIDGE_FAKE_API"), 0);
        __atomic_store_n(&taken, 1, __ATOMIC_RELEASE);
    }
    return v;
}

/* Drivers promotion was tried on and works with: by default it is on only there, the way built-in
 * presets apply only on a GPU they were tested on. Elsewhere the log says what would be offered
 * and nothing changes; VKBRIDGE_PROMOTE_API=1 (or 1.2 / 1.3) turns it on anyway. An entry is a
 * deviceName substring and the driver's own Vulkan minor version (-1: any). Empty until a real
 * 1.1 / 1.2 device has been run with it: every test so far took a desktop driver for an older one. */
static const struct {
    const char *device;
    int minor;
} promote_tested[] = {
    {NULL, -1} /* the end */
};

/* On the list (or on VKBRIDGE_PROMOTE_TESTED's comma-separated deviceName substrings: host tests) */
static int tested_driver(const char *name, uint32_t real_api)
{
    for (size_t i = 0; promote_tested[i].device; i++)
        if (strstr(name, promote_tested[i].device) && (promote_tested[i].minor < 0 || (uint32_t)promote_tested[i].minor == VMINOR(real_api)))
            return 1;
    const char *extra = vkb_srv_env("VKBRIDGE_PROMOTE_TESTED");
    for (const char *e = extra; e && *e;) {
        size_t n = strcspn(e, ",");
        char one[128];
        if (n && n < sizeof(one)) {
            memcpy(one, e, n);
            one[n] = 0;
            if (strstr(name, one)) return 1;
        }
        e += n + (e[n] == ',');
    }
    return 0;
}

/* VKBRIDGE_PROMOTE_API: the highest version offered; unset / auto (the default): 1.3 on a tested
 * driver only (*forced 0); 1 / on: 1.3 on any driver, 1.2 / 1.3: that one (*forced 1); 0 / off: none. */
static uint32_t promote_wanted(const char **src, int *forced)
{
    const char *v = vkb_srv_env("VKBRIDGE_PROMOTE_API");
    *forced = 0;
    *src = v && *v ? "VKBRIDGE_PROMOTE_API" : "the default";
    if (!v || !*v || !strcmp(v, "auto")) return V13;
    if (!strcmp(v, "0") || !strcmp(v, "off")) return 0;
    *forced = 1;
    if (!strcmp(v, "1") || !strcmp(v, "on")) return V13;
    uint32_t r = parse_version(v, V13);
    return BELOW(V13, r) ? V13 : r;
}

static uint32_t driver_api(uint32_t real)
{
    uint32_t f = fake_api();
    if (f && BELOW(f, real)) return VK_MAKE_API_VERSION(0, 1, VMINOR(f), VK_API_VERSION_PATCH(real));
    return real;
}

/* ------------------------------------------------------------------ what the driver has */

typedef struct ext_list {
    VkExtensionProperties *e;
    uint32_t n;
} ext_list;

static void exts_get(const vkb_dispatch *d, VkPhysicalDevice pd, ext_list *l)
{
    l->n = 0;
    d->vkEnumerateDeviceExtensionProperties(pd, NULL, &l->n, NULL);
    l->e = calloc(l->n ? l->n : 1, sizeof(*l->e));
    if (!l->e) {
        l->n = 0;
        return;
    }
    d->vkEnumerateDeviceExtensionProperties(pd, NULL, &l->n, l->e);
}

/* A driver extension the bridge may use for promotion: there, and not hidden (VKBRIDGE_FAKE_MISSING). */
static int exts_has(const ext_list *l, const char *name)
{
    if (!name) return 1;
    if (vkb_emu_ext_hidden(name)) return 0;
    for (uint32_t i = 0; i < l->n; i++)
        if (!strcmp(l->e[i].extensionName, name)) return 1;
    return 0;
}

/* An extension structure's place in a query: the app's own when it chained one, else ours. */
typedef struct slot {
    VkBaseOutStructure *p;
    int ours;
} slot;

#define STORE 768 /* every structure above fits (VkPhysicalDeviceDriverProperties, 536 bytes, is the largest) */
#define NSLOTS ((int)EF_COUNT > (int)EP_COUNT ? (int)EF_COUNT : (int)EP_COUNT)

static VkBaseOutStructure *find_in_chain(void *head, VkStructureType t)
{
    for (VkBaseOutStructure *b = ((VkBaseOutStructure *)head)->pNext; b; b = b->pNext)
        if (b->sType == t) return b;
    return NULL;
}

static int is_core_struct(VkStructureType t, uint32_t drv, int props_query)
{
    if (props_query) {
        return (t == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES && BELOW(drv, V12)) ||
               (t == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES && BELOW(drv, V12)) ||
               (t == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES && BELOW(drv, V13));
    }
    return (t == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES && BELOW(drv, V12)) ||
           (t == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES && BELOW(drv, V12)) ||
           (t == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES && BELOW(drv, V13));
}

static int core_of(VkStructureType t)
{
    switch ((int)t) {
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES: case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES: return 11;
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES: case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES: return 12;
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES: case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES: return 13;
    default: return 0;
    }
}

/*
 * One query (features or properties) on a driver below the version of some structures in it:
 * those are unlinked, the extensions' structures chained in their place (the app's own where it
 * chained them), the driver asked, and the core structures filled. The app's chain is linked back
 * as it was. q: vkGetPhysicalDeviceFeatures2 or ...Properties2 (props_query).
 */
typedef struct translate {
    uint32_t drv;
    const ext_list *exts;
    int props_query;
    VkBaseOutStructure *core[3];       /* the app's 1.1 / 1.2 / 1.3 structures taken out (or ours) */
    slot slots[NSLOTS];
    uint8_t store[NSLOTS][STORE];
} translate;

static void chain_query(translate *x, void *head, void (*query)(void *ctx, void *head), void *ctx)
{
    VkBaseOutStructure *h = head;
    VkBaseOutStructure *saved[64];
    VkBaseOutStructure *nodes[64];
    uint32_t nn = 0;
    for (VkBaseOutStructure *b = h->pNext; b && nn < 64; b = b->pNext) {
        nodes[nn] = b;
        saved[nn] = b->pNext;
        nn++;
    }
    VkBaseOutStructure *saved_head = h->pNext;
    /* the chain the driver gets: the app's structures it knows, then ours */
    VkBaseOutStructure *tail = h;
    for (uint32_t i = 0; i < nn; i++) {
        int c = core_of(nodes[i]->sType);
        if (c && is_core_struct(nodes[i]->sType, x->drv, x->props_query)) {
            x->core[c - 11] = nodes[i];
            continue;
        }
        tail->pNext = nodes[i];
        tail = nodes[i];
    }
    const ext_struct *tab = x->props_query ? prop_structs : feat_structs;
    uint32_t count = x->props_query ? EP_COUNT : EF_COUNT;
    for (uint32_t i = 0; i < count; i++) {
        x->slots[i].p = NULL;
        x->slots[i].ours = 0;
        /* only for the core structures taken out: 1.1's from 1.1's own, ... */
        int c = tab[i].version == V11 ? 0 : tab[i].version == V12 ? 1 : 2;
        if (!tab[i].stype || !x->core[c] || (!x->props_query && i >= EF_EDS) || tab[i].size > STORE) continue;
        if (!exts_has(x->exts, tab[i].ext)) continue;
        VkBaseOutStructure *app = NULL;
        for (uint32_t j = 0; j < nn; j++)
            if (nodes[j]->sType == tab[i].stype) app = nodes[j];
        if (app) {
            x->slots[i].p = app;
            continue;
        }
        /* the same structure for two rows (EF_DI_EXT has none): one is enough */
        int dup = 0;
        for (uint32_t j = 0; j < i; j++)
            if (x->slots[j].ours && x->slots[j].p->sType == tab[i].stype) {
                x->slots[i] = x->slots[j];
                dup = 1;
            }
        if (dup) continue;
        memset(x->store[i], 0, tab[i].size);
        VkBaseOutStructure *s = (VkBaseOutStructure *)x->store[i];
        s->sType = tab[i].stype;
        tail->pNext = s;
        tail = s;
        x->slots[i].p = s;
        x->slots[i].ours = 1;
    }
    tail->pNext = NULL;
    query(ctx, head);
    h->pNext = saved_head;
    for (uint32_t i = 0; i < nn; i++) nodes[i]->pNext = saved[i];
}

/* ------------------------------------------------------------------ knowledge per GPU */

typedef struct promo_gpu {
    uint8_t uuid[VK_UUID_SIZE];
    uint32_t real_api;     /* the driver's */
    uint32_t drv_api;      /* the driver's (or VKBRIDGE_FAKE_API's) */
    uint32_t offered;      /* the version the app sees: drv_api when nothing is promoted */
    uint32_t spv_max;      /* the SPIR-V version the driver takes */
    int shuffle;           /* subgroup shuffles in the driver: dynamic-id broadcasts lowered to them */
    uint32_t lanes;        /* without shuffles: the largest subgroup, each dynamic-id broadcast a chain over its lanes */
    int active;            /* translation wrappers on (VKBRIDGE_PROMOTE_API not 0, driver below 1.3) */
    char missing[512];     /* what the next version lacks (for the log and the session report) */
    char name[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE];
    int int64;             /* shaderInt64, which no emulation adds */
    uint32_t push;         /* maxPushConstantsSize, the driver's (the bridge passes it on) */
} promo_gpu;

static promo_gpu gpus[VKB_MAX_PDS];
/* The same GPUs as gamescope sees them: promoted wherever every piece is there, tested or not. On a
 * Vulkan 1.1 driver (Mali r32p1: G77, G710, G610, G57) gamescope found no device it could use and
 * Steam mode ended at "starting display and audio" (MaliBridge#10); the games keep the driver's own
 * version (and DroidDeck's old DXVK for it), only the compositor is promoted. */
static promo_gpu gpus_ui[VKB_MAX_PDS];
static int ngpus;
static pthread_mutex_t gpus_lock = PTHREAD_MUTEX_INITIALIZER;

static const promo_gpu *gpu_of(const vkb_srv_table *t, VkPhysicalDevice pd)
{
    const vkb_dispatch *d = &t->real;
    const vkb_srv_table *inst = t->is_device ? t->parent : t;
    const promo_gpu *list = inst && inst->promote_ui ? gpus_ui : gpus;
    VkPhysicalDeviceIDProperties id = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &id};
    if (!d->vkGetPhysicalDeviceProperties2) return NULL;
    d->vkGetPhysicalDeviceProperties2(pd, &p2);
    const promo_gpu *r = NULL;
    pthread_mutex_lock(&gpus_lock);
    for (int i = 0; i < ngpus && !r; i++)
        if (!memcmp(list[i].uuid, id.deviceUUID, VK_UUID_SIZE)) r = &list[i];
    pthread_mutex_unlock(&gpus_lock);
    return r;
}

static int rows_checked = -1;

/* Each property range the same length in both structures (a header with another layout would
 * copy garbage: the row is skipped then). */
static int check_rows(void)
{
    if (rows_checked >= 0) return rows_checked;
    rows_checked = 1;
    for (size_t i = 0; i < NPROPS; i++)
        if (props[i].len != props[i].ext_len) {
            VKB_ERR("promotion: property row %zu: %u bytes against %u; not copied", i, props[i].len, props[i].ext_len);
            rows_checked = 0;
        }
    return rows_checked;
}

/* The core structures from the extensions' (after a features query). */
static void fill_core_features(const translate *x, const promo_gpu *g, VkBaseOutStructure *core[3])
{
    for (int c = 0; c < 3; c++) {
        if (!core[c]) continue;
        size_t sz = c == 0 ? sizeof(VkPhysicalDeviceVulkan11Features) : c == 1 ? sizeof(VkPhysicalDeviceVulkan12Features)
                                                                                : sizeof(VkPhysicalDeviceVulkan13Features);
        memset((uint8_t *)core[c] + sizeof(VkBaseOutStructure), 0, sz - sizeof(VkBaseOutStructure));
    }
    for (size_t i = 0; i < NFEATS; i++) {
        const feat_row *r = &feats[i];
        VkBaseOutStructure *c = core[r->core - 11];
        if (!c) continue;
        VkBool32 v = VK_FALSE;
        if (r->ext_off == NONE) v = exts_has(x->exts, feat_structs[r->ef].ext);
        else if (x->slots[r->ef].p) v = *(const VkBool32 *)((const uint8_t *)x->slots[r->ef].p + r->ext_off);
        if (r->ef == EF_PRIVATE) v = VK_TRUE; /* the client's own (descriptor.c) */
        *(VkBool32 *)((uint8_t *)c + r->core_off) = v;
    }
    /* A shuffle where the driver has them, else a chain of constant-lane broadcasts (spirv_downgrade.h). */
    if (core[1]) ((VkPhysicalDeviceVulkan12Features *)core[1])->subgroupBroadcastDynamicId = g && (g->shuffle || g->lanes);
}

static void fill_core_props(const translate *x, VkBaseOutStructure *core[3], const VkPhysicalDeviceLimits *limits)
{
    int ok = check_rows();
    for (size_t i = 0; i < NPROPS && ok; i++) {
        const prop_row *r = &props[i];
        VkBaseOutStructure *c = core[r->core - 11];
        if (!c) continue;
        if (x->slots[r->ep].p) memcpy((uint8_t *)c + r->core_off, (const uint8_t *)x->slots[r->ep].p + r->ext_off, r->len);
        else memset((uint8_t *)c + r->core_off, 0, r->len);
    }
    if (core[1]) ((VkPhysicalDeviceVulkan12Properties *)core[1])->framebufferIntegerColorSampleCounts = VK_SAMPLE_COUNT_1_BIT;
    if (core[2]) {
        VkPhysicalDeviceVulkan13Properties *p = (void *)core[2];
        /* No extension says it; one block of the largest size always fits. */
        p->maxInlineUniformTotalSize = p->maxInlineUniformBlockSize;
        if (!x->slots[EP_TEXALIGN].p) {
            /* Without VK_EXT_texel_buffer_alignment the 1.0 limit holds for every texel buffer (0
             * here had apps aligning offsets to 0). */
            p->storageTexelBufferOffsetAlignmentBytes = limits->minTexelBufferOffsetAlignment;
            p->uniformTexelBufferOffsetAlignmentBytes = limits->minTexelBufferOffsetAlignment;
            p->storageTexelBufferOffsetSingleTexelAlignment = VK_FALSE;
            p->uniformTexelBufferOffsetSingleTexelAlignment = VK_FALSE;
        }
    }
}

typedef struct query_ctx {
    const vkb_dispatch *d;
    VkPhysicalDevice pd;
} query_ctx;

static void q_features(void *ctx, void *head)
{
    query_ctx *q = ctx;
    q->d->vkGetPhysicalDeviceFeatures2(q->pd, head);
}

static void q_props(void *ctx, void *head)
{
    query_ctx *q = ctx;
    q->d->vkGetPhysicalDeviceProperties2(q->pd, head);
}

static void append(char *buf, size_t cap, const char *s)
{
    size_t l = strlen(buf);
    if (l + strlen(s) + 3 < cap) snprintf(buf + l, cap - l, "%s%s", l ? ", " : "", s);
}

/* A GPU nothing is promoted on (a driver without the 1.1 queries, no memory): recorded as it is, so
 * that apps-vulkan says what games see on it instead of being left from another session. */
static void record_plain(const vkb_dispatch *d, VkPhysicalDevice pd)
{
    promo_gpu g;
    memset(&g, 0, sizeof(g));
    VkPhysicalDeviceProperties p;
    VkPhysicalDeviceFeatures f;
    d->vkGetPhysicalDeviceProperties(pd, &p);
    d->vkGetPhysicalDeviceFeatures(pd, &f);
    g.real_api = p.apiVersion;
    g.drv_api = driver_api(p.apiVersion);
    g.offered = g.drv_api;
    snprintf(g.name, sizeof(g.name), "%s", p.deviceName);
    g.push = p.limits.maxPushConstantsSize;
    g.int64 = f.shaderInt64 != 0;
    pthread_mutex_lock(&gpus_lock);
    if (ngpus < VKB_MAX_PDS) gpus_ui[ngpus] = g, gpus[ngpus++] = g;
    pthread_mutex_unlock(&gpus_lock);
}

void vkb_promote_decide(const vkb_dispatch *d, VkPhysicalDevice pd)
{
    if (ngpus >= VKB_MAX_PDS) return;
    if (!d->vkGetPhysicalDeviceFeatures2 || !d->vkGetPhysicalDeviceProperties2) {
        record_plain(d, pd);
        return;
    }
    promo_gpu g;
    memset(&g, 0, sizeof(g));
    VkPhysicalDeviceIDProperties id = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    VkPhysicalDeviceSubgroupProperties sg = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES, &id};
    VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &sg};
    d->vkGetPhysicalDeviceProperties2(pd, &p2);
    memcpy(g.uuid, id.deviceUUID, VK_UUID_SIZE);
    g.real_api = p2.properties.apiVersion;
    g.drv_api = driver_api(p2.properties.apiVersion);
    g.offered = g.drv_api;
    snprintf(g.name, sizeof(g.name), "%s", p2.properties.deviceName);
    g.push = p2.properties.limits.maxPushConstantsSize;
    VkPhysicalDeviceFeatures2 base = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    d->vkGetPhysicalDeviceFeatures2(pd, &base);
    g.int64 = base.features.shaderInt64 != 0;
    ext_list el;
    exts_get(d, pd, &el);
    uint32_t m = VMINOR(g.drv_api);
    g.spv_max = m >= 3 ? VKB_SPV_1_6 : m == 2 ? VKB_SPV_1_5 : exts_has(&el, "VK_KHR_spirv_1_4") ? VKB_SPV_1_4 : 0x00010300u;
    g.shuffle = (sg.supportedOperations & VK_SUBGROUP_FEATURE_SHUFFLE_BIT) != 0 && !vkb_emu_faked("shuffle");
    if (!g.shuffle) {
        /* every lane a subgroup can have: the driver's size, or with subgroup size control its largest */
        g.lanes = sg.subgroupSize;
        if (exts_has(&el, "VK_EXT_subgroup_size_control")) {
            VkPhysicalDeviceSubgroupSizeControlProperties sc = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES};
            VkPhysicalDeviceProperties2 q2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &sc};
            d->vkGetPhysicalDeviceProperties2(pd, &q2);
            if (sc.maxSubgroupSize > g.lanes) g.lanes = sc.maxSubgroupSize;
        }
        if (!g.lanes) g.lanes = 1;
        if (g.lanes > 128) g.lanes = 128;
    }
    const char *src;
    int forced;
    uint32_t want = promote_wanted(&src, &forced);
    const char *name = p2.properties.deviceName;
    int allowed = forced || tested_driver(name, g.real_api);
    if (!forced && allowed) src = "tested on this driver";
    if (!want || !BELOW(g.drv_api, V13) || BELOW(g.drv_api, V11)) {
        if (BELOW(g.drv_api, V13) && !BELOW(g.drv_api, V11))
            VKB_INFO("promotion: %s stays at Vulkan 1.%u (VKBRIDGE_PROMOTE_API=0)", name, m);
        free(el.e);
        pthread_mutex_lock(&gpus_lock);
        gpus_ui[ngpus] = g;
        gpus[ngpus++] = g;
        pthread_mutex_unlock(&gpus_lock);
        return;
    }
    g.active = 1;
    /* The features as a promoted device would report them. */
    VkPhysicalDeviceVulkan11Features f11 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &f11};
    VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, &f12};
    VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f13};
    translate *x = calloc(1, sizeof(*x));
    if (!x) {
        free(el.e);
        record_plain(d, pd);
        return;
    }
    x->drv = g.drv_api;
    x->exts = &el;
    query_ctx q = {d, pd};
    chain_query(x, &f2, q_features, &q);
    fill_core_features(x, &g, x->core);
    free(x);
    /* Each version above the driver's in turn, while every piece is there. */
    for (uint32_t v = m + 1; v <= 3 && v <= VMINOR(want); v++) {
        uint32_t ver = VK_MAKE_API_VERSION(0, 1, v, 0);
        char miss[480] = "";
        for (size_t i = 0; i < NPLAIN; i++)
            if (plain_exts[i].version == ver && plain_exts[i].need && !exts_has(&el, plain_exts[i].ext)) append(miss, sizeof(miss), plain_exts[i].ext);
        for (size_t i = 0; i < NNEEDS; i++) {
            if (needs[i].version != ver) continue;
            const void *s = needs[i].core == 12 ? (const void *)&f12 : (const void *)&f13;
            if (*(const VkBool32 *)((const uint8_t *)s + needs[i].off)) continue;
            /* named by the extension it would come from */
            char what[160];
            const char *ext = NULL;
            for (size_t r = 0; r < NFEATS && !ext; r++)
                if (feats[r].core == needs[i].core && feats[r].core_off == needs[i].off) ext = feat_structs[feats[r].ef].ext;
            if (!strcmp(needs[i].name, "subgroupBroadcastDynamicId")) ext = "subgroup shuffles";
            snprintf(what, sizeof(what), "%s (%s)", ext ? ext : "?", needs[i].name);
            append(miss, sizeof(miss), what);
        }
        if (v == 3) {
            /* the extended dynamic states have no core bit: their extension's own */
            static const uint8_t eds[] = {EF_EDS, EF_EDS2};
            for (size_t i = 0; i < 2; i++)
                if (!exts_has(&el, feat_structs[eds[i]].ext)) append(miss, sizeof(miss), feat_structs[eds[i]].ext);
        }
        if (*miss) {
            snprintf(g.missing, sizeof(g.missing), "Vulkan 1.%u lacks %s", v, miss);
            break;
        }
        g.offered = VK_MAKE_API_VERSION(0, 1, v, VK_API_VERSION_PATCH(g.drv_api));
    }
    free(el.e);
    promo_gpu ui = g; /* gamescope's: promoted when complete, tested or not */
    if (g.offered != g.drv_api && !allowed) {
        /* not tested on this driver: said, and the app sees the driver's own version */
        VKB_INFO("promotion: %s would offer Vulkan 1.%u on its Vulkan 1.%u driver%s; off: not tested on this driver (VKBRIDGE_PROMOTE_API=1 "
                 "turns it on)", name, VMINOR(g.offered), m, fake_api() ? " (VKBRIDGE_FAKE_API)" : "");
        g.offered = g.drv_api;
        g.active = 0;
    } else if (g.offered != g.drv_api)
        VKB_INFO("promotion: %s offered as Vulkan 1.%u on its Vulkan 1.%u driver%s (%s; SPIR-V above %u.%u lowered to it)", name,
                 VMINOR(g.offered), m, fake_api() ? " (VKBRIDGE_FAKE_API)" : "", src, (g.spv_max >> 16) & 0xff, (g.spv_max >> 8) & 0xff);
    else
        VKB_INFO("promotion: %s stays at Vulkan 1.%u%s", name, m, fake_api() ? " (VKBRIDGE_FAKE_API)" : "");
    if (*g.missing) VKB_INFO("promotion: %s: %s", name, g.missing);
    if (!g.shuffle && ui.offered != ui.drv_api && !BELOW(ui.offered, V12))
        VKB_INFO("promotion: %s has no subgroup shuffles: a dynamic-id broadcast (Vulkan 1.2) becomes %u constant-lane broadcasts and selects",
                 name, g.lanes);
    if (ui.offered != g.offered)
        VKB_INFO("promotion: %s offered to gamescope as Vulkan 1.%u (Steam mode's compositor; games keep 1.%u)", name, VMINOR(ui.offered),
                 VMINOR(g.offered));
    pthread_mutex_lock(&gpus_lock);
    gpus_ui[ngpus] = ui;
    gpus[ngpus++] = g;
    pthread_mutex_unlock(&gpus_lock);
}

int vkb_promote_describe(const uint8_t uuid[VK_UUID_SIZE], uint32_t *offered, uint32_t *drv, const char **missing)
{
    int r = 0;
    pthread_mutex_lock(&gpus_lock);
    for (int i = 0; i < ngpus && !r; i++)
        if (!memcmp(gpus[i].uuid, uuid, VK_UUID_SIZE)) {
            *offered = gpus[i].offered;
            *drv = gpus[i].drv_api;
            *missing = gpus[i].missing;
            r = 1;
        }
    pthread_mutex_unlock(&gpus_lock);
    return r;
}

/*
 * <cache dir>/apps-vulkan: what games see on the first GPU, for the launcher to choose a DXVK that
 * starts on it before the next session (DroidDeck's AppVulkan: DXVK 3 needs Vulkan 1.3, shaderInt64
 * and 256 bytes of push constants; 1.10 starts on 1.1). key=value lines, replaced atomically:
 *   device, api (what games see: the offered version), driver_api (the driver's own), promotion
 *   (on: api is above driver_api by promotion; off: api is the driver's), shader_int64,
 *   max_push_constants, server (the build that wrote it).
 * Written at every start, after the tested list was applied, so it never says a version that is
 * not offered; removed when no GPU could be read, so that an old one is not taken for this one.
 */
void vkb_promote_write_apps_file(void)
{
    const char *dir = vkb_pcache_dir();
    if (!dir || !*dir) return;
    char path[1024], tmp[1060], text[640];
    snprintf(path, sizeof(path), "%s/apps-vulkan", dir);
    pthread_mutex_lock(&gpus_lock);
    if (!ngpus) {
        pthread_mutex_unlock(&gpus_lock);
        if (unlink(path) == 0) VKB_INFO("%s removed: no GPU read at this start", path);
        return;
    }
    const promo_gpu *g = &gpus[0];
    int promoted = g->active && g->offered != g->drv_api;
    snprintf(text, sizeof(text), "device=%s\napi=%u.%u.%u\ndriver_api=%u.%u.%u\npromotion=%s\nshader_int64=%d\nmax_push_constants=%u\nserver=%s\n",
             g->name, VK_API_VERSION_MAJOR(g->offered), VK_API_VERSION_MINOR(g->offered), VK_API_VERSION_PATCH(g->offered),
             VK_API_VERSION_MAJOR(g->drv_api), VK_API_VERSION_MINOR(g->drv_api), VK_API_VERSION_PATCH(g->drv_api), promoted ? "on" : "off",
             g->int64, g->push, VKB_GIT);
    pthread_mutex_unlock(&gpus_lock);
    snprintf(tmp, sizeof(tmp), "%s.%d.tmp", path, (int)getpid());
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    size_t len = strlen(text);
    int ok = fd >= 0 && write(fd, text, len) == (ssize_t)len && fsync(fd) == 0;
    int e = errno;
    if (fd >= 0 && close(fd) != 0 && ok) ok = 0, e = errno;
    if (ok && rename(tmp, path) != 0) ok = 0, e = errno;
    if (!ok) {
        if (vkb_cache_write_failed("apps-vulkan", e)) VKB_WARN("could not write %s: %s", path, strerror(e));
        unlink(tmp);
    }
}

/* ------------------------------------------------------------------ the app's queries */

static VKAPI_ATTR void VKAPI_CALL pr_GetPhysicalDeviceProperties(VkPhysicalDevice pd, VkPhysicalDeviceProperties *p)
{
    vkb_srv_table *t = vkb_srv_current_table();
    if (!t) return;
    t->real.vkGetPhysicalDeviceProperties(pd, p);
    const promo_gpu *g = gpu_of(t, pd);
    if (g) p->apiVersion = g->offered;
}

static VKAPI_ATTR void VKAPI_CALL pr_GetPhysicalDeviceProperties2(VkPhysicalDevice pd, VkPhysicalDeviceProperties2 *p)
{
    vkb_srv_table *t = vkb_srv_current_table();
    if (!t) return;
    const promo_gpu *g = gpu_of(t, pd);
    if (!g || !g->active) {
        t->real.vkGetPhysicalDeviceProperties2(pd, p);
        if (g) p->properties.apiVersion = g->offered;
        return;
    }
    ext_list el;
    exts_get(&t->real, pd, &el);
    translate *x = calloc(1, sizeof(*x));
    if (!x) {
        free(el.e);
        t->real.vkGetPhysicalDeviceProperties2(pd, p);
        return;
    }
    x->drv = g->drv_api;
    x->exts = &el;
    x->props_query = 1;
    query_ctx q = {&t->real, pd};
    chain_query(x, p, q_props, &q);
    fill_core_props(x, x->core, &p->properties.limits);
    /* the structures of extensions the bridge does itself */
    p->properties.apiVersion = g->offered;
    free(x);
    free(el.e);
}

static VKAPI_ATTR void VKAPI_CALL pr_GetPhysicalDeviceFeatures2(VkPhysicalDevice pd, VkPhysicalDeviceFeatures2 *f)
{
    vkb_srv_table *t = vkb_srv_current_table();
    if (!t) return;
    const promo_gpu *g = gpu_of(t, pd);
    if (!g || !g->active) {
        t->real.vkGetPhysicalDeviceFeatures2(pd, f);
        return;
    }
    ext_list el;
    exts_get(&t->real, pd, &el);
    translate *x = calloc(1, sizeof(*x));
    if (!x) {
        free(el.e);
        t->real.vkGetPhysicalDeviceFeatures2(pd, f);
        return;
    }
    x->drv = g->drv_api;
    x->exts = &el;
    query_ctx q = {&t->real, pd};
    chain_query(x, f, q_features, &q);
    fill_core_features(x, g, x->core);
    if (!BELOW(g->offered, V13)) {
        VkBaseOutStructure *pv = find_in_chain(f, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIVATE_DATA_FEATURES);
        if (pv) ((VkPhysicalDevicePrivateDataFeatures *)pv)->privateData = VK_TRUE; /* the client's own */
    }
    free(x);
    free(el.e);
}

void vkb_promote_instance_install(vkb_srv_table *t)
{
    if (!t->real.vkGetPhysicalDeviceProperties2 || !t->real.vkGetPhysicalDeviceFeatures2) return;
    int any = 0;
    pthread_mutex_lock(&gpus_lock);
    const promo_gpu *list = t->promote_ui ? gpus_ui : gpus;
    for (int i = 0; i < ngpus; i++) any |= list[i].offered != list[i].real_api || list[i].active; /* VKBRIDGE_FAKE_API alone too */
    pthread_mutex_unlock(&gpus_lock);
    if (!any) return;
    t->dt.vkGetPhysicalDeviceProperties = pr_GetPhysicalDeviceProperties;
    t->dt.vkGetPhysicalDeviceProperties2 = pr_GetPhysicalDeviceProperties2;
    t->dt.vkGetPhysicalDeviceFeatures2 = pr_GetPhysicalDeviceFeatures2;
}

/* ------------------------------------------------------------------ vkCreateDevice */

static int list_has(const char *const *l, uint32_t n, const char *s)
{
    for (uint32_t i = 0; i < n; i++)
        if (l[i] && !strcmp(l[i], s)) return 1;
    return 0;
}

/* The chain's structure of type st, else a new zeroed one at the chain's head. */
static VkBaseOutStructure *ensure(VkDeviceCreateInfo *ci, const ext_struct *s, vkb_arena *a)
{
    for (VkBaseOutStructure *b = (VkBaseOutStructure *)ci->pNext; b; b = b->pNext)
        if (b->sType == s->stype) return b;
    VkBaseOutStructure *n = vkb_arena_alloc(a, s->size);
    if (!n) return NULL;
    memset(n, 0, s->size);
    n->sType = s->stype;
    n->pNext = (VkBaseOutStructure *)ci->pNext;
    ci->pNext = n;
    return n;
}

const char **vkb_promote_device_create_info(vkb_srv_table *inst, VkPhysicalDevice pd, VkDeviceCreateInfo *ci, const char **exts,
                                            uint32_t *n, vkb_arena *a)
{
    const promo_gpu *g = gpu_of(inst, pd);
    if (!g || !g->active) return exts;
    uint32_t app = inst->api_version ? inst->api_version : V11;
    uint32_t eff = BELOW(app, g->offered) ? app : g->offered; /* the version the app's device has */
    ext_list el;
    exts_get(&inst->real, pd, &el);
    const char **out = vkb_arena_alloc(a, (*n + EF_COUNT + NPLAIN + 8) * sizeof(char *));
    if (!out) {
        free(el.e);
        return exts;
    }
    memcpy(out, exts, *n * sizeof(char *));
    uint32_t k = *n;
    /* The core structures the driver does not know become the extensions' ones. */
    char turned[160] = "";
    for (VkBaseOutStructure *prev = (VkBaseOutStructure *)ci; prev->pNext;) {
        VkBaseOutStructure *b = prev->pNext;
        int c = core_of(b->sType);
        if (!c || !is_core_struct(b->sType, g->drv_api, 0)) {
            prev = b;
            continue;
        }
        prev->pNext = b->pNext; /* out of the chain (it lives in the request's arena) */
        for (size_t i = 0; i < NFEATS; i++) {
            const feat_row *r = &feats[i];
            if (r->core != c || !*(const VkBool32 *)((const uint8_t *)b + r->core_off)) continue;
            const ext_struct *s = &feat_structs[r->ef];
            if (!exts_has(&el, s->ext)) continue; /* the bridge's own (privateData) or not offered */
            if (s->ext && !list_has(out, k, s->ext)) out[k++] = s->ext;
            if (r->ext_off == NONE) continue;
            VkBaseOutStructure *e = ensure(ci, s, a);
            if (e) *(VkBool32 *)((uint8_t *)e + r->ext_off) = VK_TRUE;
        }
        char one[24];
        snprintf(one, sizeof(one), "%s1.%d", *turned ? ", " : "", c - 10);
        strncat(turned, one, sizeof(turned) - strlen(turned) - 1);
    }
    /* The promoted version's commands: from the extensions, enabled. */
    if (BELOW(g->drv_api, eff)) {
        for (uint32_t i = 0; i < EF_COUNT; i++) {
            const ext_struct *s = &feat_structs[i];
            if (s->ext && !BELOW(eff, s->version) && BELOW(g->drv_api, s->version) && exts_has(&el, s->ext) && !list_has(out, k, s->ext) &&
                strcmp(s->ext, "VK_EXT_private_data"))
                out[k++] = s->ext;
        }
        for (size_t i = 0; i < NPLAIN; i++)
            if (!BELOW(eff, plain_exts[i].version) && exts_has(&el, plain_exts[i].ext) && !list_has(out, k, plain_exts[i].ext) &&
                strcmp(plain_exts[i].ext, "VK_EXT_tooling_info"))
                out[k++] = plain_exts[i].ext;
    }
    /* 1.3's features without a core bit, on where the driver has them. */
    if (BELOW(g->drv_api, V13) && !BELOW(eff, V13) && inst->real.vkGetPhysicalDeviceFeatures2) {
        for (size_t i = 0; i < NIMPLICIT; i++) {
            const ext_struct *s = &feat_structs[implicit13[i].ef];
            if (!exts_has(&el, s->ext)) continue;
            uint8_t have[STORE];
            if (s->size > STORE) continue;
            memset(have, 0, s->size);
            ((VkBaseOutStructure *)have)->sType = s->stype;
            VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, have};
            inst->real.vkGetPhysicalDeviceFeatures2(pd, &f2);
            if (!*(VkBool32 *)(have + implicit13[i].off)) continue;
            VkBaseOutStructure *e = ensure(ci, s, a);
            if (e) *(VkBool32 *)((uint8_t *)e + implicit13[i].off) = VK_TRUE;
            if (!list_has(out, k, s->ext)) out[k++] = s->ext;
        }
    }
    free(el.e);
    if (*turned || k != *n)
        VKB_INFO("vkCreateDevice: promoted to Vulkan 1.%u on a 1.%u driver: %s%s%u extension(s) for its core", VMINOR(eff), VMINOR(g->drv_api),
                 *turned ? turned : "", *turned ? " feature structures turned into the extensions'; " : "", k - *n);
    *n = k;
    return out;
}

/* ------------------------------------------------------------------ VKBRIDGE_FAKE_API's commands */

/* Core commands of 1.2 and 1.3 that came from extensions: a driver below that version has them
 * under the extension's name only. */
static const struct {
    uint32_t version;
    const char *name;
} core_cmds[] = {
    {V12, "vkCmdDrawIndirectCount"}, {V12, "vkCmdDrawIndexedIndirectCount"}, {V12, "vkCreateRenderPass2"},
    {V12, "vkCmdBeginRenderPass2"}, {V12, "vkCmdNextSubpass2"}, {V12, "vkCmdEndRenderPass2"}, {V12, "vkResetQueryPool"},
    {V12, "vkGetSemaphoreCounterValue"}, {V12, "vkWaitSemaphores"}, {V12, "vkSignalSemaphore"}, {V12, "vkGetBufferDeviceAddress"},
    {V12, "vkGetBufferOpaqueCaptureAddress"}, {V12, "vkGetDeviceMemoryOpaqueCaptureAddress"},
    {V13, "vkCreatePrivateDataSlot"}, {V13, "vkDestroyPrivateDataSlot"}, {V13, "vkSetPrivateData"}, {V13, "vkGetPrivateData"},
    {V13, "vkCmdSetEvent2"}, {V13, "vkCmdResetEvent2"}, {V13, "vkCmdWaitEvents2"}, {V13, "vkCmdPipelineBarrier2"},
    {V13, "vkCmdWriteTimestamp2"}, {V13, "vkQueueSubmit2"}, {V13, "vkCmdCopyBuffer2"}, {V13, "vkCmdCopyImage2"},
    {V13, "vkCmdCopyBufferToImage2"}, {V13, "vkCmdCopyImageToBuffer2"}, {V13, "vkCmdBlitImage2"}, {V13, "vkCmdResolveImage2"},
    {V13, "vkCmdBeginRendering"}, {V13, "vkCmdEndRendering"}, {V13, "vkCmdSetCullMode"}, {V13, "vkCmdSetFrontFace"},
    {V13, "vkCmdSetPrimitiveTopology"}, {V13, "vkCmdSetViewportWithCount"}, {V13, "vkCmdSetScissorWithCount"},
    {V13, "vkCmdBindVertexBuffers2"}, {V13, "vkCmdSetDepthTestEnable"}, {V13, "vkCmdSetDepthWriteEnable"},
    {V13, "vkCmdSetDepthCompareOp"}, {V13, "vkCmdSetDepthBoundsTestEnable"}, {V13, "vkCmdSetStencilTestEnable"},
    {V13, "vkCmdSetStencilOp"}, {V13, "vkCmdSetRasterizerDiscardEnable"}, {V13, "vkCmdSetDepthBiasEnable"},
    {V13, "vkCmdSetPrimitiveRestartEnable"}, {V13, "vkGetDeviceBufferMemoryRequirements"}, {V13, "vkGetDeviceImageMemoryRequirements"},
    {V13, "vkGetDeviceImageSparseMemoryRequirements"},
};

static PFN_vkGetDeviceProcAddr fake_next;

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL fake_gdpa(VkDevice dev, const char *name)
{
    uint32_t f = fake_api();
    for (size_t i = 0; i < sizeof(core_cmds) / sizeof(core_cmds[0]); i++)
        if (BELOW(f, core_cmds[i].version) && !strcmp(name, core_cmds[i].name)) return NULL;
    return fake_next(dev, name);
}

PFN_vkGetDeviceProcAddr vkb_promote_gdpa(PFN_vkGetDeviceProcAddr real)
{
    if (!fake_api() || !BELOW(fake_api(), V13)) return real;
    fake_next = real;
    return fake_gdpa;
}

/* ------------------------------------------------------------------ SPIR-V lowered for the driver */

typedef struct lower_dev {
    VkDevice dev;
    uint32_t target;
    uint32_t lanes; /* no shuffles: dynamic-id broadcasts as chains over this many lanes (0: shuffles) */
    PFN_vkCreateShaderModule create_module;
    PFN_vkCreateGraphicsPipelines create_gfx;
    PFN_vkCreateComputePipelines create_comp;
} lower_dev;

#define LOWER_DEVS 64
static lower_dev lower_devs[LOWER_DEVS];
static pthread_mutex_t lower_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned long lowered_total;

static int lower_find(VkDevice d, lower_dev *out)
{
    int r = 0;
    pthread_mutex_lock(&lower_lock);
    for (int i = 0; i < LOWER_DEVS && !r; i++)
        if (lower_devs[i].dev == d) {
            *out = lower_devs[i];
            r = 1;
        }
    pthread_mutex_unlock(&lower_lock);
    return r;
}

/* The module for the driver: code itself (0), a lowered copy (1, *out to free), or -1 (logged). */
static int lower(uint32_t target, uint32_t lanes, const uint32_t *code, size_t size, uint32_t **out, size_t *out_size)
{
    vkb_downgrade_stats st;
    const char *why;
    int r = vkb_spv_downgrade_ex(code, size, target, lanes, out, out_size, &st, &why);
    if (r < 0) {
        VKB_ONCE(VKB_LOG_WARN, "promotion: a SPIR-V %u.%u module not lowered to %u.%u (%s); given to the driver as it is", (st.from >> 16) & 0xff,
                 (st.from >> 8) & 0xff, (target >> 16) & 0xff, (target >> 8) & 0xff, why ? why : "?");
        return -1;
    }
    if (r > 0) {
        unsigned long n = __atomic_add_fetch(&lowered_total, 1, __ATOMIC_RELAXED);
        if (n == 1 || !(n & (n - 1)))
            VKB_INFO("promotion: SPIR-V %u.%u lowered to %u.%u (%lu module(s) so far; this one: %u extension(s) declared, %u layer "
                     "capabilities, %u dynamic broadcast(s) made shuffles, %u made chains over %u lanes)",
                     (st.from >> 16) & 0xff, (st.from >> 8) & 0xff, (target >> 16) & 0xff, (target >> 8) & 0xff, n, st.extensions,
                     st.layer_caps, st.broadcasts, st.unrolled, lanes);
    }
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL lw_CreateShaderModule(VkDevice device, const VkShaderModuleCreateInfo *ci,
                                                           const VkAllocationCallbacks *a, VkShaderModule *m)
{
    lower_dev l;
    if (!lower_find(device, &l)) return VK_ERROR_INITIALIZATION_FAILED;
    uint32_t *code = NULL;
    size_t size = 0;
    if (lower(l.target, l.lanes, ci->pCode, ci->codeSize, &code, &size) <= 0) return l.create_module(device, ci, a, m);
    VkShaderModuleCreateInfo c = *ci;
    c.pCode = code;
    c.codeSize = size;
    VkResult r = l.create_module(device, &c, a, m);
    free(code);
    return r;
}

/* Stages with their module inline (VkShaderModuleCreateInfo in the stage's chain, maintenance5):
 * the structure lives in the request's arena, so its code is swapped for the lowered copy during
 * the call and put back after it. */
typedef struct inline_swap {
    VkShaderModuleCreateInfo *smci;
    const uint32_t *code;
    size_t size;
    uint32_t *lowered;
} inline_swap;

static VkShaderModuleCreateInfo *inline_module(const VkPipelineShaderStageCreateInfo *st)
{
    for (const VkBaseInStructure *b = st->pNext; b; b = b->pNext)
        if (b->sType == VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO) return (VkShaderModuleCreateInfo *)b;
    return NULL;
}

/* Returns how many swaps were made into sw (cap of them). */
static uint32_t swap_stages(uint32_t target, uint32_t lanes, uint32_t n, const VkPipelineShaderStageCreateInfo *st, inline_swap *sw, uint32_t have, uint32_t cap)
{
    for (uint32_t i = 0; i < n && have < cap; i++) {
        VkShaderModuleCreateInfo *m = inline_module(&st[i]);
        if (!m || m->codeSize < 8 || m->pCode[1] <= target) continue;
        uint32_t *code = NULL;
        size_t size = 0;
        if (lower(target, lanes, m->pCode, m->codeSize, &code, &size) <= 0) continue;
        sw[have] = (inline_swap){m, m->pCode, m->codeSize, code};
        m->pCode = code;
        m->codeSize = size;
        have++;
    }
    return have;
}

static void swap_back(inline_swap *sw, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        sw[i].smci->pCode = sw[i].code;
        sw[i].smci->codeSize = sw[i].size;
        free(sw[i].lowered);
    }
}

#define SWAPS 256

static VKAPI_ATTR VkResult VKAPI_CALL lw_CreateGraphicsPipelines(VkDevice device, VkPipelineCache cache, uint32_t n,
                                                                const VkGraphicsPipelineCreateInfo *infos, const VkAllocationCallbacks *a,
                                                                VkPipeline *out)
{
    lower_dev l;
    if (!lower_find(device, &l)) return VK_ERROR_INITIALIZATION_FAILED;
    inline_swap sw[SWAPS];
    uint32_t ns = 0;
    for (uint32_t i = 0; i < n; i++) ns = swap_stages(l.target, l.lanes, infos[i].stageCount, infos[i].pStages, sw, ns, SWAPS);
    VkResult r = l.create_gfx(device, cache, n, infos, a, out);
    swap_back(sw, ns);
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL lw_CreateComputePipelines(VkDevice device, VkPipelineCache cache, uint32_t n,
                                                               const VkComputePipelineCreateInfo *infos, const VkAllocationCallbacks *a,
                                                               VkPipeline *out)
{
    lower_dev l;
    if (!lower_find(device, &l)) return VK_ERROR_INITIALIZATION_FAILED;
    inline_swap sw[SWAPS];
    uint32_t ns = 0;
    for (uint32_t i = 0; i < n; i++) ns = swap_stages(l.target, l.lanes, 1, &infos[i].stage, sw, ns, SWAPS);
    VkResult r = l.create_comp(device, cache, n, infos, a, out);
    swap_back(sw, ns);
    return r;
}

/* Beneath every layer (before t->dt is made from t->real): what reaches the driver is lowered. */
void vkb_promote_device_init(vkb_srv_table *t)
{
    const promo_gpu *g = gpu_of(t, t->physical);
    if (!g || !g->active || !BELOW(g->drv_api, g->offered)) return;
    if (!t->real.vkCreateShaderModule || !t->real.vkCreateGraphicsPipelines || !t->real.vkCreateComputePipelines) return;
    pthread_mutex_lock(&lower_lock);
    int slot = -1;
    for (int i = 0; i < LOWER_DEVS && slot < 0; i++)
        if (!lower_devs[i].dev) slot = i;
    if (slot >= 0) {
        lower_devs[slot] = (lower_dev){t->device, g->spv_max, g->lanes, t->real.vkCreateShaderModule, t->real.vkCreateGraphicsPipelines,
                                       t->real.vkCreateComputePipelines};
        t->real.vkCreateShaderModule = lw_CreateShaderModule;
        t->real.vkCreateGraphicsPipelines = lw_CreateGraphicsPipelines;
        t->real.vkCreateComputePipelines = lw_CreateComputePipelines;
    }
    pthread_mutex_unlock(&lower_lock);
    if (slot < 0) VKB_WARN("promotion: more than %d devices; this one's SPIR-V is not lowered", LOWER_DEVS);
}

void vkb_promote_device_destroy(vkb_srv_table *t)
{
    pthread_mutex_lock(&lower_lock);
    for (int i = 0; i < LOWER_DEVS; i++)
        if (lower_devs[i].dev == t->device) memset(&lower_devs[i], 0, sizeof(lower_devs[i]));
    pthread_mutex_unlock(&lower_lock);
}
