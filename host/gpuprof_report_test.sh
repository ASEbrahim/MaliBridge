#!/bin/bash
# The pass profiler (VKBRIDGE_GPU_PROF=1 in <cache dir>/env; server/server_gpuprof.c) on frames recorded as vkd3d-proton
# records them (tests/gpuprof_report_check.c: one-time-submit command buffers, dynamic rendering, vkCmdDrawIndirectCount,
# vkCmdDispatchBase), a server of its own for each step, the summary period shortened to 2 s (VKBRIDGE_TEST_GPROF_S):
#   1  one command buffer begun again each frame (DXVK's way): a "gpu prof" summary naming command buffers read;
#   2  a new command buffer each frame, none begun twice: the same (the old profiler reported only when a buffer was begun again);
#   3  new command buffers freed after each frame: the same;
#   4  a primary begun with a non-NULL pInheritanceInfo: read as primary, the same;
#   5  the indirect-count draws and the dispatch base are counted (a line with indirect-count draw calls, a dispatch count), in step 1;
#   6  a device with nothing recorded: the summary still comes (0 command buffers), so silence means the profiler is off;
#   7  =counters-only: one line says the pass profiler is off and why.
# Prints one line per step and "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
work=$(mktemp -d)
cache=$work/c
mkdir -p "$cache"
sock=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-gpuprof-$$.sock
log=$work/server.log
srv=
start() {
  [ -n "$srv" ] && kill $srv 2>/dev/null && wait $srv 2>/dev/null
  rm -f "$sock"
  : > "$log"
  printf 'VKBRIDGE_GPU_PROF=%s\nVKBRIDGE_TEST_GPROF_S=2\n' "${1:-1}" > "$cache/env"
  env -u VKBRIDGE_FAKE_MISSING -u VKBRIDGE_HIDE_EXTS -u VKBRIDGE_FAKE_API -u VKBRIDGE_MODE -u VKBRIDGE_GPU_PROF \
      VK_DRIVER_FILES=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json} VK_ICD_FILENAMES= \
      "$out/vkbridge-server" --socket "$sock" --log "$log" --cache-dir "$cache" >/dev/null 2>&1 &
  srv=$!
  for _ in $(seq 50); do [ -S "$sock" ] && break; sleep 0.1; done
}
stop() { [ -n "$srv" ] && kill $srv 2>/dev/null && wait $srv 2>/dev/null; srv=; }
trap 'kill $srv 2>/dev/null; rm -f "$sock"; [ -n "$KEEP" ] || rm -rf "$work"' EXIT
export VKBRIDGE_SOCKET=$sock VK_DRIVER_FILES=$out/vkbridge_icd.json VK_ICD_FILENAMES=
unset VKBRIDGE_HIDE_EXTS VKBRIDGE_MAX_API
fails=0 steps=0
step() { steps=$((steps+1)); if [ "$1" = 0 ]; then echo "  ok   $2"; else echo "  FAIL $2"; fails=$((fails+1)); fi; }
run() { timeout 120 "$out/gpuprof_report_check" "$@" >$work/run.log 2>&1; rc=$?; res=$(tail -1 $work/run.log); stop; }
# lines of the log: the summaries, the biggest "in N command buffers"
read_n() { grep -o 'gpu prof [0-9.]*s: .* in [0-9]* command buffers' "$log" | sed 's/.* in \([0-9]*\) command buffers/\1/' | sort -n | tail -1; }

for m in reuse fresh freed inherit; do
  start
  run $m 6
  n=$(read_n)
  [ $rc = 0 ] && [ "${n:-0}" -ge 20 ]
  step $? "$m: $res; summary read ${n:-none} command buffers"
  if [ $m = reuse ]; then
    ind=$(grep -c 'indirect-count draw calls' "$log"); dsp=$(grep -cE '[1-9][0-9]* dispatches' "$log")
    [ "$ind" -ge 1 ] && [ "$dsp" -ge 1 ]
    step $? "reuse: indirect-count draw calls named in $ind line(s), dispatches in $dsp"
  fi
done
start
run idle 5
grep -qE 'gpu prof [0-9.]+s: 0\.0 ms GPU/s in 0 command buffers' "$log"
step $? "idle: heartbeat summary with 0 command buffers"
start counters-only
run idle 1
grep -q 'GPU profiler off (VKBRIDGE_GPU_PROF=counters-only' "$log"
step $? "counters-only: the pass profiler says it is off: $(grep -o 'GPU profiler off.*' "$log" | head -1)"
if [ $fails = 0 ]; then echo "OK: $steps steps"; else echo "FAILED: $fails of $steps steps"; exit 1; fi
