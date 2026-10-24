#!/bin/bash
# VK_KHR_dynamic_rendering as render passes (server_emu_dynrend.c; Mali's Vulkan 1.1 drivers lack it
# and Zink requires it) on the desktop driver, a server of its own for each step, dynrend_check:
#   1  as it is (the driver has it): passes, nothing emulated;
#   2  the driver taken as lacking it (VKBRIDGE_FAKE_MISSING=dynrend), the device at Vulkan 1.2
#      (VKBRIDGE_MAX_API): passes through the emulation (renderings, pipelines, a secondary counted);
#   3  the same with VKBRIDGE_EMU_DYNREND=0: not offered;
#   4  a 1.1 driver without it, promoted to 1.2 (as the G57 / G610 / G710 on r32p1 would be):
#      passes, vkCmdBindVertexBuffers2 given to the 1.2 device (Zink 26.2 binds with it).
# Prints one line per step and "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
work=$(mktemp -d)
cache=$work/c
mkdir -p "$cache"
sock=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-dynrend-$$.sock
log=$work/server.log
srv=
start() {
  [ -n "$srv" ] && kill $srv 2>/dev/null && wait $srv 2>/dev/null
  rm -f "$sock"
  : > "$log"
  env -u VKBRIDGE_FAKE_MISSING -u VKBRIDGE_HIDE_EXTS -u VKBRIDGE_FAKE_API -u VKBRIDGE_PROMOTE_API -u VKBRIDGE_PROMOTE_TESTED -u VKBRIDGE_MODE \
      -u VKBRIDGE_EMU_DYNREND "$@" \
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
run() {
  timeout 120 "$@" >$work/run.log 2>&1
  rc=$?
  res=$(tail -1 $work/run.log)
  [ $rc = 0 ] || grep '^FAIL' $work/run.log | head -3 | sed 's/^/       /'
}
counts() { grep -o 'dynamic rendering emulation, device [0-9]*: .*' "$log" | tail -1 | sed 's/.*: //'; }

# 1
start
run "$out/dynrend_check"
[ $rc = 0 ] && echo "$res" | grep -q "^OK: 12 of 12" && ! grep -q "dynamic rendering emulated" "$log"
step $? "the driver's own: $res"
# 2
start VKBRIDGE_FAKE_MISSING=dynrend
run env VKBRIDGE_MAX_API=1.2 "$out/dynrend_check"
stop
[ $rc = 0 ] && echo "$res" | grep -q "^OK: 12 of 12" && grep -q "Vulkan 1.2, VK_KHR_dynamic_rendering listed" $work/run.log &&
  grep -q "dynamic rendering emulated on device [0-9]*: renderings as render passes$" "$log" &&
  grep -q "dynamic rendering emulation, device [0-9]*: [1-9][0-9]* rendering(s), 4 pipeline(s), 1 secondary begin(s), [1-9][0-9]* render pass(es)" "$log"
step $? "emulated (VKBRIDGE_FAKE_MISSING=dynrend, Vulkan 1.2): $(counts); $res"
# 3
start VKBRIDGE_FAKE_MISSING=dynrend VKBRIDGE_EMU_DYNREND=0
run env VKBRIDGE_MAX_API=1.2 "$out/dynrend_check"
[ $rc != 0 ] && grep -q "^FAIL dynamic rendering offered: extension missing, feature 0" $work/run.log && ! grep -q "dynamic rendering emulated" "$log"
step $? "VKBRIDGE_EMU_DYNREND=0: not offered ($res)"
# 4
start VKBRIDGE_FAKE_API=1.1 VKBRIDGE_PROMOTE_API=1 VKBRIDGE_FAKE_MISSING=dynrend,VK_KHR_dynamic_rendering,VK_EXT_extended_dynamic_state
run "$out/dynrend_check"
stop
[ $rc = 0 ] && echo "$res" | grep -q "^OK: 12 of 12" && grep -q "Vulkan 1.2, VK_KHR_dynamic_rendering listed" $work/run.log &&
  grep -q "^PASS vkCmdBindVertexBuffers2" $work/run.log && grep -q "offered as Vulkan 1.2 on its Vulkan 1.1 driver" "$log" &&
  grep -q "dynamic rendering emulated on device" "$log"
step $? "a 1.1 driver without it, promoted to 1.2: $(counts); $res"

if [ $fails = 0 ]; then echo "OK: $steps steps"; else echo "FAILED: $fails of $steps steps"; exit 1; fi
