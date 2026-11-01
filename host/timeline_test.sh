#!/bin/bash
# Timeline semaphores kept by the bridge where the driver has none (server_timeline.c): Mali r18p0 /
# r19p0 (a player log Mali-G52 Mali-G76, Vulkan 1.1) have no VK_KHR_timeline_semaphore, which
# gamescope (Vulkan 1.2) and Zink need. VKBRIDGE_FAKE_MISSING=timeline hides the desktop driver's
# and takes the bridge's path. tests/timeline_check.c's eight steps (host signal, GPU signal with
# the app's fence, a wait inside one submit, wait-before-signal from another thread, wait-any,
# binary semaphores beside it, 200 numbered submits) in each of:
#   1  the driver's own, through VK_KHR_timeline_semaphore (nothing of the bridge's in the way)
#   2  the driver's own, Vulkan 1.2 core names
#   3  imitated (FAKE_MISSING=timeline), the extension: the log says the bridge keeps them, and the
#      device's summary counts the driver submits
#   4  imitated, Vulkan 1.2 core names
#   5  imitated, VKBRIDGE_EMU_TIMELINE=0: not offered (the check skips), as before the emulation
# Prints one line per step and "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
work=$(mktemp -d)
sock=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-timeline-$$.sock
host_icd=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json}
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
  env -u VKBRIDGE_FAKE_MISSING -u VKBRIDGE_EMU_TIMELINE VKBRIDGE_PROBE_BUDGET_S=0 "$@" VK_DRIVER_FILES=$host_icd VK_ICD_FILENAMES= \
      "$out/vkbridge-server" --socket "$sock" --log "$log" --cache-dir "$d/c" >/dev/null 2>&1 &
  srv=$!
  for _ in $(seq 300); do [ -S "$sock" ] && break; kill -0 $srv 2>/dev/null || break; sleep 0.05; done
}
check() { timeout 60 "$out/timeline_check" $1 >"$work/check-$2.log" 2>&1; echo $? >"$work/check-$2.rc"; }
trap 'stop; rm -f "$sock"; [ -n "$KEEP" ] && echo "kept $work" || rm -rf "$work"' EXIT
export VKBRIDGE_SOCKET=$sock VK_DRIVER_FILES=$out/vkbridge_icd.json VK_ICD_FILENAMES=
unset VKBRIDGE_HIDE_EXTS VKBRIDGE_MAX_API VKBRIDGE_FAKE_MISSING VKBRIDGE_MODE
fails=0 steps=0
step() { steps=$((steps+1)); if [ "$1" = 0 ]; then echo "  ok   $2"; else echo "  FAIL $2"; fails=$((fails+1)); sed 's/^/       /' "$3"; fi; }
passed() { [ "$(cat "$work/check-$1.rc")" = 0 ] && grep -q "^OK: 0 failure" "$work/check-$1.log"; }

start native
check "" native
passed native; step $? "1 the driver's own, the extension ($(tail -1 "$work/check-native.log"))" "$work/check-native.log"
check core native-core
passed native-core && ! grep -q "timeline semaphores: emulated" "$log"
step $? "2 the driver's own, Vulkan 1.2 ($(tail -1 "$work/check-native-core.log")); nothing of the bridge's" "$work/check-native-core.log"

start emu VKBRIDGE_FAKE_MISSING=timeline
check "" emu
passed emu && grep -q "timeline semaphores: emulated with fences" "$log" && grep -q "timeline semaphores (emulated): [0-9]* driver submit" "$log"
step $? "3 imitated, the extension ($(tail -1 "$work/check-emu.log"); $(grep -m1 -o 'timeline semaphores (emulated): [0-9]* driver submit(s), [0-9]* CPU wait.*' "$log"))" "$work/check-emu.log"
check core emu-core
passed emu-core
step $? "4 imitated, Vulkan 1.2 ($(tail -1 "$work/check-emu-core.log"))" "$work/check-emu-core.log"
if grep -q "vkbridge-server E" "$log"; then step 1 "server errors in the imitated runs" "$log"; fi

start off VKBRIDGE_FAKE_MISSING=timeline VKBRIDGE_EMU_TIMELINE=0
check "" off
[ "$(cat "$work/check-off.rc")" = 77 ]
step $? "5 imitated, VKBRIDGE_EMU_TIMELINE=0: not offered ($(tail -1 "$work/check-off.log"))" "$work/check-off.log"

stop
if [ $fails = 0 ]; then echo "OK: $steps steps"; else echo "FAILED: $fails of $steps steps"; exit 1; fi
