#!/bin/bash
# The layer-only pass-through geometry stage fold and the UE4 LUT stand-in (spirv_gsfold.c,
# server_guard.c), each with a server of its own: 1 VKBRIDGE_LAYER_GS_FOLD=1 folds it up front;
# 2 a compile that never ends (VKBRIDGE_TEST_CRASH_PIPELINE=spin on the geometry stage) gets the
# fold as its fallback (VKBRIDGE_COMPILE_FALLBACKS=gsfold) and is remembered; 3 the next server
# takes the remembered fallback at once; 4 a never-ending UE4-LUT-shaped fragment compile gets the
# neutral LUT stand-in, whose texel values are checked; 5 a LUT compile that is slow, not endless (3 s
# under a 2 s limit, a player log's CombineLUTs): the stand-in in the first run, compiled again with twice the limit
# in the next (built, with its colour grading, kept as "built with a longer limit"), built with that limit at
# once in the third; 6 a LUT shape the driver spins on, in several variants: one timeout and one retry per device,
# then the stand-in at once (Stray, a player log). The picture is checked each time (gsfold_check). Prints "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
work=$(mktemp -d)
sock=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-gsfold-$$.sock
srv=
start() {
  [ -n "$srv" ] && kill $srv 2>/dev/null && wait $srv 2>/dev/null
  rm -f "$sock"
  env "$@" VK_DRIVER_FILES=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json} VK_ICD_FILENAMES= \
    "$out/vkbridge-server" --socket "$sock" --log "$work/server.log" --cache-dir "$work/c" >/dev/null 2>&1 &
  srv=$!
  for _ in $(seq 50); do [ -S "$sock" ] && break; sleep 0.1; done
}
trap 'kill $srv 2>/dev/null; rm -f "$sock"; rm -rf "$work"' EXIT
export VKBRIDGE_SOCKET=$sock VK_DRIVER_FILES=$out/vkbridge_icd.json VK_ICD_FILENAMES=
fails=0
step() { if [ "$1" = 0 ]; then echo "  ok   $2"; else echo "  FAIL $2"; fails=$((fails+1)); fi; }

start VKBRIDGE_LAYER_GS_FOLD=1
timeout 60 "$out/gsfold_check" >$work/1.log 2>&1 && grep -q "pass-through geometry stage folded into the vertex shader) for stages .*: built" "$work/server.log"
step $? "VKBRIDGE_LAYER_GS_FOLD=1: geometry stage folded, picture right ($(tail -1 $work/1.log))"
: > "$work/server.log"
start VKBRIDGE_TEST_CRASH_PIPELINE=spin VKBRIDGE_COMPILE_WARN_S=1 VKBRIDGE_COMPILE_TIMEOUT_S=3 VKBRIDGE_COMPILE_FALLBACKS=gsfold
timeout 90 "$out/gsfold_check" --mark >$work/2.log 2>&1 && grep -q "TIMED OUT" "$work/server.log" &&
  grep -q "built with its fallback: pass-through geometry stage folded" "$work/server.log" && grep -q " m=3 " "$work/c/stuck-pipelines"
step $? "never-ending compile: built without the geometry stage, remembered ($(tail -1 $work/2.log))"
: > "$work/server.log"
start VKBRIDGE_TEST_CRASH_PIPELINE=spin VKBRIDGE_COMPILE_WARN_S=1 VKBRIDGE_COMPILE_TIMEOUT_S=3 VKBRIDGE_COMPILE_FALLBACKS=gsfold
s0=$(date +%s)
timeout 90 "$out/gsfold_check" --mark >$work/3.log 2>&1 && ! grep -q "TIMED OUT" "$work/server.log" && [ $(( $(date +%s) - s0 )) -lt 3 ]
step $? "next run: the remembered fallback at once ($(tail -1 $work/3.log))"
rm -rf "$work/c"
: > "$work/server.log"
start VKBRIDGE_TEST_CRASH_PIPELINE=spin VKBRIDGE_COMPILE_WARN_S=1 VKBRIDGE_COMPILE_TIMEOUT_S=3
timeout 90 "$out/gsfold_check" --lut >$work/4.log 2>&1 && [ "$(grep -c 'TIMED OUT' "$work/server.log")" = 1 ] &&
  grep -q "built with its fallback: neutral colour-grading LUT stand-in" "$work/server.log" && grep -q " m=4 v=" "$work/c/stuck-pipelines"
step $? "never-ending UE4 LUT compile: one timeout, then the neutral LUT stand-in with the right texels ($(tail -1 $work/4.log))"
# 5
rm -rf "$work/c"
r5=
for run in 1 2 3; do
  : > "$work/server.log"
  start VKBRIDGE_TEST_CRASH_PIPELINE=slow VKBRIDGE_TEST_SLOW_S=3 VKBRIDGE_COMPILE_WARN_S=1 VKBRIDGE_COMPILE_TIMEOUT_S=2
  s0=$(date +%s)
  timeout 90 "$out/gsfold_check" --lutslow >$work/5-$run.log 2>&1
  el=$(( $(date +%s) - s0 ))
  grep -q "RESULT built" $work/5-$run.log || r5="$r5 run$run-not-built"
  n=$(grep -c 'TIMED OUT' "$work/server.log")
  fb=$(grep -c "built with its fallback: neutral colour-grading LUT stand-in" "$work/server.log")
  case $run in
    1) [ "$n" = 1 ] && [ "$fb" = 1 ] && grep -q " m=4 " "$work/c/stuck-pipelines" || r5="$r5 run1(timeouts $n, stand-in $fb)" ;;
    2) [ "$n" = 0 ] && [ "$fb" = 0 ] && grep -q "compiled again with 4 s" "$work/server.log" && grep -q "built with 4 s from now on" "$work/server.log" &&
         tail -1 "$work/c/stuck-pipelines" | grep -q " m=5 .* t=2 " || r5="$r5 run2(timeouts $n, stand-in $fb)" ;;
    3) [ "$n" = 0 ] && [ "$fb" = 0 ] && ! grep -q "compiled again" "$work/server.log" && [ "$el" -ge 3 ] || r5="$r5 run3(timeouts $n, stand-in $fb, ${el}s)" ;;
  esac
done
# a retry that does not finish either (a compile of 9 s under 2 s: the retry's 4 s is not enough): the stand-in again, no third try
rm -rf "$work/c"
for run in 1 2 3; do
  : > "$work/server.log"
  start VKBRIDGE_TEST_CRASH_PIPELINE=slow VKBRIDGE_TEST_SLOW_S=9 VKBRIDGE_COMPILE_WARN_S=1 VKBRIDGE_COMPILE_TIMEOUT_S=2
  timeout 90 "$out/gsfold_check" --lutslow >$work/5b-$run.log 2>&1
  grep -q "RESULT built" $work/5b-$run.log || r5="$r5 b-run$run-not-built"
  n=$(grep -c 'TIMED OUT' "$work/server.log"); fb=$(grep -c "built with its fallback: neutral colour-grading LUT stand-in\|compile fallback (neutral colour-grading LUT stand-in).*built" "$work/server.log")
  case $run in
    1) [ "$n" = 1 ] || r5="$r5 b1(timeouts $n)" ;;
    2) [ "$n" = 1 ] && grep -q "compiled again with 4 s" "$work/server.log" && grep -q "the stand-in again" "$work/server.log" || r5="$r5 b2(timeouts $n)" ;;
    3) [ "$n" = 0 ] && ! grep -q "compiled again" "$work/server.log" || r5="$r5 b3(timeouts $n)" ;;
  esac
done
step $([ -z "$r5" ] && echo 0 || echo 1) "slow UE4 LUT compile (3 s, limit 2 s): stand-in, then compiled again with 4 s and kept, then built with 4 s at once; a retry that times out too: the stand-in, no third try${r5:+ (failed:$r5)}"
# 6: a LUT compile that never finishes in time, in several variants (Stray's CombineLUTs on the G925, a player log:
# a 10 s freeze per new variant, and 20 s more for each one's retry in the next run). Variant A times out once; B in the
# same run gets the stand-in at once; in the next run A's retry with twice the limit times out too - the driver spins on
# the shape - so B is not retried and a new C gets the stand-in at once; in the third run nothing times out or is retried.
rm -rf "$work/c"
r6=
lut6() { GSFOLD_VARIANT=$1 timeout 90 "$out/gsfold_check" --lutslow >$work/6-$2-$1.log 2>&1; grep -q "RESULT built" $work/6-$2-$1.log || r6="$r6 run$2-v$1-not-built"; }
for run in 1 2 3; do
  : > "$work/server.log"
  start VKBRIDGE_TEST_CRASH_PIPELINE=slow VKBRIDGE_TEST_SLOW_S=9 VKBRIDGE_COMPILE_WARN_S=1 VKBRIDGE_COMPILE_TIMEOUT_S=2
  s0=$(date +%s)
  case $run in 1) lut6 1 1; lut6 2 1 ;; 2) lut6 1 2; lut6 2 2; lut6 3 2 ;; 3) lut6 1 3; lut6 2 3; lut6 3 3; lut6 4 3 ;; esac
  el=$(( $(date +%s) - s0 ))
  n=$(grep -c 'TIMED OUT' "$work/server.log"); again=$(grep -c "compiled again" "$work/server.log")
  case $run in
    1) [ "$n" = 1 ] || r6="$r6 run1(timeouts $n, want 1)" ;;
    2) [ "$n" = 1 ] && [ "$again" = 1 ] && grep -q "spins on the UE4 LUT shape" "$work/server.log" || r6="$r6 run2(timeouts $n want 1, retries $again want 1)" ;;
    3) [ "$n" = 0 ] && [ "$again" = 0 ] && [ "$el" -lt 6 ] && grep -q "get their neutral stand-in at once" "$work/server.log" ||
         r6="$r6 run3(timeouts $n want 0, retries $again want 0, ${el}s)" ;;
  esac
done
step $([ -z "$r6" ] && echo 0 || echo 1) "LUT compile that spins in several variants: one timeout, then the stand-in at once for new variants; one retry, then none${r6:+ (failed:$r6)}"
if [ $fails = 0 ]; then echo "OK: 6 steps"; else echo "FAILED: $fails of 6 steps"; exit 1; fi
