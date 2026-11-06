#!/bin/bash
# The capability check before the driver (server_guard.c): a pipeline whose stage reaches the driver
# with a clip or cull distance built-in, on a driver without shaderClipDistance / shaderCullDistance.
# ULTRAKILL (Mali r44p1: 0x40) and No Man's Sky (a player log G925 r49p1: 0x18 x3 G615 r44p1: 0x40 x3,
# device poisoned, then the server died in a draw) both died in Mali's compiler that way, each time
# after the bridge's clip rewrite had not taken the built-in out. The driver is now never handed one:
# the pipeline is refused (VK_ERROR_INITIALIZATION_FAILED, a failed pipeline as DXVK/vkd3d know it).
# The server imitates the driver: VKBRIDGE_FAKE_MISSING=clip,cull (no clip/cull distance) and
# VKBRIDGE_TEST_CRASH_PIPELINE=clip (a stage with such a built-in faults at 0x40 inside the driver's
# compile, as Mali's compiler did). VKBRIDGE_EMU_CLIP=0 VKBRIDGE_EMU_CULL=0 leave the built-in in (the
# rewrite failing, as for NMS before row 186).
#   1  the clip pipeline is refused before the driver: no contained crash, the refusal line names
#      ClipDistance and shaderClipDistance; four programs in a row, none poisons anything;
#   2  with the emulation on (the default) the same shader is rewritten, built and clips correctly:
#      emulate first, refuse only what nothing takes;
#   3  and smoke's pipelines (glslang's gl_PerVertex declares clip/cull distances too) are built and draw;
#   4  A/B: with VKBRIDGE_CAP_GATE=0 the imitated compiler fault happens (contained in the compile:
#      NMS's "CONTAINED driver crash ... in vkCreateGraphicsPipelines").
# Prints one line per step and "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
corpus=$PWD/../tests/corpus
work=$(mktemp -d)
srv=
trap '[ -n "$srv" ] && kill $srv 2>/dev/null; rm -f "$work"/*.sock; [ -n "$KEEP" ] && echo "kept $work" || rm -rf "$work"' EXIT
fails=0 steps=0
step() { steps=$((steps+1)); if [ "$1" = 0 ]; then echo "  ok   $2"; else echo "  FAIL $2"; fails=$((fails+1)); fi; }

start() {
  local name=$1; shift
  sock=$work/$name.sock
  env VKBRIDGE_PROBE=0 VKBRIDGE_FAKE_MISSING=clip,cull VKBRIDGE_TEST_CRASH_PIPELINE=clip "$@" \
      VK_DRIVER_FILES=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json} VK_ICD_FILENAMES= \
      "$out/vkbridge-server" --socket "$sock" --log "$work/$name.log" --cache-dir "$work/c-$name" >/dev/null 2>&1 &
  srv=$!
  for _ in $(seq 50); do [ -S "$sock" ] && break; sleep 0.1; done
}
client() { VKBRIDGE_SOCKET=$sock VK_DRIVER_FILES=$out/vkbridge_icd.json VK_ICD_FILENAMES= timeout ${T:-60} "$@"; }
clip() { client "$out/clip_pipeline" "$corpus/clip_vs_block.spv" - "$corpus/clip_fs.spv"; }
stop() { kill $srv 2>/dev/null; wait $srv 2>/dev/null; srv=; }

start gate VKBRIDGE_EMU_CLIP=0 VKBRIDGE_EMU_CULL=0
for i in 1 2 3 4; do clip >"$work/gate-$i.out" 2>&1; done
sleep 0.2
ref=$(grep -m1 "capability check: .*refused before the driver" "$work/gate.log" | sed 's/.*vkbridge-server E //')
kill -0 $srv 2>/dev/null && ! grep -q "CONTAINED driver crash\|crashed with signal\|poisoned" "$work/gate.log" &&
  echo "$ref" | grep -q "ClipDistance (shaderClipDistance)" && [ "$(grep -c 'capability check: .*refused before the driver' "$work/gate.log")" -ge 1 ] &&
  ! grep -q "faulting on purpose" "$work/gate.log"
step $? "1 refused before the driver, nothing contained or poisoned: ${ref:0:260}"
stop

start emu
clip >"$work/emu.out" 2>&1; r=$?
sleep 0.2
[ $r = 0 ] && ! grep -q "capability check: .*refused before the driver\|CONTAINED driver crash\|faulting on purpose" "$work/emu.log"
step $? "2 with the emulation on: rewritten, built, clipped (exit $r: $(tail -1 "$work/emu.out"))"
client "$out/vkbridge-smoke" >"$work/smoke.out" 2>&1 && ! grep -q "capability check: .*refused before the driver\|CONTAINED driver crash\|faulting on purpose" "$work/emu.log"
step $? "3 smoke's pipelines are built and draw on the same server: $(tail -1 "$work/smoke.out")"
stop

start ab VKBRIDGE_EMU_CLIP=0 VKBRIDGE_EMU_CULL=0 VKBRIDGE_CAP_GATE=0
clip >"$work/ab.out" 2>&1
sleep 0.2
line=$(grep -m1 "CONTAINED driver crash" "$work/ab.log" | sed 's/.*vkbridge-server E //')
echo "$line" | grep -q "signal 11 (address 0x40) in vkCreateGraphicsPipelines" && grep -q "faulting on purpose (imitated Mali compiler)" "$work/ab.log"
step $? "4 A/B: without the check the imitated Mali compiler faults: ${line:0:200}"
stop

if [ $fails = 0 ]; then echo "OK: $steps steps"; else echo "FAILED: $fails of $steps steps"; [ -n "$KEEP" ] || for f in "$work"/*.log; do echo "--- $f"; grep -E " E | W " "$f" | tail -6; done; fi
[ $fails = 0 ]
