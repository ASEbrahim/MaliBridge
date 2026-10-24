#!/bin/bash
# Source this: a host bridge server that behaves like the Mali-G720 driver as far as Phase 0
# found (no BCn, clip/cull distance, vertex attribute divisor, maintenance5, push descriptors, robustness2, 500,000 descriptors per stage, no opaque fds; Vulkan 1.3),
# and with Mali's memory types: the only device-local type without host access is lazily allocated
# (lazymem: ordinary allocations there fail with -2, as a Pixel 9 on an older client showed).
# VKB_MISSING_ADD=xringmem adds to the list (xringmem: the XRing O3's 22 memory types, images and buffers allowing different ones;
# g720mem: the G720's and G925's 4 types; server_memory.c).
# VKB_MISSING_ADD=dmabuftypes: a dma-buf's fd properties list only the host-visible cached types, as Mali's do.
# VKB_SERVER_ENV="VAR=value ...": more of the server's environment (VKBRIDGE_FAKE_DRIVER_VERSION=r44p1: Mali's errata).
# Like Mali r44 it refuses dma-buf images at LINEAR/OPTIMAL tiling (dmabuflinear: only DRM-modifier
# dma-buf images are made; the rest get VK_ERROR_INVALID_DRM_FORMAT_MODIFIER_PLANE_LAYOUT_EXT).
# Like the Mali-G925 it refuses VK_IMAGE_CREATE_EXTENDED_USAGE_BIT on multi-planar formats (planarext:
# DXVK's NV12 textures, Media Foundation's video frames).
# Like Mali it samples ETC2/EAC and ASTC LDR (RADV emulates them when vk_require_etc2/astc are set),
# so BC textures are transcoded; VKB_BC_TRANSCODE=0 starts it decoding them instead, VKB_BC_ASTC_BLOCK=6x6 / 8x8
# transcodes BC2 / BC3 / BC7 to that ASTC block, VKB_TEX_MAXDIM=<n>[,all] caps textures' size.
. "$(dirname "${BASH_SOURCE[0]}")/server_start.sh"
vkb_server_start ${VKB_SERVER_ENV:-} VKBRIDGE_FAKE_MISSING=${VKB_MISSING:-bc,clip,cull,divisor,maint5,depthclip,push,robustness2,dualsrc,fillmode,multiviewport,maint6,desclimits,opaquefd,lazymem,dmabuflinear,planarext}${VKB_MISSING_ADD:+,$VKB_MISSING_ADD} \
    vk_require_etc2=true vk_require_astc=true VKBRIDGE_BC_TRANSCODE=${VKB_BC_TRANSCODE:-1} \
    ${VKB_BC_ASTC_BLOCK:+VKBRIDGE_BC_ASTC_BLOCK=$VKB_BC_ASTC_BLOCK} ${VKB_TEX_MAXDIM:+VKBRIDGE_TEX_MAXDIM=$VKB_TEX_MAXDIM}
chmod 666 "$VKBRIDGE_SOCKET"
export VKBRIDGE_HIDE_EXTS=VK_KHR_maintenance5,VK_KHR_push_descriptor,VK_EXT_robustness2,VK_KHR_robustness2,VK_EXT_vertex_attribute_divisor,VK_KHR_vertex_attribute_divisor,VK_EXT_depth_clip_enable,VK_KHR_maintenance6
export VKBRIDGE_MAX_API=1.3
echo "vkbridge server up, imitating Mali's gaps"
