#!/bin/bash
# The Xclipse 530 (Galaxy A55, Samsung driver, 2026-10-26 logs): no VK_EXT_external_memory_host, and
# buffers made with the dma-buf or AHB handle type allow no host-visible type, so every sharing
# method failed and smoke passed 6 of 13. Imitated on the desktop driver with
# VKBRIDGE_FAKE_MISSING=hostptr,extbufhv, a server of its own per step:
#   1  the fallback off (VKBRIDGE_UNFLAGGED_BUFFERS=0): no method works, smoke 6 of 13 (the player's);
#      the test buffer's masks logged (create result, host-visible types none);
#   2  the fallback on (default): a dma-buf method chosen with unflagged buffers, smoke 13 of 13;
#   3  nothing faked: the choice as before, no unflagged buffers;
#   4  as 1 with both settings in <cache dir>/env instead of the environment (the tablet's way: the
#      kill switch was read with getenv() only in v0.5.4.1 rc1, so it did nothing there).
# Prints one line per step and "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
work=$(mktemp -d)
sock=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-xclipse-$$.sock
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
  [ -n "${ENVFILE:-}" ] && printf '%s\n' $ENVFILE > "$d/c/env" # <cache dir>/env, as on a tablet
  log=$d/vkbridge-server.log
  : > "$log"
  env -u VKBRIDGE_FAKE_MISSING -u VKBRIDGE_HIDE_EXTS -u VKBRIDGE_MODE -u VKBRIDGE_MEMORY -u VKBRIDGE_UNFLAGGED_BUFFERS \
      VKBRIDGE_PROBE_BUDGET_S=0 "$@" VK_DRIVER_FILES=$host_icd VK_ICD_FILENAMES= \
      "$out/vkbridge-server" --socket "$sock" --log "$log" --cache-dir "$d/c" >/dev/null 2>&1 &
  srv=$!
  for _ in $(seq 300); do [ -S "$sock" ] && break; kill -0 $srv 2>/dev/null || break; sleep 0.05; done
}
smoke() { timeout 120 "$out/vkbridge-smoke" >"$work/smoke-$1.log" 2>&1; tail -1 "$work/smoke-$1.log"; }
summary() { grep -m1 -o 'memory sharing summary for .*' "$log"; }
trap 'stop; rm -f "$sock"; [ -n "$KEEP" ] && echo "kept $work" || rm -rf "$work"' EXIT
export VKBRIDGE_SOCKET=$sock VK_DRIVER_FILES=$out/vkbridge_icd.json VK_ICD_FILENAMES=
unset VKBRIDGE_HIDE_EXTS VKBRIDGE_MAX_API VKBRIDGE_FAKE_MISSING VKBRIDGE_MODE
fails=0 steps=0
step() { steps=$((steps+1)); if [ "$1" = 0 ]; then echo "  ok   $2"; else echo "  FAIL $2"; fails=$((fails+1)); fi; }

# 1
start off VKBRIDGE_FAKE_MISSING=hostptr,extbufhv VKBRIDGE_UNFLAGGED_BUFFERS=0
s1=$(smoke off)
grep -q "memory sharing: no method works" "$log" && grep -q "6 of 13 checks passed" "$work/smoke-off.log" &&
  grep -q "memory sharing summary for .*hostptr: no extensions" "$log" &&
  grep -q "memory sharing: dmabuf test buffer (usage 0x[0-9a-f]*) with handle types 0x200: create 0, types 0x[0-9a-f]* (host-visible 0x0); without handle types 0x[0-9a-f]* (host-visible 0x[1-9a-f]" "$log" &&
  grep -q "dmaheap: no fd properties\|dmaheap: a dma-heap fd imports into" "$log" && ! grep -q "(unflagged" "$log"
step $? "hostptr,extbufhv with the fallback off: no method works, as the Xclipse 530 ($s1; $(summary))"

# 2
start on VKBRIDGE_FAKE_MISSING=hostptr,extbufhv
s2=$(smoke on)
grep -q "13 of 13 checks passed" "$work/smoke-on.log" && grep -q "memory sharing summary for .*dma[a-z]*: chosen (unflagged buffers)" "$log" &&
  grep -q "with buffers made without its handle type (outside the Vulkan spec" "$log" &&
  grep -q "memory sharing: dma[a-z]* test buffer (usage 0x[0-9a-f]*) with handle types 0x0 (unflagged): create 0" "$log" &&
  ! grep -q "memory sharing: no method works" "$log"
step $? "hostptr,extbufhv: a dma-buf method with unflagged buffers ($s2; $(summary))"

# 3
start plain
s3=$(smoke plain)
grep -q "13 of 13 checks passed" "$work/smoke-plain.log" && grep -q "memory sharing summary for .*: chosen, types" "$log" &&
  ! grep -q "(unflagged" "$log"
step $? "nothing faked: the usual choice, no unflagged buffers ($s3; $(summary))"

# 4
ENVFILE="VKBRIDGE_FAKE_MISSING=hostptr,extbufhv VKBRIDGE_UNFLAGGED_BUFFERS=0" start envfile
s4=$(smoke envfile)
grep -q "override from .*/env: VKBRIDGE_UNFLAGGED_BUFFERS=0" "$log" && grep -q "memory sharing: no method works" "$log" &&
  grep -q "6 of 13 checks passed" "$work/smoke-envfile.log" && ! grep -q "(unflagged" "$log"
step $? "the same from <cache dir>/env: the fallback stays off ($s4; $(summary))"
stop

if [ $fails = 0 ]; then echo "OK: $steps steps"; else echo "FAILED: $fails of $steps steps"; exit 1; fi
