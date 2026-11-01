#!/bin/bash
# dma-buf memory over AHardwareBuffers (server_dmabuf_ahb.c): Kirin's Mali and Maleoon drivers (a player log,
# a player log) have no VK_KHR_external_memory_fd / VK_EXT_external_memory_dma_buf,
# which gamescope enables unconditionally. On the host the desktop driver's own dma-bufs stand in for the
# AHardwareBuffers (VKBRIDGE_TEST_EXPORT_PAGES=driver); tests/dmabufahb_check.c exports gamescope's
# flippable image and Zink's LINEAR one in one process and imports them in another.
#   1  the driver as it is: nothing of the path (the modifier extension offered, no "on" line)
#   2  imitated Kirin (VKBRIDGE_FAKE_MISSING=dmabufext), host pages: the start's check passes, the check passes
#      (Zink's image read back through gamescope's, a mismatched import and foreign fds refused; the server's
#      modifier extension with LINEAR alone: Xwayland's pixmap exported from a modifier list and read back
#      through gamescope's explicit LINEAR import, one at another pitch refused), and the log counts exports
#      and imports
#   3  VKBRIDGE_DMABUF_VIA_AHB=1 on the driver with dma-bufs of its own (the tablet check): the same, foreign
#      fds left to the driver
#   4  imitated Kirin, VKBRIDGE_DMABUF_VIA_AHB=0: no dma-buf memory offered (the check skips), as before
#   5  imitated Kirin without host pages: "not possible", no dma-buf memory offered
# Prints one line per step and "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
work=$(mktemp -d)
sock=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-dmabufahb-$$.sock
host_icd=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json}
kirin=dmabufext
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
  env -u VKBRIDGE_FAKE_MISSING -u VKBRIDGE_DMABUF_VIA_AHB -u VKBRIDGE_TEST_EXPORT_PAGES VKBRIDGE_PROBE_BUDGET_S=0 "$@" \
      VK_DRIVER_FILES=$host_icd VK_ICD_FILENAMES= \
      "$out/vkbridge-server" --socket "$sock" --log "$log" --cache-dir "$d/c" >/dev/null 2>&1 &
  srv=$!
  for _ in $(seq 300); do [ -S "$sock" ] && break; kill -0 $srv 2>/dev/null || break; sleep 0.05; done
}
check() { timeout 60 "$out/dmabufahb_check" $1 >"$work/check-$2.log" 2>&1; echo $? >"$work/check-$2.rc"; }
trap 'stop; rm -f "$sock"; [ -n "$KEEP" ] && echo "kept $work" || rm -rf "$work"' EXIT
export VKBRIDGE_SOCKET=$sock VK_DRIVER_FILES=$out/vkbridge_icd.json VK_ICD_FILENAMES=
unset VKBRIDGE_HIDE_EXTS VKBRIDGE_MAX_API VKBRIDGE_FAKE_MISSING VKBRIDGE_MODE
fails=0 steps=0
step() { steps=$((steps+1)); if [ "$1" = 0 ]; then echo "  ok   $2"; else echo "  FAIL $2"; fails=$((fails+1)); sed 's/^/       /' "$3"; fi; }
passed() { [ "$(cat "$work/check-$1.rc")" = 0 ] && grep -q "^OK: 0 failure" "$work/check-$1.log"; }

start native
check "" native
grep -q "VK_EXT_image_drm_format_modifier offered" "$work/check-native.log" && ! grep -q "dma-buf over AHardwareBuffer: on" "$log"
step $? "1 the driver as it is: its own dma-bufs, the modifier extension offered, the path off" "$work/check-native.log"

start kirin VKBRIDGE_FAKE_MISSING=$kirin VKBRIDGE_TEST_EXPORT_PAGES=driver
check "foreign-refused ahb" kirin
passed kirin && grep -q "the start's check passed" "$log" && grep -q "dma-buf over AHardwareBuffer: on (the driver has no dma-buf memory)" "$log" &&
  grep -q "VK_EXT_image_drm_format_modifier offered" "$work/check-kirin.log" && grep -q "made 3 export(s), 0 import(s)" "$log" &&
  grep -q "made 0 export(s), 2 import(s), refused 2" "$log"
step $? "2 imitated Kirin ($(tail -1 "$work/check-kirin.log"); $(grep -m1 -o 'OPTIMAL passed[^;]*' "$log"))" "$work/check-kirin.log"
if grep -q "vkbridge-server E" "$log"; then step 1 "server errors in the imitated run" "$log"; fi

start forced VKBRIDGE_DMABUF_VIA_AHB=1 VKBRIDGE_TEST_EXPORT_PAGES=driver
check ahb forced
passed forced && grep -q "dma-buf over AHardwareBuffer: on (VKBRIDGE_DMABUF_VIA_AHB=1" "$log" &&
  grep -q "VK_EXT_image_drm_format_modifier offered" "$work/check-forced.log"
step $? "3 VKBRIDGE_DMABUF_VIA_AHB=1 on a driver with dma-bufs ($(tail -1 "$work/check-forced.log"))" "$work/check-forced.log"

start off VKBRIDGE_FAKE_MISSING=$kirin VKBRIDGE_TEST_EXPORT_PAGES=driver VKBRIDGE_DMABUF_VIA_AHB=0
check "" off
[ "$(cat "$work/check-off.rc")" = 77 ] && grep -q "dma-buf over AHardwareBuffer: off (VKBRIDGE_DMABUF_VIA_AHB=0)" "$log"
step $? "4 imitated Kirin, VKBRIDGE_DMABUF_VIA_AHB=0: no dma-buf memory offered ($(tail -1 "$work/check-off.log"))" "$work/check-off.log"

start nopages VKBRIDGE_FAKE_MISSING=$kirin
check "" nopages
[ "$(cat "$work/check-nopages.rc")" = 77 ] && grep -q "dma-buf over AHardwareBuffer: not possible" "$log"
step $? "5 imitated Kirin without the buffers: not possible, nothing offered ($(tail -1 "$work/check-nopages.log"))" "$work/check-nopages.log"

stop
if [ $fails = 0 ]; then echo "OK: $steps steps"; else echo "FAILED: $fails of $steps steps"; exit 1; fi
