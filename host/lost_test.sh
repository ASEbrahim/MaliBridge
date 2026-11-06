#!/bin/bash
# Pixel 9, Mali-G715 r54p3, WoW Classic: the game's device was lost, the game submitted four
# more times, and the driver's own thread (mali-event-hand, in its device-fault recording) faulted;
# the one server died with every program. server_lost.c: a lost device's submissions and waits
# answer VK_ERROR_DEVICE_LOST without reaching the driver (the latch), and a "mali-*" driver thread
# that faults while a device is lost is stopped instead of taking the server down (the park).
# VKBRIDGE_TEST_LOST=3:after imitates E6ZA (lost_check's 3rd submission loses its device; every
# submission that reaches the lost device after it makes a thread named mali-event-hand fault
# inside the driver); =3:loss makes that thread fault at the loss itself (the latch cannot help).
#   1  latch: lost_check sees the loss and -4 after it, nothing faults, the server lives;
#   2  another program (vkbridge-smoke) then works on the same server;
#   3  A/B: with VKBRIDGE_LOST_LATCH=0 the imitated driver thread takes the server down, and the
#      crash line names the thread (mali-event-hand) - E6ZA's fingerprint;
#   4  park: with the fault at the loss the thread is stopped, the server lives and says so;
#      lost_check still ends, its device and instance are left as they are at its exit;
#   5  another program then works on the same server;
#   6  A/B: with VKBRIDGE_LOST_PARK=0 that fault takes the server down;
#   7  a program that loses nothing: no "is lost" line, the smoke passes;
#   8  transient (VKBRIDGE_TEST_LOST=3:blip, the A1 family): a fence wait answers -4 and so does the
#      poll right after, then the device works: not latched, the next rounds all succeed (with the
#      old one-answer latch every call after it answered -4 for good);
#   9  a loss seen only through waits (=3:wait): the first lost answer is a suspicion, the next wait
#      600 ms later confirms it and the device is latched (its device wait answers -4 without the driver);
#  10  E6ZA's own order (=3:wait:evt): the loss is first answered by a fence wait (a suspicion only since
#      cf541d5), 51 ms later the game submits 4 times and every submission reaching the lost device makes
#      the imitated mali-event-hand thread fault: the first submission's -4 latches the device, the other
#      3 never reach the driver (one imitated fault), the faulting thread is stopped, the server lives;
#  11  another program then works on the same server;
#  12  A/B: with VKBRIDGE_LOST_PARK=0 the same order takes the server down with E6ZA's fingerprint
#      (signal 11 at 0x101 in (no Vulkan call), thread "mali-event-hand").
#  13  a suspicion nothing confirmed expires (=3:blip:again, test11 audit P3, ledger 166): a fence wait and its
#      poll answer -4, nothing reaches the driver for 2.5 s, then the next fence wait and its poll answer -4
#      again: a new suspicion, not a confirmation (before, any later repeat latched the device for good: the A1
#      black screen back on job-manager drivers); the rounds after it succeed;
#  14  a loss answered by waits still latches when the repeat comes within the window (=3:wait, ~1.4 s apart).
# Prints one line per step and "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
work=$(mktemp -d)
srv=
trap '[ -n "$srv" ] && kill $srv 2>/dev/null; rm -f "$work"/*.sock; [ -n "$KEEP" ] && echo "kept $work" || rm -rf "$work"' EXIT
fails=0 steps=0
step() { steps=$((steps+1)); if [ "$1" = 0 ]; then echo "  ok   $2"; else echo "  FAIL $2"; fails=$((fails+1)); fi; }

# start <name> VAR=value...
start() {
  local name=$1; shift
  sock=$work/$name.sock
  env VKBRIDGE_PROBE=0 "$@" VK_DRIVER_FILES=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json} VK_ICD_FILENAMES= \
      "$out/vkbridge-server" --socket "$sock" --log "$work/$name.log" --cache-dir "$work/c-$name" >/dev/null 2>&1 &
  srv=$!
  for _ in $(seq 50); do [ -S "$sock" ] && break; sleep 0.1; done
}
client() { VKBRIDGE_SOCKET=$sock VK_DRIVER_FILES=$out/vkbridge_icd.json VK_ICD_FILENAMES= timeout 60 "$@"; }
stop() { kill $srv 2>/dev/null; wait $srv 2>/dev/null; srv=; }
gone() { for _ in $(seq 30); do kill -0 $srv 2>/dev/null || return 0; sleep 0.1; done; return 1; }

start latch VKBRIDGE_TEST_LOST=3:after
client "$out/lost_check" >"$work/latch.out" 2>&1; r=$?
sleep 0.3
[ $r = 0 ] && kill -0 $srv 2>/dev/null && ! grep -q "crashed with signal" "$work/latch.log" &&
  ! grep -q "event thread faults" "$work/latch.log" && grep -q "is lost (vkQueueSubmit returned VK_ERROR_DEVICE_LOST)" "$work/latch.log"
step $? "1 latch: lost_check ($(tail -1 "$work/latch.out")); server alive, nothing faulted: $(grep -m1 ' is lost ' "$work/latch.log" | sed 's/.*vkbridge-server E //' | cut -c1-110)"
client "$out/vkbridge-smoke" >"$work/latch.smoke" 2>&1
step $? "2 another program works after it ($(tail -1 "$work/latch.smoke"))"
stop

start nolatch VKBRIDGE_TEST_LOST=3:after VKBRIDGE_LOST_LATCH=0
{ client "$out/lost_check" >"$work/nolatch.out" 2>&1; } 2>/dev/null
gone; dead=$?
line=$(grep -m1 "crashed with signal" "$work/nolatch.log")
[ $dead = 0 ] && echo "$line" | grep -q "in (no Vulkan call)" && grep -q '  thread [0-9]* "mali-event-hand"' "$work/nolatch.log"
step $? "3 A/B: without the latch the imitated driver thread kills the server: ${line#*E } | $(grep -m1 '  thread ' "$work/nolatch.log" | sed 's/.*E   //' | cut -c1-60)"
srv=

start park VKBRIDGE_TEST_LOST=3:loss
client "$out/lost_check" >"$work/park.out" 2>&1; r=$?
sleep 0.5
kill -0 $srv 2>/dev/null && ! grep -q "crashed with signal" "$work/park.log" &&
  grep -q 'driver thread [0-9]* "mali-event-hand" faulted .* after device [0-9]* (pid [0-9]*) was lost: the thread is stopped' "$work/park.log" &&
  grep -Eq "exited without destroying (device|instance) [0-9]+, which is left as it is|instance [0-9]+ is left as it is" "$work/park.log"
step $? "4 park: the server lives (lost_check: $(tail -1 "$work/park.out")): $(grep -m1 'driver thread' "$work/park.log" | sed 's/.*vkbridge-server E //' | cut -c1-150)"
client "$out/vkbridge-smoke" >"$work/park.smoke" 2>&1
step $? "5 another program works after it ($(tail -1 "$work/park.smoke"))"
stop

start nopark VKBRIDGE_TEST_LOST=3:loss VKBRIDGE_LOST_PARK=0
{ client "$out/lost_check" >"$work/nopark.out" 2>&1; } 2>/dev/null
gone; dead=$?
[ $dead = 0 ] && grep -q '  thread [0-9]* "mali-event-hand"' "$work/nopark.log"
step $? "6 A/B: with VKBRIDGE_LOST_PARK=0 the fault kills the server: $(grep -m1 'crashed with' "$work/nopark.log" | sed 's/.*E //')"
srv=

start plain
client "$out/vkbridge-smoke" >"$work/plain.smoke" 2>&1; r=$?
[ $r = 0 ] && ! grep -q " is lost " "$work/plain.log"
step $? "7 a program that loses nothing: no latch, smoke passes ($(tail -1 "$work/plain.smoke"))"
stop

start blip VKBRIDGE_TEST_LOST=3:blip
client "$out/lost_check" 20 5 300 transient >"$work/blip.out" 2>&1; r=$?
[ $r = 0 ] && kill -0 $srv 2>/dev/null && ! grep -q " is lost (" "$work/blip.log" &&
  grep -q "not taken as lost until the driver says so again" "$work/blip.log" && grep -q "said lost, then finished work: not lost" "$work/blip.log"
step $? "8 transient lost answer: not latched ($(tail -1 "$work/blip.out")): $(grep -m1 'said lost' "$work/blip.log" | sed 's/.*vkbridge-server . //' | cut -c1-90)"
stop

start waitloss VKBRIDGE_TEST_LOST=3:wait
client "$out/lost_check" 20 0 300 >"$work/waitloss.out" 2>&1; r=$?
[ $r = 0 ] && kill -0 $srv 2>/dev/null && grep -q "is lost (vkWaitForFences answered VK_ERROR_DEVICE_LOST again [0-9]* ms after" "$work/waitloss.log"
step $? "9 a loss seen through waits is confirmed by the next ($(tail -1 "$work/waitloss.out")): $(grep -m1 ' is lost ' "$work/waitloss.log" | sed 's/.*vkbridge-server E //' | cut -c1-120)"
stop

start evt VKBRIDGE_TEST_LOST=3:wait:evt
client "$out/lost_check" 20 4 51 >"$work/evt.out" 2>&1; r=$?
sleep 0.5
nf=$(grep -c "the imitated driver's event thread faults" "$work/evt.log")
[ $r = 0 ] && kill -0 $srv 2>/dev/null && ! grep -q "crashed with signal" "$work/evt.log" &&
  grep -q "vkWaitForFences answered VK_ERROR_DEVICE_LOST, and so did a poll right after: not taken as lost" "$work/evt.log" &&
  grep -q "is lost (vkQueueSubmit returned VK_ERROR_DEVICE_LOST)" "$work/evt.log" && [ "$nf" = 1 ] &&
  grep -q 'driver thread [0-9]* "mali-event-hand" faulted .* after device [0-9]* (pid [0-9]*) was lost: the thread is stopped' "$work/evt.log"
step $? "10 E6ZA order (wait first, then 4 submissions): latched at the first submission, $nf imitated fault(s), thread stopped, server alive (lost_check: $(tail -1 "$work/evt.out")): $(grep -m1 ' is lost ' "$work/evt.log" | sed 's/.*vkbridge-server E //' | cut -c1-90)"
client "$out/vkbridge-smoke" >"$work/evt.smoke" 2>&1
step $? "11 another program works after it ($(tail -1 "$work/evt.smoke"))"
stop

start evtnopark VKBRIDGE_TEST_LOST=3:wait:evt VKBRIDGE_LOST_PARK=0
{ client "$out/lost_check" 20 4 51 >"$work/evtnopark.out" 2>&1; } 2>/dev/null
gone; dead=$?
line=$(grep -m1 "crashed with signal" "$work/evtnopark.log")
[ $dead = 0 ] && echo "$line" | grep -q "crashed with signal 11 (code 1, address 0x101) in (no Vulkan call)" && grep -q '  thread [0-9]* "mali-event-hand"' "$work/evtnopark.log"
step $? "12 A/B: with VKBRIDGE_LOST_PARK=0 the E6ZA order kills the server: ${line#*E }"
srv=

start again VKBRIDGE_TEST_LOST=3:blip:again
client "$out/lost_check" 20 5 2500 again >"$work/again.out" 2>&1; r=$?
[ $r = 0 ] && kill -0 $srv 2>/dev/null && ! grep -q " is lost (" "$work/again.log" &&
  [ "$(grep -c "fence wait [34] answers VK_ERROR_DEVICE_LOST, its poll too" "$work/again.log")" = 2 ] &&
  grep -q "a new suspicion, not taken as lost" "$work/again.log" && grep -q "said lost, then finished work: not lost" "$work/again.log"
step $? "13 two transients 2.5 s apart, nothing finished between: not latched ($(tail -1 "$work/again.out")): $(grep -m1 'new suspicion' "$work/again.log" | sed 's/.*vkbridge-server . //' | cut -c1-110)"
stop

start waitslow VKBRIDGE_TEST_LOST=3:wait
client "$out/lost_check" 20 0 700 >"$work/waitslow.out" 2>&1; r=$?
[ $r = 0 ] && kill -0 $srv 2>/dev/null && grep -q "is lost (vkWaitForFences answered VK_ERROR_DEVICE_LOST again 1[0-9][0-9][0-9] ms after" "$work/waitslow.log"
step $? "14 a loss whose waits repeat ~1.4 s apart is still latched ($(tail -1 "$work/waitslow.out")): $(grep -m1 ' is lost ' "$work/waitslow.log" | sed 's/.*vkbridge-server E //' | cut -c1-120)"
stop

if [ $fails = 0 ]; then echo "OK: $steps steps"; else echo "FAILED: $fails of $steps steps"; [ -n "$KEEP" ] || for f in "$work"/*.log; do echo "--- $f"; grep -E " E |W " "$f" | tail -6; done; fi
[ $fails = 0 ]
