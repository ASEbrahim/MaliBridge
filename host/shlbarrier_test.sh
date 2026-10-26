#!/bin/bash
# shl_barrier (server_spvfix.c + spirv_shlbarrier.c) on the desktop driver, with a server of its
# own (its own socket and cache directory; the switches go in <cache dir>/env):
#   1  bitwise_check as it is (not an Arm GPU: shl_barrier off, nothing rewritten);
#   2  VKBRIDGE_SHL_BARRIER=1: right values; the check's module rewritten (its constant shift
#      barriered), the device's total logged; the other SPIR-V fixes stay off;
#   3  the same with the module inline in the pipeline's stage (maintenance5);
#   4  stacked with MK-03 (the bitwise bug imitated, bitwise_scalar over it): right values;
#   5  vkbridge-smoke with it on: 13 of 13, with some of its modules barriered and the rest untouched;
#   6  the hang detector names the module by the app's code (dump-spirv/h-<app hash>.spv), holding
#      the code the driver got;
#   7  the pass over the corpus (tests/shlbarrier/*.spvasm, tests/corpus, the check's own shader):
#      the expected barriers in shl_barrier's own modules (64-bit and non-constant shifts left
#      alone), each rewritten module spirv-val clean, deterministic, unchanged by a second pass.
# spirv-as/spirv-val from PATH, else from the vkb-spirv-tools docker image (built on first use).
# Prints one line per step and "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
work=$(mktemp -d)
cache=$work/c
mkdir -p "$cache"
sock=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-shlbarrier-$$.sock
log=$work/server.log
srv=
start() {
  [ -n "$srv" ] && kill $srv 2>/dev/null && wait $srv 2>/dev/null
  rm -f "$sock"
  env -u VKBRIDGE_FAKE_MISSING -u VKBRIDGE_HIDE_EXTS -u VKBRIDGE_ERRATA -u VKBRIDGE_ERRATA_IMITATE -u VKBRIDGE_MODE \
      -u VKBRIDGE_FAKE_DRIVER_VERSION -u VKBRIDGE_ERRATA_PROBE -u VKBRIDGE_SHL_BARRIER -u VKBRIDGE_SPIRV_FIX -u VKBRIDGE_HANG_DETECT \
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
total() { echo "$new" | grep -o 'shl_barrier: [0-9]* of [0-9]* module(s) rewritten, [0-9]* barrier(s)' | tail -1; }

app_hash=$(python3 -c '
import re, struct, sys
w = [int(x, 16) for x in re.findall(r"0x[0-9a-fA-F]+", open(sys.argv[1]).read())]
h = 0xcbf29ce484222325
for b in struct.pack("<%dI" % len(w), *w): h = ((h ^ b) * 0x100000001b3) & 0xffffffffffffffff
print("%016x" % h)' ../tests/bitwise_comp.inc)

start
# 1
run "$out/bitwise_check" --
[ $rc = 0 ] && ! echo "$new" | grep -q "shl_barrier:" && [ -z "$(total)" ]
step $? "as it is, not an Arm GPU: off, untouched ($res)"
# 2
run "$out/bitwise_check" -- VKBRIDGE_SHL_BARRIER=1
[ $rc = 0 ] && echo "$new" | grep -q "Mali SPIR-V fixes off, YCbCr compute samplers kept, shl_barrier on (VKBRIDGE_SHL_BARRIER)" &&
  echo "$new" | grep -q "shl_barrier: optimization barriers after constant left shifts" && total | grep -q ": 1 of 1 module(s) rewritten, [1-9]"
step $? "VKBRIDGE_SHL_BARRIER=1: $res; $(total)"
# 3
run "$out/bitwise_check" --inline -- VKBRIDGE_SHL_BARRIER=1
if echo "$res" | grep -q "^SKIP"; then step 0 "inline module: $res"
else [ $rc = 0 ] && total | grep -q ": 1 of 1 module(s) rewritten, [1-9]"; step $? "inline module: $res; $(total)"; fi
# 4
run "$out/bitwise_check" -- VKBRIDGE_SHL_BARRIER=1 VKBRIDGE_ERRATA_IMITATE=bitwise_scalar VKBRIDGE_ERRATA=+bitwise_scalar
[ $rc = 0 ] && total | grep -q ": 1 of 1 module(s) rewritten"
step $? "stacked with bitwise_scalar over its imitation: $res"
# 5
run "$out/vkbridge-smoke" -- VKBRIDGE_SHL_BARRIER=1
[ $rc = 0 ] && total | grep -qE ": [1-9][0-9]* of [1-9][0-9]* module\(s\) rewritten" &&
  t5=$(total | grep -o '[0-9]* of [0-9]*') && [ "${t5%% *}" -lt "${t5##* }" ]
step $? "vkbridge-smoke: $res; $(total)"
# 6
mkdir -p "$cache/dump-spirv"
run "$out/bitwise_check" -- VKBRIDGE_SHL_BARRIER=1 VKBRIDGE_HANG_DETECT=1
dumped=$cache/dump-spirv/h-$app_hash.spv
inc_bytes=$(( $(grep -o '0x[0-9a-fA-F]*' ../tests/bitwise_comp.inc | wc -l) * 4 ))
[ $rc = 0 ] && [ -f "$dumped" ] && [ "$(stat -c %s "$dumped")" -gt "$inc_bytes" ] && [ "$(ls "$cache/dump-spirv" | grep -c '^h-')" = 1 ]
step $? "the hang detector names the module h-$app_hash (the app's), its dump the driver's code ($(stat -c %s "$dumped" 2>/dev/null) bytes, the app's $inc_bytes)"
rm -rf "$cache/dump-spirv"

# 7 the corpus
if command -v spirv-val >/dev/null && command -v spirv-as >/dev/null; then
  run_t=
else
  docker image inspect vkb-spirv-tools >/dev/null 2>&1 ||
    printf 'FROM ubuntu:24.04\nRUN apt-get update && apt-get install -y --no-install-recommends spirv-tools\n' |
    docker build -q -t vkb-spirv-tools - >/dev/null
  run_t="docker run --rm -i -u $(id -u):$(id -g) -v $work:$work -v $PWD/..:$PWD/..:ro vkb-spirv-tools"
fi
mkdir -p "$work/in" "$work/rw"
python3 -c '
import re, struct, sys
w = [int(x, 16) for x in re.findall(r"0x[0-9a-fA-F]+", open(sys.argv[1]).read())]
open(sys.argv[2], "wb").write(struct.pack("<%dI" % len(w), *w))' ../tests/bitwise_comp.inc "$work/in/bitwise_comp.spv"
cp ../tests/corpus/*.spv "$work/in/"
$run_t bash -c "for s in $PWD/../tests/corpus/*.spvasm $PWD/../tests/shlbarrier/*.spvasm; do spirv-as --target-env vulkan1.1 -o $work/in/\$(basename \$s .spvasm).spv \$s; done"
ls "$work"/in/*.spv > "$work/in.list"
"$out/shlbarrier_corpus" "$work/rw" @"$work/in.list" > "$work/corpus.log"
rc=$?
grep "FAIL\|not rewritten" "$work/corpus.log" | head -5
(cd "$work/rw" && ls) | sed "s|^|$work/in/|" > "$work/pairs.list"
val=$($run_t bash -c "ok=0 bad=0 skip=0; while read f; do b=\${f##*/}; if ! spirv-val --target-env vulkan1.3 \"\$f\" >/dev/null 2>&1; then skip=\$((skip+1)); elif spirv-val --target-env vulkan1.3 $work/rw/\$b > $work/val.err 2>&1; then ok=\$((ok+1)); else bad=\$((bad+1)); echo \"invalid after the rewrite: \$b: \$(head -1 $work/val.err)\" >&2; fi; done; echo \$ok \$bad \$skip" < "$work/pairs.list")
read -r vok vbad vskip <<< "$val"
grep "^TOTAL" "$work/corpus.log" | sed 's/^/    /'
[ $rc = 0 ] && [ "${vbad:-1}" = 0 ] && [ "${vok:-0}" -ge 3 ] &&
  grep -q "^shifts.spv: 2 scalar, 2 vector barrier(s); 1 not 32-bit; 0 already" "$work/corpus.log" &&
  grep -q "^noint.spv: 1 scalar, 1 vector barrier(s); 0 not 32-bit; 0 already" "$work/corpus.log" &&
  grep -q "^bitwise_comp.spv: " "$work/corpus.log"
step $? "corpus: $(grep -o '[0-9]* modules, [0-9]* rewritten' "$work/corpus.log"); spirv-val: ${vok:-?} clean, ${vbad:-?} invalid, ${vskip:-?} with an invalid input"

if [ $fails = 0 ]; then echo "OK: $steps steps"; else echo "FAILED: $fails of $steps steps"; exit 1; fi
