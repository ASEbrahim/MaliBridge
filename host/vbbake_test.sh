#!/bin/bash
# vb_bake (MK-05, option B; server_errata_vbbake.c): strides baked into pipeline variants where
# dynamic strides are wrong even one binding per call (the Galaxy A16's G68 r38p1, MaliBridge#3).
#     1  the G68's shape imitated (VKBRIDGE_ERRATA_IMITATE=vb_bake: the last stride of a call goes
#        to every binding), fix off: vbstride_check fails
#     2  imitated, fix forced on: vbstride_check passes and variants were made (the strides still
#        reach the imitation; a pipeline with its strides baked in ignores them)
#     3  no imitation, fix forced on: vbstride_check passes (exact on a right driver)
#     4  fix forced on: dynstate_check and polygon_check (dynamic strides with other state) pass
#     5  not forced and no probe result for the single-binding shape: not installed
# Prints one line per step and "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
work=$(mktemp -d)
cache=$work/c
mkdir -p "$cache"
sock=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-vbbake-$$.sock
log=$work/server.log
srv=
env -u VKBRIDGE_FAKE_MISSING -u VKBRIDGE_HIDE_EXTS -u VKBRIDGE_ERRATA -u VKBRIDGE_ERRATA_IMITATE -u VKBRIDGE_MODE \
    -u VKBRIDGE_FAKE_DRIVER_VERSION -u VKBRIDGE_ERRATA_PROBE -u VKBRIDGE_ERRATA_IMITATE_VB \
    VK_DRIVER_FILES=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json} VK_ICD_FILENAMES= \
    "$out/vkbridge-server" --socket "$sock" --log "$log" --cache-dir "$cache" >/dev/null 2>&1 &
srv=$!
for _ in $(seq 50); do [ -S "$sock" ] && break; sleep 0.1; done
trap 'kill $srv 2>/dev/null; rm -f "$sock"; [ -n "$KEEP" ] || rm -rf "$work"' EXIT
export VKBRIDGE_SOCKET=$sock VK_DRIVER_FILES=$out/vkbridge_icd.json VK_ICD_FILENAMES=
unset VKBRIDGE_HIDE_EXTS VKBRIDGE_MAX_API
fails=0 steps=0
step() { steps=$((steps+1)); if [ "$1" = 0 ]; then echo "  ok   $2"; else echo "  FAIL $2"; fails=$((fails+1)); fi; }

# run <env lines...> -- <program>: one program with <cache dir>/env = the env lines; $rc, $res
# (its last line), $new (the server log lines it made)
run() {
  local envs=()
  while [ "$1" != -- ]; do envs+=("$1"); shift; done
  shift
  printf '%s\n' "${envs[@]}" > "$cache/env"
  local l; l=$(wc -l < "$log")
  timeout 300 "$out/$1" >$work/run.log 2>&1
  rc=$?
  res=$(tail -1 $work/run.log)
  sleep 0.3 # the device's last log lines
  new=$(tail -n +$((l+1)) "$log")
}
lg() { echo "$new" | grep -o "$1" | tail -1; }

run VKBRIDGE_ERRATA=-vb_bake VKBRIDGE_ERRATA_IMITATE=vb_bake -- vbstride_check
[ $rc != 0 ] && echo "$new" | grep -q "imitated: [1-9][0-9]* call(s) whose last stride went to every binding"
step $? "the G68's shape imitated, fix off: vbstride_check fails ($(grep '^shape' $work/run.log | cut -c1-60))"

run VKBRIDGE_ERRATA=+vb_bake VKBRIDGE_ERRATA_IMITATE=vb_bake -- vbstride_check
[ $rc = 0 ] && echo "$new" | grep -q "vb_bake: [1-9][0-9]* pipeline(s) with their strides baked in, [1-9][0-9]* variant(s) made"
step $? "imitated, fix on: $res; $(lg 'vb_bake: [0-9]* pipeline(s) with their strides baked in, [0-9]* variant(s) made, [0-9]* variant bind(s)')"

run VKBRIDGE_ERRATA=+vb_bake -- vbstride_check
[ $rc = 0 ]
step $? "no imitation, fix on: $res"

run VKBRIDGE_ERRATA=+vb_bake -- dynstate_check
r1=$rc d1=$res
run VKBRIDGE_ERRATA=+vb_bake -- polygon_check
[ $r1 = 0 ] && [ $rc = 0 ]
step $? "fix on: dynstate_check: $d1; polygon_check: $res"

run -- vbstride_check
[ $rc = 0 ] && ! echo "$new" | grep -q "vb_bake: [0-9]* pipeline"
step $? "not forced, no single-binding probe result: not installed"

if [ $fails = 0 ]; then echo "OK: $steps steps"; else echo "FAILED: $fails of $steps steps"; exit 1; fi
