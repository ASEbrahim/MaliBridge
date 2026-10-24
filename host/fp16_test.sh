#!/bin/bash
# fp16_hide (MK-20: server_errata_fp16.c) and its probe fp16_math (server_probe.c) on the desktop
# driver, with a server of its own per step (socket, cache directory, session folder); what the
# app is shown is fp16_check's (tests/fp16_check.c) first GPU:
#   1  as it is: fp16_math passes (every sub-case), fp16_hide stays off, the features and the
#      16-bit float controls are the driver's own (fp16_check on the driver directly prints the
#      same), and a device with shaderFloat16 enabled keeps it;
#   2  a driver that computes 16-bit floats wrong (VKBRIDGE_ERRATA_IMITATE=fp16_hide, performance
#      mode: the probe alone decides): fp16_math fails with the bug's pattern and names the
#      sub-cases, fp16_hide turns on, shaderFloat16 and the 16-bit float controls are hidden while
#      the 16-bit storage features (storageBuffer16BitAccess: DXVK 3 requires it), shaderInt8 and
#      shaderInt16 stay as the driver's, an app that enables shaderFloat16 anyway has it (only it)
#      taken off its device, DXVK's device creation succeeds (fp16_check dxvk; and d3d11_check
#      through DXVK when VKB_WINE_DIR is set), and the report's errata table shows the failed probe;
#   3  the same with VKBRIDGE_NO_FP16=0: offered again, the log says what it overrode;
#   4  VKBRIDGE_NO_FP16=1 on a right driver: hidden, 16-bit storage kept, DXVK's device made;
#   5  VKBRIDGE_NO_FP16=1 in one game's preset: hidden for that game only.
# Prints one line per step and "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
work=$(mktemp -d)
sock=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-fp16-$$.sock
icd=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json}
srv=
log=
stop() { [ -n "$srv" ] && kill $srv 2>/dev/null && wait $srv 2>/dev/null; srv=; }
start() { # start <name> [env...]: a server of its own, cache $work/<name>/c, log $log
  local d=$work/$1
  shift
  stop
  rm -f "$sock"
  mkdir -p "$d/c/presets" "$d/s"
  log=$d/s/vkbridge-server.log
  : > "$log"
  [ -n "$PRESET" ] && echo "$PRESET" > "$d/c/presets/4242.env"
  env -u VKBRIDGE_FAKE_MISSING -u VKBRIDGE_HIDE_EXTS -u VKBRIDGE_ERRATA -u VKBRIDGE_ERRATA_IMITATE -u VKBRIDGE_NO_FP16 \
      -u VKBRIDGE_MODE -u VKBRIDGE_FAKE_DRIVER_VERSION -u VKBRIDGE_ERRATA_PROBE -u VKBRIDGE_PROBE_FAKE -u VKBRIDGE_WAIT_FAKE_ERROR \
      VKBRIDGE_PROBE_BUDGET_S=0 "$@" VK_DRIVER_FILES=$icd VK_ICD_FILENAMES= \
      "$out/vkbridge-server" --socket "$sock" --log "$log" --cache-dir "$d/c" >/dev/null 2>&1 &
  srv=$!
  for _ in $(seq 300); do [ -S "$sock" ] && break; kill -0 $srv 2>/dev/null || break; sleep 0.05; done
}
# check [env...]: fp16_check enable through the bridge; $got: its output, $new: the log lines it added
check() {
  local l; l=$(wc -l < "$log")
  got=$(timeout 60 env "$@" "$out/fp16_check" enable 2>&1)
  new=$(tail -n +$((l+1)) "$log")
}
# dxvk: DXVK 3's device creation through the bridge with fp16 hidden (storageBuffer16BitAccess
# required and kept, shaderFloat16 off, nothing taken off the device); $dx: its line
dxvk() {
  local l; l=$(wc -l < "$log")
  dx=$(timeout 60 "$out/fp16_check" dxvk 2>&1 | grep '^dxvk:')
  echo "$dx" | grep -q "^dxvk: storageBuffer16BitAccess (required) offered, shaderFloat16 (optional) off; device: 0$" &&
    ! tail -n +$((l+1)) "$log" | grep -q "taken off its device"
}
field() { echo "$got" | grep -o "$1=[0-9]*" | head -1 | cut -d= -f2; }
prof() { cat "$work/$1"/c/probe-*.json 2>/dev/null | grep -o '"name": "fp16_math", "status": "[a-z]*", [^}]*' | head -1; }
hidden() { # shaderFloat16 and the fp16 controls off; the 16-bit storage (storageBuffer16BitAccess on: DXVK
  # requires it), shaderInt8/16 and the fp32 controls as on the driver; the device made
  for f in v12.shaderFloat16 ext.shaderFloat16 shaderDenormPreserveFloat16 shaderRoundingModeRTEFloat16; do
    [ "$(field $f)" = 0 ] || return 1
  done
  [ "$(field v11.storageBuffer16BitAccess)" = 1 ] && [ "$(field ext.storageBuffer16BitAccess)" = 1 ] || return 1
  for f in v11.storageBuffer16BitAccess v11.uniformAndStorageBuffer16BitAccess v11.storagePushConstant16 ext.storageBuffer16BitAccess \
           v12.shaderInt8 ext.shaderInt8 shaderInt16 shaderDenormPreserveFloat32; do [ "$(field $f)" = "$(echo "$native" | grep -o "$f=[0-9]*" | head -1 | cut -d= -f2)" ] || return 1; done
  echo "$got" | grep -q "^device: .*: 0$"
}
trap 'stop; rm -f "$sock"; [ -n "$KEEP" ] && echo "kept $work" || rm -rf "$work"' EXIT
export VKBRIDGE_SOCKET=$sock VK_ICD_FILENAMES=
unset VKBRIDGE_HIDE_EXTS VKBRIDGE_MAX_API VKBRIDGE_FAKE_MISSING SteamAppId
fails=0 steps=0
step() { steps=$((steps+1)); if [ "$1" = 0 ]; then echo "  ok   $2"; else echo "  FAIL $2"; fails=$((fails+1)); fi; }

native=$(VK_DRIVER_FILES=$icd timeout 60 "$out/fp16_check" 2>&1)
export VK_DRIVER_FILES=$out/vkbridge_icd.json
case "$native" in *v12.shaderFloat16=1*v11.storageBuffer16BitAccess=1*) ;; *) echo "OK: 0 steps (SKIP: the desktop driver has no shaderFloat16: $native)"; exit 0 ;; esac

# 1
start plain
check
base=$got
p=$(prof plain)
echo "$p" | grep -q '"status": "pass", .*"cases": "0x1f", "failed": "0x0"' && ! echo "$new" | grep -q "fp16_hide ON" &&
  [ "$(echo "$got" | grep '^features')" = "$(echo "$native" | grep '^features')" ] &&
  [ "$(echo "$got" | grep '^properties' | sed 's/^[^:]*: [^:]*://')" = "$(echo "$native" | grep '^properties' | sed 's/^[^:]*: [^:]*://')" ] &&
  echo "$got" | grep -q "^device: .*: 0$" && ! echo "$new" | grep -q "taken off its device"
step $? "as it is: fp16_math $(echo "$p" | grep -o '"status": "[a-z]*"'), fp16 offered as the driver does ($(echo "$got" | grep -o 'v12.shaderFloat16=[0-9]*'), $(echo "$got" | grep -o 'v11.storageBuffer16BitAccess=[0-9]*')), the device keeps it"

# 2
start imitated VKBRIDGE_MODE=performance VKBRIDGE_ERRATA_IMITATE=fp16_hide
check
p=$(prof imitated)
echo "$p" | grep -q '"status": "fail", .*"want": "0x8000", "cases": "0x1f", "failed": "0x13"' &&
  grep -q "probe: fp16_math fail (.*case 0 lanes.*): the bug's pattern (MK-20): 16-bit float arithmetic wrong; shaderFloat16 hidden from clients" "$log"
step $? "the bug imitated: fp16_math fails with the bug's pattern, sub-cases fma, range, chain (failed $(echo "$p" | grep -o '"failed": "[0-9a-fx]*"'))"
echo "$new" | grep -q "errata: fp16_hide ON (probe fp16_math failed; performance mode) - MK-20" &&
  grep -q "errata: fp16_hide: shaderFloat16 and the 16-bit float controls hidden from clients, 16-bit storage kept (probe fp16_math failed; performance mode) - MK-20" "$log" && hidden
step $? "  fp16_hide on: $(echo "$got" | grep -o 'v12.shaderFloat16=[0-9]*\|ext.shaderFloat16=[0-9]*\|v11.storageBuffer16BitAccess=[0-9]*\|ext.storageBuffer16BitAccess=[0-9]*\|shaderInt16=[0-9]*\|shaderDenormPreserveFloat16=[0-9]*' | tr '\n' ' ')"
echo "$new" | grep -q "errata: fp16_hide: the app enabled shaderFloat16 anyway; taken off its device (probe fp16_math failed; performance mode) - MK-20"
step $? "  an app that enables shaderFloat16 and storageBuffer16BitAccess anyway: shaderFloat16 taken off, the storage kept, the device made ($(echo "$got" | grep '^device'))"
dxvk
step $? "  DXVK's device creation with fp16_hide on: $dx"
if [ -n "${VKB_WINE_DIR:-}" ] && [ -d "$VKB_WINE_DIR/app" ]; then
  ./d3d11_test.sh > "$work/d3d.log" 2>&1
  grep -q '^OK' "$work/d3d.log"
  step $? "  d3d11_check via DXVK with fp16_hide on: $(grep -E '^(OK|FAILED)' "$work/d3d.log" | head -1)"
fi
rep=$work/imitated/s/malibridge-report.md
for _ in $(seq 100); do grep -q "^| fp16_hide" $rep 2>/dev/null && break; sleep 0.1; done
grep -q "^| fp16_hide | MK-20 | fp16_math | \*\*fail (fma, range, chain)\*\* | " $rep
step $? "  the report: $(grep -m1 '^| fp16_hide' $rep | cut -c1-140)"

# 3
start imitated0 VKBRIDGE_MODE=performance VKBRIDGE_ERRATA_IMITATE=fp16_hide VKBRIDGE_NO_FP16=0
check
[ "$(echo "$got" | grep '^features')" = "$(echo "$base" | grep '^features')" ] &&
  echo "$new" | grep -q "errata: fp16_hide OFF (VKBRIDGE_NO_FP16=0; it would be on: probe fp16_math failed; performance mode)"
step $? "VKBRIDGE_NO_FP16=0 over a failed probe: offered ($(echo "$got" | grep -o 'v12.shaderFloat16=[0-9]*')); $(echo "$new" | grep -o 'errata: fp16_hide OFF.*' | head -1)"

# 4
start forced VKBRIDGE_NO_FP16=1
check
echo "$new" | grep -q "errata: fp16_hide ON (VKBRIDGE_NO_FP16=1; probe fp16_math passed) - MK-20" && hidden
step $? "VKBRIDGE_NO_FP16=1 on a right driver: hidden ($(echo "$got" | grep -o 'v12.shaderFloat16=[0-9]*\|v11.storageBuffer16BitAccess=[0-9]*' | tr '\n' ' ')$(echo "$new" | grep -o 'errata: fp16_hide ON.*' | head -1))"
dxvk
step $? "  DXVK's device creation: $dx"

# 5
PRESET=VKBRIDGE_NO_FP16=1 start preset
check SteamAppId=4242
game=$got g_new=$new
check SteamAppId=4343
other=$got
got=$game
hidden && [ "$(echo "$other" | grep '^features')" = "$(echo "$base" | grep '^features')" ] && echo "$g_new" | grep -q "preset for steam4242: VKBRIDGE_NO_FP16=1"
step $? "one game's preset: that game $(echo "$game" | grep -o 'v12.shaderFloat16=[0-9]*'), another $(echo "$other" | grep -o 'v12.shaderFloat16=[0-9]*')"

if [ $fails = 0 ]; then echo "OK: $steps steps"; else echo "FAILED: $fails of $steps steps"; exit 1; fi
