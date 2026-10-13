#!/bin/bash
# Source this: a host bridge server that behaves like the Mali-G720 driver as far as Phase 0
# found (no BCn, clip/cull distance, vertex attribute divisor, maintenance5, push descriptors, robustness2, 500,000 descriptors per stage, no opaque fds; Vulkan 1.3).
# Like Mali it samples ETC2/EAC and ASTC LDR (RADV emulates them when vk_require_etc2/astc are set),
# so BC textures are transcoded; VKB_BC_TRANSCODE=0 starts it decoding them instead.
_vkb_host=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
export VKBRIDGE_SOCKET=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-host.sock
pgrep -x vkbridge-server >/dev/null && kill $(pgrep -x vkbridge-server) 2>/dev/null; sleep 0.3
rm -f "$VKBRIDGE_SOCKET"
VKBRIDGE_FAKE_MISSING=${VKB_MISSING:-bc,clip,cull,divisor,maint5,depthclip,push,robustness2,dualsrc,fillmode,multiviewport,maint6,desclimits,opaquefd} VK_DRIVER_FILES=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json} \
    vk_require_etc2=true vk_require_astc=true VKBRIDGE_BC_TRANSCODE=${VKB_BC_TRANSCODE:-1} \
    nohup "$_vkb_host/out/vkbridge-server" --socket "$VKBRIDGE_SOCKET" --log "$_vkb_host/out/server.log" >/dev/null 2>&1 &
for _i in $(seq 50); do [ -S "$VKBRIDGE_SOCKET" ] && break; sleep 0.1; done
chmod 666 "$VKBRIDGE_SOCKET"
export VK_DRIVER_FILES=$_vkb_host/out/vkbridge_icd.json
export VKBRIDGE_HIDE_EXTS=VK_KHR_maintenance5,VK_KHR_push_descriptor,VK_EXT_robustness2,VK_KHR_robustness2,VK_EXT_vertex_attribute_divisor,VK_KHR_vertex_attribute_divisor,VK_EXT_depth_clip_enable,VK_KHR_maintenance6
export VKBRIDGE_MAX_API=1.3
echo "vkbridge server up, imitating Mali's gaps"
