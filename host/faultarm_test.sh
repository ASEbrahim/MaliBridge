#!/bin/bash
# The hang detector armed by a GPU fault, for that game's next launch only (God of War on a player's POCO X6 Pro, Mali-G615
# MC6 r44p1, a player log 2026-11-09-08: GoW.exe's device lost 23 s in, the driver's fault report 'TRANSLATION_FAULT at level 1
# ... Access type 0x3: Write'; the loss record names the submissions but not the writer). The hang detector
# (server_hang.c) names it: a fence per submission (the exact one), the pipelines it ran (the bridge's own BC dispatches
# too), every 64-bit push-constant value checked against the buffers handed out (DXVK 3 passes UAV counters as raw
# addresses), the fault address looked up in live and destroyed buffers. It is off by default (it costs a fence and an
# empty submission per submit) and a player cannot switch it on. So: a confirmed loss whose driver fault report has a
# memory address writes <cache dir>/hang-detect-next/<program@key>; the next launch of that program (same name and
# key) has the detector on for its own devices, once (the file is taken at its first device). VKBRIDGE_FAULT_ARM=0:
# never armed.
#   1  a loss with a fault address arms it: the line, the file (named after program@key);
#   2  the next launch: another program first (detector off for it, the file stays), then the armed program: detector
#      on for its devices, one line saying why; its loss is named by the detector (HANG lines, the fault address
#      looked up from the loss record's guarded driver query, the detector asks the driver no second time); the file
#      is gone (taken) and the detector's own loss does not arm it again;
#   3  the launch after a launch that took it (no loss): detector off;
#   4  a loss with no driver fault report: not armed;
#   5  VKBRIDGE_FAULT_ARM=0: a loss with a fault address does not arm it;
#   7  the default (no VKBRIDGE_FAULT_ARM, test59): nothing armed, an older file not taken; steps 1-4 and 6 opt in with =1;
#   6  the same program with another key (another game): not armed for it.
# Prints one line per step and "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
work=$(mktemp -d)
srv=
trap '[ -n "$srv" ] && kill $srv 2>/dev/null; rm -f "$work"/*.sock; [ -n "$KEEP" ] && echo "kept $work" || rm -rf "$work"' EXIT
fails=0 steps=0
step() { steps=$((steps+1)); if [ "$1" = 0 ]; then echo "  ok   $2"; else echo "  FAIL $2"; fails=$((fails+1)); fi; }
KEY=steam11087263542703816704

# start <name> <cache> VAR=value...
start() {
  local name=$1 cache=$2; shift 2
  sock=$work/$name.sock
  mkdir -p "$work/r-$name"
  env -u VKBRIDGE_HANG_DETECT -u VKBRIDGE_FAULT_ARM -u VKBRIDGE_TEST_LOST -u VKBRIDGE_TEST_DEVICE_FAULT \
      VKBRIDGE_PROBE=0 VKBRIDGE_REPORT_DIR="$work/r-$name" "$@" VK_DRIVER_FILES=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json} VK_ICD_FILENAMES= \
      timeout 120 "$out/vkbridge-server" --socket "$sock" --log "$work/$name.log" --cache-dir "$work/$cache" >/dev/null 2>&1 &
  srv=$!
  for _ in $(seq 50); do [ -S "$sock" ] && break; sleep 0.1; done
}
# client <key> <program> args...
client() { local k=$1; shift; VKBRIDGE_PCACHE_KEY=$k VKBRIDGE_SOCKET=$sock VK_DRIVER_FILES=$out/vkbridge_icd.json VK_ICD_FILENAMES= timeout 60 "$@"; }
stop() { kill $srv 2>/dev/null; wait $srv 2>/dev/null; srv=; }
lose() { client "$1" "$out/lost_check" 20 0 300 cb >"$work/$2.out" 2>&1; sleep 0.5; }
armfile() { echo "$work/$1/hang-detect-next/lost_check@$2"; }
hl() { grep -E "hang detector|HANG" "$work/$1.log" | sed 's/.*vkbridge-server . //' | head -${2:-3} | cut -c1-170 | tr '\n' '|'; }

# 1 a loss with a fault address arms it
start a1 cA VKBRIDGE_TEST_LOST=3:wait VKBRIDGE_TEST_DEVICE_FAULT=va VKBRIDGE_FAULT_ARM=1
lose $KEY a1
grep -q "fault address 0x7f1234560000" "$work/a1.log" &&
  grep -q "loss record, device [0-9]*: the hang detector is armed for the next launch of lost_check@$KEY (that program's devices, one launch; VKBRIDGE_FAULT_ARM=0: never)" "$work/a1.log" &&
  [ -f "$(armfile cA $KEY)" ] && ! grep -q "GPU hang detector on" "$work/a1.log"
step $? "1 a loss with a fault address arms the next launch: $(hl a1 2) file $( [ -f "$(armfile cA $KEY)" ] && echo there || echo missing)"
stop

# 2 the next launch: another program is not touched, the armed one gets the detector, which names its loss
start a2 cA VKBRIDGE_TEST_LOST=3:wait VKBRIDGE_TEST_DEVICE_FAULT=va VKBRIDGE_FAULT_ARM=1
client other "$out/vkbridge-smoke" >"$work/a2.smoke" 2>&1; rs=$?
sm_off=1; grep -q "GPU hang detector on" "$work/a2.log" && sm_off=0
file_kept=0; [ -f "$(armfile cA $KEY)" ] && file_kept=1
lose $KEY a2
[ $rs = 0 ] && [ $sm_off = 1 ] && [ $file_kept = 1 ] &&
  grep -q "hang detector on for lost_check@$KEY: its last launch lost a device with a GPU fault (imitated: a GPU page fault); this launch only" "$work/a2.log" &&
  grep -q "device [0-9]*: GPU hang detector on" "$work/a2.log" &&
  grep -Eq "HANG device [0-9]+ queue [0-9]+: submission #[0-9]+ " "$work/a2.log" &&
  grep -q "HANG   fault address 7f1234560000 is in no buffer whose address was asked for" "$work/a2.log" &&
  ! grep -q "HANG fault info" "$work/a2.log" &&
  [ ! -f "$(armfile cA $KEY)" ] &&
  grep -q "the hang detector was on for this launch: not armed again" "$work/a2.log"
step $? "2 next launch: smoke untouched (file kept $file_kept), the armed program named by the detector, file taken: $(hl a2 5)"
stop

# 3 the launch after one that took it (no loss there): off
start a3 cB VKBRIDGE_TEST_LOST=3:wait VKBRIDGE_TEST_DEVICE_FAULT=va VKBRIDGE_FAULT_ARM=1
lose $KEY a3a
stop
start a3b cB VKBRIDGE_FAULT_ARM=1
client $KEY "$out/lost_check" 20 0 300 cb >"$work/a3b.out" 2>&1 # no loss here: it ends 'never lost', its device ran
stop
start a3c cB VKBRIDGE_FAULT_ARM=1
client $KEY "$out/lost_check" 20 0 300 cb >"$work/a3c.out" 2>&1
grep -q "device [0-9]* created" "$work/a3c.log" && grep -q "GPU hang detector on" "$work/a3b.log" && ! grep -q "GPU hang detector on" "$work/a3c.log" &&
  [ ! -f "$(armfile cB $KEY)" ]
step $? "3 taken once: on in the launch after the loss, off in the one after that: b: $(hl a3b 1) c: $(hl a3c 1)"
stop

# 4 a loss with no driver fault report: not armed
start a4 cC VKBRIDGE_TEST_LOST=3:wait VKBRIDGE_FAULT_ARM=1
lose $KEY a4
grep -q "loss record, device [0-9]*: no fault report" "$work/a4.log" && ! grep -q "hang detector is armed" "$work/a4.log" &&
  [ ! -e "$work/cC/hang-detect-next" -o -z "$(ls -A "$work/cC/hang-detect-next" 2>/dev/null)" ]
step $? "4 a loss with no fault report does not arm: $(grep -m1 'no fault report' "$work/a4.log" | sed 's/.*vkbridge-server . //' | cut -c1-140)"
stop

# 5 the off switch
start a5 cD VKBRIDGE_TEST_LOST=3:wait VKBRIDGE_TEST_DEVICE_FAULT=va VKBRIDGE_FAULT_ARM=0
lose $KEY a5
grep -q "fault address 0x7f1234560000" "$work/a5.log" && ! grep -q "hang detector is armed" "$work/a5.log" && [ ! -f "$(armfile cD $KEY)" ]
step $? "5 VKBRIDGE_FAULT_ARM=0: not armed"
stop

# 6 armed for one game, the same program under another key is another game
start a6 cE VKBRIDGE_TEST_LOST=3:wait VKBRIDGE_TEST_DEVICE_FAULT=va VKBRIDGE_FAULT_ARM=1
lose $KEY a6
stop
start a6b cE VKBRIDGE_FAULT_ARM=1
client steam400 "$out/lost_check" 20 0 300 cb >"$work/a6b.out" 2>&1
grep -q "device [0-9]* created" "$work/a6b.log" && ! grep -q "GPU hang detector on" "$work/a6b.log" && [ -f "$(armfile cE $KEY)" ]
step $? "6 another key (another game) is not armed; the armed game's file stays"
stop

# 7 the default (test59, 11-14: the armed launch cost DMC5 ~10% fps on the Ultra): off unless
#   VKBRIDGE_FAULT_ARM=1. A loss with a fault address arms nothing, and an arm file an older build left is not taken:
#   that launch runs without the detector.
start a7 cF VKBRIDGE_TEST_LOST=3:wait VKBRIDGE_TEST_DEVICE_FAULT=va
lose $KEY a7
stop
mkdir -p "$work/cF/hang-detect-next"; echo "a GPU fault (an older build's file)" > "$(armfile cF steam400)"
start a7b cF
client steam400 "$out/lost_check" 20 0 300 cb >"$work/a7b.out" 2>&1
grep -q "fault address 0x7f1234560000" "$work/a7.log" && ! grep -q "hang detector is armed" "$work/a7.log" && [ ! -f "$(armfile cF $KEY)" ] &&
  grep -q "device [0-9]* created" "$work/a7b.log" && ! grep -q "GPU hang detector on" "$work/a7b.log"
step $? "7 default (no VKBRIDGE_FAULT_ARM): a loss arms nothing; an older build's arm file is not taken: $(hl a7 1) $(hl a7b 1)"
stop

if [ $fails = 0 ]; then echo "OK: $steps of $steps steps"; else echo "FAILED: $fails of $steps steps"; exit 1; fi
