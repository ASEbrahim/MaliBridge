#!/bin/bash
# Extended dynamic state 1 and 2 for Zink where the driver has none (server_vidyn.c, VKB_VIDYN_EDS):
# Mali's Vulkan 1.1 drivers (r32p1: G68, G77, G610, G710) imitated - VKBRIDGE_FAKE_MISSING with
# vidyn, dynrend and eds on Mali's usual list, and Vulkan 1.1 to the client. Without the two
# extensions Zink (Mesa 26.0 to 26.2.4) bakes every state into its pipelines and keeps the old
# pipeline when only the vertex layout changes (Mesa 167df22a): Balatro's text, and Chromium's
# (Steam's UI) translucent blurred bars drawn from unblurred, misplaced content.
#     1  the desktop driver itself: gleds.py passes (the check is right)
#     2  r32p1 imitated: both extensions offered to Zink by the server, glvtxlayout.py (two vertex
#        layouts, one program) and gleds.py (depth, cull, front face, topology, stencil, scissor,
#        rasterizer discard, primitive restart) right, no variant failed or draw dropped
#     3  the same with VKBRIDGE_EMU_EDS=0: neither offered (as before); the vertex layout case shows
#        Mesa's bug where the desktop's Mesa has it (informational, not a failure)
#     4  a client that is not Zink on the imitated driver: no extended dynamic state offered
#     5  the desktop driver's own extended dynamic state (nothing imitated): not emulated
#     7  r38p1 job manager with the Steam-mode fix (EDS1 hidden from Zink, Vulkan 1.1): emulated, right
#        (Galaxy Tab S9 FE: Steam's menus flashing on the GPU)
#     8  r38p1 CSF with the Steam-mode fix (Dimensity 9200's Immortalis-G715): emulated, right
#     9  r44p1 and r49p1 CSF (Galaxy Tab S10+, S11 Ultra): neither the Steam-mode fix nor the emulation, right
#     6  headless Chromium (when installed): the page through the imitated r32p1 with the
#        emulation is the same picture as through the bridge at Vulkan 1.3; with it off, it differs
#        where Mesa has the bug (informational)
# Prints one line per step and "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
work=$(mktemp -d)
# a server of its own: the caller's (run_tests.sh exports VKB_SERVER_PID) is left alone
unset VKB_SERVER_PID
export VKBRIDGE_SOCKET=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-eds-$$.sock VKB_SERVER_LOG=$work/server.log
trap '[ -n "$VKB_SERVER_PID" ] && kill $VKB_SERVER_PID 2>/dev/null; rm -f "$VKBRIDGE_SOCKET"; [ -n "$KEEP" ] || rm -rf "$work"' EXIT
fails=0 steps=0
step() { steps=$((steps+1)); if [ "$1" = 0 ]; then echo "  ok   $2"; else echo "  FAIL $2"; fails=$((fails+1)); fi; }
zink() { MESA_LOADER_DRIVER_OVERRIDE=zink GALLIUM_DRIVER=zink timeout 120 "$@"; }

# mali <extra server env...>: a server imitating the Vulkan 1.1 Mali (VKB_MISSING_ADD's list)
mali() {
  rm -f "$VKB_SERVER_LOG"
  VKB_SERVER_ENV="$*" . ./mali_env.sh >/dev/null
  export VKBRIDGE_MAX_API=1.1
  # what r32p1 lacks besides (Zink takes EDS3 and pipeline libraries only with EDS2)
  export VKBRIDGE_HIDE_EXTS=$VKBRIDGE_HIDE_EXTS,VK_EXT_graphics_pipeline_library,VK_EXT_extended_dynamic_state3
}
gl() { zink python3 -I ../tests/gl/$1 >$work/$1.log 2>&1; grep -o 'RESULT.*' $work/$1.log | tail -1; }
logged() { sleep 0.3; grep -c "$1" "$VKB_SERVER_LOG"; }
summary() { grep -o 'vertex input: [0-9]* pipeline(s) kept.*' "$VKB_SERVER_LOG" | tail -1; }

# 1
unset VK_DRIVER_FILES
r=$(VK_DRIVER_FILES=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json} gl gleds.py)
echo "$r" | grep -q "^RESULT ok"
step $? "the desktop driver itself: $r"

# 2
VKB_MISSING_ADD=vidyn,dynrend,eds mali VKBRIDGE_FAKE_API=1.1
r1=$(gl glvtxlayout.py); r2=$(gl gleds.py)
echo "$r1" | grep -q "^RESULT ok" && echo "$r2" | grep -q "^RESULT ok" &&
  [ "$(logged 'VK_EXT_extended_dynamic_state and _state2 offered to Zink: the server')" -ge 1 ] &&
  [ "$(logged 'extended dynamic state 1 and 2 for Zink: emulated')" -ge 2 ] &&
  ! grep -q "variant failed" "$VKB_SERVER_LOG" && summary | grep -q " 0 failed.* 0 draw(s) dropped"
step $? "r32p1 imitated, emulated: $(echo "$r1" | cut -c1-28); gleds $(echo "$r2" | cut -c1-24); $(summary | grep -o '[0-9]* variant(s) made')"

# 3
VKB_MISSING_ADD=vidyn,dynrend,eds mali VKBRIDGE_EMU_EDS=0
r1=$(gl glvtxlayout.py)
[ "$(logged 'VK_EXT_extended_dynamic_state and _state2 offered to Zink: the server')" = 0 ] && [ "$(logged 'extended dynamic state 1 and 2 for Zink')" = 0 ]
step $? "VKBRIDGE_EMU_EDS=0: not offered; two vertex layouts: $(echo "$r1" | cut -c1-28) (WRONG = this Mesa has the bug the emulation avoids)"

# 4
VKB_MISSING_ADD=vidyn,dynrend,eds mali VKBRIDGE_FAKE_API=1.1
n=$(timeout 60 vulkaninfo 2>/dev/null | grep -c 'VK_EXT_extended_dynamic_state[^3]')
[ "$n" = 0 ]
step $? "a client that is not Zink: no extended dynamic state 1/2 listed ($n)"

# 5
VKB_MISSING_ADD=vidyn,dynrend mali
r2=$(gl gleds.py)
echo "$r2" | grep -q "^RESULT ok" && [ "$(logged 'extended dynamic state 1 and 2 for Zink: emulated')" = 0 ]
step $? "the driver's own extended dynamic state: not emulated; gleds $(echo "$r2" | cut -c1-24)"

# 7 (before 6: needs no Chromium)
VKB_MISSING_ADD=vidyn mali VKBRIDGE_FAKE_DRIVER_VERSION=r38p1:jm
r1=$(gl glvtxlayout.py); r2=$(gl gleds.py)
echo "$r1" | grep -q "^RESULT ok" && echo "$r2" | grep -q "^RESULT ok" &&
  [ "$(logged 'a1: VK_EXT_extended_dynamic_state not offered')" -ge 1 ] &&
  [ "$(logged 'extended dynamic state 1 and 2 for Zink: emulated')" -ge 2 ] &&
  ! grep -q "variant failed" "$VKB_SERVER_LOG" && summary | grep -q " 0 failed.* 0 draw(s) dropped"
step $? "r38p1 job manager (Steam-mode fix on; Galaxy Tab S9 FE): $(echo "$r1" | cut -c1-28); gleds $(echo "$r2" | cut -c1-24)"

# 8
VKB_MISSING_ADD=vidyn mali VKBRIDGE_FAKE_DRIVER_VERSION=r38p1:csf
r1=$(gl glvtxlayout.py); r2=$(gl gleds.py)
echo "$r1" | grep -q "^RESULT ok" && echo "$r2" | grep -q "^RESULT ok" &&
  [ "$(logged 'a1: VK_EXT_extended_dynamic_state not offered.*(Arm driver r38, CSF, ')" -ge 1 ] &&
  [ "$(logged 'extended dynamic state 1 and 2 for Zink: emulated')" -ge 2 ] &&
  ! grep -q "variant failed" "$VKB_SERVER_LOG" && summary | grep -q " 0 failed.* 0 draw(s) dropped"
step $? "r38p1 CSF (Steam-mode fix on): $(echo "$r1" | cut -c1-28); gleds $(echo "$r2" | cut -c1-24)"

# 9
for v in r44p1:csf r49p1:csf; do
  VKB_MISSING_ADD=vidyn mali VKBRIDGE_FAKE_DRIVER_VERSION=$v
  r1=$(gl glvtxlayout.py); r2=$(gl gleds.py)
  echo "$r1" | grep -q "^RESULT ok" && echo "$r2" | grep -q "^RESULT ok" &&
    [ "$(logged 'a1: ')" = 0 ] && [ "$(logged 'extended dynamic state 1 and 2 for Zink: emulated')" = 0 ]
  step $? "${v%%:*} CSF: no Steam-mode fix, not emulated; $(echo "$r1" | cut -c1-28); gleds $(echo "$r2" | cut -c1-24)"
done

# 6
hs=$(ls -d ${VKB_CHROMIUM:-$HOME/.cache/ms-playwright/chromium_headless_shell-*/chrome-headless-shell-linux64/chrome-headless-shell} 2>/dev/null | tail -1)
shot() { # shot <name>: the page through the server running now
  rm -rf $work/ud
  zink "$hs" --no-sandbox --disable-gpu-sandbox --use-gl=angle --use-angle=gl-egl --ignore-gpu-blocklist --enable-gpu-rasterization \
    --user-data-dir=$work/ud --window-size=800,600 --screenshot=$work/$1.png "file://$PWD/../tests/gl/blurpage.html" >/dev/null 2>&1
}
if [ -x "$hs" ] && python3 -I -c 'import PIL' 2>/dev/null; then
  VKB_MISSING_ADD=vidyn,dynrend mali; export VKBRIDGE_MAX_API=1.3; shot ref
  VKB_MISSING_ADD=vidyn,dynrend,eds mali VKBRIDGE_FAKE_API=1.1; shot emu
  same=$(python3 -I ../tests/gl/imgsame.py $work/ref.png $work/emu.png); rc=$?
  VKB_MISSING_ADD=vidyn,dynrend,eds mali VKBRIDGE_FAKE_API=1.1 VKBRIDGE_EMU_EDS=0; shot off
  off=$(python3 -I ../tests/gl/imgsame.py $work/ref.png $work/off.png)
  [ $rc = 0 ] && [ -s $work/ref.png ]
  step $? "Chromium, r32p1 imitated with the emulation vs Vulkan 1.3: $same; with it off: $off"
else
  echo "  skip Chromium (no headless shell or no PIL)"
fi

if [ $fails = 0 ]; then echo "OK: $steps steps"; else echo "FAILED: $fails of $steps steps"; exit 1; fi
