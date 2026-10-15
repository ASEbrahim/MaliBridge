#!/bin/bash
# Sourced by bridge_env.sh and mali_env.sh: (re)starts this shell's host server.
#   vkb_server_start VAR=value ...   the server's extra environment
# The socket is VKBRIDGE_SOCKET (default ${XDG_RUNTIME_DIR:-/tmp}/vkbridge-host.sock) and the log
# VKB_SERVER_LOG (default host/out/server.log); run_tests.sh points both into a directory of its
# own run. Only the server this shell started (VKB_SERVER_PID), or one left listening on this very
# socket, is stopped: other worktrees' and other runs' servers are left alone.
_vkb_host=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# The vkbridge-server processes whose --socket argument is exactly $1
vkb_server_pids_on() {
  local p a prev
  for p in $(pgrep -x vkbridge-server); do
    prev=
    while IFS= read -r -d '' a; do
      [ "$prev" = --socket ] && [ "$a" = "$1" ] && { echo "$p"; break; }
      prev=$a
    done 2>/dev/null < "/proc/$p/cmdline"
  done
}
vkb_server_stop() {
  local pids p
  pids="${VKB_SERVER_PID:-} $(vkb_server_pids_on "$VKBRIDGE_SOCKET")"
  for p in $pids; do kill "$p" 2>/dev/null; done
  for p in $pids; do
    for _i in $(seq 30); do kill -0 "$p" 2>/dev/null || break; sleep 0.1; done
  done
  unset VKB_SERVER_PID
  rm -f "$VKBRIDGE_SOCKET"
}
vkb_server_start() {
  export VKBRIDGE_SOCKET=${VKBRIDGE_SOCKET:-${XDG_RUNTIME_DIR:-/tmp}/vkbridge-host.sock}
  local log=${VKB_SERVER_LOG:-$_vkb_host/out/server.log}
  vkb_server_stop
  env "$@" VK_DRIVER_FILES=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json} VK_ICD_FILENAMES= \
    nohup "$_vkb_host/out/vkbridge-server" --socket "$VKBRIDGE_SOCKET" --log "$log" >/dev/null 2>&1 9>&- &
  export VKB_SERVER_PID=$!
  for _i in $(seq 50); do [ -S "$VKBRIDGE_SOCKET" ] && break; sleep 0.1; done
  export VK_DRIVER_FILES=$_vkb_host/out/vkbridge_icd.json
}
