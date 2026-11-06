#!/bin/bash
# VK_KHR_imageless_framebuffer made by the bridge (server_emu_imageless.c) where the driver lacks it:
# Mali r18-r26 (Vulkan 1.1). Mesa's Zink requires the extension (26.0.8, DroidDeck's Ubuntu runtime:
# "ZINK: VK_KHR_imageless_framebuffer required!"), so Steam's UI died there with "failed to load driver:
# zink" (a player log, HUAWEI P40 Pro, Mali-G76 1.1.97). On the desktop driver under the Mali profiles
# (host/mali_profile.sh), a server of its own for each step:
#   1  r32-r34 (the driver has the extension): imageless_check passes on the driver's own, nothing emulated;
#   2  r21-r26: imageless_check passes through the emulation, the counts exact: 27 begins, 4 framebuffers
#      made (A, B, B's new view, A on the remade framebuffer), 1 freed by a view destroy, the rest by
#      framebuffer destroys;
#   3  r21-r26 with VKBRIDGE_EMU_IMAGELESS=0: not offered;
#   4  r21-r26: Zink (the desktop's Mesa) over the bridge starts (OpenGL 2.1 there: no transform feedback, so
#      no 3.0) and draws 4 rounds of 2 framebuffer objects right (on the base: no Zink screen at all);
#   5  the same with ZINK_DEBUG=rp (Zink's tiler path): draws right, and no imageless framebuffer is made: Zink
#      26.x renders only with dynamic rendering (zink_context.c: vkCmdBeginRendering, nothing else), so the
#      extension is only its start-up check there; imageless_check (step 2) covers the render-pass path;
#   6  r18-r19 (timeline semaphores the bridge's too): Zink draws right;
#   7  r32-r34: Zink draws right on the driver's own, nothing emulated.
# Prints one line per step and "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
work=$(mktemp -d)
cache=$work/c
mkdir -p "$cache"
sock=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-imageless-$$.sock
log=$work/server.log
srv=
. ./mali_profile.sh >/dev/null
start() {
  [ -n "$srv" ] && kill $srv 2>/dev/null && wait $srv 2>/dev/null
  rm -f "$sock"
  : > "$log"
  env -u VKBRIDGE_FAKE_MISSING -u VKBRIDGE_HIDE_EXTS -u VKBRIDGE_FAKE_API -u VKBRIDGE_PROMOTE_API -u VKBRIDGE_PROMOTE_TESTED -u VKBRIDGE_MODE \
      -u VKBRIDGE_EMU_IMAGELESS -u VKBRIDGE_SERVER_HIDE_EXTS "$@" \
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
  [ $rc = 0 ] || grep '^FAIL\|^RESULT\|rror' $work/run.log | head -3 | sed 's/^/       /'
}
counts() { grep -o 'imageless framebuffer emulation, device [0-9]*: .*' "$log" | tail -1 | sed 's/.*: //'; }
emulated() { grep -q "imageless framebuffers emulated on device" "$log"; }
# LIBGL_ALWAYS_SOFTWARE: surfaceless EGL without a render node, so Zink takes the bridge's device instead of matching the
# PC's DRM node (which the bridge's device is not: "ZINK: failed to choose pdev")
zink() { run env LIBGL_ALWAYS_SOFTWARE=1 MESA_LOADER_DRIVER_OVERRIDE=zink GALLIUM_DRIVER=zink "$@" python3 -I ../tests/gl/glimageless.py; }

# 1
mp_apply r32-r34
start
run "$out/imageless_check"
stop
[ $rc = 0 ] && echo "$res" | grep -q "^OK: 1[0-9] of 1[0-9]" && ! emulated
step $? "r32-r34, the driver's own: $res"
# 2
mp_apply r21-r26
start
run "$out/imageless_check"
stop
[ $rc = 0 ] && echo "$res" | grep -q "^OK: 1[0-9] of 1[0-9]" && emulated &&
  grep -q "imageless framebuffer emulation, device [0-9]*: 27 begin(s), 4 framebuffer(s) made, 1 freed by a view destroy" "$log"
step $? "r21-r26, emulated: $(counts); $res"
# 3
start VKBRIDGE_EMU_IMAGELESS=0
run "$out/imageless_check"
stop
[ $rc != 0 ] && grep -q "^FAIL imageless framebuffers offered: extension missing, feature 0" $work/run.log && ! emulated
step $? "r21-r26, VKBRIDGE_EMU_IMAGELESS=0: not offered ($res)"
# 4
start
zink
stop
[ $rc = 0 ] && echo "$res" | grep -q "^RESULT ok" && emulated
step $? "r21-r26, Zink: $(grep -m1 '^renderer' $work/run.log | sed 's/.*| //' | cut -c1-40); $res"
# 5
start
zink ZINK_DEBUG=rp
stop
[ $rc = 0 ] && echo "$res" | grep -q "^RESULT ok" && emulated && ! grep -q "first imageless framebuffer" "$log"
step $? "r21-r26, Zink with ZINK_DEBUG=rp, no imageless framebuffer made: $res"
# 6
mp_apply r18-r19
start
zink ZINK_DEBUG=rp
stop
[ $rc = 0 ] && echo "$res" | grep -q "^RESULT ok" && emulated
step $? "r18-r19, Zink with ZINK_DEBUG=rp: $res"
# 7
mp_apply r32-r34
start
zink ZINK_DEBUG=rp
stop
[ $rc = 0 ] && echo "$res" | grep -q "^RESULT ok" && ! emulated
step $? "r32-r34, Zink on the driver's own: $res"

if [ $fails = 0 ]; then echo "OK: $steps steps"; else echo "FAILED: $fails of $steps steps"; exit 1; fi
