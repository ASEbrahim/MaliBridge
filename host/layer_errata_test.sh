#!/bin/bash
# The driver errata in the layer (server_errata*.c), on the desktop driver: the layer runs the
# errata in-process, so the decisions are the bridge's (host/errata_test.sh), read from the
# process's environment and <cache dir>/env (VKBRIDGE_CACHE_DIR here: a fresh one per run).
#   1  the decisions by Mali driver version (VKBRIDGE_FAKE_DRIVER_VERSION) x mode: which errata
#      are on for r44p1 (Mali-G720), r49p1 (Mali-G925, given as its beta driverInfo string), r54p2,
#      r54p3, a job manager GPU, an Arm driver whose version cannot be read; clamp_zero (MK-32) on
#      r49p1 only for a beta build and only in safe mode, on r51p0, off from r52p0; cull_nontri
#      (MK-16) always in safe mode, below r52 in performance mode; dedicated_import (MK-39) below
#      r54p1 and for an unknown version; stencil_layout (MK-11) r41p0-r54p0; dyn_reemit (MK-04) below r44p1 in performance mode;
#   2  VKBRIDGE_ERRATA=+name,-name forces, an unknown name is warned about;
#   3  the no-op example fix with its imitation beneath it, on a device whose null descriptors are
#      emulated: the layer's own image views (the stand-ins; the app makes none) go through both.
#
#   4  the bridge's query and device-creation fixes in the layer: fp16_hide (MK-20) and gs_hide
# shown and enforced (fp16_check, gshide_check), the extension hiding
#      (VKBRIDGE_SERVER_HIDE_EXTS, vulkaninfo); a driver crash inside a pipeline compile fails that
#      call (crash_contain_check with VKBRIDGE_TEST_CRASH_PIPELINE=1), the process's own faults
#      still reach its own handler (layer_sigchain_check), and without the layer's handler
#      (VKBRIDGE_LAYER_CRASH_HANDLER=0) the same crash ends the process.
#
# Not here, unlike the bridge's test: the probe-driven errata (the layer runs no startup probe),
# the "same decisions again" short line (one device per process in the layer), and the protocol
# hash (the bridge's own).
#
# Prints one line per step and "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
build=${VKB_BUILD:-$PWD/../build-linux-x86-64}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
export VKBRIDGE_CACHE_DIR=$work/cache # the layer's cache directory (its env file, the stuck-pipeline list): this run's own
fails=0 steps=0
step() { steps=$((steps+1)); if [ "$1" = 0 ]; then echo "  ok   $2"; else echo "  FAIL $2"; fails=$((fails+1)); fi; }

lib=$build/libVkLayer_malibridge_compat.so
if [ ! -f "$lib" ]; then
  echo "no layer at $lib; build it first:" >&2
  echo "  cmake -B build-linux-x86-64 -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_BUILD_TYPE=Release && cmake --build build-linux-x86-64" >&2
  exit 1
fi
# The manifest needs an absolute library_path: the loader does not resolve a relative one against
# the manifest's own directory.
manifest=$work/VkLayer_malibridge_compat.json
cat > "$manifest" <<EOF
{
    "file_format_version": "1.1.2",
    "layer": {
        "name": "VK_LAYER_MALIBRIDGE_compat",
        "type": "GLOBAL",
        "library_path": "$lib",
        "api_version": "1.3.278",
        "implementation_version": "1",
        "description": "MaliBridge Compatibility & Errata Layer for Arm Mali"
    }
}
EOF
export VK_LAYER_PATH=$work
export VK_INSTANCE_LAYERS=VK_LAYER_MALIBRIDGE_compat

# One device with the given environment; the errata's decisions are in its log.
device() {
    env -u VKBRIDGE_FAKE_MISSING -u VKBRIDGE_HIDE_EXTS -u VKBRIDGE_ERRATA -u VKBRIDGE_ERRATA_IMITATE \
        -u VKBRIDGE_MODE -u VKBRIDGE_FAKE_DRIVER_VERSION -u VKBRIDGE_ERRATA_PROBE \
        "$@" timeout 120 "$out/vkbridge-smoke" >"$work/smoke.log" 2>&1
    new=$(grep -E "errata" "$work/smoke.log")
}
# The errata on, from the device's summary line
on_set() { echo "$new" | grep -o 'errata: device .*: on [^;]*' | tail -1 | sed 's/.*: on //'; }

# 1 the version matrix
matrix() { # mode version expected
    device "VKBRIDGE_MODE=$1" "VKBRIDGE_FAKE_DRIVER_VERSION=$2"
    local got; got=$(on_set)
    [ "$got" = "$3" ]
    step $? "$1 mode, $2: $got$([ "$got" = "$3" ] || echo " (want $3)")"
}
matrix safe r44p1 wait_retry,bitwise_scalar,dyn_reemit,vb_split,view_quarantine,view_counts,clamp_zero,cull_nontri,dedicated_import,stencil_layout
matrix safe v1.r49p1-03bet0 wait_retry,bitwise_scalar,dyn_reemit,vb_split,view_quarantine,view_counts,clamp_zero,cull_nontri,dedicated_import,stencil_layout
echo "$new" | grep -q "driver r49p1 beta (VKBRIDGE_FAKE_DRIVER_VERSION \"v1.r49p1-03bet0\")" &&
  echo "$new" | grep -q "errata: view_quarantine ON (always; safe mode" &&
  echo "$new" | grep -q "errata: clamp_zero ON (r49p1 in r49p1-r49p1 (a beta build: Arm's fix in r49p1 not confirmed for it); safe mode) - MK-32"
step $? "the beta driverInfo string decoded, the reasons logged ($(echo "$new" | grep -o 'errata: view_quarantine ON.*'); $(echo "$new" | grep -o 'errata: clamp_zero ON.*' | cut -c1-60)...)"
matrix safe v1.r49p1-01eac0 wait_retry,bitwise_scalar,dyn_reemit,vb_split,view_quarantine,view_counts,cull_nontri,dedicated_import,stencil_layout
matrix safe r51p0 wait_retry,bitwise_scalar,dyn_reemit,vb_split,view_quarantine,view_counts,clamp_zero,cull_nontri,dedicated_import,stencil_layout
matrix safe r52p0 wait_retry,bitwise_scalar,dyn_reemit,vb_split,view_quarantine,view_counts,cull_nontri,dedicated_import,stencil_layout
matrix safe r54p0 wait_retry,dyn_reemit,vb_split,view_quarantine,view_counts,cull_nontri,dedicated_import,stencil_layout
matrix safe r54p2 wait_retry,dyn_reemit,vb_split,view_quarantine,view_counts,cull_nontri
matrix safe r54p3 wait_retry,dyn_reemit,vb_split,view_quarantine,view_counts,cull_nontri
matrix performance r44p1 wait_retry,vb_split,view_counts,clamp_zero,cull_nontri,dedicated_import,stencil_layout
matrix performance r44p0 wait_retry,dyn_reemit,vb_split,view_counts,clamp_zero,cull_nontri,dedicated_import,stencil_layout
matrix performance r36p0 wait_retry,dyn_reemit,vb_split,view_counts,cull_nontri,dedicated_import
matrix performance v1.r49p1-03bet0 wait_retry,dyn_reemit,vb_split,view_quarantine,view_counts,cull_nontri,dedicated_import,stencil_layout
matrix performance r52p0 wait_retry,dyn_reemit,vb_split,view_counts,dedicated_import,stencil_layout
matrix performance r54p2 wait_retry,vb_split,view_counts
matrix performance r54p3 wait_retry,vb_split,view_counts
matrix performance r46p0:jm wait_retry,dyn_reemit,vb_split,view_counts,clamp_zero,cull_nontri,dedicated_import,stencil_layout
matrix performance r46p0:csf wait_retry,vb_split,view_counts,clamp_zero,cull_nontri,dedicated_import,stencil_layout
matrix performance r50p0 wait_retry,dyn_reemit,vb_split,view_quarantine,view_counts,clamp_zero,cull_nontri,dedicated_import,stencil_layout
matrix safe not-a-version wait_retry,dyn_reemit,vb_split,view_quarantine,view_counts,clamp_zero,cull_nontri,dedicated_import,stencil_layout

# 2 switches
device VKBRIDGE_FAKE_DRIVER_VERSION=r44p1 VKBRIDGE_ERRATA=+example,-view_counts,no_such_erratum
got=$(on_set)
[ "$got" = wait_retry,bitwise_scalar,dyn_reemit,vb_split,view_quarantine,clamp_zero,cull_nontri,dedicated_import,stencil_layout,example ] &&
  echo "$new" | grep -q "errata: view_counts OFF (VKBRIDGE_ERRATA=-view_counts" &&
  echo "$new" | grep -q "no erratum named no_such_erratum" && echo "$new" | grep -q "errata: example ON (VKBRIDGE_ERRATA=+example)"
step $? "VKBRIDGE_ERRATA=+example,-view_counts: $got; the unknown name warned about"

# 3 the example fix and its imitation, beneath the null-descriptor emulation.
# The null-descriptor stand-ins are the layer's own views, so they are in this run's log (the
# bridge's are the client's, in the server's log).
env -u VKBRIDGE_FAKE_MISSING -u VKBRIDGE_HIDE_EXTS -u VKBRIDGE_ERRATA -u VKBRIDGE_ERRATA_IMITATE \
    -u VKBRIDGE_MODE -u VKBRIDGE_FAKE_DRIVER_VERSION -u VKBRIDGE_ERRATA_PROBE \
    VKBRIDGE_FAKE_MISSING=robustness2 VKBRIDGE_ERRATA=+example VKBRIDGE_ERRATA_IMITATE=example \
    timeout 60 "$out/nulldesc_check" >"$work/3.log" 2>&1
r=$?
new=$(grep -E "errata|null descriptor stand-in" "$work/3.log")
calls=$(echo "$new" | grep -o 'example: [0-9]* vkCreateImageView call(s) through the fix, [0-9]* through' | tail -1)
fix=$(echo "$calls" | grep -o 'example: [0-9]*' | grep -o '[0-9]*'); imi=$(echo "$calls" | grep -o ', [0-9]* through' | grep -o '[0-9]*')
# The same count as the bridge's (the stand-ins are the layer's own views here, the client's there).
[ $r = 0 ] && echo "$new" | grep -q "errata: example bug imitated beneath the fix" && [ "${fix:-0}" -ge 6 ] && [ "$fix" = "$imi" ] &&
  [ "$(echo "$new" | grep -c 'null descriptor stand-in made')" -ge 6 ]
step $? "the layer's own views through the fix and the imitation beneath: ${fix:-0} and ${imi:-0} ($(tail -1 $work/3.log))"

# 4 the query and device-creation fixes, and crash containment
clean_env="-u VKBRIDGE_FAKE_MISSING -u VKBRIDGE_HIDE_EXTS -u VKBRIDGE_SERVER_HIDE_EXTS -u VKBRIDGE_ERRATA -u VKBRIDGE_ERRATA_IMITATE -u VKBRIDGE_MODE -u VKBRIDGE_FAKE_DRIVER_VERSION -u VKBRIDGE_GS_HIDE_PROCS"
got=$(env $clean_env VKBRIDGE_ERRATA=+fp16_hide timeout 60 "$out/fp16_check" enable 2>&1)
echo "$got" | grep -q '^features: v12.shaderFloat16=0 ext.shaderFloat16=0 v11.storageBuffer16BitAccess=1' &&
  echo "$got" | grep -q '^device: shaderFloat16 and storageBuffer16BitAccess enabled: 0' &&
  echo "$got" | grep -q 'fp16_hide: the app enabled shaderFloat16 anyway; taken off its device'
step $? "fp16_hide (MK-20): shaderFloat16 hidden, 16-bit storage kept, taken off a device that enables it ($(echo "$got" | grep -o '^features: [^ ]* [^ ]*'))"
got=$(env $clean_env VKBRIDGE_ERRATA=+gs_hide VKBRIDGE_GS_HIDE_PROCS=gshide_check timeout 60 "$out/gshide_check" 2>&1)
echo "$got" | grep -q '^features: v10.geometryShader=0 f2.geometryShader=0' && echo "$got" | grep -q '^limits: v10.maxGeometryOutputVertices=0' &&
  echo "$got" | grep -q '^device: geometryShader enabled: 0'
step $? "gs_hide: geometry shaders hidden from the named process and taken off its device ($(echo "$got" | grep -o '^features: [^ ]* [^ ]*'))"
got=$(env $clean_env VKBRIDGE_ERRATA=+gs_hide VKBRIDGE_GS_HIDE_PROCS=someone_else timeout 60 "$out/gshide_check" 2>&1)
echo "$got" | grep -q '^device: geometryShader enabled: 1' || ! echo "$got" | grep -q 'v10.geometryShader=0'
step $? "gs_hide: another process keeps them ($(echo "$got" | grep -o '^features: [^ ]*'))"
if command -v vulkaninfo >/dev/null 2>&1; then
  ext=VK_KHR_maintenance1 # a device extension every Vulkan 1.1 driver lists, lavapipe's included
  a=$(env $clean_env timeout 60 vulkaninfo 2>/dev/null | grep -c "$ext ")
  b=$(env $clean_env VKBRIDGE_SERVER_HIDE_EXTS=$ext timeout 60 vulkaninfo 2>/dev/null | grep -c "$ext ")
  [ -n "$ext" ] && [ "$a" -gt 0 ] && [ "$b" = 0 ]
  step $? "VKBRIDGE_SERVER_HIDE_EXTS=$ext: listed $a time(s) before, $b after"
fi
if command -v vulkaninfo >/dev/null 2>&1; then
  # The a1 gate (server_apicap.c) on an old job-manager Arm driver: in the layer only Steam mode's own
  # processes lose VK_EXT_extended_dynamic_state; a game's process keeps it unless VKBRIDGE_A1_ZINK=1
  eds() { grep -c "VK_EXT_extended_dynamic_state " "$work/a1.txt"; }
  cp "$(command -v vulkaninfo)" "$work/steam"
  env $clean_env -u VKBRIDGE_A1_ZINK VKBRIDGE_FAKE_DRIVER_VERSION=r38p1:jm timeout 60 vulkaninfo >"$work/a1.txt" 2>/dev/null; a=$(eds)
  env $clean_env VKBRIDGE_A1_ZINK=1 VKBRIDGE_FAKE_DRIVER_VERSION=r38p1:jm timeout 60 vulkaninfo >"$work/a1.txt" 2>/dev/null; b=$(eds)
  env $clean_env -u VKBRIDGE_A1_ZINK VKBRIDGE_FAKE_DRIVER_VERSION=r38p1:jm timeout 60 "$work/steam" >"$work/a1.txt" 2>/dev/null; c=$(eds)
  [ "$a" -gt 0 ] && [ "$b" = 0 ] && [ "$c" = 0 ]
  step $? "a1 gate (r38p1 job manager imitated): a game's process keeps VK_EXT_extended_dynamic_state ($a), VKBRIDGE_A1_ZINK=1 hides it ($b), a process named steam loses it ($c)"
fi
got=$(env $clean_env VKBRIDGE_TEST_CRASH_PIPELINE=1 timeout 120 "$out/crash_contain_check" 2>&1); r=$?
[ $r = 0 ] && ! echo "$got" | grep -q '^FAIL' && [ "$(echo "$got" | grep -c 'CONTAINED driver crash')" = 3 ]
step $? "driver crashes in compiles contained: $(echo "$got" | grep -c '^PASS') checks pass, $(echo "$got" | grep -c 'CONTAINED driver crash') contained (exit $r)"
env $clean_env VKBRIDGE_LAYER_CRASH_HANDLER=0 VKBRIDGE_TEST_CRASH_PIPELINE=1 timeout 120 "$out/crash_contain_check" >/dev/null 2>&1; r=$?
[ $r != 0 ]
step $? "without the layer's handler the same crash ends the process (exit $r)"
if cc -O1 -I../third_party/Vulkan-Headers-1.4.341/include -o "$work/layer_sigchain_check" ../tests/layer_sigchain_check.c -ldl 2>/dev/null; then
  got=$(env $clean_env timeout 60 "$work/layer_sigchain_check" 2>&1); r=$?
  [ $r = 0 ] && echo "$got" | grep -q '^OK'
  step $? "the process's own faults reach its own handler: $(echo "$got" | grep -c '^PASS') checks pass (exit $r)"
else
  step 1 "cannot build layer_sigchain_check"
fi

if [ $fails = 0 ]; then echo "OK: $steps steps"; else echo "FAILED: $fails of $steps steps"; exit 1; fi
