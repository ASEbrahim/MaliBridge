#!/bin/bash
# Low memory on the server (server_memory.c vkb_mem_low; MaliBridge#18), a server of its own per step:
#   1  told 500 MB are free of 8000 (VKBRIDGE_FAKE_MEMAVAIL_MB / _MEMTOTAL_MB): poolfull_check lowmem
#      passes (the pool fills to 8 pools' worth before and after), the server says "memory low"
#      once, its memory line has the free memory, "(low)" and the overflow pool count, and the idle
#      overflow pools are freed at resets (7, said once);
#   2  told 6000 MB are free: the same run, no "memory low", no overflow pool freed.
# Prints one line per step and "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
work=$(mktemp -d)
sock=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-lowmem-$$.sock
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
  env -u VKBRIDGE_FAKE_MISSING -u VKBRIDGE_HIDE_EXTS -u VKBRIDGE_MODE -u VKBRIDGE_MEM_STATS "$@" \
      VK_DRIVER_FILES=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json} VK_ICD_FILENAMES= \
      "$out/vkbridge-server" --socket "$sock" --log "$log" --cache-dir "$d/c" >/dev/null 2>&1 &
  srv=$!
  for _ in $(seq 100); do [ -S "$sock" ] && break; kill -0 $srv 2>/dev/null || break; sleep 0.1; done
}
export VKBRIDGE_SOCKET=$sock VK_DRIVER_FILES=$out/vkbridge_icd.json VK_ICD_FILENAMES=
unset VKBRIDGE_HIDE_EXTS VKBRIDGE_MAX_API VKBRIDGE_SYNC
fails=0 steps=0
step() { steps=$((steps+1)); if [ "$1" = 0 ]; then echo "  ok   $2"; else echo "  FAIL $2"; fails=$((fails+1)); fi; }

# 1
start low VKBRIDGE_FAKE_MEMTOTAL_MB=8000 VKBRIDGE_FAKE_MEMAVAIL_MB=500
timeout 90 "$out/poolfull_check" lowmem >$work/low.out 2>&1; rc=$?
stop
said=$(grep -c "memory low: 500 MB free of 8000 MB" "$log")
freed=$(grep -o "memory low, [0-9]* idle overflow pools freed" "$log" | head -1 | grep -o '[0-9]*')
line=$(grep -c "mem: rss .* free 500M swap [0-9]*M (low) |.*desc overflow pools [0-9]*" "$log")
[ $rc = 0 ] && [ "$said" = 1 ] && [ "$freed" = 7 ] && [ "$line" -ge 1 ]
step $? "told 500 MB free: $(tail -1 $work/low.out), said $said time(s), $freed overflow pools freed, $line memory line(s) marked low"

# 2
start plenty VKBRIDGE_FAKE_MEMTOTAL_MB=8000 VKBRIDGE_FAKE_MEMAVAIL_MB=6000
timeout 90 "$out/poolfull_check" lowmem >$work/ok.out 2>&1; rc=$?
stop
[ $rc = 0 ] && ! grep -q "memory low" "$log" && grep -q "mem: rss .* free 6000M swap [0-9]*M |" "$log"
step $? "told 6000 MB free: $(tail -1 $work/ok.out), nothing freed, not said"

if [ $fails = 0 ]; then echo "OK: $steps steps"; else echo "FAILED: $fails of $steps steps"; exit 1; fi
