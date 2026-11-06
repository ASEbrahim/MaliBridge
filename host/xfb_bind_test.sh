#!/bin/bash
# Mali-G925 r49p1, Feed the Cups through Zink and a player log (Mali-G615 r44p1, WEBFISHING
# through Zink): the one server died in Mali's vkCmdBeginTransformFeedbackEXT (near-null reads, 0xe78
# and 0xe60), command 31 of a stream that had bound its pipeline. The vertex-input layer
# (server_vidyn.c; vb_bake the same) holds a kept pipeline's bind until the draw, and Zink begins
# transform feedback between the bind and the draw: the driver had no graphics pipeline bound.
# The begin now binds what the draw would. And a begin with no pipeline at all (one that failed to
# build) is left out in the replay with its end, as draws are.
# VKBRIDGE_TEST_CRASH_CMD=xfb_unbound imitates Mali: a begin that reaches the driver with no graphics
# pipeline bound in its command buffer faults at 0xe78.
#   1  Zink's shape (xfb_check zink, the vertex input emulated: VKBRIDGE_FAKE_MISSING=vidyn): every
#      capture right, the server lives;
#   2  A/B: VKBRIDGE_XFB_BIND_FIRST=0 - the server dies in vkCmdBeginTransformFeedbackEXT at 0xe78
#      after a stream that bound its pipeline (a player log's fingerprint);
#   3  a failed pipeline (xfb_check nopipe): begin, draw and end left out, the server lives;
#   4  A/B: VKBRIDGE_UNBOUND_DRAWS=keep - the imitated fault kills the server.
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
  env VKBRIDGE_PROBE=0 VKBRIDGE_TEST_CRASH_CMD=xfb_unbound "$@" VK_DRIVER_FILES=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json} VK_ICD_FILENAMES= \
      "$out/vkbridge-server" --socket "$sock" --log "$work/$name.log" --cache-dir "$work/c-$name" >/dev/null 2>&1 &
  srv=$!
  for _ in $(seq 50); do [ -S "$sock" ] && break; sleep 0.1; done
}
check() { VKBRIDGE_SOCKET=$sock VK_DRIVER_FILES=$out/vkbridge_icd.json VK_ICD_FILENAMES= timeout 60 "$out/xfb_check" "$@"; }
stop() { kill $srv 2>/dev/null; wait $srv 2>/dev/null; srv=; }
gone() { for _ in $(seq 30); do kill -0 $srv 2>/dev/null || return 0; sleep 0.1; done; return 1; }

start zink VKBRIDGE_FAKE_MISSING=vidyn
check zink >"$work/zink.out" 2>&1; r=$?
[ $r = 77 ] && { echo "OK: skipped (no transform feedback)"; exit 0; }
sleep 0.2
[ $r = 0 ] && kill -0 $srv 2>/dev/null && ! grep -q "crashed with signal" "$work/zink.log" && grep -q "vertex input: first pipeline variant" "$work/zink.log"
step $? "1 Zink's shape with the vertex input emulated: $(tail -1 "$work/zink.out"); server alive"
stop

# VKBRIDGE_REQ_GUARD=0: the A/B arms show the fault reaching the driver; the request guard (reqguard_test.sh) would contain it
start zinknofix VKBRIDGE_FAKE_MISSING=vidyn VKBRIDGE_XFB_BIND_FIRST=0 VKBRIDGE_REQ_GUARD=0
{ check zink >"$work/zinknofix.out" 2>&1; } 2>/dev/null
gone; dead=$?
line=$(grep -m1 "crashed with signal" "$work/zinknofix.log")
[ $dead = 0 ] && echo "$line" | grep -q "address 0xe78) in vkCmdBeginTransformFeedbackEXT" && grep -q "replaying command buffer .* vkCmdBindPipeline" "$work/zinknofix.log"
step $? "2 A/B: with VKBRIDGE_XFB_BIND_FIRST=0 the server dies: ${line#*E }"
srv=

start nopipe
check nopipe >"$work/nopipe.out" 2>&1; r=$?
sleep 0.2
[ $r = 0 ] && kill -0 $srv 2>/dev/null && grep -q "vkCmdBeginTransformFeedbackEXT with no graphics pipeline bound .*: left out" "$work/nopipe.log"
step $? "3 a failed pipeline: begin and draw left out ($(tail -1 "$work/nopipe.out"))"
stop

start nopipekeep VKBRIDGE_UNBOUND_DRAWS=keep VKBRIDGE_REQ_GUARD=0
{ check nopipe >"$work/nopipekeep.out" 2>&1; } 2>/dev/null
gone; dead=$?
[ $dead = 0 ] && grep -q "crashed with signal 11 (code 1, address 0xe78) in vkCmdBeginTransformFeedbackEXT" "$work/nopipekeep.log"
step $? "4 A/B: with VKBRIDGE_UNBOUND_DRAWS=keep the imitated fault kills the server"
srv=

if [ $fails = 0 ]; then echo "OK: $steps steps"; else echo "FAILED: $fails of $steps steps"; [ -n "$KEEP" ] || for f in "$work"/*.log; do echo "--- $f"; grep -E " E | W " "$f" | tail -4; done; fi
[ $fails = 0 ]
