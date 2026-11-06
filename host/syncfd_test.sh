#!/bin/bash
# Galaxy S24+, Xclipse 940, 2026-10-28: Xwayland sat 15 minutes in vkGetSemaphoreFdKHR
# (a sync-fd export) and the X server with it. A driver that keeps a batch in user space until its
# waits are met answers a sync-fd export of what that batch signals only when it goes out; RADV
# does that on the host (tests/syncfd_check.c: a batch held 3 s on a timeline value). A server of
# its own per step:
#   1  the bound off (VKBRIDGE_SYNCFD_EXPORT_MS=0): the export through the bridge waits the 3 s
#      (what Xwayland met, there with no end); skipped where the driver does not block;
#   2  VKBRIDGE_SYNCFD_EXPORT_MS=500: the export returns in about 0.5 s with fd -1, the semaphore is
#      destroyed at once and that destroy is held back until the driver's call returns; the later
#      ordinary export is quick and real;
#   3  a submit held 6.5 s in the server (VKBRIDGE_TEST_DELAY_CMD), a vkDeviceWaitIdle on another thread: the
#      log names the held vkQueueSubmit as what the wait waited for (a stall's cause in a log);
#   4  nothing set on a driver that is not Samsung's: the bound stays off;
#   5  Galaxy S24+, v0.5.4.2, the export bound on: a submit held 6.5 s and its export right after on the
#      same thread, the order deadline off (VKBRIDGE_SYNCFD_ORDER_MS=0): the export waits the whole 6.5 s and the log
#      has no line about it (the export sits unread behind the held submit on its own connection);
#   6  the same with VKBRIDGE_SYNCFD_ORDER_MS=500: the export is answered at 0.5 s with fd -1, the log names the held
#      submit, the driver's export is not called;
#   7  the same deadline with the client's side flush off (VKBRIDGE_SYNCFD_SIDE=0 in the app): the export waits the
#      whole 6.5 s again (the client's half is needed: the server never reads the export until the submit is out).
#   8  X5 (Xwayland's vkEndCommandBuffer never returned on Xclipse 940 and 540): one recorded command of a
#      replayed stream held 6.5 s (vkCmdSetViewport), a vkDeviceWaitIdle waiting behind the End: the log's line names the
#      command, its place in the stream (command 4 of 5) and the ones before it, and the thread's state;
#   9  the same with the driver's own End after the stream held: the line says so and lists the stream's last commands.
# Prints one line per step and "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
work=$(mktemp -d)
sock=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-syncfd-$$.sock
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
  env -u VKBRIDGE_FAKE_MISSING -u VKBRIDGE_SYNCFD_EXPORT_MS -u VKBRIDGE_SYNCFD_ORDER_MS -u VKBRIDGE_TEST_DELAY_CMD -u VKBRIDGE_TEST_DELAY_MS \
      VKBRIDGE_PROBE_BUDGET_S=0 "$@" VK_DRIVER_FILES=$host_icd VK_ICD_FILENAMES= \
      "$out/vkbridge-server" --socket "$sock" --log "$log" --cache-dir "$d/c" >/dev/null 2>&1 &
  srv=$!
  for _ in $(seq 300); do [ -S "$sock" ] && break; kill -0 $srv 2>/dev/null || break; sleep 0.05; done
}
check() { timeout 60 "$out/syncfd_check" "$1" >"$work/check-$2.log" 2>&1; tail -1 "$work/check-$2.log"; }
trap 'stop; rm -f "$sock"; [ -n "$KEEP" ] && echo "kept $work" || rm -rf "$work"' EXIT
export VKBRIDGE_SOCKET=$sock VK_DRIVER_FILES=$out/vkbridge_icd.json VK_ICD_FILENAMES=
unset VKBRIDGE_HIDE_EXTS VKBRIDGE_MAX_API VKBRIDGE_FAKE_MISSING VKBRIDGE_MODE VKBRIDGE_SYNCFD_SIDE
fails=0 steps=0
step() { steps=$((steps+1)); if [ "$1" = 0 ]; then echo "  ok   $2"; else echo "  FAIL $2"; fails=$((fails+1)); fi; }

# 1
start off VKBRIDGE_SYNCFD_EXPORT_MS=0
c1=$(check direct off)
case "$c1" in
SKIP*) echo "  skip the driver does not block a sync-fd export ($c1)"; noblock=1 ;;
*) grep -q "^OK" <<<"$c1" && ! grep -q "sync-fd export:" "$log"
   step $? "the bound off: the export waits for the held batch, as Xwayland did ($c1; $(grep -m1 'held batch' "$work/check-off.log" | sed 's/^ *//'))" ;;
esac

# 2
if [ -z "$noblock" ]; then
  start bounded VKBRIDGE_SYNCFD_EXPORT_MS=500
  c2=$(check bounded bounded)
  sleep 1 # the driver's call returns about 3 s in; its line follows
  grep -q "^OK" <<<"$c2" && grep -q "sync-fd export: answered within 500 ms (VKBRIDGE_SYNCFD_EXPORT_MS)" "$log" &&
    grep -q "sync-fd export: the driver's vkGetSemaphoreFdKHR has not returned in 500 ms (pid [0-9]* syncfd_check" "$log" &&
    grep -q "a destroy of a handle whose export is still in the driver held back" "$log" &&
    grep -q "sync-fd export: the driver's vkGetSemaphoreFdKHR returned after [23]\.[0-9] s (0; 1 so far), its fd closed; the held-back destroy done" "$log" &&
    ! grep -q "left alive" "$log"
  step $? "VKBRIDGE_SYNCFD_EXPORT_MS=500: answered at the deadline with fd -1, the destroy done after the driver's call ($c2; $(grep -m1 'held batch' "$work/check-bounded.log" | sed 's/^ *//'))"
fi

# 3
start stall VKBRIDGE_TEST_DELAY_CMD=vkQueueSubmit VKBRIDGE_TEST_DELAY_MS=6500
c3=$(check stall stall)
grep -q "^OK" <<<"$c3" && grep -q "test: vkQueueSubmit held 6500 ms" "$log" &&
  grep -q "process [0-9]* (syncfd_check): vkDeviceWaitIdle has waited 5 s for the process's earlier requests (up to [0-9]*, [0-9]* done); running: vkQueueSubmit for [4-6]\.[0-9] s \[its thread ([0-9]*) is " "$log"
step $? "a held asynchronous submit: the waiting call's line names it ($c3; $(grep -m1 -o 'vkDeviceWaitIdle has waited.*' "$log"))"

# 4
start default
c4=$(check any default)
grep -q "^OK" <<<"$c4" && ! grep -q "sync-fd export:" "$log"
step $? "nothing set, not Samsung's driver: no bound ($c4)"

# 5
start orderoff VKBRIDGE_SYNCFD_ORDER_MS=0 VKBRIDGE_TEST_DELAY_CMD=vkQueueSubmit VKBRIDGE_TEST_DELAY_MS=6500
c5=$(check orderoff orderoff)
grep -q "^OK" <<<"$c5" && grep -q "test: vkQueueSubmit held 6500 ms" "$log" && ! grep -q "has waited\|the deadline\|sync-fd export:" "$log"
step $? "the order deadline off: the export waits behind the held submit with no line in the log ($c5; $(grep -m1 'same thread' "$work/check-orderoff.log" | sed 's/^ *//'))"

# 6
start order VKBRIDGE_SYNCFD_ORDER_MS=500 VKBRIDGE_TEST_DELAY_CMD=vkQueueSubmit VKBRIDGE_TEST_DELAY_MS=6500
c6=$(check order order)
grep -q "^OK" <<<"$c6" && grep -q "sync-fd export: waits at most 500 ms for the process's earlier requests (VKBRIDGE_SYNCFD_ORDER_MS)" "$log" &&
  grep -q "process [0-9]* (syncfd_check): vkGetSemaphoreFdKHR waited 500 ms for the process's earlier requests (up to [0-9]*, [0-9]* done), the deadline; running: vkQueueSubmit for 0\.[0-9] s;" "$log" &&
  grep -q "sync-fd export: vkGetSemaphoreFdKHR answered with fd -1 after the order wait's deadline, the driver not called (pid [0-9]* syncfd_check; 1 so far" "$log" &&
  ! grep -q "has not returned" "$log"
step $? "VKBRIDGE_SYNCFD_ORDER_MS=500: the export is answered at the deadline with fd -1, the held submit named ($c6; $(grep -m1 'same thread' "$work/check-order.log" | sed 's/^ *//'))"

# 7
start sideoff VKBRIDGE_SYNCFD_ORDER_MS=500 VKBRIDGE_TEST_DELAY_CMD=vkQueueSubmit VKBRIDGE_TEST_DELAY_MS=6500
c7=$(VKBRIDGE_SYNCFD_SIDE=0 check orderoff sideoff)
grep -q "^OK" <<<"$c7" && ! grep -q "the deadline" "$log"
step $? "the client's side flush off: the export waits behind the held submit despite the deadline ($c7; $(grep -m1 'same thread' "$work/check-sideoff.log" | sed 's/^ *//'))"

# 8
start endcmd VKBRIDGE_TEST_DELAY_CMD=vkCmdSetViewport VKBRIDGE_TEST_DELAY_MS=6500
c8=$(check endstall endcmd)
grep -q "^OK" <<<"$c8" && grep -q "test: vkCmdSetViewport held 6500 ms" "$log" &&
  grep -q "process [0-9]* (syncfd_check): vkDeviceWaitIdle has waited 5 s for the process's earlier requests (up to [0-9]*, [0-9]* done); running: vkEndCommandBuffer for [4-6]\.[0-9] s (inside vkCmdSetViewport, command 4 of 5; after: vkBeginCommandBuffer, vkCmdPipelineBarrier, vkCmdPipelineBarrier)" "$log"
step $? "a held replayed command: the waiting call's line names it and its place ($c8; $(grep -m1 -o 'running: vkEndCommandBuffer.*' "$log" | cut -c1-230))"

# 9
start endhold VKBRIDGE_TEST_DELAY_CMD=driver-end VKBRIDGE_TEST_DELAY_MS=6500
c9=$(check endstall endhold)
grep -q "^OK" <<<"$c9" && grep -q "test: the driver's End held 6500 ms" "$log" &&
  grep -q "running: vkEndCommandBuffer for [4-6]\.[0-9] s (in the driver's own End, after the stream of 5 command(s); the last: vkBeginCommandBuffer, vkCmdPipelineBarrier, vkCmdPipelineBarrier, vkCmdSetViewport, vkCmdPipelineBarrier)" "$log"
step $? "a held driver End: the line says it is past the stream and lists its last commands ($c9; $(grep -m1 -o 'running: vkEndCommandBuffer.*' "$log" | cut -c1-230))"
stop

if [ $fails = 0 ]; then echo "OK: $steps steps"; else echo "FAILED: $fails of $steps steps"; exit 1; fi
