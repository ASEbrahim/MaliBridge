#!/bin/bash
# Two server-side switches, on the desktop driver with a server of its own (its own socket and
# cache directory, so <cache dir>/env is the tablet's way of setting them):
#   1  a semaphore wait the driver fails with -3 (VKBRIDGE_WAIT_FAKE_ERROR imitates the Mali-G925)
#      reaches the app as VK_SUCCESS once the semaphore is signalled (server_wait.c);
#   2  the same with VKBRIDGE_WAIT_FIX=0: the app sees -3 (the negative control);
#   3  VKBRIDGE_FAKE_MISSING / VKBRIDGE_SERVER_HIDE_EXTS from <cache dir>/env reach the startup
#      self-test: the native features are emulated and the hidden driver extensions are not
#      offered, with no client-side setting (a newer Mali taking an older one's paths).
# Prints one line per step and "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
work=$(mktemp -d)
cache=$work/c
mkdir -p "$cache"
sock=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-wait-$$.sock
log=$work/server.log
srv=
start() {
  [ -n "$srv" ] && kill $srv 2>/dev/null && wait $srv 2>/dev/null
  rm -f "$sock"
  env -u VKBRIDGE_FAKE_MISSING -u VKBRIDGE_HIDE_EXTS VK_DRIVER_FILES=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json} VK_ICD_FILENAMES= \
      "$out/vkbridge-server" --socket "$sock" --log "$log" --cache-dir "$cache" >/dev/null 2>&1 &
  srv=$!
  for _ in $(seq 50); do [ -S "$sock" ] && break; sleep 0.1; done
}
trap 'kill $srv 2>/dev/null; rm -f "$sock"; [ -n "$KEEP" ] || rm -rf "$work"' EXIT
export VKBRIDGE_SOCKET=$sock VK_DRIVER_FILES=$out/vkbridge_icd.json VK_ICD_FILENAMES=
unset VKBRIDGE_HIDE_EXTS
fails=0
step() { if [ "$1" = 0 ]; then echo "  ok   $2"; else echo "  FAIL $2"; fails=$((fails+1)); fi; }

start
# 1
printf 'VKBRIDGE_WAIT_FAKE_ERROR=1\n' > "$cache/env"
timeout 120 "$out/vkbridge-smoke" >$work/1.log 2>&1 && grep -q "PASS timeline semaphore" $work/1.log &&
  grep -q "vkWaitSemaphores: the driver failed the wait (-3).*app gets 0" "$log"
step $? "wait failed with -3 by the driver, semaphore signalled: the app gets VK_SUCCESS ($(tail -1 $work/1.log))"
# 2
printf 'VKBRIDGE_WAIT_FAKE_ERROR=1\nVKBRIDGE_WAIT_FIX=0\n' > "$cache/env"
timeout 120 "$out/vkbridge-smoke" >$work/2.log 2>&1
grep -q "FAIL timeline semaphore" $work/2.log
step $? "negative control, VKBRIDGE_WAIT_FIX=0: the app sees the driver's error ($(grep -m1 'timeline semaphore' $work/2.log))"
# 3
printf 'VKBRIDGE_FAKE_MISSING=depthclip,push,maint5,maint6,dualsrc\nVKBRIDGE_SERVER_HIDE_EXTS=VK_EXT_extended_dynamic_state3\n' > "$cache/env"
: > "$log"
start
timeout 60 vulkaninfo >$work/3.vi 2>&1
timeout 120 "$out/vkbridge-smoke" >$work/3.log 2>&1 &&
  grep -q "device extensions hidden from clients: VKBRIDGE_FAKE_MISSING=depthclip" "$log" &&
  emu=$(grep -m1 -o 'emulating 0x[0-9a-f]*' "$log" | cut -d' ' -f2) && [ $((emu & 0x5260)) = $((0x5260)) ] &&
  ! grep -q "VK_EXT_extended_dynamic_state3" $work/3.vi && grep -q "VK_KHR_push_descriptor" $work/3.vi &&
  grep -q "VK_EXT_depth_clip_enable" $work/3.vi
step $? "fake-missing from <cache dir>/env: emulation $emu, EDS3 hidden, emulated push/depth clip offered ($(tail -1 $work/3.log))"

if [ $fails = 0 ]; then echo "OK: 3 steps"; else echo "FAILED: $fails of 3 steps"; exit 1; fi
