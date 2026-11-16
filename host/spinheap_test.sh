#!/bin/bash
# What the server itself keeps of each compile the watchdog interrupts (server_guard.c): the S10+ (G720 r44p1, DS3's
# pre-caching, 11-11) kept ~3 MB a compile after 145 interrupts (+443 MB once every device was gone). Here a
# compile spins in the server's own test hook (VKBRIDGE_TEST_CRASH_PIPELINE=spin: no driver memory is involved), so
# what the heap keeps is ours alone: a warm-up of 5 interrupted compiles (the stuck-pipelines file, its lists), then
# SPIN_N (40) more, each a pipeline of its own, then the heap once their device is gone.
# SRV_OUT=<dir>: the server and client ICD to test. Prints the numbers and "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
srv_out=${SRV_OUT:-$out}
LVP=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/lvp_icd.json}
N=${SPIN_N:-40}
work=$(mktemp -d)
sock=$work/s.sock
log=$work/server.log
unset WAYLAND_DISPLAY DISPLAY SteamAppId
VKBRIDGE_TEST_CRASH_PIPELINE=spin VKBRIDGE_COMPILE_WARN_S=1 VKBRIDGE_COMPILE_TIMEOUT_S=1 VKBRIDGE_PROBE=0 VK_DRIVER_FILES=$LVP VK_ICD_FILENAMES=$LVP \
  "$srv_out/vkbridge-server" --socket "$sock" --log "$log" --cache-dir "$work/c" >$work/srv.out 2>&1 &
srv=$!
trap 'kill $srv 2>/dev/null; if [ -n "$KEEP" ]; then echo "kept $work"; else rm -rf "$work"; fi' EXIT
for _ in $(seq 100); do [ -S "$sock" ] && break; sleep 0.1; done
cl() { VKBRIDGE_SOCKET=$sock VK_DRIVER_FILES=$srv_out/vkbridge_icd.json VK_ICD_FILENAMES= timeout 600 "$out/pcache_check" "$@"; }
heap() { grep -o "(anon [0-9]*M" "$log" | tail -1 | grep -o "[0-9]*"; }
# the heap in KB right now: the server's mallinfo is in its memory lines (MB); read it as KB from /proc for finer steps
kb() { awk '/^(Private_Dirty|Private_Clean):/ {s+=$2} END {print s+0}' /proc/$srv/smaps_rollup 2>/dev/null; }
cl --first 0 --count 5 --mark >$work/w.out 2>&1
sleep 6
h0=$(heap); k0=$(kb); t0=$(grep -c 'TIMED OUT' "$log")
cl --first 1000 --count $N --mark >$work/m.out 2>&1
sleep 6
h1=$(heap); k1=$(kb); t1=$(grep -c 'TIMED OUT' "$log")
cl --first 2000 --count $N --mark >$work/m2.out 2>&1
sleep 6
h2=$(heap); k2=$(kb); t2=$(grep -c 'TIMED OUT' "$log")
n=$((t1 - t0)); n2=$((t2 - t1))
per=$(( n > 0 ? (k1 - k0) / n : 0 )); per2=$(( n2 > 0 ? (k2 - k1) / n2 : 0 ))
echo "  $n compiles interrupted (warm-up $t0): anon ${h0}M -> ${h1}M; private memory ${k0} KB -> ${k1} KB (${per} KB a compile, the S10+'s ~3000 KB); $n2 more: -> ${k2} KB (${per2} KB a compile)"
per=$(( (n + n2) > 0 ? (k2 - k0) / (n + n2) : 0 )) # over both: what stays (a first rise that comes back is not kept)
if [ "$n" -ge "$N" ] && [ "$n2" -ge "$N" ] && [ "$per" -le 64 ]; then echo "OK: the server keeps ${per} KB of an interrupted compile"; else echo "FAILED: ${per} KB kept a compile ($n interrupted)"; exit 1; fi
