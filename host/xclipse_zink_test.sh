#!/bin/bash
# Zink on Samsung's Xclipse driver (imitated): EDS2 left out for Zink only (push descriptors too with =push), the switch, the page check.
# Prints one line per step and "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
work=$(mktemp -d)
sock=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-xzink-$$.sock
host_icd=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json}
srv=
log=
stop() { [ -n "$srv" ] && kill $srv 2>/dev/null && wait $srv 2>/dev/null; srv=; }
start() { # start <name> [env...]: a server of its own, log $log; $envfile goes to <cache dir>/env
  local d=$work/$1
  shift
  stop
  rm -f "$sock"
  mkdir -p "$d/c"
  [ -n "$envfile" ] && printf '%s\n' "$envfile" > "$d/c/env"
  log=$d/vkbridge-server.log
  : > "$log"
  env -u VKBRIDGE_FAKE_MISSING -u VKBRIDGE_HIDE_EXTS -u VKBRIDGE_MODE -u VKBRIDGE_FAKE_DRIVER -u VKBRIDGE_XCLIPSE_ZINK \
      VKBRIDGE_PROBE_BUDGET_S=0 "$@" VK_DRIVER_FILES=$host_icd VK_ICD_FILENAMES= \
      "$out/vkbridge-server" --socket "$sock" --log "$log" --cache-dir "$d/c" >/dev/null 2>&1 &
  srv=$!
  for _ in $(seq 300); do [ -S "$sock" ] && break; kill -0 $srv 2>/dev/null || break; sleep 0.05; done
}
trap 'stop; rm -f "$sock"; [ -n "$KEEP" ] && echo "kept $work" || rm -rf "$work"' EXIT
export VKBRIDGE_SOCKET=$sock VK_DRIVER_FILES=$out/vkbridge_icd.json VK_ICD_FILENAMES=
unset VKBRIDGE_HIDE_EXTS VKBRIDGE_MAX_API VKBRIDGE_FAKE_MISSING VKBRIDGE_MODE
fails=0 steps=0
step() { steps=$((steps+1)); if [ "$1" = 0 ]; then echo "  ok   $2"; else echo "  FAIL $2"; fails=$((fails+1)); fi; }
zink() { MESA_LOADER_DRIVER_OVERRIDE=zink GALLIUM_DRIVER=zink timeout 120 "$@"; }
gl() { zink python3 -I ../tests/gl/glvtxlayout.py >$work/gl.log 2>&1; grep -o 'RESULT.*' $work/gl.log | tail -1; }
line='xclipse: VK_EXT_extended_dynamic_state2 not offered to Zink'
both='xclipse: VK_EXT_extended_dynamic_state2 and VK_KHR_push_descriptor not offered to Zink'
# what a client named "mesa zink" is offered: "offered: VK_KHR_push_descriptor <0|1>, VK_EXT_extended_dynamic_state2 <0|1>"
offered() { timeout 120 "$out/vidyn_check" "mesa zink" 2>/dev/null | grep -m1 '^offered:'; }
# the feature structs of Zink's first device (EDS2's is 1000377000)
chain() { sleep 0.3; grep -m1 'vkCreateDevice: pEnabledFeatures.*chain:' "$log"; }

# 1
start samsung VKBRIDGE_FAKE_DRIVER=samsung
r=$(gl); c=$(chain)
o=$(offered)
echo "$r" | grep -q '^RESULT ok' && grep -q "$line (Samsung's driver" "$log" && [ -n "$c" ] &&
  ! echo "$c" | grep -qE ' 1000377000( |$)' && [ "$o" = "offered: VK_KHR_push_descriptor 1, VK_EXT_extended_dynamic_state2 0" ]
step $? "Samsung's driver imitated, Zink: EDS2 not offered, its device without it, push descriptors offered; $(echo "$r" | cut -c1-24)"

# 2
n=$(timeout 60 vulkaninfo 2>/dev/null | grep -cE 'VK_KHR_push_descriptor |VK_EXT_extended_dynamic_state2 ')
[ "$n" -ge 2 ]
step $? "the same driver, a client that is not Zink: both listed ($n)"

# 3
start off VKBRIDGE_FAKE_DRIVER=samsung VKBRIDGE_XCLIPSE_ZINK=0
r=$(gl); c=$(chain)
echo "$r" | grep -q '^RESULT ok' && ! grep -q "xclipse:" "$log" && echo "$c" | grep -qE ' 1000377000( |$)'
step $? "VKBRIDGE_XCLIPSE_ZINK=0: Zink gets EDS2 again; $(echo "$r" | cut -c1-24)"

# 3b
start push VKBRIDGE_FAKE_DRIVER=samsung VKBRIDGE_XCLIPSE_ZINK=push
r=$(gl); c=$(chain)
o=$(offered)
echo "$r" | grep -q '^RESULT ok' && grep -q "$both (VKBRIDGE_XCLIPSE_ZINK" "$log" &&
  ! echo "$c" | grep -qE ' 1000377000( |$)' && [ "$o" = "offered: VK_KHR_push_descriptor 0, VK_EXT_extended_dynamic_state2 0" ]
step $? "VKBRIDGE_XCLIPSE_ZINK=push: push descriptors left out as well as EDS2; $(echo "$r" | cut -c1-24)"

# 3c
envfile=VKBRIDGE_XCLIPSE_ZINK=0 start envoff VKBRIDGE_FAKE_DRIVER=samsung
r=$(gl); c=$(chain)
echo "$r" | grep -q '^RESULT ok' && ! grep -q "xclipse:" "$log" && echo "$c" | grep -qE ' 1000377000( |$)'
step $? "env file VKBRIDGE_XCLIPSE_ZINK=0: Zink gets EDS2 again; $(echo "$r" | cut -c1-24)"
envfile=

# 3d
start eds2 VKBRIDGE_FAKE_DRIVER=samsung VKBRIDGE_XCLIPSE_ZINK=eds2
r=$(gl); c=$(chain); o=$(offered)
echo "$r" | grep -q '^RESULT ok' && grep -q "$line (VKBRIDGE_XCLIPSE_ZINK" "$log" &&
  ! echo "$c" | grep -qE ' 1000377000( |$)' && [ "$o" = "offered: VK_KHR_push_descriptor 1, VK_EXT_extended_dynamic_state2 0" ]
step $? "VKBRIDGE_XCLIPSE_ZINK=eds2: as the default, EDS2 only; $(echo "$r" | cut -c1-24)"

# 4
start desktop
r=$(gl); c=$(chain)
echo "$r" | grep -q '^RESULT ok' && ! grep -q "xclipse:" "$log" && echo "$c" | grep -qE ' 1000377000( |$)'
step $? "the desktop driver (not Samsung's): nothing hidden; $(echo "$r" | cut -c1-24)"

# 5
hs=$(ls -d ${VKB_CHROMIUM:-$HOME/.cache/ms-playwright/chromium_headless_shell-*/chrome-headless-shell-linux64/chrome-headless-shell} 2>/dev/null | tail -1)
shot() { # shot <name>: the page through the Vulkan driver VK_DRIVER_FILES names now
  rm -rf $work/ud
  zink "$hs" --no-sandbox --disable-gpu-sandbox --use-gl=angle --use-angle=gl-egl --ignore-gpu-blocklist --enable-gpu-rasterization \
    --user-data-dir=$work/ud --window-size=1560,720 --screenshot=$work/$1.png "file://$PWD/../tests/gl/signinpage.html" >/dev/null 2>&1
}
if [ -x "$hs" ] && python3 -I -c 'import PIL' 2>/dev/null; then
  stop
  VK_DRIVER_FILES=$host_icd shot ref
  start a56 VKBRIDGE_FAKE_DRIVER=samsung VKBRIDGE_FAKE_MISSING=hostptr,extbufhv,bc,maint6 VKBRIDGE_MODE=safe
  shot a56
  same=$(python3 -I ../tests/gl/imgsame.py $work/ref.png $work/a56.png); rc=$?
  [ $rc = 0 ] && [ -s $work/ref.png ] && grep -q "$line" "$log" && grep -q "chosen (unflagged buffers)" "$log"
  step $? "Chromium, the A56 imitated (Samsung's driverID, unflagged dma-buf sharing, safe mode) vs Zink directly: $same"
else
  echo "  skip Chromium (no headless shell or no PIL)"
fi

if [ $fails = 0 ]; then echo "OK: $steps steps"; else echo "FAILED: $fails of $steps steps"; exit 1; fi
