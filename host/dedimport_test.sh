#!/bin/bash
# dedicated_import (MK-39, server_errata_mem.c) on the desktop driver, with a server of its own (its
# own socket and cache directory; the switches go in <cache dir>/env, re-read at each device),
# dedimport_check:
#   1  as it is (not an Arm GPU: off): every case passes;
#   2  the bug imitated beneath, fix off: the import into a plain optimal image fails, the one into
#      a buffer made for dma-bufs does not;
#   3  the bug imitated, fix on (from the version gate, r44p1): all pass, the struct taken off the
#      image's import; off on r54p1;
#   4  the protocol hash is unchanged (server-side only).
# Prints one line per step and "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
work=$(mktemp -d)
cache=$work/c
mkdir -p "$cache"
sock=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-dedimport-$$.sock
log=$work/server.log
srv=
start() {
  [ -n "$srv" ] && kill $srv 2>/dev/null && wait $srv 2>/dev/null
  rm -f "$sock"
  env -u VKBRIDGE_FAKE_MISSING -u VKBRIDGE_HIDE_EXTS -u VKBRIDGE_ERRATA -u VKBRIDGE_ERRATA_IMITATE -u VKBRIDGE_MODE \
      -u VKBRIDGE_FAKE_DRIVER_VERSION -u VKBRIDGE_ERRATA_PROBE "$@" \
      VK_DRIVER_FILES=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json} VK_ICD_FILENAMES= \
      "$out/vkbridge-server" --socket "$sock" --log "$log" --cache-dir "$cache" >/dev/null 2>&1 &
  srv=$!
  for _ in $(seq 50); do [ -S "$sock" ] && break; sleep 0.1; done
}
trap 'kill $srv 2>/dev/null; rm -f "$sock"; [ -n "$KEEP" ] || rm -rf "$work"' EXIT
export VKBRIDGE_SOCKET=$sock VK_DRIVER_FILES=$out/vkbridge_icd.json VK_ICD_FILENAMES=
unset VKBRIDGE_HIDE_EXTS VKBRIDGE_MAX_API
fails=0 steps=0
step() { steps=$((steps+1)); if [ "$1" = 0 ]; then echo "  ok   $2"; else echo "  FAIL $2"; fails=$((fails+1)); fi; }

# run <env lines...> -- <program args...>: $rc, $res (last line), $new (the server's log lines),
# $out_log (the program's output)
run() {
  local envs=()
  while [ "$1" != -- ]; do envs+=("$1"); shift; done
  shift
  printf '%s\n' "${envs[@]}" > "$cache/env"
  local l; l=$(wc -l < "$log")
  timeout 120 "$out/$@" >$work/run.log 2>&1
  rc=$?
  res=$(tail -1 $work/run.log)
  new=$(tail -n +$((l+1)) "$log")
}
on_set() { echo "$new" | grep -o 'errata: device .*: on [^;]*' | tail -1 | sed 's/.*: on //'; }
has() { case ",$1," in *",$2,"*) return 0 ;; esac; return 1; }

start
# 1
run -- dedimport_check
if echo "$res" | grep -q "0 of 0"; then
  step 0 "dedimport_check: $(grep -m1 SKIP $work/run.log)"
else
  [ $rc = 0 ] && ! has "$(on_set)" dedicated_import
  step $? "dedicated_import off (not an Arm GPU): $res"
  # 2
  run VKBRIDGE_ERRATA_IMITATE=dedicated_import -- dedimport_check
  [ $rc != 0 ] && echo "$res" | grep -q "^FAIL: 1 of 3 checks failed" &&
    grep -q "^FAIL dma-buf imported with a dedicated struct into an optimal image" $work/run.log &&
    grep -q "^PASS dma-buf imported with a dedicated struct into a buffer made for dma-bufs" $work/run.log
  step $? "the bug imitated, no fix: $res (the plain image's import refused, the dma-buf buffer's not)"
  # 3
  run VKBRIDGE_ERRATA_IMITATE=dedicated_import VKBRIDGE_FAKE_DRIVER_VERSION=r44p1 -- dedimport_check
  r8=$rc res8=$res new8=$new
  run VKBRIDGE_FAKE_DRIVER_VERSION=r54p1 -- dedimport_check
  [ $r8 = 0 ] && echo "$new8" | grep -q "errata: dedicated_import ON (r44p1 in r0p0-r54p0 (fixed between r32p1 and r54p1); safe mode) - MK-39" &&
    { echo "$new8" | grep -q "errata: dedicated_import: an import's dedicated image (made without handle type 0x200) taken off: 0" ||
      grep -q "errata: dedicated_import: an import's dedicated image" "$log"; } &&
    [ $rc = 0 ] && ! has "$(on_set)" dedicated_import
  step $? "the fix from the version gate (r44p1) over the imitation: $res8; off on r54p1"
fi


# 4
grep -q "^#define VKB_PROTOCOL_HASH 0x4a9da939fdf65896ULL" ../common/gen/vkb_gen.h
step $? "protocol hash $(grep -o '0x[0-9a-f]*' ../common/gen/vkb_gen.h | head -1)"

if [ $fails = 0 ]; then echo "OK: $steps steps"; else echo "FAILED: $fails of $steps steps"; exit 1; fi
