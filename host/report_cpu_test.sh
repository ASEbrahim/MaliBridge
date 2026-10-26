#!/bin/bash
# The session report's CPU lines (server_report.c) on a fake cpufreq tree (VKBRIDGE_CPUFREQ_DIR),
# the way HyperOS shows it to an app: scaling_cur_freq readable, scaling_max_freq not there.
#   policy0  at its floor (300 MHz) all along: idling, not held;
#   policy4  500 MHz for a few seconds, then 1900 of its 2700 (cpuinfo_max_freq): held;
# while a host program holds a device and presents (its frames line written by the test).
#   1  the cap is unknown, not "capped to 100%" (report.md and cpu_cap_min_pct null in the .json);
#   2  "CPU held at a fixed clock" names p4 1900 MHz and not p0.
# Prints one line per step and "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
work=$(mktemp -d)
sess=$work/s
cpu=$work/cpufreq
mkdir -p "$sess" "$cpu/policy0" "$cpu/policy4"
echo 300000 >"$cpu/policy0/scaling_cur_freq"
echo 2000000 >"$cpu/policy0/cpuinfo_max_freq"
echo 500000 >"$cpu/policy4/scaling_cur_freq"
echo 2700000 >"$cpu/policy4/cpuinfo_max_freq"
sock=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-cpu-$$.sock
env VKBRIDGE_CPUFREQ_DIR=$cpu VKBRIDGE_PROBE=0 VK_DRIVER_FILES=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json} VK_ICD_FILENAMES= \
    "$out/vkbridge-server" --socket "$sock" --log "$sess/vkbridge-server.log" --cache-dir "$work/c" >/dev/null 2>&1 &
srv=$!
for _ in $(seq 50); do [ -S "$sock" ] && break; sleep 0.1; done
trap 'kill $srv $prog $feed 2>/dev/null; rm -f "$sock"; [ -n "$KEEP" ] && echo "kept $work" || rm -rf "$work"' EXIT
export VKBRIDGE_SOCKET=$sock VK_DRIVER_FILES=$out/vkbridge_icd.json VK_ICD_FILENAMES=
fails=0 steps=0
step() { steps=$((steps+1)); if [ "$1" = 0 ]; then echo "  ok   $2"; else echo "  FAIL $2"; fails=$((fails+1)); fi; }
mono() { python3 -c 'import time; print("%.3f" % time.monotonic())'; }

"$out/pcache_check" --count 2 --hold 40 >$work/a.log 2>&1 &
prog=$!
( while :; do t=$(mono); echo "pid=$prog comm=pcache_check presents=600 t_first=$t t_last=$t fps_avg=60.0 fps_p50=60.0 swapchains=1 external=0" \
    >"$sess/vkbridge-frames-$prog.txt"; sleep 0.5; done ) &
feed=$!
sleep 5
echo 1900000 >"$cpu/policy4/scaling_cur_freq"
sleep 20
md=$(cat "$sess/malibridge-report.md" 2>/dev/null)
cap=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["device"]["cpu_cap_min_pct"])' "$sess/malibridge-report.json" 2>&1)
step $(grep -q "CPU cap unknown" <<<"$md" && ! grep -q "capped to 100%" <<<"$md" && [ "$cap" = None ]; echo $?) \
    "1 cap unknown ($(grep -c 'cap unknown' <<<"$md") line(s); json cpu_cap_min_pct $cap)"
held=$(grep -o "CPU held at a fixed clock while it ran: [^(]*" <<<"$md" | head -1)
step $(grep -q "p4 1900 MHz" <<<"$held" && ! grep -q "p0 " <<<"$held"; echo $?) "2 ${held:-no held line}"

if [ $fails = 0 ]; then echo "OK: $steps steps"; else echo "FAILED: $fails of $steps steps"; fi
[ $fails = 0 ]
