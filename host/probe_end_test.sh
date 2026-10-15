#!/bin/bash
# How a start that ends inside a startup probe is judged (server_probe.c, judge_end), and the
# probe's cost at the default budget, on the desktop driver with a server of its own:
#   1  the default budget (6 s) and limit (4 s, 8 s for the heavy probes): the heavy probes never
#      fit, so they run after the server listens; the start and the background part are logged
#      ("probe: N ms at start", "background probes done (B ms background, T ms in all)"), and a
#      start from the cached profile logs its own cost ("probe: cached profile, N ms at start");
#   2  the session stopped (SIGTERM) inside a background probe: the next start runs it again and
#      decides nothing from it;
#   3  the server killed (SIGKILL, as the app ends a session) inside a background probe: the same,
#      counted as 1 of 3 ends without a word;
#   4  an abort inside a probe (VKBRIDGE_PROBE_FAKE=compile_aces:abort): died at the next start,
#      its decision applied (the LUT stand-in at once);
#   5  a hang the watchdog cannot interrupt (compile_aces:stall), ended by a kill: died at the
#      next start, its decision applied;
#   6  the process stopped (SIGSTOP, a suspended session) inside a background probe until its
#      watchdog fires: not counted as a hang, run again at the next start.
# Prints one line per step and "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
work=$(mktemp -d)
sock=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-probe-end-$$.sock
srv=
wait_n=150
start() { # start <cache dir> <session dir> [env...]
  local cache=$1 sess=$2
  shift 2
  stop
  rm -f "$sock"
  mkdir -p "$cache" "$sess"
  env "$@" VK_DRIVER_FILES=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json} VK_ICD_FILENAMES= \
    "$out/vkbridge-server" --socket "$sock" --log "$sess/vkbridge-server.log" --cache-dir "$cache" >/dev/null 2>&1 &
  srv=$!
  for _ in $(seq $wait_n); do [ -S "$sock" ] && break; kill -0 $srv 2>/dev/null || break; sleep 0.1; done
}
stop() { [ -n "$srv" ] && kill $srv 2>/dev/null && wait $srv 2>/dev/null; srv=; }
killsrv() { [ -n "$srv" ] && kill -"$1" $srv 2>/dev/null; wait $srv 2>/dev/null; srv=; }
waitfor() { # waitfor <pattern> <file> [tenths]
  for _ in $(seq ${3:-200}); do grep -q "$1" "$2" 2>/dev/null && return 0; sleep 0.1; done
  return 1
}
trap 'stop; rm -f "$sock"; [ -n "$KEEP" ] && echo "kept $work" || rm -rf "$work"' EXIT
ulimit -c 0
unset VKBRIDGE_HIDE_EXTS VKBRIDGE_FAKE_MISSING VKBRIDGE_PROBE_BUDGET_S VKBRIDGE_PROBE_TIMEOUT_S
fails=0
step() { if [ "$1" = 0 ]; then echo "  ok   $2"; else echo "  FAIL $2"; fails=$((fails+1)); fi; }
prof() { ls $1/probe-*.json 2>/dev/null | head -1; }
# the line number of the first match (999999 when none)
at() { local n; n=$(grep -n -m1 "$1" "$2" | cut -d: -f1); echo ${n:-999999}; }

# 1  the defaults
start $work/c1 $work/s1
log=$work/s1/vkbridge-server.log
waitfor "background probes done" $log
ms=$(grep -m1 -o 'probe: [0-9]* ms at start' $log | grep -o '[0-9]*')
[ -S "$sock" ] && grep -q "probe(s) continue in the background" $log && [ -n "$ms" ] && [ "$ms" -lt 6000 ] &&
  [ $(at "listening on" $log) -lt $(at "probe: compile_lut_standin " $log) ] && [ $(at "listening on" $log) -lt $(at "probe: compile_aces " $log) ] &&
  grep -q "probe: background probes done ([0-9]* ms background, [0-9]* ms in all); runtime flags" $log &&
  grep -q '"complete": true' "$(prof $work/c1)"
step $? "default budget: the heavy probes run after the server listens ($(grep -m1 -o 'probe: [0-9]* ms at start' $log); $(grep -m1 -o 'background probes done ([^)]*)' $log))"
start $work/c1 $work/s1b
log=$work/s1b/vkbridge-server.log
grep -q "probe: cached profile, [0-9.]* ms at start" $log && ! grep -q "probe: probing" $log
step $? "cached profile: its start cost logged ($(grep -m1 -o 'cached profile, [0-9.]* ms at start' $log))"

# 2, 3  a session that ends inside a background probe (made 3 s longer to end it there)
slow="VKBRIDGE_PROBE_FAKE=compile_aces:slow"
for sig in TERM KILL; do
  c=$work/c-$sig
  start $c $work/s-$sig $slow
  log=$work/s-$sig/vkbridge-server.log
  waitfor "compile_aces takes 3 s longer on purpose" $log 300 && sleep 0.5
  inside=0; grep -q '"running": "compile_aces"' "$(prof $c)" && inside=1
  killsrv $sig 2>/dev/null
  mark=$(tail -c +65 $(ls $c/probe-*.json.beat | head -1) 2>/dev/null | tr -d '\0\n' | cut -c1-20)
  start $c $work/s-$sig-2 $slow
  log2=$work/s-$sig-2/vkbridge-server.log
  waitfor "background probes done" $log2 300
  p=$(prof $c)
  if [ $sig = TERM ]; then
    want='was stopped inside the "compile_aces" probe (stopped after [0-9]* ms (Terminated)); running it again, nothing decided from it'
  else
    want='ended inside the "compile_aces" probe (ended after [0-9]* ms without a word.*: 1 of 3 in a row before it counts as died); running it again'
  fi
  [ $inside = 1 ] && grep -q "$want" $log2 && grep -q '"name": "compile_aces", "status": "pass"' "$p" && grep -q '"complete": true' "$p" &&
    ! grep -q '"status": "died"' "$p" && ! grep -q "UE4 LUT pipelines get" $log2 && ! grep -q "runtime flags 0x[1-9]" $log2
  step $? "SIG$sig inside a background probe: run again, nothing decided (mark: ${mark:-none}; $(grep -m1 -o 'compile_aces" probe ([^;]*' $log2 | cut -c22-100))"
done

# 4, 5  a real abort, a hang past the watchdog: died, decided (each GPU ends there once)
for kind in abort stall; do
  c=$work/c-$kind
  for k in 1 2 3 4; do
    [ $kind = stall ] && wait_n=60
    start $c $work/s-$kind$k VKBRIDGE_PROBE_BUDGET_S=0 VKBRIDGE_PROBE_TIMEOUT_S=1 VKBRIDGE_PROBE_FAKE=compile_aces:$kind
    wait_n=150
    [ -S "$sock" ] && break
    killsrv KILL # the app's kill after its 15 s for the socket (here 6 s, past the 2 s limit + 2 s)
  done
  logs=$(ls $work/s-$kind*/vkbridge-server.log)
  log=$work/s-$kind$k/vkbridge-server.log
  p=$(prof $c)
  if [ $kind = abort ]; then why='the process ended with signal 6'; else why='the watchdog could not interrupt it'; fi
  [ $k -gt 1 ] && [ -S "$sock" ] && grep -q "died inside the \"compile_aces\" probe (.*$why" $logs &&
    grep -q "probe: compile_aces died (.*UE4 LUT pipelines get their stand-in at once" $log && grep -q "probe: runtime flags 0x1 from the probe" $log &&
    grep -q '"name": "compile_aces", "status": "died"' "$p" && grep -q '"complete": true' "$p"
  step $? "a real $kind inside a probe: died at the next start, the LUT stand-in decided ($k starts)"
done

# 6  stopped past the limit (8 s): 10 s stopped
c=$work/c-stop
start $c $work/s-stop $slow
log=$work/s-stop/vkbridge-server.log
waitfor "compile_aces takes 3 s longer on purpose" $log 300
kill -STOP $srv; sleep 10; kill -CONT $srv
waitfor "background probes done" $log 200
start $c $work/s-stop2 $slow
waitfor "background probes done" $work/s-stop2/vkbridge-server.log 300
p=$(prof $c)
grep -q "probe: compile_aces reached its time limit after [0-9.]* s, [0-9.]* s of them with the process stopped; not counted" $log &&
  grep -q "background probes done (.*); some left for the next start" $log && grep -q "probe: probing .* continuing an earlier run" $work/s-stop2/vkbridge-server.log &&
  grep -q '"name": "compile_aces", "status": "pass"' "$p" && grep -q '"complete": true' "$p" && ! grep -q "UE4 LUT pipelines get" $log $work/s-stop2/vkbridge-server.log
step $? "stopped inside a probe past its limit: not a hang, run again at the next start ($(grep -m1 -o 'after [0-9.]* s, [0-9.]* s of them with the process stopped' $log))"

if [ $fails = 0 ]; then echo "OK: 7 steps"; else echo "FAILED: $fails of 7 steps"; exit 1; fi
