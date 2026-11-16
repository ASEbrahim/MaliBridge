#!/bin/bash
# The command-buffer state errata (server_errata_cmd.c) on the desktop driver, with a server of its
# own (its own socket and cache directory; the switches go in <cache dir>/env, re-read at each
# device):
#   dyn_reemit (MK-04), dynstate_check:
#     1  on (safe mode, probe dyn_state not passed): every case passes, state is sent again after
#        binds, no static value is (the 3893903 part is for drivers known to be affected);
#     2  the bug imitated beneath the fix, fix off: every case fails;
#     3  the bug imitated, fix on: all pass, the static sample mask sent (3893903);
#     4  fix off, no imitation: all pass (the desktop driver keeps its state);
#     5  performance mode on r54p3: off, not installed, all pass;
#     (the runs below add +example: their decisions differ from every other run, so each logs in full)
#     5b safe mode where the probe passed and no range names the driver (this desktop driver, r44p1,
#        r54p1): off and not installed, all pass; r44p0 (in Arm 3809538's range): on;
#     5c r48p0-r53p0 CSF (Arm 3893903/3893905, r49p1 beta): only the EDS3 states sent again - with
#        that shape imitated (VKBRIDGE_ERRATA_IMITATE_DYN=eds3) every case passes, with fewer
#        values sent than the whole fix; with the r38-r44 shape imitated the depth/cull/stride
#        cases fail (the narrowing is real, and the test sees it); a job manager GPU there (r49p1:jm,
#        also ANGLE's r46-r51 JM range) keeps the whole fix and passes;
#   vb_split (MK-05), vbstride_check:
#     6  on (always): all pass, the multi-binding calls split;
#     7  the bug imitated, fix off: only the call from binding 1 fails - "multi-binding calls only";
#     8  the bug imitated, fix on: all pass, nothing reaches the imitation wrong;
#     9  the bug imitated on single calls too (VKBRIDGE_ERRATA_IMITATE_VB=single), fix on: the
#        single-binding case fails and the check says option B - the probe's two shapes told apart;
#    10  VK_EXT_vertex_input_dynamic_state: hidden on r44p1, shown on r49p1 and with -vb_split;
#   both:
#    11  both bugs imitated, both fixes on: dynstate_check and vbstride_check pass (the wrappers of
#        vkCmdBindVertexBuffers2 chain: split, then the state record, then the imitation);
#    12  the divisor emulation's own binds (VKBRIDGE_FAKE_MISSING=divisor) through both, imitated:
#        divisor_check passes; smoke passes;
#    13  Mali-G720's gaps that meet the fixes (VKBRIDGE_FAKE_MISSING=maint5,divisor,fillmode: a driver
#        without maintenance5 is imitated beneath every layer - VK_WHOLE_SIZE in
#        vkCmdBindVertexBuffers2's pSizes binds an empty range, as on the G720 r44p1), r44p1, safe
#        mode: dynstate_check (its stride case with the buffer bound by vkCmdBindVertexBuffers, which
#        dyn_reemit sent again with pSizes VK_WHOLE_SIZE, and the case with a client's VK_WHOLE_SIZE,
#        which the divisor emulation's wrapper let through), vbstride_check, divisor_check and
#        polygon_check (its dynamic-stride wireframe: the emulation's binds around the draw) pass,
#        and no VK_WHOLE_SIZE reached the driver;
#    14  the protocol hash is unchanged (the layer is server-side only).
# Prints one line per step and "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
work=$(mktemp -d)
cache=$work/c
mkdir -p "$cache"
sock=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-cmdstate-$$.sock
log=$work/server.log
srv=
start() {
  [ -n "$srv" ] && kill $srv 2>/dev/null && wait $srv 2>/dev/null
  rm -f "$sock"
  env -u VKBRIDGE_FAKE_MISSING -u VKBRIDGE_HIDE_EXTS -u VKBRIDGE_ERRATA -u VKBRIDGE_ERRATA_IMITATE -u VKBRIDGE_MODE \
      -u VKBRIDGE_FAKE_DRIVER_VERSION -u VKBRIDGE_ERRATA_PROBE -u VKBRIDGE_ERRATA_IMITATE_VB -u VKBRIDGE_ERRATA_IMITATE_DYN "$@" \
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

# run <env lines...> -- <program args...>: one program with <cache dir>/env = the env lines;
# $rc its status, $res its last line, $new the server log lines it made
run() {
  local envs=()
  while [ "$1" != -- ]; do envs+=("$1"); shift; done
  shift
  printf '%s\n' "${envs[@]}" > "$cache/env"
  local l; l=$(wc -l < "$log")
  case "$1" in /*) timeout 300 "$@" ;; *) timeout 300 "$out/$@" ;; esac >$work/run.log 2>&1
  rc=$?
  res=$(tail -1 $work/run.log)
  sleep 0.2 # the device's last log lines
  new=$(tail -n +$((l+1)) "$log")
}
lg() { echo "$new" | grep -o "$1" | tail -1; }
nfail() { grep -c '^FAIL ' $work/run.log; }
ncase() { grep -cE '^(PASS|FAIL) ' $work/run.log; }
num() { echo "$new" | grep -o "$1" | tail -1 | grep -oE '^[0-9]+'; }

start
# The probe of this cache directory passed on the desktop driver; NV: as if it had not (safe mode keeps the fix on then)
NV=VKBRIDGE_ERRATA_PROBE=dyn_state:none
# dyn_reemit (MK-04)
run $NV -- dynstate_check
eds3=$(grep -c "with the EDS3 sample mask" $work/run.log)
[ $rc = 0 ] && echo "$new" | grep -q "errata: dyn_reemit ON (probe dyn_state not passed; safe mode" && [ "$(num '[0-9]* with state sent again')" -ge 7 ] &&
  echo "$new" | grep -q " 0 static value(s) (3893903)"
step $? "dyn_reemit on: $res; $(lg '[0-9]* graphics bind(s), [0-9]* with state sent again: [0-9]* value(s)')"
run VKBRIDGE_ERRATA=-dyn_reemit VKBRIDGE_ERRATA_IMITATE=dyn_reemit -- dynstate_check
[ $rc != 0 ] && [ "$(nfail)" = $(( $(ncase) - 1 )) ] && [ "$(nfail)" -ge 7 ] && grep -q "^PASS vkCreateDevice" $work/run.log
step $? "the bug imitated, fix off: $(nfail) of $(( $(ncase) - 1 )) cases fail ($(grep -m1 -o '^FAIL depth write.*wanted [^;]*' $work/run.log))"
run $NV VKBRIDGE_ERRATA_IMITATE=dyn_reemit -- dynstate_check
full=$(num '[0-9]* value(s), [0-9]* vertex')
[ $rc = 0 ] && echo "$new" | grep -q "errata: dyn_reemit bug imitated beneath the fix" &&
  { [ "$eds3" = 0 ] || [ "$(num '[0-9]* static value(s) (3893903)')" -ge 1 ]; }
step $? "the bug imitated, fix on: $res; $(lg '[0-9]* static value(s) (3893903)'); $(lg 'imitated: [0-9]* bind(s) reset')"
run VKBRIDGE_ERRATA=-dyn_reemit -- dynstate_check
[ $rc = 0 ]
step $? "fix off, no imitation (the desktop driver keeps its state): $res"
run VKBRIDGE_MODE=performance VKBRIDGE_FAKE_DRIVER_VERSION=r54p3 -- dynstate_check
[ $rc = 0 ] && ! echo "$new" | grep -q "errata: dyn_reemit ON" && ! echo "$new" | grep -q "errata: dyn_reemit:"
step $? "performance mode, r54p3: off and not installed; $res"
# vb_split (MK-05)
run -- vbstride_check
[ $rc = 0 ] && echo "$new" | grep -q "errata: vb_split ON (always" && [ "$(num '[0-9]* vkCmdBindVertexBuffers2 call(s) split')" -ge 2 ]
step $? "vb_split on: $res; $(lg 'vb_split: [0-9]* vkCmdBindVertexBuffers2 call(s) split into [0-9]*')"
run VKBRIDGE_ERRATA=-vb_split VKBRIDGE_ERRATA_IMITATE=vb_split -- vbstride_check
[ $rc != 0 ] && [ "$(nfail)" = 1 ] && grep -q "^FAIL binding 0 alone, then one call from binding 1" $work/run.log &&
  grep -q "^shape: strides wrong in multi-binding calls only" $work/run.log
step $? "the bug imitated, fix off: $(grep -o 'strides seen [0-9 /]*' $work/run.log | head -1)wanted 16 / 32 / 8; $(grep '^shape' $work/run.log)"
run VKBRIDGE_ERRATA_IMITATE=vb_split -- vbstride_check
[ $rc = 0 ] && echo "$new" | grep -q "; 0 imitated wrong" && grep -q "^shape: strides right" $work/run.log
step $? "the bug imitated, fix on: $res; $(lg 'split into [0-9]*; [0-9]* imitated wrong')"
run VKBRIDGE_ERRATA_IMITATE=vb_split VKBRIDGE_ERRATA_IMITATE_VB=single -- vbstride_check
[ $rc != 0 ] && grep -q "^FAIL single-binding calls" $work/run.log && grep -q "^shape: strides wrong in single-binding calls too" $work/run.log
step $? "the bug imitated on single calls too, fix on: the split cannot help; $(grep '^shape' $work/run.log | cut -c1-60)..."
# vulkaninfo lists the extensions before it makes its devices: one run shows the setting, as
# vkbQueryServer reads <cache dir>/env again when it changed (two runs were needed while only
# vkCreateDevice read it)
vi_ext() {
  run "$@" -- "$(command -v vulkaninfo)"
  ext=$(grep -c "VK_EXT_vertex_input_dynamic_state" $work/run.log)
}
vi_ext VKBRIDGE_FAKE_DRIVER_VERSION=r44p1; a=$ext; hidden=$(lg 'hidden from clients (.*below r48)')
vi_ext VKBRIDGE_FAKE_DRIVER_VERSION=v1.r49p1-03bet0; b=$ext
vi_ext VKBRIDGE_FAKE_DRIVER_VERSION=r44p1 VKBRIDGE_ERRATA=-vb_split; c=$ext
[ "$a" = 0 ] && [ -n "$hidden" ] && [ "$b" -ge 1 ] && [ "$c" -ge 1 ]
step $? "VK_EXT_vertex_input_dynamic_state seen by vulkaninfo: r44p1 $a ($hidden), r49p1 $b, r44p1 with -vb_split $c"

# dyn_reemit by driver (5b, 5c; after the vulkaninfo step: its "hidden from clients" line is logged once per server for r44p1)
off_ok() { [ $rc = 0 ] && echo "$new" | grep -q "errata: device" && ! echo "$new" | grep -q "errata: dyn_reemit ON" && ! echo "$new" | grep -q "errata: dyn_reemit:"; }
run VKBRIDGE_ERRATA=+example VKBRIDGE_ERRATA_PROBE=dyn_state:pass -- dynstate_check
off_ok; a=$?; ra=$res
run VKBRIDGE_ERRATA=+example VKBRIDGE_ERRATA_PROBE=dyn_state:pass VKBRIDGE_FAKE_DRIVER_VERSION=r44p1 -- dynstate_check
off_ok; b=$?
run VKBRIDGE_ERRATA=+example VKBRIDGE_ERRATA_PROBE=dyn_state:pass VKBRIDGE_FAKE_DRIVER_VERSION=r54p1 -- dynstate_check
off_ok; c=$?
run VKBRIDGE_ERRATA=+example VKBRIDGE_ERRATA_PROBE=dyn_state:pass VKBRIDGE_FAKE_DRIVER_VERSION=r44p0 -- dynstate_check
[ $a = 0 ] && [ $b = 0 ] && [ $c = 0 ] && [ $rc = 0 ] && echo "$new" | grep -q "errata: dyn_reemit ON (r44p0 in r0p0-r44p0 .*; safe mode; probe dyn_state passed)"
step $? "safe mode, probe passed: off on this driver ($ra), r44p1 ($b), r54p1 ($c); on for r44p0: $(lg 'errata: dyn_reemit ON ([^)]*)' | cut -c1-90)"
b49=VKBRIDGE_FAKE_DRIVER_VERSION=v1.r49p1-03bet0
run VKBRIDGE_ERRATA=+example VKBRIDGE_ERRATA_PROBE=dyn_state:pass $b49 VKBRIDGE_ERRATA_IMITATE=dyn_reemit VKBRIDGE_ERRATA_IMITATE_DYN=eds3 -- dynstate_check
narrow=$(num '[0-9]* value(s), [0-9]* vertex'); nvb=$(echo "$new" | grep -o '[0-9]* vertex buffer binding(s)' | tail -1 | grep -o '^[0-9]*')
[ $rc = 0 ] && echo "$new" | grep -q "errata: dyn_reemit ON (r49p1 in r48p0-r53 .*; the EDS3 states only; safe mode" &&
  [ "${narrow:-x}" -lt "${full:-0}" ] && [ "$nvb" = 0 ] && { [ "$eds3" = 0 ] || [ "$narrow" -ge 1 ]; }
step $? "r49p1 beta (Arm 3893903/3893905 only), EDS3 shape imitated: $res; values sent again $narrow (whole fix: $full), vertex buffer bindings ${nvb:-?}"
run VKBRIDGE_ERRATA=+example VKBRIDGE_ERRATA_PROBE=dyn_state:pass $b49 VKBRIDGE_ERRATA_IMITATE=dyn_reemit -- dynstate_check
[ $rc != 0 ] && grep -q "^FAIL depth write OFF across a bind" $work/run.log && grep -q "^FAIL vertex stride 16 across a bind" $work/run.log &&
  { [ "$eds3" = 0 ] || grep -q "^PASS sample mask 0 across a bind" $work/run.log; }
step $? "r49p1 beta with the r38-r44 shape imitated instead: $(nfail) case(s) fail, the EDS3 ones pass (the scope is real)"
run VKBRIDGE_ERRATA=+example VKBRIDGE_ERRATA_PROBE=dyn_state:pass VKBRIDGE_FAKE_DRIVER_VERSION=r49p1:jm VKBRIDGE_ERRATA_IMITATE=dyn_reemit -- dynstate_check
[ $rc = 0 ] && echo "$new" | grep -q "errata: dyn_reemit ON (r49p1 in" && ! echo "$new" | grep -q "the EDS3 states only"
step $? "r49p1 on a job manager GPU (ANGLE's r46-r51 JM range too): the whole fix, $res"

# both
run $NV VKBRIDGE_ERRATA_IMITATE=vb_split,dyn_reemit -- dynstate_check
r1=$rc; s1=$res
run VKBRIDGE_ERRATA_IMITATE=vb_split,dyn_reemit -- vbstride_check
[ $r1 = 0 ] && [ $rc = 0 ] && echo "$new" | grep -q "; 0 imitated wrong"
step $? "both bugs imitated, both fixes on: dynstate_check $s1; vbstride_check $res"
run $NV VKBRIDGE_FAKE_MISSING=divisor VKBRIDGE_ERRATA_IMITATE=vb_split,dyn_reemit -- divisor_check
r1=$rc; s1=$res
run $NV VKBRIDGE_ERRATA_IMITATE=vb_split,dyn_reemit -- vkbridge-smoke
[ $r1 = 0 ] && [ $rc = 0 ]
step $? "both imitated and fixed: divisor_check with the divisor emulated ($s1), smoke ($res)"

# FAKE_MISSING is taken once per server process: a server of its own, the env written first
printf '%s\n' VKBRIDGE_FAKE_MISSING=maint5,divisor,fillmode VKBRIDGE_FAKE_DRIVER_VERSION=r44p1 > "$cache/env"
start
w=VKBRIDGE_FAKE_MISSING=maint5,divisor,fillmode; v=VKBRIDGE_FAKE_DRIVER_VERSION=r44p1
run $w $v $NV -- dynstate_check
r1=$rc; s1=$res; ws=$(grep -c "^PASS vertex stride 16 across a bind, size VK_WHOLE_SIZE" $work/run.log); nw=$(echo "$new" | grep -c "VK_WHOLE_SIZE vertex buffer size reached"); on=$(echo "$new" | grep -c "errata: dyn_reemit ON (probe dyn_state not passed")
run $w $v $NV -- vbstride_check
r2=$rc; nw=$((nw + $(echo "$new" | grep -c "VK_WHOLE_SIZE vertex buffer size reached")))
run $w $v $NV -- divisor_check
r3=$rc; nw=$((nw + $(echo "$new" | grep -c "VK_WHOLE_SIZE vertex buffer size reached")))
run $w $v $NV -- polygon_check
r4=$rc; s4=$res; nw=$((nw + $(echo "$new" | grep -c "VK_WHOLE_SIZE vertex buffer size reached")))
pst=$(grep -c "^PASS LINE, vkCmdDraw, dynamic vertex stride" $work/run.log)
[ $r1 = 0 ] && [ $r2 = 0 ] && [ $r3 = 0 ] && [ $r4 = 0 ] && [ "$ws" = 1 ] && [ "$pst" = 1 ] && [ "$nw" = 0 ] &&
  [ "$on" -ge 1 ] && echo "$new" | grep -q "polygon mode LINE drawn as lines (first"
step $? "no maintenance5 + divisor + fill mode emulated, r44p1: dynstate_check $s1 (VK_WHOLE_SIZE case $ws), vbstride $r2, divisor $r3, polygon_check $s4 (dynamic stride $pst); $nw VK_WHOLE_SIZE at the driver"

grep -q "^#define VKB_PROTOCOL_HASH 0xe88f0e43c2b566b9ULL" ../common/gen/vkb_gen.h
step $? "protocol hash $(grep -o '0x[0-9a-f]*' ../common/gen/vkb_gen.h | head -1)"

if [ $fails = 0 ]; then echo "OK: $steps steps"; else echo "FAILED: $fails of $steps steps"; exit 1; fi
