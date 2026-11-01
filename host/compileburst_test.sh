#!/bin/bash
# Compile hitches, new pipelines at their draws (tests/compileburst_check.c: 5 frames with 2 new
# pipelines each, then one frame with 8; every other one without vertex input), with a server of
# its own (socket, cache directory; switches in <cache dir>/env) and VKBRIDGE_HITCH_MS=100:
#   1  cold, a slow compiler imitated (VKBRIDGE_ASYNC_COMPILE_IMITATE_MS=80):
#      off: every draw there, a hitch line with cause=compile on each frame with new pipelines;
#      =1 (draws skipped; every pipeline with vertex input here, so that all may skip): shorter
#      frames, but draws missing (the clear colour left);
#      =3 (deferred): every draw there, frames shorter than off (the 2-pipeline frames under the
#      100 ms threshold, the burst at most about two thirds), fewer compile hitches, no draw skipped;
#      =3 with 4 worker threads: the burst shorter again;
#   2  warm, no imitated delay, each mode run twice on the same cache: every draw there, the
#      deferred mode hands back real pipelines (nothing left in the background) and its frames are
#      as long as off's;
#   3  the server is alive and logged no protocol errors.
# Prints one line per step (with the numbers) and "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
work=$(mktemp -d)
cache=$work/c
mkdir -p "$cache"
sock=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-burst-$$.sock
log=$work/server.log
srv=
start() {
  [ -n "$srv" ] && kill $srv 2>/dev/null && wait $srv 2>/dev/null
  rm -f "$sock"
  env -u VKBRIDGE_FAKE_MISSING -u VKBRIDGE_HIDE_EXTS -u VKBRIDGE_ERRATA -u VKBRIDGE_ERRATA_IMITATE -u VKBRIDGE_MODE -u VKBRIDGE_HITCH_MS \
      -u VKBRIDGE_ASYNC_COMPILE -u VKBRIDGE_ASYNC_COMPILE_IMITATE_MS -u VKBRIDGE_ASYNC_COMPILE_WAIT_MS -u VKBRIDGE_ASYNC_COMPILE_THREADS \
      VKBRIDGE_PROBE_BUDGET_S=0 "$@" \
      VK_DRIVER_FILES=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json} VK_ICD_FILENAMES= \
      "$out/vkbridge-server" --socket "$sock" --log "$log" --cache-dir "$cache" >/dev/null 2>&1 &
  srv=$!
  for _ in $(seq 100); do [ -S "$sock" ] && break; sleep 0.05; done
}
trap 'kill $srv 2>/dev/null; rm -f "$sock"; [ -n "$KEEP" ] || rm -rf "$work"' EXIT
export VKBRIDGE_SOCKET=$sock VK_DRIVER_FILES=$out/vkbridge_icd.json VK_ICD_FILENAMES=
unset VKBRIDGE_HIDE_EXTS VKBRIDGE_MAX_API VKBRIDGE_HITCH_MS
fails=0 steps=0
step() { steps=$((steps+1)); if [ "$1" = 0 ]; then echo "  ok   $2"; else echo "  FAIL $2"; fails=$((fails+1)); fi; }

# run <tag> <env lines...>: $rc; $work/<tag>.log the check's output, $work/<tag>.srv its server lines
run() {
  local tag=$1; shift
  printf '%s\n' "$@" > "$cache/env"
  local l; l=$(wc -l < "$log")
  # the guest's switch: the env file is read at vkCreateDevice, after the client asked for it
  VKBRIDGE_HITCH_MS=100 timeout 120 "$out/compileburst_check" --tag "$tag" $ARGS > "$work/$tag.log" 2>&1
  rc=$?
  sleep 0.3 # the device's end: the async summary and the hitch summary
  tail -n +$((l+1)) "$log" > "$work/$tag.srv"
}
# val <tag> <kind> <field>: from the check's summary line
val() { grep "^summary $1 $2 " "$work/$1.log" | grep -o " $3=[0-9/]*" | cut -d= -f2; }
hitches() { grep -c "hitch: .*cause=compile" "$work/$1.srv"; }
hitch_ms() { grep -o "hitch: .*cause=compile" "$work/$1.srv" | grep -o " ms=[0-9]*" | cut -d= -f2 | awk '{s+=$1} END {print s+0}'; }
asum() { grep -o 'async compile, device [0-9]*: .*' "$work/$1.srv" | tail -1; }
allin() { local d; d=$(val $1 $2 drawn); [ -n "$d" ] && [ "${d%/*}" = "${d#*/}" ]; }
line() { echo "$1: small mean $(val $1 small mean_ms) ms max $(val $1 small max_ms), burst $(val $1 burst max_ms) ms, ordinary $(val $1 ordinary mean_ms) ms; drawn $(val $1 small drawn) + $(val $1 burst drawn); compile hitches $(hitches $1) ($(hitch_ms $1) ms)"; }

start
# 1
run off VKBRIDGE_ASYNC_COMPILE_IMITATE_MS=80
[ $rc = 0 ] && allin off small && allin off burst && [ "$(val off small mean_ms)" -ge 150 ] && [ "$(hitches off)" -ge 6 ]
step $? "cold, off: $(line off)"
ARGS="--odd-fullscreen 0" run skip VKBRIDGE_ASYNC_COMPILE=1 VKBRIDGE_ASYNC_COMPILE_IMITATE_MS=80
d1=$(val skip small drawn) d2=$(val skip burst drawn)
[ $rc = 0 ] && [ -n "$d1" ] && { ! allin skip small || ! allin skip burst; }
step $? "cold, =1 (skips): $(line skip) - missing draws, as expected"
echo "       server: $(asum skip | cut -c1-240)"
run defer VKBRIDGE_ASYNC_COMPILE=3 VKBRIDGE_ASYNC_COMPILE_IMITATE_MS=80
s0=$(val off small mean_ms) b0=$(val off burst max_ms) s3=$(val defer small mean_ms) b3=$(val defer burst max_ms)
[ $rc = 0 ] && allin defer small && allin defer burst && grep -q "no draw skipped" "$work/defer.srv" &&
  asum defer | grep -q ' 0 draws skipped' && asum defer | grep -qE ' [1-9][0-9]* binds waited' &&
  [ "$s3" -lt 100 ] && [ $((b3 * 3)) -le $((b0 * 2)) ] && [ "$(hitches defer)" -lt "$(hitches off)" ]
step $? "cold, =3 (deferred): $(line defer)"
echo "       server: $(asum defer | cut -c1-300)"
run defer4 VKBRIDGE_ASYNC_COMPILE=3 VKBRIDGE_ASYNC_COMPILE_THREADS=4 VKBRIDGE_ASYNC_COMPILE_IMITATE_MS=80
b4=$(val defer4 burst max_ms)
[ $rc = 0 ] && allin defer4 small && allin defer4 burst && [ "$b4" -lt "$b3" ]
step $? "cold, =3 with 4 threads: $(line defer4)"

# 2
rm -f "$cache"/*.vkpc
run warm0a
run warm0
run warm3a VKBRIDGE_ASYNC_COMPILE=3
run warm3 VKBRIDGE_ASYNC_COMPILE=3
w0=$(val warm0 small mean_ms) w3=$(val warm3 small mean_ms) o0=$(val warm0 ordinary mean_ms) o3=$(val warm3 ordinary mean_ms)
diff=$((w3 - w0)); diff=${diff#-}
[ $rc = 0 ] && allin warm0 small && allin warm0 burst && allin warm3 small && allin warm3 burst &&
  asum warm3 | grep -q ' 0 draws skipped' && [ "$diff" -le 3 ] && [ "$(hitches warm3)" = 0 ] && [ "$(hitches warm0)" = 0 ]
step $? "warm: off $(line warm0 | cut -d: -f2-) | =3 $(line warm3 | cut -d: -f2-)"
echo "       server (=3, warm): $(asum warm3 | cut -c1-240)"

# 3
kill -0 $srv 2>/dev/null && ! grep -qE "command stream:|malformed request|unknown command id|caught signal|contained crash" "$log"
step $? "server alive, no protocol errors or crashes"

if [ $fails = 0 ]; then echo "OK: $steps steps"; else echo "FAILED: $fails of $steps steps"; exit 1; fi
