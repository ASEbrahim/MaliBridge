#!/bin/bash
# The MK probes (server_probe.c: one per Mali driver erratum a probe can tell) and the errata
# decisions they feed (server_errata.c), on the desktop driver with a server of its own per step
# (socket, cache directory, session folder):
#   1  every MK probe passes natively; the profile is PROBE_VERSION 3 with the sub-cases recorded;
#      safe mode: the always-on errata on, each with its probe's pass in the reason; the report
#      lists every erratum with its probe result and both modes' decisions (Markdown and JSON);
#   2  performance mode from the same profile (no re-probe): the probe-gated errata stay off;
#   3  each erratum's bug imitated (VKBRIDGE_ERRATA_IMITATE): its probe fails with the bug's own
#      pattern, the other MK probes pass, and the decision follows - viewport_flip, bitwise_scalar,
#      dyn_reemit and fp16_hide turn on in performance mode, the record-only ones say the probe failed; the
#      report diff tool shows the probe, its sub-cases and the decision it changed;
#   4  vb_strides tells the two stride shapes apart (VKBRIDGE_ERRATA_IMITATE_VB=single: option B);
#   5  viewport_flip on a probe failure in safe mode too; a crashed probe decides nothing;
#   6  VKBRIDGE_PROBE_FAKE=dyn_state:fail (the device-side switch) turns dyn_reemit on in
#      performance mode, VKBRIDGE_PROBE_APPLY=0 records it without;
#   7  the default budget: the slow light probes (wait_eintr, rendering_churn) go to the background,
#      the rest stays at start; the cost of each, at start and in the background;
#   8  the protocol hash is unchanged.
# Prints one line per step and "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
work=$(mktemp -d)
sock=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-probemk-$$.sock
srv=
log=
stop() { [ -n "$srv" ] && kill $srv 2>/dev/null && wait $srv 2>/dev/null; srv=; }
start() { # start <name> [env...]: a server of its own, cache $work/<name>/c, log $log
  local d=$work/$1
  shift
  stop
  rm -f "$sock"
  mkdir -p "$d/c" "$d/s"
  log=$d/s/vkbridge-server.log
  : > "$log"
  env -u VKBRIDGE_FAKE_MISSING -u VKBRIDGE_HIDE_EXTS -u VKBRIDGE_ERRATA -u VKBRIDGE_ERRATA_IMITATE -u VKBRIDGE_ERRATA_IMITATE_VB \
      -u VKBRIDGE_MODE -u VKBRIDGE_FAKE_DRIVER_VERSION -u VKBRIDGE_ERRATA_PROBE -u VKBRIDGE_PROBE_FAKE -u VKBRIDGE_WAIT_FAKE_ERROR \
      VKBRIDGE_PROBE_BUDGET_S=0 "$@" VK_DRIVER_FILES=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json} VK_ICD_FILENAMES= \
      "$out/vkbridge-server" --socket "$sock" --log "$log" --cache-dir "$d/c" >/dev/null 2>&1 &
  srv=$!
  for _ in $(seq 300); do [ -S "$sock" ] && break; kill -0 $srv 2>/dev/null || break; sleep 0.05; done
}
# A device (the smoke test); $new: the log lines it added
device() {
  local l; l=$(wc -l < "$log")
  timeout 120 "$out/vkbridge-smoke" >"$work/smoke.log" 2>&1
  new=$(tail -n +$((l+1)) "$log")
}
on_set() { echo "$new" | grep -o 'errata: device .*: on [^;]*' | tail -1 | sed 's/.*: on //'; }
prof() { ls "$work/$1"/c/probe-*.json 2>/dev/null | head -1; }
# status of probe $2 in every profile of step $1 ("pass pass" with two GPUs)
status_of() { cat "$work/$1"/c/probe-*.json 2>/dev/null | grep -o "\"name\": \"$2\", \"status\": \"[a-z]*\"" | sed 's/.*"status": "//; s/"//' | sort -u | tr '\n' ' ' | sed 's/ $//'; }
trap 'stop; rm -f "$sock"; [ -n "$KEEP" ] && echo "kept $work" || rm -rf "$work"' EXIT
export VKBRIDGE_SOCKET=$sock VK_DRIVER_FILES=$out/vkbridge_icd.json VK_ICD_FILENAMES=
unset VKBRIDGE_HIDE_EXTS VKBRIDGE_MAX_API VKBRIDGE_FAKE_MISSING
fails=0 steps=0
step() { steps=$((steps+1)); if [ "$1" = 0 ]; then echo "  ok   $2"; else echo "  FAIL $2"; fails=$((fails+1)); fi; }
mk="viewport_flip bitwise_vec dyn_state vb_strides view3d_layers wait_eintr rendering_churn fp16_math"

# 1
start plain
device
ok=1; res=
for p in $mk; do st=$(status_of plain $p); res="$res $p=$st"; [ "$st" = pass ] || [ "$st" = skip ] || ok=0; done
p1=$(prof plain)
[ $ok = 1 ] && grep -q '"probe_version": 3' "$p1" && grep -q '"name": "dyn_state", "status": "pass", .*"cases": "0x[1-9a-f]*", "failed": "0x0"' "$p1"
step $? "every MK probe passes:$res"
got=$(on_set)
[ "$got" = wait_retry,dyn_reemit,vb_split,view_quarantine,view_counts,cull_nontri ] && echo "$new" | grep -q "errata: dyn_reemit ON (always; safe mode; probe dyn_state passed) - MK-04" &&
  echo "$new" | grep -q "errata: view_quarantine ON (always; safe mode; probe rendering_churn passed, recorded only)" &&
  echo "$new" | grep -q "errata: wait_retry ON (always; safe mode; probe wait_eintr passed, recorded only)" &&
  echo "$new" | grep -q "errata: vb_split ON (always; safe mode; probe vb_strides passed, recorded only)" &&
  echo "$new" | grep -q "errata: view_counts ON (always; safe mode; probe view3d_layers passed, recorded only)"
step $? "safe mode: on $got, each with its probe's pass"
rep=$work/plain/s/malibridge-report
for _ in $(seq 100); do grep -q "^Driver errata, GPU 0" $rep.md 2>/dev/null && break; sleep 0.1; done
rows=$(grep -c '^| [a-z_]* | MK-0[1-7] |' $rep.md)
python3 - "$rep.json" <<'PY' 2>$work/1j.log
import json, sys
r = json.load(open(sys.argv[1]))
e = {x["name"]: x for x in r["probe"][0]["errata"]}
assert len(e) == 14, e.keys()
assert not e["gs_hide"]["probe"] and not e["gs_hide"]["safe"]["on"] and not e["gs_hide"]["performance"]["on"], e["gs_hide"]
assert e["vb_bake"]["probe"] == "vb_strides" and e["vb_bake"]["probe_gates"], e["vb_bake"]
assert not e["clamp_zero"]["probe"] and not e["clamp_zero"]["safe"]["on"], e["clamp_zero"]
assert e["dyn_reemit"]["safe"]["on"] and not e["dyn_reemit"]["performance"]["on"], e["dyn_reemit"]
assert e["viewport_flip"]["probe_status"] == "pass" and not e["viewport_flip"]["safe"]["on"]
assert e["view_quarantine"]["probe"] == "rendering_churn" and not e["view_quarantine"]["probe_gates"]
assert r["probe"][0]["mode"] == "safe"
PY
j=$?
[ $j = 0 ] && [ "$rows" -ge 7 ] && grep -q "| dyn_reemit | MK-04 | dyn_state | pass | \*\*ON\*\* (always; safe mode; probe dyn_state passed) - in force | off (not needed here) |" $rep.md
step $? "the report: $rows errata rows with the probe result and both modes ($(grep -m1 '| viewport_flip |' $rep.md | cut -c1-120)...)$([ $j = 0 ] || head -3 $work/1j.log)"

# 2
start plain VKBRIDGE_MODE=performance
device
got=$(on_set)
grep -q "probe: device profile .* not passed" $log && ! grep -q "probe: probing" $log && [ "$got" = wait_retry,vb_split,view_counts ] &&
  echo "$new" | grep -q "performance mode: on wait_retry,vb_split,view_counts; off viewport_flip,bitwise_scalar,dyn_reemit,view_quarantine"
step $? "performance mode, same profile (not probed again): on $got"

# 3
imitate() { # erratum probe flag expected-line
  start "$1" VKBRIDGE_MODE=performance VKBRIDGE_ERRATA_IMITATE=$1
  device
  local others=1 p st res
  for p in $mk; do
    [ "$p" = "$2" ] && continue
    st=$(status_of "$1" $p); [ "$st" = pass ] || [ "$st" = skip ] || { others=0; res="$res $p=$st"; }
  done
  st=$(status_of "$1" "$2")
  local pf; pf=$(prof "$1")
  [ "$st" = fail ] && [ $others = 1 ] && grep -q "\"name\": \"$2\", \"status\": \"fail\", .*\"want\": \"$3\"" "$pf" &&
    grep -q "probe: $2 .*the bug imitated beneath: VKBRIDGE_ERRATA_IMITATE" $log && { [ -z "$4" ] || echo "$new" | grep -q "$4"; }
  step $? "$1 imitated: $2 $st ($(grep -m1 -o "probe: $2 .*" $log | cut -c30-150)...)$res; on $(on_set)"
}
imitate wait_retry wait_eintr 0x80 "errata: wait_retry ON (always; performance mode; probe wait_eintr failed, recorded only)"
imitate viewport_flip viewport_flip 0x100 "errata: viewport_flip ON (probe viewport_flip failed; performance mode)"
imitate bitwise_scalar bitwise_vec 0x200 "errata: bitwise_scalar ON (probe bitwise_vec failed; performance mode) - MK-03"
imitate dyn_reemit dyn_state 0x400 "errata: dyn_reemit ON (probe dyn_state failed; performance mode) - MK-04"
imitate vb_split vb_strides 0x800 "errata: vb_split ON (always; performance mode; probe vb_strides failed, recorded only)"
imitate view_counts view3d_layers 0x4000 "errata: view_counts ON (always; performance mode; probe view3d_layers failed, recorded only)"
if [ "$(status_of plain fp16_math)" = skip ]; then
  step 0 "fp16_hide imitated: fp16_math skip (driver lacks native shaderFloat16)"
else
  imitate fp16_hide fp16_math 0x8000 "errata: fp16_hide ON (probe fp16_math failed; performance mode) - MK-20"
fi
imitate view_quarantine rendering_churn 0x2000 ""
for _ in $(seq 100); do grep -q "^| view_quarantine" $work/view_quarantine/s/malibridge-report.md 2>/dev/null && break; sleep 0.1; done
! echo "$new" | grep -q "errata: view_quarantine ON" &&
  grep -q "^| view_quarantine | MK-06 | rendering_churn (records) | \*\*fail (views)\*\* | \*\*ON\*\* (always; safe mode; probe rendering_churn failed, recorded only) | off (not needed here) - in force |" \
    $work/view_quarantine/s/malibridge-report.md
step $? "  rendering_churn's failure recorded only: view_quarantine stays off off the driver's range in performance mode (safe mode: always on) ($(grep -m1 '^| view_quarantine' $work/view_quarantine/s/malibridge-report.md | cut -c1-100))"
grep -q "| dyn_reemit | MK-04 | dyn_state | \*\*fail (depth_write, cull, stencil_wmask" $work/dyn_reemit/s/malibridge-report.md 2>/dev/null ||
  { sleep 6; grep -q "| dyn_reemit | MK-04 | dyn_state | \*\*fail (depth_write, cull, stencil_wmask" $work/dyn_reemit/s/malibridge-report.md; }
step $? "  the report names dyn_state's failed sub-cases ($(grep -m1 -o '| dyn_reemit | MK-04 | dyn_state | [^|]*' $work/dyn_reemit/s/malibridge-report.md))"

python3 ../tools/malibridge_report_diff.py $work/plain/s/malibridge-report.json $work/dyn_reemit/s/malibridge-report.json >$work/3d.log 2>&1
grep -q "probe dyn_state: pass -> fail, flags 0x0 -> 0x400, failed 0x0 -> 0x1f" $work/3d.log &&
  grep -q "erratum dyn_reemit, performance mode: off -> on (probe dyn_state failed; performance mode)" $work/3d.log
step $? "  the diff tool: the failed probe, its sub-cases and the decision it changed ($(grep -m1 'erratum dyn_reemit' $work/3d.log))"

# 4
start vbsingle VKBRIDGE_MODE=performance VKBRIDGE_ERRATA_IMITATE=vb_split VKBRIDGE_ERRATA_IMITATE_VB=single
device
grep -q '"name": "vb_strides", "status": "fail", .*"want": "0x1800", "cases": "0x7", "failed": "0x6"' "$(prof vbsingle)" &&
  grep -q "probe: vb_strides .*single-binding calls: WRONG.*splitting cannot fix it (option B" $log
step $? "single-binding calls wrong too: option B told apart ($(grep -m1 -o 'probe: vb_strides .*' $log | cut -c30-170)...)"

# 5
start flipsafe VKBRIDGE_ERRATA_IMITATE=viewport_flip
device
echo "$new" | grep -q "errata: viewport_flip ON (probe viewport_flip failed; safe mode) - MK-02$"
step $? "viewport_flip on a probe failure in safe mode too ($(echo "$new" | grep -o 'errata: viewport_flip ON.*' | cut -c1-90))"
start flipcrash VKBRIDGE_MODE=performance VKBRIDGE_PROBE_FAKE=viewport_flip:crash,dyn_state:hang
device
[ "$(status_of flipcrash viewport_flip)" = crash ] && [ "$(status_of flipcrash dyn_state)" = hang ] &&
  ! echo "$new" | grep -q "errata: viewport_flip ON" && ! echo "$new" | grep -q "errata: dyn_reemit ON"
step $? "a crashed or hung MK probe decides nothing: viewport_flip and dyn_reemit off in performance mode (on $(on_set))"

# 6
start fake VKBRIDGE_MODE=performance VKBRIDGE_PROBE_FAKE=dyn_state:fail
device
a=$(on_set)
echo "$new" | grep -q "errata: dyn_reemit ON (probe dyn_state failed; performance mode)"; r1=$?
start fake0 VKBRIDGE_MODE=performance VKBRIDGE_PROBE_FAKE=dyn_state:fail VKBRIDGE_PROBE_APPLY=0
device
[ $r1 = 0 ] && ! echo "$new" | grep -q "errata: dyn_reemit ON" && [ "$(status_of fake0 dyn_state)" = fail ]
step $? "VKBRIDGE_PROBE_FAKE=dyn_state:fail: on $a; with VKBRIDGE_PROBE_APPLY=0 recorded only (on $(on_set))"

# 7
start budget VKBRIDGE_PROBE_BUDGET_S=6
for _ in $(seq 100); do [ "$(grep -c 'background probes done' $log)" -ge "$(grep -c 'probe: probing' $log)" ] && break; sleep 0.1; done
starts=$(grep -o 'probe: [0-9]* ms at start' $log | grep -o '[0-9]*' | tr '\n' ' ')
bgs=$(grep -o 'background probes done ([0-9]* ms background' $log | grep -o '[0-9]* ms' | tr '\n' ' ')
cost=$(for p in $mk; do grep "probe: $p " $log | head -1 | awk -v p=$p '{for (i=1;i<=NF;i++) if ($i=="ms") {printf "%s %s ms, ", p, $(i-1); break}}'; done)
first_bg=$(grep -n "continue in the background" $log | head -1 | cut -d: -f1)
ok=1
for p in wait_eintr rendering_churn; do [ "$(grep -n "probe: $p " $log | head -1 | cut -d: -f1)" -gt "${first_bg:-0}" ] || ok=0; done
for p in viewport_flip bitwise_vec dyn_state vb_strides view3d_layers; do [ "$(grep -n "probe: $p " $log | head -1 | cut -d: -f1)" -lt "${first_bg:-0}" ] || ok=0; done
if [ "$(status_of plain fp16_math)" != skip ]; then
  [ "$(grep -n "probe: fp16_math " $log | head -1 | cut -d: -f1)" -lt "${first_bg:-0}" ] || ok=0
fi
[ $ok = 1 ] && [ -n "$bgs" ]
step $? "default budget: start ${starts}ms, background ${bgs}(wait_eintr and rendering_churn there); ${cost%, }"

# 8
grep -q "^#define VKB_PROTOCOL_HASH 0x4a9da939fdf65896ULL" ../common/gen/vkb_gen.h
step $? "protocol hash $(grep -o '0x[0-9a-f]*' ../common/gen/vkb_gen.h | head -1)"

if [ $fails = 0 ]; then echo "OK: $steps steps"; else echo "FAILED: $fails of $steps steps"; exit 1; fi
