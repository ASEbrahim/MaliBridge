#!/bin/bash
# The startup probe (server_probe.c) and the session report (server_report.c), on the desktop
# driver with a server of its own (socket, cache directory, session folder), failures imitated
# with VKBRIDGE_PROBE_FAKE:
#   1  every probe passes natively: nothing switched on (the emulation mask stays as detected),
#      the device profile is written, complete;
#   2  the next start reads the profile instead of probing;
#   3  imitated failures heal themselves: a wrong divisor and a faulting push descriptor get their
#      emulations, a layer geometry stage drawn wrong gets the fold, a hanging LUT-shaped compile
#      gets the stand-in at once - checked through the bridge by divisor_check, gsfold_check (fold
#      up front) and gsfold_check --lut (stand-in texels, no timeout);
#   4  VKBRIDGE_PROBE_APPLY=0: the same failures recorded, nothing switched on (negative control);
#   5  a start that dies inside a probe: the next start records it as died and finishes the rest;
#   6  a start budget too small for the probes: the rest runs in the background, the profile
#      completes;
#   7  the session report: summary with the probe's issues, the clients (smoke, vkcube with its
#      frames), JSON that parses; the diff tool tells step 1's profile from step 3's.
# Prints one line per step and "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
tools=$PWD/../tools
work=$(mktemp -d)
sock=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-probe-$$.sock
srv=
start() { # start <cache dir> <session dir> [env...]
  local cache=$1 sess=$2
  shift 2
  stop
  rm -f "$sock"
  mkdir -p "$cache" "$sess"
  env "$@" VK_DRIVER_FILES=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json} VK_ICD_FILENAMES= VKBRIDGE_PROBE_TIMEOUT_S=1 \
    "$out/vkbridge-server" --socket "$sock" --log "$sess/vkbridge-server.log" --cache-dir "$cache" >/dev/null 2>&1 &
  srv=$!
  for _ in $(seq 150); do [ -S "$sock" ] && break; kill -0 $srv 2>/dev/null || break; sleep 0.1; done
}
stop() { [ -n "$srv" ] && kill $srv 2>/dev/null && wait $srv 2>/dev/null; srv=; }
trap 'stop; rm -f "$sock"; [ -n "$KEEP" ] && echo "kept $work" || rm -rf "$work"' EXIT
export VKBRIDGE_SOCKET=$sock VK_DRIVER_FILES=$out/vkbridge_icd.json VK_ICD_FILENAMES=
unset VKBRIDGE_HIDE_EXTS VKBRIDGE_FAKE_MISSING
fails=0
step() { if [ "$1" = 0 ]; then echo "  ok   $2"; else echo "  FAIL $2"; fails=$((fails+1)); fi; }
emu_of() { grep -m1 -o 'emulating 0x[0-9a-f]*' "$1" | tail -1 | cut -d' ' -f2; }
fake=divisor:fail,push_descriptor:crash,layer_gs:fail,compile_aces:hang

# 1
start $work/c1 $work/s1 VKBRIDGE_PROBE_BUDGET_S=0
log=$work/s1/vkbridge-server.log
prof=$(ls $work/c1/probe-*.json 2>/dev/null | head -1)
[ -S "$sock" ] && grep -q "probe: probing .* a fresh run" $log && ! grep -q "probe: emulations switched on" $log &&
  [ -n "$prof" ] && grep -q '"complete": true' "$prof" && [ "$(grep -c '"status": "pass"' "$prof")" -ge 15 ] &&
  ! grep -q '"status": "\(fail\|crash\|hang\|died\)"' "$prof"
step $? "every probe passes on the desktop driver: nothing switched on ($(emu_of $log); $(grep -c '"status": "pass"' "$prof" 2>/dev/null) passed, $(grep -m1 -o 'probe: [0-9]* ms at start' $log))"
# 2
start $work/c1 $work/s2 VKBRIDGE_PROBE_BUDGET_S=0
grep -q "probe: device profile .* not passed" $work/s2/vkbridge-server.log && ! grep -q "probe: probing" $work/s2/vkbridge-server.log
step $? "next start: the cached profile, no probing ($(grep -m1 -o 'device profile .*' $work/s2/vkbridge-server.log | cut -c1-60)...)"
# 3
start $work/c3 $work/s3 VKBRIDGE_PROBE_BUDGET_S=0 VKBRIDGE_PROBE_FAKE=$fake
log=$work/s3/vkbridge-server.log
emu=$(emu_of $log)
prof3=$(ls $work/c3/probe-*.json 2>/dev/null | head -1)
grep -q "probe: divisor fail .*vertex attribute divisor EMULATED instead" $log &&
  grep -q "probe: push_descriptor crash .*push descriptors EMULATED instead" $log &&
  grep -q '"name": "compile_aces", "status": "hang"' "$prof3" && grep -q '"name": "push_descriptor", "status": "crash"' "$prof3" &&
  [ $(( ${emu:-0} & 0x4002 )) = $((0x4002)) ]
step $? "imitated failures: divisor and push descriptors emulated (emulating $emu), the crash and the hang recorded"
timeout 60 "$out/divisor_check" >$work/3a.log 2>&1 && timeout 60 "$out/vkbridge-smoke" >>$work/3a.log 2>&1
step $? "  the emulated divisor draws right, smoke passes on emulated push descriptors ($(grep -m1 'checks, ' $work/3a.log); $(tail -1 $work/3a.log))"
timeout 60 "$out/gsfold_check" >$work/3b.log 2>&1 && grep -q "pass-through geometry stage folded into the vertex shader) for stages .*: built" $log
step $? "  layer geometry stage folded up front, picture right ($(tail -1 $work/3b.log))"
timeout 60 "$out/gsfold_check" --lut >$work/3c.log 2>&1 && grep -q "LUT pipeline built with its neutral stand-in at once" $log && ! grep -q "TIMED OUT" $log
step $? "  UE4 LUT shape: the stand-in at once, no timeout, its texels ($(tail -1 $work/3c.log))"
# 4
start $work/c4 $work/s4 VKBRIDGE_PROBE_BUDGET_S=0 VKBRIDGE_PROBE_FAKE=$fake VKBRIDGE_PROBE_APPLY=0
log=$work/s4/vkbridge-server.log
emu=$(emu_of $log)
grep -q "would be emulated (VKBRIDGE_PROBE_APPLY=0)" $log && ! grep -q "probe: emulations switched on" $log && [ $(( ${emu:-0} & 0x4002 )) = 0 ]
step $? "VKBRIDGE_PROBE_APPLY=0: recorded, nothing switched on (emulating $emu)"
# 5
start $work/c5 $work/s5 VKBRIDGE_PROBE_BUDGET_S=0 VKBRIDGE_PROBE_FAKE=waits:die
sleep 0.5
died=0; kill -0 $srv 2>/dev/null || died=1
srv=
grep -q '"running": "waits"' $work/c5/probe-*.json 2>/dev/null; marked=$?
# (each GPU dies once: a desktop with two takes three starts)
for k in 1 2 3; do
  start $work/c5 $work/s5$k VKBRIDGE_PROBE_BUDGET_S=0 VKBRIDGE_PROBE_FAKE=waits:die
  [ -S "$sock" ] && break
  srv=
done
log=$work/s5$k/vkbridge-server.log
ok=1
for f in $work/c5/probe-*.json; do grep -q '"name": "waits", "status": "died"' $f && grep -q '"complete": true' $f || ok=0; done
[ $died = 1 ] && [ $marked = 0 ] && [ -S "$sock" ] && grep -q 'died inside the "waits" probe' $work/s5*/vkbridge-server.log && [ $ok = 1 ]
step $? "a start that died inside a probe: the next records it as died and completes the profile"
# 6
start $work/c6 $work/s6 VKBRIDGE_PROBE_BUDGET_S=0.000001
log=$work/s6/vkbridge-server.log
for _ in $(seq 100); do grep -q "background probes done" $log && break; sleep 0.1; done
[ -S "$sock" ] && grep -q "probe(s) continue in the background" $log && grep -q "background probes done" $log &&
  grep -q '"complete": true' $(ls $work/c6/probe-*.json | head -1)
step $? "start budget exceeded: the rest probed in the background, profile complete"
# 7
start $work/c3 $work/s7 VKBRIDGE_PROBE_BUDGET_S=0 VKBRIDGE_PROBE_FAKE=$fake
export VKBRIDGE_CLIENT_LOG=$work/s7/vkbridge-client.log
timeout 60 "$out/vkbridge-smoke" >$work/7a.log 2>&1
vk=0
if command -v vkcube-wayland >/dev/null && [ -n "$WAYLAND_DISPLAY" ]; then timeout 30 vkcube-wayland --c 150 >/dev/null 2>&1; vk=1; fi
unset VKBRIDGE_CLIENT_LOG
for _ in $(seq 120); do grep -q '"name": "vkbridge-smoke"' $work/s7/malibridge-report.json 2>/dev/null && { [ $vk = 0 ] || grep -q 'fps_avg=[1-9]' $work/s7/malibridge-report.json; } && break; sleep 0.1; done
rep=$work/s7/malibridge-report.md
python3 -c "import json,sys; r=json.load(open(sys.argv[1])); assert r['issue_count'] >= 3 and len(r['processes']) >= 1 and r['probe'][0]['results']" \
  $work/s7/malibridge-report.json 2>$work/7j.log &&
  grep -q "^## Summary: [0-9]* issues" $rep && grep -q "probe divisor\( (GPU 0)\)\?: fail -> vertex attribute divisor emulated" $rep &&
  grep -q "probe compile_aces\( (GPU 0)\)\?: hang -> UE4 LUT pipelines get their stand-in at once" $rep && grep -q "### vkbridge-smoke" $rep &&
  grep -q "pipelines: [0-9]* built" $rep && { [ $vk = 0 ] || grep -q "frames: \`pid=.*presents=" $rep; }
step $? "session report: $(grep -m1 -o 'Summary: [0-9]* issues' $rep), $(grep -c '^### ' $rep) processes$([ $vk = 1 ] && echo ", vkcube's frames: $(grep -o 'fps_avg=[0-9.]*' $rep | head -1)")"
python3 "$tools/malibridge_report_diff.py" "$prof" "$prof3" >$work/7d.log 2>&1
grep -q "probe divisor: pass -> fail, emu 0x0 -> 0x2" $work/7d.log && grep -q "probe compile_aces: pass -> hang" $work/7d.log &&
  ! python3 "$tools/malibridge_report_diff.py" "$prof" "$prof3" --regressions >/dev/null 2>&1 &&
  python3 "$tools/malibridge_report_diff.py" $work/s7/malibridge-report.json $work/s7/malibridge-report.json | grep -q "issues: \([0-9]*\) -> \1"
step $? "report diff: the imitated failures tell profile 3 from profile 1 ($(grep -c '^probe' $work/7d.log) probe differences)"

if [ $fails = 0 ]; then echo "OK: 10 steps"; else echo "FAILED: $fails of 10 steps"; exit 1; fi
