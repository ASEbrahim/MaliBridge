#!/bin/bash
# pv_zink_name (PV-08: server_errata_zink.c) on the desktop driver, with a server of its own per step
# (socket, cache directory, session folder). PowerVR's driver and its application profile for the
# exact engine name "mesa zink" are imitated: VKBRIDGE_FAKE_DRIVER=powervr (server_driverid.c) makes
# the driver PowerVR-looking and hides sparse, vertexInputDynamicState and graphicsPipelineLibrary
# from instances made under that name. zinkname_check (tests/zinkname_check.c) makes an instance
# under the name given and prints what the GPU offers. Steps:
#   a  the profile, the fix off (-pv_zink_name): Zink's name sees the features hidden, another name sees them
#   b  the default: the probe sees the profile (log), Zink's name is passed as a neutral one (log): vertexInputDynamicState
#      offered, what else the profile hid (sparse, graphicsPipelineLibrary, its extension) hidden again (wallentx's form);
#      another engine name is not touched
#   h  VKBRIDGE_ZINK_KEEP_PROFILE=0: the renamed instance sees everything the neutral name shows
#   c  the probe off (VKBRIDGE_ZINK_PROFILE_PROBE=0): nothing decides the rename: hidden as before
#   d  the probe off and VKBRIDGE_ERRATA=+pv_zink_name: renamed anyway
#   e  a driver that is not PowerVR (the desktop's): not probed, no rename, offered
#   f  the decision from a probe result (VKBRIDGE_ERRATA_PROBE=zink_profile:pass|fail), probe off
#   g  the log names the PowerVR driver and the profile
# Prints one line per step and "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
work=$(mktemp -d)
sock=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-zinkname-$$.sock
icd=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json}
srv=
log=
dir=
stop() { [ -n "$srv" ] && kill $srv 2>/dev/null && wait $srv 2>/dev/null; srv=; }
start() { # start <name> [env...]
  dir=$work/$1
  shift
  stop
  rm -f "$sock"
  mkdir -p "$dir/c" "$dir/s"
  log=$dir/s/vkbridge-server.log
  : > "$log"
  env -u VKBRIDGE_FAKE_MISSING -u VKBRIDGE_HIDE_EXTS -u VKBRIDGE_ERRATA -u VKBRIDGE_ERRATA_IMITATE -u VKBRIDGE_FAKE_DRIVER \
      -u VKBRIDGE_MODE -u VKBRIDGE_FAKE_DRIVER_VERSION -u VKBRIDGE_ERRATA_PROBE -u VKBRIDGE_PROBE_FAKE -u VKBRIDGE_ZINK_PROFILE_PROBE \
      VKBRIDGE_PROBE_BUDGET_S=0 "$@" VK_DRIVER_FILES=$icd VK_ICD_FILENAMES= \
      "$out/vkbridge-server" --socket "$sock" --log "$log" --cache-dir "$dir/c" >/dev/null 2>&1 &
  srv=$!
  for _ in $(seq 300); do [ -S "$sock" ] && break; kill -0 $srv 2>/dev/null || break; sleep 0.05; done
}
check() { # check [engine name]: $got: its output, $new: the log lines it added
  local l; l=$(wc -l < "$log")
  got=$(timeout 60 "$out/zinkname_check" "$@" 2>&1)
  new=$(tail -n +$((l+1)) "$log")
}
f() { echo "$got" | grep -o "$1=[0-9]*" | head -1 | cut -d= -f2; }
offered() { [ "$(f sparseBinding)" = "$nsp" ] && [ "$(f vertexInputDynamicState)" = "$nvi" ] && [ "$(f graphicsPipelineLibrary)" = "$ngp" ]; }
vionly() { [ "$(f sparseBinding)" = 0 ] && [ "$(f vertexInputDynamicState)" = "$nvi" ] && [ "$(f graphicsPipelineLibrary)" = 0 ]; }
hidden() { [ "$(f sparseBinding)" = 0 ] && [ "$(f vertexInputDynamicState)" = 0 ] && [ "$(f graphicsPipelineLibrary)" = 0 ]; }
trap 'stop; rm -f "$sock"; [ -n "$KEEP" ] && echo "kept $work" || rm -rf "$work"' EXIT
export VKBRIDGE_SOCKET=$sock VK_ICD_FILENAMES=
unset VKBRIDGE_HIDE_EXTS VKBRIDGE_MAX_API VKBRIDGE_FAKE_MISSING SteamAppId
fails=0 steps=0
step() { steps=$((steps+1)); if [ "$1" = 0 ]; then echo "  ok   $2"; else echo "  FAIL $2"; fails=$((fails+1)); fi; }

native=$(VK_DRIVER_FILES=$icd timeout 60 "$out/zinkname_check" 2>&1)
nsp=$(echo "$native" | grep -o 'sparseBinding=[0-9]*' | cut -d= -f2)
nvi=$(echo "$native" | grep -o 'vertexInputDynamicState=[0-9]*' | cut -d= -f2)
ngp=$(echo "$native" | grep -o 'graphicsPipelineLibrary=[0-9]*' | cut -d= -f2)
if [ "$nsp$nvi" != 11 ] ; then echo "OK: 0 steps (SKIP: the desktop driver lacks sparse binding or vertex input dynamic state, nothing to hide: $native)"; exit 0; fi
export VK_DRIVER_FILES=$out/vkbridge_icd.json
PVR=VKBRIDGE_FAKE_DRIVER=powervr

# a
start a $PVR VKBRIDGE_ERRATA=-pv_zink_name
check
hidden && echo "$got" | grep -q "extVI=" && check "Other Engine" && offered
step $? "the profile, fix off: \"mesa zink\" sees sparse/vertexInputDynamicState/graphicsPipelineLibrary hidden, another name sees them"
# b
start b $PVR
check
ok=0
vionly || ok=1
echo "$new" | grep -q "the renamed instance keeps the profile's other [0-9]* hidden thing(s) hidden; only vertexInputDynamicState is shown - PV-08" || ok=6
grep -q "pv_zink_name: probe: the driver's profile for engine name \"mesa zink\" hides [0-9]* thing(s) from Zink: .*sparseBinding.*vertexInputDynamicState" "$log" || ok=2
echo "$new" | grep -q "errata: pv_zink_name: instance made under engine name \"Mesa zink\" instead of \"mesa zink\" (probe zink_profile failed; safe mode) - PV-08" || ok=3
check "Other Engine"
offered && ! echo "$new" | grep -q "pv_zink_name" || ok=4
check "Zink"
offered && ! echo "$new" | grep -q "instance made under" || ok=5
step $ok "default: the probe sees the profile, \"mesa zink\" is passed as \"Mesa zink\": vertexInputDynamicState offered, sparse/GPL hidden again ($(echo "$got" | grep features)); other engine names untouched (rc $ok)"
# c
start c $PVR VKBRIDGE_ZINK_PROFILE_PROBE=0
check
hidden && grep -q "pv_zink_name: profile probe off" "$log" && ! echo "$new" | grep -q "instance made under"
step $? "probe off: no decision, hidden as before"
# d
start d $PVR VKBRIDGE_ZINK_PROFILE_PROBE=0 VKBRIDGE_ERRATA=+pv_zink_name
check
offered && echo "$new" | grep -q "instance made under engine name \"Mesa zink\" instead of \"mesa zink\" (.*VKBRIDGE_ERRATA=+pv_zink_name"
step $? "probe off, +pv_zink_name: renamed anyway"
# e
start e
check
offered && ! grep -q "pv_zink_name: probe" "$log" && ! echo "$new" | grep -q "instance made under"
step $? "not a PowerVR driver: not probed, not renamed, offered"
# f
start f1 $PVR VKBRIDGE_ZINK_PROFILE_PROBE=0 VKBRIDGE_ERRATA_PROBE=zink_profile:pass
check
hidden && ! echo "$new" | grep -q "instance made under"
r1=$?
start f2 $PVR VKBRIDGE_ZINK_PROFILE_PROBE=0 VKBRIDGE_ERRATA_PROBE=zink_profile:fail
check
offered && echo "$new" | grep -q "instance made under engine name \"Mesa zink\" instead of \"mesa zink\" (probe zink_profile failed"
step $((r1 + $?)) "decision from a probe result: pass -> hidden as before, fail -> renamed"
# g
start g $PVR
check
grep -q "pv_zink_name: probe: the driver's profile" "$log" && grep -q "GPU driver: driverID 7 (IMAGINATION_PROPRIETARY)" "$log"
step $? "the log names the PowerVR driver and the profile the probe saw"

# h
start h $PVR VKBRIDGE_ZINK_KEEP_PROFILE=0
check
offered && echo "$new" | grep -q "the renamed instance sees everything the neutral name shows (VKBRIDGE_ZINK_KEEP_PROFILE=0)"
step $? "VKBRIDGE_ZINK_KEEP_PROFILE=0: the renamed instance sees everything ($(echo "$got" | grep features))"

echo
if [ $fails = 0 ]; then echo "OK: $steps steps"; else echo "FAILED: $fails of $steps steps"; exit 1; fi
