#!/bin/bash
# The session report's "presents stopped" line (server_report.c), with a server of its own and
# VKBRIDGE_REPORT_STALL_S=2. A host program holds a device for a while; its frames line (what
# client/frames.c writes at presents, here written by the test) says its last frame was:
#   1  just now, the program running on: no line;
#   2  then nothing new for several seconds while it runs: "no frame for the last N s while it runs";
#   3  a program that exits several seconds after its last frame: "presents stopped N s before it exited";
#   4  a program presenting up to its exit (a normal quit), the report after its exit: no line for it.
# Prints one line per step and "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
work=$(mktemp -d)
sess=$work/s
mkdir -p "$sess"
sock=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-stall-$$.sock
env VKBRIDGE_REPORT_STALL_S=2 VKBRIDGE_PROBE=0 VK_DRIVER_FILES=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json} VK_ICD_FILENAMES= \
    "$out/vkbridge-server" --socket "$sock" --log "$sess/vkbridge-server.log" --cache-dir "$work/c" >/dev/null 2>&1 &
srv=$!
for _ in $(seq 50); do [ -S "$sock" ] && break; sleep 0.1; done
trap 'kill $srv $prog $feed 2>/dev/null; rm -f "$sock"; [ -n "$KEEP" ] && echo "kept $work" || rm -rf "$work"' EXIT
export VKBRIDGE_SOCKET=$sock VK_DRIVER_FILES=$out/vkbridge_icd.json VK_ICD_FILENAMES=
fails=0 steps=0
step() { steps=$((steps+1)); if [ "$1" = 0 ]; then echo "  ok   $2"; else echo "  FAIL $2"; fails=$((fails+1)); fi; }
mono() { python3 -c 'import time; print("%.3f" % time.monotonic())'; }
frames() { # frames <pid> <t_last>: the client's line, 600 presents up to t_last
  local t=$2 tf
  tf=$(python3 -c "print('%.3f' % ($t - 10))")
  echo "pid=$1 comm=pcache_check presents=600 t_first=$tf t_last=$t fps_avg=60.0 fps_p50=60.0 ft_p50=16 ft_p90=17 ft_p99=20 ft_max=30 swapchains=1 external=0" \
    > "$sess/vkbridge-frames-$1.txt"
}
report_after() { # report_after <seconds>: the report as it is after its next rewrites
  sleep "$1"
  cat "$sess/malibridge-report.md" 2>/dev/null
}

"$out/pcache_check" --count 2 --hold 30 >$work/a.log 2>&1 &
prog=$!
sleep 1
( while :; do frames $prog "$(mono)"; sleep 0.5; done ) &   # presenting
feed=$!
r1=$(report_after 6 | grep -c "presents stopped\|no frame for the last")
kill $feed; wait $feed 2>/dev/null
step $([ "$r1" = 0 ]; echo $?) "1 frames up to now: no stall line ($r1)"
r2=$(report_after 6 | grep -o "no frame for the last [0-9]* s while it runs" | head -1)
step $([ -n "$r2" ]; echo $?) "2 frames stopped while it runs: ${r2:-no line}"
kill $prog 2>/dev/null; wait $prog 2>/dev/null; prog=
r3=$(report_after 7 | grep -o "presents stopped [0-9]* s before it exited" | head -1)
step $([ -n "$r3" ]; echo $?) "3 exited after its last frame: ${r3:-no line}"

SteamAppId=4999 "$out/pcache_check" --count 2 --hold 4 >$work/d.log 2>&1 &
prog=$!
sleep 1
( while :; do frames $prog "$(mono)"; sleep 0.5; done ) &
feed=$!
wait $prog 2>/dev/null; prog=
kill $feed; wait $feed 2>/dev/null
rep4=$(report_after 7)
r4=$(grep "steam4999" <<<"$rep4" | grep -c "presents stopped\|no frame for the last")
step $(grep -q "steam4999" <<<"$rep4" && [ "$r4" = 0 ]; echo $?) "4 frames up to its exit: no stall line for it ($r4; $(grep -c steam4999 <<<"$rep4") report line(s) name it)"

if [ $fails = 0 ]; then echo "OK: $steps steps"; else echo "FAILED: $fails of $steps steps"; fi
[ $fails = 0 ]
