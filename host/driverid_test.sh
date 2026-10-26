#!/bin/bash
# Arm's driver told by driverID, not by the vendor ID (server_driverid.c): Mesa PanVK reports
# vendor 0x13B5 like Arm's driver, with a Mesa driverVersion (26.3.0, which decodes as "r26p3",
# inside the errata's ranges). VKBRIDGE_FAKE_DRIVER imitates each beneath the server on the desktop
# driver; vkbridge-smoke through the bridge each time. A server of its own per step:
#   0  the desktop driver as it is: the errata on for every driver (safe mode's "always") recorded;
#   1  panvk: driverID MESA_PANVK logged; the errata say "not Arm's driver" and only step 0's are on
#      (none from the version gates); no Mali SPIR-V fixes, no shl_barrier; the client passes the
#      dedicated hint on;
#   2  arm (ARM_PROPRIETARY, r44p1): Arm's driver; errata on from the r44p1 ranges; SPIR-V fixes and
#      shl_barrier on; the client clears the dedicated hint;
#   3  arm11 (Vulkan 1.1 without VK_KHR_driver_properties): driverID unknown, the vendor ID decides
#      as before: the same as step 2.
# Prints one line per step and "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
work=$(mktemp -d)
cache=$work/c
sock=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-driverid-$$.sock
log=$work/server.log
srv=
stop() { [ -n "$srv" ] && kill $srv 2>/dev/null && wait $srv 2>/dev/null; srv=; }
# start <VAR=value...>: a new server with these in its environment, a fresh log and cache
start() {
  stop
  rm -rf "$sock" "$log" "$cache"
  mkdir -p "$cache"
  env -u VKBRIDGE_FAKE_MISSING -u VKBRIDGE_HIDE_EXTS -u VKBRIDGE_ERRATA -u VKBRIDGE_ERRATA_IMITATE -u VKBRIDGE_MODE \
      -u VKBRIDGE_FAKE_DRIVER_VERSION -u VKBRIDGE_FAKE_DRIVER -u VKBRIDGE_ERRATA_PROBE -u VKBRIDGE_SPIRV_FIX -u VKBRIDGE_SHL_BARRIER \
      -u VKBRIDGE_YCBCR_SWAP -u VKBRIDGE_FAKE_API "$@" \
      VK_DRIVER_FILES=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json} VK_ICD_FILENAMES= \
      "$out/vkbridge-server" --socket "$sock" --log "$log" --cache-dir "$cache" >/dev/null 2>&1 &
  srv=$!
  for _ in $(seq 100); do [ -S "$sock" ] && break; sleep 0.1; done
}
trap 'stop; rm -f "$sock"; [ -n "$KEEP" ] || rm -rf "$work"' EXIT
export VKBRIDGE_SOCKET=$sock VK_DRIVER_FILES=$out/vkbridge_icd.json VK_ICD_FILENAMES=
unset VKBRIDGE_HIDE_EXTS VKBRIDGE_MAX_API VKBRIDGE_DEDICATED_HINT
fails=0 steps=0
step() { steps=$((steps+1)); if [ "$1" = 0 ]; then echo "  ok   $2"; else echo "  FAIL $2"; fails=$((fails+1)); fi; }
# smoke: vkbridge-smoke through the server; rc, res (its last line), app (its output)
smoke() {
  VKBRIDGE_LOG_LEVEL=2 timeout 120 "$out/vkbridge-smoke" >$work/run.log 2>&1
  rc=$?
  res=$(tail -1 $work/run.log)
  app=$(cat $work/run.log)
  sleep 0.2
}
summary() { grep -o "errata: device [0-9]*, .*: on [^;]*" "$log" | head -1; }
# errata turned on by the version gates (the driver in an affected range)
ranged() { grep "errata: [a-z_0-9]* ON (" "$log" | grep -c "r44p1\|r26p3\|in range\|affected"; }

on_list() { summary | sed 's/.*: on //'; }

# 0
start
smoke
if [ $rc = 77 ]; then echo "SKIP: $res"; exit 0; fi
base_on=$(on_list)
[ $rc = 0 ] && [ -n "$base_on" ] && grep -q "GPU driver: driverID [0-9]* (MESA_RADV\|MESA_LLVMPIPE\|other): not Arm's driver$" "$log"
step $? "the desktop driver: not Arm's ($res; on: $base_on)"
# 1
start VKBRIDGE_FAKE_DRIVER=panvk
smoke
[ $rc = 0 ] && grep -q "imitating Mesa PanVK 26.3.0 (VKBRIDGE_FAKE_DRIVER=panvk)" "$log" &&
  grep -q "GPU driver: driverID 20 (MESA_PANVK): not Arm's driver although the vendor is Arm; Arm's errata and fixes stay off" "$log" &&
  summary | grep -q "not Arm's driver (driverID MESA_PANVK)" && [ "$(ranged)" = 0 ] && [ "$(on_list)" = "$base_on" ] &&
  ! grep -q "Mali SPIR-V fixes on\|shl_barrier on" "$log" &&
  echo "$app" | grep -q "dedicated allocation hint: the driver's answer passed through"
step $? "panvk: Arm's errata and fixes off, the same errata as the desktop driver ($res; on: $(on_list))"
# 2
start VKBRIDGE_FAKE_DRIVER=arm
smoke
arm_on=$(on_list)
[ $rc = 0 ] && grep -q "GPU driver: driverID 9 (ARM_PROPRIETARY): Arm's driver; its errata and fixes apply" "$log" &&
  summary | grep -q "driver r44p1" && [ "$(ranged)" -gt 0 ] && [ "$arm_on" != "$base_on" ] &&
  grep -q "Mali SPIR-V fixes on, .*shl_barrier on" "$log" &&
  echo "$app" | grep -q "dedicated allocation hint: prefersDedicatedAllocation cleared"
step $? "arm: Arm's errata and fixes on ($res; on: $arm_on)"
# 3
start VKBRIDGE_FAKE_DRIVER=arm11
smoke
[ $rc = 0 ] && grep -q "GPU driver: driverID unknown (Vulkan 1.1, no VK_KHR_driver_properties): Arm's driver by the vendor ID" "$log" &&
  summary | grep -q "driver r44p1" && [ "$(on_list)" = "$arm_on" ] &&
  grep -q "Mali SPIR-V fixes on, .*shl_barrier on" "$log" &&
  echo "$app" | grep -q "dedicated allocation hint: prefersDedicatedAllocation cleared"
step $? "arm11, no driverID: the vendor ID decides, as step 2 ($res)"

if [ $fails = 0 ]; then echo "OK: $steps steps"; else echo "FAILED: $fails of $steps steps"; exit 1; fi
