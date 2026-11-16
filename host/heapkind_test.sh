#!/bin/bash
# The server's memory by kind of object (server/server_heapkind.c): Motor Town's ~2 GB
# server heap (player logs) and RE3's PowerVR layouts had no owner on any line.
# tests/heapkind_check makes a known set of objects; VKBRIDGE_TEST_HEAPKIND_GROW=layout:24 makes each pipeline
# layout create keep 24 MB in the server (as PowerVR's driver keeps ~305 MB per layout). A server of its own per step:
#   1  plenty of memory: a "memkind:" line with the live counts (shader 4, layout 3, setlayout 1, pipeline 3,
#      descpool 2, pcache 1, memory 1) and the layouts' growth (>= 100 MB of the 5 x 24), no "memtop:" line;
#   2  told 500 MB are free of 8000: after "memory low", a "memtop:" line whose first owner is the layouts of
#      heapkind_check (its pid), count 3, >= 100 MB;
#   3  VKBRIDGE_HEAPKIND=0: no memkind/memtop line (memory low still said).
# Prints one line per step and "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
work=$(mktemp -d)
sock=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-heapkind-$$.sock
srv=
stop() { [ -n "$srv" ] && kill $srv 2>/dev/null && wait $srv 2>/dev/null; srv=; }
trap 'stop; rm -f "$sock"; [ -n "$KEEP" ] && echo "kept $work" || rm -rf "$work"' EXIT
start() { # start <name> [env...]
  local d=$work/$1
  shift
  stop
  rm -f "$sock"
  mkdir -p "$d/c"
  log=$d/server.log
  env -u VKBRIDGE_FAKE_MISSING -u VKBRIDGE_HIDE_EXTS -u VKBRIDGE_MODE -u VKBRIDGE_MEM_STATS -u VKBRIDGE_HEAPKIND "$@" \
      VKBRIDGE_PROBE=0 VKBRIDGE_MEMKIND_S=5 VKBRIDGE_TEST_HEAPKIND_GROW=layout:24 \
      VK_DRIVER_FILES=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json} VK_ICD_FILENAMES= \
      "$out/vkbridge-server" --socket "$sock" --log "$log" --cache-dir "$d/c" >/dev/null 2>&1 &
  srv=$!
  for _ in $(seq 100); do [ -S "$sock" ] && break; kill -0 $srv 2>/dev/null || break; sleep 0.1; done
}
export VKBRIDGE_SOCKET=$sock VK_DRIVER_FILES=$out/vkbridge_icd.json VK_ICD_FILENAMES=
unset VKBRIDGE_HIDE_EXTS VKBRIDGE_MAX_API VKBRIDGE_SYNC
fails=0 steps=0
step() { steps=$((steps+1)); if [ "$1" = 0 ]; then echo "  ok   $2"; else echo "  FAIL $2"; fails=$((fails+1)); fi; }
check() { timeout 60 "$out/heapkind_check" 12 >$work/$1.out 2>&1; rc=$?; pid=$(sed -n 's/^pid //p' $work/$1.out); stop; }
mb() { grep -o " $1 [0-9-]*/[0-9-]*M" | tail -1 | sed 's/.*\/\([0-9-]*\)M/\1/'; } # mb <kind> < line: its growth
live() { grep -o " $1 [0-9-]*" | tail -1 | awk '{print $2}'; } # live <kind> < line

# 1
start plenty VKBRIDGE_FAKE_MEMTOTAL_MB=8000 VKBRIDGE_FAKE_MEMAVAIL_MB=6000
check plenty
k=$(grep -o "memkind: .*" "$log" | tail -1)
[ $rc = 0 ] && [ "$(echo "$k" | live shader)" = 4 ] && [ "$(echo "$k" | live layout)" = 3 ] && [ "$(echo "$k" | live setlayout)" = 1 ] &&
  [ "$(echo "$k" | live pipeline)" = 3 ] && [ "$(echo "$k" | live descpool)" = 2 ] && [ "$(echo "$k" | live pcache)" = 1 ] &&
  [ "$(echo "$k" | live memory)" = 1 ] && [ "$(echo "$k" | mb layout)" -ge 100 ] && ! grep -q "memtop:" "$log"
step $? "1 counts: ${k:-no memkind line} ($(tail -1 $work/plenty.out))"

# 2
start low VKBRIDGE_FAKE_MEMTOTAL_MB=8000 VKBRIDGE_FAKE_MEMAVAIL_MB=500
check low
t=$(grep -o "memtop: .*" "$log" | head -1)
first=$(echo "$t" | sed 's/.*owners kind\/client\/live\/grew: *//' | cut -d';' -f1)
[ $rc = 0 ] && grep -q "memory low: 500 MB free of 8000 MB" "$log" && echo "$first" | grep -Eq "^layout heapkind_check\(pid $pid\) 3 [0-9]+M$" &&
  [ "$(echo "$first" | grep -o '[0-9]*M$' | tr -d M)" -ge 100 ] && echo "$t" | grep -Eq "^memtop: memory low \(1 of [0-9]+\), server anon [0-9]+M heap [0-9]+M"
step $? "2 memory low: ${t:-no memtop line}"

# 3
start off VKBRIDGE_FAKE_MEMTOTAL_MB=8000 VKBRIDGE_FAKE_MEMAVAIL_MB=500 VKBRIDGE_HEAPKIND=0
check off
[ $rc = 0 ] && grep -q "memory low: 500 MB" "$log" && ! grep -q "memkind:\|memtop:" "$log"
step $? "3 VKBRIDGE_HEAPKIND=0: no memkind/memtop line"

if [ $fails = 0 ]; then echo "OK: $steps steps"; else echo "FAILED: $fails of $steps steps"; exit 1; fi
