#!/bin/bash
# fragdepth_nodepth (MK-49, server_errata_fragdepth.c + spirv_fragdepth.c) on the desktop driver, with a server of its own
# (its own socket and cache directory; the switches go in <cache dir>/env). fragdepth_check draws a full-screen triangle whose
# fragment shader writes a colour and gl_FragDepth (vkd3d-proton's layered copy that never finishes on the Mali-G925 r49p1 when
# the rendering has no depth attachment: RE Village and The Witcher 3 player logs):
#   1  as it is (not an Arm GPU): off, both cases right, nothing changed;
#   2  the bug imitated beneath, no fix: the colour-only pipeline refused, the one with a depth attachment made and right;
#   3  the imitation with the fix from the gate (r49p1 on a G925) over it: both cases right, the colour-only pipeline's fragment
#      module logged once as changed, the depth case's untouched (its depth 0.75 read back);
#   4  the gate: r49p1 on a G720 leaves it off;
#   5  the protocol hash is unchanged (server-side only).
# Prints one line per step and "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
work=$(mktemp -d)
cache=$work/c
mkdir -p "$cache"
sock=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-fragdepth-$$.sock
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

# run <env lines...> -- <mode>: $rc, $res (last line), $new (the server's log lines)
run() {
  local envs=()
  while [ "$1" != -- ]; do envs+=("$1"); shift; done
  shift
  printf '%s\n' "${envs[@]}" > "$cache/env"
  local l; l=$(wc -l < "$log")
  timeout 120 "$out/fragdepth_check" "$@" >$work/run.log 2>&1
  rc=$?
  res=$(tail -1 $work/run.log)
  new=$(tail -n +$((l+1)) "$log")
}

start
# 1
run VKBRIDGE_MODE=safe -- color; r1=$rc res1=$res n1=$new
run VKBRIDGE_MODE=safe -- depth
[ $r1 = 0 ] && [ $rc = 0 ] && ! echo "$n1$new" | grep -q "fragdepth_nodepth: fragment module"
step $? "as it is, not an Arm GPU: $res1; $res"
# 2
run VKBRIDGE_ERRATA_IMITATE=fragdepth_nodepth -- color; r2=$rc res2=$res n2=$new
run VKBRIDGE_ERRATA_IMITATE=fragdepth_nodepth -- depth
[ $r2 != 0 ] && echo "$res2" | grep -q "FAIL: pipeline" && echo "$n2" | grep -q "fragdepth_nodepth imitation: a pipeline writes FragDepth with no depth attachment" &&
  [ $rc = 0 ] && ! echo "$new" | grep -q "fragdepth_nodepth imitation: a pipeline"
step $? "the bug imitated, no fix: colour-only $res2; with depth $res"
# 3
G='VKBRIDGE_FAKE_DRIVER_VERSION=r49p1@Mali-G925-Immortalis MC12'
run VKBRIDGE_ERRATA_IMITATE=fragdepth_nodepth "$G" VKBRIDGE_MODE=performance -- color; r3=$rc res3=$res n3=$new
run VKBRIDGE_ERRATA_IMITATE=fragdepth_nodepth "$G" VKBRIDGE_MODE=performance -- depth
[ $r3 = 0 ] && echo "$n3" | grep -q "errata: fragdepth_nodepth ON" &&
  echo "$n3" | grep -qE "fragdepth_nodepth: fragment module h-[0-9a-f]+: its FragDepth output dropped in a pipeline with no depth attachment" &&
  echo "$n3" | grep -q "fragdepth_nodepth: device [0-9]*: 1 pipeline(s) given a fragment shader without its depth output, 0 left" &&
  echo "$n3" | grep -q "the imitation refused 0 pipeline(s)" &&
  [ $rc = 0 ] && echo "$res" | grep -q "depth 0.75" && echo "$new" | grep -q "fragdepth_nodepth: device [0-9]*: 0 pipeline(s) given"
step $? "the imitation, the fix (r49p1 G925) over it: colour-only $res3; with depth $res (untouched)"
# 4
run VKBRIDGE_ERRATA_IMITATE=fragdepth_nodepth 'VKBRIDGE_FAKE_DRIVER_VERSION=r49p1@Mali-G720-Immortalis MC12' VKBRIDGE_MODE=performance -- color
[ $rc != 0 ] && ! echo "$new" | grep -q "errata: fragdepth_nodepth ON"
step $? "the gate: r49p1 on a G720 leaves it off ($res)"
kill $srv 2>/dev/null; wait $srv 2>/dev/null; srv=
# 5
grep -q "^#define VKB_PROTOCOL_HASH 0xe88f0e43c2b566b9ULL" ../common/gen/vkb_gen.h
step $? "protocol hash $(grep -o '0x[0-9a-f]*' ../common/gen/vkb_gen.h | head -1)"
if [ $fails = 0 ]; then echo "OK: $steps steps"; else echo "FAILED: $fails of $steps steps"; [ -n "$KEEP" ] && echo "kept $work"; exit 1; fi
