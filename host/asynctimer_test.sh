#!/bin/bash
# The client's async-send timer (client/conn.c "async sends" lines; tests/asynctimer_check): a thread named "asyncer" queues
# vkResetFences for 3.5 s while the server is stopped for one second in the middle (the socket fills, the flush blocks).
#   1  VKBRIDGE_ASYNC_TIMER=1 (every call timed; the default samples 1 in 64 for the plain append; lock waits and flushes are timed exactly): the client log has "async sends over"
#      lines for asyncer whose calls add up to >= 80 % of the calls the program made, with an estimated queuing time > 0 and
#      a longest call (max_us) >= 200 ms (the stopped second showed up in a call: the stall is named, not lost);
#   2  VKBRIDGE_ASYNC_TIMER=0: no such line;

#   3  the default (a thread says its line only when queuing took >= 1 % of a window of a second or more, so the last short window is
#      silent): the lines add up to >= 50 % of the calls, the counts exact (counted every call), and the stopped second is in the
#      estimate once (800-3500 ms), not scaled by the sampling.
#   4  a 0.3 s burst of thousands of calls: no line by default (a window under a second is no evidence; tests/gl read stderr).
# One line per step, "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
icd=${VKB_CLIENT_ICD:-$out/vkbridge_icd.json}
work=$(mktemp -d "${XDG_RUNTIME_DIR:-/tmp}/vkb-at.XXXX")
sock=$work/s.sock
srv=
fails=0
stop() { [ -n "$srv" ] && kill -CONT $srv 2>/dev/null; [ -n "$srv" ] && kill $srv 2>/dev/null && wait $srv 2>/dev/null; srv=; }
trap 'stop; [ -n "${VKB_KEEP:-}" ] && echo "kept $work" || rm -rf "$work"' EXIT
mkdir -p "$work/c"
# run <name> [env...]: a server of its own, the program with the env, the server stopped from 1.0 to 2.0 s
run() {
  local name=$1; shift
  rm -f "$sock"
  env -u VKBRIDGE_FAKE_MISSING -u VKBRIDGE_HIDE_EXTS -u VKBRIDGE_MODE VKBRIDGE_PROBE_BUDGET_S=0 \
      VK_DRIVER_FILES=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json} VK_ICD_FILENAMES= \
      "$out/vkbridge-server" --socket "$sock" --log "$work/$name.server.log" --cache-dir "$work/c" >/dev/null 2>&1 &
  srv=$!
  for _ in $(seq 300); do [ -S "$sock" ] && break; sleep 0.05; done
  ( sleep 1.0; kill -STOP $srv 2>/dev/null; sleep 1.0; kill -CONT $srv 2>/dev/null ) &
  local hold=$!
  env "$@" VKBRIDGE_SOCKET="$sock" VK_DRIVER_FILES="$icd" VK_ICD_FILENAMES= VKBRIDGE_WAIT_STATS=1 VKBRIDGE_CLIENT_LOG="$work/$name.client.log" \
      VKBRIDGE_LOG_LEVEL=2 timeout 60 "$out/asynctimer_check" ${runms:-3500} >"$work/$name.out" 2>&1
  res=$?
  wait $hold 2>/dev/null
  stop
  calls=$(grep -o "calls=[0-9]*" "$work/$name.out" | cut -d= -f2)
  lines=$(grep -c "async sends over .*thread asyncer" "$work/$name.client.log" 2>/dev/null)
  sum=$(grep "async sends over .*thread asyncer" "$work/$name.client.log" 2>/dev/null | grep -o ": [0-9]* calls" | awk '{s+=$2} END {print s+0}')
  est=$(grep "async sends over .*thread asyncer" "$work/$name.client.log" 2>/dev/null | grep -o "queuing ~[0-9.]* ms" | tr -d "queuing~ ms" | awk '{s+=$1} END {print s+0}')
  mx=$(grep "async sends over .*thread asyncer" "$work/$name.client.log" 2>/dev/null | grep -o "max_us=[0-9]*" | cut -d= -f2 | sort -n | tail -1)
}
step() { if [ "$1" = 0 ]; then echo "ok   $2"; else echo "FAIL $2"; fails=$((fails+1)); fi; }

run all VKBRIDGE_ASYNC_TIMER=1
awk -v c="${calls:-0}" -v s="${sum:-0}" -v e="${est:-0}" -v m="${mx:-0}" 'BEGIN {exit !(c > 1000 && s >= 0.8 * c && s <= c && e > 0 && m >= 200000)}'; st=$?
step $st "1 timer on, every call: program $calls calls, lines $lines add up to $sum, ~${est} ms queuing, longest call ${mx:-?} us"
[ $st = 0 ] || { grep "async sends" "$work/all.client.log" | tail -3 | cut -c1-300; tail -2 "$work/all.out"; }

run off VKBRIDGE_ASYNC_TIMER=0
[ $res = 0 ] && [ "${lines:-0}" = 0 ]; st=$?
step $st "2 timer off: $lines lines"

run def
awk -v c="${calls:-0}" -v s="${sum:-0}" -v e="${est:-0}" 'BEGIN {exit !(c > 1000 && s >= 0.5 * c && s <= c && e >= 800 && e <= 3500)}'; st=$?
step $st "3 default: program $calls calls, lines $lines add up to $sum, ~${est} ms queuing (the stopped second, counted once: 800-3500)"
[ $st = 0 ] || { grep "async sends" "$work/def.client.log" | tail -3 | cut -c1-300; tail -2 "$work/def.out"; }

# 4 a short burst (0.3 s, thousands of calls) says nothing by default: a window under a second is no evidence, and the Zink
# checks (tests/gl) read the programs' stderr (a line at every thread's exit failed imageless_test 11-08)
runms=300 run quiet
[ $res = 0 ] && [ "${calls:-0}" -gt 1000 ] && [ "${lines:-0}" = 0 ]; st=$?
step $st "4 a 0.3 s burst of $calls calls: $lines lines"

if [ $fails = 0 ]; then echo "OK: async-send lines per thread with calls, estimated queuing ms and the longest call; off switch"
else echo "FAILED: $fails step(s)"; exit 1; fi
