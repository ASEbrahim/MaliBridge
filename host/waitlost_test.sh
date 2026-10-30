#!/bin/bash
# server_wait.c's check of a timed-out semaphore wait answered VK_ERROR_DEVICE_LOST, a server per step.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
work=$(mktemp -d)
sock=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-waitlost-$$.sock
log=$work/server.log
srv=
# start <env line>...: a fresh cache directory whose env holds the lines, then the server
start() {
  [ -n "$srv" ] && kill $srv 2>/dev/null && wait $srv 2>/dev/null
  rm -f "$sock"
  : > "$log"
  cache=$work/c$steps
  rm -rf "$cache"; mkdir -p "$cache"
  printf '%s\n' "$@" > "$cache/env"
  env -u VKBRIDGE_FAKE_MISSING -u VKBRIDGE_HIDE_EXTS -u VKBRIDGE_ERRATA -u VKBRIDGE_ERRATA_IMITATE -u VKBRIDGE_WAIT_FIX \
      -u VKBRIDGE_WAIT_FAKE_ERROR -u VKBRIDGE_WAIT_FAKE_RESULT -u VKBRIDGE_WAIT_FAKE_LOST -u VKBRIDGE_WAIT_LOST_FIX \
      -u VKBRIDGE_WAIT_RETRY_MS -u VKBRIDGE_WAIT_GRACE_S -u VKBRIDGE_ERRATA_PROBE -u VKBRIDGE_PROBE -u VKBRIDGE_PROBE_FAKE \
      VK_DRIVER_FILES=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json} VK_ICD_FILENAMES= \
      "$out/vkbridge-server" --socket "$sock" --log "$log" --cache-dir "$cache" >/dev/null 2>&1 &
  srv=$!
  for _ in $(seq 50); do [ -S "$sock" ] && break; sleep 0.1; done
  # the probe's start part (waits is in it) is done before the server listens; give its log a moment
  for _ in $(seq 100); do grep -q "probe: .*waits\|probe:.*done\|probe: .*profile" "$log" 2>/dev/null && break; sleep 0.1; done
}
trap 'kill $srv 2>/dev/null; rm -f "$sock"; [ -n "$KEEP" ] || rm -rf "$work"' EXIT
export VKBRIDGE_SOCKET=$sock VK_DRIVER_FILES=$out/vkbridge_icd.json VK_ICD_FILENAMES= VKBRIDGE_QUERY_MIRROR=0
unset VKBRIDGE_HIDE_EXTS
fails=0 steps=0
step() { steps=$((steps+1)); if [ "$1" = 0 ]; then echo "  ok   $2"; else echo "  FAIL $2"; fails=$((fails+1)); fi; }
check() { local name=$1; shift; timeout 120 "$out/wait_check" "$@" >$work/$name.log 2>&1; }
cases="sem-finite sem-infinite fence-finite fence-infinite"
line="wait_retry: vkWaitSemaphores: the driver answered a wait with VK_ERROR_DEVICE_LOST after "
flagline="probe: waits fail (.*a timed-out wait answered VK_ERROR_DEVICE_LOST with the device fine.*): timed-out semaphore waits the driver answers with VK_ERROR_DEVICE_LOST are checked"
n() { grep -c "$1" "$log"; }

# 1
start VKBRIDGE_WAIT_FAKE_LOST=1
check 1 ok $cases sem-slow sem-nosubmit
r=$?
[ $r = 0 ] && grep -q "$flagline" "$log" && grep -q "${line}0\.[0-9]* s (timeout 0\.300 s) with the device fine (semaphore at 0, wants 1): the app gets 2 (VK_TIMEOUT)" "$log" &&
  [ "$(n "$line")" = 1 ]
step $? "imitated, probe failed: sem-finite VK_TIMEOUT at 300 ms, sem-slow VK_SUCCESS (grace), sem-nosubmit VK_TIMEOUT at 1 s; $(n "$line") check line ($(tail -1 $work/1.log))"
# 2
start VKBRIDGE_WAIT_FAKE_LOST=1 VKBRIDGE_WAIT_LOST_FIX=0
check 2 lost sem-finite sem-infinite
[ $? = 0 ] && grep -q "$flagline" "$log" && [ "$(n "$line")" = 0 ] &&
  grep -q "wait_retry: timed-out semaphore waits answered VK_ERROR_DEVICE_LOST: passed on (VKBRIDGE_WAIT_LOST_FIX=0)" "$log"
step $? "... VKBRIDGE_WAIT_LOST_FIX=0: the app gets VK_ERROR_DEVICE_LOST ($(grep -m1 sem-finite $work/2.log))"
# 3
start
grep -q "probe: waits fail" "$log"; probe_failed=$?
printf '%s\n' VKBRIDGE_WAIT_FAKE_LOST=1 > "$cache/env"
check 3 lost sem-finite sem-infinite
[ $? = 0 ] && [ $probe_failed != 0 ] && [ "$(n "$line")" = 0 ]
step $? "imitation after a clean probe: gated off, the app gets VK_ERROR_DEVICE_LOST ($(grep -m1 sem-finite $work/3.log))"
# 4
start
check 4 ok $cases sem-slow sem-nosubmit
[ $? = 0 ] && ! grep -q "probe: waits fail" "$log" && [ "$(n "$line")" = 0 ]
step $? "no imitation (a driver without the bug): probe passes, waits as before, no check line ($(tail -1 $work/4.log))"
# 5
start VKBRIDGE_WAIT_LOST_FIX=1
check 5 ok $cases sem-slow sem-nosubmit
[ $? = 0 ] && [ "$(n "$line")" = 0 ] && grep -q "VK_ERROR_DEVICE_LOST: checked (VKBRIDGE_WAIT_LOST_FIX=1)" "$log"
step $? "VKBRIDGE_WAIT_LOST_FIX=1 without the bug: the same, no check line ($(tail -1 $work/5.log))"

if [ $fails = 0 ]; then echo "OK: $steps steps"; else echo "FAILED: $fails of $steps steps"; exit 1; fi
