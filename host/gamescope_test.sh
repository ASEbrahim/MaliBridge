#!/bin/bash
# Phase 3 host test: Arch's gamescope (the runtime's distribution) in a container WITHOUT any GPU
# device, drawing only through the bridge, nested on the desktop's Wayland. Run after
# bridge_env.sh. Usage: gamescope_test.sh [command inside gamescope...]
# VKB_GS_HEADLESS=1: gamescope's headless backend at a fixed VKB_GS_REFRESH (default 1000 Hz), no
# window. Its frame rate does not depend on the monitor (a nested window runs at the refresh of the
# monitor under the pointer: 60 or 144 Hz on the same desk). The host's Wayland socket stays mounted:
# gamescope connects to it even headless.
host=$(cd "$(dirname "$0")" && pwd)
R=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
cmd=${*:-vkcube --c 600}
gs="--backend wayland --expose-wayland -W 960 -H 540 -w 960 -h 540"
[ "${VKB_GS_HEADLESS:-0}" = 1 ] && gs="--backend headless -r ${VKB_GS_REFRESH:-1000} -W 960 -H 540 -w 960 -h 540"
exec docker run --rm ${VKB_DOCKER_EXTRA:-} --tmpfs /run/xdg:mode=0700 \
  -e XDG_RUNTIME_DIR=/run/xdg -e WAYLAND_DISPLAY=host-wayland -e HOME=/root \
  -v "$R/${WAYLAND_DISPLAY:-wayland-0}:/run/host-wayland" -v "$VKBRIDGE_SOCKET:/run/vkbridge.sock" \
  -v "$host/out:$host/out" -v "$host/out/shots:/shots" -e VK_DRIVER_FILES=$host/out/vkbridge_icd.json -e VKBRIDGE_SOCKET=/run/vkbridge.sock \
  -e VKBRIDGE_DRM_RENDER=${VKBRIDGE_DRM_RENDER:-} -e VKBRIDGE_HIDE_EXTS=${VKBRIDGE_HIDE_EXTS:-} -e VKBRIDGE_MAX_API=${VKBRIDGE_MAX_API:-} -e VKBRIDGE_LOG_LEVEL=${VKBRIDGE_LOG_LEVEL:-2} -e VKBRIDGE_WSI_DUMP=${VKBRIDGE_WSI_DUMP:-} \
  vkb-arch-gamescope bash -c "ln -s /run/host-wayland /run/xdg/host-wayland; gamescope $gs -- $cmd"
