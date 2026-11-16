#!/bin/bash
# Always-on performance diagnostics (server/server_perf.c; client/hitch.c window fields;):
# per 10-s window of a presenting process a "perf:" line (verdict, fps, frame-time p50/p90, repeated frames,
# GPU busy and clock, the present wait split into driver / queue / IPC / local, compile/upload time, thermal,
# CPU cap, memory) and a "perf-threads:" line (the busiest threads with their CPU % and clusters, and the
# background processes), plus a Performance section in the session report. Periods shortened here
# (VKBRIDGE_TEST_PERF_PERIODS=0.5,2: a sample every 0.5 s, a window every 2 s); the GPU's sysfs files and the
# cpufreq tree are fakes (VKBRIDGE_GPU_SYSFS_ROOT, VKBRIDGE_CPUFREQ_DIR), so the numbers are known.
#   1  defaults: perf lines with sane values (fps, p50, GPU busy 73 / 850 MHz from the fakes, the present split
#      adding up, a window with the 300 ms frame counting repeats), perf-threads with the client's own thread
#      and the server among the background processes; the report's Performance section and json;
#   2  GPU busy 97 -> verdict GPU;   3  a spinning game thread, GPU busy 10 -> CPU, the spinner on top;
#   4  thermal status 4 in vkbridge-clocks.log -> THERMAL;   5  MemAvailable 300 MB -> MEMORY;
#   6  a 1.5 s pipeline compile -> COMPILE;   7  a 1.5 s BC conversion -> UPLOAD;
#   8  the server holding the present's fence wait 6 ms in its queue (VKBRIDGE_TEST_PRESENT_QUEUE_MS) -> PRESENT,
#      queue_ms about 6;   9  VKBRIDGE_PERF_DIAG=0: no perf line;
#  11  cpu-threads: line (per-thread run, run-queue wait, idle, sleep reasons; cost_us_s);
#  12  GPU busy unreadable (no GPU files) and the present's driver wait held 60 ms (VKBRIDGE_TEST_PRESENT_DRIVER_MS)
#      -> verdict GPU-or-latency, never GPU; the report says "GPU-or-latency (unmeasured)";
#  13  one cluster's clock held at 65 % of its maximum (scaling_cur_freq 1300000 of 2000000) -> "cpu-cap:" line with
#      that cluster held, the other not, and the line in the report (P4);
#  14  60 fps for 10 s, then about 10 (hitch_check --slow-after) -> "perf-collapse: ... state=start" with fps under half
#      the median, and vkbridge-perf-state.txt in the session folder saying collapse=1 (P5);
#  10  cost: hitch_check --bench with the diagnostics on and off, wall time per frame (printed; on within 15%).
# Prints one line per step and "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
icd=${VKB_CLIENT_ICD:-$out/vkbridge_icd.json}
work=$(mktemp -d)
sock=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-perfdiag-$$.sock
srv=
fails=0
stop() { [ -n "$srv" ] && kill $srv 2>/dev/null && wait $srv 2>/dev/null; srv=; }
trap 'stop; rm -f "$sock"; [ -n "$KEEP" ] && echo "kept $work" || rm -rf "$work"' EXIT
# the fakes: GPU busy/clock files, two CPU clusters over this machine's CPUs at their maximum
root=$work/root
mkdir -p "$root/sys/kernel/gpu"
gpu() { echo "$1 %" >"$root/sys/kernel/gpu/gpu_busy"; echo 850000000 >"$root/sys/kernel/gpu/gpu_clock"; }
# every CPU the server may run on: nproc counts a container's CPU quota (docker --cpus 4: 4), while its threads run on
# any CPU of the machine, and one the fake does not name shows as @cpuN (ci-replay, 11-07)
ncpu=$(nproc --all); half=$((ncpu / 2)); [ $half -ge 1 ] || half=1
cpu=$work/cpufreq
mkdir -p "$cpu/policy0" "$cpu/policy$half"
echo "$(seq -s ' ' 0 $((half - 1)))" >"$cpu/policy0/related_cpus"
echo "$(seq -s ' ' $half $((ncpu - 1)))" >"$cpu/policy$half/related_cpus"
for p in policy0 policy$half; do
  echo 2000000 >"$cpu/$p/scaling_cur_freq"; echo 2000000 >"$cpu/$p/scaling_max_freq"; echo 2000000 >"$cpu/$p/cpuinfo_max_freq"
done
# start <name> [env...]: a server with its session folder $work/<name>/ (the client's frames files go there too)
start() {
  local name=$1; shift
  stop
  rm -f "$sock"
  sess=$work/$name
  mkdir -p "$sess" "$work/c"
  slog=$sess/vkbridge-server.log
  env -u VKBRIDGE_FAKE_MISSING -u VKBRIDGE_HIDE_EXTS -u VKBRIDGE_ERRATA -u VKBRIDGE_ERRATA_IMITATE -u VKBRIDGE_MODE -u VKBRIDGE_PERF_DIAG \
      -u VKBRIDGE_FAKE_DRIVER_VERSION -u VKBRIDGE_ERRATA_PROBE -u VKBRIDGE_HITCH_MS -u VKBRIDGE_HANG_DETECT -u VKBRIDGE_TEST_HITCH_DELAY_MS \
      VKBRIDGE_PROBE_BUDGET_S=0 VKBRIDGE_TEST_PERF_PERIODS=0.5,2 VKBRIDGE_GPU_SYSFS_ROOT="$root" VKBRIDGE_CPUFREQ_DIR="$cpu" "$@" \
      VK_DRIVER_FILES=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json} VK_ICD_FILENAMES= \
      "$out/vkbridge-server" --socket "$sock" --log "$slog" --cache-dir "$work/c" >/dev/null 2>&1 &
  srv=$!
  for _ in $(seq 300); do [ -S "$sock" ] && break; kill -0 $srv 2>/dev/null || break; sleep 0.05; done
}
# run [env...] -- <hitch_check args>: $res its exit status
run() {
  local e=()
  while [ "$1" != -- ]; do e+=("$1"); shift; done
  shift
  timeout 120 env -u VKBRIDGE_HITCH_MS "${e[@]}" VKBRIDGE_SOCKET="$sock" VK_DRIVER_FILES="$icd" VK_ICD_FILENAMES= \
    VKBRIDGE_CLIENT_LOG="$sess/vkbridge-client.log" VKBRIDGE_LOG_LEVEL=2 "$out/hitch_check" "$@" >"$sess/client.out" 2>&1
  res=$?
}
gone() { for _ in $(seq 100); do grep -q "client process [0-9]* gone" "$slog" && return; sleep 0.05; done; sleep 0.3; }
step() { if [ "$1" = 0 ]; then echo "ok   $2"; else echo "FAIL $2"; fails=$((fails+1)); fi; }
# the perf-threads line of the longest window: a program's last window is cut short at its exit (s=1.0 with the burner
# at 50 %), so "the last line" was a race (failed on two PCs 11-08, burner:50 vs burner:100 in the same run)
longest() { awk '{s=0; for (i=1;i<=NF;i++) if ($i ~ /^s=/) s=substr($i,3); print s "\t" $0}' | sort -s -n -k1,1 | tail -1 | cut -f2-; }
perf() { grep -o "perf: pid=[0-9]* comm=hitch_check .*" "$slog"; }
field() { grep -o " $1=[^ ]*" | cut -d= -f2; } # field <name> < lines
verdicts() { perf | field verdict | sort | uniq -c | awk '{printf "%s:%s ", $2, $1}'; }
has_verdict() { perf | grep -q " verdict=$1 "; }
dump() { perf | cut -c1-400 | sed 's/^/     /' | tail -4; grep -o "perf-threads: .*" "$slog" | cut -c1-300 | sed 's/^/     /' | tail -2; tail -2 "$sess/client.out" | sed 's/^/     /'; }

# 1 defaults, sane values
gpu 73
start base
run -- --ms 300 --steady 7000 sleep
gone; stop
lines=$(perf | wc -l)
fps_ok=$(perf | field fps | awk '$1 >= 15 && $1 <= 400 {n++} END {print n+0}')
p50_ok=$(perf | field ft_p50 | awk '$1 > 0 && $1 < 100 {n++} END {print n+0}')
gpu_ok=$(perf | grep -c " gpu_busy=73 gpu_mhz=850 ")
# present split: driver + queue + ipc + local = present (per frame, ms), within 0.3 ms or 10 %
split_ok=$(perf | awk '{for (i=1;i<=NF;i++){split($i,a,"="); v[a[1]]=a[2]} s=v["driver_ms"]+v["queue_ms"]+v["ipc_ms"]+v["local_ms"]; d=s-v["present_ms"]; if (d<0) d=-d; if (v["present_ms"]!="" && (d<=0.3 || d<=0.1*v["present_ms"])) n++} END {print n+0}')
rep=$(perf | field repeats | sort -n | tail -1)
thr=$(grep -o "perf-threads: pid=[0-9]* comm=hitch_check .* top=hitch_check:.*" "$slog" | longest)
md=$sess/malibridge-report.md
[ $res = 0 ] && [ "$lines" -ge 2 ] && [ "$fps_ok" = "$lines" ] && [ "$p50_ok" = "$lines" ] && [ "$gpu_ok" = "$lines" ] && [ "$split_ok" = "$lines" ] &&
  [ "${rep:-0}" -ge 5 ] && perf | grep -qE " verdict=(CPU|GPU|PRESENT|COMPILE|UPLOAD|THERMAL|MEMORY|NONE) " &&
  echo "$thr" | grep -qE " top=hitch_check:[0-9]+@p[0-9]" && echo "$thr" | grep -qE " others=.*server:[0-9]+" &&
  grep -q "^## Performance" "$md" && grep -qE "perf-summary: pid=[0-9]+ comm=hitch_check windows=[0-9]+ verdicts=[A-Z]+:[0-9]+" "$md" &&
  python3 -c 'import json,sys; p=json.load(open(sys.argv[1]))["performance"]; assert any(x["comm"]=="hitch_check" and x["windows"]>=2 for x in p)' "$sess/malibridge-report.json"
st=$?
step $st "1 defaults: $lines perf lines (fps ok $fps_ok, p50 ok $p50_ok, GPU 73%/850 MHz $gpu_ok, split adds up $split_ok), most repeats ${rep:-?}, verdicts $(verdicts); $(echo "$thr" | grep -o ' top=[^ ]*' | cut -c1-80)"
[ $st = 0 ] || dump
# 12 the work counts (server_conn.c vkb_srv_count_work): every line has them, and the presenting program's submits
# show (at least one a frame window-wide, as hitch_check submits each frame)
wk_ok=$(perf | grep -cE " upload_ms=[0-9]+ rpasses=[0-9]+ b2i_copies=[0-9]+ buf_creates=[0-9]+ submits=[0-9]+ hitches=")
sub_ok=$(perf | awk '{for (i=1;i<=NF;i++){split($i,a,"="); v[a[1]]=a[2]} if (v["n"] > 0 && v["submits"] >= v["n"]) k++} END {print k+0}')
[ "$lines" -ge 2 ] && [ "$wk_ok" = "$lines" ] && [ "$sub_ok" -ge 1 ]
st=$?
step $st "12 work counts on the perf line: $wk_ok of $lines lines, $sub_ok with submits >= frames; $(perf | grep -o 'rpasses=.*submits=[0-9]*' | tail -1)"
[ $st = 0 ] || dump

# 2 GPU-bound
gpu 97
start gpu
run -- --steady 5000
gone; stop
has_verdict GPU; st=$?
step $st "2 GPU busy 97%: verdicts $(verdicts)"
[ $st = 0 ] || dump

# 3 CPU-bound: a spinning game thread
gpu 10
start cpu
run -- --burn --steady 5000
gone; stop
thr=$(grep -o "perf-threads: pid=[0-9]* comm=hitch_check .*" "$slog" | longest)
has_verdict CPU && echo "$thr" | grep -qE " top=burner:(8[5-9]|9[0-9]|1[0-9][0-9])@"; st=$?
step $st "3 a spinning thread, GPU 10%: verdicts $(verdicts); $(echo "$thr" | grep -o ' top=[^,]*')"
[ $st = 0 ] || dump

# 4 thermal status 4 (the app's clock log)
start thermal
( for i in $(seq 40); do echo "12:00:0$((i % 10)) p0 2000/2000 | gpu 850 MHz | thermal 4 headroom 1.10" >>"$sess/vkbridge-clocks.log"; sleep 0.2; done ) &
feed=$!
run -- --steady 5000
gone; stop; kill $feed 2>/dev/null; wait $feed 2>/dev/null
has_verdict THERMAL && perf | grep -q " thermal=4 "; st=$?
step $st "4 thermal 4: verdicts $(verdicts)"
[ $st = 0 ] || dump

# 5 memory short
printf 'MemTotal:        7600000 kB\nMemFree:          100000 kB\nMemAvailable:     300000 kB\nSwapTotal:       4000000 kB\nSwapFree:        1000000 kB\n' >"$work/meminfo"
start mem VKBRIDGE_TEST_MEMINFO="$work/meminfo"
run -- --steady 5000
gone; stop
has_verdict MEMORY && perf | grep -q " mem_avail_mb=292 swap_used_mb=2929$"; st=$?
step $st "5 MemAvailable 300 MB: verdicts $(verdicts)"
[ $st = 0 ] || dump

# 6 a slow compile
start compile VKBRIDGE_TEST_CRASH_PIPELINE=slow VKBRIDGE_TEST_SLOW_S=1.5
run -- --steady 3000 compile
gone; stop
has_verdict COMPILE; st=$?
step $st "6 a 1.5 s compile: verdicts $(verdicts); compile_ms $(perf | field compile_ms | sort -n | tail -1)"
[ $st = 0 ] || dump

# 7 a slow BC conversion
start upload VKBRIDGE_FAKE_MISSING=bc VKBRIDGE_BC_TRANSCODE=0 VKBRIDGE_TEST_HITCH_DELAY_MS=1500
run -- --steady 3000 bc
gone; stop
has_verdict UPLOAD; st=$?
step $st "7 a 1.5 s BC conversion: verdicts $(verdicts); upload_ms $(perf | field upload_ms | sort -n | tail -1)"
[ $st = 0 ] || dump

# 8 our own queue in the present wait
start present VKBRIDGE_TEST_PRESENT_QUEUE_MS=6
run -- --steady 5000
gone; stop
q=$(perf | field queue_ms | sort -n | tail -1)
has_verdict PRESENT && awk -v q="${q:-0}" 'BEGIN {exit !(q >= 5 && q <= 9)}'; st=$?
step $st "8 the present's wait held 6 ms in the queue: verdicts $(verdicts); queue_ms $q"
[ $st = 0 ] || dump

# 9 off
start off VKBRIDGE_PERF_DIAG=0
run -- --steady 3000
gone; stop
[ $res = 0 ] && ! grep -q "perf: \|perf-threads: " "$slog" && ! grep -q "^## Performance" "$sess/malibridge-report.md"
step $? "9 VKBRIDGE_PERF_DIAG=0: no perf line, no Performance section"

# 11 the per-thread CPU evidence (the "cpu-threads:" line, server/server_cpuprof.h): a spinning thread shows run >= 80 %
# and states R; the game's own thread (it sleeps between frames) shows idle time and S; a window's line says how long the
# sampling itself took (cost_us_s) and what wchan was readable (the check accepts an unreadable wchan: Android hides it)
gpu 10
start cputh
run -- --burn --steady 5000
gone; stop
ct=$(grep -o "cpu-threads: pid=[0-9]* comm=hitch_check .*" "$slog" | longest)
echo "$ct" | grep -qE "[ =]burner\{run=(8[0-9]|9[0-9]|100) rq=[0-9]+ idle=[0-9]+ sl=[0-9]+ st=[RSDO0-9]*R[0-9]+" &&
  echo "$ct" | grep -qE "[ =]hitch_check\{run=[0-9]+ rq=[0-9]+ idle=[0-9]+ .* st=[RS0-9]*S[0-9]+" &&
  echo "$ct" | grep -qE " cost_us_s=[0-9.]+( |$)"; st=$?
step $st "11 cpu-threads: $(echo "$ct" | cut -c1-260)"
[ $st = 0 ] || { echo "$ct" | sed 's/^/     /'; dump; }

# 12 GPU busy unreadable, the driver's wait long: GPU-or-latency, not GPU
mkdir -p "$work/nogpu"
start gpulat VKBRIDGE_GPU_SYSFS_ROOT="$work/nogpu" VKBRIDGE_TEST_PRESENT_DRIVER_MS=60
run -- --steady 5000
gone; stop
d=$(perf | field driver_ms | sort -n | tail -1)
has_verdict GPU-or-latency && ! has_verdict GPU && [ "$(perf | grep -vc ' gpu_busy=? ')" = 0 ] &&
  grep -q "GPU-or-latency (unmeasured) [0-9]" "$sess/malibridge-report.md" && grep -qE "verdicts=([A-Za-z-]+:[0-9]+,)*GPU-or-latency:[0-9]+" "$sess/malibridge-report.md"
st=$?
step $st "12 GPU busy unreadable, driver wait 60 ms: verdicts $(verdicts); driver_ms $d"
[ $st = 0 ] || dump

# 13 a cluster held at 65 % of its maximum
echo 1300000 >"$cpu/policy$half/scaling_cur_freq"
gpu 50
start cpucap
run -- --steady 10000
gone; stop
echo 2000000 >"$cpu/policy$half/scaling_cur_freq"
cc=$(grep -o "cpu-cap: .*" "$slog" | tail -1)
echo "$cc" | grep -q " p$half=1300/2000MHz(65%):held top=[0-9]*%" && echo "$cc" | grep -q " p0=2000/2000MHz(100%) top=" &&
  echo "$cc" | grep -q " held=p$half$" && grep -q "^cpu-cap: samples=[0-9]* .*held=p$half$" "$sess/malibridge-report.md"
st=$?
step $st "13 a cluster held at 65 %: ${cc:-no cpu-cap line}"
[ $st = 0 ] || dump

# 14 an fps collapse
start collapse
run -- --steady 16000 --slow-after 10000
gone; stop
cl=$(grep -o "perf-collapse: pid=[0-9]* comm=hitch_check state=start .*" "$slog" | head -1)
ps=$(cat "$sess/vkbridge-perf-state.txt" 2>/dev/null)
echo "$cl" | awk '{for (i=1;i<=NF;i++){split($i,a,"="); v[a[1]]=a[2]}} END {exit !(v["fps"] > 0 && v["fps"] < 0.5 * v["fps_med"] && v["fps_med"] >= 20)}' &&
  echo "$ps" | grep -qE "^pid=[0-9]+ comm=hitch_check t=[0-9.]+ fps=[0-9.]+ fps_med=[0-9]+ collapse=1 since=[0-9.]+ windows=[0-9]+$"
st=$?
step $st "14 fps collapse: ${cl:-no perf-collapse line} | state file: ${ps:-none}"
[ $st = 0 ] || dump

# 10 cost: per-frame wall time with the diagnostics on and off (alternating, 3 each, the medians)
on=(); offv=()
for i in 1 2 3; do
  for mode in on off; do
    if [ $mode = on ]; then start bench_on; else start bench_off VKBRIDGE_PERF_DIAG=0; fi
    run -- --bench 1500 20
    stop
    v=$(grep -o "wall_us_per_frame=[0-9.]*" "$sess/client.out" | cut -d= -f2)
    if [ $mode = on ]; then on+=("${v:-0}"); else offv+=("${v:-0}"); fi
  done
done
mon=$(printf '%s\n' "${on[@]}" | sort -n | sed -n 2p); moff=$(printf '%s\n' "${offv[@]}" | sort -n | sed -n 2p)
awk -v a="$mon" -v b="$moff" 'BEGIN {exit !(a > 0 && b > 0 && a <= b * 1.15)}'
step $? "10 cost: wall per frame on ${mon} us vs off ${moff} us (medians of 3; frames of 20 round trips)"

if [ $fails = 0 ]; then echo "OK: perf lines with verdicts CPU, GPU, PRESENT, COMPILE, UPLOAD, THERMAL, MEMORY; threads and background CPU; present split; report section; off switch; cost on ${mon} us / off ${moff} us a frame"
else echo "FAILED: $fails step(s)"; exit 1; fi
