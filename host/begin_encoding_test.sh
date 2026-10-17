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
#   4  the server is still up and step 1 passes again on it.
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

if [ $fails = 0 ]; then echo "OK: $steps steps"; else echo "FAILED: $fails of $steps steps"; fi
[ $fails = 0 ]
