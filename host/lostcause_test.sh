#!/bin/bash
# What the log says when a device is lost (ledger row 225; a player log: POCO X6 Pro, Mali-G615 r44p1, Resident
# Evil Village, the device lost 25-30 s into play; his server log had "device 17 LOST: vkWaitForFences returned
# VK_ERROR_DEVICE_LOST", the suspicion and the latch, and nothing on what the GPU was running). server_lostlog.c: at a
# device's first loss, once and off the game's threads, the log gets what its last submissions carried (command
# buffers, their commands, draws and dispatches, the pipelines bound, which ones a wait saw finish) and, where the
# driver offers VK_EXT_device_fault, the driver's own fault report, asked for under the crash guard with a time limit
# (a player log's r54p3 driver faulted inside its own fault recording). A line in the session report too. No setting:
# VKBRIDGE_LOST_REPORT=0 is the emergency off switch.
# Lavapipe has no VK_EXT_device_fault: VKBRIDGE_TEST_DEVICE_FAULT=<mode> imitates a driver that offers it
# (va: a fault report with an address and a vendor record; crash: the query faults inside the driver; hang: the query
# never comes back; refuse: vkCreateDevice refuses the extension once). The loss itself is VKBRIDGE_TEST_LOST's.
#   1  the player's fingerprint (a loss answered by fence waits, =3:wait, command buffers submitted): the record names the
#      submissions, the newest NOT seen finished, its command buffer and commands, and the driver's fault report
#      (address, vendor record); the session report has the cause line; the program ends as before;
#   2  a player log: the fault query faults inside the driver: contained (the server lives, says so), the record of the
#      submissions is still written, the session report still written; another program works after it;
#   3  the query never comes back: interrupted at its limit, the server lives, the program ends within its time;
#   4  a driver that refuses the extension at vkCreateDevice: the device is made without it, said once; the
#      record says no fault report;
#   5  a program that ends at its first lost answer (never latched): the record is written when its device goes,
#      no driver query;
#   6  the emergency off switch (VKBRIDGE_LOST_REPORT=0): no record, the extension not asked for;
#   7  a program that loses nothing: no record line;
#   8  a transient lost answer the driver takes back (=3:blip): no record (it was not lost);
#   9  bounded: 6 losses on one server, at most 4 records and one line saying no more.
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
  mkdir -p "$work/r-$name"
  env VKBRIDGE_PROBE=0 VKBRIDGE_REPORT_DIR="$work/r-$name" "$@" VK_DRIVER_FILES=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json} VK_ICD_FILENAMES= \
      "$out/vkbridge-server" --socket "$sock" --log "$work/$name.log" --cache-dir "$work/c-$name" >/dev/null 2>&1 &
  srv=$!
  for _ in $(seq 50); do [ -S "$sock" ] && break; sleep 0.1; done
}
client() { VKBRIDGE_SOCKET=$sock VK_DRIVER_FILES=$out/vkbridge_icd.json VK_ICD_FILENAMES= timeout 60 "$@"; }
stop() { kill $srv 2>/dev/null; wait $srv 2>/dev/null; srv=; }
# the report is written by its own thread: up to 3 s for a line
report_has() { for _ in $(seq 30); do grep -q "$2" "$work/r-$1/malibridge-report.md" 2>/dev/null && return 0; sleep 0.1; done; return 1; }
rec() { grep "loss record" "$work/$1.log" | sed 's/.*vkbridge-server . //'; }
show() { rec "$1" | head -${2:-3} | cut -c1-170 | tr '\n' '|'; }

# 1 the player's fingerprint
start player VKBRIDGE_TEST_LOST=3:wait VKBRIDGE_TEST_DEVICE_FAULT=va
client "$out/lost_check" 20 0 300 cb >"$work/player.out" 2>&1; r=$?
sleep 0.5
L=$work/player.log
[ $r = 0 ] && kill -0 $srv 2>/dev/null && grep -q "is lost (vkWaitForFences answered VK_ERROR_DEVICE_LOST again" "$L" &&
  grep -q "loss record, device [0-9]* (pid [0-9]*, lost_check): " "$L" &&
  grep -Eq "loss record, device [0-9]*:   submission [0-9]+ of [0-9]+, [0-9.]+ s before the loss: vkQueueSubmit, .*: finished" "$L" &&
  grep -Eq "loss record, device [0-9]*:   submission [0-9]+ of [0-9]+, [0-9.]+ s before the loss: vkQueueSubmit, .*: NOT seen finished" "$L" &&
  grep -Eq "loss record, device [0-9]*:     command buffer 0x[0-9a-f]+: 3 commands \([0-9]+ bytes\), 0 draws, 0 dispatches" "$L" &&
  grep -q "loss record, device [0-9]*: the driver's fault report (VK_EXT_device_fault, VK_SUCCESS): \"imitated: a GPU page fault\", 1 address(es), 1 vendor record(s)" "$L" &&
  grep -q "loss record, device [0-9]*:   fault address 0x7f1234560000 (read, precision 4096)" "$L" &&
  grep -q "loss record, device [0-9]*:   vendor: \"imitated: job timeout\" code 0x4 data 0x58" "$L" &&
  report_has player "loss record: .*fault address 0x7f1234560000"
step $? "1 the player's fingerprint: the record names what ran and the driver's report ($(tail -1 "$work/player.out")): $(show player 4)"
stop

# 2 a player log: the query faults in the driver
start fault2 VKBRIDGE_TEST_LOST=3:wait VKBRIDGE_TEST_DEVICE_FAULT=crash
client "$out/lost_check" 20 0 300 cb >"$work/fault2.out" 2>&1; r=$?
sleep 0.5
E=$work/fault2.log
[ $r = 0 ] && kill -0 $srv 2>/dev/null && ! grep -q "crashed with signal" "$E" &&
  grep -Eq "loss record, device [0-9]*:   submission [0-9]+ of [0-9]+, .*: NOT seen finished" "$E" &&
  grep -q "loss record, device [0-9]*: the driver's fault report faulted (signal 11) and was stopped there" "$E" &&
  report_has fault2 "loss record: .*the driver's fault report faulted"
step $? "2 a player log: the fault query faults in the driver, contained ($(tail -1 "$work/fault2.out")): $(rec fault2 | grep -m1 'faulted' | cut -c1-170)"
client "$out/vkbridge-smoke" >"$work/fault2.smoke" 2>&1
step $? "2b another program works after it ($(tail -1 "$work/fault2.smoke"))"
stop

# 3 the query never comes back
start hang VKBRIDGE_TEST_LOST=3:wait VKBRIDGE_TEST_DEVICE_FAULT=hang
t0=$(date +%s)
client "$out/lost_check" 20 0 300 cb >"$work/hang.out" 2>&1; r=$?
el=$(( $(date +%s) - t0 ))
sleep 0.3
[ $r = 0 ] && [ $el -le 15 ] && kill -0 $srv 2>/dev/null && ! grep -q "crashed with signal" "$work/hang.log" &&
  grep -q "loss record, device [0-9]*: the driver's fault report did not come back within 2 s and was interrupted" "$work/hang.log"
step $? "3 the fault query hangs: interrupted, the program ended in ${el} s ($(tail -1 "$work/hang.out")): $(rec hang | grep -m1 'interrupted' | cut -c1-150)"
client "$out/vkbridge-smoke" >"$work/hang.smoke" 2>&1
step $? "3b another program works after it ($(tail -1 "$work/hang.smoke"))"
stop

# 4 the driver refuses the extension at vkCreateDevice
start refuse VKBRIDGE_TEST_LOST=3:wait VKBRIDGE_TEST_DEVICE_FAULT=refuse
client "$out/lost_check" 20 0 300 cb >"$work/refuse.out" 2>&1; r=$?
sleep 0.3
[ $r = 0 ] && grep -q "vkCreateDevice refused VK_EXT_device_fault (-7): the device is made without it" "$work/refuse.log" &&
  grep -q "loss record, device [0-9]*: no fault report from the driver (VK_EXT_device_fault refused at vkCreateDevice)" "$work/refuse.log"
step $? "4 refused at vkCreateDevice: made without it ($(tail -1 "$work/refuse.out")): $(grep -m1 'refused VK_EXT_device_fault' "$work/refuse.log" | sed 's/.*vkbridge-server . //' | cut -c1-110)"
stop

# 5 a program that ends at its first lost answer
start quit VKBRIDGE_TEST_LOST=3:wait VKBRIDGE_TEST_DEVICE_FAULT=va
client "$out/lost_check" 20 0 0 exit >"$work/quit.out" 2>&1; r=$?
sleep 0.3
[ $r = 0 ] && ! grep -q " is lost (" "$work/quit.log" &&
  grep -q "loss record, device [0-9]* (pid [0-9]*, lost_check): said lost (vkWaitForFences) and not confirmed before the device went" "$work/quit.log" &&
  grep -Eq "loss record, device [0-9]*:   submission [0-9]+ of [0-9]+, .*: NOT seen finished" "$work/quit.log" &&
  ! grep -q "loss record, device [0-9]*: the driver's fault report" "$work/quit.log"
step $? "5 ended at its first lost answer: the record when the device goes, no driver query ($(tail -1 "$work/quit.out")): $(show quit 1)"
stop

# 6 the emergency off switch
start off VKBRIDGE_TEST_LOST=3:wait VKBRIDGE_TEST_DEVICE_FAULT=va VKBRIDGE_LOST_REPORT=0
client "$out/lost_check" 20 0 300 cb >"$work/off.out" 2>&1; r=$?
sleep 0.3
[ $r = 0 ] && grep -q "is lost (vkWaitForFences" "$work/off.log" && ! grep -q "loss record," "$work/off.log" &&
  grep -q "loss records off (VKBRIDGE_LOST_REPORT=0)" "$work/off.log"
step $? "6 VKBRIDGE_LOST_REPORT=0: no record ($(tail -1 "$work/off.out"))"
stop

# 7 a program that loses nothing
start plain VKBRIDGE_TEST_DEVICE_FAULT=va
client "$out/vkbridge-smoke" >"$work/plain.smoke" 2>&1; r=$?
sleep 0.3
[ $r = 0 ] && ! grep -q "loss record" "$work/plain.log"
step $? "7 nothing lost: no record ($(tail -1 "$work/plain.smoke"))"
stop

# 8 a transient lost answer the driver takes back
start blip VKBRIDGE_TEST_LOST=3:blip VKBRIDGE_TEST_DEVICE_FAULT=va
client "$out/lost_check" 20 5 300 transient >"$work/blip.out" 2>&1; r=$?
sleep 0.3
[ $r = 0 ] && grep -q "said lost, then finished work: not lost" "$work/blip.log" && ! grep -q "loss record" "$work/blip.log"
step $? "8 a transient lost answer: no record ($(tail -1 "$work/blip.out"))"
stop

# 9 bounded
start many VKBRIDGE_TEST_LOST=3:wait VKBRIDGE_TEST_DEVICE_FAULT=va
for i in 1 2 3 4 5 6; do client "$out/lost_check" 20 1 50 >"$work/many.$i.out" 2>&1; done
sleep 0.5
nrec=$(grep -c "loss record, device [0-9]* (pid" "$work/many.log")
[ "$nrec" = 4 ] && [ "$(grep -c "loss records: 4 written this run, no more" "$work/many.log")" = 1 ]
step $? "9 bounded: $nrec records for 6 losses, then one line: $(grep -m1 'loss records: ' "$work/many.log" | sed 's/.*vkbridge-server . //')"
stop

if [ $fails = 0 ]; then echo "OK: $steps steps"; else echo "FAILED: $fails of $steps steps"; fi
[ $fails = 0 ]
