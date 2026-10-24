#!/bin/bash
# The Mali-G715 r54p3's robustness2 (Pixel 8 / 8 Pro / 8a, Android 17): the extension and
# nullDescriptor, without robustBufferAccess2 / robustImageAccess2 (VKBRIDGE_FAKE_MISSING=robust2access
# imitates it: the feature query says false, a device asking for either is refused). DXVK 3 requires
# robustBufferAccess2 and drops such an adapter; with none left CreateDXGIFactory fails (E_FAIL) and
# no D3D game starts (NieR "No Graphic Memory", Dead Space "CreateDXGIFactory1(IDXGIFactory4) E_FAIL").
#   1. emulation off (VKBRIDGE_EMU_ROBUST2ACCESS=0): the client sees both false, DXVK refuses the
#      adapter (with VKB_WINE_DIR) - the bug as the testers had it
#   2. on (the default): both reported, taken off at vkCreateDevice with robustBufferAccess on, the
#      driver's own nullDescriptor kept; vkbridge-smoke and DXVK's d3d11_check pass
#   3. a driver with the whole of robustness2 (no imitation): no such emulation, nothing changes
# A server of its own (VKB_RUN_DIR); needs out/ built.
cd "$(dirname "$0")"
VKB_RUN_DIR=${VKB_RUN_DIR:-$(mktemp -d "${XDG_RUNTIME_DIR:-/tmp}/vkbridge-r2-$$.XXXX")}
export VKBRIDGE_SOCKET=$VKB_RUN_DIR/r2.sock VKB_SERVER_LOG=$VKB_RUN_DIR/r2.log
unset VKB_SERVER_PID VKBRIDGE_HIDE_EXTS VKBRIDGE_MAX_API
. ./server_start.sh
trap 'vkb_server_stop' EXIT
pass=0; fail=0
check() { if eval "$2"; then pass=$((pass+1)); echo "  ok   $1"; else fail=$((fail+1)); echo "  FAIL $1"; fi; }
# the G715's gaps the bridge emulates, as the server found them (missing 0x8c0f), plus robust2access
G715=bc,clip,cull,divisor,fillmode,multiviewport,texelalign
wine_ok() { [ -n "${VKB_WINE_DIR:-}" ] && [ -d "$VKB_WINE_DIR/app" ]; }

echo "== 1. imitated, emulation off"
: >"$VKB_SERVER_LOG"
vkb_server_start VKBRIDGE_FAKE_MISSING=$G715,robust2access VKBRIDGE_EMU_ROBUST2ACCESS=0
timeout 60 ./out/robust2_check >out/t-r2-off.log 2>&1
check "the client sees robustBufferAccess2 0, robustImageAccess2 0, nullDescriptor 1 ($(grep -m1 '^robustness2' out/t-r2-off.log))" \
  "grep -q '^robustness2: buffer 0 image 0 null 1' out/t-r2-off.log"
check "server: imitation on, the emulation not offered" \
  "grep -q 'imitating the Mali-G715 r54p3' $VKB_SERVER_LOG && grep -q 'not reported (VKBRIDGE_EMU_ROBUST2ACCESS=0)' $VKB_SERVER_LOG"
if wine_ok; then
  ./d3d11_test.sh >out/t-r2-d3d11-off.log 2>&1
  check "DXVK finds no adapter: the testers' failure ($(grep -m1 -o "required feature 'robustBufferAccess2'[^)]*)" "$VKB_WINE_DIR"/d3d11_check_dxgi.log 2>/dev/null))" \
    "! grep -q '^OK:' out/t-r2-d3d11-off.log && grep -q \"required feature 'robustBufferAccess2'\" $VKB_WINE_DIR/d3d11_check_dxgi.log"
fi

echo "== 2. imitated, emulation on (default)"
: >"$VKB_SERVER_LOG"
vkb_server_start VKBRIDGE_FAKE_MISSING=$G715,robust2access
timeout 60 ./out/robust2_check >out/t-r2-on.log 2>&1
check "robust2_check passes ($(tail -1 out/t-r2-on.log))" "grep -q '^OK' out/t-r2-on.log"
check "the client sees all three ($(grep -m1 '^robustness2' out/t-r2-on.log))" "grep -q '^robustness2: buffer 1 image 1 null 1' out/t-r2-on.log"
check "server: both taken off, robustBufferAccess and robustImageAccess on" \
  "grep -q 'robustBufferAccess2 robustImageAccess2 taken off (the driver lacks them; VKB_EMU_ROBUST2_ACCESS); robustBufferAccess on' $VKB_SERVER_LOG && grep -q 'robustImageAccess on (robustness2 robustImageAccess2 reported)' $VKB_SERVER_LOG"
check "no device refused by the imitated driver" "! grep -q 'imitated robustness2: device refused' $VKB_SERVER_LOG"
check "the driver's null descriptors kept (no null stand-ins)" "! grep -q 'null stand-in' $VKB_SERVER_LOG"
timeout 120 ./out/vkbridge-smoke >out/t-r2-smoke.log 2>&1
check "vkbridge-smoke ($(tail -1 out/t-r2-smoke.log))" "grep -q '^OK:' out/t-r2-smoke.log"
if wine_ok; then
  ./d3d11_test.sh >out/t-r2-d3d11-on.log 2>&1
  check "d3d11_check via DXVK ($(grep -E '^(OK|FAILED)' out/t-r2-d3d11-on.log))" "grep -q '^OK:' out/t-r2-d3d11-on.log"
fi

echo "== 3. the driver's whole robustness2"
: >"$VKB_SERVER_LOG"
vkb_server_start VKBRIDGE_FAKE_MISSING=$G715
timeout 60 ./out/robust2_check >out/t-r2-plain.log 2>&1
check "robust2_check passes, the driver's values ($(grep -m1 '^robustness2' out/t-r2-plain.log))" \
  "grep -q '^OK' out/t-r2-plain.log && grep -q '^robustness2: buffer 1 image 1 null 1' out/t-r2-plain.log"
check "server: no such emulation, nothing taken off" "! grep -q 'taken off (the driver lacks them' $VKB_SERVER_LOG && ! grep -q 'robustness2: the driver.s has' $VKB_SERVER_LOG"

echo "robust2_test: $pass passed, $fail failed"
[ "$fail" = 0 ]
