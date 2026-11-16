#!/bin/bash
# Which DXVK takes the bridge's device when the bridge offers Vulkan 1.1 (old Mali drivers: r32p1
# Mali-G77 / G57 / G710 report 1.1.177; a player log). Proton Experimental's own DXVK (3.x) must say
# "Skipping: Device does not support Vulkan 1.3" / "No adapters found" (a D3D game then never makes a
# device, as Evoland did when a Steam update of Proton put that DXVK back), and DXVK 1.10.3 (the one
# DroidDeck picks for Vulkan < 1.3, ComponentsManager.bridgeDxvk) must make its device. A Vulkan 1.3
# driver as itself: both make it. Shows whether the DroidDeck rule still matches what DXVK accepts.
#   VKB_WINE_DIR=<dir with app/d3d11_check.exe, app/libvkd3d*.dll and pfx/ (d3d11_test.sh)>
#   DXVK 1.10.3 in $VKB_DEVPROFILE_CACHE/dxvk (devprofile/fetch_dxvk.sh 1.10.3)
#   VKB_HOST_ICD: the desktop driver (lavapipe by default). Prints "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
P="$HOME/.steam/steam/steamapps/common/Proton - Experimental/files"
W=${VKB_WINE_DIR:?set VKB_WINE_DIR}
D110=${VKB_DEVPROFILE_CACHE:-$HOME/.cache/malibridge-devprofile}/dxvk/dxvk-1.10.3/x64
[ -x "$P/bin/wine" ] && [ -f "$D110/dxgi.dll" ] || { echo "SKIP: no Proton Experimental or no DXVK 1.10.3 ($D110)"; exit 0; }
unset WAYLAND_DISPLAY DISPLAY VKBRIDGE_HIDE_EXTS VKBRIDGE_MAX_API VKB_SERVER_LOG
work=$(mktemp -d); sock=${XDG_RUNTIME_DIR:-/tmp}/vkb-dxvkapi-$$.sock; srv=
trap 'kill $srv 2>/dev/null; rm -f "$sock"; [ -n "$KEEP" ] || rm -rf "$work"' EXIT
fails=0
check() {  # <api: 1.1 or native> <dxvk dir> <name> <want: ok|rejected>
  [ -n "$srv" ] && kill $srv 2>/dev/null && wait $srv 2>/dev/null
  rm -f "$sock"
  local fake=; [ "$1" = native ] || fake=VKBRIDGE_FAKE_API=$1
  env -u VKBRIDGE_FAKE_API $fake VK_DRIVER_FILES=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/lvp_icd.json} VK_ICD_FILENAMES= \
    "$out/vkbridge-server" --socket "$sock" --log "$work/server-$1.log" --cache-dir "$work/c" >/dev/null 2>&1 &
  srv=$!
  for _ in $(seq 100); do [ -S "$sock" ] && break; sleep 0.1; done
  local app=$work/$1-$3; mkdir -p "$app"
  cp "$W"/app/d3d11_check.exe "$W"/app/libvkd3d*.dll "$2"/d3d11.dll "$2"/dxgi.dll "$app/"
  (cd "$app" && VKBRIDGE_SOCKET=$sock VK_DRIVER_FILES=$out/vkbridge_icd.json VK_ICD_FILENAMES=$out/vkbridge_icd.json \
    WINEPREFIX=$W/pfx WINEDEBUG=-all WINEDLLOVERRIDES="d3d11,dxgi=n,b" DXVK_LOG_PATH=$app DXVK_LOG_LEVEL=info \
    timeout 180 "$P/bin/wine" d3d11_check.exe >check.log 2>&1); local rc=$?
  local got=ok
  grep -q "No adapters found" "$app"/check.log "$app"/*.log 2>/dev/null && got=rejected
  [ $got = ok ] && [ $rc != 0 ] && got="exit $rc"
  local why; why=$(grep -h -m1 "Skipping:" "$app"/check.log 2>/dev/null | sed 's/^info: *//')
  if [ "$got" = "$4" ]; then echo "  ok   Vulkan $1, DXVK $3: $got${why:+ ($why)}"
  else echo "  FAIL Vulkan $1, DXVK $3: $got, want $4${why:+ ($why)}"; fails=$((fails+1)); fi
}
check 1.1 "$P/lib/wine/dxvk/x86_64-windows" proton rejected
check 1.1 "$D110" 1.10.3 ok
check native "$P/lib/wine/dxvk/x86_64-windows" proton ok
check native "$D110" 1.10.3 ok
[ $fails = 0 ] && echo "OK: DXVK 1.10.3 takes a Vulkan 1.1 bridge device, Proton's own DXVK does not" || echo "FAILED: $fails"
[ $fails = 0 ]
