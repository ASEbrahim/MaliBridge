/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * SPIR-V version lowering for an API version the bridge offers above the driver's (server_promote.c).
 *
 * A Vulkan 1.2 device takes SPIR-V 1.5 and a 1.3 device SPIR-V 1.6; a 1.1 driver with
 * VK_KHR_spirv_1_4 takes 1.4. What 1.5 and 1.6 added over 1.4 is, nearly all of it, extensions
 * made core: the same opcodes, capabilities and enumerants, declared with an OpExtension in 1.4.
 * So a newer module becomes a 1.4 one by
 *   - the version word set to 1.4;
 *   - an OpExtension for every capability (or instruction) that 1.5/1.6 took from an extension:
 *       StorageBuffer8BitAccess, UniformAndStorageBuffer8BitAccess, StoragePushConstant8  SPV_KHR_8bit_storage
 *       ShaderNonUniform, RuntimeDescriptorArray, the *ArrayDynamicIndexing and
 *         *ArrayNonUniformIndexing ones of descriptor indexing                          SPV_EXT_descriptor_indexing
 *       VulkanMemoryModel, VulkanMemoryModelDeviceScope                                  SPV_KHR_vulkan_memory_model
 *       PhysicalStorageBufferAddresses                                                   SPV_KHR_physical_storage_buffer
 *       DemoteToHelperInvocation (1.6)                                                   SPV_EXT_demote_to_helper_invocation
 *       DotProduct, DotProductInput* (1.6)                                               SPV_KHR_integer_dot_product
 *       OpTerminateInvocation (1.6)                                                      SPV_KHR_terminate_invocation
 *   - the 1.5 capabilities ShaderViewportIndex and ShaderLayer replaced by ShaderViewportIndexLayerEXT
 *     (SPV_EXT_shader_viewport_index_layer; the vertex and tessellation evaluation stages only, as
 *     the 1.5 capabilities are where the driver lacks VK_EXT_shader_viewport_index_layer - the
 *     bridge then reports shaderOutputViewportIndex / shaderOutputLayer off);
 *   - OpGroupNonUniformBroadcast with an id that is not a constant (1.5's subgroupBroadcastDynamicId)
 *     made OpGroupNonUniformShuffle, which takes any id and gives the same value (capability
 *     GroupNonUniformShuffle; the bridge reports the feature only when the driver has shuffles).
 * Whether the driver can do what the module asks for is the features' business: the bridge reports
 * a feature only when the driver's extension has it.
 *
 * Our own code from the SPIR-V specification's change lists (no other implementation looked at).
 */
#ifndef VKB_SPIRV_DOWNGRADE_H
#define VKB_SPIRV_DOWNGRADE_H

#include <stddef.h>
#include <stdint.h>

#define VKB_SPV_1_4 0x00010400u
#define VKB_SPV_1_5 0x00010500u
#define VKB_SPV_1_6 0x00010600u

typedef struct vkb_downgrade_stats {
    uint32_t from;              /* the module's version */
    unsigned extensions;        /* OpExtension instructions added */
    unsigned layer_caps;        /* ShaderViewportIndex / ShaderLayer replaced */
    unsigned broadcasts;        /* dynamic-id broadcasts made shuffles */
} vkb_downgrade_stats;

/* A module above `target` (VKB_SPV_1_4 or 1.5) rewritten for it. 1: rewritten (*out malloc'd,
 * *out_size bytes), 0: nothing to do (the module is at or below target; nothing allocated), -1: not
 * possible (*why says why; the module is left as it is). st may be NULL. */
int vkb_spv_downgrade(const uint32_t *code, size_t size, uint32_t target, uint32_t **out, size_t *out_size,
                      vkb_downgrade_stats *st, const char **why);

#endif
