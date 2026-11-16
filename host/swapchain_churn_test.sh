#!/bin/bash
# A program that makes a new surface and swapchain for every frame (a player log, KOTOR through Zink: ~6 a second, each one's images
# ~950 round trips a second) must not pay the images' creation each time (client/wsi.c keeps a destroyed swapchain's idle images
# for a few seconds for the next one with the same device, extent, format, usage and count). tests/swapchain_churn_check on a
# headless surface, one process per phase, 60 cycles each, the client's vkCreateImage round trips (VKBRIDGE_STATS lists a
# command from 100 on, absent = under 100):
#   1  A, the same size every frame: at most 8 vkCreateImage (one set of 4; before the fix 240)
#   2  B, the extent alternating every frame (a resize each cycle): at most 16 (two sets of 4; before 240)
#   3  C, a size the pool never saw: the images are made new (3 or more)
#   4  VKBRIDGE_SWAPCHAIN_RECYCLE=0 (the old behaviour): A makes 240 again (the switch works)
# Prints one line per step and "OK: ..." / "FAILED: ..." last. VKB_CLIENT_ICD=<icd json> tests another client build (one
# before the fix fails 1 and 2).
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
icd=${VKB_CLIENT_ICD:-$out/vkbridge_icd.json}
work=$(mktemp -d)
sock=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-swchurn-$$.sock
srv=
fails=0
trap '[ -n "$srv" ] && kill $srv 2>/dev/null && wait $srv 2>/dev/null; [ -n "${VKB_KEEP:-}" ] && echo "kept $work" || rm -rf "$work"; rm -f "$sock"' EXIT
mkdir -p "$work/c"
env -u VKBRIDGE_FAKE_MISSING -u VKBRIDGE_HIDE_EXTS -u VKBRIDGE_ERRATA -u VKBRIDGE_MODE VKBRIDGE_PROBE_BUDGET_S=0 \
    VK_DRIVER_FILES=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json} VK_ICD_FILENAMES= \
    "$out/vkbridge-server" --socket "$sock" --log "$work/server.log" --cache-dir "$work/c" >/dev/null 2>&1 &
srv=$!
for _ in $(seq 300); do [ -S "$sock" ] && break; kill -0 $srv 2>/dev/null || break; sleep 0.05; done
[ -S "$sock" ] || { echo "FAILED: server did not start"; exit 1; }
run() { # run <name> <phase> [env...]: $res exit status, $ci vkCreateImage round trips (0 = not listed)
  local name=$1 phase=$2; shift 2
  env -u VKBRIDGE_SWAPCHAIN_RECYCLE "$@" VKBRIDGE_SOCKET="$sock" VK_DRIVER_FILES=$icd VK_ICD_FILENAMES= VKBRIDGE_STATS=3600 \
    timeout 120 "$out/swapchain_churn_check" --phase "$phase" --cycles 60 >"$work/$name.log" 2>&1
  res=$?
  ci=$(grep -E "\]: +vkCreateImage +[0-9]+ sync" "$work/$name.log" | sed -E 's/.*vkCreateImage +([0-9]+) sync.*/\1/' | head -1)
  ci=${ci:-0}
}
step() { if [ "$1" = 0 ]; then echo "ok   $2"; else echo "FAIL $2"; fails=$((fails+1)); sed 's/^/       /' "$work/$3.log" | grep -v 'sync\|^       $' | tail -6; fi; }
run a A;  [ $res = 0 ] && [ "$ci" -le 8 ];  step $? "1 same size every frame: $ci vkCreateImage ($(grep -E "^(OK|FAILED)" "$work/a.log" | tail -1))" a
run b B;  [ $res = 0 ] && [ "$ci" -le 16 ]; step $? "2 resize every frame: $ci vkCreateImage" b
run c C;  [ $res = 0 ];                      step $? "3 a new size: made and presented" c
run o A VKBRIDGE_SWAPCHAIN_RECYCLE=0; [ $res = 0 ] && [ "$ci" -ge 200 ]; step $? "4 VKBRIDGE_SWAPCHAIN_RECYCLE=0: $ci vkCreateImage (the old 240)" o
if [ $fails = 0 ]; then echo "OK: a surface and swapchain per frame reuse the idle image set; resizes still work"
else echo "FAILED: $fails step(s)"; exit 1; fi
