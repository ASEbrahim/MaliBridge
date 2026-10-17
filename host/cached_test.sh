#!/bin/bash
# Cached readback (server_memory.c, VKBRIDGE_CACHED_READBACK) on the desktop driver, with a server
# of its own per step. The host has no dma-heap a user may open, so the dma-buf strategy stands in
# (VKBRIDGE_MEMORY=dmabuf: RADV exports dma-bufs, and DMA_BUF_IOCTL_SYNC works on them):
#   1  no switch, safe mode: the self-test passes cached pages in the driver's HOST_CACHED types
#      (and says so), but nothing is offered: no app type, the frame readback uncached;
#   2  performance mode: the client's frame readback (vkcube's X11 software path) reads through
#      cached pages, invalidated before every frame; still no type for apps;
#   3  VKBRIDGE_CACHED_READBACK=apps: a HOST_CACHED type without HOST_COHERENT for apps;
#      cached_check's flush / GPU write / invalidate rounds read back right, and every flush and
#      invalidate is a DMA_BUF_IOCTL_SYNC;
#   4  VKBRIDGE_CACHED_READBACK=0: no self-test of cached pages, nothing offered in performance
#      mode either;
#   5  the guest's VKBRIDGE_CACHED_READBACK=0: the frame readback stays uncached;
#   6  the hostptr strategy (no dma-buf to sync through): no cached readback;
#   7  no protocol errors in any step's log.
# Prints one line per step and "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
work=$(mktemp -d)
sock=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-cached-$$.sock
srv=
log=
logs=
stop() { [ -n "$srv" ] && kill $srv 2>/dev/null && wait $srv 2>/dev/null; srv=; }
# start <name> [env...]: a server of its own (cache $work/<name>), log $log
start() {
  local d=$work/$1
  shift
  stop
  rm -f "$sock"
  mkdir -p "$d"
  log=$d/server.log
  logs="$logs $log"
  : > "$log"
  env -u VKBRIDGE_FAKE_MISSING -u VKBRIDGE_HIDE_EXTS -u VKBRIDGE_MODE -u VKBRIDGE_CACHED_READBACK -u VKBRIDGE_MEMORY \
      VKBRIDGE_PROBE_BUDGET_S=0 VKBRIDGE_MEMORY=dmabuf "$@" VK_DRIVER_FILES=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json} VK_ICD_FILENAMES= \
      "$out/vkbridge-server" --socket "$sock" --log "$log" --cache-dir "$d" >/dev/null 2>&1 &
  srv=$!
  for _ in $(seq 300); do [ -S "$sock" ] && break; kill -0 $srv 2>/dev/null || break; sleep 0.05; done
}
trap 'stop; rm -f "$sock"; [ -n "$KEEP" ] && echo "kept $work" || rm -rf "$work"' EXIT
export VKBRIDGE_SOCKET=$sock VK_DRIVER_FILES=$out/vkbridge_icd.json VK_ICD_FILENAMES=
unset VKBRIDGE_HIDE_EXTS VKBRIDGE_MAX_API VKBRIDGE_FAKE_MISSING VKBRIDGE_MODE VKBRIDGE_CACHED_READBACK
fails=0 steps=0
step() { steps=$((steps+1)); if [ "$1" = 0 ]; then echo "  ok   $2"; else echo "  FAIL $2"; fails=$((fails+1)); fi; }
frames() { timeout 60 vkcube --c 60 >"$work/vkcube.log" 2>&1; }
invalidates() { grep -o 'cached readback: [0-9]* invalidates so far' "$log" | tail -1 | grep -o '[0-9]*' || echo 0; }
flushes() { grep -o 'cached readback: [0-9]* flushes so far' "$log" | tail -1 | grep -o '[0-9]*' || echo 0; }
# the self-test's own syncs (one flush and one invalidate per type it passes) are not the client's
selftest_syncs() { grep -c '(cached readback): shared OK' "$log"; }

# 1
start safe
passed=$(grep -m1 -o 'cached readback: dmabuf, types 0x[0-9a-f]* pass with cached pages' "$log" | grep -o '0x[0-9a-f]*')
timeout 60 "$out/cached_check" none >"$work/c1.log" 2>&1; c=$?
frames; f=$?
[ -n "$passed" ] && [ "$passed" != 0x0 ] && [ $c = 0 ] && [ $f = 0 ] && ! grep -q 'cached readback: offered' "$log" &&
  [ "$(invalidates)" -le "$(selftest_syncs)" ] && ! grep -q 'shared (dmabuf, cached)' "$log"
step $? "safe mode, no switch: self-test passes types $passed, nothing offered ($(tail -1 "$work/c1.log" | cut -c1-40))"

# 2
start perf VKBRIDGE_MODE=performance
timeout 60 "$out/cached_check" none >"$work/c2.log" 2>&1; c=$?
frames; f=$?
n=$(invalidates)
[ $c = 0 ] && [ $f = 0 ] && grep -q 'cached readback: offered in types 0x[0-9a-f]* to the client.s frame readback (performance mode)' "$log" &&
  grep -q 'shared (dmabuf, cached)' "$log" && [ "$n" -ge 32 ]
step $? "performance mode: frame readback cached ($n invalidates so far), no app type"

# 3
start apps VKBRIDGE_CACHED_READBACK=apps
timeout 120 "$out/cached_check" expect >"$work/c3.log" 2>&1; c=$?
nf=$(flushes) ni=$(invalidates)
[ $c = 0 ] && grep -q 'offered in types 0x[0-9a-f]* to the client.s frame readback and apps (VKBRIDGE_CACHED_READBACK)' "$log" &&
  [ "$nf" -ge 32 ] && [ "$ni" -ge 32 ] && ! grep -q 'DMA_BUF_IOCTL_SYNC (.*) failed' "$log"
step $? "VKBRIDGE_CACHED_READBACK=apps: $(grep -o 'PASS CPU writes.*' "$work/c3.log" | cut -c6-40)...; syncs $nf flushes, $ni invalidates ($(grep -o 'read speed: .*' "$work/c3.log"))"

# 4
start off VKBRIDGE_MODE=performance VKBRIDGE_CACHED_READBACK=0
timeout 60 "$out/cached_check" none >"$work/c4.log" 2>&1; c=$?
frames; f=$?
[ $c = 0 ] && [ $f = 0 ] && ! grep -q 'pass with cached pages' "$log" && ! grep -q 'cached readback: offered' "$log" &&
  ! grep -q 'shared (dmabuf, cached)' "$log" && [ "$(invalidates)" = 0 ]
step $? "VKBRIDGE_CACHED_READBACK=0: no cached self-test, nothing offered in performance mode"

# 5
start guest VKBRIDGE_MODE=performance
VKBRIDGE_CACHED_READBACK=0 frames; f=$?
[ $f = 0 ] && grep -q 'cached readback: offered' "$log" && ! grep -q 'shared (dmabuf, cached)' "$log" &&
  [ "$(invalidates)" -le "$(selftest_syncs)" ]
step $? "the guest's VKBRIDGE_CACHED_READBACK=0: frame readback uncached"

# 6
start hostptr VKBRIDGE_MODE=performance VKBRIDGE_MEMORY=hostptr
grep -q 'cached readback: not with hostptr\|memory sharing: hostptr failed the self-test' "$log" && ! grep -q 'pass with cached pages' "$log"
step $? "hostptr strategy: no cached readback ($(grep -m1 -o 'cached readback: not with hostptr\|memory sharing: hostptr failed the self-test' "$log"))"

# 7
stop
bad=$(cat $logs | grep -cE "command stream:|malformed request|unknown command id|the driver has no|bad batch entry")
[ "${bad:-0}" = 0 ]
step $? "no protocol errors"

if [ $fails = 0 ]; then echo "OK: $steps steps"; else echo "FAILED: $fails of $steps steps"; exit 1; fi
