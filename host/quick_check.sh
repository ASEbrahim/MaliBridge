#!/bin/bash
# A quick check of a few feature tests for the MaliBridge
#
#   host/quick_check.sh [plain|mali|both]     (default: both)
cd "$(dirname "$0")"
mkdir -p out
mode=${1:-both}
export VKB_RUN_DIR=$(mktemp -d "$PWD/out/quick-$$-XXXX")
export VKBRIDGE_SOCKET=$VKB_RUN_DIR/server.sock VKB_SERVER_LOG=$VKB_RUN_DIR/server.log
unset VKBRIDGE_CACHE_DIR VKB_SERVER_PID
. ./server_start.sh
trap 'vkb_server_stop; rm -rf "$VKB_RUN_DIR"' EXIT
pass=0; fail=0
result() { if [ "$1" = 0 ]; then pass=$((pass+1)); echo "  ok   $2"; else fail=$((fail+1)); echo "  FAIL $2"; fi; }
# A server-side protocol error fails the run even when the desktop driver shrugs it off.
server_errors() {
  local n; n=$(grep -cE "command stream:|malformed request|unknown command id|the driver has no|named ahead failed|bad batch entry" "$VKB_SERVER_LOG" 2>/dev/null)
  [ "${n:-0}" = 0 ] || { echo "  server log: $n protocol errors, first: $(grep -m1 -E "command stream:|malformed request|unknown command id|the driver has no|named ahead failed|bad batch entry" "$VKB_SERVER_LOG")"; return 1; }
}
run_mode() {
  local m=$1
  echo "== $m"
  : > "$VKB_SERVER_LOG"
  if [ "$m" = mali ]; then . ./mali_env.sh >/dev/null; else unset VKBRIDGE_HIDE_EXTS VKBRIDGE_MAX_API; . ./bridge_env.sh >/dev/null; fi
  # The handshake, the device creation and the extension list the app is shown.
  timeout 120 ./out/vkbridge-smoke >out/q-smoke.log 2>&1; result $? "smoke ($(tail -1 out/q-smoke.log))"
  # The entry points and the extension names the app is shown (the EXT/KHR aliases).
  timeout 60 ./out/procaddr_check >out/q-procaddr.log 2>&1; result $? "procaddr_check ($(tail -1 out/q-procaddr.log))"
  # The multi-planar formats: the flag the Mali-G925 refuses, taken off where the format covers it.
  timeout 60 ./out/planar_check >out/q-planar.log 2>&1; result $? "planar_check ($(tail -1 out/q-planar.log))"
  # The divisor emulation, and the EXT name when the driver has only the KHR one.
  timeout 60 ./out/divisor_check >out/q-divisor.log 2>&1; result $? "divisor_check ($(tail -1 out/q-divisor.log))"
  # The descriptor limits the app is shown.
  timeout 60 ./out/desc_limits_check >out/q-desc.log 2>&1; result $? "desc_limits_check ($(tail -1 out/q-desc.log))"
  # The memory types and the device properties the app is shown.
  timeout 60 ./out/memclass_check >out/q-memclass.log 2>&1; result $? "memclass_check ($(tail -1 out/q-memclass.log))"
  server_errors; result $? "no protocol errors in the server log"
}
[ "$mode" = plain ] || run_mode plain
[ "$mode" = mali ] || run_mode mali
echo "== $pass passed, $fail failed"
[ "$fail" = 0 ]
