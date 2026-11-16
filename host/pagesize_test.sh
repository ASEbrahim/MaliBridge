#!/bin/bash
# 16 KB-page kernels (a Pixel's developer option), on a 4 KB host: VKBRIDGE_FAKE_PAGE_SIZE=16384 makes the
# client report a 16 KB page where an app is told one (common/vkb_page.h), mapping lengths stay the host's.
#   1  pagesize_unit: the page for lengths, the page told, the import alignment raised to it, nonsense ignored;
#   2  placed_check as it is: the alignment told is the host's page, a power of two;
#   3  placed_check --page 16384 with the 16 KB page imitated: the alignment told is 16384, the range is
#      reserved aligned to it, the allocation's size is not a multiple of it (1 MiB + 4 KiB): placed, written,
#      copied by the GPU, unmapped with the range kept reserved, mapped again, unplaced;
#   4  the server's import alignment (host-pointer imports of the client's memfd) is raised to the page too:
#      the self-test's line "host-pointer import alignment" under the same imitation in the server's environment;
#   5  the protocol hash is unchanged.
# Prints one line per step and "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
work=$(mktemp -d)
cache=$work/c
mkdir -p "$cache"
sock=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-pagesize-$$.sock
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
# 1
"$out/pagesize_unit" >$work/unit.log 2>&1
step $? "pagesize_unit ($(tail -1 $work/unit.log))"
# 2
start
run "$out/placed_check" --
if ! grep -q "extensions offered" $work/run.log; then echo "OK: skipped (no VK_EXT_map_memory_placed)"; exit 0; fi
[ $rc = 0 ] && grep -q "PASS alignment: a power of two" $work/run.log
step $? "placed_check as it is: $res"
# 3
start
VKBRIDGE_FAKE_PAGE_SIZE=16384 timeout 120 "$out/placed_check" --page 16384 >$work/run.log 2>&1
rc=$?; res=$(tail -1 $work/run.log)
[ $rc = 0 ] && grep -q "PASS alignment told is the page asked for" $work/run.log && grep -q "PASS placed map at the reserved address" $work/run.log &&
  grep -q "PASS unmap with reserve leaves the range reserved" $work/run.log && grep -q "PASS plain map after unplacing" $work/run.log
step $? "16 KB page imitated, 1 MiB + 4 KiB placed at a 16 KB-aligned address: $res"
# 4
start VKBRIDGE_FAKE_PAGE_SIZE=16384
grep -q "memory sharing: host-pointer import alignment 16384 (driver says [0-9]*, kernel page 4096)" "$log"
step $? "the server's import alignment follows ($(grep -o 'host-pointer import alignment [0-9]* (driver says [0-9]*, kernel page [0-9]*)' "$log" | head -1))"
start
grep -q "memory sharing: host-pointer import alignment [0-9]* (driver says [0-9]*, kernel page 4096)" "$log" &&
  ! grep -q "host-pointer import alignment 16384" "$log"
step $? "without the imitation: the driver's or the page's ($(grep -o 'host-pointer import alignment [0-9]* (driver says [0-9]*' "$log" | head -1))"
# 5
grep -q "^#define VKB_PROTOCOL_HASH 0xe88f0e43c2b566b9ULL" ../common/gen/vkb_gen.h
step $? "protocol hash $(grep -o '0x[0-9a-f]*' ../common/gen/vkb_gen.h | head -1)"

if [ $fails = 0 ]; then echo "OK: $steps steps"; else echo "FAILED: $fails of $steps steps"; exit 1; fi
