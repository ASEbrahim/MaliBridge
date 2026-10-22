/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 ASEbrahim and MaliBridge contributors
 *
 * BCn emulation: what each block-compressed format is decoded into. Shared by the client (format
 * queries) and the server (image creation, decode). The decoded image is created with the UNORM
 * base format and MUTABLE_FORMAT so the decoder can write it through a storage view, and the
 * app's sRGB views still work.
 */
#ifndef VKB_EMU_FORMATS_H
#define VKB_EMU_FORMATS_H

#include <vulkan/vulkan.h>

static inline int vkb_is_bc(VkFormat f)
{
    return f >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && f <= VK_FORMAT_BC7_SRGB_BLOCK;
}

/* The multi-planar (YCbCr) formats: the Mali-G925 driver refuses VK_IMAGE_CREATE_EXTENDED_USAGE_BIT
 * on them */
static inline int vkb_is_multiplanar(VkFormat f)
{
    switch (f) {
    case VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM: case VK_FORMAT_G8_B8R8_2PLANE_420_UNORM:
    case VK_FORMAT_G8_B8_R8_3PLANE_422_UNORM: case VK_FORMAT_G8_B8R8_2PLANE_422_UNORM:
    case VK_FORMAT_G8_B8_R8_3PLANE_444_UNORM:
    case VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_420_UNORM_3PACK16: case VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16:
    case VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_422_UNORM_3PACK16: case VK_FORMAT_G10X6_B10X6R10X6_2PLANE_422_UNORM_3PACK16:
    case VK_FORMAT_G10X6_B10X6_R10X6_3PLANE_444_UNORM_3PACK16:
    case VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_420_UNORM_3PACK16: case VK_FORMAT_G12X4_B12X4R12X4_2PLANE_420_UNORM_3PACK16:
    case VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_422_UNORM_3PACK16: case VK_FORMAT_G12X4_B12X4R12X4_2PLANE_422_UNORM_3PACK16:
    case VK_FORMAT_G12X4_B12X4_R12X4_3PLANE_444_UNORM_3PACK16:
    case VK_FORMAT_G16_B16_R16_3PLANE_420_UNORM: case VK_FORMAT_G16_B16R16_2PLANE_420_UNORM:
    case VK_FORMAT_G16_B16_R16_3PLANE_422_UNORM: case VK_FORMAT_G16_B16R16_2PLANE_422_UNORM:
    case VK_FORMAT_G16_B16_R16_3PLANE_444_UNORM:
    case VK_FORMAT_G8_B8R8_2PLANE_444_UNORM: case VK_FORMAT_G10X6_B10X6R10X6_2PLANE_444_UNORM_3PACK16:
    case VK_FORMAT_G12X4_B12X4R12X4_2PLANE_444_UNORM_3PACK16: case VK_FORMAT_G16_B16R16_2PLANE_444_UNORM:
        return 1;
    default:
        return 0;
    }
}

/* The format an app's view of a BC format becomes (keeps sRGB/SNORM meaning). */
static inline VkFormat vkb_bc_view_format(VkFormat f)
{
    switch (f) {
    case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
    case VK_FORMAT_BC2_UNORM_BLOCK:
    case VK_FORMAT_BC3_UNORM_BLOCK:
    case VK_FORMAT_BC7_UNORM_BLOCK: return VK_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
    case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
    case VK_FORMAT_BC2_SRGB_BLOCK:
    case VK_FORMAT_BC3_SRGB_BLOCK:
    case VK_FORMAT_BC7_SRGB_BLOCK: return VK_FORMAT_R8G8B8A8_SRGB;
    case VK_FORMAT_BC4_UNORM_BLOCK: return VK_FORMAT_R8_UNORM;
    case VK_FORMAT_BC4_SNORM_BLOCK: return VK_FORMAT_R8_SNORM;
    case VK_FORMAT_BC5_UNORM_BLOCK: return VK_FORMAT_R8G8_UNORM;
    case VK_FORMAT_BC5_SNORM_BLOCK: return VK_FORMAT_R8G8_SNORM;
    case VK_FORMAT_BC6H_UFLOAT_BLOCK:
    case VK_FORMAT_BC6H_SFLOAT_BLOCK: return VK_FORMAT_R16G16B16A16_SFLOAT;
    default: return f;
    }
}

/* The 16-bit format BC1 UNORM decodes into where the image allows it (server_emu_bcn.c: not
 * mutable into other formats, not storage, and the GPU samples and blits the format; Vulkan makes
 * both mandatory), else UNDEFINED. No 16-bit sRGB formats exist, so sRGB BC1 stays RGBA8. Format
 * queries (the client) keep answering for vkb_bc_view_format. */
static inline VkFormat vkb_bc_packed_format(VkFormat f)
{
    switch (f) {
    case VK_FORMAT_BC1_RGB_UNORM_BLOCK: return VK_FORMAT_R5G6B5_UNORM_PACK16;
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: return VK_FORMAT_A1R5G5B5_UNORM_PACK16;
    default: return VK_FORMAT_UNDEFINED;
    }
}

static inline int vkb_bc_is_packed(VkFormat decoded)
{
    return decoded == VK_FORMAT_R5G6B5_UNORM_PACK16 || decoded == VK_FORMAT_A1R5G5B5_UNORM_PACK16;
}

/* The format the decoded image is created with and written through (storage-capable). */
static inline VkFormat vkb_bc_storage_format(VkFormat f)
{
    switch (vkb_bc_view_format(f)) {
    case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_R8_SNORM:
    case VK_FORMAT_R8_UNORM: return VK_FORMAT_R8_UNORM;
    case VK_FORMAT_R8G8_SNORM:
    case VK_FORMAT_R8G8_UNORM: return VK_FORMAT_R8G8_UNORM;
    case VK_FORMAT_R16G16B16A16_SFLOAT: return VK_FORMAT_R16G16B16A16_SFLOAT;
    default: return f;
    }
}

/* Bytes per block of a format BC is transcoded to (ETC2 / EAC / ASTC 4x4, HDR for BC6H, ASTC 6x6 /
 * 8x8 by choice; server_emu_bcn.c), 0 for any other. */
static inline uint32_t vkb_tx_block_bytes(VkFormat f)
{
    switch (f) {
    case VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK:
    case VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK:
    case VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK:
    case VK_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK:
    case VK_FORMAT_EAC_R11_UNORM_BLOCK:
    case VK_FORMAT_EAC_R11_SNORM_BLOCK: return 8;
    case VK_FORMAT_EAC_R11G11_UNORM_BLOCK:
    case VK_FORMAT_EAC_R11G11_SNORM_BLOCK:
    case VK_FORMAT_ASTC_4x4_UNORM_BLOCK:
    case VK_FORMAT_ASTC_4x4_SRGB_BLOCK:
    case VK_FORMAT_ASTC_4x4_SFLOAT_BLOCK:
    case VK_FORMAT_ASTC_6x6_UNORM_BLOCK:
    case VK_FORMAT_ASTC_6x6_SRGB_BLOCK:
    case VK_FORMAT_ASTC_8x8_UNORM_BLOCK:
    case VK_FORMAT_ASTC_8x8_SRGB_BLOCK: return 16;
    default: return 0;
    }
}

/* Texels a side of such a block: 4, or 6 / 8 for ASTC 6x6 / 8x8. */
static inline uint32_t vkb_tx_block_dim(VkFormat f)
{
    switch (f) {
    case VK_FORMAT_ASTC_6x6_UNORM_BLOCK:
    case VK_FORMAT_ASTC_6x6_SRGB_BLOCK: return 6;
    case VK_FORMAT_ASTC_8x8_UNORM_BLOCK:
    case VK_FORMAT_ASTC_8x8_SRGB_BLOCK: return 8;
    default: return 4;
    }
}

/* Bytes per 4x4 block. */
static inline uint32_t vkb_bc_block_bytes(VkFormat f)
{
    switch (f) {
    case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
    case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
    case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
    case VK_FORMAT_BC4_UNORM_BLOCK:
    case VK_FORMAT_BC4_SNORM_BLOCK: return 8;
    default: return 16;
    }
}

#endif
