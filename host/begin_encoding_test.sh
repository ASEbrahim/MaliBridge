#!/bin/bash
# vkBeginCommandBuffer's begin info on the wire (a server of its own):
#   1  begin_check ok (asynchronous ends, the default): a primary and a secondary with inheritance
#      info, begun three times each, all pass; no protocol error in the server log;
#   2  the same with VKBRIDGE_SYNC=1;
#   3  the begin info encoded without its count (VKBRIDGE_TEST_BEGIN_NOCOUNT=1, as the bridge copy
#      of 2026-10-10 did): the server refuses each begin before it reaches the driver, drops the
#      command buffer's commands, and vkEndCommandBuffer fails. Before the decoder checked the
#      count, it read 0 from the padding and handed the driver a begin info of stale bytes: Mali
#      crashed in vkBeginCommandBuffer (SIGSEGV at 0x10, Mali-G615 and G925), the desktop driver
#      did not;
#   4  the server is still up and step 1 passes again on it;
#   5-10  the driver fails to record a command buffer (as an XRing O3 out of memory did in Cyberpunk
#      2077), a server of its own each (VKBRIDGE_TEST_BEGIN_FAIL / VKBRIDGE_TEST_END_FAIL count the
#      server's begins/ends): round 1's primary's begin (4) or end (4), or its secondary's begin (3),
#      with asynchronous ends and with VKBRIDGE_SYNC=1. Nothing is replayed after a failed begin; the
#      submit leaves the command buffer out (a secondary: the primary's vkCmdExecuteCommands does),
#      its fence is still signalled, round 2 runs in full (begin_check dropped / dropped-secondary),
#      and the same server then runs step 1 for another app. Before, Mali crashed inside
#      vkEndCommandBuffer and took the session with it;
#  11-12  recorded again after a failed begin or end (VKBRIDGE_SYNC=1, begin_check recover): it runs.
# Prints one line per step and "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
work=$(mktemp -d)
sock=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-begin-$$.sock
log=$work/server.log
env -u VKBRIDGE_FAKE_MISSING -u VKBRIDGE_HIDE_EXTS -u VKBRIDGE_MODE \
    VK_DRIVER_FILES=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json} VK_ICD_FILENAMES= \
    "$out/vkbridge-server" --socket "$sock" --log "$log" --cache-dir "$work/c" >/dev/null 2>&1 &
srv=$!
for _ in $(seq 50); do [ -S "$sock" ] && break; sleep 0.1; done
trap 'kill $srv 2>/dev/null; rm -f "$sock"; [ -n "$KEEP" ] || rm -rf "$work"' EXIT
export VKBRIDGE_SOCKET=$sock VK_DRIVER_FILES=$out/vkbridge_icd.json VK_ICD_FILENAMES=
unset VKBRIDGE_HIDE_EXTS VKBRIDGE_MAX_API VKBRIDGE_SYNC VKBRIDGE_TEST_BEGIN_NOCOUNT
fails=0 steps=0
step() { steps=$((steps+1)); if [ "$1" = 0 ]; then echo "  ok   $2"; else echo "  FAIL $2"; fails=$((fails+1)); fi; }
proto_errors() { tail -n +$(($1+1)) "$log" | grep -cE "command stream:|malformed request|bad batch entry"; }

l=$(wc -l < "$log")
timeout 60 "$out/begin_check" ok >$work/r1.log 2>&1 && [ "$(proto_errors $l)" = 0 ]
step $? "1 asynchronous ends: $(tail -1 $work/r1.log); $(proto_errors $l) protocol errors"

l=$(wc -l < "$log")
VKBRIDGE_SYNC=1 timeout 60 "$out/begin_check" ok >$work/r2.log 2>&1 && [ "$(proto_errors $l)" = 0 ]
step $? "2 VKBRIDGE_SYNC=1: $(tail -1 $work/r2.log); $(proto_errors $l) protocol errors"

l=$(wc -l < "$log")
VKBRIDGE_SYNC=1 VKBRIDGE_TEST_BEGIN_NOCOUNT=1 timeout 60 "$out/begin_check" refused >$work/r3.log 2>&1
rc=$?
refusals=$(tail -n +$((l+1)) "$log" | grep -c "command stream: vkBeginCommandBuffer failed")
dropped=$(tail -n +$((l+1)) "$log" | grep -c "not begun; its [0-9]* bytes of commands are dropped")
[ $rc = 0 ] && [ "$refusals" = 6 ] && [ "$dropped" = 6 ] && kill -0 $srv 2>/dev/null
step $? "3 begin info without its count: $(tail -1 $work/r3.log); $refusals begins refused, $dropped command buffers dropped (6 each)"

l=$(wc -l < "$log")
kill -0 $srv 2>/dev/null && timeout 60 "$out/begin_check" ok >$work/r4.log 2>&1 && [ "$(proto_errors $l)" = 0 ]
step $? "4 the same server afterwards: $(tail -1 $work/r4.log)"

kill $srv 2>/dev/null; wait $srv 2>/dev/null
# One server with $1 (VAR=n); begin_check $2, with VKBRIDGE_SYNC=1 when $3; then begin_check ok on the
# same server. Sets left (left-out lines), alive, after (step 1's result on it).
cbfail() {
  local fail=$1 mode=$2 sync=$3 tag=$4 rc
  rm -f "$sock"
  env -u VKBRIDGE_FAKE_MISSING -u VKBRIDGE_HIDE_EXTS -u VKBRIDGE_MODE "$fail" \
      VK_DRIVER_FILES=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json} VK_ICD_FILENAMES= \
      "$out/vkbridge-server" --socket "$sock" --log "$work/$tag-server.log" --cache-dir "$work/c" >/dev/null 2>&1 &
  srv=$!
  for _ in $(seq 50); do [ -S "$sock" ] && break; sleep 0.1; done
  if [ -n "$sync" ]; then VKBRIDGE_SYNC=1 timeout 60 "$out/begin_check" $mode >$work/$tag.log 2>&1
  else timeout 60 "$out/begin_check" $mode >$work/$tag.log 2>&1; fi
  rc=$?
  timeout 60 "$out/begin_check" ok >$work/$tag-after.log 2>&1 && after=0 || after=1
  kill -0 $srv 2>/dev/null && alive=0 || alive=1
  left=$(grep -c "the driver did not record left out" "$work/$tag-server.log")
  notbegun=$(grep -c "the driver would not begin it" "$work/$tag-server.log")
  kill $srv 2>/dev/null; wait $srv 2>/dev/null
  [ $rc = 0 ] && [ $alive = 0 ] && [ $after = 0 ]
}
cbfail VKBRIDGE_TEST_BEGIN_FAIL=4 dropped "" r5 && [ "$left" = 1 ] && [ "$notbegun" = 1 ]
step $? "5 begin fails, asynchronous: $(tail -1 $work/r5.log); $left submit(s) left it out; then $(tail -1 $work/r5-after.log)"
cbfail VKBRIDGE_TEST_BEGIN_FAIL=4 dropped 1 r6 && [ "$left" = 1 ]
step $? "6 begin fails, VKBRIDGE_SYNC=1: $(tail -1 $work/r6.log); then $(tail -1 $work/r6-after.log)"
cbfail VKBRIDGE_TEST_END_FAIL=4 dropped "" r7 && [ "$left" = 1 ]
step $? "7 end fails, asynchronous: $(tail -1 $work/r7.log); then $(tail -1 $work/r7-after.log)"
cbfail VKBRIDGE_TEST_END_FAIL=4 dropped 1 r8 && [ "$left" = 1 ]
step $? "8 end fails, VKBRIDGE_SYNC=1: $(tail -1 $work/r8.log); then $(tail -1 $work/r8-after.log)"
cbfail VKBRIDGE_TEST_BEGIN_FAIL=3 dropped-secondary "" r9 && [ "$left" = 1 ] &&
  grep -q "vkCmdExecuteCommands: 1 command buffer(s) the driver did not record left out" $work/r9-server.log
step $? "9 a secondary's begin fails, asynchronous: $(tail -1 $work/r9.log); then $(tail -1 $work/r9-after.log)"
cbfail VKBRIDGE_TEST_BEGIN_FAIL=3 dropped-secondary 1 r10 && [ "$left" = 1 ]
step $? "10 the same, VKBRIDGE_SYNC=1: $(tail -1 $work/r10.log); then $(tail -1 $work/r10-after.log)"
cbfail VKBRIDGE_TEST_BEGIN_FAIL=4 recover 1 r11 && [ "$left" = 0 ]
step $? "11 recorded again after a failed begin: $(tail -1 $work/r11.log); $left left out"
cbfail VKBRIDGE_TEST_END_FAIL=4 recover 1 r12 && [ "$left" = 0 ]
step $? "12 recorded again after a failed end: $(tail -1 $work/r12.log); $left left out"

if [ $fails = 0 ]; then echo "OK: $steps steps"; else echo "FAILED: $fails of $steps steps"; fi
[ $fails = 0 ]
