#!/bin/bash
# bitwise_scalar (MK-03, server_errata_bitwise.c + spirv_bitwise.c) on the desktop driver, with a
# server of its own (its own socket and cache directory; the switches go in <cache dir>/env):
#   1  bitwise_check through the bridge as it is (not an Arm GPU: the fix is off, nothing rewritten);
#   2  the bug imitated beneath (VKBRIDGE_ERRATA_IMITATE=bitwise_scalar), the fix off: wrong values;
#   3  the imitation with the fix over it: right values; the module logged once, under the hash
#      of the app's SPIR-V, with its op count; the imitation finds nothing left to break;
#   4  the same with the module inline in the pipeline's stage (maintenance5);
#   5  the fix alone, from the version gate (VKBRIDGE_FAKE_DRIVER_VERSION=r44p1, safe mode): right
#      values; modules without the pattern (vkbridge-smoke's) are not touched;
#   6  layers beneath name the module by the app's code: the hang detector's dump-spirv/h-<hash>
#      is the app's hash, holding the code the driver got;
#   7  the pass over the corpus (tests/corpus, the check's own shader, and every *.spv under the
#      directories in BITWISE_CORPUS_DIRS, colon-separated): each rewritten module spirv-val clean
#      (when its input is), deterministic, with nothing left for the pass or the imitation; the
#      share of modules that hold the pattern printed;
#   8  the protocol hash is unchanged (server-side only).
# spirv-as/spirv-val from PATH, else from a docker image with Ubuntu's spirv-tools (built on first
# use, as tests/pervertex_check.sh does). Prints one line per step and "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
work=$(mktemp -d)
cache=$work/c
mkdir -p "$cache"
sock=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-bitwise-$$.sock
log=$work/server.log
srv=
start() {
  [ -n "$srv" ] && kill $srv 2>/dev/null && wait $srv 2>/dev/null
  rm -f "$sock"
  env -u VKBRIDGE_FAKE_MISSING -u VKBRIDGE_HIDE_EXTS -u VKBRIDGE_ERRATA -u VKBRIDGE_ERRATA_IMITATE -u VKBRIDGE_MODE \
      -u VKBRIDGE_FAKE_DRIVER_VERSION -u VKBRIDGE_ERRATA_PROBE -u VKBRIDGE_BITWISE_ALL -u VKBRIDGE_HANG_DETECT \
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
total() { echo "$new" | grep -o 'bitwise_scalar: device [0-9]*: [0-9]* of [0-9]* module(s) rewritten ([0-9]* op(s)' | tail -1; }

# The app's hash of the check's module (FNV-1a over its bytes, the h-<hash> names)
app_hash=$(python3 -c '
import re, struct, sys
w = [int(x, 16) for x in re.findall(r"0x[0-9a-fA-F]+", open(sys.argv[1]).read())]
h = 0xcbf29ce484222325
for b in struct.pack("<%dI" % len(w), *w): h = ((h ^ b) * 0x100000001b3) & 0xffffffffffffffff
print("%016x" % h)' ../tests/bitwise_comp.inc)

start
# 1
run "$out/bitwise_check" -- VKBRIDGE_MODE=safe
[ $rc = 0 ] && ! echo "$new" | grep -q "bitwise_scalar:" && echo "$new" | grep -q "errata: device .*: on [^;]*; off .*bitwise_scalar"
step $? "as it is, not an Arm GPU: off, untouched ($res)"
# 2
run "$out/bitwise_check" -- VKBRIDGE_ERRATA_IMITATE=bitwise_scalar
[ $rc != 0 ] && echo "$res" | grep -q "^FAIL: [0-9]* of 21 values wrong" && echo "$new" | grep -q "errata: bitwise_scalar bug imitated beneath the fix" &&
  echo "$new" | grep -q "the imitation broke 1 module"
step $? "the bug imitated, no fix: $res"
# 3
run "$out/bitwise_check" -- VKBRIDGE_ERRATA_IMITATE=bitwise_scalar VKBRIDGE_ERRATA=+bitwise_scalar
line=$(echo "$new" | grep "errata: bitwise_scalar: module h-")
[ $rc = 0 ] && [ "$(echo "$line" | grep -c .)" = 1 ] && echo "$line" | grep -q "module h-$app_hash: 9 op(s) scalarised (and/or/xor 6, not 1, shift 2" &&
  echo "$new" | grep -q "the imitation broke 0 module" && total | grep -q ": 1 of 1 module(s) rewritten (9 op"
step $? "the imitation with the fix over it: $res; $(echo "$line" | grep -o 'module h-[0-9a-f]*: [0-9]* op(s)')"
# 4
run "$out/bitwise_check" --inline -- VKBRIDGE_ERRATA_IMITATE=bitwise_scalar
r4a=$rc res4a=$res
run "$out/bitwise_check" --inline -- VKBRIDGE_ERRATA_IMITATE=bitwise_scalar VKBRIDGE_ERRATA=+bitwise_scalar
if echo "$res" | grep -q "^SKIP"; then
  step 0 "inline module: $res"
else
  # (the module is logged once per server: step 3 did; the device total counts it)
  [ $r4a != 0 ] && [ $rc = 0 ] && total | grep -q ": 1 of 1 module(s) rewritten (9 op"
  step $? "inline module: imitated $res4a; with the fix $res"
fi
# 5
run "$out/bitwise_check" -- VKBRIDGE_FAKE_DRIVER_VERSION=r44p1 VKBRIDGE_MODE=safe
r5=$rc res5=$res
echo "$new" | grep -q "errata: bitwise_scalar ON (r44p1 in r18p0-r53"
r5b=$?
run "$out/vkbridge-smoke" -- VKBRIDGE_FAKE_DRIVER_VERSION=r44p1 VKBRIDGE_MODE=safe
[ $r5 = 0 ] && [ $r5b = 0 ] && [ $rc = 0 ] && total | grep -q ": 0 of [1-9][0-9]* module(s) rewritten" && ! echo "$new" | grep -q "bitwise_scalar: module h-"
step $? "the fix from the version gate: $res5; vkbridge-smoke's modules untouched ($(total | grep -o '[0-9]* of [0-9]* module(s) rewritten'))"
# 6
mkdir -p "$cache/dump-spirv"
run "$out/bitwise_check" -- VKBRIDGE_ERRATA=+bitwise_scalar VKBRIDGE_HANG_DETECT=1
dumped=$cache/dump-spirv/h-$app_hash.spv
inc_bytes=$(( $(grep -o '0x[0-9a-fA-F]*' ../tests/bitwise_comp.inc | wc -l) * 4 ))
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
  IFS=: read -ra extra <<< "${BITWISE_CORPUS_DIRS:-}"
  for d in "${extra[@]}"; do [ -d "$d" ] && run_t+=" -v $d:$d:ro"; done
  run_t+=" vkb-spirv-tools"
fi
mkdir -p "$work/in" "$work/rw"
python3 -c '
import re, struct, sys
w = [int(x, 16) for x in re.findall(r"0x[0-9a-fA-F]+", open(sys.argv[1]).read())]
open(sys.argv[2], "wb").write(struct.pack("<%dI" % len(w), *w))' ../tests/bitwise_comp.inc "$work/in/bitwise_comp.spv"
cp ../tests/corpus/*.spv "$work/in/"
$run_t bash -c "for s in $PWD/../tests/corpus/*.spvasm; do spirv-as --target-env vulkan1.1 -o $work/in/\$(basename \$s .spvasm).spv \$s; done"
# the extra directories' modules under names of their own (the same file name in two places)
IFS=: read -ra extra <<< "${BITWISE_CORPUS_DIRS:-}"
k=0
for d in "${extra[@]}"; do [ -d "$d" ] && find "$d" -name '*.spv' -type f; done | while read -r f; do
  k=$((k+1)); ln -s "$f" "$work/in/x$k-${f##*/}"
done
ls "$work"/in/*.spv > "$work/in.list"
"$out/bitwise_corpus" -q "$work/rw" @"$work/in.list" > "$work/corpus.log"
rc=$?
grep "FAIL\|not rewritten" "$work/corpus.log" | head -5
# Each rewritten module against its input: a failure after the rewrite is the rewrite's
(cd "$work/rw" && ls) | sed "s|^|$work/in/|" > "$work/pairs.list"
val=$($run_t bash -c "ok=0 bad=0 skip=0; while read f; do b=\${f##*/}; if ! spirv-val --target-env vulkan1.3 \"\$f\" >/dev/null 2>&1; then skip=\$((skip+1)); elif spirv-val --target-env vulkan1.3 $work/rw/\$b > $work/val.err 2>&1; then ok=\$((ok+1)); else bad=\$((bad+1)); echo \"invalid after the rewrite: \$b: \$(head -1 $work/val.err)\" >&2; fi; done; echo \$ok \$bad \$skip" < "$work/pairs.list")
read -r vok vbad vskip <<< "$val"
grep "^TOTAL" "$work/corpus.log" | sed 's/^/    /'
[ $rc = 0 ] && [ "${vbad:-1}" = 0 ] && [ "${vok:-0}" -ge 1 ]
step $? "corpus: $(grep -o '[0-9]* modules, .* rewritten ([0-9.]*%)' "$work/corpus.log" | head -1); spirv-val: ${vok:-?} clean, ${vbad:-?} invalid, ${vskip:-?} with an invalid input"

# 8
grep -q "^#define VKB_PROTOCOL_HASH 0x4a9da939fdf65896ULL" ../common/gen/vkb_gen.h
step $? "protocol hash $(grep -o '0x[0-9a-f]*' ../common/gen/vkb_gen.h | head -1)"

if [ $fails = 0 ]; then echo "OK: $steps steps"; else echo "FAILED: $fails of $steps steps"; exit 1; fi
