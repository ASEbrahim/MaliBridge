#!/bin/bash
# One guard around every driver call a connection thread makes for a client (server_guard.c
# vkb_guard_serve): a driver fault there takes that program's device down, not the server with every
# program on it. The players' fingerprints, imitated beneath every layer (server_crashctx.c,
# VKBRIDGE_TEST_CRASH_CMD, VKBRIDGE_TEST_CRASH_ONCE=1: the first such call of a client faults):
#   - No Man's Sky's last crash (a player log vkbridge-server.log:4074 :1601): "crashed with signal
#     11 ... in vkCmdDraw" / vkCmdDrawIndexed inside a command buffer's replay;
#   - Dying Light, Mali-G1-Ultra: SIGSEGV at 0x32 in the driver's vkQueueSubmit2.
#   1  a fault in a replayed vkCmdDraw: the server lives and says which program, which call, which
#      command of the stream (command N of M);
#   2  that program sees VK_ERROR_DEVICE_LOST (its calls refused) and ends: no hang;
#   3  a second program works on the same server;
#   4  a fault in vkQueueSubmit (0x32): the server lives, the program sees its device lost;
#   5  and the next program works;
#   6  A/B: with VKBRIDGE_REQ_GUARD=0 the draw fault kills the server (the crash line as before).
# Prints one line per step and "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
work=$(mktemp -d)
srv=
trap '[ -n "$srv" ] && kill $srv 2>/dev/null; rm -f "$work"/*.sock; [ -n "$KEEP" ] && echo "kept $work" || rm -rf "$work"' EXIT
fails=0 steps=0
step() { steps=$((steps+1)); if [ "$1" = 0 ]; then echo "  ok   $2"; else echo "  FAIL $2"; fails=$((fails+1)); fi; }

start() {
  local name=$1 cmd=$2; shift 2
  sock=$work/$name.sock
  env VKBRIDGE_PROBE=0 VKBRIDGE_TEST_CRASH_ONCE=1 VKBRIDGE_TEST_CRASH_CMD=$cmd "$@" \
      VK_DRIVER_FILES=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json} VK_ICD_FILENAMES= \
      "$out/vkbridge-server" --socket "$sock" --log "$work/$name.log" --cache-dir "$work/c-$name" >/dev/null 2>&1 &
  srv=$!
  for _ in $(seq 50); do [ -S "$sock" ] && break; sleep 0.1; done
}
client() { VKBRIDGE_SOCKET=$sock VK_DRIVER_FILES=$out/vkbridge_icd.json VK_ICD_FILENAMES= timeout ${T:-60} "$@"; }
stop() { kill $srv 2>/dev/null; wait $srv 2>/dev/null; srv=; }

start draw vkCmdDraw
client "$out/vkbridge-smoke" >"$work/draw.out" 2>&1; r=$?
sleep 0.2
line=$(grep -m1 "CONTAINED driver crash: .* in vkCmdDraw" "$work/draw.log" | sed 's/.*vkbridge-server E //')
kill -0 $srv 2>/dev/null && ! grep -q "crashed with signal" "$work/draw.log" &&
  echo "$line" | grep -q "signal 11 (address 0x40) in vkCmdDraw (command [0-9]* of [0-9]* of command buffer 0x[0-9a-f]*'s replay) on device [0-9]* (vkbridge-smoke, pid [0-9]*)"
step $? "1 the server lives: ${line:0:260}"
[ $r != 124 ] && grep -q "the server refused the call (unknown object)" "$work/draw.out"
step $? "2 the program sees its device lost and ends (exit $r): $(grep -m1 "refused the call" "$work/draw.out" | cut -c1-120)"
client "$out/endcb_check" ok >"$work/next.out" 2>&1
step $? "3 the next program works: $(tail -1 "$work/next.out")"
stop

start submit vkQueueSubmit
client "$out/endcb_check" lost >"$work/submit.out" 2>&1; r=$?
sleep 0.2
line=$(grep -m1 "CONTAINED driver crash: .* in vkQueueSubmit" "$work/submit.log" | sed 's/.*vkbridge-server E //')
kill -0 $srv 2>/dev/null && ! grep -q "crashed with signal" "$work/submit.log" &&
  echo "$line" | grep -q "signal 11 (address 0x32) in vkQueueSubmit\( ([^)]*)\)\? on device [0-9]* (endcb_check, pid [0-9]*)" &&
  [ $r = 0 ] && grep -q "^PASS the device is lost" "$work/submit.out"
step $? "4 a submit fault: the server lives, the program sees its device lost (exit $r): ${line:0:200}"
client "$out/endcb_check" ok >"$work/next2.out" 2>&1
step $? "5 the next program works: $(tail -1 "$work/next2.out")"
stop

start noguard vkCmdDraw VKBRIDGE_REQ_GUARD=0
{ client "$out/vkbridge-smoke" >"$work/noguard.out" 2>&1; } 2>/dev/null
for _ in $(seq 30); do kill -0 $srv 2>/dev/null || break; sleep 0.1; done
line=$(grep -m1 "crashed with signal" "$work/noguard.log")
! kill -0 $srv 2>/dev/null && echo "$line" | grep -q "crashed with signal 11 (code 1, address 0x40) in vkCmdDraw"
step $? "6 A/B: without the guard the draw fault kills the server: ${line#*E }"
srv=

if [ $fails = 0 ]; then echo "OK: $steps steps"; else echo "FAILED: $fails of $steps steps"; [ -n "$KEEP" ] || for f in "$work"/*.log; do echo "--- $f"; grep -E " E | W " "$f" | tail -6; done; fi
[ $fails = 0 ]
