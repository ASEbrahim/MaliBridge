#!/bin/bash
# split_varying (MK-47, server_errata_splitvary.c + spirv_splitvary.c) on the desktop driver, with a
# server of its own (its own socket and cache directory; the switches go in <cache dir>/env):
#   splitvary_check (a vec4 vertex output read in Component pieces, Zink's fixed-function pair, and
#   the pairs around it):
#     1  as it is (not an Arm GPU): off, every case exact, nothing merged;
#     2  the bug imitated beneath, no fix: vec4-split, vec4-q and zink-split wrong, the others
#        exact - the picture the Mali-G68 r38p1 gave on a test phone (component 3 reads 0);
#     3  the imitation with the fix from the version gate (r38p1) over it: every case exact, the
#        merged fragment modules logged, the imitation breaks nothing;
#     4  forced on over the real driver (VKBRIDGE_ERRATA=+split_varying): every case exact (the
#        merged interface draws the same; 3 pipelines merged - a module merged before is logged once
#        per server, so the device's total is read), vkbridge-smoke's modules untouched;
#   and:
#     5  splitvary_unit (the pass alone): scans, merges, untouched pairs, imitation, deterministic;
#        its outputs spirv-val clean (spirv-val from PATH or the NDK's shader-tools, when found);
#     6  the protocol hash is unchanged (server-side only).
# Prints one line per step and "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
work=$(mktemp -d)
cache=$work/c
mkdir -p "$cache"
sock=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-splitvary-$$.sock
log=$work/server.log
srv=
start() {
  [ -n "$srv" ] && kill $srv 2>/dev/null && wait $srv 2>/dev/null
  rm -f "$sock"
  env -u VKBRIDGE_FAKE_MISSING -u VKBRIDGE_HIDE_EXTS -u VKBRIDGE_ERRATA -u VKBRIDGE_ERRATA_IMITATE -u VKBRIDGE_MODE \
      -u VKBRIDGE_FAKE_DRIVER_VERSION -u VKBRIDGE_ERRATA_PROBE -u VKBRIDGE_SPLITVARY_KEEP_MB "$@" \
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
  new=$(tail -n +$((l+1)) "$log")
}
failed_cases() { grep '^FAIL ' $work/run.log | sed 's/^FAIL \([^:]*\):.*/\1/' | tr '\n' ',' | sed 's/,$//'; }

start
# 1
run VKBRIDGE_MODE=safe -- splitvary_check
[ $rc = 0 ] && ! echo "$new" | grep -q "split_varying:" && echo "$new" | grep -q "errata: device .*: on [^;]*; off .*split_varying"
step $? "as it is, not an Arm GPU: off, every case exact ($res)"
# 2
run VKBRIDGE_ERRATA_IMITATE=split_varying -- splitvary_check
f=$(failed_cases)
[ $rc != 0 ] && [ "$f" = vec4-split,vec4-q,zink-split ] && echo "$new" | grep -q "errata: split_varying bug imitated beneath the fix" &&
  echo "$new" | grep -q "the imitation broke 3 pipeline(s)"
step $? "the bug imitated, no fix: wrong $f ($res)"
# 3
run VKBRIDGE_ERRATA_IMITATE=split_varying VKBRIDGE_FAKE_DRIVER_VERSION=r38p1 VKBRIDGE_MODE=performance -- splitvary_check
[ $rc = 0 ] && echo "$new" | grep -q "errata: split_varying ON (r38p1 in r0p0-.*every Arm driver" &&
  [ "$(echo "$new" | grep -c 'split_varying: fragment module h-[0-9a-f]*: location(s) 0x2 merged')" = 3 ] &&
  echo "$new" | grep -q "split_varying: device [0-9]*: 3 pipeline(s) given a merged fragment input, 0 left as they were" &&
  echo "$new" | grep -q "the imitation broke 0 pipeline(s)"
step $? "the imitation, the fix (r38p1) over it: $res; $(echo "$new" | grep -o 'split_varying: device [0-9]*: [0-9]* pipeline(s) given[^,]*' | tail -1)"
# 4
run VKBRIDGE_ERRATA=+split_varying -- splitvary_check
r4=$rc res4=$res m4=$(echo "$new" | grep -o 'split_varying: device [0-9]*: [0-9]* pipeline(s) given' | tail -1 | awk '{print $4}')
run VKBRIDGE_ERRATA=+split_varying -- vkbridge-smoke
[ $r4 = 0 ] && [ "$m4" = 3 ] && [ $rc = 0 ] && echo "$new" | grep -q "split_varying: device [0-9]*: 0 pipeline(s) given"
step $? "forced on over the real driver: splitvary_check $res4 ($m4 pipelines merged); vkbridge-smoke $res, nothing merged"
kill $srv 2>/dev/null; wait $srv 2>/dev/null; srv=

# 5
mkdir -p $work/spv
"$out/splitvary_unit" $work/spv >$work/unit.log 2>&1
r5=$?
val=spirv-val
command -v $val >/dev/null || val=$(ls ${ANDROID_HOME:-$HOME/Android/Sdk}/ndk/*/shader-tools/linux-x86_64/spirv-val 2>/dev/null | tail -1)
vres="spirv-val not found"
if [ -n "$val" ] && [ -x "$(command -v $val)" ]; then
  nv=0 bad=0
  for f in $work/spv/*.spv; do nv=$((nv+1)); $val --target-env vulkan1.3 "$f" >/dev/null 2>&1 || { bad=$((bad+1)); echo "    invalid: $(basename $f)"; }; done
  vres="$nv outputs spirv-val clean"
  [ $bad = 0 ] && [ $nv -gt 0 ] || { r5=1; vres="$bad of $nv outputs invalid"; }
fi
step $r5 "splitvary_unit: $(tail -1 $work/unit.log); $vres"

# 6
grep -q "^#define VKB_PROTOCOL_HASH 0xe88f0e43c2b566b9ULL" ../common/gen/vkb_gen.h
step $? "protocol hash $(grep -o '0x[0-9a-f]*' ../common/gen/vkb_gen.h | head -1)"

if [ $fails = 0 ]; then echo "OK: $steps steps"; else echo "FAILED: $fails of $steps steps"; exit 1; fi
