#!/bin/bash
# The hang detector armed by a GPU job that never finished, with no lost device (Elden Ring on the S11 Ultra, Mali-G925
# r49p1, a player log: 83 GPU read faults, then the GPU 99-100% busy for 130 s with no frame; vkd3d's timeline
# wait 'wants 89844, at 89843' failed by the driver (-3) every 5 s; the driver never lost the device, so the fault-arm
# (a loss with a fault address, faultarm_test.sh) never armed, and nothing named the job). A wait with no deadline
# that the driver keeps failing until the retry cap, or a semaphore wait still not done at the end of the grace, arms
# the detector for that program's next launch (<cache dir>/hang-detect-next/<program@key>), once.
#   1  an idle wait (no deadline) failed for ever past the cap: armed, one line, the file;
#   2  the next launch of that program: the detector is on, the line says why; the file is taken;
#   3  finite waits failed until their own deadline (VK_TIMEOUT there, the app's answer): not armed;
#   4  VKBRIDGE_FAULT_ARM=0: not armed;
#   5  GPU jobs that take seconds and finish (Resident Evil Village, a player log: every frame's work killed by the driver's
#      progress timer after ~7 s, the device never lost): the VKBRIDGE_HUNG_JOBS jobs of VKBRIDGE_HUNG_JOB_MS or more
#      in 60 s arm it (here 1 job of 1 s: sem-slow's 1.5 s job);
#   6  the defaults (5 jobs of 5 s in 60 s): one 1.5 s job does not;
#   7  VKBRIDGE_FAULT_ARM unset (the default since test59): nothing armed. Steps 1, 2, 5, 6 opt in with =1.
# Prints one line per step and "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
work=$(mktemp -d)
sock=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-hungarm-$$.sock
srv=
trap '[ -n "$srv" ] && kill $srv 2>/dev/null; rm -f "$sock"; [ -n "$KEEP" ] && echo "kept $work" || rm -rf "$work"' EXIT
fails=0 steps=0
step() { steps=$((steps+1)); if [ "$1" = 0 ]; then echo "  ok   $2"; else echo "  FAIL $2"; fails=$((fails+1)); fi; }
KEY=steam1245620
export VKBRIDGE_SOCKET=$sock VK_DRIVER_FILES=$out/vkbridge_icd.json VK_ICD_FILENAMES= VKBRIDGE_QUERY_MIRROR=0 VKBRIDGE_SYNC_MIRROR=0
unset VKBRIDGE_HIDE_EXTS

# start <name> <cache> KEY=VALUE... (the <cache dir>/env lines)
start() {
  local name=$1 cache=$work/$2; shift 2
  mkdir -p "$cache"
  printf '%s\n' "$@" > "$cache/env"
  rm -f "$sock"
  env -u VKBRIDGE_FAKE_MISSING -u VKBRIDGE_ERRATA -u VKBRIDGE_ERRATA_IMITATE -u VKBRIDGE_WAIT_FIX -u VKBRIDGE_HANG_DETECT \
      -u VKBRIDGE_FAULT_ARM -u VKBRIDGE_WAIT_FAKE_ERROR -u VKBRIDGE_WAIT_FAKE_RESULT -u VKBRIDGE_WAIT_RETRY_MS -u VKBRIDGE_WAIT_GRACE_S \
      -u VKBRIDGE_ERRATA_PROBE -u VKBRIDGE_HUNG_JOB_MS -u VKBRIDGE_HUNG_JOBS VKBRIDGE_PROBE=0 VKBRIDGE_REPORT_DIR="$work/r-$name" \
      VK_DRIVER_FILES=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json} VK_ICD_FILENAMES= \
      timeout 120 "$out/vkbridge-server" --socket "$sock" --log "$work/$name.log" --cache-dir "$cache" >/dev/null 2>&1 &
  srv=$!
  for _ in $(seq 50); do [ -S "$sock" ] && break; sleep 0.1; done
}
stop() { kill $srv 2>/dev/null; wait $srv 2>/dev/null; srv=; }
wc_run() { local name=$1; shift; VKBRIDGE_PCACHE_KEY=$KEY timeout 60 "$out/wait_check" "$@" >"$work/$name.out" 2>&1; }
armfile() { echo "$work/$1/hang-detect-next/wait_check@$KEY"; }
said() { grep -E "hang detector" "$work/$1.log" | sed 's/.*vkbridge-server . //' | head -2 | cut -c1-170 | tr '\n' '|'; }

# 1 an idle wait failed for ever (no deadline), past a 400 ms cap
start s1 cA VKBRIDGE_ERRATA_IMITATE=wait_retry VKBRIDGE_WAIT_FAKE_ERROR=all:idle VKBRIDGE_WAIT_RETRY_MS=400 VKBRIDGE_FAULT_ARM=1
wc_run s1 any queue-idle
stop
grep -q "vkQueueWaitIdle.*the app gets -3 (the driver's error: it kept failing)" "$work/s1.log" &&
  grep -q "wait_retry, device [0-9]*: the hang detector is armed for the next launch of wait_check@$KEY (that program's devices, one launch; VKBRIDGE_FAULT_ARM=0: never)" "$work/s1.log" &&
  [ "$(grep -c 'hang detector is armed' "$work/s1.log")" = 1 ] && [ -f "$(armfile cA)" ]
step $? "1 a wait with no deadline the driver kept failing arms the next launch: $(said s1) file $([ -f "$(armfile cA)" ] && echo there || echo missing)"

# 2 the next launch: on, says why, the file is taken
start s2 cA VKBRIDGE_FAULT_ARM=1
wc_run s2 ok sem-finite
stop
grep -q "hang detector on for wait_check@$KEY: its last launch had a GPU wait that never finished (vkQueueWaitIdle failed by the driver (-3) for 0.4 s, no deadline); this launch only" "$work/s2.log" &&
  grep -q "device [0-9]*: GPU hang detector on" "$work/s2.log" && [ ! -f "$(armfile cA)" ]
step $? "2 next launch: detector on, file taken: $(said s2)"

# 3 finite waits failed until their own deadline: the app's VK_TIMEOUT, not armed
start s3 cB VKBRIDGE_ERRATA_IMITATE=wait_retry VKBRIDGE_WAIT_FAKE_ERROR=all:fence+sem
wc_run s3 ok fence-finite sem-finite
stop
grep -q "the app gets 2 (VK_TIMEOUT, its deadline)" "$work/s3.log" && ! grep -q "hang detector is armed" "$work/s3.log" &&
  [ ! -e "$(armfile cB)" ]
step $? "3 finite waits ended at their deadline: not armed"

# 4 the off switch
start s4 cC VKBRIDGE_ERRATA_IMITATE=wait_retry VKBRIDGE_WAIT_FAKE_ERROR=all:idle VKBRIDGE_WAIT_RETRY_MS=400 VKBRIDGE_FAULT_ARM=0
wc_run s4 any queue-idle
stop
grep -q "vkQueueWaitIdle.*the app gets -3 (the driver's error: it kept failing)" "$work/s4.log" && ! grep -q "hang detector is armed" "$work/s4.log" &&
  [ ! -e "$(armfile cC)" ]
step $? "4 VKBRIDGE_FAULT_ARM=0: not armed"

# 5 a GPU job that took seconds and finished
start s5 cD VKBRIDGE_HUNG_JOB_MS=1000 VKBRIDGE_HUNG_JOBS=1 VKBRIDGE_FAULT_ARM=1
wc_run s5 ok sem-slow
r=$?
stop
[ $r = 0 ] && grep -q "wait_retry, device [0-9]*: the hang detector is armed for the next launch of wait_check@$KEY" "$work/s5.log" &&
  grep -Eq "^hung: 1 GPU job\(s\) of 1\.0 s or more in 60 s, the longest 1\.[0-9] s, the device not lost$" "$(armfile cD)"
step $? "5 a 1.5 s GPU job with the limit at 1 job of 1 s: armed ($(cat "$(armfile cD)" 2>/dev/null))"

# 6 the defaults: one 1.5 s job is not a hang
start s6 cE VKBRIDGE_FAULT_ARM=1
wc_run s6 ok sem-slow
r=$?
stop
[ $r = 0 ] && ! grep -q "hang detector is armed" "$work/s6.log" && [ ! -e "$(armfile cE)" ]
step $? "6 defaults (5 jobs of 5 s in 60 s): one 1.5 s job does not arm"

# 7 the default (test59, 11-14: the armed launch cost DMC5 ~10% fps on the Ultra): off unless VKBRIDGE_FAULT_ARM=1;
#   the same failed-for-ever wait as step 1 arms nothing
start s7 cF VKBRIDGE_ERRATA_IMITATE=wait_retry VKBRIDGE_WAIT_FAKE_ERROR=all:idle VKBRIDGE_WAIT_RETRY_MS=400
wc_run s7 any queue-idle
stop
grep -q "vkQueueWaitIdle.*the app gets -3 (the driver's error: it kept failing)" "$work/s7.log" && ! grep -q "hang detector is armed" "$work/s7.log" &&
  [ ! -e "$(armfile cF)" ]
step $? "7 default (no VKBRIDGE_FAULT_ARM): the step-1 wait arms nothing"

if [ $fails = 0 ]; then echo "OK: $steps of $steps steps"; else echo "FAILED: $fails of $steps steps"; exit 1; fi
