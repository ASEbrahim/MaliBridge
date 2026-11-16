#!/bin/bash
# VK_EXT_image_drm_format_modifier with the LINEAR modifier only, where the driver has dma-buf memory but not
# the extension and its dma-buf export is emulated (VKB_EMU_DRM_MOD_LINEAR, server_emu.c; a player log,
# a player log: Xiaomi 17T, Mali-G720 MC8 r49p1, Android 17). On those phones Zink could not export an image it had
# not made exportable (zink_resource_get_handle: "Incorrect rendering will happen because the Vulkan device
# doesn't support the 'EXT_image_drm_format_modifier' feature" in both Xwayland processes and in Steam's web
# helper GPU process, "Failed to get fd for plane" there 24 times), Xwayland exported no window buffer to
# gamescope (0 exports against 65 + 15 on a G615 that has the extension) and Steam's window never showed.
# lavapipe has dma-buf memory and no modifier extension, as r49p1; its dma-buf export is imitated missing
# (dmabufexport), the emulation's pages are memfds (VKBRIDGE_TEST_EXPORT_PAGES=memfd), with the rest of the
# Mali profile (the host suite's r49 profile where it imitates one). A server per step; gsexport_check asks and makes what Zink and gamescope do: the format's
# modifier list, the LINEAR modifier's image queries, an image made with the modifier list {LINEAR} and
# exportable memory, its modifier and plane 0 layout, vkGetMemoryFdKHR, red read through the dma-buf, and
# the dma-buf imported at the explicit LINEAR modifier (red read out of it):
#   1  VKBRIDGE_EMU_DRMMOD=0 (today's bridge): the extension not offered; gsexport_check stops at its device
#      ("Vulkan setup"), as Zink found no modifier extension on the phones (the fingerprint);
#   2  the default: the line logged, every check passes (the same red texels both ways), the images made
#      plain LINEAR (4 exports, 4 imports), nothing at DRM-modifier tiling reached the driver;
#   3  no export emulation (the driver's dma-buf export as it is): not offered, no line (unchanged);
#   4  a driver with the extension of its own (the host's, when it has one and no Mali profile hides it;
#      skipped on lavapipe): the
#      driver's, the bridge's path not taken (no line)
# Prints one line per step and "OK: ..." / "FAILED: ..." last. VKB_OUT: the build to test (before/after).
cd "$(dirname "$0")"
# The phones' driver family (host/profiles/mali-r49.txt: dma-buf memory, no modifier extension) where the suite
# imitates a Mali profile (the common one hides dma-buf memory: no Kirin has it)
[ "${VKB_MALI_PROFILE_CUR:-off}" != off ] && VKB_MALI_PROFILE=r49 . ./mali_profile.sh >/dev/null
out=${VKB_OUT:-$PWD/out}
chk=$out/gsexport_check
work=$(mktemp -d)
sock=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-drmmod-$$.sock
host_icd=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/lvp_icd.json}
mali=bc,clip,cull,divisor,maint5,depthclip,push,robustness2,dualsrc,fillmode,multiviewport,maint6,desclimits,opaquefd,lazymem,dmabuflinear,planarext
srv=
log=
stop() { [ -n "$srv" ] && kill $srv 2>/dev/null && wait $srv 2>/dev/null; srv=; }
start() { # start <name> [env...]: a server of its own, log $log
  local d=$work/$1
  shift
  stop
  rm -f "$sock"
  mkdir -p "$d/c"
  log=$d/vkbridge-server.log
  : > "$log"
  env -u VKBRIDGE_FAKE_MISSING -u VKBRIDGE_HIDE_EXTS -u VKBRIDGE_MODE -u VKBRIDGE_EMU_DRMMOD -u VKBRIDGE_TEST_EXPORT_PAGES \
      VKBRIDGE_PROBE_BUDGET_S=0 vk_require_etc2=true vk_require_astc=true "$@" VK_DRIVER_FILES=$host_icd VK_ICD_FILENAMES= \
      "$out/vkbridge-server" --socket "$sock" --log "$log" --cache-dir "$d/c" >/dev/null 2>&1 &
  srv=$!
  for _ in $(seq 300); do [ -S "$sock" ] && break; kill -0 $srv 2>/dev/null || break; sleep 0.05; done
}
trap 'stop; rm -f "$sock"; [ -n "$KEEP" ] && echo "kept $work" || rm -rf "$work"' EXIT
export VKBRIDGE_SOCKET=$sock VK_DRIVER_FILES=$out/vkbridge_icd.json VK_ICD_FILENAMES= VKBRIDGE_MAX_API=1.3
unset VKBRIDGE_HIDE_EXTS VKBRIDGE_FAKE_MISSING VKBRIDGE_MODE
fails=0 steps=0
step() { steps=$((steps+1)); if [ "$1" = 0 ]; then echo "  ok   $2"; else echo "  FAIL $2"; fails=$((fails+1)); fi; }
run() { v=$(timeout 90 "$chk" 2>&1); rc=$?; }
line='VK_EXT_image_drm_format_modifier: the driver has none; offered with the LINEAR modifier only'
listed() { timeout 60 vulkaninfo 2>/dev/null | grep -c 'VK_EXT_image_drm_format_modifier '; }
host_has=$(VK_DRIVER_FILES=$host_icd VK_ICD_FILENAMES= timeout 60 vulkaninfo 2>/dev/null | grep -c 'VK_EXT_image_drm_format_modifier ')

if [ "$host_has" = 0 ]; then imit=$mali,dmabufexport; else imit=$mali,dmabufexport,drmmod; fi

# 1
start off VKBRIDGE_FAKE_MISSING=$imit VKBRIDGE_TEST_EXPORT_PAGES=memfd VKBRIDGE_EMU_DRMMOD=0
run
n=$(listed)
[ "$rc" != 0 ] && echo "$v" | grep -q '^FAIL Vulkan setup' && [ "$n" = 0 ] && grep -q 'dma-buf export: emulated' "$log" && ! grep -q "$line" "$log"
step $? "VKBRIDGE_EMU_DRMMOD=0 (the phones' state): no modifier extension ($n listed), export emulated, gsexport_check stops at its device ($(echo "$v" | tail -1))"

# 2
start on VKBRIDGE_FAKE_MISSING=$imit VKBRIDGE_TEST_EXPORT_PAGES=memfd
run
n=$(listed)
ex=$(grep -c 'dma-buf export image with a LINEAR modifier list .* made as a plain LINEAR image' "$log")
im=$(grep -c 'dma-buf import image at the LINEAR modifier .* made as a plain LINEAR image' "$log")
[ "$rc" = 0 ] && echo "$v" | grep -q '^OK:' && [ "$n" -ge 1 ] && grep -q "$line" "$log" && [ "$ex" = 4 ] && [ "$im" = 4 ] &&
  echo "$v" | grep -q '^PASS B8G8R8A8: the LINEAR modifier listed' && echo "$v" | grep -q '^PASS 1280x720: red through the dma-buf' &&
  echo "$v" | grep -q '^PASS 1280x720 import: red read out of the imported image' &&
  ! grep -q 'vkCreateImage refused\|export emulation failed' "$log"
step $? "default: offered with LINEAR only ($n listed), the line logged, $ex exports and $im imports made plain LINEAR, gsexport_check $(echo "$v" | tail -1)"

# 3
start noexport VKBRIDGE_FAKE_MISSING=$mali
n=$(listed)
! grep -q "$line" "$log" && { [ "$host_has" != 0 ] || [ "$n" = 0 ]; }
step $? "the driver's own dma-buf export (no emulation): the bridge's modifier extension not offered, no line ($n listed)"

# 4
if [ "$host_has" != 0 ] && [ "${VKB_MALI_PROFILE_CUR:-off}" = off ]; then
  start driver VKBRIDGE_FAKE_MISSING=$mali,dmabufexport VKBRIDGE_TEST_EXPORT_PAGES=memfd
  run
  ! grep -q "$line" "$log" && [ "$(listed)" -ge 1 ]
  step $? "the driver's own modifier extension: listed, the bridge's path not taken (gsexport_check $(echo "$v" | tail -1))"
else
  echo "  (step 4 skipped: the host driver has no VK_EXT_image_drm_format_modifier of its own, or a Mali profile hides it)"
fi
stop
if [ "$fails" = 0 ]; then echo "OK: $steps of $steps steps"; else echo "FAILED: $fails of $steps steps"; fi
[ "$fails" = 0 ]
