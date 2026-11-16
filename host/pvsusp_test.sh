#!/bin/bash
# errata pv_suspend (server_pvsusp.c): on PowerVR, dynamic renderings that suspend and resume reach the driver as whole passes
# (its QueueSubmit faults on the chains vkd3d-proton 3.1 records on tilers: RE3 952060 DX12, Pixel 10 Pro XL, 11-13).
# tests/pvsusp_check: one submit of three command buffers (suspended clear red / resumed+suspended blue left half with a
# green CLEAR that must not apply / empty resumed last part), pixels read back: left blue, right red.
#   1  no PowerVR identity (lavapipe's own suspend/resume): right pixels, no pv_suspend line (other drivers unchanged)
#   2  PowerVR identity (VKBRIDGE_FAKE_DRIVER=img): the same pixels; all 3 parts begun as whole passes
#   3  PowerVR identity, VKBRIDGE_PV_SUSPEND=0: right pixels, no pv_suspend line (the switch)
# Prints one line per step and "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
work=$(mktemp -d)
sock=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-pvsusp-$$.sock
srv=
stop() { [ -n "$srv" ] && kill $srv 2>/dev/null && wait $srv 2>/dev/null; srv=; }
trap 'stop; rm -f "$sock"; [ -n "$KEEP" ] && echo "kept $work" || rm -rf "$work"' EXIT
trap 'exit 143' TERM INT HUP
start() { # start <name> [env...]
  local d=$work/$1
  shift
  stop
  rm -f "$sock"
  mkdir -p "$d/c"
  log=$d/server.log
  env -u VKBRIDGE_FAKE_MISSING -u VKBRIDGE_FAKE_DRIVER -u VKBRIDGE_PV_SUSPEND -u VKBRIDGE_HIDE_EXTS -u VKBRIDGE_MODE VKBRIDGE_PROBE_BUDGET_S=0 "$@" \
      VK_DRIVER_FILES=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/lvp_icd.json} VK_ICD_FILENAMES= \
      "$out/vkbridge-server" --socket "$sock" --log "$log" --cache-dir "$d/c" >/dev/null 2>&1 &
  srv=$!
  for _ in $(seq 100); do [ -S "$sock" ] && break; kill -0 $srv 2>/dev/null || break; sleep 0.1; done
  cl=$d/client.out
}
check() { timeout 60 "$out/pvsusp_check" >$cl 2>&1; rc=$?; sleep 0.3; stop; }
export VKBRIDGE_SOCKET=$sock VK_DRIVER_FILES=$out/vkbridge_icd.json VK_ICD_FILENAMES=
unset VKBRIDGE_HIDE_EXTS VKBRIDGE_MAX_API VKBRIDGE_SYNC
fails=0 steps=0
step() { steps=$((steps+1)); if [ "$1" = 0 ]; then echo "  ok   $2"; else echo "  FAIL $2"; fails=$((fails+1)); fi; }
px() { grep -m1 '^pixels' $cl; }
lines() { grep -c 'pv_suspend' "$log"; }
tot() { grep -o 'pv_suspend: device [0-9]* gone; [0-9]* rendering part(s) begun as whole passes so far' "$log" | tail -1 | sed 's/.*gone; //'; }

# 1
start mali
check
grep -q '^OK' $cl && [ "$(lines)" = 0 ]
step $? "no PowerVR identity: rc $rc, $(px), pv_suspend lines $(lines)"
# 2
start pvr VKBRIDGE_FAKE_DRIVER=img
check
grep -q '^OK' $cl && [ "$(tot)" = "3 rendering part(s) begun as whole passes so far" ] && ! grep -q ' E ' "$log"
step $? "PowerVR identity: rc $rc, $(px), '$(tot)', errors $(grep -c ' E ' "$log")"
# 3
start off VKBRIDGE_FAKE_DRIVER=img VKBRIDGE_PV_SUSPEND=0
check
grep -q '^OK' $cl && [ "$(lines)" = 0 ]
step $? "PowerVR identity, VKBRIDGE_PV_SUSPEND=0: rc $rc, $(px), pv_suspend lines $(lines)"
if [ $fails = 0 ]; then echo "OK: $steps steps"; else echo "FAILED: $fails of $steps steps"; fi
[ $fails = 0 ]
