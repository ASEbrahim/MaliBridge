#!/bin/bash
# The memory self-test's no-flush claim (server_memory.c test_type): a type the driver does not call
# HOST_COHERENT is first tried without flushes, and accepted as "coherent in practice" only if it passes
# NOFLUSH_PASSES times in a row (5 for a HOST_CACHED type), each pass on a fresh allocation with other data
# (a stale cache line left by the pass before holds the same pattern otherwise); one failure sends the type
# to the flushed test. On the desktop driver, whose memory is coherent, the host imitations
# VKBRIDGE_SELFTEST_FAKE_NONCOHERENT=<type mask> (those types count as not HOST_COHERENT) and
# VKBRIDGE_SELFTEST_STALE_AT=k (the k-th no-flush pass of a type reads stale) show the path. The host
# driver's types 3 (flags 0x7, uncached) and 5 (0xe, cached) are used. The server starts a self-test per GPU;
# the first GPU's lines are read.
#   1  as it is: no type is tried without flushes, every host-visible type coherent;
#   2  types 3 and 5 not coherent: 3 passes for type 3, 5 for type 5, both "coherent in practice", and still in
#      the coherent mask;
#   3  the same with the 2nd pass stale: both go to flushes ("needs flushes"), the log says how many passes
#      passed, the coherent mask loses them (a single pass would have accepted them);
#   4  the 1st pass stale: flushes, nothing passed (no "passed N of" line);
#   5  the 4th pass stale: type 3 (3 passes) is unaffected, type 5 (5 passes) fails after 3.
#   Steps 2-5 run under PowerVR's identity (VKBRIDGE_FAKE_DRIVER=img): the repeated passes are PowerVR's only.
#   7  not PowerVR (Mali's way), types 3 and 5 not coherent: one pass each, test49's line word for word, accepted.
# Prints one line per step and "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
work=$(mktemp -d)
cache=$work/c
mkdir -p "$cache"
sock=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-noflush-$$.sock
log=$work/server.log
srv=
start() { # start [env...]: a server of its own on a fresh cache directory (a fresh probe profile)
  [ -n "$srv" ] && kill $srv 2>/dev/null && wait $srv 2>/dev/null
  rm -f "$sock"
  rm -rf "$cache"; mkdir -p "$cache"
  : > "$log"
  env -u VKBRIDGE_FAKE_MISSING -u VKBRIDGE_HIDE_EXTS -u VKBRIDGE_ERRATA -u VKBRIDGE_ERRATA_IMITATE -u VKBRIDGE_MODE \
      -u VKBRIDGE_FAKE_DRIVER_VERSION -u VKBRIDGE_ERRATA_PROBE -u VKBRIDGE_PROBE_FAKE \
      VKBRIDGE_PROBE_BUDGET_S=0 "$@" VK_DRIVER_FILES=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json} VK_ICD_FILENAMES= \
      "$out/vkbridge-server" --socket "$sock" --log "$log" --cache-dir "$cache" >/dev/null 2>&1 &
  srv=$!
  for _ in $(seq 100); do [ -S "$sock" ] && break; sleep 0.1; done
}
trap 'kill $srv 2>/dev/null; rm -f "$sock"; [ -n "$KEEP" ] || rm -rf "$work"' EXIT
export VKBRIDGE_SOCKET=$sock VK_DRIVER_FILES=$out/vkbridge_icd.json VK_ICD_FILENAMES=
unset VKBRIDGE_HIDE_EXTS VKBRIDGE_MAX_API
fails=0 steps=0
step() { [ -n "$DBG" ] && [ "$1" != 0 ] && echo "$new" | grep -i "pv_mutable\|errata: device" | cut -c1-250; steps=$((steps+1)); if [ "$1" = 0 ]; then echo "  ok   $2"; else echo "  FAIL $2"; fails=$((fails+1)); fi; }
run() { # run <prog> [args] -- env lines: $new the log lines it added, $res its last output line, $rc its status
  local prog=("$1"); shift
  while [ "$1" != -- ]; do prog+=("$1"); shift; done; shift
  printf '%s\n' "$@" > "$cache/env"
  local l; l=$(wc -l < "$log")
  timeout 120 "${prog[@]}" >$work/run.log 2>&1
  rc=$?
  sleep 0.2
  res=$(tail -1 $work/run.log)
  new=$(tail -n +$((l+1)) "$log")
}
# the first GPU's self-test lines: from its "memory sharing summary" back to the one before
st() { # st [env...]: a fresh server; $first = the log up to the first GPU's summary line
  start "$@"
  first=$(sed -n '1,/memory sharing summary for/p' "$log")
}
coh_mask() { echo "$first" | grep -o 'dmabuf: chosen, types 0x[0-9a-f]* shared, 0x[0-9a-f]* coherent' | head -1 | sed 's/.*shared, //; s/ coherent//'; }
line() { echo "$first" | grep -F "dmabuf, type $1" | head -3; }

# 1
st
! echo "$first" | grep -q "coherent in practice\|no-flush pass" && [ "$(coh_mask)" = 0x72c ]
step $? "as it is: nothing tried without flushes, coherent types $(coh_mask)"
# 2
st VKBRIDGE_FAKE_DRIVER=img VKBRIDGE_SELFTEST_FAKE_NONCOHERENT=0x28
echo "$first" | grep -q "dmabuf, type 3: coherent in practice (no flushes needed; 3 passes)" &&
  echo "$first" | grep -q "dmabuf, type 5: coherent in practice (no flushes needed; 5 passes)" && [ "$(coh_mask)" = 0x72c ]
step $? "types 3 and 5 not coherent: 3 and 5 passes, accepted ($(coh_mask))"
# 3
st VKBRIDGE_FAKE_DRIVER=img VKBRIDGE_SELFTEST_FAKE_NONCOHERENT=0x28 VKBRIDGE_SELFTEST_STALE_AT=2
echo "$first" | grep -q "dmabuf, type 3: pass 2 read stale data" && echo "$first" | grep -q "dmabuf, type 3: passed 1 of 3 no-flush passes, then failed: flushes are used for it" &&
  echo "$first" | grep -q "dmabuf, type 5: passed 1 of 5 no-flush passes, then failed" &&
  echo "$first" | grep -q "dmabuf, type 3 (flags 0x7): shared OK, needs flushes" && echo "$first" | grep -q "dmabuf, type 5 (flags 0xe): shared OK, needs flushes" &&
  [ "$(coh_mask)" = 0x704 ]
step $? "2nd pass stale: flushes for both, coherent types $(coh_mask) (one pass would have accepted them)"
# 4
st VKBRIDGE_FAKE_DRIVER=img VKBRIDGE_SELFTEST_FAKE_NONCOHERENT=0x28 VKBRIDGE_SELFTEST_STALE_AT=1
echo "$first" | grep -q "dmabuf, type 3: pass 1 read stale data" && ! echo "$first" | grep -q "no-flush passes, then failed" &&
  echo "$first" | grep -q "dmabuf, type 3 (flags 0x7): shared OK, needs flushes" && [ "$(coh_mask)" = 0x704 ]
step $? "1st pass stale: flushes, nothing passed first ($(coh_mask))"
# 5
st VKBRIDGE_FAKE_DRIVER=img VKBRIDGE_SELFTEST_FAKE_NONCOHERENT=0x28 VKBRIDGE_SELFTEST_STALE_AT=4
echo "$first" | grep -q "dmabuf, type 3: coherent in practice (no flushes needed; 3 passes)" &&
  echo "$first" | grep -q "dmabuf, type 5: passed 3 of 5 no-flush passes, then failed" && [ "$(coh_mask)" = 0x70c ]
step $? "4th pass stale: type 3 (3 passes) accepted, type 5 (5 passes) fails after 3 ($(coh_mask))"
# 7
st VKBRIDGE_SELFTEST_FAKE_NONCOHERENT=0x28 VKBRIDGE_SELFTEST_STALE_AT=2
echo "$first" | grep -q "dmabuf, type 3: coherent in practice (no flushes needed)$" && echo "$first" | grep -q "dmabuf, type 5: coherent in practice (no flushes needed)$" &&
  ! echo "$first" | grep -q "no-flush passes\|read stale" && [ "$(coh_mask)" = 0x72c ]
step $? "not PowerVR: one no-flush pass as in test49 (the 2nd-pass staleness never reached), accepted ($(coh_mask))"

if [ $fails = 0 ]; then echo "OK: $steps steps"; else echo "FAILED: $fails of $steps steps"; exit 1; fi
