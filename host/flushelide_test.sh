#!/bin/bash
# Flushes and invalidates of coherent shared memory answered in the client (client/memory.c ranges_nosync,
# VKBRIDGE_FLUSH_ELIDE), with Wine's WoW64 OpenGL upload pattern (tests/flushelide_check.c: write, flush the written
# range, GPU copy, invalidate, check, 200 rounds), a server of its own per step imitating the Mali-G720's memory table
# (g720mem: the shared type is the driver's non-coherent 0xb, which the self-test finds coherent without flushes):
#   1  the switch unset (v0.5.5.4: default OFF, the old path; 0.5.6 has it on): 200 + 200 round trips, bytes right, no line;
#   2  VKBRIDGE_FLUSH_ELIDE=1: bytes right, every call VK_SUCCESS, 0 flush and 0 invalidate round trips, one
#      "flush elide:" line naming the coherent type;
#   3  VKBRIDGE_FLUSH_ELIDE=0: 200 + 200 round trips, no line;
#   4  the self-test made to find the shared type stale without flushes (VKBRIDGE_SELFTEST_STALE_AT=1): the client
#      offers it without HOST_COHERENT, and with the switch on its flushes and invalidates still all go to the server
#      (200 + 200), bytes right, no line;
#   5  cached readback (VKBRIDGE_MEMORY=dmabuf, VKBRIDGE_CACHED_READBACK=apps; RADV: dma-buf export and
#      DMA_BUF_IOCTL_SYNC): with the switch on every flush and invalidate still reaches the server and is a
#      DMA_BUF_IOCTL_SYNC there; SKIP on a driver without dma-buf export (lavapipe);
#   6  no protocol errors in any step's log.
# Prints one line per step and "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
work=$(mktemp -d)
sock=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-flushelide-$$.sock
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
      VKBRIDGE_PROBE_BUDGET_S=0 "$@" VK_DRIVER_FILES=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json} VK_ICD_FILENAMES= \
      "$out/vkbridge-server" --socket "$sock" --log "$log" --cache-dir "$d" >/dev/null 2>&1 &
  srv=$!
  for _ in $(seq 300); do [ -S "$sock" ] && break; kill -0 $srv 2>/dev/null || break; sleep 0.05; done
}
trap 'stop; rm -f "$sock"; [ -n "$KEEP" ] && echo "kept $work" || rm -rf "$work"' EXIT
export VKBRIDGE_SOCKET=$sock VK_DRIVER_FILES=$out/vkbridge_icd.json VK_ICD_FILENAMES=
unset VKBRIDGE_HIDE_EXTS VKBRIDGE_MAX_API VKBRIDGE_FAKE_MISSING VKBRIDGE_MODE VKBRIDGE_CACHED_READBACK VKBRIDGE_FLUSH_ELIDE
fails=0 steps=0
step() { steps=$((steps+1)); if [ "$1" = 0 ]; then echo "  ok   $2"; else echo "  FAIL $2"; fails=$((fails+1)); fi; }
# check <name> [check args] -- [client env...]: flushelide_check with the client's counts at exit, in $work/<name>.log
check() {
  local name=$1 arg=; shift
  [ "$1" != -- ] && { arg=$1; shift; }
  shift
  env -u VKBRIDGE_FLUSH_ELIDE VKBRIDGE_STATS=1 "$@" timeout 120 "$out/flushelide_check" $arg >"$work/$name.log" 2>&1
}
# rt <name> <command>: the client's sync count of that command (a command under 100 calls is not listed: 0 then)
rt() { local v; v=$(grep -E "^vkbridge error .*  $2 +[0-9]+ sync" "$work/$1.log" | awk '{for(i=1;i<NF;i++) if($(i+1)=="sync") print $i}' | tail -1); echo "${v:-0}"; }
okline() { tail -1 "$work/$1.log" | grep -q '^OK:'; }
said() { grep -c 'flush elide: ' "$work/$1.log"; }
desc() { echo "$(tail -1 "$work/$1.log" | cut -c1-60); flushes $(rt $1 vkFlushMappedMemoryRanges), invalidates $(rt $1 vkInvalidateMappedMemoryRanges) round trips"; }

# 1-3
start g720 VKBRIDGE_FAKE_MISSING=g720mem
check s1 --
okline s1 && [ "$(rt s1 vkFlushMappedMemoryRanges)" = 200 ] && [ "$(rt s1 vkInvalidateMappedMemoryRanges)" = 200 ] && [ "$(said s1)" = 0 ]
step $? "switch unset (default off in v0.5.5.4): $(desc s1)"
check s2 -- VKBRIDGE_FLUSH_ELIDE=1
okline s2 && [ "$(rt s2 vkFlushMappedMemoryRanges)" = 0 ] && [ "$(rt s2 vkInvalidateMappedMemoryRanges)" = 0 ] && [ "$(said s2)" = 1 ] &&
  grep -q 'flush elide: vkFlushMappedMemoryRanges of coherent shared memory answered in the client, no round trip (memory type [0-9]*, flags 0x[0-9a-f]*f;' "$work/s2.log"
step $? "VKBRIDGE_FLUSH_ELIDE=1: $(desc s2); $(grep -o 'memory type [0-9]*, flags 0x[0-9a-f]*' "$work/s2.log" | head -1)"
check s3 -- VKBRIDGE_FLUSH_ELIDE=0
okline s3 && [ "$(rt s3 vkFlushMappedMemoryRanges)" = 200 ] && [ "$(rt s3 vkInvalidateMappedMemoryRanges)" = 200 ] && [ "$(said s3)" = 0 ]
step $? "VKBRIDGE_FLUSH_ELIDE=0: $(desc s3)"

# 4 (the desktop driver's type 3 taken as not coherent and stale without flushes, as noflush_test does, the other shared
#    types coherent; lavapipe has one host-visible type, so that leaves no shared memory at all: SKIP there)
start stale VKBRIDGE_SELFTEST_FAKE_NONCOHERENT=0x8 VKBRIDGE_SELFTEST_STALE_AT=1
check s4 noncoherent -- VKBRIDGE_FLUSH_ELIDE=1
if grep -q 'FAIL a host-visible type without HOST_COHERENT' "$work/s4.log"; then
  echo "  SKIP shared type that needs flushes: none offered here ($(grep -o 'memory sharing summary for[^;]*;[^;]*;' "$log" | head -1 | cut -c1-120)); run on RADV"
else
  okline s4 && [ "$(rt s4 vkFlushMappedMemoryRanges)" = 200 ] && [ "$(rt s4 vkInvalidateMappedMemoryRanges)" = 200 ] && [ "$(said s4)" = 0 ] &&
    grep -q 'shared OK, needs flushes' "$log"
  step $? "shared type that needs flushes, switch on: $(desc s4); $(grep -o 'type [0-9]* (flags 0x[0-9a-f]*)[^;]*needs flushes' "$log" | head -1)"
fi

# 5
start cached VKBRIDGE_MEMORY=dmabuf VKBRIDGE_CACHED_READBACK=apps
check s5 noncoherent -- VKBRIDGE_FLUSH_ELIDE=1
sleep 0.3
if grep -q 'FAIL a host-visible type without HOST_COHERENT' "$work/s5.log"; then
  echo "  SKIP cached readback: no cached type offered here ($(grep -o 'memory sharing summary for[^;]*;' "$log" | head -1 | cut -c1-120)); run on RADV (cached_test too)"
else
  syncs=$(grep -o 'cached readback: [0-9]* flushes so far' "$log" | tail -1 | grep -o '[0-9]*')
  okline s5 && [ "$(rt s5 vkFlushMappedMemoryRanges)" = 200 ] && [ "$(rt s5 vkInvalidateMappedMemoryRanges)" = 200 ] && [ "$(said s5)" = 0 ] &&
    [ "${syncs:-0}" -ge 128 ] && grep -q 'shared (dmabuf, cached)' "$log"
  step $? "cached readback, switch on: $(desc s5); DMA_BUF_IOCTL_SYNC flushes logged up to ${syncs:-0}"
fi
stop

# 6
bad=$(cat $logs | grep -ci 'bad message\|protocol error')
[ "$bad" = 0 ]
step $? "no protocol errors in the servers' logs ($bad)"

if [ $fails = 0 ]; then echo "OK: $steps steps"; else echo "FAILED: $fails of $steps steps"; fi
[ $fails = 0 ]
