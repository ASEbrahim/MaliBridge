#!/bin/bash
# DXVK 1.x and a driver with transform feedback but no geometry streams (Mali-G720 r44p1, Galaxy Tab
# S10+: VKBRIDGE_FAKE_MISSING=geomstreams imitates it: the feature query says false, a device asking
# for it is refused with -8). DXVK 1.10.3 turns geometryStreams on with transformFeedback at feature
# level 10_0 and up whatever the query said, so every D3D10/11 device was refused and Hollow Knight
# stopped at "Failed to initialize graphics" (server_emu.c, vkb_emu_device_clamp_features).
#   1. imitated, VKBRIDGE_FEATURE_CLAMP=0: the client sees geometryStreams 0; featclamp_check (a device
#      made as DXVK 1.x makes it) is refused -8, and DXVK 1.10.3's d3d11_check fails (with
#      VKB_DXVK1_WINE_DIR: a VKB_WINE_DIR whose app/ holds DXVK 1.10.3's d3d11.dll and dxgi.dll)
#   2. imitated, the default: geometryStreams taken off at vkCreateDevice (logged), the device made,
#      DXVK 1.10.3's d3d11_check passes
#   3. the desktop driver as it is (geometry streams there): nothing taken off, the device made
# Issue #32 (Galaxy S21, Mali-G78 r38p0, every DXVK 1.10.3 device -8): DXVK 1.10.3 also asks for
# shaderDrawParameters without checking it. VKBRIDGE_FAKE_MISSING=drawparams imitates a driver without it.
#   4. drawparams, the clamp off: refused -8, and the server's one line names what the driver lacks
#   5. geomstreams+drawparams (the #32 suspects together), the clamp off: refused, the line names both
#   6. geomstreams+drawparams, the default: both taken off, the device made; DXVK 1.10.3's d3d11_check
#      refuses on its own (it checks shaderDrawParameters), so driver must report it
#   7. drawparams asked in VkPhysicalDeviceVulkan11Features: taken off (only it; multiview kept), and
#      refused + named with the clamp off
#   8. the desktop driver as it is, shaderDrawParameters asked: nothing taken off, the device made
# PR #31 (K11MCH1): DXVK 1.10 asks for depthClamp at every D3D11 feature level; the Mali-G76 r26p0
# lacks it (VKBRIDGE_FAKE_MISSING=depthclamp imitates it in the self-test). The emulation (bit 28)
# reports it and draws a clamping pipeline with clipping, by default on drivers below Vulkan 1.3
# (VKBRIDGE_FAKE_API=1.1 takes the desktop driver as a 1.1 one).
#   9. imitated on a 1.1 driver: the emulation is on (self-test mask and the device's), the client sees
#      depthClamp 1, a device asking for it is made
#  10. imitated on a 1.1 driver, VKBRIDGE_EMU_DEPTHCLAMP=0: the emulation stays off
#  11. the desktop driver as it is: the emulation stays off (nothing changes on a GPU with depthClamp)
#  12. imitated on the 1.3 driver as it is (a Mali-G52 on Vulkan 1.3.283 lacks depthClamp and runs Steam
#      mode on Zink): the emulation stays off, said once (the imitation is the self-test's: the client still
#      reads the desktop driver's own depthClamp); VKBRIDGE_EMU_DEPTHCLAMP=1 turns it on there
# A server of its own (VKB_RUN_DIR); needs out/ built.
cd "$(dirname "$0")"
VKB_RUN_DIR=${VKB_RUN_DIR:-$(mktemp -d "${XDG_RUNTIME_DIR:-/tmp}/vkbridge-fc-$$.XXXX")}
export VKBRIDGE_SOCKET=$VKB_RUN_DIR/fc.sock VKB_SERVER_LOG=$VKB_RUN_DIR/fc.log
unset VKB_SERVER_PID VKBRIDGE_HIDE_EXTS VKBRIDGE_MAX_API VKBRIDGE_FEATURE_CLAMP
. ./server_start.sh
trap 'vkb_server_stop' EXIT
pass=0; fail=0
check() { if eval "$2"; then pass=$((pass+1)); echo "  ok   $1"; else fail=$((fail+1)); echo "  FAIL $1"; fi; }
dxvk1_ok() { [ -n "${VKB_DXVK1_WINE_DIR:-}" ] && [ -f "$VKB_DXVK1_WINE_DIR/app/d3d11.dll" ]; }
d3d11() { VKB_WINE_DIR=$VKB_DXVK1_WINE_DIR ./d3d11_test.sh >"$1" 2>&1; }
TAKEN="turned on VkPhysicalDeviceTransformFeedbackFeaturesEXT.geometryStreams, which the driver lacks; taken off"

native=$(VK_DRIVER_FILES=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json} VK_ICD_FILENAMES= timeout 60 ./out/featclamp_check 2>&1)
case "$native" in
  *"geometryStreams=1"*) ;;
  *) echo "featclamp_test: 0 passed, 0 failed (SKIP: the desktop driver has no geometry streams: $(echo $native))"; exit 0 ;;
esac

echo "== 1. imitated, the clamp off"
: >"$VKB_SERVER_LOG"
vkb_server_start VKBRIDGE_FAKE_MISSING=geomstreams VKBRIDGE_FEATURE_CLAMP=0
timeout 60 ./out/featclamp_check >out/t-fc-off.log 2>&1
check "the client sees transformFeedback 1, geometryStreams 0 ($(grep -m1 '^features' out/t-fc-off.log))" \
  "grep -q '^features: transformFeedback=1 geometryStreams=0' out/t-fc-off.log"
check "a device made as DXVK 1.x makes it is refused -8, as on the Tab S10+ ($(grep -m1 '^device' out/t-fc-off.log))" \
  "grep -q '^device: -8' out/t-fc-off.log && grep -q 'imitated transform feedback: device refused' $VKB_SERVER_LOG"
check "the server's one line names geometryStreams, as a player's log would have ($(grep -m1 'the driver lacks:' $VKB_SERVER_LOG | sed 's/.*lacks: //'))" \
  "grep 'vkCreateDevice failed: the driver lacks:' $VKB_SERVER_LOG | grep -q 'VkPhysicalDeviceTransformFeedbackFeaturesEXT.geometryStreams\$'"
if dxvk1_ok; then
  d3d11 out/t-fc-d3d11-off.log
  check "DXVK 1.10.3's d3d11_check fails: the players' failure ($(grep -E -m1 '^(OK|FAIL)' out/t-fc-d3d11-off.log))" \
    "! grep -q '^OK:' out/t-fc-d3d11-off.log && grep -q 'vkCreateDevice failed: -8' $VKB_SERVER_LOG"
fi

echo "== 2. imitated, the default"
: >"$VKB_SERVER_LOG"
vkb_server_start VKBRIDGE_FAKE_MISSING=geomstreams
timeout 60 ./out/featclamp_check >out/t-fc-on.log 2>&1
check "the device is made ($(grep -m1 '^device' out/t-fc-on.log))" "grep -q '^device: 0' out/t-fc-on.log"
check "server: geometryStreams taken off, nothing refused" \
  "grep -q 'featclamp_check $TAKEN' $VKB_SERVER_LOG && ! grep -q 'imitated transform feedback: device refused' $VKB_SERVER_LOG"
check "server: transformFeedback kept (no other member taken off)" "[ \$(grep -c 'which the driver lacks; taken off' $VKB_SERVER_LOG) = 1 ]"
timeout 120 ./out/vkbridge-smoke >out/t-fc-smoke.log 2>&1
check "vkbridge-smoke ($(tail -1 out/t-fc-smoke.log))" "grep -q '^OK:' out/t-fc-smoke.log"
if dxvk1_ok; then
  : >"$VKB_SERVER_LOG"
  d3d11 out/t-fc-d3d11-on.log
  check "DXVK 1.10.3's d3d11_check passes ($(grep -E -m1 '^(OK|FAIL)' out/t-fc-d3d11-on.log))" \
    "grep -q '^OK:' out/t-fc-d3d11-on.log && grep -q \"$TAKEN\" $VKB_SERVER_LOG && ! grep -q 'vkCreateDevice failed' $VKB_SERVER_LOG"
fi

echo "== 3. the desktop driver as it is"
: >"$VKB_SERVER_LOG"
vkb_server_start
timeout 60 ./out/featclamp_check >out/t-fc-plain.log 2>&1
check "the client sees geometryStreams 1, the device is made ($(grep -m1 '^features' out/t-fc-plain.log); $(grep -m1 '^device' out/t-fc-plain.log))" \
  "grep -q '^features: transformFeedback=1 geometryStreams=1' out/t-fc-plain.log && grep -q '^device: 0' out/t-fc-plain.log"
check "server: nothing taken off" "! grep -q 'which the driver lacks; taken off' $VKB_SERVER_LOG"

DP_TAKEN="turned on VkPhysicalDeviceShaderDrawParametersFeatures.shaderDrawParameters, which the driver lacks; taken off"
LACKS="vkCreateDevice failed: the driver lacks:"
echo "== 4. drawparams imitated, the clamp off"
: >"$VKB_SERVER_LOG"
vkb_server_start VKBRIDGE_FAKE_MISSING=drawparams VKBRIDGE_FEATURE_CLAMP=0
timeout 60 ./out/featclamp_check drawparams >out/t-fc-dp-off.log 2>&1
check "the client sees shaderDrawParameters 0 ($(grep -m1 '^features: shader' out/t-fc-dp-off.log))" \
  "grep -q '^features: shaderDrawParameters=0' out/t-fc-dp-off.log"
check "refused -8 ($(grep -m1 '^device' out/t-fc-dp-off.log)); the server names it ($(grep -m1 "$LACKS" $VKB_SERVER_LOG | sed 's/.*lacks: //'))" \
  "grep -q '^device: -8' out/t-fc-dp-off.log && grep -q 'imitated draw parameters: device refused' $VKB_SERVER_LOG && \
   grep \"$LACKS\" $VKB_SERVER_LOG | grep -q 'VkPhysicalDeviceShaderDrawParametersFeatures.shaderDrawParameters' && \
   ! grep \"$LACKS\" $VKB_SERVER_LOG | grep -q geometryStreams"

echo "== 5. geomstreams + drawparams imitated, the clamp off"
: >"$VKB_SERVER_LOG"
vkb_server_start VKBRIDGE_FAKE_MISSING=geomstreams,drawparams VKBRIDGE_FEATURE_CLAMP=0
timeout 60 ./out/featclamp_check drawparams >out/t-fc-both-off.log 2>&1
check "refused -8; one line names both ($(grep -m1 "$LACKS" $VKB_SERVER_LOG | sed 's/.*lacks: //'))" \
  "grep -q '^device: -8' out/t-fc-both-off.log && [ \$(grep -c \"$LACKS\" $VKB_SERVER_LOG) = 1 ] && \
   grep \"$LACKS\" $VKB_SERVER_LOG | grep -q 'VkPhysicalDeviceShaderDrawParametersFeatures.shaderDrawParameters' && \
   grep \"$LACKS\" $VKB_SERVER_LOG | grep -q 'VkPhysicalDeviceTransformFeedbackFeaturesEXT.geometryStreams' && \
   ! grep \"$LACKS\" $VKB_SERVER_LOG | grep -q 'transformFeedbackFeaturesEXT.transformFeedback'"

echo "== 6. geomstreams + drawparams imitated, the default"
: >"$VKB_SERVER_LOG"
vkb_server_start VKBRIDGE_FAKE_MISSING=geomstreams,drawparams
timeout 60 ./out/featclamp_check drawparams >out/t-fc-both-on.log 2>&1
check "the device is made ($(grep -m1 '^device' out/t-fc-both-on.log))" "grep -q '^device: 0' out/t-fc-both-on.log"
check "server: shaderDrawParameters and geometryStreams taken off, nothing else, nothing refused" \
  "grep -q \"$DP_TAKEN\" $VKB_SERVER_LOG && grep -q \"$TAKEN\" $VKB_SERVER_LOG && \
   [ \$(grep -c 'which the driver lacks; taken off' $VKB_SERVER_LOG) = 2 ] && ! grep -q 'device refused' $VKB_SERVER_LOG"
if dxvk1_ok; then
  : >"$VKB_SERVER_LOG"
  d3d11 out/t-fc-d3d11-both.log
  # DXVK 1.10.3 checks shaderDrawParameters itself: without it, it refuses the feature level and never
  # calls vkCreateDevice. So DXVK devices, which did reach vkCreateDevice with the structure,
  # came from a driver that reports shaderDrawParameters: geometryStreams is what the G78 r38p0 lacks.
  check "DXVK 1.10.3 itself refuses without shaderDrawParameters, before any vkCreateDevice ($(grep -m1 'Requested feature level' out/t-fc-d3d11-both.log | sed 's/^ *//'))" \
    "grep -q 'Requested feature level not supported' out/t-fc-d3d11-both.log && ! grep -q 'vkCreateDevice' $VKB_SERVER_LOG"
fi

echo "== 7. drawparams imitated, asked in VkPhysicalDeviceVulkan11Features"
: >"$VKB_SERVER_LOG"
vkb_server_start VKBRIDGE_FAKE_MISSING=drawparams
timeout 60 ./out/featclamp_check v11 >out/t-fc-v11-on.log 2>&1
check "the client sees it 0 in the 1.1 features ($(grep -m1 '^features' out/t-fc-v11-on.log))" \
  "grep -q '^features: v11 shaderDrawParameters=0' out/t-fc-v11-on.log"
check "the device is made, only shaderDrawParameters taken off ($(grep -m1 '^device' out/t-fc-v11-on.log))" \
  "grep -q '^device: 0' out/t-fc-v11-on.log && \
   grep -q 'turned on VkPhysicalDeviceVulkan11Features.shaderDrawParameters, which the driver lacks; taken off' $VKB_SERVER_LOG && \
   [ \$(grep -c 'which the driver lacks; taken off' $VKB_SERVER_LOG) = 1 ]"
: >"$VKB_SERVER_LOG"
vkb_server_start VKBRIDGE_FAKE_MISSING=drawparams VKBRIDGE_FEATURE_CLAMP=0
timeout 60 ./out/featclamp_check v11 >out/t-fc-v11-off.log 2>&1
check "the clamp off: refused -8, named ($(grep -m1 "$LACKS" $VKB_SERVER_LOG | sed 's/.*lacks: //'))" \
  "grep -q '^device: -8' out/t-fc-v11-off.log && grep \"$LACKS\" $VKB_SERVER_LOG | grep -q 'VkPhysicalDeviceVulkan11Features.shaderDrawParameters'"

echo "== 8. the desktop driver as it is, shaderDrawParameters asked"
: >"$VKB_SERVER_LOG"
vkb_server_start
timeout 60 ./out/featclamp_check drawparams >out/t-fc-dp-plain.log 2>&1
check "the device is made, nothing taken off ($(grep -m1 '^features: shader' out/t-fc-dp-plain.log); $(grep -m1 '^device' out/t-fc-dp-plain.log))" \
  "grep -q '^features: shaderDrawParameters=1' out/t-fc-dp-plain.log && grep -q '^device: 0' out/t-fc-dp-plain.log && \
   ! grep -q 'which the driver lacks; taken off' $VKB_SERVER_LOG"

DCBIT=$((1 << 28))
# the emulation masks the server logged: the self-test's ("emulating 0x...") and each device's ("emulation 0x... installed")
dc_masks() { grep -oE 'emulating 0x[0-9a-f]+|emulation 0x[0-9a-f]+ installed' "$VKB_SERVER_LOG" | grep -oE '0x[0-9a-f]+'; }
dc_on() { local m n=0; for m in $(dc_masks); do n=$((n+1)); [ $((m & DCBIT)) = 0 ] && return 1; done; [ "$n" -gt 0 ]; }
dc_off() { local m n=0; for m in $(dc_masks); do n=$((n+1)); [ $((m & DCBIT)) != 0 ] && return 1; done; [ "$n" -gt 0 ]; }
echo "== 9. depthClamp imitated (PR #31)"
: >"$VKB_SERVER_LOG"
vkb_server_start VKBRIDGE_FAKE_MISSING=depthclamp VKBRIDGE_FAKE_API=1.1
timeout 60 ./out/featclamp_check depthclamp >out/t-fc-dc-on.log 2>&1
check "the client sees depthClamp 1, the device asking for it is made ($(grep -m1 '^features' out/t-fc-dc-on.log); $(grep -m1 '^device' out/t-fc-dc-on.log))" \
  "grep -q '^features: depthClamp=1' out/t-fc-dc-on.log && grep -q '^device: 0' out/t-fc-dc-on.log"
check "the emulation is on in the self-test and the device ($(dc_masks | tr '\n' ' '))" "dc_on"
echo "== 10. depthClamp imitated, VKBRIDGE_EMU_DEPTHCLAMP=0"
: >"$VKB_SERVER_LOG"
vkb_server_start VKBRIDGE_FAKE_MISSING=depthclamp VKBRIDGE_FAKE_API=1.1 VKBRIDGE_EMU_DEPTHCLAMP=0
timeout 60 ./out/featclamp_check depthclamp >out/t-fc-dc-off.log 2>&1
check "the emulation stays off ($(dc_masks | tr '\n' ' '))" "dc_off && grep -q '^device: 0' out/t-fc-dc-off.log"
echo "== 11. the desktop driver as it is, depthClamp asked"
: >"$VKB_SERVER_LOG"
vkb_server_start
timeout 60 ./out/featclamp_check depthclamp >out/t-fc-dc-plain.log 2>&1
check "the driver's own depthClamp, the emulation off, the device made ($(grep -m1 '^features' out/t-fc-dc-plain.log); $(dc_masks | tr '\n' ' '))" \
  "grep -q '^features: depthClamp=1' out/t-fc-dc-plain.log && grep -q '^device: 0' out/t-fc-dc-plain.log && dc_off"
echo "== 12. depthClamp imitated on the 1.3 driver as it is"
# VKBRIDGE_FAKE_API=0: the driver's own version, also under the suite's Mali profile (whose shim gives the
# server VKB_PROFILE_FAKE_API, 1.1, wherever the test sets none)
: >"$VKB_SERVER_LOG"
vkb_server_start VKBRIDGE_FAKE_MISSING=depthclamp VKBRIDGE_FAKE_API=0
timeout 60 ./out/featclamp_check depthclamp >out/t-fc-dc-13.log 2>&1
check "not reported on a 1.3+ driver, said once ($(dc_masks | tr '\n' ' '))" \
  "dc_off && grep -q '^device: 0' out/t-fc-dc-13.log && [ \$(grep -c 'depthClamp: the driver has none; not reported on a Vulkan 1\.[3-9] driver' $VKB_SERVER_LOG) = 1 ]"
: >"$VKB_SERVER_LOG"
vkb_server_start VKBRIDGE_FAKE_MISSING=depthclamp VKBRIDGE_FAKE_API=0 VKBRIDGE_EMU_DEPTHCLAMP=1
timeout 60 ./out/featclamp_check depthclamp >out/t-fc-dc-13on.log 2>&1
check "VKBRIDGE_EMU_DEPTHCLAMP=1 reports it on the 1.3 driver ($(grep -m1 '^features' out/t-fc-dc-13on.log); $(dc_masks | tr '\n' ' '))" \
  "grep -q '^features: depthClamp=1' out/t-fc-dc-13on.log && grep -q '^device: 0' out/t-fc-dc-13on.log && dc_on"

echo "featclamp_test: $pass passed, $fail failed"
[ "$fail" = 0 ]
