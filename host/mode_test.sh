#!/bin/bash
# Safe mode and performance mode (server_mode.c) on the desktop driver, with a server of its own
# per step (socket, cache directory, session folder):
#   1  no switch: safe mode (the default) - in the log with its source, the compile watchdog at
#      10 s, the session report's first line and its JSON;
#   2  <cache dir>/mode = performance (DroidDeck's Setup switch): performance mode (Setup), the
#      watchdog at 20 s, the report's first line; VKBRIDGE_COMPILE_TIMEOUT_S still wins;
#   3  precedence: VKBRIDGE_MODE in the environment over the file, VKBRIDGE_MODE in <cache dir>/env
#      over both; an unknown value is safe with a warning;
#   4  probes not run yet (a start budget too small for them): safe mode emulates every
#      probe-gated feature the GPU has, performance mode none; both masks from one start, the one
#      in force picked per device - the mode file switched under the running server changes the
#      next device's emulation and errata, no restart, no re-probe; the background part verifies
#      the native paths (the probes see performance mode's mask, so they run instead of skipping),
#      and the next start's safe mode is then native;
#   5  imitated probe results (VKBRIDGE_PROBE_FAKE): a probe that ended in error keeps its
#      emulation in safe mode only, a failed one gets it in both; the report's probe table says
#      why, and its JSON carries both masks;
#   6  the client's mode (the guest's VKBRIDGE_MODE): descriptor batching off in safe mode, on in performance mode;
#   7  the report diff tool warns first when two reports differ in mode;
#   8  the protocol hash is unchanged.
# Prints one line per step and "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
work=$(mktemp -d)
sock=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-mode-$$.sock
srv=
log=
stop() { [ -n "$srv" ] && kill $srv 2>/dev/null && wait $srv 2>/dev/null; srv=; }
# start <name> [env...]: a server of its own, cache $work/<name>/c (kept between starts of one
# name), log $log (new each start)
start() {
  local d=$work/$1
  shift
  stop
  rm -f "$sock"
  mkdir -p "$d/c" "$d/s"
  log=$d/s/vkbridge-server.log
  : > "$log"
  rm -f "$d"/s/malibridge-report.*
  env -u VKBRIDGE_FAKE_MISSING -u VKBRIDGE_HIDE_EXTS -u VKBRIDGE_ERRATA -u VKBRIDGE_ERRATA_IMITATE -u VKBRIDGE_ERRATA_IMITATE_VB \
      -u VKBRIDGE_MODE -u VKBRIDGE_FAKE_DRIVER_VERSION -u VKBRIDGE_ERRATA_PROBE -u VKBRIDGE_PROBE_FAKE -u VKBRIDGE_WAIT_FAKE_ERROR \
      -u VKBRIDGE_COMPILE_TIMEOUT_S -u VKBRIDGE_COMPILE_WARN_S -u VKBRIDGE_EMULATE -u VKBRIDGE_PROBE \
      VKBRIDGE_PROBE_BUDGET_S=0 "$@" VK_DRIVER_FILES=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json} VK_ICD_FILENAMES= \
      "$out/vkbridge-server" --socket "$sock" --log "$log" --cache-dir "$d/c" >/dev/null 2>&1 &
  srv=$!
  for _ in $(seq 300); do [ -S "$sock" ] && break; kill -0 $srv 2>/dev/null || break; sleep 0.05; done
}
# A device (the smoke test); $new: the log lines it added
device() {
  local l; l=$(wc -l < "$log")
  timeout 120 env "$@" "$out/vkbridge-smoke" >"$work/smoke.log" 2>&1
  new=$(tail -n +$((l+1)) "$log")
}
report() { # report <name>: waits for the session report's Markdown; prints its path
  local r=$work/$1/s/malibridge-report.md
  for _ in $(seq 100); do grep -q "^## Startup probe" "$r" 2>/dev/null && break; sleep 0.1; done
  echo "$r"
}
emu_of() { echo "$new" | grep -o 'device [0-9]*: emulation 0x[0-9a-f]* installed' | tail -1 | grep -o '0x[0-9a-f]*'; }
on_set() { echo "$new" | grep -o 'errata: device .*: on [^;]*' | tail -1 | sed 's/.*: on //'; }
prof() { ls "$work/$1"/c/probe-*.json 2>/dev/null | head -1; }
status_of() { cat "$work/$1"/c/probe-*.json 2>/dev/null | grep -o "\"name\": \"$2\", \"status\": \"[a-z]*\"" | sed 's/.*"status": "//; s/"//' | sort -u | tr '\n' ' ' | sed 's/ $//'; }
trap 'stop; rm -f "$sock"; [ -n "$KEEP" ] && echo "kept $work" || rm -rf "$work"' EXIT
export VKBRIDGE_SOCKET=$sock VK_DRIVER_FILES=$out/vkbridge_icd.json VK_ICD_FILENAMES=
unset VKBRIDGE_HIDE_EXTS VKBRIDGE_MAX_API VKBRIDGE_FAKE_MISSING VKBRIDGE_MODE VKBRIDGE_DESC_BATCH VKBRIDGE_NO_DESC_BATCH
fails=0 steps=0
step() { steps=$((steps+1)); if [ "$1" = 0 ]; then echo "  ok   $2"; else echo "  FAIL $2"; fails=$((fails+1)); fi; }

# 1
start def
device
rep=$(report def)
grep -q "mode: safe (the default)" $log && grep -q "compile watchdog: warn after 5 s, interrupt after 10 s (safe mode" $log &&
  head -1 $rep | grep -q "^Mode: safe (the default) - .*compile watchdog 10 s (safe mode)\.$" && sed -n 3p $rep | grep -q "^# MaliBridge session report" &&
  python3 -c 'import json,sys; m=json.load(open(sys.argv[1]))["mode"]; assert m["value"]=="safe" and m["source"]=="the default" and m["compile_timeout_s"]==10, m' \
    ${rep%.md}.json
step $? "no switch: $(head -1 $rep | cut -c1-60)..."

# 2
mkdir -p $work/file/c && echo performance > $work/file/c/mode
start file
device
rep=$(report file)
grep -q "mode: performance (Setup)" $log && grep -q "interrupt after 20 s (performance mode" $log &&
  head -1 $rep | grep -q "^Mode: performance (Setup) - .*compile watchdog 20 s (performance mode)\.$" &&
  echo "$new" | grep -q "errata: device .*, performance mode: on"
r1=$?
start file VKBRIDGE_COMPILE_TIMEOUT_S=7
device
[ $r1 = 0 ] && grep -q "interrupt after 7 s (VKBRIDGE_COMPILE_TIMEOUT_S" $log
step $? "<cache dir>/mode = performance: $(grep -o 'mode: performance (Setup)' $log | head -1), watchdog 20 s, VKBRIDGE_COMPILE_TIMEOUT_S=7 wins"

# 3
start file VKBRIDGE_MODE=safe
device
grep -q "mode: safe (VKBRIDGE_MODE)" $log; r1=$?
echo "VKBRIDGE_MODE=performance" > $work/file/c/env
start file VKBRIDGE_MODE=safe
device
grep -q "mode: performance (VKBRIDGE_MODE in <cache dir>/env)" $log; r2=$?
rm -f $work/file/c/env
echo turbo > $work/file/c/mode
start file
device
grep -q 'mode: <cache dir>/mode\|mode: .*/mode is "turbo", neither safe nor performance; safe' $log && grep -q "mode: safe (Setup)" $log; r3=$?
[ $r1$r2$r3 = 000 ]
step $? "precedence: environment over the file ($r1), <cache dir>/env over both ($r2), \"turbo\" is safe with a warning ($r3)"

# 4
start defer VKBRIDGE_PROBE_BUDGET_S=0.0001
device
safe_emu=$(emu_of); safe_on=$(on_set)
grep -q "probe: divisor not run yet: vertex attribute divisor EMULATED until it passes (safe mode)" $log &&
  grep -q "probe: push_descriptor not run yet: push descriptors EMULATED until it passes (safe mode)" $log &&
  grep -q "probe: performance mode would emulate" $log
r1=$?
echo performance > $work/defer/c/mode
device
perf_emu=$(emu_of); perf_on=$(on_set)
echo "$new" | grep -q "mode: performance (Setup) - changed" && echo "$new" | grep -q "performance mode: emulating" &&
  [ -n "$safe_emu" ] && [ "$safe_emu" != "${perf_emu:-0x0}" ] && [ "$safe_on" != "$perf_on" ] && ! echo "$new" | grep -q "probe: probing"
r2=$?
rm -f $work/defer/c/mode
device
back_emu=$(emu_of)
[ "$back_emu" = "$safe_emu" ]; r3=$?
for _ in $(seq 300); do grep -q "background probes done" $log && break; sleep 0.1; done
st=$(status_of defer divisor)/$(status_of defer push_descriptor)/$(status_of defer null_descriptor)
start defer
device
grep -q "probe: device profile" $log && ! grep -q "not run yet" $log && [ "$(emu_of)" = "${perf_emu}" ] && [ $st = pass/pass/pass ]; r4=$?
[ $r1$r2$r3$r4 = 0000 ]
step $? "not run yet: safe emulates $safe_emu, performance ${perf_emu:-none} (switched under the running server: $r2, back: $r3); verified in the background ($st), next start's safe mode ${perf_emu:-native} ($r4)"

# 5
start fake VKBRIDGE_PROBE_FAKE=push_descriptor:error,divisor:fail
device
s_emu=$(emu_of)
rep=$(report fake)
grep -q "probe: push_descriptor error (could not set up \[imitated: VKBRIDGE_PROBE_FAKE\]): push descriptors EMULATED instead of the driver's (not verified: safe mode)" $log &&
  grep -q "| push_descriptor | \*\*error\*\* | [0-9.]* | emulate push descriptors (0x4000): not verified, safe mode |" $rep &&
  grep -q "| divisor | \*\*fail\*\* | [0-9.]* | emulate vertex attribute divisor (0x2) |" $rep &&
  python3 -c '
import json, sys
p = json.load(open(sys.argv[1]))["probe"][0]
s, f = int(p["emu_safe"], 16), int(p["emu_performance"], 16)
assert s & 0x4000 and not f & 0x4000 and s & 2 and f & 2, (hex(s), hex(f))' ${rep%.md}.json
r1=$?
start fake VKBRIDGE_PROBE_FAKE=push_descriptor:error,divisor:fail VKBRIDGE_MODE=performance
device
p_emu=$(emu_of)
grep -q "probe: device profile" $log && [ $(( p_emu & 0x4000 )) = 0 ] && [ $(( s_emu & 0x4000 )) != 0 ] && [ $(( p_emu & 2 )) != 0 ]
r2=$?
[ $r1$r2 = 00 ]
step $? "imitated results: push_descriptor error -> emulated in safe mode only ($s_emu / $p_emu), divisor fail -> both; the report says why"

# 6
start client
msgs() { VKBRIDGE_STATS=100000 timeout 120 env "$@" "$out/desc_traffic_check" 2>&1 | grep -o '[0-9]* messages' | head -1; }
ms=$(msgs VKBRIDGE_MODE=safe); mp=$(msgs VKBRIDGE_MODE=performance); mo=$(msgs VKBRIDGE_MODE=performance VKBRIDGE_NO_DESC_BATCH=1)
[ -n "$ms" ] && [ "$ms" = "$mo" ] && [ "$ms" != "$mp" ]
step $? "client: descriptor batching off in safe ($ms), on in performance ($mp), VKBRIDGE_NO_DESC_BATCH=1 wins ($mo)"

# 7
stop
d=$(python3 ../tools/malibridge_report_diff.py $work/def/s/malibridge-report.json $work/defer/s/malibridge-report.json 2>&1)
! echo "$d" | grep -q "WARNING: the reports differ in mode"
r1=$?
d2=$(python3 ../tools/malibridge_report_diff.py $work/defer/s/malibridge-report.json $work/fake/s/malibridge-report.json 2>&1)
[ $r1 = 0 ] && echo "$d2" | sed -n 3p | grep -q "^WARNING: the reports differ in mode: safe (the default) -> performance (VKBRIDGE_MODE)"
step $? "the diff tool: $(echo "$d2" | grep -m1 '^WARNING' | cut -c1-90)"

# 8
grep -q "^#define VKB_PROTOCOL_HASH 0x4a9da939fdf65896ULL" ../common/gen/vkb_gen.h
step $? "protocol hash $(grep -o '0x[0-9a-f]*' ../common/gen/vkb_gen.h | head -1)"

if [ $fails = 0 ]; then echo "OK: $steps steps"; else echo "FAILED: $fails of $steps steps"; exit 1; fi
