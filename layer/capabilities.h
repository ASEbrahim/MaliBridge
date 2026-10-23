/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <cstdint>
#include <vulkan/vulkan.h>

#include "layer_ctx.h"

extern "C" {
#include "vkb_wire.h"
}

uint32_t filter_device_create_info(VkPhysicalDevice physicalDevice,
                                   LayerCtx *parent,
                                   VkDeviceCreateInfo *pCreateInfo,
                                   vkb_arena *arena);
