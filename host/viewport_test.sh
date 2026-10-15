#!/bin/bash
# viewport_flip (MK-02, server_errata_flip.c + spirv_flip.c) on the desktop driver, with a server of
# its own (its own socket and cache directory; the switches go in <cache dir>/env, re-read at each
# device). viewport_check draws under negative and positive viewports - static, dynamic, with
# count, two viewports, points, a geometry stage, a depth range - and compares each picture with
# the CPU's (facing and culling, gl_FragCoord.y/.z, the sign of dFdy, the app's y, gl_PointCoord):
#   1  as it is: off (the startup probe passes on a correct driver), every case passes;
#   2  the bug imitated beneath, the fix off: every case with a negative viewport fails (the
#      picture upside down, facing and culling reversed), the two positive-only ones pass;
#   3  the bug imitated, the fix forced on (+viewport_flip): every case passes, nothing reaches
#      the imitation, the dynamic pipeline drawn positive got a variant at draw time;
#   4  the fix forced on the desktop driver (correct): every case passes - forcing it is
#      invisible, the test of it on our tablets; also with no variants kept (each pipeline draws
#      with its own mask, its viewports mirrored to match); the shaders inline (maintenance5);
#   5  the gate: a failed viewport_flip probe turns it on in safe and in performance mode (and
#      fixes the imitated bug), a passed one or none leaves it off - the G715 range alone too;
#   6  d3d11_check through DXVK (when VKB_WINE_DIR is set): fails with the bug imitated, passes with
#      the fix over it and with the fix forced; the other checks with the fix forced on: smoke, divisor_check (its own variant pipelines),
#      gsfold_check, recycle_check, nulldesc_check, view3d_check; smoke with the bug imitated too;
#   7  the cost: viewport_check --bench (dynamic-viewport pipelines made, drawn negative, then
#      positive: one variant each) with the fix off and on, the server's totals;
#   8  the pass over the corpus (tests/corpus, the checks' own shaders, *.spv under
#      FLIP_CORPUS_DIRS): every flipped module spirv-val clean, deterministic;
#   9  the protocol hash is unchanged (server-side only).
# spirv-as/spirv-val from PATH, else from the vkb-spirv-tools docker image (bitwise_test.sh's).
# Prints one line per step and "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
work=$(mktemp -d)
cache=$work/c
mkdir -p "$cache"
sock=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-viewport-$$.sock
log=$work/server.log
srv=
start() {
  [ -n "$srv" ] && kill $srv 2>/dev/null && wait $srv 2>/dev/null
  rm -f "$sock"
  env -u VKBRIDGE_FAKE_MISSING -u VKBRIDGE_HIDE_EXTS -u VKBRIDGE_ERRATA -u VKBRIDGE_ERRATA_IMITATE -u VKBRIDGE_MODE \
      -u VKBRIDGE_FAKE_DRIVER_VERSION -u VKBRIDGE_ERRATA_PROBE -u VKBRIDGE_FLIP_RETAIN_MB "$@" \
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
  timeout 300 "$out/$@" >$work/run.log 2>&1
  rc=$?
  res=$(tail -1 $work/run.log)
  sleep 0.2 # the device's last log lines
  new=$(tail -n +$((l+1)) "$log")
}
lg() { echo "$new" | grep -o "$1" | tail -1; }
npass() { grep -c '^PASS ' $work/run.log; }
nfail() { grep -c '^FAIL ' $work/run.log; }
# the cases with no negative viewport
positive_only='^PASS (static viewport \(0, 0, 32, 32\)|the same pipeline, dynamic viewport \(4, 2, 24, 26\))'

start
run -- viewport_check
total=$(npass)
[ $rc = 0 ] && [ "$total" -ge 12 ] && ! echo "$new" | grep -q "errata: viewport_flip ON" && echo "$new" | grep -q "errata: device .*off .*viewport_flip"
step $? "off (the probe passes on this driver): $res; $(head -1 $work/run.log | grep -o 'extended dynamic state.*')"
cp $work/run.log $work/plain.log

run VKBRIDGE_ERRATA_IMITATE=viewport_flip -- viewport_check
nf=$(nfail) np=$(npass) pos=$(grep -cE "$positive_only" $work/run.log)
[ $rc != 0 ] && [ "$np" = 2 ] && [ "$pos" = 2 ] && [ "$nf" = $((total - 2)) ] &&
  grep -q '^FAIL static viewport (0, 32, 32, -32), back faces culled: pixel' $work/run.log &&
  echo "$new" | grep -q "the imitation turned [1-9][0-9]* viewport(s) and [1-9][0-9]* pipeline(s) upside down"
step $? "the bug imitated, fix off: $nf of $total cases fail, the $np positive-only ones pass; $(lg 'the imitation turned [0-9]* viewport(s) and [0-9]* pipeline(s)')"
grep -A9 '^FAIL static viewport (0, 32, 32, -32), no culling' $work/run.log | sed 's/^/       /' | head -10

run VKBRIDGE_ERRATA=+viewport_flip VKBRIDGE_ERRATA_IMITATE=viewport_flip -- viewport_check
[ $rc = 0 ] && [ "$(npass)" = "$total" ] && echo "$new" | grep -q "errata: viewport_flip ON (VKBRIDGE_ERRATA=+viewport_flip[;)].* - MK-02" &&
  echo "$new" | grep -q "the imitation turned 0 viewport(s) and 0 pipeline(s) upside down" &&
  echo "$new" | grep -q "a variant for viewport mask 0x0 (the app's has 0x1) compiled at draw time"
step $? "the bug imitated, fix on: $res; $(lg 'device [0-9]*: [0-9]* pipeline(s) made.*')"
fixlog=$new

run VKBRIDGE_ERRATA=+viewport_flip -- viewport_check
[ $rc = 0 ] && [ "$(npass)" = "$total" ] && diff <(grep -E '^(PASS|FAIL|SKIP)' $work/plain.log) <(grep -E '^(PASS|FAIL|SKIP)' $work/run.log) >/dev/null
step $? "the fix forced on the desktop driver: $res, the same lines as without it"
# what the variants were: one for the positive dynamic draws, one for the mixed two-viewport one
echo "$fixlog" | grep -o 'pipeline 0x[0-9a-f]*: a variant .*' | sed 's/^/       /'

# the shaders chained to the stages (maintenance5): the flipped stage's inline code swapped
run VKBRIDGE_ERRATA=+viewport_flip VKBRIDGE_ERRATA_IMITATE=viewport_flip -- viewport_check --inline
[ $rc = 0 ] && { grep -q '^SKIP --inline' $work/run.log || [ "$(npass)" = "$total" ]; } &&
  echo "$new" | grep -q "the imitation turned 0 viewport(s) and 0 pipeline(s) upside down"
step $? "inline modules, the bug imitated, fix on: $res; $(lg '[0-9]* variant(s) compiled at draw time')"

# no variants (none kept): each pipeline draws with its own, its viewports mirrored to match
run VKBRIDGE_ERRATA=+viewport_flip VKBRIDGE_FLIP_RETAIN_MB=0 -- viewport_check
[ $rc = 0 ] && [ "$(npass)" = "$total" ] && echo "$new" | grep -q " 0 variant(s) compiled at draw time" &&
  echo "$new" | grep -q "new pipelines keep the viewport signs they were made for" && ! echo "$new" | grep -q " 0 re-sent before a draw"
step $? "the fix forced, no variants kept (VKBRIDGE_FLIP_RETAIN_MB=0): $res; $(lg '[0-9]* re-sent before a draw')"

# 5 the gate
for mode in safe performance; do
  run VKBRIDGE_MODE=$mode VKBRIDGE_ERRATA_PROBE=viewport_flip:fail VKBRIDGE_ERRATA_IMITATE=viewport_flip -- viewport_check
  [ $rc = 0 ] && echo "$new" | grep -q "errata: viewport_flip ON (probe viewport_flip failed; $mode mode"
  step $? "$mode mode, probe viewport_flip failed, the bug imitated: $res; $(lg 'errata: viewport_flip ON ([^)]*)')"
done
for mode in safe performance; do
  run VKBRIDGE_MODE=$mode VKBRIDGE_ERRATA_PROBE=viewport_flip:pass VKBRIDGE_FAKE_DRIVER_VERSION=r54p2 -- vkbridge-smoke
  [ $rc = 0 ] && ! echo "$new" | grep -q "errata: viewport_flip ON"
  step $? "$mode mode, probe viewport_flip passed (r54p2): off ($(lg 'errata: device .*: on [^;]*' | sed 's/.*: on /on /'))"
done
run VKBRIDGE_FAKE_DRIVER_VERSION=r54p2 -- vkbridge-smoke
[ $rc = 0 ] && ! echo "$new" | grep -q "errata: viewport_flip ON"
step $? "safe mode, r54p2, no probe result: off (a flip where it is not needed costs compiles)"

# 6 the other checks with the fix forced on
for t in vkbridge-smoke divisor_check gsfold_check recycle_check nulldesc_check view3d_check; do
  [ -x "$out/$t" ] || continue
  run VKBRIDGE_ERRATA=+viewport_flip -- $t
  [ $rc = 0 ] && echo "$new" | grep -q "errata: viewport_flip ON (VKBRIDGE_ERRATA=+viewport_flip[;)]"
  step $? "$t with +viewport_flip: $res; $(lg '[0-9]* pipeline(s) made ([^)]*), [0-9]* with a flipped stage')"
done
run VKBRIDGE_ERRATA=+viewport_flip VKBRIDGE_ERRATA_IMITATE=viewport_flip -- vkbridge-smoke
[ $rc = 0 ]
step $? "vkbridge-smoke, the bug imitated, fix on: $res; $(lg 'the imitation turned [0-9]* viewport(s) and [0-9]* pipeline(s)')"

# DXVK (graphics pipeline libraries on RADV: the pre-rasterization library flipped, the linked
# pipelines drawn with it), when the Proton prefix is there (VKB_WINE_DIR, see README)
if [ -n "${VKB_WINE_DIR:-}" ] && [ -d "$VKB_WINE_DIR/app" ]; then
  for sw in "VKBRIDGE_ERRATA_IMITATE=viewport_flip" "VKBRIDGE_ERRATA=+viewport_flip VKBRIDGE_ERRATA_IMITATE=viewport_flip" "VKBRIDGE_ERRATA=+viewport_flip"; do
    printf '%s\n' $sw > "$cache/env"
    ./d3d11_test.sh > $work/d3d.log 2>&1
    r=$(grep -E '^(OK|FAILED)' $work/d3d.log)
    case $sw in
      VKBRIDGE_ERRATA_IMITATE=*) echo "$r" | grep -q '^FAILED' ;;
      *) echo "$r" | grep -q '^OK' ;;
    esac
    step $? "d3d11_check via DXVK, $sw: $r"
  done
fi

# 7 the cost: the driver's shader cache off, so each pipeline compiles
start RADV_DEBUG=nocache MESA_SHADER_CACHE_DISABLE=true
for sw in -viewport_flip +viewport_flip; do
  run VKBRIDGE_ERRATA=$sw -- viewport_check --bench 300
  echo "       $sw: $(grep -o 'bench: .*' $work/run.log)"
  [ "$sw" = +viewport_flip ] && echo "       $(lg 'device [0-9]*: [0-9]* pipeline(s) made.*')"
  bench_rc=$((${bench_rc:-0} | rc))
done
[ "$bench_rc" = 0 ] && echo "$new" | grep -q "300 variant(s) compiled at draw time"
step $? "viewport_check --bench 300 with the fix off and on (300 variants at draw time)"
start

# 8 the corpus
if command -v spirv-val >/dev/null; then
  run_t=
else
  docker image inspect vkb-spirv-tools >/dev/null 2>&1 ||
    printf 'FROM ubuntu:24.04\nRUN apt-get update && apt-get install -y --no-install-recommends spirv-tools\n' |
    docker build -q -t vkb-spirv-tools - >/dev/null
  run_t="docker run --rm -i -u $(id -u):$(id -g) -v $work:$work -v $PWD/..:$PWD/..:ro"
  IFS=: read -ra extra <<< "${FLIP_CORPUS_DIRS:-}"
  for d in "${extra[@]}"; do [ -d "$d" ] && run_t+=" -v $d:$d:ro"; done
  run_t+=" vkb-spirv-tools"
fi
mkdir -p "$work/in" "$work/fl"
for inc in ../tests/viewport_*.inc ../tests/gsfold_*.inc ../tests/divisor_*.inc; do
  python3 -c '
import re, struct, sys
w = [int(x, 16) for x in re.findall(r"0x[0-9a-fA-F]+", open(sys.argv[1]).read())]
open(sys.argv[2], "wb").write(struct.pack("<%dI" % len(w), *w))' "$inc" "$work/in/$(basename "$inc" .inc).spv"
done
cp ../tests/corpus/*.spv "$work/in/"
$run_t bash -c "for s in $PWD/../tests/corpus/*.spvasm; do spirv-as --target-env vulkan1.1 -o $work/in/\$(basename \$s .spvasm).spv \$s; done"
IFS=: read -ra extra <<< "${FLIP_CORPUS_DIRS:-}"
k=0
for d in "${extra[@]}"; do [ -d "$d" ] && find "$d" -name '*.spv' -type f; done | while read -r f; do
  k=$((k+1)); ln -s "$f" "$work/in/x$k-${f##*/}"
done
ls "$work"/in/*.spv > "$work/in.list"
"$out/flip_corpus" "$work/fl" @"$work/in.list" > "$work/corpus.log"
crc=$?
grep "FAIL" "$work/corpus.log" | head -5
ls "$work"/fl/*.spv > "$work/fl.list" 2>/dev/null
val=$($run_t bash -c "ok=0 bad=0; while read f; do if spirv-val --target-env vulkan1.3 \"\$f\" > $work/val.err 2>&1; then ok=\$((ok+1)); else bad=\$((bad+1)); echo \"invalid: \${f##*/}: \$(head -1 $work/val.err)\" >&2; fi; done; echo \$ok \$bad" < "$work/fl.list")
read -r vok vbad <<< "$val"
grep -q "^flipped viewport_vert_mv.spv main (vertex): 1 site(s), gl_PerVertex, by ViewportIndex" "$work/corpus.log" &&
  grep -q "^flipped drg_dxvk_vs.spv main (vertex): .*loose gl_Position" "$work/corpus.log" &&
  grep -q "^flipped viewport_geom.spv main (geometry): 1 site(s)" "$work/corpus.log"
cases=$?
[ $crc = 0 ] && [ $cases = 0 ] && [ "${vbad:-1}" = 0 ] && [ "${vok:-0}" -gt 0 ]
step $? "$(tail -1 "$work/corpus.log"); spirv-val: ${vok:-?} clean, ${vbad:-?} invalid"
grep '^flipped' "$work/corpus.log" | sed 's/^/       /'

grep -q "^#define VKB_PROTOCOL_HASH 0x4a9da939fdf65896ULL" ../common/gen/vkb_gen.h
step $? "protocol hash $(grep -o '0x[0-9a-f]*' ../common/gen/vkb_gen.h | head -1)"

if [ $fails = 0 ]; then echo "OK: $steps steps"; else echo "FAILED: $fails of $steps steps"; exit 1; fi
