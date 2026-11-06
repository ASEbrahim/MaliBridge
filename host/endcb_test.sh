#!/bin/bash
# Xiaomi, XRing O3, Mali-G2-Ultra r56p0 beta, Animal Well through vkd3d-proton: the one
# server died twice, same driver pc, with a read of address 0x18 inside the driver's
# vkEndCommandBuffer of a client's command buffer ("command 4 of its stream; the last ones:
# vkBeginCommandBuffer vkCmdResetQueryPool vkCmdPipelineBarrier vkCmdFillBuffer"), and took Steam
# and everything with it. A fault there is now contained (server_guard.c): the command buffer is not
# recorded, its device is stopped (taken as lost, never reached again: the fault left the driver's
# command buffer state broken), its program ends, the other programs go on.
# VKBRIDGE_TEST_CRASH_CMD=vkEndCommandBuffer VKBRIDGE_TEST_CRASH_ONCE=1: a client's first replayed
# end reads 0x18 at the driver boundary (server_crashctx.c).
#   1  the server lives and says it contained the fault in vkEndCommandBuffer, naming the stream:
#      the app's begin + reset, then the query mirror's barrier + fill (client/query.c) - QQBX's 4;
#   2  the program sees its device lost and ends (no hang);
#   3  a second program works on the same server;
#   4  the first program's exit leaves the stopped device as it is (no driver call on it);
#   5  with VKBRIDGE_QUERY_MIRROR=0 (guest) the same command buffer holds the app's 2 commands only;
#   6  A/B: with VKBRIDGE_NO_CRASH_GUARD=1 the same fault kills the server (QQBX's crash line).
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
  env VKBRIDGE_PROBE=0 VKBRIDGE_TEST_CRASH_ONCE=1 VKBRIDGE_TEST_CRASH_CMD=vkEndCommandBuffer "$@" \
      VK_DRIVER_FILES=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json} VK_ICD_FILENAMES= \
      "$out/vkbridge-server" --socket "$sock" --log "$work/$name.log" --cache-dir "$work/c-$name" >/dev/null 2>&1 &
  srv=$!
  for _ in $(seq 50); do [ -S "$sock" ] && break; sleep 0.1; done
}
run() { VKBRIDGE_SOCKET=$sock VK_DRIVER_FILES=$out/vkbridge_icd.json VK_ICD_FILENAMES= timeout ${T:-60} "$out/endcb_check" "$@"; }
stop() { kill $srv 2>/dev/null; wait $srv 2>/dev/null; srv=; }
tail4='vkBeginCommandBuffer vkCmdResetQueryPool vkCmdPipelineBarrier vkCmdFillBuffer'

start guard
run lost >"$work/lost.out" 2>&1; r=$?
sleep 0.2
line=$(grep -m1 'CONTAINED driver crash: .* in vkEndCommandBuffer' "$work/guard.log" | sed 's/.*vkbridge-server E //')
kill -0 $srv 2>/dev/null && ! grep -q "crashed with signal" "$work/guard.log" &&
  echo "$line" | grep -q "signal 11 (address 0x18) in vkEndCommandBuffer on device [0-9]* (endcb_check, pid [0-9]*)" &&
  grep -q "the stream: $tail4\$" "$work/guard.log"
step $? "1 the server lives: ${line:0:200}; $(grep -m1 -o 'the stream: .*' "$work/guard.log")"
[ $r = 0 ] && grep -q "^PASS the device is lost" "$work/lost.out"
step $? "2 the program sees its device lost and ends (exit $r): $(tail -1 "$work/lost.out")"
run ok >"$work/ok.out" 2>&1
step $? "3 the next program works: $(tail -1 "$work/ok.out")"
for _ in $(seq 30); do grep -q "exited without destroying device .* left as it is\|device [0-9]* destroyed" "$work/guard.log" && break; sleep 0.1; done
dev=$(echo "$line" | sed -n 's/.* on device \([0-9]*\) .*/\1/p')
! grep -q "device $dev destroyed" "$work/guard.log" && grep -q "without destroying device $dev, which is left as it is" "$work/guard.log"
step $? "4 the stopped device $dev is left as it is at the program's end: $(grep -m1 "device $dev, which is left" "$work/guard.log" | sed 's/.*vkbridge-server W //' | cut -c1-140)"
stop

start nomirror
VKBRIDGE_QUERY_MIRROR=0 run lost >"$work/nomirror.out" 2>&1
sleep 0.2
grep -q 'the stream: vkBeginCommandBuffer vkCmdResetQueryPool$' "$work/nomirror.log"
step $? "5 without the query mirror the stream is the app's own: $(grep -m1 -o 'the stream: .*' "$work/nomirror.log")"
stop

start noguard VKBRIDGE_NO_CRASH_GUARD=1
{ run lost >"$work/noguard.out" 2>&1; } 2>/dev/null
for _ in $(seq 30); do kill -0 $srv 2>/dev/null || break; sleep 0.1; done
line=$(grep -m1 "crashed with signal" "$work/noguard.log")
! kill -0 $srv 2>/dev/null && echo "$line" | grep -q "crashed with signal 11 (code 1, address 0x18) in vkEndCommandBuffer (the driver's, after the stream)" &&
  grep -q "the last ones: $tail4\$" "$work/noguard.log"
step $? "6 A/B: without containment the fault kills the server: ${line#*E }"
srv=

if [ $fails = 0 ]; then echo "OK: $steps steps"; else echo "FAILED: $fails of $steps steps"; [ -n "$KEEP" ] || for f in "$work"/*.log; do echo "--- $f"; grep -E " E | W " "$f" | tail -6; done; fi
[ $fails = 0 ]
