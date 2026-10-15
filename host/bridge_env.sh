#!/bin/bash
# Source this: starts a host vkbridge server on a desktop driver (VKB_HOST_ICD, default RADV) and
# points this shell's Vulkan programs at the bridge ICD.
#   [VKB_HOST_ICD=...] [VKB_MISSING=...] . host/bridge_env.sh   (VKB_MISSING: the server's VKBRIDGE_FAKE_MISSING)
# Socket and log: see server_start.sh (re-sourcing restarts only this shell's server).
. "$(dirname "${BASH_SOURCE[0]}")/server_start.sh"
vkb_server_start VKBRIDGE_FAKE_MISSING=${VKB_MISSING:-${VKBRIDGE_FAKE_MISSING:-}}
echo "vkbridge server up (${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json}) at $VKBRIDGE_SOCKET"
