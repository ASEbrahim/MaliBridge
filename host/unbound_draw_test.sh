#!/bin/bash
# Mali-G615 MC2 r44p1, No Man's Sky: after the third contained compile crash the device's
# pipelines failed (server_guard.c), the game skipped the binds and drew; Mali's vkCmdDrawIndexed read
# through the missing pipeline (0x7a0) and the one server died with every program. The replay leaves
# out a draw or dispatch with no pipeline bound in its command buffer, and a vkCmdBindPipeline of
# VK_NULL_HANDLE (RADV crashes on that one itself). VKBRIDGE_TEST_CRASH_CMD=unbound imitates Mali: a
# draw that reaches the driver with no graphics pipeline bound faults.
#   1  crash_contain_check passes whole: the contained crashes, the poisoning, the unbound draw, the
#      null bind and the unbound dispatch left out on the poisoned device, a new device draws and
#      dispatches correctly (the output of bound draws as before);
#   2  the server lives and said what it left out (vkCmdDraw, vkCmdDispatch);
#   3  the A/B: with VKBRIDGE_UNBOUND_DRAWS=keep the imitated Mali fault takes the server down in
#      vkCmdDraw, and the crash line says no graphics pipeline was bound.
# Prints one line per step and "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
work=$(mktemp -d)
srv=
trap '[ -n "$srv" ] && kill $srv 2>/dev/null; rm -f "$work"/*.sock; [ -n "$KEEP" ] && echo "kept $work" || rm -rf "$work"' EXIT
fails=0 steps=0
step() { steps=$((steps+1)); if [ "$1" = 0 ]; then echo "  ok   $2"; else echo "  FAIL $2"; fails=$((fails+1)); fi; }

# start <name> VAR=value...: a server with VKBRIDGE_TEST_CRASH_PIPELINE=1 and the imitated Mali draw fault
start() {
  local name=$1; shift
  sock=$work/$name.sock
  env VKBRIDGE_PROBE=0 VKBRIDGE_TEST_CRASH_PIPELINE=1 VKBRIDGE_TEST_CRASH_CMD=unbound "$@" \
      VK_DRIVER_FILES=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json} VK_ICD_FILENAMES= \
      "$out/vkbridge-server" --socket "$sock" --log "$work/$name.log" --cache-dir "$work/c-$name" >/dev/null 2>&1 &
  srv=$!
  for _ in $(seq 50); do [ -S "$sock" ] && break; sleep 0.1; done
}
check() { VKBRIDGE_SOCKET=$sock VK_DRIVER_FILES=$out/vkbridge_icd.json VK_ICD_FILENAMES= timeout 120 "$out/crash_contain_check" --unbound >"$work/$1.out" 2>&1; }

start fix
check fix; r=$?
step $r "1 crash_contain_check with the fix ($(tail -1 "$work/fix.out"))"
kill -0 $srv 2>/dev/null && ! grep -q "crashed with signal" "$work/fix.log" &&
  grep -q "vkCmdDraw with no graphics pipeline bound in command buffer .* (crash_contain_c, pid [0-9]*): left out" "$work/fix.log" &&
  grep -q "vkCmdDispatch with no compute pipeline bound in command buffer .*: left out" "$work/fix.log"
step $? "2 the server lives and said what it left out: $(grep -m1 'with no graphics pipeline bound' "$work/fix.log" | sed 's/.*vkbridge-server W //' | cut -c1-160)"
kill $srv 2>/dev/null; wait $srv 2>/dev/null; srv=

# VKBRIDGE_REQ_GUARD=0: the fault reaches the driver as before the request guard (reqguard_test.sh contains it)
start keep VKBRIDGE_UNBOUND_DRAWS=keep VKBRIDGE_REQ_GUARD=0
{ check keep; } 2>/dev/null # the server dies here (and the check with it): the shell's notices are not the test's
for _ in $(seq 30); do kill -0 $srv 2>/dev/null || break; sleep 0.1; done
line=$(grep -m1 "crashed with signal" "$work/keep.log")
! kill -0 $srv 2>/dev/null && echo "$line" | grep -q "in vkCmdDraw from crash_contain_c" && grep -q "state: no graphics pipeline bound in this command buffer" "$work/keep.log"
step $? "3 without it (VKBRIDGE_UNBOUND_DRAWS=keep) the imitated Mali fault kills the server: ${line#*E } | $(grep -m1 '  state: ' "$work/keep.log" | sed 's/.*E   //')"
srv=

if [ $fails = 0 ]; then echo "OK: $steps steps"; else echo "FAILED: $fails of $steps steps"; grep -E "FAIL" "$work"/*.out | head; fi
[ $fails = 0 ]
