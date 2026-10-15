#!/bin/bash
# Waits the driver fails with -3 (the wait_retry erratum, server_wait.c, MK-01) and the
# server-side imitation of an older Mali's feature set, on the desktop driver with a server of its
# own (its own socket and cache directory, so <cache dir>/env is the tablet's way of setting them).
# The bug is imitated beneath the fix (VKBRIDGE_ERRATA_IMITATE=wait_retry; VKBRIDGE_WAIT_FAKE_ERROR
# says how); wait_check makes every kind of wait the bridge forwards:
#   1  each kind failed once by the "driver": with the fix a finite wait on what is never signalled
#      ends in VK_TIMEOUT at its deadline (300 ms), the others in VK_SUCCESS; one line per kind;
#   2  the same with VKBRIDGE_ERRATA=-wait_retry: each kind reaches the app as -3 (the negative
#      control); 3 VKBRIDGE_WAIT_FIX=0 (v0.4's switch) does the same;
#   4  every call failed: only the deadline ends a finite wait (VK_TIMEOUT), polling ends the rest;
#   5  every other call failed, the fix's retries too;
#   6  an idle wait failed for ever, VKBRIDGE_WAIT_RETRY_MS=400: the driver's error after 400 ms;
#   7  the bridge's own wait: a process gone without destroying its device, the server's
#      vkDeviceWaitIdle in the clean-up goes through the fix;
#   8  v0.4's VKBRIDGE_WAIT_FAKE_ERROR=n alone (any build) with the smoke test: the semaphore wait
#      recovers with the fix, -3 without; 9 the wait_eintr probe's result is recorded with it;
#  10  VKBRIDGE_FAKE_MISSING / VKBRIDGE_SERVER_HIDE_EXTS from <cache dir>/env reach the startup
#      self-test: the native features are emulated and the hidden driver extensions are not
#      offered, with no client-side setting (a newer Mali taking an older one's paths);
#  11  the protocol hash is unchanged.
# Prints one line per step and "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
work=$(mktemp -d)
cache=$work/c
mkdir -p "$cache"
sock=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-wait-$$.sock
log=$work/server.log
srv=
start() {
  [ -n "$srv" ] && kill $srv 2>/dev/null && wait $srv 2>/dev/null
  rm -f "$sock"
  : > "$log"
  env -u VKBRIDGE_FAKE_MISSING -u VKBRIDGE_HIDE_EXTS -u VKBRIDGE_ERRATA -u VKBRIDGE_ERRATA_IMITATE -u VKBRIDGE_WAIT_FIX \
      -u VKBRIDGE_WAIT_FAKE_ERROR -u VKBRIDGE_WAIT_RETRY_MS -u VKBRIDGE_ERRATA_PROBE \
      VK_DRIVER_FILES=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json} VK_ICD_FILENAMES= \
      "$out/vkbridge-server" --socket "$sock" --log "$log" --cache-dir "$cache" >/dev/null 2>&1 &
  srv=$!
  for _ in $(seq 50); do [ -S "$sock" ] && break; sleep 0.1; done
}
trap 'kill $srv 2>/dev/null; rm -f "$sock"; [ -n "$KEEP" ] || rm -rf "$work"' EXIT
# The query wait must reach the server, not the client's mirror
export VKBRIDGE_SOCKET=$sock VK_DRIVER_FILES=$out/vkbridge_icd.json VK_ICD_FILENAMES= VKBRIDGE_QUERY_MIRROR=0
unset VKBRIDGE_HIDE_EXTS
fails=0 steps=0
step() { steps=$((steps+1)); if [ "$1" = 0 ]; then echo "  ok   $2"; else echo "  FAIL $2"; fails=$((fails+1)); fi; }
# wait_check <name> <expect> [case...] with <cache dir>/env = the KEY=VALUE lines in $envs
check() {
  local name=$1; shift
  printf '%s\n' "${envs[@]}" > "$cache/env"
  timeout 120 "$out/wait_check" "$@" >$work/$name.log 2>&1
}
all="fence-finite fence-infinite sem-finite sem-infinite queue-idle device-idle query-wait fence-status"
kinds="vkWaitForFences vkWaitSemaphores vkQueueWaitIdle vkDeviceWaitIdle vkGetQueryPoolResults vkGetFenceStatus"

start
# 1
envs=(VKBRIDGE_ERRATA_IMITATE=wait_retry VKBRIDGE_WAIT_FAKE_ERROR=once:fence+sem+idle+query+status)
check 1 ok $all
r=$?; missing=
for k in $kinds; do grep -q "wait_retry: $k.*: the driver failed the wait (-3).*the app gets" "$log" || missing="$missing $k"; done
[ $r = 0 ] && [ -z "$missing" ] && grep -q "errata: wait_retry ON (always; safe mode) - MK-01" "$log" &&
  grep -q "vkWaitForFences.*the app gets 2 (VK_TIMEOUT, its deadline)" "$log"
step $? "each kind failed once: finite waits time out at 300 ms, the rest succeed; logged once per kind$([ -n "$missing" ] && echo ", not:$missing") ($(tail -1 $work/1.log))"
# 2
envs=(VKBRIDGE_ERRATA_IMITATE=wait_retry VKBRIDGE_WAIT_FAKE_ERROR=once:fence+sem+idle+query+status VKBRIDGE_ERRATA=-wait_retry)
check 2 bug $all && grep -q "errata: wait_retry OFF (VKBRIDGE_ERRATA=-wait_retry" "$log"
step $? "negative control, VKBRIDGE_ERRATA=-wait_retry: every kind reaches the app as -3 ($(tail -1 $work/2.log))"
# 3
envs=(VKBRIDGE_ERRATA_IMITATE=wait_retry VKBRIDGE_WAIT_FIX=0)
check 3 bug && grep -q "errata: wait_retry OFF (VKBRIDGE_WAIT_FIX=0" "$log"
step $? "VKBRIDGE_WAIT_FIX=0 still turns the fix off ($(tail -1 $work/3.log))"
# 4
envs=(VKBRIDGE_ERRATA_IMITATE=wait_retry VKBRIDGE_WAIT_FAKE_ERROR=all:fence+sem+query)
check 4 ok fence-finite fence-infinite sem-finite sem-infinite query-wait
step $? "every call failed: finite waits end at their deadline, signalled ones and the query by polling ($(grep -o 'fence-finite.*ms' $work/4.log); $(tail -1 $work/4.log))"
# 5
envs=(VKBRIDGE_ERRATA_IMITATE=wait_retry VKBRIDGE_WAIT_FAKE_ERROR=alt)
check 5 ok
step $? "every other call failed, retries too ($(tail -1 $work/5.log))"
# 6
envs=(VKBRIDGE_ERRATA_IMITATE=wait_retry VKBRIDGE_WAIT_FAKE_ERROR=all:idle VKBRIDGE_WAIT_RETRY_MS=400)
check 6 any queue-idle
ms=$(grep -o 'queue-idle: -3 after [0-9]*' $work/6.log | grep -o '[0-9]*$')
[ -n "$ms" ] && [ "$ms" -ge 390 ] && grep -q "vkQueueWaitIdle.*the app gets -3 (the driver's error: it kept failing)" "$log"
step $? "a driver failing an idle wait for ever: its error after the 400 ms cap (${ms:-?} ms), logged"
# 7
start
envs=(VKBRIDGE_WAIT_FAKE_ERROR=once:idle)
check 7 any leak
sleep 1
sed -n '/exited without destroying device/,$p' "$log" | grep -q "wait_retry: vkDeviceWaitIdle: the driver failed the wait (-3).*the app gets 0"
step $? "the bridge's own wait: the clean-up's vkDeviceWaitIdle of a process gone goes through the fix"
# 8
start
printf 'VKBRIDGE_WAIT_FAKE_ERROR=1:sem\n' > "$cache/env"
timeout 120 "$out/vkbridge-smoke" >$work/8.log 2>&1 && grep -q "PASS timeline semaphore" $work/8.log &&
  grep -q "wait_retry: vkWaitSemaphores: the driver failed the wait (-3).*the app gets 0" "$log"
step $? "v0.4's VKBRIDGE_WAIT_FAKE_ERROR=1:sem alone, the smoke test: the semaphore wait recovers ($(tail -1 $work/8.log))"
printf 'VKBRIDGE_WAIT_FAKE_ERROR=1:sem\nVKBRIDGE_WAIT_FIX=0\n' > "$cache/env"
timeout 120 "$out/vkbridge-smoke" >$work/8b.log 2>&1
grep -q "FAIL timeline semaphore" $work/8b.log
step $? "the same with VKBRIDGE_WAIT_FIX=0: the app sees the driver's error ($(grep -m1 'timeline semaphore' $work/8b.log))"
# 9
printf 'VKBRIDGE_ERRATA_PROBE=wait_eintr:fail\n' > "$cache/env"
timeout 120 "$out/vkbridge-smoke" >$work/9.log 2>&1
grep -q "errata: wait_retry ON (always; safe mode; probe wait_eintr failed, recorded only)" "$log"
step $? "the wait_eintr probe's result recorded with the decision"
# 10
printf 'VKBRIDGE_FAKE_MISSING=depthclip,push,maint5,maint6,dualsrc\nVKBRIDGE_SERVER_HIDE_EXTS=VK_EXT_extended_dynamic_state3\n' > "$cache/env"
start
timeout 60 vulkaninfo >$work/10.vi 2>&1
timeout 120 "$out/vkbridge-smoke" >$work/10.log 2>&1 &&
  grep -q "device extensions hidden from clients: VKBRIDGE_FAKE_MISSING=depthclip" "$log" &&
  emu=$(grep -m1 -o 'emulating 0x[0-9a-f]*' "$log" | cut -d' ' -f2) && [ $((emu & 0x5260)) = $((0x5260)) ] &&
  ! grep -q "VK_EXT_extended_dynamic_state3" $work/10.vi && grep -q "VK_KHR_push_descriptor" $work/10.vi &&
  grep -q "VK_EXT_depth_clip_enable" $work/10.vi
step $? "fake-missing from <cache dir>/env: emulation $emu, EDS3 hidden, emulated push/depth clip offered ($(tail -1 $work/10.log))"
# 11
grep -q "^#define VKB_PROTOCOL_HASH 0x4a9da939fdf65896ULL" ../common/gen/vkb_gen.h
step $? "protocol hash $(grep -o '0x[0-9a-f]*' ../common/gen/vkb_gen.h | head -1)"

if [ $fails = 0 ]; then echo "OK: $steps steps"; else echo "FAILED: $fails of $steps steps"; exit 1; fi
