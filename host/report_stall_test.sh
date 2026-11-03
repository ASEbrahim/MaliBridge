#!/bin/bash
# The session report's "presents stopped" line (server_report.c), with a server of its own and
# VKBRIDGE_REPORT_STALL_S=2. A host program holds a device for a while; its frames line (what
# client/frames.c writes at presents, here written by the test) says its last frame was:
#   1  just now, the program running on: no line;
#   2  then nothing new for several seconds while it runs: "no frame for the last N s while it runs";
#   3  a program that exits several seconds after its last frame: "presents stopped N s before it exited";
#   4  a program presenting up to its exit (a normal quit), the report after its exit: no line for it;
#   5  a program stops while another presents after it: a note "another program was in front (normal)", not a problem;
#      (step 2 also checks that with no other program presenting the alarm is in the Summary, worded as before);
#   6  a game process that exits soon after its device was created (VKBRIDGE_REPORT_QUICK_EXIT_S=3 here): "<exe> exited N s
#      after its device was created (M frames); a Proton log helps: launch options PROTON_LOG=1 %command%" in the Summary
#      (LEGO Indiana Jones gone 104 ms after vkCreateDevice, nothing in the log);
#   7  not for Steam's and Proton's own programs (steamwebhelper, explorer.exe, gamescope), nor for a game that ran past
#      that with its frames counted (a normal quit); with 5 frames only, a game that ran longer is named.
# Prints one line per step and "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
work=$(mktemp -d)
sess=$work/s
mkdir -p "$sess"
sock=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-stall-$$.sock
env VKBRIDGE_REPORT_STALL_S=2 VKBRIDGE_REPORT_QUICK_EXIT_S=3 VKBRIDGE_PROBE=0 VK_DRIVER_FILES=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json} VK_ICD_FILENAMES= \
    "$out/vkbridge-server" --socket "$sock" --log "$sess/vkbridge-server.log" --cache-dir "$work/c" >/dev/null 2>&1 &
srv=$!
for _ in $(seq 50); do [ -S "$sock" ] && break; sleep 0.1; done
trap 'kill $srv $prog $prog2 $feed 2>/dev/null; rm -f "$sock"; [ -n "$KEEP" ] && echo "kept $work" || rm -rf "$work"' EXIT
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
rep2=$(report_after 6)
r2=$(grep -o "no frame for the last [0-9]* s while it runs" <<<"$rep2" | head -1)
s2=$(sed -n '/^## Summary/,/^## Device/p' <<<"$rep2" | grep -c "no frame for the last [0-9]* s while it runs (presents stopped)")
step $([ -n "$r2" ] && [ "$s2" = 1 ] && ! grep -q "in front" <<<"$rep2"; echo $?) "2 frames stopped while it runs, nothing else presents: ${r2:-no line}, in the Summary ($s2), no 'in front' remark"
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

# 5: two programs; the one behind (Steam UI / Wine desktop) stops presenting while the one in front
# keeps going: the behind one is still named, as a note ("another program was in front (normal)")
# under "Notes", not as a problem in the Summary; the front one has no line.
"$out/pcache_check" --count 2 --hold 30 >$work/e.log 2>&1 &
prog=$!
"$out/pcache_check" --count 2 --hold 30 >$work/f.log 2>&1 &
prog2=$!
sleep 1
frames $prog "$(mono)"
( while :; do frames $prog2 "$(mono)"; sleep 0.5; done ) &
feed=$!
rep5=$(report_after 8)
summ5=$(sed -n '/^## Summary/,/^## Notes\|^## Device/p' <<<"$rep5")
notes5=$(sed -n '/^## Notes/,/^## Device/p' <<<"$rep5")
n5=$(grep "no frame for the last" <<<"$notes5" | grep -c "another program was in front (normal)")
s5=$(grep -c "presents stopped\|no frame for the last" <<<"$summ5")
step $([ "$n5" = 1 ] && [ "$s5" = 0 ]; echo $?) "5a program behind another that presents: one informational note ($n5), none in the Summary ($s5)"
kill $feed $prog2 2>/dev/null; wait $feed 2>/dev/null
kill $prog 2>/dev/null; prog=

# 6, 7: programs named as games and as Steam's / Proton's own, all exiting 1-2 s after their device
for nm in mygame steamwebhelper explorer.exe gamescope-wl; do ln -sf "$out/pcache_check" "$work/$nm"; done
for nm in mygame steamwebhelper explorer.exe gamescope-wl; do
  "$work/$nm" --count 2 --hold 1 >$work/x-$nm.log 2>&1 &
  wait $!
done
rep6=$(report_after 7)
sum6=$(sed -n '/^## Summary/,/^## Notes\|^## Device/p' <<<"$rep6")
g6=$(grep -c "mygame: exited [0-9]* s after its device was created (0 frames); a Proton log helps: launch options PROTON_LOG=1 %command%" <<<"$sum6")
h6=$(grep "exited [0-9]* s after its device was created" <<<"$rep6" | grep -c "steamwebhelper\|explorer.exe\|gamescope")
step $([ "$g6" = 1 ] && [ "$h6" = 0 ]; echo $?) "6 a game exiting right after its device: the line in the Summary ($g6), the helpers' (steamwebhelper, explorer.exe, gamescope) none ($h6)"

# a game that ran past the limit with its frames counted (a normal quit): no line; with 5 frames only: the line
"$work/mygame" --count 2 --hold 5 >$work/y.log 2>&1 &
prog=$!
sleep 1
( while :; do frames $prog "$(mono)"; sleep 0.5; done ) &
feed=$!
wait $prog 2>/dev/null; prog=
kill $feed; wait $feed 2>/dev/null
rep7=$(report_after 7)
n7=$(grep "mygame" <<<"$rep7" | grep "exited [0-9]* s after its device was created" | grep -vc "(0 frames)")
"$work/mygame" --count 2 --hold 5 >$work/z.log 2>&1 &
prog=$!
sleep 1
echo "pid=$prog comm=mygame presents=5 t_first=1.0 t_last=2.0 fps_avg=60.0 fps_p50=60.0 ft_p50=16 ft_p90=17 ft_p99=20 ft_max=30 swapchains=1 external=0" > "$sess/vkbridge-frames-$prog.txt"
wait $prog 2>/dev/null; prog=
rep8=$(report_after 7)
n8=$(grep -c "mygame: exited [0-9]* s after its device was created (5 frames)" <<<"$rep8")
step $([ "$n7" = 0 ] && [ "$n8" = 1 ]; echo $?) "7 a game that ran past the limit with 600 frames: no line ($n7); with 5 frames: named ($n8)"

if [ $fails = 0 ]; then echo "OK: $steps steps"; else echo "FAILED: $fails of $steps steps"; fi
[ $fails = 0 ]
