#!/bin/bash
# The GPU profiler's dispatch spans (server_gpuprof.c, VKBRIDGE_GPU_PROF). Consecutive dispatches shared one span, so
# under =2 the costliest span named its first pipeline only, whatever the others in it cost (idea seen in a fork of
# K11MCH1's, no PR: a span per dispatch; built here as a span per compute pipeline, so a run of one pipeline's
# dispatches stays one stamp and command buffers with hundreds of dispatches keep within the 512 stamps they get).
# tests/gpuprof_check.c against a server of its own per step:
#   1  =2, "runs" (A: 1 dispatch x 16384 workgroups; B: 600 dispatches x 1; a copy): two dispatch spans, one naming
#      16384 workgroups in 1 dispatch, one 600 in 600; the copy's span is in the report; nothing cut short;
#   2  =1, the same: one dispatch span (pipelines not told apart, as before), the copy's span in the report;
#   3  =2, "alternate" (A and B every dispatch, 600): the report says command buffers were cut short;
#   4  off: no profiler line at all.
# Fails before the change: step 1 one dispatch span, B not named.
# Prints one line per step and "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
work=$(mktemp -d)
sock=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-gpuprof-$$.sock
srv=
log=
stop() { [ -n "$srv" ] && kill $srv 2>/dev/null && wait $srv 2>/dev/null; srv=; }
trap 'stop; rm -f "$sock"; [ -n "$KEEP" ] && echo "kept $work" || rm -rf "$work"' EXIT
start() { # start <name> [env...]
  local d=$work/$1
  shift
  stop
  rm -f "$sock"
  mkdir -p "$d/c"
  log=$d/server.log
  env -u VKBRIDGE_FAKE_MISSING -u VKBRIDGE_HIDE_EXTS -u VKBRIDGE_MODE -u VKBRIDGE_GPU_PROF "$@" \
      VK_DRIVER_FILES=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json} VK_ICD_FILENAMES= \
      "$out/vkbridge-server" --socket "$sock" --log "$log" --cache-dir "$d/c" >/dev/null 2>&1 &
  srv=$!
  for _ in $(seq 100); do [ -S "$sock" ] && break; kill -0 $srv 2>/dev/null || break; sleep 0.1; done
}
export VKBRIDGE_SOCKET=$sock VK_DRIVER_FILES=$out/vkbridge_icd.json VK_ICD_FILENAMES=
unset VKBRIDGE_HIDE_EXTS VKBRIDGE_MAX_API VKBRIDGE_SYNC
fails=0 steps=0
step() { steps=$((steps+1)); if [ "$1" = 0 ]; then echo "  ok   $2"; else echo "  FAIL $2"; fails=$((fails+1)); fi; }
run() { timeout 120 "$out/gpuprof_check" "$1" >"$work/client-$2.out" 2>&1 && grep -q '^OK:' "$work/client-$2.out"; }
# the first report only (later ones may hold the Begin after the loop alone)
report() { awk '/gpu prof [0-9.]+s:/{n++} n==1' "$log" | grep -v 'optimal-tiling'; }

# 1
start prof2 VKBRIDGE_GPU_PROF=2
run runs 1; rc=$?
r=$(report)
nd=$(grep -c 'between passes, dispatches' <<<"$r")
a=$(grep -c 'worst .*: pipeline .* 1 draws/dispatches 16384 prims/workgroups' <<<"$r")
b=$(grep -c 'worst .*: pipeline .* 600 draws/dispatches 600 prims/workgroups' <<<"$r")
cp=$(grep -c 'between passes, buffer copies/fills' <<<"$r")
cut=$(grep -c 'cut short' <<<"$r")
[ $rc = 0 ] && [ "$nd" = 2 ] && [ "$a" = 1 ] && [ "$b" = 1 ] && [ "$cp" = 1 ] && [ "$cut" = 0 ]
step $? "=2 runs: dispatch spans $nd (want 2), A named $a, B's 600 as one $b, copy seen $cp, cut short $cut"

# 2
start prof1 VKBRIDGE_GPU_PROF=1
run runs 2; rc=$?
r=$(report)
nd=$(grep -c 'between passes, dispatches' <<<"$r")
cp=$(grep -c 'between passes, buffer copies/fills' <<<"$r")
[ $rc = 0 ] && [ "$nd" = 1 ] && [ "$cp" = 1 ]
step $? "=1 runs: dispatch spans $nd (want 1, as before), copy seen $cp"

# 3
start alt VKBRIDGE_GPU_PROF=2
run alternate 3; rc=$?
r=$(report)
cut=$(grep -o '[0-9]* cut short' <<<"$r" | head -1)
[ $rc = 0 ] && [ "${cut%% *}" -gt 0 ] 2>/dev/null
step $? "=2 alternate: header says '${cut:-nothing cut short}'"

# 4
start off
GPUPROF_S=1 run runs 4; rc=$?
lines=$(grep -c 'gpu prof' "$log")
[ $rc = 0 ] && [ "$lines" = 0 ]
step $? "off: profiler lines $lines"

stop
[ $fails = 0 ] && echo "OK: $steps steps" || echo "FAILED: $fails of $steps steps"
[ $fails = 0 ]
