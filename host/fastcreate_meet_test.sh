#!/bin/bash
# Where fast create (VKBRIDGE_FAST_CREATE, server_conn.c) meets the test46 server changes that also sit on the create /
# destroy / memory paths: PowerVR's leftover tracking and destroy (server_children.c, a player log), the GPU hang detector the
# fault-arm turns on (server_hang.c, a player log) and import_serial (server_errata_import.c, G615 MC6 r44p1, a player log).
# Fast create moves only WHEN a create's handler runs (ahead of other threads' queued work); every server layer must still
# see and record it. Runs, each with the switch on (default) and off:
#   1/2  pvrdestroy_test.sh (7 steps)                      FAST_CREATE=1 / =0
#   3/4  pvrdestroy_test.sh with VKBRIDGE_HANG_DETECT=1    FAST_CREATE=1 / =0 (as pvrhang_test.sh)
#   5    importrace_test.sh (5 steps)                      FAST_CREATE=1
# and checks: every run OK; in the =1 runs the servers said 'fast create: on' and creates ran ahead of queued work (so the
# fast path was taken under the PowerVR imitation / the detector / the import race), in the =0 runs 'fast create: off'
# and none ran ahead; the leftovers the PowerVR sweep names ('destroyed first: ...', 'left alive by its process ...')
# are the same per step with the switch on and off; with the detector on it is on in every server but step 0's; no
# crash-like line anywhere. One line per check, "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
fails=0 steps=0
step() { steps=$((steps+1)); if [ "$1" = 0 ]; then echo "  ok   $2"; else echo "  FAIL $2"; fails=$((fails+1)); fi; }
# run <tag> <script> <timeout> [VAR=value ...]: the script's kept work dir in $work/<tag>.dir
run() {
  local tag=$1 script=$2 to=$3; shift 3
  env -u VKBRIDGE_FAST_CREATE -u VKBRIDGE_HANG_DETECT KEEP=1 "$@" timeout "$to" "./$script" >"$work/$tag.txt" 2>&1
  local rc=$?
  local kept; kept=$(sed -n 's/^kept //p' "$work/$tag.txt" | tail -1)
  [ -n "$kept" ] && [ -d "$kept" ] && mv "$kept" "$work/$tag.dir"
  return $rc
}
# servers' logs of a run (pvrdestroy: <step>/server.log; importrace: <name>.log)
slogs() { find "$work/$1.dir" -maxdepth 2 \( -name server.log -o -name '*.log' \) -type f 2>/dev/null | sort; }
ahead() { cat $(slogs "$1") 2>/dev/null | grep -o 'fast create on, so far: [0-9]* image creates: [0-9]* ran ahead' | awk '{s+=$(NF-2)} END {print s+0}'; }
bufahead() { cat $(slogs "$1") 2>/dev/null | grep -o '[0-9]* buffer creates, [0-9]* ran ahead' | awk '{s+=$4} END {print s+0}'; }
said() { grep -l "$2" $(slogs "$1") 2>/dev/null | wc -l; } # servers whose log has the line
last() { grep -v '^kept ' "$work/$1.txt" | tail -1; }
# per step dir: the PowerVR sweep's sentences (handles removed so on and off compare)
sweep() { for d in "$work/$1.dir"/*/; do [ -f "$d/server.log" ] || continue; printf '%s: ' "$(basename "$d")";
  grep -o "destroyed first:.*\|left alive by its process.*" "$d/server.log" | sed 's/0x[0-9a-f]*//g; s/[0-9]\{3,\}//g' | sort | tr '\n' '|'; echo; done; }
crash() { grep -liE "SIGSEGV|SIGABRT|double free|use after free" $(slogs "$1") 2>/dev/null | wc -l; }

run pvr1 pvrdestroy_test.sh 280 VKBRIDGE_FAST_CREATE=1; r1=$?
run pvr0 pvrdestroy_test.sh 280 VKBRIDGE_FAST_CREATE=0; r0=$?
step $(( r1 || r0 )) "pvrdestroy_test: FAST_CREATE=1 '$(last pvr1)' rc $r1; =0 '$(last pvr0)' rc $r0"
a1=$(ahead pvr1) b1=$(bufahead pvr1) a0=$(ahead pvr0) b0=$(bufahead pvr0)
[ "$(said pvr1 'fast create: on')" -ge 1 ] && [ $((a1 + b1)) -ge 1 ] && [ "$(said pvr0 'fast create: off')" -ge 1 ] && [ $((a0 + b0)) = 0 ] && [ "$(said pvr0 'fast create: on')" = 0 ]
step $? "PowerVR imitation: on: $a1 image + $b1 buffer creates ran ahead ('fast create: on' in $(said pvr1 'fast create: on') servers); off: $a0 + $b0 ran ahead ('fast create: off' in $(said pvr0 'fast create: off'))"
s1=$(sweep pvr1) s0=$(sweep pvr0)
[ -n "$s1" ] && [ "$s1" = "$s0" ] && echo "$s1" | grep -q "image(s)"
step $? "the PowerVR sweep names the same leftovers with fast create on and off"
[ "$s1" = "$s0" ] || { echo "    on:  $s1" | head -12; echo "    off: $s0" | head -12; }

run hang1 pvrdestroy_test.sh 280 VKBRIDGE_FAST_CREATE=1 VKBRIDGE_HANG_DETECT=1; h1=$?
run hang0 pvrdestroy_test.sh 280 VKBRIDGE_FAST_CREATE=0 VKBRIDGE_HANG_DETECT=1; h0=$?
step $(( h1 || h0 )) "pvrdestroy_test with the hang detector on: FAST_CREATE=1 '$(last hang1)' rc $h1; =0 '$(last hang0)' rc $h0"
n1=$(slogs hang1 | wc -l) d1=$(said hang1 'GPU hang detector on') n0=$(slogs hang0 | wc -l) d0=$(said hang0 'GPU hang detector on')
ha=$(( $(ahead hang1) + $(bufahead hang1) ))
[ $n1 -ge 7 ] && [ $d1 -ge $((n1-1)) ] && [ $n0 -ge 7 ] && [ $d0 -ge $((n0-1)) ] && [ $ha -ge 1 ] && [ "$(sweep hang1)" = "$(sweep hang0)" ] && [ "$(sweep hang1)" = "$s1" ]
step $? "detector on in $d1 of $n1 servers (on) / $d0 of $n0 (off); $ha creates ran ahead with it on; sweep the same on/off and as without the detector"

run imp1 importrace_test.sh 600 VKBRIDGE_FAST_CREATE=1; i1=$?
ia=$(( $(ahead imp1) + $(bufahead imp1) ))
[ $i1 = 0 ] && [ "$(said imp1 'fast create: on')" -ge 1 ]
step $? "importrace_test with FAST_CREATE=1: '$(last imp1)' rc $i1; 'fast create: on' in $(said imp1 'fast create: on') servers, $ia creates ran ahead"

c=$(( $(crash pvr1) + $(crash pvr0) + $(crash hang1) + $(crash hang0) ))
[ $c = 0 ]
step $? "no crash-like line in any PowerVR/detector server log ($c)"

if [ $fails = 0 ]; then echo "OK: $steps checks (fast create with PowerVR's sweep, the hang detector, import_serial)"; else
  for t in pvr1 pvr0 hang1 hang0 imp1; do echo "--- $t"; sed 's/^/    /' "$work/$t.txt" | tail -12; done
  echo "FAILED: $fails of $steps checks"; exit 1; fi
