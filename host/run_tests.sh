#!/bin/bash
# Host regression suite for the bridge: every automated check, in two server modes -
#   plain   the desktop driver as it is (RADV)
#   mali    the desktop driver with Mali-G720's known gaps imitated (mali_env.sh)
# Needs: make (out/), Proton Experimental for the DXVK check (VKB_WINE_DIR prepared, see README),
# docker image vkb-arch-gamescope for the gamescope/Zink check (skipped if absent).
cd "$(dirname "$0")"
pass=0; fail=0
result() { if [ "$1" = 0 ]; then pass=$((pass+1)); echo "  ok   $2"; else fail=$((fail+1)); echo "  FAIL $2"; fi; }
# A server-side protocol error fails the run even when the desktop driver shrugs it off (a missing
# count in vkBeginCommandBuffer's encoding passed every check on RADV and crashed Mali).
server_errors() {
  local n; n=$(grep -cE "command stream:|malformed request|unknown command id|the driver has no|named ahead failed|bad batch entry" out/server.log 2>/dev/null)
  [ "${n:-0}" = 0 ] || { echo "  server log: $n protocol errors, first: $(grep -m1 -E "command stream:|malformed request|unknown command id|the driver has no|named ahead failed|bad batch entry" out/server.log)"; return 1; }
}
run_mode() {
  local mode=$1
  echo "== $mode"
  : > out/server.log
  if [ "$mode" = mali ]; then . ./mali_env.sh >/dev/null; else unset VKBRIDGE_HIDE_EXTS VKBRIDGE_MAX_API; . ./bridge_env.sh >/dev/null; fi
  timeout 120 ./out/vkbridge-smoke >out/t-smoke.log 2>&1; result $? "smoke ($(tail -1 out/t-smoke.log))"
  if [ "$mode" = mali ]; then
    # Mali's memory types (lazymem): images get an ordinary device-local type, never the lazily
    # allocated one (an older client did: image checks failed at "setup", gamescope got -2)
    grep -q "imitating Mali's memory types" out/server.log && ! grep -q "lazily allocated, imitated" out/server.log &&
      ! grep -q "no ordinary device-local type" out/t-smoke.log
    result $? "smoke, Mali's memory types: images in ordinary memory ($(grep -m1 -o 'first allocation in memory type [0-9]* (flags 0x[0-9a-f]*, heap [0-9]*): [0-9]* bytes, plain' out/server.log))"
    # Other Mali drivers: push descriptors but not robustness2 (nulls in push descriptor writes are
    # replaced too), no multiDrawIndirect (Mali-G57 r32/r38: indirect draws are split)
    VKB_MISSING=robustness2,multidraw . ./mali_env.sh >/dev/null
    VKBRIDGE_HIDE_EXTS=VK_EXT_robustness2,VK_KHR_robustness2 timeout 120 ./out/vkbridge-smoke >out/t-smoke-push.log 2>&1 &&
      grep -q "null descriptors in push descriptor writes replaced" out/server.log &&
      grep -q "indirect draws split" out/server.log
    result $? "smoke, native push descriptors without robustness2, no multiDrawIndirect ($(tail -1 out/t-smoke-push.log))"
    . ./mali_env.sh >/dev/null
    # BC transcoded to ETC2/EAC/ASTC (compared by PSNR), then decoded as the fallback does (exact)
    timeout 180 ./out/bcn_check transcode >out/t-bcn.log 2>&1; result $? "bcn_check, BC transcoded ($(tail -1 out/t-bcn.log))"
    VKB_BC_TRANSCODE=0 . ./mali_env.sh >/dev/null
    timeout 180 ./out/bcn_check >out/t-bcn-decode.log 2>&1; result $? "bcn_check, BC decoded ($(tail -1 out/t-bcn-decode.log))"
    . ./mali_env.sh >/dev/null
  else
    timeout 180 ./out/bcn_check >out/t-bcn.log 2>&1; result $? "bcn_check ($(tail -1 out/t-bcn.log))"
  fi
  # Null images of every shape (cube, 3D, 1D, arrays, uint/sint): on mali each gets a stand-in of
  # its binding's shape, from the pipeline's SPIR-V
  if [ -x out/nulldesc_check ]; then
    timeout 60 ./out/nulldesc_check >out/t-nulldesc.log 2>&1 &&
      { [ "$mode" != mali ] || [ "$(grep -c 'null descriptor stand-in made' out/server.log)" -ge 6 ]; }
    result $? "nulldesc_check ($(tail -1 out/t-nulldesc.log)$([ "$mode" = mali ] && echo "; $(grep -c 'null descriptor stand-in made' out/server.log) stand-in shapes"))"
  fi
  if [ -x out/opaque_check ]; then timeout 60 ./out/opaque_check >out/t-opaque.log 2>&1; result $? "opaque_check ($(tail -1 out/t-opaque.log))"; fi
  if [ -x out/recycle_check ]; then timeout 120 ./out/recycle_check >out/t-recycle.log 2>&1; result $? "recycle_check ($(tail -1 out/t-recycle.log))"; fi
  # Descriptor traffic, batched (vkbBatch, packed updates) and not; the client's totals in the line
  if [ -x out/desc_traffic_check ]; then
    for nb in 0 1; do
      VKBRIDGE_DESC_BATCH=$((1-nb)) VKBRIDGE_STATS=100000 timeout 120 ./out/desc_traffic_check >out/t-desc-$nb.log 2>&1
      result $? "desc_traffic_check, VKBRIDGE_NO_DESC_BATCH=$nb ($(tail -1 out/t-desc-$nb.log | grep -o '^[A-Z]*'); $(grep -o '[0-9]* messages, [0-9]* KB on the wire' out/t-desc-$nb.log))"
    done
  fi
  # DXVK's frame structure (frames in flight, pools reset per frame, submits from another thread)
  if [ -x out/flicker_check ]; then
    for nb in 0 1; do
      VKBRIDGE_DESC_BATCH=$((1-nb)) timeout 180 ./out/flicker_check >out/t-flicker-$nb.log 2>&1
      result $? "flicker_check, VKBRIDGE_NO_DESC_BATCH=$nb ($(grep -o '[0-9]* draws wrong in [0-9]* frames' out/t-flicker-$nb.log))"
    done
  fi
  if [ -n "${VKB_WINE_DIR:-}" ] && [ -d "$VKB_WINE_DIR/app" ]; then
    ./d3d11_test.sh >out/t-d3d11.log 2>&1; grep -q "^OK:" out/t-d3d11.log; result $? "d3d11_check via DXVK ($(grep -E '^(OK|FAILED)' out/t-d3d11.log))"
    # Every frame read back, through DXVK, with batching on (needs d3d11_flicker.exe in the prefix)
    if [ -f "$VKB_WINE_DIR/app/d3d11_flicker.exe" ]; then
      VKBRIDGE_DESC_BATCH=1 ./d3d11_test.sh d3d11_flicker.exe >out/t-d3d11-flicker.log 2>&1; grep -q "^OK" out/t-d3d11-flicker.log
      result $? "d3d11_flicker via DXVK, batched ($(grep -o '[0-9]* cells wrong in [0-9]* frames' out/t-d3d11-flicker.log))"
    fi
  fi
  timeout 60 vkcube-wayland --c 120 >out/t-vkcube.log 2>&1; result $? "vkcube-wayland (dma-buf)"
  VKBRIDGE_WSI_SHM=1 timeout 60 vkcube-wayland --c 120 >>out/t-vkcube.log 2>&1; result $? "vkcube-wayland (wl_shm)"
  timeout 60 vkcube --c 120 >>out/t-vkcube.log 2>&1; result $? "vkcube (X11)"
  if docker image inspect vkb-arch-gamescope >/dev/null 2>&1; then
    VKB_DOCKER_EXTRA="--device /dev/dri/renderD128" VKBRIDGE_DRM_RENDER=226:128 timeout 120 ./gamescope_test.sh \
      "bash -c 'export MESA_LOADER_DRIVER_OVERRIDE=zink GALLIUM_DRIVER=zink LIBGL_KOPPER_DRI2=true; timeout 20 glxgears 2>&1 | grep -m1 frames'" \
      >out/t-gamescope-$mode.log 2>&1
    grep -q "frames in" out/t-gamescope-$mode.log; result $? "gamescope + Zink glxgears ($(grep -m1 -o '[0-9.]* FPS' out/t-gamescope-$mode.log))"
  fi
}
run_mode plain
server_errors; result $? "no protocol errors in the server log (plain)"
run_mode mali
server_errors; result $? "no protocol errors in the server log (mali)"
echo "== $pass passed, $fail failed"
[ "$fail" = 0 ]
