#!/bin/bash
# server_emu.c's extended dynamic state hiding and render-pass emulation per driver version, a server per step.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
work=$(mktemp -d)
cache=$work/c
mkdir -p "$cache"
sock=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-a1zink-$$.sock
log=$work/server.log
srv=
start() {
  [ -n "$srv" ] && kill $srv 2>/dev/null && wait $srv 2>/dev/null
  rm -f "$sock"
  : > "$log"
  env -u VKBRIDGE_FAKE_MISSING -u VKBRIDGE_HIDE_EXTS -u VKBRIDGE_FAKE_API -u VKBRIDGE_PROMOTE_API -u VKBRIDGE_PROMOTE_TESTED -u VKBRIDGE_MODE \
      -u VKBRIDGE_EMU_DYNREND -u VKBRIDGE_A1_ZINK -u VKBRIDGE_FAKE_DRIVER_VERSION -u VKBRIDGE_SERVER_HIDE_EXTS -u VKBRIDGE_STEAM_API_CAP "$@" \
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

exts() { VK_DRIVER_FILES=$out/vkbridge_icd.json timeout 60 vulkaninfo 2>/dev/null | grep -o 'VK_[A-Za-z0-9_]*' > $work/exts.txt; }
has() { grep -qx "$1" $work/exts.txt; }
once() { [ "$(grep -cx "$1" $work/exts.txt)" -ge 1 ] && [ "$(sort $work/exts.txt | uniq -c | awk -v e="$1" '$2==e{print $1}')" = "$(sort $work/exts.txt | uniq -c | awk '$2=="VK_KHR_swapchain"{print $1}')" ]; }
line() { grep -c "a1: VK_EXT_extended_dynamic_state not offered and dynamic rendering made as render passes" "$log"; }

# 1
start VKBRIDGE_FAKE_DRIVER_VERSION=r38p1:jm
exts
run env VKBRIDGE_MAX_API=1.2 "$out/dynrend_check"
stop
[ "$(line)" = 1 ] && ! has VK_EXT_extended_dynamic_state && has VK_EXT_extended_dynamic_state2 && once VK_KHR_dynamic_rendering &&
  [ $rc = 0 ] && echo "$res" | grep -q "^OK: 12 of 12" && grep -q "dynamic rendering emulated on device" "$log"
step $? "r38p1 job manager: on ($(grep -o 'a1: .*(Arm[^;]*' "$log" | head -1)); $res"
# 2
start VKBRIDGE_FAKE_DRIVER_VERSION=r38p1:jm VKBRIDGE_A1_ZINK=0
exts
stop
[ "$(line)" = 0 ] && has VK_EXT_extended_dynamic_state && ! grep -q "dynamic rendering emulated on device" "$log"
step $? "r38p1 job manager, VKBRIDGE_A1_ZINK=0: off"
# 3
start VKBRIDGE_FAKE_DRIVER_VERSION=r40p0:jm
exts
stop
[ "$(line)" = 1 ] && ! has VK_EXT_extended_dynamic_state
step $? "r40p0 job manager (RG556): on"
# 4
start VKBRIDGE_FAKE_DRIVER_VERSION=r44p1
exts
stop
[ "$(line)" = 0 ] && has VK_EXT_extended_dynamic_state
step $? "r44p1 (CSF): off"

# 5
start VKBRIDGE_FAKE_DRIVER_VERSION=r38p1-01bet0:jm VKBRIDGE_FAKE_API=1.1
exts
stop
[ "$(line)" = 1 ] && ! has VK_EXT_extended_dynamic_state && grep -q "Vulkan 1.1 to apps, VK_EXT_extended_dynamic_state offered" "$log" &&
  ! grep -q "api cap:" "$log"
step $? "r38p1 beta job manager at Vulkan 1.1 (#11's S10 / Note10+): on, nothing capped"
# 6
start VKBRIDGE_FAKE_DRIVER_VERSION=r54p1:jm
exts
stop
[ "$(line)" = 0 ] && has VK_EXT_extended_dynamic_state && ! grep -q "dynamic rendering emulated on device" "$log"
step $? "r54p1 job manager (Redmi G57): off"
# 7
start VKBRIDGE_FAKE_DRIVER_VERSION=r38p0:jm VKBRIDGE_FAKE_API=1.1 VKBRIDGE_SERVER_HIDE_EXTS=VK_EXT_extended_dynamic_state
exts
stop
[ "$(line)" = 0 ] && ! grep -q "dynamic rendering emulated on device" "$log"
step $? "r38p0 job manager at Vulkan 1.1 without the extension (the S21): off"
# 8
start VKBRIDGE_FAKE_DRIVER_VERSION=unknown:jm
exts
stop
[ "$(line)" = 1 ] && ! has VK_EXT_extended_dynamic_state
step $? "unknown release on a job manager GPU: on"

if [ $fails = 0 ]; then echo "OK: $steps steps"; else echo "FAILED: $fails of $steps steps"; exit 1; fi
