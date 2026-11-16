#!/bin/bash
# Query reads that reach the server while another thread keeps the async queue full (tests/querypoll_check, the BL2 / Tekken shape:
# DXVK's getData polls on the render thread beside dxvk-cs's request stream). VKBRIDGE_QUERY_MIRROR=0, so every read is a wire read.
# The stream is paced at 400k requests a second (rate=<n>; RE3's dxvk-cs sent ~166k): an unpaced one (rate=0, ~12-16M a second here)
# outruns the server for good, and then the old way's reads wait ~30 ms each (RADV) and the run never ends.
#   1  VKBRIDGE_QUERY_NOBARRIER=0 (the old way: the whole queue written first, the server's order wait behind every queued request):
#      all correctness checks pass; its median read time is the reference
#   2  the default: non-WAIT reads go without the flush and the order wait: correctness checks pass, and the p90 read is at most
#      a third of step 1's p90 and under 300 us (p90, not the median: on RADV the old way's median swings 12-34 us run to run
#      while its p90 stays 62-70 us; judged by the median, 2 of 4 green runs failed on 2026-11-12)
#   3  VKBRIDGE_TEST_QUERY_NOGATE=1 (test hook: the host-reset check left out): reads right after vkResetQueryPool say "available"
#      (stale) - the check can see a missing gate
#   4  the GPU-reset pool keeps the barrier, said once in the client log
# Step 0 runs the check natively (no bridge) with a second thread reading the query nonstop: lavapipe itself then answers
# "available" with 0, or "not ready" after "ready", or never ready in 2 s (its own reads under concurrency, 2026-11-10: width 92,
# never 95 natively; RADV 0/0). On such a driver the 32-bit/availability/partial and "became ready" checks say nothing about the
# bridge: they are reported, not judged; stale-after-reset, WAIT and the GPU-reset pool are judged everywhere.
# One line per step, "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
icd=${VKB_CLIENT_ICD:-$out/vkbridge_icd.json}
work=$(mktemp -d "${XDG_RUNTIME_DIR:-/tmp}/vkb-qp.XXXX")
sock=$work/s.sock
srv=
fails=0
stop() { [ -n "$srv" ] && kill $srv 2>/dev/null && wait $srv 2>/dev/null; srv=; }
trap 'stop; [ -n "${VKB_KEEP:-}" ] && echo "kept $work" || rm -rf "$work"' EXIT
mkdir -p "$work/c"
run() {
  local name=$1; shift
  rm -f "$sock"
  env -u VKBRIDGE_FAKE_MISSING -u VKBRIDGE_HIDE_EXTS -u VKBRIDGE_MODE VKBRIDGE_PROBE_BUDGET_S=0 \
      VK_DRIVER_FILES=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json} VK_ICD_FILENAMES= \
      "$out/vkbridge-server" --socket "$sock" --log "$work/$name.server.log" --cache-dir "$work/c" >/dev/null 2>&1 &
  srv=$!
  for _ in $(seq 300); do [ -S "$sock" ] && break; sleep 0.05; done
  env "$@" VKBRIDGE_QUERY_MIRROR=0 VKBRIDGE_SOCKET="$sock" VK_DRIVER_FILES="$icd" VK_ICD_FILENAMES= VKBRIDGE_CLIENT_LOG="$work/$name.client.log" \
      VKBRIDGE_LOG_LEVEL=2 timeout 120 "$out/querypoll_check" ${rounds:-600} ${rate:-400000} >"$work/$name.out" 2>&1
  res=$?
  stop
  line=$(grep "^RESULT:" "$work/$name.out")
  med=$(echo "$line" | grep -o "median_us=[0-9.]*" | cut -d= -f2)
  p90=$(echo "$line" | grep -o "p90_us=[0-9.]*" | cut -d= -f2)
  stale=$(echo "$line" | grep -o "stale=[0-9]*" | cut -d= -f2)
  nfail=$(grep -c "^FAIL" "$work/$name.out")
}
# the bridge's own checks (those a driver's concurrent-read faults cannot fail)
bridge_fails() { grep -cE "^FAIL (host reset then read|WAIT read|GPU-reset pool)" "$work/$1.out"; }
step() { if [ "$1" = 0 ]; then echo "ok   $2"; else echo "FAIL $2"; fails=$((fails+1)); fi; }
show() { grep -E "^(FAIL|RESULT|flood)" "$work/$1.out" | cut -c1-200; }

VK_DRIVER_FILES=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json} VK_ICD_FILENAMES= timeout 200 "$out/querypoll_check" ${rounds:-600} ${rate:-400000} 1 >"$work/native.out" 2>&1
drvbad=$(grep -cE "^FAIL (32-bit|every query|hammer)" "$work/native.out")
echo "info 0 the driver alone (no bridge, a 2nd reader thread): $(grep -o "width_bad=[0-9]* gpu_bad=[0-9]* never=[0-9]*" "$work/native.out" | head -1); $([ "$drvbad" = 0 ] && echo "all checks judged" || echo "its concurrent reads fail by themselves: width/ready checks reported, not judged")"
jfail() { if [ "$drvbad" = 0 ]; then echo "$nfail"; else bridge_fails "$1"; fi; }

run old VKBRIDGE_QUERY_NOBARRIER=0
nj=$(jfail old)
[ $res = 0 ] && [ -n "$line" ] && [ "$nj" = 0 ]; st=$?
mold=${med:-0}; p90old=${p90:-0}
step $st "1 the old way (flush + order wait): read median ${med:-?} us, p90 ${p90:-?} us, $nfail check(s) failed ($nj judged)"
[ $st = 0 ] || show old

run def
nj=$(jfail def)
[ $res = 0 ] && [ -n "$line" ] && [ "$nj" = 0 ] && awk -v n="${p90:-1e9}" -v o="$p90old" 'BEGIN {exit !(n <= o / 3 && n < 300)}'; st=$?
step $st "2 the default: read p90 ${p90:-?} us (old ${p90old} us; at most a third and < 300 us), median ${med:-?} us (old ${mold} us), $nfail check(s) failed ($nj judged)"
[ $st = 0 ] || show def
grep -q "keep the barrier: it is reset on the GPU" "$work/def.client.log" 2>/dev/null; st4=$?

run nogate VKBRIDGE_TEST_QUERY_NOGATE=1
[ "${stale:-0}" -gt 0 ]; st=$?
step $st "3 the host-reset check left out (test hook): ${stale:-?} stale reads (must be > 0: the check sees it)"

step $st4 "4 the GPU-reset pool keeps the barrier (client log line)"

if [ $fails = 0 ]; then echo "OK: non-WAIT query reads without the queue flush and order wait, never stale after a host reset; WAIT and GPU-reset reads as before"
else echo "FAILED: $fails step(s)"; exit 1; fi
