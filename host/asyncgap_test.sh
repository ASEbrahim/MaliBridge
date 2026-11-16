#!/bin/bash
# a player log (OPPO PME110, Mali-G1-Ultra, Dying Light, v0.5.5.2): "process 30634: async request 823 never arrived;
# continuing", 2 ms later a SIGSEGV at 0x32 in Mali's vkQueueSubmit2 ended the server and every program (twice in that
# session). The order wait took any 3 s wait as a request that would never come once any connection of the program had
# ended (every thread that exits ends its own), while the request was an asynchronous vkEndCommandBuffer replay still
# running (waiting for its BC transcoders); the submit of that command buffer then ran beside the replay.
# tests/asyncgap_check.c, a server of its own per step:
#   1  the End's replay held 5 s (VKBRIDGE_TEST_DELAY_CMD=driver-end), its submit + fence wait on another thread after a
#      thread's connection ended: the submit runs only after the End (fence after >= 4.5 s), no "never arrived", the
#      wait's line names the End it waits for, the server lives;
#   2  the same under VKBRIDGE_ORDER_GAP=continue (the rule before the fix): "never arrived; continuing" and the submit run
#      before the End was done (fence before 4.5 s) or the server dead - the arm that fails without the fix;
#   3  the End never sent (VKBRIDGE_TEST_DROP_ASYNC=vkEndCommandBuffer in the app): a real gap; after 3 s the program's
#      device is taken as lost, the submit never reaches the driver (fence wait -4), the server lives;
#   4  the same under VKBRIDGE_ORDER_GAP=continue: "continuing", the submit of the never-ended command buffer reaches the
#      driver (no -4) or the server dies;
#   5  nothing held or dropped: the fence comes at once, no order line.
# Prints one line per step and "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
work=$(mktemp -d)
sock=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-asyncgap-$$.sock
host_icd=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json}
srv=
log=
stop() { [ -n "$srv" ] && kill $srv 2>/dev/null && wait $srv 2>/dev/null; srv=; }
start() { # start <name> [env...]: a server of its own, log $log
  local d=$work/$1
  shift
  stop
  rm -f "$sock"
  mkdir -p "$d/c"
  log=$d/vkbridge-server.log
  : > "$log"
  env -u VKBRIDGE_TEST_DELAY_CMD -u VKBRIDGE_TEST_DELAY_MS -u VKBRIDGE_ORDER_GAP \
      VKBRIDGE_PROBE_BUDGET_S=0 "$@" VK_DRIVER_FILES=$host_icd VK_ICD_FILENAMES= \
      "$out/vkbridge-server" --socket "$sock" --log "$log" --cache-dir "$d/c" >/dev/null 2>&1 &
  srv=$!
  for _ in $(seq 300); do [ -S "$sock" ] && break; kill -0 $srv 2>/dev/null || break; sleep 0.05; done
}
check() { # check <mode> <name> [app env...]
  local m=$1 n=$2
  shift 2
  env "$@" timeout 60 "$out/asyncgap_check" "$m" >"$work/check-$n.log" 2>&1
  tail -1 "$work/check-$n.log"
}
alive() { kill -0 $srv 2>/dev/null; }
ms_of() { sed -n 's/.* ms=\([0-9]*\).*/\1/p' <<<"$1"; }
trap 'stop; rm -f "$sock"; [ -n "$KEEP" ] && echo "kept $work" || rm -rf "$work"' EXIT
export VKBRIDGE_SOCKET=$sock VK_DRIVER_FILES=$out/vkbridge_icd.json VK_ICD_FILENAMES=
unset VKBRIDGE_HIDE_EXTS VKBRIDGE_MAX_API VKBRIDGE_FAKE_MISSING VKBRIDGE_MODE VKBRIDGE_TEST_DROP_ASYNC VKBRIDGE_SYNC
fails=0 steps=0
step() { steps=$((steps+1)); if [ "$1" = 0 ]; then echo "  ok   $2"; else echo "  FAIL $2"; fails=$((fails+1)); fi; }
hold="VKBRIDGE_TEST_DELAY_CMD=driver-end VKBRIDGE_TEST_DELAY_MS=5000"

# 1
start held $hold
c1=$(check held held); m1=$(ms_of "$c1")
grep -q "wait=0 " <<<"$c1" && [ "${m1:-0}" -ge 4500 ] && grep -q "test: the driver's End held 5000 ms" "$log" &&
  ! grep -q "never arrived" "$log" &&
  grep -q "waits over 3 s for its asynchronous request [0-9]* while vkEndCommandBuffer runs on another connection; waiting for it" "$log" && alive
step $? "a held End, a connection ended: the submit waits for the End ($c1; $(grep -m1 -o 'waits over 3 s.*' "$log" | cut -c1-150))"

# 2
start heldold $hold VKBRIDGE_ORDER_GAP=continue
c2=$(check held heldold); m2=$(ms_of "$c2")
grep -q "async request [0-9]* never arrived; continuing" "$log" && { [ "${m2:-0}" -lt 4500 ] || ! alive; }
step $? "the same, the rule before the fix: gone past while the End still ran ($c2; server $(alive && echo alive || echo dead); $(grep -m1 -o 'async request.*' "$log"))"

# 3
start drop
c3=$(check drop drop VKBRIDGE_TEST_DROP_ASYNC=vkEndCommandBuffer)
grep -q "wait=-4 " <<<"$c3" && grep -q "VKBRIDGE_TEST_DROP_ASYNC: vkEndCommandBuffer (request [0-9]*) not sent" "$work/check-drop.log" &&
  grep -q "asynchronous request [0-9]* never arrived (a connection of the program ended without sending it, and nothing of the program ran for 3 s); [A-Za-z0-9]* goes on and its 1 device(s) are taken as lost" "$log" &&
  grep -q "device [0-9]* (pid [0-9]*) is lost (an asynchronous request of its program never arrived)" "$log" && alive
step $? "an End never sent: the device taken as lost, its submit kept from the driver ($c3; $(grep -m1 -o 'asynchronous request [0-9]* never arrived.\{0,60\}' "$log"))"

# 4
start dropold VKBRIDGE_ORDER_GAP=continue
c4=$(check drop dropold VKBRIDGE_TEST_DROP_ASYNC=vkEndCommandBuffer)
grep -q "async request [0-9]* never arrived; continuing" "$log" && { ! grep -q "wait=-4 " <<<"$c4" || ! alive; }
step $? "the same, the rule before the fix: the never-ended command buffer submitted to the driver ($c4; server $(alive && echo alive || echo dead))"

# 5
start plain
c5=$(check held plain); m5=$(ms_of "$c5")
grep -q "wait=0 " <<<"$c5" && [ "${m5:-9999}" -lt 2500 ] && ! grep -q "never arrived\|waits over 3 s" "$log" && alive
step $? "nothing held: the fence at once, no order line ($c5)"
stop

if [ $fails = 0 ]; then echo "OK: $steps steps"; else echo "FAILED: $fails of $steps steps"; exit 1; fi
