#!/bin/bash
# VKBRIDGE_HIDE_CALIBRATED_TS (server_emu.c) on the desktop driver, with a server of its own (its
# own socket and cache directory), the device extensions as vulkaninfo lists them through the bridge:
#   1  as it is: whichever calibrated_timestamps names the driver has are offered;
#   2  =1 in <cache dir>/env: neither name offered;
#   3  =ext: the EXT name hidden, the KHR name still offered (if the driver has it);
#   4  =1 in one game's preset (<cache dir>/presets/<appid>.env): hidden for that game only.
# Prints one line per step and "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
work=$(mktemp -d)
cache=$work/c
mkdir -p "$cache/presets"
sock=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-calts-$$.sock
log=$work/server.log
srv=
trap 'kill $srv 2>/dev/null; rm -f "$sock"; [ -n "$KEEP" ] || rm -rf "$work"' EXIT
command -v vulkaninfo >/dev/null || { echo "OK: 0 steps (SKIP: no vulkaninfo)"; exit 0; }
rm -f "$sock"
env -u VKBRIDGE_FAKE_MISSING -u VKBRIDGE_SERVER_HIDE_EXTS -u VKBRIDGE_HIDE_CALIBRATED_TS -u VKBRIDGE_MODE \
    VK_DRIVER_FILES=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json} VK_ICD_FILENAMES= \
    "$out/vkbridge-server" --socket "$sock" --log "$log" --cache-dir "$cache" >/dev/null 2>&1 &
srv=$!
for _ in $(seq 100); do [ -S "$sock" ] && break; sleep 0.1; done
export VKBRIDGE_SOCKET=$sock VK_DRIVER_FILES=$out/vkbridge_icd.json VK_ICD_FILENAMES=
unset VKBRIDGE_HIDE_EXTS VKBRIDGE_MAX_API SteamAppId
fails=0 steps=0
step() { steps=$((steps+1)); if [ "$1" = 0 ]; then echo "  ok   $2"; else echo "  FAIL $2"; fails=$((fails+1)); fi; }
# setenv <lines>: <cache dir>/env, which the server reads again at each device (a device made for it)
setenv() { printf '%s\n' "$@" > "$cache/env"; timeout 60 "$out/vkbridge-smoke" >/dev/null 2>&1; }
# names [env...]: the calibrated_timestamps names offered, sorted, on one line
names() { timeout 60 env "$@" vulkaninfo 2>/dev/null | grep -o 'VK_[A-Z]*_calibrated_timestamps' | sort -u | tr '\n' ' '; }

setenv ""
base=$(names)
has_ext=0 has_khr=0
case "$base" in *VK_EXT_calibrated_timestamps*) has_ext=1 ;; esac
case "$base" in *VK_KHR_calibrated_timestamps*) has_khr=1 ;; esac
[ $has_ext = 1 ] || [ $has_khr = 1 ]
step $? "1 as it is: offered: ${base:-none}"

setenv VKBRIDGE_HIDE_CALIBRATED_TS=1
got=$(names)
[ -z "$got" ]
step $? "2 =1: offered: ${got:-none}"

setenv VKBRIDGE_HIDE_CALIBRATED_TS=ext
got=$(names)
case "$got" in *VK_EXT_*) r=1 ;; *) r=0 ;; esac
[ $r = 0 ] && { [ $has_khr = 0 ] || case "$got" in *VK_KHR_calibrated_timestamps*) true ;; *) false ;; esac; }
step $? "3 =ext: offered: ${got:-none}"

setenv ""
echo VKBRIDGE_HIDE_CALIBRATED_TS=1 > "$cache/presets/4242.env"
game=$(names SteamAppId=4242)
other=$(names SteamAppId=4343)
[ -z "$game" ] && [ "$other" = "$base" ] && grep -q "preset for steam4242: VKBRIDGE_HIDE_CALIBRATED_TS=1" "$log"
step $? "4 one game's preset: that game offered ${game:-none}, another ${other:-none}"

if [ $fails = 0 ]; then echo "OK: $steps steps"; else echo "FAILED: $fails of $steps steps"; exit 1; fi
