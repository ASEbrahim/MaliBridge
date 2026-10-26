#!/bin/bash
# The separate server and a hangup (server_main.c): the app ends the server itself, last, once the
# session's programs are gone; a SIGHUP that reaches the whole session must not end it first.
#   1  after a SIGHUP the server still runs;
#   2  and still serves a program (vkbridge-smoke);
#   3  a SIGTERM still stops it.
# Prints one line per step and "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
work=$(mktemp -d)
sock=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-hup-$$.sock
env VKBRIDGE_PROBE=0 VK_DRIVER_FILES=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json} VK_ICD_FILENAMES= \
    "$out/vkbridge-server" --socket "$sock" --log "$work/vkbridge-server.log" --cache-dir "$work/c" >/dev/null 2>&1 &
srv=$!
for _ in $(seq 50); do [ -S "$sock" ] && break; sleep 0.1; done
trap 'kill $srv 2>/dev/null; rm -f "$sock"; [ -n "$KEEP" ] && echo "kept $work" || rm -rf "$work"' EXIT
fails=0 steps=0
step() { steps=$((steps+1)); if [ "$1" = 0 ]; then echo "  ok   $2"; else echo "  FAIL $2"; fails=$((fails+1)); fi; }

kill -HUP $srv; sleep 0.5
kill -0 $srv 2>/dev/null; step $? "1 running after a SIGHUP"
VKBRIDGE_SOCKET=$sock VK_DRIVER_FILES=$out/vkbridge_icd.json VK_ICD_FILENAMES= "$out/vkbridge-smoke" >"$work/smoke.log" 2>&1
step $? "2 serves a program after it ($(tail -1 "$work/smoke.log"))"
kill -TERM $srv; wait $srv 2>/dev/null; st=$?; srv=
step $([ $st = 143 ]; echo $?) "3 a SIGTERM stops it (status $st)"

if [ $fails = 0 ]; then echo "OK: $steps steps"; else echo "FAILED: $fails of $steps steps"; fi
[ $fails = 0 ]
