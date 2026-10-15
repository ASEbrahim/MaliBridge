#!/bin/bash
# clamp_zero (MK-32, server_errata_clampzero.c + spirv_clampzero.c) on the desktop driver, with a
# server of its own (its own socket and cache directory; the switches go in <cache dir>/env):
#   1  clampzero_check through the bridge as it is (not an Arm GPU: the fix is off, nothing rewritten);
#   2  the bug imitated beneath (VKBRIDGE_ERRATA_IMITATE=clamp_zero), the fix off: wrong values;
#   3  the imitation with the fix from the version gate (VKBRIDGE_FAKE_DRIVER_VERSION=r44p1, safe
#      mode) over it: right values; the module logged once, under the hash of the app's SPIR-V,
#      with its op count (the clamp of a push constant is not the pattern); nothing left to break;
#   4  the same with the module inline in the pipeline's stage (maintenance5);
#   5  forced on (VKBRIDGE_ERRATA=+clamp_zero): the wide form, every zero limit (the push
#      constant's clamp too); with VKBRIDGE_CLAMP_ZERO_BFI=1 the BitFieldInsert zero; both right
#      over the imitation; vkbridge-smoke's modules without a zero limit untouched;
#   6  layers beneath name the module by the app's code: the hang detector's dump-spirv/h-<hash>
#      is the app's hash, holding the code the driver got;
#   7  the pass over the corpus (tests/corpus, the check's own shader, and every *.spv under the
#      directories in CLAMPZERO_CORPUS_DIRS, colon-separated), narrow and wide: each rewritten
#      module spirv-val clean (when its input is), deterministic, with nothing left for the pass or
#      the imitation; the share of modules that hold the pattern printed;
#   8  the protocol hash is unchanged (server-side only).
# spirv-as/spirv-val from PATH, else from a docker image with Ubuntu's spirv-tools (built on first
# use, as tests/pervertex_check.sh does). Prints one line per step and "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
work=$(mktemp -d)
cache=$work/c
mkdir -p "$cache"
sock=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-clampzero-$$.sock
log=$work/server.log
srv=
start() {
  [ -n "$srv" ] && kill $srv 2>/dev/null && wait $srv 2>/dev/null
  rm -f "$sock"
  env -u VKBRIDGE_FAKE_MISSING -u VKBRIDGE_HIDE_EXTS -u VKBRIDGE_ERRATA -u VKBRIDGE_ERRATA_IMITATE -u VKBRIDGE_MODE \
      -u VKBRIDGE_FAKE_DRIVER_VERSION -u VKBRIDGE_ERRATA_PROBE -u VKBRIDGE_CLAMP_ZERO_ALL -u VKBRIDGE_CLAMP_ZERO_BFI -u VKBRIDGE_HANG_DETECT \
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

# One run of a program with <cache dir>/env = the arguments after --; $new: its log lines, $res its
# last output line, $rc its exit status
run() {
  local prog=("$1"); shift
  while [ "$1" != -- ]; do prog+=("$1"); shift; done; shift
  printf '%s\n' "$@" > "$cache/env"
  local l; l=$(wc -l < "$log")
  timeout 120 "${prog[@]}" >$work/run.log 2>&1
  rc=$?
  res=$(tail -1 $work/run.log)
  new=$(tail -n +$((l+1)) "$log")
}
total() { echo "$new" | grep -o 'clamp_zero: device [0-9]*: [0-9]* of [0-9]* module(s) rewritten ([0-9]* op(s)' | tail -1; }

# The app's hash of the check's module (FNV-1a over its bytes, the h-<hash> names)
app_hash=$(python3 -c '
import re, struct, sys
w = [int(x, 16) for x in re.findall(r"0x[0-9a-fA-F]+", open(sys.argv[1]).read())]
h = 0xcbf29ce484222325
for b in struct.pack("<%dI" % len(w), *w): h = ((h ^ b) * 0x100000001b3) & 0xffffffffffffffff
print("%016x" % h)' ../tests/clampzero_comp.inc)

start
# 1
run "$out/clampzero_check" -- VKBRIDGE_MODE=safe
[ $rc = 0 ] && ! echo "$new" | grep -q "clamp_zero:" && echo "$new" | grep -q "errata: device .*: on [^;]*; off .*clamp_zero"
step $? "as it is, not an Arm GPU: off, untouched ($res)"
# 2
run "$out/clampzero_check" -- VKBRIDGE_ERRATA_IMITATE=clamp_zero
[ $rc != 0 ] && echo "$res" | grep -q "^FAIL: [0-9]* of 33 values wrong" && echo "$new" | grep -q "errata: clamp_zero bug imitated beneath the fix" &&
  echo "$new" | grep -q "the imitation broke 1 module"
step $? "the bug imitated, no fix: $res"
# 3
run "$out/clampzero_check" -- VKBRIDGE_ERRATA_IMITATE=clamp_zero VKBRIDGE_FAKE_DRIVER_VERSION=r44p1 VKBRIDGE_MODE=safe
line=$(echo "$new" | grep "errata: clamp_zero: module h-")
[ $rc = 0 ] && echo "$new" | grep -q "errata: clamp_zero ON (r44p1 in r38p1-r49p0; safe mode) - MK-32" &&
  echo "$new" | grep -q "clamp_zero gives min/max/clamp of constant-selected values an opaque zero (a Private variable)" &&
  [ "$(echo "$line" | grep -c .)" = 1 ] && echo "$line" | grep -q "module h-$app_hash: 9 op(s) given an opaque zero (clamp 6, min/max 3; value by select 5, phi 0, memory 4; Private variable)" &&
  echo "$new" | grep -q "the imitation broke 0 module" && total | grep -q ": 1 of 1 module(s) rewritten (9 op"
step $? "the imitation with the fix from the version gate over it: $res; $(echo "$line" | grep -o 'module h-[0-9a-f]*: [0-9]* op(s)')"
# 4
run "$out/clampzero_check" --inline -- VKBRIDGE_ERRATA_IMITATE=clamp_zero
r4a=$rc res4a=$res
run "$out/clampzero_check" --inline -- VKBRIDGE_ERRATA_IMITATE=clamp_zero VKBRIDGE_FAKE_DRIVER_VERSION=r44p1
if echo "$res" | grep -q "^SKIP"; then
  step 0 "inline module: $res"
else
  # (the module is logged once per server: step 3 did; the device total counts it)
  [ $r4a != 0 ] && [ $rc = 0 ] && total | grep -q ": 1 of 1 module(s) rewritten (9 op"
  step $? "inline module: imitated $res4a; with the fix $res"
fi
# 5
run "$out/clampzero_check" -- VKBRIDGE_ERRATA_IMITATE=clamp_zero VKBRIDGE_ERRATA=+clamp_zero
r5=$rc res5=$res
echo "$new" | grep -q "errata: clamp_zero ON (VKBRIDGE_ERRATA=+clamp_zero) - MK-32" &&
  echo "$new" | grep -q "clamp_zero gives every min/max/clamp with a constant zero limit an opaque zero (a Private variable) - forced on, the wide form" &&
  total | grep -q ": 1 of 1 module(s) rewritten (10 op"
r5b=$?
run "$out/clampzero_check" -- VKBRIDGE_ERRATA_IMITATE=clamp_zero VKBRIDGE_ERRATA=+clamp_zero VKBRIDGE_CLAMP_ZERO_BFI=1
r5c=$rc res5c=$res
echo "$new" | grep -q "an opaque zero (OpBitFieldInsert, VKBRIDGE_CLAMP_ZERO_BFI)" && echo "$new" | grep -q "the imitation broke 0 module" &&
  total | grep -q ": 1 of 1 module(s) rewritten (10 op"
r5d=$?
run "$out/vkbridge-smoke" -- VKBRIDGE_ERRATA=+clamp_zero
[ $r5 = 0 ] && [ $r5b = 0 ] && [ $r5c = 0 ] && [ $r5d = 0 ] && [ $rc = 0 ] && total | grep -q ": [0-9]* of [1-9][0-9]* module(s) rewritten"
step $? "forced on, the wide form: $res5 (10 ops); BitFieldInsert zero: $res5c; vkbridge-smoke: $res ($(total | grep -o '[0-9]* of [0-9]* module(s) rewritten'))"
# 6
mkdir -p "$cache/dump-spirv"
run "$out/clampzero_check" -- VKBRIDGE_ERRATA=+clamp_zero VKBRIDGE_HANG_DETECT=1
dumped=$cache/dump-spirv/h-$app_hash.spv
inc_bytes=$(( $(grep -o '0x[0-9a-fA-F]*' ../tests/clampzero_comp.inc | wc -l) * 4 ))
[ $rc = 0 ] && [ -f "$dumped" ] && [ "$(stat -c %s "$dumped")" -gt "$inc_bytes" ] &&
  [ "$(ls "$cache/dump-spirv" | grep -c '^h-')" = 1 ]
step $? "the hang detector names the module h-$app_hash (the app's), its dump the driver's code ($(stat -c %s "$dumped" 2>/dev/null) bytes, the app's $inc_bytes)"
rm -rf "$cache/dump-spirv"

# 7 the corpus
if command -v spirv-val >/dev/null; then
  run_t=
else
  docker image inspect vkb-spirv-tools >/dev/null 2>&1 ||
    printf 'FROM ubuntu:24.04\nRUN apt-get update && apt-get install -y --no-install-recommends spirv-tools\n' |
    docker build -q -t vkb-spirv-tools - >/dev/null
  run_t="docker run --rm -i -u $(id -u):$(id -g) -v $work:$work -v $PWD/..:$PWD/..:ro"
  IFS=: read -ra extra <<< "${CLAMPZERO_CORPUS_DIRS:-}"
  for d in "${extra[@]}"; do [ -d "$d" ] && run_t+=" -v $d:$d:ro"; done
  run_t+=" vkb-spirv-tools"
fi
mkdir -p "$work/in" "$work/rw"
python3 -c '
import re, struct, sys
w = [int(x, 16) for x in re.findall(r"0x[0-9a-fA-F]+", open(sys.argv[1]).read())]
open(sys.argv[2], "wb").write(struct.pack("<%dI" % len(w), *w))' ../tests/clampzero_comp.inc "$work/in/clampzero_comp.spv"
cp ../tests/corpus/*.spv "$work/in/"
$run_t bash -c "for s in $PWD/../tests/corpus/*.spvasm; do spirv-as --target-env vulkan1.1 -o $work/in/\$(basename \$s .spvasm).spv \$s; done"
# the extra directories' modules under names of their own (the same file name in two places)
IFS=: read -ra extra <<< "${CLAMPZERO_CORPUS_DIRS:-}"
k=0
for d in "${extra[@]}"; do [ -d "$d" ] && find "$d" -name '*.spv' -type f; done | while read -r f; do
  k=$((k+1)); ln -s "$f" "$work/in/x$k-${f##*/}"
done
ls "$work"/in/*.spv > "$work/in.list"
# narrow (the version gate's form) and wide (forced on), each against its input: a failure after
# the rewrite is the rewrite's
for form in narrow wide; do
  flag=; [ $form = wide ] && flag=-a
  rm -rf "$work/rw"; mkdir -p "$work/rw"
  "$out/clampzero_corpus" -q $flag "$work/rw" @"$work/in.list" > "$work/corpus.log"
  rc=$?
  grep "FAIL\|not rewritten" "$work/corpus.log" | head -5
  (cd "$work/rw" && ls) | sed "s|^|$work/in/|" > "$work/pairs.list"
  val=$($run_t bash -c "ok=0 bad=0 skip=0; while read f; do b=\${f##*/}; if ! spirv-val --target-env vulkan1.3 \"\$f\" >/dev/null 2>&1; then skip=\$((skip+1)); elif spirv-val --target-env vulkan1.3 $work/rw/\$b > $work/val.err 2>&1; then ok=\$((ok+1)); else bad=\$((bad+1)); echo \"invalid after the rewrite: \$b: \$(head -1 $work/val.err)\" >&2; fi; done; echo \$ok \$bad \$skip" < "$work/pairs.list")
  read -r vok vbad vskip <<< "$val"
  grep "^TOTAL" "$work/corpus.log" | sed 's/^/    /'
  [ $rc = 0 ] && [ "${vbad:-1}" = 0 ] && [ "${vok:-0}" -ge 1 ]
  step $? "corpus, $form: $(grep -o '[0-9]* modules, .* rewritten ([0-9.]*%)' "$work/corpus.log" | head -1); spirv-val: ${vok:-?} clean, ${vbad:-?} invalid, ${vskip:-?} with an invalid input"
done

# 8
grep -q "^#define VKB_PROTOCOL_HASH 0x4a9da939fdf65896ULL" ../common/gen/vkb_gen.h
step $? "protocol hash $(grep -o '0x[0-9a-f]*' ../common/gen/vkb_gen.h | head -1)"

if [ $fails = 0 ]; then echo "OK: $steps steps"; else echo "FAILED: $fails of $steps steps"; exit 1; fi
