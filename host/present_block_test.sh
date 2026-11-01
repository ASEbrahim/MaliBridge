#!/bin/bash
# A present must not hold the presenting thread on GPU work that waits for that thread's later
# signal (wait-before-signal; client/wsi.c, VKBRIDGE_PRESENT_HOLD_MS), each step with a server of its
# own. tests/present_block_check, thread A named "vkd3d_queue" (vkd3d-proton presents from its queue
# thread): a frame whose work waits on T >= 1, presented, then T = 1 from the same thread:
#   1  headless surface (the copy path), T from the host: the present returns after the hold
#      (VKBRIDGE_PRESENT_HOLD_MS, 500 ms), frame 1 is not shown before T, shown after; the late
#      frame's line in the client log; no lost device;
#   2  headless, T by a submit on a second queue: the same (skipped with one queue);
#   3  headless, VKBRIDGE_PRESENT_HOLD_MS=0: the present returns at once;
#   4  Wayland surface, dma-buf images (the presenting thread): the present returns at once, and the
#      bridge's presenting thread is not named after the app's thread (one "vkd3d_queue" thread);
#   5  Wayland, VKBRIDGE_WSI_SHM=1 (the wl_shm copy path): as 1.
# 4 and 5 are skipped without a compositor in WAYLAND_DISPLAY. VKB_CLIENT_ICD=<icd json> runs it
# against another client build (one before the fix fails 1, 2, 3, 5: the watchdog ends the
# deadlock after 8 s). Prints one line per step and "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
icd=${VKB_CLIENT_ICD:-$out/vkbridge_icd.json}
work=$(mktemp -d)
sock=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-pblock-$$.sock
srv=
fails=0
stop() { [ -n "$srv" ] && kill $srv 2>/dev/null && wait $srv 2>/dev/null; srv=; }
trap 'stop; [ -n "${VKB_KEEP:-}" ] && echo "kept $work" || rm -rf "$work"; rm -f "$sock"' EXIT
start() { # start <name>: a server with cache $work/c
  stop
  rm -f "$sock"
  mkdir -p "$work/c" "$work/$1"
  env -u VKBRIDGE_FAKE_MISSING -u VKBRIDGE_HIDE_EXTS -u VKBRIDGE_ERRATA -u VKBRIDGE_ERRATA_IMITATE -u VKBRIDGE_MODE \
      -u VKBRIDGE_FAKE_DRIVER_VERSION -u VKBRIDGE_ERRATA_PROBE -u VKBRIDGE_HITCH_MS -u VKBRIDGE_HANG_DETECT \
      -u VKBRIDGE_REPORT_DIR -u VKBRIDGE_FRAME_QUEUE -u VKBRIDGE_LATLOG -u VKBRIDGE_PRESENT_WAIT -u VKBRIDGE_PRESENT_TIMEOUT_S \
      VKBRIDGE_PROBE_BUDGET_S=0 VK_DRIVER_FILES=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json} VK_ICD_FILENAMES= \
      "$out/vkbridge-server" --socket "$sock" --log "$work/$1/server.log" --cache-dir "$work/c" >/dev/null 2>&1 &
  srv=$!
  for _ in $(seq 300); do [ -S "$sock" ] && break; kill -0 $srv 2>/dev/null || break; sleep 0.05; done
}
run() { # run <name> [env...] -- <present_block_check args>: $res its exit status, $clog the client log
  local name=$1; shift
  local e=()
  while [ "$1" != -- ]; do e+=("$1"); shift; done
  shift
  start "$name"
  clog=$work/$name.client.log
  : > "$clog"
  timeout 60 env -u VKBRIDGE_LATLOG -u VKBRIDGE_FRAME_QUEUE -u VKBRIDGE_PRESENT_WAIT -u VKBRIDGE_PRESENT_HOLD_MS -u VKBRIDGE_WSI_SHM \
    -u VKBRIDGE_PRESENT_TIMEOUT_S "${e[@]}" VKBRIDGE_SOCKET="$sock" VK_DRIVER_FILES="$icd" VK_ICD_FILENAMES= VKBRIDGE_CLIENT_LOG="$clog" \
    VKBRIDGE_LOG_LEVEL=2 "$out/present_block_check" "$@" >"$work/$name.out" 2>&1
  res=$?
}
step() { # step <status> <text>
  if [ "$1" = 0 ]; then echo "ok   $2"; else echo "FAIL $2"; fails=$((fails+1)); fi
}
last() { grep -E '^(OK|FAILED|SKIPPED)' "$work/$1.out" | tail -1; }
pms() { grep -m1 -o 'signalled later by this thread): [-0-9]* after [0-9]* ms' "$work/$1.out" | awk '{ print $(NF-1) }'; }
show() { [ "$1" = 0 ] || sed 's/^/       /' "$work/$2.out" | grep -v '^       ok' | tail -8; }
named() { grep -o 'threads named vkd3d_queue: [0-9]* before the present, [0-9]* after' "$work/$1.out" | awk '{ print $(NF-1) }'; }

# 1 headless, host signal
run hl --
p=$(pms hl)
[ $res = 0 ] && [ "${p:-0}" -ge 450 ] && [ "${p:-9999}" -lt 1500 ] && grep -q 'not shown while its work waits: vkWaitForPresentKHR(1, 0) = 2' "$work/hl.out" &&
  grep -q 'swapchain: a frame not rendered within 500 ms is shown by the swapchain.s thread once it is' "$clog" && ! grep -q 'device is reported lost' "$clog"
s=$?; step $s "1 headless, T from the host: the present returned after ${p:-?} ms ($(last hl))"; show $s hl

# 2 headless, T by a submit on a second queue
run q2 -- --queue2
if [ $res = 3 ]; then echo "ok   2 headless, second queue: skipped ($(last q2))"
else
  p=$(pms q2)
  [ $res = 0 ] && [ "${p:-9999}" -lt 1500 ]
  s=$?; step $s "2 headless, T by a submit on a second queue: the present returned after ${p:-?} ms ($(last q2))"; show $s q2
fi

# 3 headless, no hold
run hold0 VKBRIDGE_PRESENT_HOLD_MS=0 --
p=$(pms hold0)
[ $res = 0 ] && [ "${p:-9999}" -lt 100 ]
s=$?; step $s "3 headless, VKBRIDGE_PRESENT_HOLD_MS=0: the present returned after ${p:-?} ms ($(last hold0))"; show $s hold0

# 4 Wayland, dma-buf
run wl -- --wayland
if [ $res = 3 ]; then echo "ok   4 Wayland: skipped ($(last wl))"
else
  p=$(pms wl); nm=$(named wl)
  [ $res = 0 ] && [ "${p:-9999}" -lt 100 ] && [ "${nm:-0}" = 1 ] && grep -q 'presented as dma-bufs' "$clog"
  s=$?; step $s "4 Wayland, dma-buf: the present returned after ${p:-?} ms; ${nm:-?} thread(s) named vkd3d_queue after it ($(last wl))"; show $s wl
  # 5 Wayland, wl_shm
  run shm VKBRIDGE_WSI_SHM=1 -- --wayland
  p=$(pms shm)
  [ $res = 0 ] && [ "${p:-0}" -ge 450 ] && [ "${p:-9999}" -lt 1500 ] && grep -q 'presented by copy' "$clog"
  s=$?; step $s "5 Wayland, wl_shm: the present returned after ${p:-?} ms ($(last shm))"; show $s shm
fi
stop

if [ $fails = 0 ]; then echo "OK: a present does not hold its thread on a frame that waits for that thread's later signal"
else echo "FAILED: $fails step(s)"; exit 1; fi
