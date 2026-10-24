#!/bin/bash
# stencil_layout (MK-11, Arm erratum 4021056, server_errata_ds.c) on the desktop driver, with a
# server of its own (its own socket and cache directory; the switches go in <cache dir>/env,
# re-read at each device), stencillayout_check:
#   1  as it is (not an Arm GPU: off): both cases pass;
#   2  the bug imitated beneath, fix off: the render pass's stencil is lost, dynamic rendering not;
#   3  the bug imitated, fix on from the version gate (r49p1, the G925's): both pass, the render
#      pass's layouts and vkCmdBeginRendering's merged, the device's totals logged;
#   4  the gate: off on r54p1 (fixed) and r40p0 (before), on on r41p0 and r54p0; in performance
#      mode on on r44p1, off on r54p1;
#   5  forced on (VKBRIDGE_ERRATA=+stencil_layout) on this GPU: both pass (the rewrite is exact).
# Prints one line per step and "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
work=$(mktemp -d)
cache=$work/c
mkdir -p "$cache"
sock=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-stencil-$$.sock
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

# run <env lines...> -- <program args...>: $rc, $res (last line), $new (the server's log lines)
run() {
  local envs=()
  while [ "$1" != -- ]; do envs+=("$1"); shift; done
  shift
  printf '%s\n' "${envs[@]}" > "$cache/env"
  local l; l=$(wc -l < "$log")
  timeout 120 "$out/$@" >$work/run.log 2>&1
  rc=$?
  res=$(tail -1 $work/run.log)
  sleep 0.2
  new=$(tail -n +$((l+1)) "$log")
}
on_set() { echo "$new" | grep -o 'errata: device .*: on [^;]*' | tail -1 | sed 's/.*: on //'; }
has() { case ",$1," in *",$2,"*) return 0 ;; esac; return 1; }

start
# 1
run -- stencillayout_check
if echo "$res" | grep -q "0 of 0"; then
  step 0 "stencillayout_check: $(grep -m1 SKIP $work/run.log)"
else
  [ $rc = 0 ] && ! has "$(on_set)" stencil_layout && echo "$res" | grep -q "^OK: 2 of 2"
  step $? "stencil_layout off (not an Arm GPU): $res"
  # 2
  run VKBRIDGE_ERRATA_IMITATE=stencil_layout -- stencillayout_check
  [ $rc != 0 ] && echo "$res" | grep -q "^FAIL: 1 of 2 checks failed" &&
    grep -q "^FAIL render pass with separate stencil layouts" $work/run.log &&
    grep -q "^PASS dynamic rendering" $work/run.log &&
    echo "$new" | grep -q "errata: stencil_layout bug imitated beneath the fix"
  step $? "the bug imitated, no fix: $res (the render pass's stencil lost, dynamic rendering's not)"
  # 3
  run VKBRIDGE_ERRATA_IMITATE=stencil_layout VKBRIDGE_FAKE_DRIVER_VERSION=r49p1 -- stencillayout_check
  [ $rc = 0 ] && echo "$res" | grep -q "^OK: 2 of 2" &&
    echo "$new" | grep -q "errata: stencil_layout ON (r49p1 in r41p0-r54p0 (Arm 4021056, fixed in r54p1); safe mode) - MK-11" &&
    { echo "$new" | grep -q "errata: stencil_layout: a render pass's separate stencil layouts merged into combined ones (2): 0" ||
      grep -q "errata: stencil_layout: a render pass's separate stencil layouts merged" "$log"; } &&
    grep -q "errata: stencil_layout: vkCmdBeginRendering's depth and stencil layouts merged (0x3b9e7769, 0x3b9e776a -> 0x3b9c9308)" "$log" &&
    echo "$new" | grep -q "errata: stencil_layout: 1 render pass(es), 1 with separate stencil layouts merged (2 layouts), 0 left .*; 1 vkCmdBeginRendering merged; imitated: 0 pass(es) with the pattern, 0 stencil(s) lost"
  step $? "the fix from the version gate (r49p1) over the imitation: $res"
  # 4
  g=
  for v in r54p1:off r40p0:off r41p0:on r54p0:on; do
    run VKBRIDGE_FAKE_DRIVER_VERSION=${v%:*} -- stencillayout_check
    if has "$(on_set)" stencil_layout; then s=on; else s=off; fi
    [ $rc = 0 ] && [ $s = ${v#*:} ] || g="$g ${v%:*}=$s/rc$rc"
  done
  run VKBRIDGE_MODE=performance VKBRIDGE_FAKE_DRIVER_VERSION=r44p1 -- stencillayout_check
  has "$(on_set)" stencil_layout || g="$g perf-r44p1=off"
  run VKBRIDGE_MODE=performance VKBRIDGE_FAKE_DRIVER_VERSION=r54p1 -- stencillayout_check
  has "$(on_set)" stencil_layout && g="$g perf-r54p1=on"
  [ -z "$g" ]
  step $? "the gate: off on r54p1 and r40p0, on on r41p0 and r54p0, performance mode on r44p1 / off r54p1${g:+; wrong:$g}"
  # 5
  run VKBRIDGE_ERRATA=+stencil_layout -- stencillayout_check
  [ $rc = 0 ] && echo "$res" | grep -q "^OK: 2 of 2" && has "$(on_set)" stencil_layout
  step $? "forced on (exact on any driver): $res"
fi

if [ $fails = 0 ]; then echo "OK: $steps steps"; else echo "FAILED: $fails of $steps steps"; exit 1; fi
