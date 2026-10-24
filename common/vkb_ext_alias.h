/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * Device extensions a newer driver may expose only under their promoted KHR name, while apps still
 * ask for the EXT one: same structures (aliased sTypes), enumerants and commands. Where the driver
 * has the KHR name and not the EXT one, the client offers the EXT name too (physdev.c) and the
 * server enables the KHR one in its place at vkCreateDevice (server_instance.c).
 *
 * Mali-G925 on Android 16 (driver Vulkan 1.3.278) has VK_KHR_vertex_attribute_divisor only; vkd3d-proton
 * looks for VK_EXT_vertex_attribute_divisor (spec version 3) and refused the device ("Lacking
 * support for VK_EXT_vertex_attribute_divisor", The Witcher 3 DX12, 2026-10-14).
 *
 * The one structure that is not aliased - VkPhysicalDeviceVertexAttributeDivisorPropertiesEXT -
 * is filled by the client from the KHR properties (emu.c).
 */
#ifndef VKB_EXT_ALIAS_H
#define VKB_EXT_ALIAS_H

#include <stdint.h>

typedef struct vkb_ext_alias {
    const char *app;       /* the name apps ask for */
    const char *driver;    /* the name the driver may have instead */
    uint32_t spec;         /* the app name's spec version, as offered */
} vkb_ext_alias;

static const vkb_ext_alias vkb_ext_aliases[] = {
    {"VK_EXT_vertex_attribute_divisor", "VK_KHR_vertex_attribute_divisor", 3},
    {"VK_EXT_index_type_uint8", "VK_KHR_index_type_uint8", 1},
    {"VK_EXT_line_rasterization", "VK_KHR_line_rasterization", 1},
    {"VK_EXT_robustness2", "VK_KHR_robustness2", 1},
    {"VK_EXT_global_priority", "VK_KHR_global_priority", 2},
    {"VK_EXT_global_priority_query", "VK_KHR_global_priority", 1},
    {"VK_EXT_load_store_op_none", "VK_KHR_load_store_op_none", 1},
};

#define VKB_EXT_ALIAS_COUNT (sizeof(vkb_ext_aliases) / sizeof(vkb_ext_aliases[0]))

#endif
