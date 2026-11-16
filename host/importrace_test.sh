#!/bin/bash
# import_serial (MK-48, server_errata_import.c): a player's RE crashes (a player log, Mali-G615 r44p1):
# the server faulted at libGLES_mali.so+0xa177bc (SEGV_ACCERR) in a compositor's dma-buf import while another
# program's device was in its own memory calls. VKBRIDGE_TEST_CRASH_CMD=import_race imitates that race beneath
# every layer (server_crashctx.c: it faults an import that meets ANOTHER device's memory call). The fix is keyed to
# the driver of the 4 logs: r44p1 on a Mali-G615 MC6 (VKBRIDGE_FAKE_DRIVER_VERSION=r44p1@Mali-G615 MC6 gives the gates
# that GPU name). Programs on one server (importrace_check compositor + game, or one program with both on two threads):
#   1  the race imitated, fix off (VKBRIDGE_ERRATA=-import_serial on r44p1 G615 MC6): the import faults the way a player's
#      did - 'CONTAINED driver crash ... in vkAllocateMemory', SEGV_ACCERR - and the compositor's imports fail;
#   2  the race imitated, the fix from the gate (r44p1 G615 MC6): no fault, every import and allocation works, the
#      device's line says import_serial is on, the totals show imports run alone and no call went on unlocked;
#   3  off where it is not needed: r49p1 and the desktop driver (not Arm) leave it off (the imitation faults
#      on r49p1 as in 1); without the imitation, the fix on r44p1 G615 MC6 runs both programs right;
#   4  inactive on every other r44p1 GPU (the host's own name, the S10+'s Mali-G720-Immortalis MC12, a Mali-G615 MC2):
#      not a line of import_serial in the log and the imitation faults as without the fix (the old path, calls not
#      serialized); VKBRIDGE_ERRATA=+import_serial turns it on there (the diagnosis switch);
#   5  one device importing on one thread while it allocates on another (as re8's own export imports beside its
#      loads), fix on: not one call waited for the lock (only another device's calls are kept apart).
# Prints one line per step and "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
work=$(mktemp -d)
srv=
sock=$work/s.sock
trap '[ -n "$srv" ] && kill $srv 2>/dev/null; rm -f "$sock"; [ -n "$KEEP" ] && echo "kept $work" || rm -rf "$work"' EXIT
fails=0 steps=0
step() { steps=$((steps+1)); if [ "$1" = 0 ]; then echo "  ok   $2"; else echo "  FAIL $2"; fails=$((fails+1)); fi; }
start() { # <name> <env...>
  local name=$1; shift
  [ -n "$srv" ] && { kill $srv 2>/dev/null; wait $srv 2>/dev/null; }
  rm -f "$sock"; log=$work/$name.log
  env -u VKBRIDGE_ERRATA -u VKBRIDGE_FAKE_DRIVER_VERSION -u VKBRIDGE_MODE -u VKBRIDGE_FAKE_MISSING VKBRIDGE_PROBE=0 "$@" \
      VK_DRIVER_FILES=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json} VK_ICD_FILENAMES= \
      "$out/vkbridge-server" --socket "$sock" --log "$log" --cache-dir "$work/c-$name" >/dev/null 2>&1 &
  srv=$!
  for _ in $(seq 50); do [ -S "$sock" ] && break; sleep 0.1; done
}
both() { # the compositor and the game at once: $crc $grc $cres $gres
  VKBRIDGE_SOCKET=$sock VK_DRIVER_FILES=$out/vkbridge_icd.json VK_ICD_FILENAMES= timeout 120 "$out/importrace_check" game ${GAME_N:-60} >$work/g.out 2>&1 &
  local g=$!
  VKBRIDGE_SOCKET=$sock VK_DRIVER_FILES=$out/vkbridge_icd.json VK_ICD_FILENAMES= timeout 120 "$out/importrace_check" compositor ${COMP_N:-20} >$work/c.out 2>&1
  crc=$?; wait $g; grc=$?
  cres=$(tail -1 $work/c.out); gres=$(tail -1 $work/g.out)
}
contained() { grep -c 'CONTAINED driver crash: signal 11 .* in vkAllocateMemory' "$log"; }
on_line() { grep -o 'errata: device .*: on [^;]*' "$log" | tail -1; grep -o 'errata: import_serial ON[^-]*' "$log" | head -1; }

# 1
start race-off VKBRIDGE_TEST_CRASH_CMD=import_race "VKBRIDGE_FAKE_DRIVER_VERSION=r44p1@Mali-G615 MC6" VKBRIDGE_ERRATA=-import_serial
both
n=$(contained)
kill -0 $srv 2>/dev/null && [ "$n" -ge 1 ] && grep -q 'fault: code 2 (SEGV_ACCERR)' "$log" && [ $crc != 0 ]
step $? "1 race imitated, fix off: $n contained fault(s) in vkAllocateMemory ($(grep -m1 -o 'fault: code 2 (SEGV_ACCERR)' "$log")); compositor: $cres; game: $gres"

# 2
start race-on VKBRIDGE_TEST_CRASH_CMD=import_race "VKBRIDGE_FAKE_DRIVER_VERSION=r44p1@Mali-G615 MC6"
both
n=$(contained)
kill -0 $srv 2>/dev/null && [ "$n" = 0 ] && [ $crc = 0 ] && [ $grc = 0 ] && grep -q 'errata: import_serial ON (r44p1 in r44p1-r44p1, G615 MC6 ' "$log" &&
  grep -q 'errata: import_serial: device [0-9]*: totals in the process so far: [1-9][0-9]* call(s) alone' "$log" &&
  ! grep -q 'goes on without the lock' "$log"
step $? "2 race imitated, fix from the gate (r44p1 G615 MC6): $n faults; compositor: $cres; game: $gres; $(grep -o 'totals in the process so far: [0-9]* call(s) alone, [0-9]* shared' "$log" | tail -1)"

# 3
start r49 VKBRIDGE_TEST_CRASH_CMD=import_race VKBRIDGE_FAKE_DRIVER_VERSION=r49p1
both
n49=$(contained); off49=$(grep -c 'import_serial ON' "$log")
start desk
both
offd=$(grep -c 'import_serial ON\|import_serial:' "$log"); d1=$crc d2=$grc dres="$cres / $gres"
start plain "VKBRIDGE_FAKE_DRIVER_VERSION=r44p1@Mali-G615 MC6"
both
[ "$n49" -ge 1 ] && [ "$off49" = 0 ] && [ "$offd" = 0 ] && [ $d1 = 0 ] && [ $d2 = 0 ] && [ $crc = 0 ] && [ $grc = 0 ] && [ "$(contained)" = 0 ]
step $? "3 off on r49p1 ($n49 imitated fault(s), as without the fix) and on the desktop driver ($dres); r44p1 G615 MC6 without the imitation: $cres / $gres"

# 4
r4=0 s4= k=0
for v in r44p1 "r44p1@Mali-G720-Immortalis MC12" "r44p1@Mali-G615 MC2"; do
  k=$((k+1))
  start other$k VKBRIDGE_TEST_CRASH_CMD=import_race "VKBRIDGE_FAKE_DRIVER_VERSION=$v"
  both
  # the decision summary lists it under "off"; any line of its own (ON, install, totals) means it is in
  n=$(contained); m=$(grep -c 'import_serial ON\|errata: import_serial:' "$log")
  { [ "$n" -ge 1 ] && [ "$m" = 0 ]; } || r4=1
  s4="$s4 ${v#r44p1@}: $n imitated fault(s), $m import_serial line(s);"
done
start forced VKBRIDGE_TEST_CRASH_CMD=import_race "VKBRIDGE_FAKE_DRIVER_VERSION=r44p1@Mali-G720-Immortalis MC12" VKBRIDGE_ERRATA=+import_serial
both
nf=$(contained)
{ [ "$nf" = 0 ] && grep -q 'errata: import_serial ON (VKBRIDGE_ERRATA=+import_serial)' "$log"; } || r4=1
step $r4 "4 inactive on the other r44p1 GPUs (the old path):$s4 forced on the G720 with +import_serial: $nf fault(s)"

# 5
start self VKBRIDGE_TEST_CRASH_CMD=import_race "VKBRIDGE_FAKE_DRIVER_VERSION=r44p1@Mali-G615 MC6"
VKBRIDGE_SOCKET=$sock VK_DRIVER_FILES=$out/vkbridge_icd.json VK_ICD_FILENAMES= timeout 120 "$out/importrace_check" both ${SELF_N:-20} >$work/b.out 2>&1
brc=$?
for _ in $(seq 50); do grep -q 'totals in the process so far' "$log" && break; sleep 0.1; done
tot=$(grep -o 'totals in the process so far: .*' "$log" | tail -1)
w=$(echo "$tot" | grep -o '[0-9]* waited' | grep -o '[0-9]*')
[ $brc = 0 ] && [ "$(contained)" = 0 ] && [ -n "$w" ] && [ "$w" = 0 ] && echo "$tot" | grep -q '[1-9][0-9]* call(s) alone'
step $? "5 one device importing beside its own allocations (two threads), fix on: $(tr '\n' ' ' < $work/b.out); ${tot:-no totals line}"

[ $fails = 0 ] && echo "OK: $steps steps" || echo "FAILED: $fails of $steps steps"
[ $fails = 0 ]
