#!/bin/bash
# A player's RE8 on a Mali-G615 MC6 r44p1 (ledger 297, a player log): after a contained driver fault the server lists the
# other calls that were in flight. That list was made after the fault's jump, behind dladdr, /proc/self/maps and the call's
# description (on the phone 22-30 ms later), so the calls it named had started AFTER the fault and no log showed what ran AT it.
# The calls in the driver at the fault are now copied inside the signal handler (the markers every request and every guarded
# compile already set) and printed as "calls in flight at the fault:"; the late list stays, as "calls in flight after recovery:".
# Four programs on one server (tests/faultsnap_check.c), with the late list held VKBRIDGE_TEST_FAULT_LATE_MS=60 after the jump
# (the phone's 22-30 ms, with room):
#   C  hold       its vkCreateEvent is in the server (held 4 s: VKBRIDGE_TEST_DELAY_CMD) before and during the fault;
#   G  bgcompile  its marked pipeline compiles on the asynchronous compile's worker thread (VKBRIDGE_ASYNC_COMPILE=3, slow
#                 for 6 s: VKBRIDGE_TEST_CRASH_PIPELINE=slow): a background compile in the driver at the fault;
#   A  fault      its vkAllocateMemory faults (VKBRIDGE_TEST_CRASH_CMD=vkAllocateMemory, SEGV_ACCERR, once): contained;
#   B  late       sees the CONTAINED line, waits 8 ms, then starts its vkCreateEvent: a call that began after the fault.
#   1  the fault is contained, the server lives, A's allocation fails, C's and B's calls work;
#   2  "calls in flight at the fault:" names C's vkCreateEvent and G's background compile, and not B;
#   3  "calls in flight after recovery:" (the late list) names B: the old line's reading, which the at-fault line corrects;
#   4  the line comes after the fault line and before the late one, in the CONTAINED block of A's call.
# Prints one line per step and "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
work=$(mktemp -d)
srv=
sock=$work/s.sock
log=$work/server.log
trap '[ -n "$srv" ] && kill $srv 2>/dev/null; rm -f "$sock"; [ -n "$KEEP" ] && echo "kept $work" || rm -rf "${work:?}"' EXIT
fails=0 steps=0
step() { steps=$((steps+1)); if [ "$1" = 0 ]; then echo "  ok   $2"; else echo "  FAIL $2"; fails=$((fails+1)); fi; }
wait_for() { # <file> <pattern> <tenths>
  for _ in $(seq "$3"); do grep -q "$2" "$1" 2>/dev/null && return 0; sleep 0.1; done
  return 1
}
client() { VKBRIDGE_SOCKET=$sock VK_DRIVER_FILES=$out/vkbridge_icd.json VK_ICD_FILENAMES= timeout 60 "$out/faultsnap_check" "$@"; }

env -u VKBRIDGE_ERRATA -u VKBRIDGE_FAKE_DRIVER_VERSION -u VKBRIDGE_MODE VKBRIDGE_PROBE=0 \
    VKBRIDGE_TEST_CRASH_CMD=vkAllocateMemory VKBRIDGE_TEST_CRASH_ONCE=1 VKBRIDGE_TEST_CRASH_ACCERR=1 \
    VKBRIDGE_TEST_DELAY_CMD=vkCreateEvent VKBRIDGE_TEST_DELAY_MS=4000 VKBRIDGE_TEST_DELAY_REPEAT=1 \
    VKBRIDGE_ASYNC_COMPILE=3 VKBRIDGE_TEST_CRASH_PIPELINE=slow VKBRIDGE_TEST_SLOW_S=6 \
    VKBRIDGE_TEST_FAULT_LATE_MS=60 \
    VK_DRIVER_FILES=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json} VK_ICD_FILENAMES= \
    "$out/vkbridge-server" --socket "$sock" --log "$log" --cache-dir "$work/cache" >/dev/null 2>&1 &
srv=$!
for _ in $(seq 50); do [ -S "$sock" ] && break; sleep 0.1; done

client late "$log" 8 >"$work/b.out" 2>&1 & pb=$!
wait_for "$work/b.out" '^ready' 300
client bgcompile 8 >"$work/g.out" 2>&1 & pg=$!
wait_for "$log" 'test: compiling slowly on purpose' 300
client hold >"$work/c.out" 2>&1 & pc=$!
wait_for "$log" 'test: vkCreateEvent held' 300
client fault >"$work/a.out" 2>&1; ra=$?
wait $pb; rb=$?
wait $pc; rc=$?
wait $pg; rg=$?
A=$(sed -n 's/^pid //p' "$work/a.out"); B=$(sed -n 's/^pid //p' "$work/b.out"); C=$(sed -n 's/^pid //p' "$work/c.out")
G=$(sed -n 's/^pid //p' "$work/g.out")

kill -0 $srv 2>/dev/null && [ $ra = 0 ] && [ $rb = 0 ] && [ $rc = 0 ] &&
  grep -q "CONTAINED driver crash: signal 11 .* in vkAllocateMemory on device [0-9]* (faultsnap_check, pid $A)" "$log"
step $? "1 contained in A's vkAllocateMemory (pid $A), server lives: A $(tail -1 "$work/a.out"); C $(tail -1 "$work/c.out"); B $(tail -1 "$work/b.out"); G $(grep -m1 'pipeline create' "$work/g.out") (exit $rg)"

at=$(grep -m1 '  calls in flight at the fault: ' "$log" | sed 's/.*E   //')
echo "$at" | grep -q "faultsnap_check (pid $C, thread [0-9]*) vkCreateEvent [0-9]* ms" &&
  echo "$at" | grep -q "compile vkCreateGraphicsPipelines on device [0-9]* (thread [0-9]*, vkCreateGraphicsPipelines (in the background)) [0-9]* ms" &&
  ! echo "$at" | grep -q "pid $B,"
step $? "2 at the fault: C's held call and G's background compile, not B (pid $B): ${at:-no such line}"

late=$(grep -m1 '  calls in flight after recovery: \|  other calls in flight: ' "$log" | sed 's/.*E   //')
echo "$late" | grep -q "calls in flight after recovery: " && echo "$late" | grep -q "pid $B,"
step $? "3 the late list names B, which started after the fault: ${late:-no such line}"

blk=$(grep -A8 "CONTAINED driver crash: .* in vkAllocateMemory" "$log" | grep -o 'fault: code\|in flight at the fault\|in flight after recovery' | tr '\n' ',')
[ "$blk" = "fault: code,in flight at the fault,in flight after recovery," ]
step $? "4 order in the CONTAINED block: ${blk:-none}"

[ $fails = 0 ] && echo "OK: $steps steps" || echo "FAILED: $fails of $steps steps"
[ $fails = 0 ]
