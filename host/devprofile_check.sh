#!/bin/bash
# Device-profile check: given a phone GPU's Vulkan profile (a Khronos profiles JSON, as
# `vulkaninfo --json` writes on a current SDK and vulkan.gpuinfo.org exports; devprofile/fetch_gpuinfo.py
# fetches one), answer on the host:
#   1. does the bridge start on that GPU: the server's self-test and startup probe, then the
#      client's vkbridge-smoke (what DroidDeck runs before the session), and
#   2. does Zink (Mesa's GL on Vulkan, which Steam's UI draws through) start on it, and
#   3. does gamescope start on it, with Zink's glxgears inside (docker image vkb-arch-gamescope:
#      devprofile/gamescope.Dockerfile; skipped without it), and
#   4. does DXVK (1.10.3, 2.6.2, 2.7.1 and Proton Experimental's own) accept the device the bridge
#      presents, and which one DroidDeck would pick for it (ComponentsManager.bridgeDxvk).
# The real server runs, with every per-GPU path it has (feature emulation, hiding, errata, Vulkan
# promotion), on the desktop driver made to answer as the phone's: the Khronos Profiles layer
# (devprofile/build_profiles_layer.sh; deviceName, vendorID, driverInfo and driverVersion included) over
# VK_LAYER_MALIBRIDGE_devclamp, which takes off at vkCreateDevice only what the desktop driver lacks
# (written to devclamp.log). The phone's memory table is imitated where the server has an imitation
# of it (g720mem, xringmem; the profile's "x-malibridge" memory types, which fetch_gpuinfo.py adds).
# Not imitated: memory sharing (dma-buf / dma-heap / AHB: the desktop's), the phone's RAM, its
# shader compiler and its driver bugs.
#   devprofile_check.sh [-o <out dir>] [--dxvk "1.10.3 2.6.2 2.7.1 proton"] [--no-wine]
#                       [--server-env "VAR=value ..."] <profile.json>...
# (--server-env: more of the server's environment, e.g. VKBRIDGE_EMU_ROBUST2ACCESS=0 shows the Pixel
# G715 as v0.5.1 left it.)
# Needs: make -C host devprofile; the layer (VKB_PROFILES_LAYER_DIR, default
# $VKB_DEVPROFILE_CACHE/vp/build/layer); for DXVK, VKB_WINE_DIR (as d3d11_test.sh) and the DXVK
# releases (devprofile/fetch_dxvk.sh into $VKB_DEVPROFILE_CACHE/dxvk). Profiles: devprofile/fetch_devices.sh
# (the testers' GPUs, devices.txt), fetch_gpuinfo.py <report id>; merge_server_log.py adds a tester's
# extensions from a vkbridge-server.log. Summary: <out>/summary.md.
caller=$PWD
cd "$(dirname "$0")"
host=$PWD
CACHE=${VKB_DEVPROFILE_CACHE:-$HOME/.cache/malibridge-devprofile}
VPL=${VKB_PROFILES_LAYER_DIR:-$CACHE/vp/build/layer}
PROTON="$HOME/.steam/steam/steamapps/common/Proton - Experimental/files"
out=out/devprofile-runs
dxvk_list="1.10.3 2.6.2 2.7.1 proton"
wine=1
server_env=
profiles=()
while [ $# -gt 0 ]; do
  case $1 in
    -o) out=$(cd "$caller" && realpath -m "$2"); shift 2 ;;
    --dxvk) dxvk_list=$2; shift 2 ;;
    --no-wine) wine=0; shift ;;
    --server-env) server_env=$2; shift 2 ;;
    -h|--help) sed -n '2,28p' "$0"; exit 0 ;;
    *) profiles+=("$(cd "$caller" && realpath "$1")"); shift ;;
  esac
done
[ ${#profiles[@]} -gt 0 ] || { sed -n '2,28p' "$0"; exit 2; }
[ -f "$VPL/VkLayer_khronos_profiles.json" ] || { echo "no Khronos Profiles layer in $VPL: run devprofile/build_profiles_layer.sh"; exit 2; }
[ -f out/devprofile/VkLayer_malibridge_devclamp.json ] && [ -x out/vkbridge-server ] || { echo "run make -C host devprofile first"; exit 2; }
if [ $wine = 1 ] && { [ -z "${VKB_WINE_DIR:-}" ] || [ ! -x "$PROTON/bin/wine" ]; }; then
  echo "(no VKB_WINE_DIR or Proton Experimental: DXVK not run)"; wine=0
fi
mkdir -p "$out"; out=$(realpath "$out")
unset VKB_SERVER_PID VKBRIDGE_HIDE_EXTS VKBRIDGE_MAX_API VKBRIDGE_FAKE_MISSING
# The bridge's socket lives in a short directory of its own (sun_path is 108 bytes)
RUN=$(mktemp -d "${XDG_RUNTIME_DIR:-/tmp}/vkbdp-$$.XXXX")
export VKBRIDGE_SOCKET=$RUN/s.sock
. ./server_start.sh
trap 'vkb_server_stop; rm -rf "$RUN"' EXIT

# DXVK's DLLs for a version: the release's x64 ones, or Proton Experimental's own
dxvk_dir() {
  if [ "$1" = proton ]; then echo "$PROTON/lib/wine/dxvk/x86_64-windows"; else echo "$CACHE/dxvk/dxvk-$1/x64"; fi
}

# The server on the desktop driver answering as the profile in $1/profile; more of its env after it
start_server() {
  local d=$1; shift
  # One RADV GPU only (the profile applies to every one the driver lists)
  vkb_server_start VK_LAYER_PATH="$VPL:$host/out/devprofile" \
    VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_profiles:VK_LAYER_MALIBRIDGE_devclamp VKB_DEVCLAMP_LOG="$d/devclamp.log" \
    VK_KHRONOS_PROFILES_PROFILE_DIRS="$d/profile" VK_KHRONOS_PROFILES_EMULATE_PORTABILITY=false \
    VK_KHRONOS_PROFILES_SIMULATE_CAPABILITIES=SIMULATE_API_VERSION_BIT,SIMULATE_FEATURES_BIT,SIMULATE_PROPERTIES_BIT,SIMULATE_EXTENSIONS_BIT,SIMULATE_FORMATS_BIT \
    VK_KHRONOS_PROFILES_DEBUG_REPORTS=DEBUG_REPORT_ERROR_BIT \
    MESA_VK_DEVICE_SELECT=${VKB_DEVPROFILE_GPU:-1002:744c} MESA_VK_DEVICE_SELECT_FORCE_DEFAULT_DEVICE=1 \
    vk_require_etc2=true vk_require_astc=true VKBRIDGE_CACHE_DIR="$d/cache" "$@"
  for _i in $(seq 300); do
    grep -q 'listening on' "$VKB_SERVER_LOG" 2>/dev/null && break
    kill -0 "$VKB_SERVER_PID" 2>/dev/null || break
    sleep 0.1
  done
}

gamescope_ok=0
docker image inspect vkb-arch-gamescope >/dev/null 2>&1 && [ -e /dev/dri/renderD128 ] && gamescope_ok=1
[ $gamescope_ok = 1 ] || echo "(no docker image vkb-arch-gamescope: gamescope not run; see devprofile/gamescope.Dockerfile)"

for prof in "${profiles[@]}"; do
  name=$(basename "$prof" .json)
  d=$out/$name
  rm -rf "$d"; mkdir -p "$d/profile" "$d/cache"
  cp "$prof" "$d/profile/"
  echo "== $name"
  # The phone's memory table, where the bridge has an imitation of it (server_memory.c: g720mem is
  # the G720's/G925's 0x7 0xb 0x11 [0x21], which most Mali drivers report; xringmem the XRing O3's 22)
  mem=$(python3 -I devprofile/memtable.py "$prof")
  echo "${mem:-none, the desktop memory types}" >"$d/memory-imitation.txt"
  export VKB_SERVER_LOG=$d/server.log
  : >"$d/devclamp.log"
  start_server "$d" ${mem:+VKBRIDGE_FAKE_MISSING=$mem} $server_env
  if grep -q 'listening on' "$d/server.log" 2>/dev/null; then
    timeout 120 ./out/vkbridge-smoke >"$d/smoke.log" 2>&1; echo "exit $?" >>"$d/smoke.log"
    # every kind of image and buffer gets a memory type (the XRing O3's v0.5.1 failure)
    [ -x out/memclass_check ] && { timeout 60 ./out/memclass_check >"$d/memclass.log" 2>&1; echo "exit $?" >>"$d/memclass.log"; }
    timeout 60 vulkaninfo --json=0 --output "$d/presented.json" >"$d/vulkaninfo.log" 2>&1
    # Zink on the presented device: Steam's UI draws through it (CEF -> ANGLE -> Zink -> bridge)
    MESA_LOADER_DRIVER_OVERRIDE=zink EGL_PLATFORM=surfaceless timeout 90 eglinfo -B -p surfaceless >"$d/zink.log" 2>&1
    echo "exit $?" >>"$d/zink.log"
    if [ $wine = 1 ]; then
      for v in $dxvk_list; do
        dd=$(dxvk_dir "$v")
        [ -f "$dd/dxgi.dll" ] || { echo "  (no DXVK $v in $dd: devprofile/fetch_dxvk.sh)"; continue; }
        app=$d/dxvk-$v; mkdir -p "$app"
        cp "$VKB_WINE_DIR"/app/d3d11_check.exe "$VKB_WINE_DIR"/app/libvkd3d*.dll "$app/"
        cp "$dd"/d3d11.dll "$dd"/dxgi.dll "$app/"
        (cd "$app" && WINEPREFIX=$VKB_WINE_DIR/pfx WINEDEBUG=-all WINEDLLOVERRIDES="d3d11,dxgi=n,b" DXVK_LOG_PATH=$app \
          DXVK_LOG_LEVEL=info timeout 180 "$PROTON/bin/wine" d3d11_check.exe >"$app/check.log" 2>&1; echo "exit $?" >>"$app/check.log")
        rm -f "$app"/*.dll "$app"/*.exe
      done
    fi
  else
    echo "  the server did not start (see $d/server.log)"
  fi
  vkb_server_stop
  # gamescope (Arch's, headless) with Zink's glxgears in it, as a Steam session starts. On the
  # desktop's memory types: the memory-table imitations do not hold with the desktop's memory
  # sharing for gamescope's exportable images (every GPU fails so, the G720 too).
  if [ $gamescope_ok = 1 ] && grep -q 'listening on' "$d/server.log"; then
    export VKB_SERVER_LOG=$d/server-gamescope.log
    start_server "$d" $server_env
    chmod 666 "$VKBRIDGE_SOCKET"
    timeout 90 docker run --rm --tmpfs /run/xdg:mode=0700 -e XDG_RUNTIME_DIR=/run/xdg -e HOME=/root --device /dev/dri/renderD128 \
      -v "$VKBRIDGE_SOCKET:/run/vkbridge.sock" -v "$host/out:$host/out" -e VK_DRIVER_FILES="$host/out/vkbridge_icd.json" \
      -e VKBRIDGE_SOCKET=/run/vkbridge.sock -e VKBRIDGE_DRM_RENDER=226:128 vkb-arch-gamescope bash -c \
      'timeout -s INT 25 gamescope --backend headless -W 960 -H 540 -w 960 -h 540 -- env MESA_LOADER_DRIVER_OVERRIDE=zink timeout 15 glxgears -info 2>&1' \
      2>&1 | grep -a -v '^GL_EXTENSIONS\|xkbcomp\|^>\|scriptmgr' >"$d/gamescope.log"
    vkb_server_stop
    export VKB_SERVER_LOG=$d/server.log
  fi
  python3 -I "$host/devprofile/verdict.py" "$d" | tee "$d/verdict.txt"
done
python3 -I "$host/devprofile/verdict.py" --summary "$out" >"$out/summary.md"
echo "summary: $out/summary.md"
