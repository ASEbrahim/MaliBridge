#!/bin/bash
# The background startup probes at the lowest CPU priority (server_probe.c bg_lowest_priority), on the desktop driver with a
# server of its own:
#   1  the background probes' thread ("vkb-probe-bg") read back from /proc while it runs: SCHED_IDLE (policy 5) and nice 19,
#      the line saying so; once they are done no other thread of the server is left at either (nothing inherited it: none
#      at SCHED_IDLE or niced past the main thread, which a build wrapper may run at nice 19 already);
#   2  the server on one CPU with six busy loops beside it, compile_aces spinning (VKBRIDGE_PROBE_FAKE=compile_aces:hang): it
#      reaches its time limit while it mostly waited for a CPU, so it is not counted ("waiting for a CPU; not counted, run
#      again at the next start"), nothing recorded as a hang; the next start, the CPU free and no fake, runs it and it passes;
#   3  the same spin with the CPU free is a real hang: still recorded as one.
# Prints one line per step and "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
work=$(mktemp -d)
sock=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-probe-prio-$$.sock
srv=
busy=
start() { # start <cache dir> <session dir> [env...]; PIN=<cpu> runs it on that CPU only
  local cache=$1 sess=$2
  shift 2
  stop
  rm -f "$sock"
  mkdir -p "$cache" "$sess"
  ${PIN:+taskset -c $PIN} env "$@" VK_DRIVER_FILES=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json} VK_ICD_FILENAMES= \
    "$out/vkbridge-server" --socket "$sock" --log "$sess/vkbridge-server.log" --cache-dir "$cache" >/dev/null 2>&1 &
  srv=$!
  for _ in $(seq 300); do [ -S "$sock" ] && break; kill -0 $srv 2>/dev/null || break; sleep 0.1; done
}
stop() { [ -n "$srv" ] && kill $srv 2>/dev/null && wait $srv 2>/dev/null; srv=; }
unbusy() { for p in $busy; do kill $p 2>/dev/null; wait $p 2>/dev/null; done; busy=; }
waitfor() { # waitfor <pattern> <file> [tenths]
  for _ in $(seq ${3:-200}); do grep -q "$1" "$2" 2>/dev/null && return 0; sleep 0.1; done
  return 1
}
waitdone() { # every GPU's background probes done
  local n
  for _ in $(seq ${2:-300}); do
    n=$(grep -c "probe(s) continue in the background" "$1" 2>/dev/null)
    [ "${n:-0}" -ge 1 ] && [ "$(grep -c 'probe: background probes done' "$1")" -ge "$n" ] && return 0
    sleep 0.1
  done
  return 1
}
# stat <task dir>: "<nice> <policy>" (fields 19 and 41 of /proc/.../stat, counted after the name in parentheses)
stat_of() { sed 's/^.*) //' "$1/stat" 2>/dev/null | awk '{print $17, $39}'; }
trap 'unbusy; stop; rm -f "$sock"; [ -n "$KEEP" ] && echo "kept $work" || rm -rf "$work"' EXIT
ulimit -c 0
unset VKBRIDGE_HIDE_EXTS VKBRIDGE_FAKE_MISSING VKBRIDGE_PROBE_BUDGET_S VKBRIDGE_PROBE_TIMEOUT_S VKBRIDGE_PROBE_FAKE
fails=0
step() { if [ "$1" = 0 ]; then echo "  ok   $2"; else echo "  FAIL $2"; fails=$((fails+1)); fi; }
prof() { ls $1/probe-*.json 2>/dev/null | head -1; }

# 1  read back while compile_aces runs (made 3 s longer in the background)
start $work/c1 $work/s1 VKBRIDGE_PROBE_FAKE=compile_aces:slow
log=$work/s1/vkbridge-server.log
waitfor "compile_aces takes 3 s longer on purpose" $log 600
seen=
for t in /proc/$srv/task/*; do [ "$(cat $t/comm 2>/dev/null)" = vkb-probe-bg ] && seen=$(stat_of $t); done
waitdone $log 600
left=
base=$(stat_of /proc/$srv/task/$srv | cut -d' ' -f1)
for t in /proc/$srv/task/*; do
  s=$(stat_of $t)
  { [ "${s#* }" = 5 ] || [ "${s%% *}" -gt "${base:-0}" ]; } 2>/dev/null && left="$left $(cat $t/comm 2>/dev/null)($s)"
done
[ "$seen" = "19 5" ] && grep -q "probe: background probes at the lowest CPU priority (SCHED_IDLE, nice 19)" $log && [ -z "$left" ]
step $? "the background probes' thread at the lowest priority (nice, policy read back: ${seen:-no thread}; others left at it:${left:- none}; $(grep -m1 -o 'background probes at [^,]*' $log))"

# 2  starved on a busy CPU: not counted, run again; then passes with the CPU free
cpu=$(taskset -pc $$ 2>/dev/null | sed 's/.*: //; s/[,-].*//')
cpu=${cpu:-0}
for _ in 1 2 3 4 5 6; do taskset -c $cpu bash -c 'while :; do :; done' & busy="$busy $!"; done
PIN=$cpu start $work/c2 $work/s2 VKBRIDGE_PROBE_FAKE=compile_aces:hang VKBRIDGE_PROBE_TIMEOUT_S=2 VKBRIDGE_PROBE_TX_TIMEOUT_S=2
log=$work/s2/vkbridge-server.log
waitdone $log 1800
unbusy
p=$(prof $work/c2)
starved=$(grep -c "compile_aces reached its time limit after [0-9.]* s, [0-9.]* s of them waiting for a CPU; not counted, run again at the next start, with the probes after it" $log)
busyhang=$(grep -o '"name": "[a-z_]*", "status": "hang"' "$p" 2>/dev/null | tr '\n' ' ')
[ "$starved" -ge 1 ] && [ -z "$busyhang" ]
r2=$?
start $work/c2 $work/s2b
log2=$work/s2b/vkbridge-server.log
waitdone $log2 600
p=$(prof $work/c2)
[ $r2 = 0 ] && grep -q '"complete": true' "$p" && ! grep -q '"status": "hang"' "$p"
step $? "starved on a busy CPU: $starved probe(s) not counted, none recorded as a hang (busy start ok: $([ $r2 = 0 ] && echo yes || echo "no: ${busyhang:-no line}")); run again with the CPU free, the profile complete ($(grep -m1 -o '[a-z_]* reached its time limit after [0-9.]* s, [0-9.]* s of them waiting' $log); hangs: $(grep -o '"name": "[a-z_]*", "status": "hang"' "$p" | tr '\n' ' ')$(grep -c '"complete": true' "$p") complete)"

# 3  the same spin with the CPU free is a real hang: still one
start $work/c3 $work/s3 VKBRIDGE_PROBE_FAKE=compile_aces:hang VKBRIDGE_PROBE_TIMEOUT_S=2
log=$work/s3/vkbridge-server.log
waitdone $log 600
grep -q '"name": "compile_aces", "status": "hang"' "$(prof $work/c3)" && ! grep -q "compile_aces reached its time limit.*waiting for a CPU" $log
step $? "a real hang (the CPU free) still recorded as one"

stop
[ $fails = 0 ] && echo "OK: background probes at the lowest priority, starved ones run again, hangs still hangs" || echo "FAILED: $fails step(s)"
[ $fails = 0 ]
