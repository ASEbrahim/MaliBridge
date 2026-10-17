#!/bin/bash
# The driver-errata layer (server_errata.c) on the desktop driver, with a server of its own (its
# own socket and cache directory; the switches go in <cache dir>/env, re-read at each device):
#   1  the decisions by Mali driver version (VKBRIDGE_FAKE_DRIVER_VERSION) x mode: which errata
#      are on for r44p1 (Mali-G720), r49p1 (Mali-G925, given as its beta driverInfo string), r54p2,
#      r54p3, a job manager GPU, an Arm driver whose version cannot be read; clamp_zero (MK-32) on
#      r49p1 only for a beta build and only in safe mode, on r51p0, off from r52p0; cull_nontri
#      (MK-16) always in safe mode, below r52 in performance mode; dedicated_import (MK-39) below
#      r54p1 and for an unknown version; dyn_reemit (MK-04) below r44p1 in performance mode;
#   2  probe-driven ones (VKBRIDGE_ERRATA_PROBE): a failed probe turns its erratum on, a probe that
#      only records does not;
#   3  VKBRIDGE_ERRATA=+name,-name forces, an unknown name is warned about; a later device with
#      the same decisions logs one short line, another mode logs them in full again;
#   4  the no-op example fix with its imitation beneath it, on a device whose null descriptors are
#      emulated: the bridge's own image views (the stand-ins; the app makes none) go through both;
#   5  the protocol hash is unchanged (the layer is server-side only).
# Prints one line per step and "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
work=$(mktemp -d)
cache=$work/c
mkdir -p "$cache"
sock=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-errata-$$.sock
log=$work/server.log
srv=
start() {
  [ -n "$srv" ] && kill $srv 2>/dev/null && wait $srv 2>/dev/null
  rm -f "$sock"
  env -u VKBRIDGE_FAKE_MISSING -u VKBRIDGE_HIDE_EXTS -u VKBRIDGE_ERRATA -u VKBRIDGE_ERRATA_IMITATE -u VKBRIDGE_MODE \
      -u VKBRIDGE_FAKE_DRIVER_VERSION -u VKBRIDGE_ERRATA_PROBE "$@" VK_DRIVER_FILES=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json} VK_ICD_FILENAMES= \
      "$out/vkbridge-server" --socket "$sock" --log "$log" --cache-dir "$cache" >/dev/null 2>&1 &
  srv=$!
  for _ in $(seq 50); do [ -S "$sock" ] && break; sleep 0.1; done
}
trap 'kill $srv 2>/dev/null; rm -f "$sock"; [ -n "$KEEP" ] || rm -rf "$work"' EXIT
export VKBRIDGE_SOCKET=$sock VK_DRIVER_FILES=$out/vkbridge_icd.json VK_ICD_FILENAMES=
unset VKBRIDGE_HIDE_EXTS VKBRIDGE_MAX_API
fails=0 steps=0
step() { steps=$((steps+1)); if [ "$1" = 0 ]; then echo "  ok   $2"; else echo "  FAIL $2"; fails=$((fails+1)); fi; }

# One device with <cache dir>/env = the arguments (one KEY=VALUE each); $new: its log lines
device() {
  printf '%s\n' "$@" > "$cache/env"
  local l; l=$(wc -l < "$log")
  timeout 120 "$out/vkbridge-smoke" >$work/smoke.log 2>&1
  new=$(tail -n +$((l+1)) "$log")
}
# The errata on, from the device's summary line
on_set() { echo "$new" | grep -o 'errata: device .*: on [^;]*' | tail -1 | sed 's/.*: on //'; }

start
# 1 the version matrix
matrix() { # mode version expected
  device "VKBRIDGE_MODE=$1" "VKBRIDGE_FAKE_DRIVER_VERSION=$2"
  local got; got=$(on_set)
  [ "$got" = "$3" ]
  step $? "$1 mode, $2: $got$([ "$got" = "$3" ] || echo " (want $3)")"
}
matrix safe r44p1 wait_retry,bitwise_scalar,dyn_reemit,vb_split,view_counts,clamp_zero,cull_nontri,dedicated_import
matrix safe v1.r49p1-03bet0 wait_retry,bitwise_scalar,dyn_reemit,vb_split,view_quarantine,view_counts,clamp_zero,cull_nontri,dedicated_import
echo "$new" | grep -q "driver r49p1 beta (VKBRIDGE_FAKE_DRIVER_VERSION \"v1.r49p1-03bet0\")" &&
  echo "$new" | grep -q "errata: view_quarantine ON (r49p1 in r49p1-r49p2; safe mode[;)]" &&
  echo "$new" | grep -q "errata: clamp_zero ON (r49p1 in r49p1-r49p1 (a beta build: Arm's fix in r49p1 not confirmed for it); safe mode) - MK-32"
step $? "the beta driverInfo string decoded, the reasons logged ($(echo "$new" | grep -o 'errata: view_quarantine ON.*'); $(echo "$new" | grep -o 'errata: clamp_zero ON.*' | cut -c1-60)...)"
matrix safe v1.r49p1-01eac0 wait_retry,bitwise_scalar,dyn_reemit,vb_split,view_quarantine,view_counts,cull_nontri,dedicated_import
matrix safe r51p0 wait_retry,bitwise_scalar,dyn_reemit,vb_split,view_quarantine,view_counts,clamp_zero,cull_nontri,dedicated_import
matrix safe r52p0 wait_retry,bitwise_scalar,dyn_reemit,vb_split,view_counts,cull_nontri,dedicated_import
matrix safe r54p0 wait_retry,dyn_reemit,vb_split,view_counts,cull_nontri,dedicated_import
matrix safe r54p2 wait_retry,dyn_reemit,vb_split,view_counts,cull_nontri
matrix safe r54p3 wait_retry,dyn_reemit,vb_split,view_counts,cull_nontri
matrix performance r44p1 wait_retry,vb_split,view_counts,clamp_zero,cull_nontri,dedicated_import
matrix performance r44p0 wait_retry,dyn_reemit,vb_split,view_counts,clamp_zero,cull_nontri,dedicated_import
matrix performance r36p0 wait_retry,dyn_reemit,vb_split,view_counts,cull_nontri,dedicated_import
matrix performance v1.r49p1-03bet0 wait_retry,dyn_reemit,vb_split,view_quarantine,view_counts,cull_nontri,dedicated_import
matrix performance r52p0 wait_retry,dyn_reemit,vb_split,view_counts,dedicated_import
matrix performance r54p2 wait_retry,vb_split,view_counts
matrix performance r54p3 wait_retry,vb_split,view_counts
matrix performance r46p0:jm wait_retry,dyn_reemit,vb_split,view_counts,clamp_zero,cull_nontri,dedicated_import
matrix performance r46p0:csf wait_retry,vb_split,view_counts,clamp_zero,cull_nontri,dedicated_import
matrix performance r50p0 wait_retry,dyn_reemit,vb_split,view_quarantine,view_counts,clamp_zero,cull_nontri,dedicated_import
matrix safe not-a-version wait_retry,dyn_reemit,vb_split,view_quarantine,view_counts,clamp_zero,cull_nontri,dedicated_import

# 2 probes
device VKBRIDGE_MODE=performance VKBRIDGE_FAKE_DRIVER_VERSION=r54p3 VKBRIDGE_ERRATA_PROBE=viewport_flip:fail,bitwise_vec:fail,rendering_churn:fail,dyn_state:pass
got=$(on_set)
[ "$got" = wait_retry,viewport_flip,bitwise_scalar,vb_split,view_counts ] && echo "$new" | grep -q "errata: viewport_flip ON (probe viewport_flip failed; performance mode"
step $? "failed probes turn on viewport_flip and bitwise_scalar, the record-only rendering_churn does not: $got"

# 3 switches
device VKBRIDGE_FAKE_DRIVER_VERSION=r44p1 VKBRIDGE_ERRATA=+example,-view_counts,no_such_erratum
got=$(on_set)
[ "$got" = wait_retry,bitwise_scalar,dyn_reemit,vb_split,clamp_zero,cull_nontri,dedicated_import,example ] && echo "$new" | grep -q "errata: view_counts OFF (VKBRIDGE_ERRATA=-view_counts" &&
  echo "$new" | grep -q "no erratum named no_such_erratum" && echo "$new" | grep -q "errata: example ON (VKBRIDGE_ERRATA=+example)"
step $? "VKBRIDGE_ERRATA=+example,-view_counts: $got; the unknown name warned about"
# the same decisions again: one short line, not the reasons again; another mode: in full again
first=$(echo "$new" | grep -o 'errata: device [0-9]*,' | tail -1 | grep -o '[0-9]*')
device VKBRIDGE_FAKE_DRIVER_VERSION=r44p1 VKBRIDGE_ERRATA=+example,-view_counts
got=$(on_set)
[ "$got" = wait_retry,bitwise_scalar,dyn_reemit,vb_split,clamp_zero,cull_nontri,dedicated_import,example ] && ! echo "$new" | grep -qE "errata: [a-z_]* O(N|FF) \(" &&
  echo "$new" | grep -q "errata: device [0-9]*: same as device $first (safe mode): on $got\$" &&
  ! echo "$new" | grep -q "clamp_zero gives"
r=$?; same=$(echo "$new" | grep -o 'errata: device [0-9]*: same as.*' | tail -1)
device VKBRIDGE_MODE=performance VKBRIDGE_FAKE_DRIVER_VERSION=r44p1 VKBRIDGE_ERRATA=+example,-view_counts
[ $r = 0 ] && echo "$new" | grep -q "errata: view_counts OFF (VKBRIDGE_ERRATA=-view_counts" &&
  echo "$new" | grep -q "errata: device [0-9]*, .*, performance mode: on $(on_set);" && echo "$new" | grep -q "clamp_zero gives"
step $? "a later device with the same decisions: \"${same:-no short line}\"; in performance mode in full again (on $(on_set))"

# 4 the example fix and its imitation, beneath the null-descriptor emulation
printf 'VKBRIDGE_FAKE_MISSING=robustness2\nVKBRIDGE_ERRATA=+example\nVKBRIDGE_ERRATA_IMITATE=example\n' > "$cache/env"
: > "$log"
start
timeout 60 "$out/nulldesc_check" >$work/4.log 2>&1
r=$?
calls=$(grep -o 'example: [0-9]* vkCreateImageView call(s) through the fix, [0-9]* through' "$log" | tail -1)
fix=$(echo "$calls" | grep -o 'example: [0-9]*' | grep -o '[0-9]*'); imi=$(echo "$calls" | grep -o ', [0-9]* through' | grep -o '[0-9]*')
[ $r = 0 ] && grep -q "errata: example bug imitated beneath the fix" "$log" && [ "${fix:-0}" -ge 6 ] && [ "$fix" = "$imi" ] &&
  [ "$(grep -c 'null descriptor stand-in made' "$log")" -ge 6 ]
step $? "the bridge's own views through the fix and the imitation beneath: ${fix:-0} and ${imi:-0} ($(tail -1 $work/4.log))"

# 5
grep -q "^#define VKB_PROTOCOL_HASH 0x4a9da939fdf65896ULL" ../common/gen/vkb_gen.h
step $? "protocol hash $(grep -o '0x[0-9a-f]*' ../common/gen/vkb_gen.h | head -1)"

if [ $fails = 0 ]; then echo "OK: $steps steps"; else echo "FAILED: $fails of $steps steps"; exit 1; fi
