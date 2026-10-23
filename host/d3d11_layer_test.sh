#!/bin/bash
# Runs tests/d3d11_check.exe (or the exe named as $1, with the rest as its arguments) under Proton
# Experimental's wine + DXVK, with the LAYER (VkLayer_malibridge_compat) as the Vulkan
# implementation instead of the bridge: DXVK talks to the layer, the layer to the driver, in one
# process.
#
# Needs the same prefix as host/d3d11_test.sh (VKB_WINE_DIR: the exe, DXVK's d3d11/dxgi and
# vkd3d's DLLs) and a layer build for the exe's architecture: a 32-bit exe needs a 32-bit layer
# (cmake -B build-linux-x86-32 -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_BUILD_TYPE=Release
#  -DCMAKE_C_FLAGS=-m32 -DCMAKE_CXX_FLAGS=-m32 && cmake --build build-linux-x86-32).
#
# The prefix, with Proton Experimental's wine and DXVK:
#   P="$HOME/.steam/steam/steamapps/common/Proton - Experimental/files"
#   W=$HOME/dxvk-test; mkdir -p "$W/app"
#   x86_64-w64-mingw32-gcc -O2 tests/d3d11_check.c -o "$W/app/d3d11_check.exe" \
#       -ld3d11 -ld3dcompiler_47 -ldxgi
#   WINEPREFIX="$W/pfx" "$P/bin/wine" wineboot -u
#   cd "$W/pfx/drive_c/windows/system32"
#   cp "$P/lib/wine/dxvk/x86_64-windows/"{d3d11,dxgi}.dll .
#   cp "$P/lib/wine/x86_64-windows/"{d3dcompiler_47,wined3d}.dll .
#   cp "$P/lib/vkd3d/x86_64-windows/"libvkd3d*.dll .
#
#   VKB_WINE_DIR=... VKB_LAYER_BUILD=... host/d3d11_layer_test.sh [exe] [args...]
#
# The driver is the host's own (VK_DRIVER_FILES is not set): the layer is the only thing between
# DXVK and it. The GPU is taken as a Mali-G720 (VKBRIDGE_FAKE_DRIVER_VERSION for the version the
# errata's gates see, VKBRIDGE_FAKE_MISSING for the features it lacks - the same list
# host/mali_env.sh uses), so the layer's emulations are what make the D3D11 features work: the
# BCn textures, the clip distance and the vertex attribute divisor, and the errata's decisions are
# the G720's. VKBRIDGE_ERRATA_IMITATE puts the Mali driver's own bugs beneath the fixes that
# repair them; it is off by default (see below).
P="$HOME/.steam/steam/steamapps/common/Proton - Experimental/files"
W=${VKB_WINE_DIR:?set VKB_WINE_DIR}
here=$(cd "$(dirname "$0")" && pwd)
build=${VKB_LAYER_BUILD:-$here/../build-linux-x86-64}
exe=${1:-d3d11_check.exe}
[ $# -gt 0 ] && shift
# The exe may be named in the prefix's app dir or given as a path (a game, say).
case "$exe" in
    /*) exe_path=$exe ;;
    *) exe_path=$W/app/$exe ;;
esac
exe_dir=$(dirname "$exe_path")

lib=$build/libVkLayer_malibridge_compat.so
if [ ! -f "$lib" ]; then
  echo "no layer at $lib; build it first:" >&2
  echo "  cmake -B build-linux-x86-64 -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_BUILD_TYPE=Release && cmake --build build-linux-x86-64" >&2
  exit 1
fi

# The manifest needs an absolute library_path: the loader does not resolve a relative one against
# the manifest's own directory.
layers=$(mktemp -d)
trap 'rm -rf "$layers"' EXIT
cat > "$layers/VkLayer_malibridge_compat.json" <<EOF
{
    "file_format_version": "1.1.2",
    "layer": {
        "name": "VK_LAYER_MALIBRIDGE_compat",
        "type": "GLOBAL",
        "library_path": "$lib",
        "api_version": "1.3.278",
        "implementation_version": "1",
        "description": "MaliBridge Compatibility & Errata Layer for Arm Mali"
    }
}
EOF

# The layer must match the exe's architecture: Wine's 32-bit loader cannot load a 64-bit layer
# (and the other way round).
bits64() { case "$1" in *PE32+*|*"x86-64"*|*"x86_64"*|*"ELF 64-bit"*) echo 1 ;; *) echo 0 ;; esac; }
exe64=$(bits64 "$(file -b "$exe_path" 2>/dev/null)")
lib64=$(bits64 "$(file -b "$lib")")
if [ "$exe64" != "$lib64" ]; then
  if [ "$exe64" = 0 ]; then
    echo "the exe is 32-bit and the layer 64-bit: Wine's 32-bit loader cannot load it." >&2
  else
    echo "the exe is 64-bit and the layer 32-bit: Wine's 64-bit loader cannot load it." >&2
  fi
  echo "Build a layer for the exe's architecture (VKB_LAYER_BUILD=...) and point this script at it." >&2
  exit 1
fi

# As close to a Mali as the host can be made:
#   - the GPU counts as an Arm G720 (VKBRIDGE_FAKE_DRIVER_VERSION): the errata's decisions, the
#     emulations and the driver's identity follow from it;
#   - the features a G720 lacks are taken away (VKBRIDGE_FAKE_MISSING), so the emulations are
#     what the app gets.
#
# VKBRIDGE_ERRATA_IMITATE puts the driver's own bugs beneath the fixes that repair them. It is off
# by default: the imitations are host-test shims, and the ones that break the emulation's own
# shaders (viewport_flip: the BCn decoder's compute shader and the clip distance's discard) break
# the emulations themselves - the bridge does exactly the same, so the two paths still agree, but
# the run says nothing about the emulations then.
mali_version=${VKBRIDGE_FAKE_DRIVER_VERSION:-r44p1}
mali_missing=${VKBRIDGE_FAKE_MISSING:-bc,clip,cull,divisor,maint5,depthclip,push,robustness2,dualsrc,fillmode,multiviewport,maint6,desclimits,opaquefd}
mali_imitate=${VKBRIDGE_ERRATA_IMITATE:-}

# The exe's own directory is the working directory (a game's data files are next to it).
out=$(cd "$exe_dir" && \
  WINEPREFIX=$W/pfx WINEDEBUG=-all WINEDLLOVERRIDES=${WINEDLLOVERRIDES:-"d3d11,dxgi=n,b"} DXVK_LOG_PATH=$W \
  VK_LAYER_PATH=$layers VK_INSTANCE_LAYERS=VK_LAYER_MALIBRIDGE_compat \
  VKBRIDGE_FAKE_DRIVER_VERSION=$mali_version \
  VKBRIDGE_FAKE_MISSING=$mali_missing \
  VKBRIDGE_ERRATA_IMITATE=$mali_imitate \
  timeout 180 "$P/bin/wine" "$exe_path" "$@" 2>&1)
rc=$?
# The app's own lines, without Wine's and the layer's log (VKBRIDGE_DEBUG=1 for that).
echo "$out" | grep -v "^info:\|^warn:\|fixme\|err:hid\|wineserver\|vkbridge-server"
exit $rc
