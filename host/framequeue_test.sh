#!/bin/bash
# The frame queue cap (VKBRIDGE_FRAME_QUEUE; client/wsi.c, wsi_wayland.c wl_wait_queue), each step
# with a server of its own. present_late_check on a Wayland surface (dma-buf images, the presenting
# thread) holds its first frame on the GPU for 300 ms, then presents 30 frames 16 ms apart (it
# prints how long each vkQueuePresentKHR took):
#   1  off (nothing set): the held frame's present returns at once, nothing said about a frame queue;
#   2  VKBRIDGE_FRAME_QUEUE=1 in the guest's environment: it returns only once its frame is rendered
#      (300 ms); the client says the cap and, at the end, that every present waited;
#   3  VKBRIDGE_FRAME_QUEUE=2 in <cache dir>/env (the server's answer): the cap said with 1 frame
#      allowed to still render, so the held frame (its own, no earlier one) returns at once;
#   4  VKBRIDGE_FRAME_QUEUE=0 in the guest beats <cache dir>/env's 1: as off;
#   5  VKBRIDGE_FRAME_QUEUE=1 on a headless surface (presents that wait for each frame already):
#      nothing changes (300 ms as without it), no waits counted.
# Skipped without a compositor in WAYLAND_DISPLAY. Prints one line per step and "OK: ..." /
# "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
icd=${VKB_CLIENT_ICD:-$out/vkbridge_icd.json}
work=$(mktemp -d)
sock=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-fq-$$.sock
srv=
fails=0
stop() { [ -n "$srv" ] && kill $srv 2>/dev/null && wait $srv 2>/dev/null; srv=; }
trap 'stop; [ -n "${VKB_KEEP:-}" ] && echo "kept $work" || rm -rf "$work"; rm -f "$sock"' EXIT
# start <name> [env...]: a server with cache $work/c ($work/c/env as written before), session folder $work/<name>
start() {
  local name=$1; shift
  stop
  rm -f "$sock"
  mkdir -p "$work/c" "$work/$name"
  sdir=$work/$name
  env -u VKBRIDGE_FAKE_MISSING -u VKBRIDGE_HIDE_EXTS -u VKBRIDGE_ERRATA -u VKBRIDGE_ERRATA_IMITATE -u VKBRIDGE_MODE \
      -u VKBRIDGE_FAKE_DRIVER_VERSION -u VKBRIDGE_ERRATA_PROBE -u VKBRIDGE_HITCH_MS -u VKBRIDGE_HANG_DETECT \
      -u VKBRIDGE_REPORT_DIR -u VKBRIDGE_FRAME_QUEUE -u VKBRIDGE_LATLOG VKBRIDGE_PROBE_BUDGET_S=0 "$@" \
      VK_DRIVER_FILES=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json} VK_ICD_FILENAMES= \
      "$out/vkbridge-server" --socket "$sock" --log "$sdir/server.log" --cache-dir "$work/c" >/dev/null 2>&1 &
  srv=$!
  for _ in $(seq 300); do [ -S "$sock" ] && break; kill -0 $srv 2>/dev/null || break; sleep 0.05; done
}
# run <name> [env...] -- <present_late_check args>: $res its exit status, $clog the client log, $lat the latency log
run() {
  local name=$1; shift
  local e=()
  while [ "$1" != -- ]; do e+=("$1"); shift; done
  shift
  clog=$work/$name.client.log
  : > "$clog"
  timeout 120 env -u VKBRIDGE_LATLOG -u VKBRIDGE_PRESENT_TIMEOUT_S -u VKBRIDGE_FRAME_QUEUE "${e[@]}" \
    VKBRIDGE_SOCKET="$sock" VK_DRIVER_FILES="$icd" VK_ICD_FILENAMES= VKBRIDGE_CLIENT_LOG="$clog" VKBRIDGE_LOG_LEVEL=2 \
    "$out/present_late_check" ok --delay-ms 300 --frames 30 --sleep-ms 16 "$@" >"$work/$name.out" 2>&1
  res=$?
  for _ in $(seq 100); do grep -q "client process [0-9]* gone" "$sdir/server.log" && break; sleep 0.05; done
}
step() { # step <status> <text>
  if [ "$1" = 0 ]; then echo "ok   $2"; else echo "FAIL $2"; fails=$((fails+1)); fi
}
last() { grep -E '^(OK|FAILED|SKIPPED)' "$work/$1.out" | tail -1; }
# held <name>: ms the held frame's vkQueuePresentKHR took
held() { grep -m1 -o 'frame 0: present [-0-9]* after [0-9]*' "$work/$1.out" | awk '{ print $NF }'; }
waited() { grep -o 'frame queue: [0-9]* of [0-9]* presents waited' "$clog" | tail -1; }

# 1 off
start off
run off -- --wayland
if grep -q '^SKIPPED' "$work/off.out"; then
  echo "OK: skipped (no compositor in WAYLAND_DISPLAY)"
  exit 0
fi
h=$(held off)
[ $res = 0 ] && [ "${h:-999}" -lt 100 ] && ! grep -q 'frame queue' "$clog"
step $? "1 off: the held frame's present returned after ${h:-?} ms ($(last off))"

# 2 the guest's environment, 1
start guest1
run guest1 VKBRIDGE_FRAME_QUEUE=1 -- --wayland
h=$(held guest1)
[ $res = 0 ] && [ "${h:-0}" -ge 290 ] &&
  grep -q 'frame queue: a present returns once at most 0 frames (its own included) are still rendering (VKBRIDGE_FRAME_QUEUE=1)' "$clog" &&
  waited | grep -q '^frame queue: 31 of 31 presents waited'
step $? "2 VKBRIDGE_FRAME_QUEUE=1, guest: the held frame's present returned after ${h:-?} ms; $(waited) ($(last guest1))"

# 3 <cache dir>/env, 2
echo "VKBRIDGE_FRAME_QUEUE=2" > "$work/c/env"
start envfile2
run envfile2 -- --wayland
h=$(held envfile2)
[ $res = 0 ] && [ "${h:-999}" -lt 100 ] &&
  grep -q 'frame queue: a present returns once at most 1 frame (its own included) is still rendering (VKBRIDGE_FRAME_QUEUE=2)' "$clog" &&
  waited | grep -q 'of 31 presents waited'
step $? "3 VKBRIDGE_FRAME_QUEUE=2, <cache dir>/env: the held frame's present returned after ${h:-?} ms; $(waited) ($(last envfile2))"

# 4 the guest's 0 beats <cache dir>/env
echo "VKBRIDGE_FRAME_QUEUE=1" > "$work/c/env"
start guest0
run guest0 VKBRIDGE_FRAME_QUEUE=0 -- --wayland
h=$(held guest0)
[ $res = 0 ] && [ "${h:-999}" -lt 100 ] && ! grep -q 'frame queue' "$clog"
step $? "4 VKBRIDGE_FRAME_QUEUE=0 in the guest, 1 in <cache dir>/env: the held frame's present returned after ${h:-?} ms ($(last guest0))"
rm -f "$work/c/env"

# 5 headless: the present waits for its frame anyway
start headless
run headless VKBRIDGE_FRAME_QUEUE=1 --
h=$(held headless)
[ $res = 0 ] && [ "${h:-0}" -ge 290 ] && ! grep -q 'presents waited' "$clog"
step $? "5 VKBRIDGE_FRAME_QUEUE=1, headless: the held frame's present returned after ${h:-?} ms (as without it), no waits counted ($(last headless))"
stop

if [ $fails = 0 ]; then echo "OK: the frame queue cap from the guest and <cache dir>/env, 1 and 2, off by 0 and by default"
else echo "FAILED: $fails step(s)"; exit 1; fi
