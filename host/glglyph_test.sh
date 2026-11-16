#!/bin/bash
# Text as GoldSrc (Half-Life) draws it, through Zink (tests/gl/glglyph.py: 256x256 font pages, glyphs
# written one by one with glTexSubImage2D between frames that draw them as GL_QUADS with blending;
# RGBA / ALPHA / LUMINANCE_ALPHA pages, client memory, a pixel buffer, ROW_LENGTH/SKIP).
# a player log (Tab S9 FE Mali-G68 r38p1 JM) and a player log (vivo Mali-G925 r49p1 CSF): menu text
# in Half-Life drawn as fragments of letters.
#     1  Zink on the desktop driver itself, no bridge: the reference picture
#     2  r38p1 job manager imitated, performance mode (a player log's driver): the same picture
#     3  r49p1 CSF imitated, safe mode (a player log's driver): the same picture
#     4  split_varying's imitation beneath with the fix over it (r38p1): the same picture as 1.
#        Zink on the desktop driver does not make the split pair the Mali run showed (its fixed-function
#        fragment shader is linked with the vertex shader here: no pipeline gets merged), so the bug
#        itself is shown by splitvary_test.sh (splitvary_check) and on a device, not here.
# Steps 2 and 3 run with split_varying on (an Arm version faked).
# Prints one line per step and "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
work=$(mktemp -d)
unset VKB_SERVER_PID
export VKBRIDGE_SOCKET=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-glglyph-$$.sock VKB_SERVER_LOG=$work/server.log
trap '[ -n "$VKB_SERVER_PID" ] && kill $VKB_SERVER_PID 2>/dev/null; rm -f "$VKBRIDGE_SOCKET"; [ -n "$KEEP" ] || rm -rf "$work"' EXIT
fails=0 steps=0
step() { steps=$((steps+1)); if [ "$1" = 0 ]; then echo "  ok   $2"; else echo "  FAIL $2"; fails=$((fails+1)); fi; }
zink() { MESA_LOADER_DRIVER_OVERRIDE=zink GALLIUM_DRIVER=zink LIBGL_ALWAYS_SOFTWARE=1 timeout 300 "$@"; }
mali() {
  [ -n "$VKB_SERVER_PID" ] && kill $VKB_SERVER_PID 2>/dev/null && wait $VKB_SERVER_PID 2>/dev/null
  rm -f "$VKB_SERVER_LOG"
  VKB_SERVER_ENV="$*" . ./mali_env.sh >/dev/null
}
# 1
r=$(VK_DRIVER_FILES=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json} zink python3 -I ../tests/gl/glglyph.py $work/ref.bin 2>&1 | grep -o 'RESULT.*' | tail -1)
echo "$r" | grep -q "^RESULT ok"
step $? "Zink on the desktop driver itself: $r"
# 2
VKB_MISSING_ADD=dynrend,multidraw,vidyn mali VKBRIDGE_FAKE_DRIVER_VERSION=r38p1:jm VKBRIDGE_MODE=performance
zink python3 -I ../tests/gl/glglyph.py $work/g68.bin $work/ref.bin >$work/g68.log 2>&1; rc=$?
[ $rc = 0 ] && grep -q 'mesa zink' "$VKB_SERVER_LOG"
step $? "r38p1 job manager imitated: $(grep -o 'RESULT.*' $work/g68.log | tail -1); $(grep -o 'split_varying: device [0-9]*: [0-9]* pipeline(s) given[^,]*' "$VKB_SERVER_LOG" | tail -1)"
[ $rc = 0 ] || grep '^  ' $work/g68.log | sed 's/^/     /'
# 3
mali VKBRIDGE_FAKE_DRIVER_VERSION=r49p1:csf VKBRIDGE_MODE=safe
zink python3 -I ../tests/gl/glglyph.py $work/g925.bin $work/ref.bin >$work/g925.log 2>&1; rc=$?
[ $rc = 0 ] && grep -q 'mesa zink' "$VKB_SERVER_LOG"
step $? "r49p1 CSF imitated: $(grep -o 'RESULT.*' $work/g925.log | tail -1)"
[ $rc = 0 ] || grep '^  ' $work/g925.log | sed 's/^/     /'
# 4
mali VKBRIDGE_FAKE_DRIVER_VERSION=r38p1:jm VKBRIDGE_MODE=performance VKBRIDGE_ERRATA_IMITATE=split_varying
zink python3 -I ../tests/gl/glglyph.py $work/fix.bin $work/ref.bin >$work/fix.log 2>&1; rc=$?
kill $VKB_SERVER_PID 2>/dev/null; wait $VKB_SERVER_PID 2>/dev/null; unset VKB_SERVER_PID
merged=$(grep -c 'split_varying: fragment module h-[0-9a-f]*: location(s) 0x[0-9a-f]* merged' "$VKB_SERVER_LOG")
broke=$(grep -o 'the imitation broke [0-9]* pipeline' "$VKB_SERVER_LOG" | awk '{s+=$4} END {print s+0}')
[ $rc = 0 ] && [ "$broke" = 0 ]
step $? "the imitation beneath, the fix over it: $(grep -o 'RESULT.*' $work/fix.log | tail -1); fragment modules merged $merged (Zink here: none expected), imitation broke $broke"
[ $rc = 0 ] || grep '^  ' $work/fix.log | sed 's/^/     /'
if [ $fails = 0 ]; then echo "OK: $steps steps"; else echo "FAILED: $fails of $steps steps"; exit 1; fi
