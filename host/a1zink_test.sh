#!/bin/bash
# The Steam-mode black screen's fix (server_emu.c a1_decide/#15) on the desktop driver,
# taken as Arm's (VKBRIDGE_FAKE_DRIVER_VERSION), a server of its own for each step:
#   1  r38p1 job manager: one "a1:" line, VK_EXT_extended_dynamic_state not listed (its 2 and 3 are),
#      VK_KHR_dynamic_rendering listed once, dynrend_check passes through the render-pass emulation;
#   2  the same with VKBRIDGE_A1_ZINK=0: no line, VK_EXT_extended_dynamic_state listed;
#   3  r40p0 job manager (the RG556): on;
#   4  r44p1 (CSF, our tablets): off;
#   5  r38p1 beta on a job manager GPU taken as a Vulkan 1.1 driver (Galaxy S10 / Note10+ phones, G76
#      r38p1-01bet0, Vulkan 1.1 to apps, the extension offered): on (the cap there: apicap_test step 5b);
#   6  r54p1 on a job manager GPU (a player log's Redmi G57 MC2, Big Picture shown without the fix): off;
#   7  r38p0 on a job manager GPU at Vulkan 1.1 without the extension (the Galaxy S21, Big Picture shown):
#      off (the extension hidden from the server with VKBRIDGE_SERVER_HIDE_EXTS stands in for its driver);
#   8  unknown release (no version in driverInfo) on a job manager GPU: on;
#   9  r38p1 on a CSF GPU (Dimensity 9200's Immortalis-G715, a player log): on, the line says CSF,
#      dynrend_check passes through the render-pass emulation;
#  10  r43p0 on a CSF GPU: on; unknown release on a CSF GPU: off.
#  11  r38p1 job manager, a game's process (not Zink, not one of Steam mode's): the line is there, but it keeps
#      VK_EXT_extended_dynamic_state and the driver's VK_KHR_dynamic_rendering, its device has no render-pass
#      emulation and dynrend_check passes on the driver's; VKBRIDGE_A1_ZINK=1 gives it the fix too.
# Steps 1-10 list and draw as "steam" and "Xwayland" (links to the tools), the processes the fix is for.
# Prints one line per step and "OK: ..." / "FAILED: ..." last.
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

mkdir -p $work/bin
ln -s "$(command -v vulkaninfo)" $work/bin/steam
ln -s "$out/dynrend_check" $work/bin/Xwayland
exts() { VK_DRIVER_FILES=$out/vkbridge_icd.json timeout 60 "${1:-$work/bin/steam}" 2>/dev/null | grep -o 'VK_[A-Za-z0-9_]*' > $work/exts.txt; }
has() { grep -qx "$1" $work/exts.txt; }
once() { [ "$(grep -cx "$1" $work/exts.txt)" -ge 1 ] && [ "$(sort $work/exts.txt | uniq -c | awk -v e="$1" '$2==e{print $1}')" = "$(sort $work/exts.txt | uniq -c | awk '$2=="VK_KHR_swapchain"{print $1}')" ]; }
line() { grep -c "a1: VK_EXT_extended_dynamic_state not offered and dynamic rendering made as render passes" "$log"; }

# 1
start VKBRIDGE_FAKE_DRIVER_VERSION=r38p1:jm
exts
run env VKBRIDGE_MAX_API=1.2 "$work/bin/Xwayland"
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
[ "$(line)" = 1 ] && ! has VK_EXT_extended_dynamic_state && grep -q "Vulkan 1.1 to apps, VK_EXT_extended_dynamic_state offered" "$log"
step $? "r38p1 beta job manager at Vulkan 1.1 (S10 / Note10+): on (its cap: apicap_test step 5b)"
# 6
start VKBRIDGE_FAKE_DRIVER_VERSION=r54p1:jm
exts
stop
[ "$(line)" = 0 ] && has VK_EXT_extended_dynamic_state && ! grep -q "dynamic rendering emulated on device" "$log"
step $? "r54p1 job manager (a player log's Redmi G57): off"
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

# 9
start VKBRIDGE_FAKE_DRIVER_VERSION=r38p1:csf
exts
run env VKBRIDGE_MAX_API=1.2 "$work/bin/Xwayland"
stop
[ "$(line)" = 1 ] && ! has VK_EXT_extended_dynamic_state && has VK_EXT_extended_dynamic_state2 && once VK_KHR_dynamic_rendering &&
  grep -q "a1: .*(Arm driver r38, CSF, " "$log" &&
  [ $rc = 0 ] && echo "$res" | grep -q "^OK: 12 of 12" && grep -q "dynamic rendering emulated on device" "$log"
step $? "r38p1 CSF (G715): on ($(grep -o 'a1: .*(Arm[^;]*' "$log" | head -1)); $res"
# 10
start VKBRIDGE_FAKE_DRIVER_VERSION=r43p0:csf
exts
stop
[ "$(line)" = 1 ] && ! has VK_EXT_extended_dynamic_state
step $? "r43p0 CSF: on"
start VKBRIDGE_FAKE_DRIVER_VERSION=unknown:csf
exts
stop
[ "$(line)" = 0 ] && has VK_EXT_extended_dynamic_state
step $? "unknown release on a CSF GPU: off"

# 11
start VKBRIDGE_FAKE_DRIVER_VERSION=r38p1:jm
exts vulkaninfo
run env VKBRIDGE_MAX_API=1.2 "$out/dynrend_check"
stop
[ "$(line)" = 1 ] && has VK_EXT_extended_dynamic_state && once VK_KHR_dynamic_rendering &&
  [ $rc = 0 ] && echo "$res" | grep -q "^OK: " && ! grep -q "dynamic rendering emulated on device" "$log"
step $? "r38p1 job manager, a game's process: the driver's extended dynamic state and dynamic rendering; $res"
start VKBRIDGE_FAKE_DRIVER_VERSION=r38p1:jm VKBRIDGE_A1_ZINK=1
exts vulkaninfo
stop
[ "$(line)" = 1 ] && ! has VK_EXT_extended_dynamic_state && once VK_KHR_dynamic_rendering
step $? "r38p1 job manager, a game's process with VKBRIDGE_A1_ZINK=1: on"

if [ $fails = 0 ]; then echo "OK: $steps steps"; else echo "FAILED: $fails of $steps steps"; exit 1; fi
