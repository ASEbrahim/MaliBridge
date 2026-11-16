#!/bin/bash
# v0.5.5.4: the pacer's "on" steps also ask for VKBRIDGE_ASYNC_ACQUIRE=1 and VKBRIDGE_PRESENT_RELEASE_THREAD=1, which are off by
# default in this build (the pacer was made on top of them; with them off, VKBRIDGE_PACER=1 alone changes nothing).
# The frame pacer (VKBRIDGE_PACER=1, client/pacer.c): a DXVK-shaped game (tests/pacer_check: engine "DXVK", frame latency
# gate 3, an emulated GPU) under the fake gamescope (tests/fake_nested_wayland.py; memfd stand-ins give dma-buf images on
# lavapipe), one server per step:
#   1  GPU-bound (12 ms GPU, 1 ms CPU), pacer off: present wait not offered to DXVK, no pacer line, the gate keeps
#      >= 2.5 frames between a present and its GPU done (the queue the pacer is for);
#   2  the same with VKBRIDGE_PACER=1: present wait offered, the pacer says it engaged, present->done <= 1.6 frames,
#      fps >= 0.98 x step 1, late frames (the GPU waited for one) <= 5%;
#   3  CPU-bound (3 ms GPU, 12 ms CPU), pacer off; 4 the same with the pacer on: never engaged (the GPU is not the
#      limit), fps within 2% of step 3.
#   5  GPU-bound, VKBRIDGE_PACER=1 only in <cache dir>/env (the server's switch file; the app writes it), nothing in the
#      game's environment: the client takes it from the server (vkb_server_pd_info.client_knobs) as from the guest: present
#      wait offered, the pacer engaged;
#   6  the same file with the guest's VKBRIDGE_PACER=0: the guest wins, no pacer, present wait not offered.
#   Steps 1, 2, 5, 6: one 'perf-switches:' line per device in the client log, each switch with its value and where it came
#   from (1 PACER=0(default) PRESENT_WAIT=0(default), 2 PACER=1(guest env) PRESENT_WAIT=1(pacer), 5 PACER=1(env file),
#   6 PACER=0(guest env)).
# Skipped without python3 or a working compositor. One line per step, "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
icd=${VKB_CLIENT_ICD:-$out/vkbridge_icd.json}
work=$(mktemp -d "${XDG_RUNTIME_DIR:-/tmp}/vkb-pc.XXXX") # short: socket paths
sock=$work/s.sock
srv=
fake=
fails=0
stop() { [ -n "$srv" ] && kill $srv 2>/dev/null && wait $srv 2>/dev/null; srv=; }
stop_fake() { [ -n "$fake" ] && kill $fake 2>/dev/null && wait $fake 2>/dev/null; fake=; }
trap 'stop; stop_fake; [ -n "${VKB_KEEP:-}" ] && echo "kept $work" || rm -rf "$work"' EXIT
start() { # start <name> [env...]: a server of its own
  stop
  local name=$1; shift
  rm -f "$sock"
  mkdir -p "$work/c"
  env -u VKBRIDGE_FAKE_MISSING -u VKBRIDGE_HIDE_EXTS -u VKBRIDGE_ERRATA -u VKBRIDGE_ERRATA_IMITATE -u VKBRIDGE_MODE \
      -u VKBRIDGE_FRAME_QUEUE -u VKBRIDGE_LATLOG -u VKBRIDGE_HITCH_MS -u VKBRIDGE_PRESENT_WAIT -u VKBRIDGE_PACER \
      VKBRIDGE_PROBE_BUDGET_S=0 VKBRIDGE_TEST_SCANOUT=memfd "$@" \
      VK_DRIVER_FILES=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json} VK_ICD_FILENAMES= \
      "$out/vkbridge-server" --socket "$sock" --log "$work/$name.server.log" --cache-dir "$work/c" >/dev/null 2>&1 &
  srv=$!
  for _ in $(seq 300); do [ -S "$sock" ] && break; kill -0 $srv 2>/dev/null || break; sleep 0.05; done
}
fake_start() {
  stop_fake
  rm -f "$work/nested.sock" "$work/host.sock" "$work/fake.out"
  python3 -I ../tests/fake_nested_wayland.py --nested "$work/nested.sock" --host "$work/host.sock" --log "$work/fake.jsonl" \
    > "$work/fake.out" 2>&1 &
  fake=$!
  for _ in $(seq 400); do grep -q ready "$work/fake.out" 2>/dev/null && break; kill -0 $fake 2>/dev/null || break; sleep 0.05; done
}
run() { # run <name> [env...] -- <pacer_check args>: $res its status, $clog the client log
  local name=$1; shift
  local e=()
  while [ "$1" != -- ]; do e+=("$1"); shift; done
  shift
  clog=$work/$name.client.log
  : > "$clog"
  start "$name" $tlenv
  fake_start
  timeout ${RUN_TIMEOUT:-120} env -u VKBRIDGE_WSI_AHB -u VKBRIDGE_WSI_SHM -u VKBRIDGE_PRESENT_WAIT -u VKBRIDGE_PACER -u VKBRIDGE_FRAME_QUEUE \
    -u VKBRIDGE_LATLOG -u DISPLAY "${e[@]}" \
    WAYLAND_DISPLAY="$work/nested.sock" VKBRIDGE_HOST_WAYLAND="$work/host.sock" VKBRIDGE_SOCKET="$sock" \
    VK_DRIVER_FILES="$icd" VK_ICD_FILENAMES= VKBRIDGE_CLIENT_LOG="$clog" VKBRIDGE_LOG_LEVEL=2 \
    "$out/pacer_check" "$@" >"$work/$name.out" 2>&1
  res=$?
}
step() { if [ "$1" = 0 ]; then echo "ok   $2"; else echo "FAIL $2"; fails=$((fails+1)); fi; }
result() { grep -m1 '^RESULT' "$work/$1.out" | sed 's/^RESULT //'; }
field() { # field <name> <word>: the number after <word> on the RESULT line
  grep -m1 '^RESULT' "$work/$1.out" | awk -v w="$2" '{ for (i = 1; i < NF; i++) if ($i == w) { v = $(i+1); gsub(/[^0-9.]/, "", v); print v; exit } }'
}
frames() { grep -m1 -o '= [0-9.]* frames' "$work/$1.out" | awk '{ print $2 }'; }
latepct() { grep -m1 -o 'late [0-9.]*%' "$work/$1.out" | tr -dc '0-9.'; }
# perfsw <name> <want>...: one 'perf-switches:' line per device created, with each <want> (fixed strings)
perfsw() {
  local c=$work/$1.client.log n d w; shift
  n=$(grep -c 'perf-switches:' "$c"); d=$(grep -c 'device created on' "$c")
  [ "$n" -ge 1 ] && [ "$n" = "$d" ] || return 1
  for w in "$@"; do grep 'perf-switches:' "$c" | grep -qF -- "$w" || return 1; done
}
pswline() { grep -m1 -o 'perf-switches:.*' "$work/$1.client.log" || echo 'no perf-switches line'; }
show() { sed 's/^/     /' "$work/$1.out" | tail -6; grep -E "pacer|error|warn" "$work/$1.client.log" 2>/dev/null | grep -v "^.*pacer_check [0-9]*]: [^p]" | head -40 | sed 's/^/     client: /'; }
ge() { awk -v a="$1" -v b="$2" 'BEGIN { exit !(a + 0 >= b + 0) }'; }

if ! command -v python3 >/dev/null; then echo "SKIPPED: no python3"; exit 0; fi

# The emulated GPU signals a timeline semaphore that the frame's submit already waits for (wait-before-signal), and the app
# runs ahead of it when it is GPU-bound. A host driver whose vkQueueSubmit blocks on such a wait (Mesa 26.0.x lavapipe, the
# GitHub runner's) stalls the program: the signal is a later request of the same program, held back behind the blocked
# submit (the GPU-bound steps then print "a frame is taking more than 10 s"). A probe of 40 GPU-bound frames finds out; if it
# stalls, the servers run on the bridge's own timeline (VKBRIDGE_FAKE_MISSING=timeline, which parks such a submit on a worker)
# and the log says so. The pacer under test is the client's; the timeline is only the test's GPU stand-in. Never a silent pass:
# the steps below still judge the same numbers.
tlenv=
RUN_TIMEOUT=20 run probe -- --gpu-ms 12 --cpu-ms 1 --frames 40 --measure 10
if [ $res = 3 ]; then echo "SKIPPED: $(grep -m1 SKIPPED "$work/probe.out")"; exit 0; fi
if [ $res != 0 ]; then
  echo "NOTE: lavapipe 26.0.8 (the CI runner's) blocks in vkQueueSubmit on a timeline wait-before-signal (probe of 40 GPU-bound frames stalled, status $res); timeline faked for this step (VKBRIDGE_FAKE_MISSING=timeline)"
  tlenv=VKBRIDGE_FAKE_MISSING=timeline
fi

GB="--gpu-ms 12 --cpu-ms 1 --frames 700 --measure 250"
CB="--gpu-ms 3 --cpu-ms 12 --frames 600 --measure 250"

# 1 GPU-bound, off
run gb_off -- $GB
if [ $res = 3 ]; then echo "SKIPPED: $(grep -m1 SKIPPED "$work/gb_off.out")"; exit 0; fi
f1=$(frames gb_off); fps1=$(field gb_off fps)
[ $res = 0 ] && grep -q 'present wait not offered' "$work/gb_off.out" && ! grep -q 'pacer:' "$work/gb_off.client.log" && ge "$f1" 2.5
s=$?; step $s "1 GPU-bound, pacer off: $(result gb_off)"; [ $s = 0 ] || show gb_off
perfsw gb_off 'PACER=0(default)' 'PRESENT_WAIT=0(default)'
step $? "1 perf-switches: $(pswline gb_off)"

# 2 GPU-bound, on
run gb_on VKBRIDGE_PACER=${PACER_ENV:-1} VKBRIDGE_ASYNC_ACQUIRE=1 VKBRIDGE_PRESENT_RELEASE_THREAD=1 -- $GB
f2=$(frames gb_on); fps2=$(field gb_on fps); l2=$(latepct gb_on)
[ $res = 0 ] && grep -q 'present wait offered' "$work/gb_on.out" && grep -q 'pacer: engaged' "$work/gb_on.client.log" && \
  ge 1.6 "${f2:-9}" && ge "${fps2:-0}" "$(awk -v a="$fps1" 'BEGIN { print a * 0.98 }')" && ge 5 "${l2:-100}"
s=$?; step $s "2 GPU-bound, VKBRIDGE_PACER=1: $(result gb_on) (off: $f1 frames, $fps1 fps)"; [ $s = 0 ] || show gb_on
perfsw gb_on 'PACER=1(guest env)' 'PRESENT_WAIT=1(pacer)'
step $? "2 perf-switches: $(pswline gb_on)"

# 3 + 4 CPU-bound, off and on
run cb_off -- $CB
fps3=$(field cb_off fps)
[ $res = 0 ]
s=$?; step $s "3 CPU-bound, pacer off: $(result cb_off)"; [ $s = 0 ] || show cb_off
run cb_on VKBRIDGE_PACER=${PACER_ENV:-1} VKBRIDGE_ASYNC_ACQUIRE=1 VKBRIDGE_PRESENT_RELEASE_THREAD=1 -- $CB
fps4=$(field cb_on fps)
[ $res = 0 ] && ! grep -q 'pacer: engaged' "$work/cb_on.client.log" && \
  ge "${fps4:-0}" "$(awk -v a="$fps3" 'BEGIN { print a * 0.98 }')" && ge "$(awk -v a="$fps3" 'BEGIN { print a * 1.02 }')" "${fps4:-0}"
s=$?; step $s "4 CPU-bound, VKBRIDGE_PACER=1 (never engaged, fps within 2%): $(result cb_on) (off: $fps3 fps)"; [ $s = 0 ] || show cb_on

# 5 GPU-bound, VKBRIDGE_PACER=1 in <cache dir>/env only (what the app's Performance setting writes)
mkdir -p "$work/c"
printf 'VKBRIDGE_PACER=1\nVKBRIDGE_ASYNC_ACQUIRE=1\nVKBRIDGE_PRESENT_RELEASE_THREAD=1\n' > "$work/c/env"
run gb_file -- $GB
f5=$(frames gb_file); fps5=$(field gb_file fps)
[ $res = 0 ] && grep -q 'present wait offered' "$work/gb_file.out" && grep -q 'pacer: engaged' "$work/gb_file.client.log" && \
  ge 1.6 "${f5:-9}" && ge "${fps5:-0}" "$(awk -v a="$fps1" 'BEGIN { print a * 0.98 }')"
s=$?; step $s "5 GPU-bound, VKBRIDGE_PACER=1 in <cache dir>/env only: $(result gb_file) [$(grep -m1 -o 'pacer: [a-z ]*' "$work/gb_file.client.log" || echo 'no pacer line')]"
[ $s = 0 ] || show gb_file
perfsw gb_file 'PACER=1(env file)' 'PRESENT_WAIT=1(pacer)'
step $? "5 perf-switches: $(pswline gb_file)"

# 6 the same file, the guest's VKBRIDGE_PACER=0 wins
run gb_guest0 VKBRIDGE_PACER=0 -- $GB
[ $res = 0 ] && grep -q 'present wait not offered' "$work/gb_guest0.out" && ! grep -q 'pacer:' "$work/gb_guest0.client.log"
s=$?; step $s "6 <cache dir>/env VKBRIDGE_PACER=1, guest VKBRIDGE_PACER=0 (the guest wins: no pacer): $(result gb_guest0)"
[ $s = 0 ] || show gb_guest0
perfsw gb_guest0 'PACER=0(guest env)' 'PRESENT_WAIT=0(default)'
step $? "6 perf-switches: $(pswline gb_guest0)"
rm -f "$work/c/env"

if [ $fails = 0 ]; then echo "OK: the pacer cut the queue from $f1 to $f2 frames at $fps2 vs $fps1 fps and stayed off when CPU-bound"
else echo "FAILED: $fails step(s)"; exit 1; fi
