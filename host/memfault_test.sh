#!/bin/bash
#, Mali-G615 MC6 r44p1: the one server died in the driver's
# vkBindImageMemory and, the next time, in its vkAllocateMemory (same driver pc, SEGV_ACCERR), and
# took Steam, the compositor and the game with it. The cause inside the driver is not known; a
# fault in those calls is now contained (server_guard.c): the allocation fails with
# VK_ERROR_OUT_OF_DEVICE_MEMORY, a bind takes its device as lost, the other programs go on.
# VKBRIDGE_TEST_CRASH_CMD=<call> VKBRIDGE_TEST_CRASH_ONCE=1 VKBRIDGE_TEST_CRASH_ACCERR=1: a client's
# first such call faults at the driver boundary the way did (code 2, a 2-byte store at ..e5e
# into a mapping without access).
#   1  allocation: the server lives and says it contained the fault in vkAllocateMemory;
#   2  the program then goes on and a second program works;
#   3  bind: the server lives, the program's device is taken as lost (its waits answer -4);
#   4  a second program works on the same server;
#   5  A/B: with VKBRIDGE_NO_CRASH_GUARD=1 the same fault kills the server (fingerprint);
#   6  a fault with a driver lock held (VKBRIDGE_TEST_CRASH_LOCK=1: the lock is left held, the program's
#      next allocation waits on it for good): the stall watchdog (VKBRIDGE_MEM_STALL_S=3 here, 30 by
#      default) ends that program's session, it ends in seconds, the server lives;
#   7  a second program then waits on the same lock: the server ends (as the fault would have ended
#      it), with the watchdog's line in its log and crash.log; the program ends instead of freezing;
#   8  A/B: with VKBRIDGE_MEM_STALL_S=0 the first program freezes (still in its allocation after 12 s).
#  10  (a player log/5PA6 on t12+, RE-CRASH 11-05) a contained fault says what the uncontained crash line said: code 2
#      (SEGV_ACCERR), pc as library + offset, the faulting thread and what the address lies in, the call's
#      arguments (allocation size/type/path; bind image + memory) and the other calls in flight; the CONTAINED
#      line alone (t12-t15) could not tell next RE log's hypotheses apart.
#   9  test11 audit P2 (ledger 167): after the watchdog of 6 stops a program, its driver event thread faults
#      (VKBRIDGE_TEST_LOST=1:stop) and is parked; the driver needed that thread (the imitation: every fence
#      wait then hangs in the driver; the held lock is the faulting device's own, VKBRIDGE_TEST_CRASH_LOCK=dev,
#      so the next program's allocations pass). The parked thread's release (VKBRIDGE_TEST_PARK_STALL_S=3 here, 30 s by
#      default) still starts: the next program's wait hangs for seconds, then the thread dies of its fault as
#      before (the server ends, its crash line names the thread) - not a freeze of every program for good.
# Prints one line per step and "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
work=$(mktemp -d)
srv=
trap '[ -n "$srv" ] && kill $srv 2>/dev/null; rm -f "$work"/*.sock; [ -n "$KEEP" ] && echo "kept $work" || rm -rf "$work"' EXIT
fails=0 steps=0
step() { steps=$((steps+1)); if [ "$1" = 0 ]; then echo "  ok   $2"; else echo "  FAIL $2"; fails=$((fails+1)); fi; }

start() {
  local name=$1; shift
  sock=$work/$name.sock
  env VKBRIDGE_PROBE=0 VKBRIDGE_TEST_CRASH_ONCE=1 VKBRIDGE_TEST_CRASH_ACCERR=1 "$@" \
      VK_DRIVER_FILES=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json} VK_ICD_FILENAMES= \
      "$out/vkbridge-server" --socket "$sock" --log "$work/$name.log" --cache-dir "$work/c-$name" >/dev/null 2>&1 &
  srv=$!
  for _ in $(seq 50); do [ -S "$sock" ] && break; sleep 0.1; done
}
smoke() { VKBRIDGE_SOCKET=$sock VK_DRIVER_FILES=$out/vkbridge_icd.json VK_ICD_FILENAMES= timeout ${T:-60} "$out/vkbridge-smoke"; }
stop() { kill $srv 2>/dev/null; wait $srv 2>/dev/null; srv=; }

start alloc VKBRIDGE_TEST_CRASH_CMD=vkAllocateMemory
smoke >"$work/alloc1.out" 2>&1
sleep 0.2
kill -0 $srv 2>/dev/null && ! grep -q "crashed with signal" "$work/alloc.log" &&
  grep -q "CONTAINED driver crash: signal 11 (address 0x[0-9a-f]*e5e) in vkAllocateMemory on device [0-9]* (vkbridge-smoke, pid [0-9]*) - the allocation fails" "$work/alloc.log"
step $? "1 allocation: the server lives: $(grep -m1 'CONTAINED' "$work/alloc.log" | sed 's/.*vkbridge-server E //' | cut -c1-170)"
smoke >"$work/alloc2.out" 2>&1
step $? "2 the next program works ($(tail -1 "$work/alloc2.out")); the faulting one: $(tail -1 "$work/alloc1.out")"
stop
cp "$work/alloc.log" "$work/alloc-kept.log"

start bind VKBRIDGE_TEST_CRASH_CMD=vkBindImageMemory
smoke >"$work/bind1.out" 2>&1
sleep 0.2
kill -0 $srv 2>/dev/null && ! grep -q "crashed with signal" "$work/bind.log" &&
  grep -q "CONTAINED driver crash: signal 11 .* in vkBindImageMemory on device [0-9]* (vkbridge-smoke, pid [0-9]*) - the device is taken as lost" "$work/bind.log" &&
  grep -q "is lost (a driver fault in a memory bind was contained)" "$work/bind.log"
step $? "3 bind: the server lives, the device is taken as lost: $(grep -m1 ' is lost ' "$work/bind.log" | sed 's/.*vkbridge-server E //' | cut -c1-120)"
smoke >"$work/bind2.out" 2>&1
step $? "4 the next program works ($(tail -1 "$work/bind2.out"))"
stop
a_pc=$(grep -m1 -A1 'CONTAINED driver crash: .* in vkAllocateMemory' "$work/alloc-kept.log" | grep -m1 '  fault: ')
a_where=$(grep -m1 -A3 'CONTAINED driver crash: .* in vkAllocateMemory' "$work/alloc-kept.log" | grep -m1 '  thread [0-9]* "')
a_args=$(grep -m1 -A4 'CONTAINED driver crash: .* in vkAllocateMemory' "$work/alloc-kept.log" | grep -m1 '  allocation of ')
a_oth=$(grep -m1 -A5 'CONTAINED driver crash: .* in vkAllocateMemory' "$work/alloc-kept.log" | grep -m1 '  other calls in flight: ')
b_args=$(grep -m1 -A4 'CONTAINED driver crash: .* in vkBindImageMemory' "$work/bind.log" | grep -m1 '  bind of image 0x')
echo "$a_pc" | grep -Eq 'fault: code 2 \(SEGV_ACCERR\), pc 0x[0-9a-f]+ \(vkbridge-server\+0x[0-9a-f]+\)' &&
  echo "$a_where" | grep -Eq 'address 0x[0-9a-f]+e5e is in [0-9a-f]+-[0-9a-f]+ ---p \(anonymous\)' &&
  echo "$a_args" | grep -Eq 'allocation of [1-9][0-9]* bytes in memory type [0-9]+, ' && [ -n "$a_oth" ] &&
  echo "$b_args" | grep -q 'to memory 0x'
step $? "10 the contained fault's detail: ${a_pc#*E   } | ${a_where#*E   } | ${a_args#*E   } | ${a_oth#*E   } | ${b_args#*E   }"

start noguard VKBRIDGE_TEST_CRASH_CMD=vkBindImageMemory VKBRIDGE_NO_CRASH_GUARD=1
{ smoke >"$work/noguard.out" 2>&1; } 2>/dev/null
for _ in $(seq 30); do kill -0 $srv 2>/dev/null || break; sleep 0.1; done
line=$(grep -m1 "crashed with signal" "$work/noguard.log")
! kill -0 $srv 2>/dev/null && echo "$line" | grep -q "crashed with signal 11 (code 2, address 0x[0-9a-f]*e5e) in vkBindImageMemory"
step $? "5 A/B: without containment the fault kills the server: ${line#*E }"
srv=

start lock VKBRIDGE_TEST_CRASH_CMD=vkAllocateMemory VKBRIDGE_TEST_CRASH_LOCK=1 VKBRIDGE_MEM_STALL_S=3
t0=$(date +%s)
T=30 smoke >"$work/lock1.out" 2>&1; r=$?
took=$(( $(date +%s) - t0 ))
sleep 0.2
[ $r != 124 ] && [ $took -le 20 ] && kill -0 $srv 2>/dev/null &&
  grep -q "memory fault watchdog: vkbridge-smoke (pid [0-9]*) has been in vkAllocateMemory for [0-9]* s, after a driver fault in vkAllocateMemory on device [0-9]* was contained" "$work/lock.log" &&
  grep -q "Its session ends" "$work/lock.log"
step $? "6 held lock: the stuck program's session ends in ${took} s (exit $r), the server lives: $(grep -m1 'memory fault watchdog: vkb' "$work/lock.log" | sed 's/.*vkbridge-server E //' | cut -c1-150)"
t0=$(date +%s)
{ T=30 smoke >"$work/lock2.out" 2>&1; r=$?; } 2>/dev/null
took=$(( $(date +%s) - t0 ))
for _ in $(seq 30); do kill -0 $srv 2>/dev/null || break; sleep 0.1; done
! kill -0 $srv 2>/dev/null && [ $r != 124 ] && [ $took -le 20 ] &&
  grep -q "memory fault watchdog: .* the server ends now" "$work/lock.log" && grep -q "crashed with signal 6" "$work/lock.log"
step $? "7 a second program on the held lock: the server ends (program ended in ${took} s, exit $r): $(grep -m1 'the server ends now' "$work/lock.log" | sed 's/.*vkbridge-server E //' | cut -c1-130)"
srv=

start nowatch VKBRIDGE_TEST_CRASH_CMD=vkAllocateMemory VKBRIDGE_TEST_CRASH_LOCK=1 VKBRIDGE_MEM_STALL_S=0
T=12 smoke >"$work/nowatch.out" 2>&1; r=$?
[ $r = 124 ] && grep -q "memory fault watchdog: off" "$work/nowatch.log"
step $? "8 A/B: without the watchdog the program freezes in its allocation (timeout after 12 s: exit $r)"
kill -9 $srv 2>/dev/null; wait $srv 2>/dev/null; srv=

start stopevt VKBRIDGE_TEST_CRASH_CMD=vkAllocateMemory VKBRIDGE_TEST_CRASH_LOCK=dev VKBRIDGE_MEM_STALL_S=3 VKBRIDGE_TEST_LOST=1:stop VKBRIDGE_TEST_PARK_STALL_S=3
T=30 smoke >"$work/stopevt1.out" 2>&1
for _ in $(seq 30); do grep -q 'driver thread [0-9]* "mali-event-hand" faulted' "$work/stopevt.log" && break; sleep 0.1; done
t0=$(date +%s)
{ VKBRIDGE_SOCKET=$sock VK_DRIVER_FILES=$out/vkbridge_icd.json VK_ICD_FILENAMES= timeout 40 "$out/lost_check" >"$work/stopevt2.out" 2>&1; r=$?; } 2>/dev/null
took=$(( $(date +%s) - t0 ))
for _ in $(seq 50); do kill -0 $srv 2>/dev/null || break; sleep 0.1; done
! kill -0 $srv 2>/dev/null && [ $r != 124 ] && [ $took -le 25 ] &&
  grep -q 'driver thread [0-9]* "mali-event-hand" faulted' "$work/stopevt.log" &&
  grep -q "none came back for 3 s after a driver thread was stopped" "$work/stopevt.log" &&
  grep -q '  thread [0-9]* "mali-event-hand"' "$work/stopevt.log"
step $? "9 a driver thread faults after the watchdog's stop and is parked; the next program's hung wait is released in ${took} s (exit $r): $(grep -m1 'none came back' "$work/stopevt.log" | sed 's/.*vkbridge-server E //' | cut -c1-110)"
kill -9 $srv 2>/dev/null; wait $srv 2>/dev/null; srv=

if [ $fails = 0 ]; then echo "OK: $steps steps"; else echo "FAILED: $fails of $steps steps"; [ -n "$KEEP" ] || for f in "$work"/*.log; do echo "--- $f"; grep -E " E | W " "$f" | tail -5; done; fi
[ $fails = 0 ]
