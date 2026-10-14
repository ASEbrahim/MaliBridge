#!/bin/bash
# The layer-only pass-through geometry stage fold and the UE4 LUT stand-in (spirv_gsfold.c,
# server_guard.c), each with a server of its own: 1 VKBRIDGE_LAYER_GS_FOLD=1 folds it up front;
# 2 a compile that never ends (VKBRIDGE_TEST_CRASH_PIPELINE=spin on the geometry stage) gets the
# fold as its fallback (VKBRIDGE_COMPILE_FALLBACKS=gsfold) and is remembered; 3 the next server
# takes the remembered fallback at once; 4 a never-ending UE4-LUT-shaped fragment compile gets the
# neutral LUT stand-in, whose texel values are checked. The picture is checked each time
# (gsfold_check). Prints "OK: ..." / "FAILED: ..." last.
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
if [ $fails = 0 ]; then echo "OK: 4 steps"; else echo "FAILED: $fails of 4 steps"; exit 1; fi
