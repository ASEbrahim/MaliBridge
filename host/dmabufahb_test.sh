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
# Mali-G52 r18p0 (Kirin 810) refusing LINEAR images with storage usage (VKBRIDGE_FAKE_MISSING=linearstorage):
#   6  gamescope's images (gsexport_check) asked, made with the usage the driver takes, exported and imported
#   7  the same without the refusal: nothing reduced
# A driver whose start's check fails OPTIMAL images and passes LINEAR ones (OPTIMAL read back 0x00000000;
# VKBRIDGE_TEST_DAB_WRONG=optimal imitates it on the host):
#   8  the path on, LINEAR only: gamescope finds the dma-buf extensions and the LINEAR modifier, Zink's LINEAR image is
#      exported, imported as gamescope does (made LINEAR) and read back; what cannot be LINEAR is refused
#   9  gamescope's images (gsexport_check) asked, made, exported and imported in that mode
#  10  both tilings failing (VKBRIDGE_TEST_DAB_WRONG=optimal,linear): nothing offered, as before
# Prints one line per step and "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
work=$(mktemp -d)
sock=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-dmabufahb-$$.sock
host_icd=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json}
kirin=dmabufext
# The pages: the host driver's own dma-bufs, or memfds where it exports none (lavapipe without /dev/udmabuf,
# the CI and the PC while the owner plays); lavapipe has no modifier extension of its own (step 1).
case $host_icd in *lvp*) pages=${VKB_DAB_PAGES:-memfd} native_mods="not offered" ;; *) pages=${VKB_DAB_PAGES:-driver} native_mods=offered ;; esac
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
  env -u VKBRIDGE_FAKE_MISSING -u VKBRIDGE_DMABUF_VIA_AHB -u VKBRIDGE_TEST_EXPORT_PAGES -u VKBRIDGE_TEST_DAB_WRONG VKBRIDGE_PROBE_BUDGET_S=0 "$@" \
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
grep -q "VK_EXT_image_drm_format_modifier $native_mods" "$work/check-native.log" && ! grep -q "dma-buf over AHardwareBuffer: on" "$log"
step $? "1 the driver as it is: its own dma-bufs, the modifier extension $native_mods (the driver's), the path off" "$work/check-native.log"

start kirin VKBRIDGE_FAKE_MISSING=$kirin VKBRIDGE_TEST_EXPORT_PAGES=$pages
check "foreign-refused ahb" kirin
passed kirin && grep -q "the start's check passed" "$log" && grep -q "dma-buf over AHardwareBuffer: on (the driver has no dma-buf memory)" "$log" &&
  grep -q "VK_EXT_image_drm_format_modifier offered" "$work/check-kirin.log" && grep -q "made 3 export(s), 0 import(s)" "$log" &&
  grep -q "made 0 export(s), 2 import(s), refused 2" "$log"
step $? "2 imitated Kirin ($(tail -1 "$work/check-kirin.log"); $(grep -m1 -o 'OPTIMAL passed[^;]*' "$log"))" "$work/check-kirin.log"
if grep -q "vkbridge-server E" "$log"; then step 1 "server errors in the imitated run" "$log"; fi

start forced VKBRIDGE_DMABUF_VIA_AHB=1 VKBRIDGE_TEST_EXPORT_PAGES=$pages
check ahb forced
passed forced && grep -q "dma-buf over AHardwareBuffer: on (VKBRIDGE_DMABUF_VIA_AHB=1" "$log" &&
  grep -q "VK_EXT_image_drm_format_modifier offered" "$work/check-forced.log"
step $? "3 VKBRIDGE_DMABUF_VIA_AHB=1 on a driver with dma-bufs ($(tail -1 "$work/check-forced.log"))" "$work/check-forced.log"

start off VKBRIDGE_FAKE_MISSING=$kirin VKBRIDGE_TEST_EXPORT_PAGES=$pages VKBRIDGE_DMABUF_VIA_AHB=0
check "" off
[ "$(cat "$work/check-off.rc")" = 77 ] && grep -q "dma-buf over AHardwareBuffer: off (VKBRIDGE_DMABUF_VIA_AHB=0)" "$log"
step $? "4 imitated Kirin, VKBRIDGE_DMABUF_VIA_AHB=0: no dma-buf memory offered ($(tail -1 "$work/check-off.log"))" "$work/check-off.log"

start nopages VKBRIDGE_FAKE_MISSING=$kirin
check "" nopages
[ "$(cat "$work/check-nopages.rc")" = 77 ] && grep -q "dma-buf over AHardwareBuffer: not possible" "$log"
step $? "5 imitated Kirin without the buffers: not possible, nothing offered ($(tail -1 "$work/check-nopages.log"))" "$work/check-nopages.log"

# Kirin's Mali-G52 r18p0: the driver refuses a LINEAR image with storage usage (VKBRIDGE_FAKE_MISSING=linearstorage),
# so gamescope's LINEAR-modifier queries (BGRA8, SAMPLED | STORAGE | TRANSFER_SRC, usage 0xd, MUTABLE_FORMAT with a view list)
# were refused, it found no modifier and stopped on "modifiers.size > 0" (Steam died 10 s in). The bridge answers
# them for the usage the driver makes and makes the image with it; gsexport_check asks and makes gamescope's images
# (queries, a list with LINEAR, export, import at the explicit LINEAR modifier) through the path.
#   6  the refusal imitated: gamescope's images asked, made, exported, imported; the log says which usage they got
#   7  the driver as it is (imitated Kirin, no refusal): the same, and nothing reduced (a driver that takes 0xd is unchanged)
start gslinear VKBRIDGE_FAKE_MISSING=$kirin,linearstorage VKBRIDGE_TEST_EXPORT_PAGES=$pages
rm -f "$work/gslinear-client.log"
VKBRIDGE_CLIENT_LOG=$work/gslinear-client.log timeout 60 "$out/gsexport_check" >"$work/gsexport-linear.log" 2>&1
rc=$?
[ $rc = 0 ] && grep -q "imitating r18p0's refusal of LINEAR images with storage usage" "$log" &&
  grep -q "dma-buf over AHardwareBuffer: the driver refuses a LINEAR image of format 44 with usage 0xd; answered for an image made with usage 0x5" "$log" &&
  grep -q "dma-buf over AHardwareBuffer: the driver refuses a LINEAR image of format 44 with usage 0xf; image made with usage 0x7" "$log" &&
  ! grep -q "the LINEAR-modifier query stays refused" "$work/gslinear-client.log" && ! grep -q "vkbridge-server E" "$log"
step $? "6 imitated r18p0 (no LINEAR storage): gamescope's images asked, made with the usage the driver takes, exported, imported ($(tail -1 "$work/gsexport-linear.log"))" "$work/gsexport-linear.log"

start gsplain VKBRIDGE_FAKE_MISSING=$kirin VKBRIDGE_TEST_EXPORT_PAGES=$pages
timeout 60 "$out/gsexport_check" >"$work/gsexport-plain.log" 2>&1
rc=$?
[ $rc = 0 ] && ! grep -q "the driver refuses a LINEAR image" "$log" && ! grep -q "vkbridge-server E" "$log"
step $? "7 imitated Kirin, LINEAR storage taken: gamescope's images as before, no usage reduced ($(tail -1 "$work/gsexport-plain.log"))" "$work/gsexport-plain.log"

start linonly VKBRIDGE_FAKE_MISSING=$kirin VKBRIDGE_TEST_EXPORT_PAGES=$pages VKBRIDGE_TEST_DAB_WRONG=optimal
check "foreign-refused ahb linear-only" linonly
passed linonly && grep -q "OPTIMAL FAILED: texel 0x00000000 WRONG; LINEAR passed" "$log" &&
  grep -q "dma-buf over AHardwareBuffer: on, LINEAR only (the driver has no dma-buf memory; the start's check failed OPTIMAL images and passed LINEAR ones)" "$log" &&
  grep -q "VK_EXT_image_drm_format_modifier offered" "$work/check-linonly.log" &&
  grep -q "dma-buf image [0-9a-f]* (pid [0-9]*): format 44 320x200, tiling 0 made 1" "$log" &&
  grep -q "LINEAR only: a dma-buf image of format 126 .* cannot be LINEAR; refused" "$log" &&
  grep -q "LINEAR only: .* not exported" "$log" && ! grep -q "vkbridge-server E" "$log"
step $? "8 OPTIMAL failed, LINEAR passed: on, LINEAR only ($(tail -1 "$work/check-linonly.log"))" "$work/check-linonly.log"

start gslinonly VKBRIDGE_FAKE_MISSING=$kirin VKBRIDGE_TEST_EXPORT_PAGES=$pages VKBRIDGE_TEST_DAB_WRONG=optimal
timeout 60 "$out/gsexport_check" >"$work/gsexport-linonly.log" 2>&1
rc=$?
[ $rc = 0 ] && grep -q "on, LINEAR only" "$log" && ! grep -q "vkbridge-server E" "$log"
step $? "9 OPTIMAL failed, LINEAR passed: gamescope's images asked, made, exported, imported ($(tail -1 "$work/gsexport-linonly.log"))" "$work/gsexport-linonly.log"

start bothwrong VKBRIDGE_FAKE_MISSING=$kirin VKBRIDGE_TEST_EXPORT_PAGES=$pages VKBRIDGE_TEST_DAB_WRONG=optimal,linear
check "" bothwrong
[ "$(cat "$work/check-bothwrong.rc")" = 77 ] && grep -q "OPTIMAL FAILED: texel 0x00000000 WRONG; LINEAR FAILED: texel 0x00000000 WRONG" "$log" &&
  grep -q "wanted (the driver has no dma-buf memory) but the start's check failed" "$log"
step $? "10 both tilings failed: nothing offered ($(tail -1 "$work/check-bothwrong.log"))" "$work/check-bothwrong.log"

stop
if [ $fails = 0 ]; then echo "OK: $steps steps"; else echo "FAILED: $fails of $steps steps"; exit 1; fi
