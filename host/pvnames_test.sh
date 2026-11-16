#!/bin/bash
# pv_names (PV-01, PowerVR: an input reads zero beside an output of its OpName; server_errata_pvnames.c,
# spirv_names.c, the probe pv_names in server_probe.c) on the desktop driver, with a server of its
# own (its own socket and cache directory; the switches go in <cache dir>/env):
#   1  pvnames_check through the bridge as it is (the desktop driver has no such bug): the probe
#      passes, the fix is off, no module touched;
#   2  the bug imitated beneath (VKBRIDGE_ERRATA_IMITATE=pv_names), the fix forced off: the pixel is
#      black; the probe (run with the imitation beneath it) fails with the bug's pattern and the
#      stripped twin right;
#   3  the same imitation, nothing forced: the probe's failure turns the fix on (safe mode and
#      performance mode alike) and the picture is right; the imitation broke nothing;
#   3b the same under the PowerVR identity (VKBRIDGE_FAKE_DRIVER=img-full);
#   4  the imitation with the fix forced over it (a profile whose probe passed): right; the modules
#      (vertex and fragment) logged once; the device total counts them;
#   5  the same with the modules inline in the pipeline's stages (maintenance5);
#   6  layers beneath name the module by the app's code: the hang detector's dump-spirv/h-<hash> is
#      the app's hash, holding the code the driver got (without its names);
#   7  the pass over the corpus (tests/corpus, the check's own shaders, and every *.spv under the
#      directories in PVNAMES_CORPUS_DIRS, colon-separated): each result spirv-val clean (when its
#      input is), exactly the module without its names, deterministic, nothing left for the pass or
#      the imitation;
#   8  the protocol hash is unchanged (server-side only).
# spirv-as/spirv-val from PATH, else the NDK's shader-tools, else a docker image with Ubuntu's
# spirv-tools (as bitwise_test.sh does). Prints one line per step and "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
work=$(mktemp -d)
cache=$work/c
mkdir -p "$cache"
sock=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-pvnames-$$.sock
log=$work/server.log
srv=
# start [env...]: a server of its own on a fresh cache directory (a fresh probe profile)
start() {
  [ -n "$srv" ] && kill $srv 2>/dev/null && wait $srv 2>/dev/null
  rm -f "$sock"
  rm -rf "$cache"; mkdir -p "$cache"
  : > "$log"
  env -u VKBRIDGE_FAKE_MISSING -u VKBRIDGE_HIDE_EXTS -u VKBRIDGE_ERRATA -u VKBRIDGE_ERRATA_IMITATE -u VKBRIDGE_MODE \
      -u VKBRIDGE_FAKE_DRIVER_VERSION -u VKBRIDGE_ERRATA_PROBE -u VKBRIDGE_HANG_DETECT -u VKBRIDGE_PROBE_FAKE \
      VKBRIDGE_PROBE_BUDGET_S=0 "$@" VK_DRIVER_FILES=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json} VK_ICD_FILENAMES= \
      "$out/vkbridge-server" --socket "$sock" --log "$log" --cache-dir "$cache" >/dev/null 2>&1 &
  srv=$!
  for _ in $(seq 100); do [ -S "$sock" ] && break; sleep 0.1; done
}
trap 'kill $srv 2>/dev/null; rm -f "$sock"; [ -n "$KEEP" ] || rm -rf "$work"' EXIT
export VKBRIDGE_SOCKET=$sock VK_DRIVER_FILES=$out/vkbridge_icd.json VK_ICD_FILENAMES=
unset VKBRIDGE_HIDE_EXTS VKBRIDGE_MAX_API
fails=0 steps=0
step() { [ -n "$DBG" ] && [ "$1" != 0 ] && echo "$new" | grep -i "pv_names\|errata: device" | cut -c1-250; steps=$((steps+1)); if [ "$1" = 0 ]; then echo "  ok   $2"; else echo "  FAIL $2"; fails=$((fails+1)); fi; }

# One run of a program with <cache dir>/env = the arguments after --; $new: its log lines, $res its
# last output line, $rc its exit status
run() {
  local prog=("$1"); shift
  while [ "$1" != -- ]; do prog+=("$1"); shift; done; shift
  printf '%s\n' "$@" > "$cache/env"
  local l; l=$(wc -l < "$log")
  timeout 120 "${prog[@]}" >$work/run.log 2>&1
  rc=$?
  sleep 0.4 # the server logs the device's total when the connection closes
  res=$(tail -1 $work/run.log)
  new=$(tail -n +$((l+1)) "$log")
}
total() { echo "$new" | grep -o 'pv_names: device [0-9]*: names dropped from [0-9]* of [0-9]* module(s)' | tail -1; }

# The app's hash of the check's vertex module (FNV-1a over its bytes, the h-<hash> names)
app_hash=$(python3 -c '
import re, struct, sys
w = [int(x, 16) for x in re.findall(r"0x[0-9a-fA-F]+", open(sys.argv[1]).read())]
h = 0xcbf29ce484222325
for b in struct.pack("<%dI" % len(w), *w): h = ((h ^ b) * 0x100000001b3) & 0xffffffffffffffff
print("%016x" % h)' ../tests/pvnames_vert.inc)

# 1
start
run "$out/pvnames_check" -- VKBRIDGE_MODE=safe
[ $rc = 0 ] && ! echo "$new" | grep -q "pv_names: module" && ! grep -q "probe: pv_names " "$log" &&
  echo "$new" | grep -q "errata: device .*: on [^;]*; off .*pv_names"
step $? "as it is (not PowerVR): probe not run, fix off, untouched ($res)"
# 1b PowerVR's identity on a driver without the bug: the probe runs and passes, the fix off
start VKBRIDGE_FAKE_DRIVER=img-full
run "$out/pvnames_check" -- VKBRIDGE_MODE=safe
[ $rc = 0 ] && ! echo "$new" | grep -q "pv_names: module" && grep -q "probe: pv_names  *pass" "$log"
step $? "PowerVR identity, no bug beneath: the probe runs and passes, fix off ($res)"
# 2
start VKBRIDGE_ERRATA_IMITATE=pv_names
run "$out/pvnames_check" -- VKBRIDGE_ERRATA=-pv_names
[ $rc != 0 ] && echo "$res" | grep -q "^FAIL: pixel (0 0 0 255)" && echo "$new" | grep -q "errata: pv_names bug imitated beneath the fix" &&
  grep -q "probe: pv_names fail (shared names: WRONG (0 0 0 255, want 64 128 191 255); names stripped: right - the input reads wrong beside an output of its name" "$log" &&
  echo "$new" | grep -q "errata: pv_names OFF (VKBRIDGE_ERRATA=-pv_names; it would be on: probe pv_names failed" &&
  echo "$new" | grep -q "the imitation broke 1 module"
step $? "the bug imitated, fix forced off: $res"
# 3
for mode in safe performance; do
  start VKBRIDGE_ERRATA_IMITATE=pv_names
  run "$out/pvnames_check" -- VKBRIDGE_MODE=$mode
  [ $rc = 0 ] && echo "$new" | grep -q "errata: pv_names ON (probe pv_names failed; $mode mode) - PV-01" &&
    echo "$new" | grep -q "errata: pv_names: module [0-9]* -> [0-9]* bytes, names dropped" && echo "$new" | grep -q "the imitation broke 0 module" &&
    total | grep -q ": names dropped from 2 of 2 module"
  step $? "the imitation, the probe's failure alone turns the fix on ($mode mode): $res"
done
# 3b the PowerVR identity (VKBRIDGE_FAKE_DRIVER=img-full: vendor 0x1010, driverID 7): the probe-gated fix turns on there as anywhere, and
# no Arm path comes with it
start VKBRIDGE_ERRATA_IMITATE=pv_names VKBRIDGE_FAKE_DRIVER=img-full
run "$out/pvnames_check" -- VKBRIDGE_MODE=safe
[ $rc = 0 ] && grep -q "imitating PowerVR (VKBRIDGE_FAKE_DRIVER=img-full)" "$log" && echo "$new" | grep -q "errata: pv_names ON (probe pv_names failed; safe mode) - PV-01" &&
  echo "$new" | grep -q "PowerVR driver .*CL 6908880), not Arm's: its own CL ranges only" && ! echo "$new" | grep -q "errata: [a-z_]* ON (r[0-9]*p[0-9]*" &&
  total | grep -q ": names dropped from 2 of 2 module"
step $? "PowerVR identity (VKBRIDGE_FAKE_DRIVER=img-full): the probe's failure turns the fix on, no version gate with it: $res"
# 4 (a profile whose probe passed: the fix forced on over the imitation)
start
run "$out/pvnames_check" -- VKBRIDGE_ERRATA_IMITATE=pv_names VKBRIDGE_ERRATA=+pv_names
line=$(echo "$new" | grep "errata: pv_names: module ")
[ $rc = 0 ] && [ "$(echo "$line" | grep -c .)" = 2 ] && echo "$new" | grep -q "errata: pv_names ON (VKBRIDGE_ERRATA=+pv_names" &&
  echo "$new" | grep -q "the imitation broke 0 module" && total | grep -q ": names dropped from 2 of 2 module"
step $? "the imitation with the fix forced over it: $res; $(echo "$line" | grep -o 'module [0-9]* -> [0-9]* bytes' | tr '\n' ' ')"
# 5
run "$out/pvnames_check" --inline -- VKBRIDGE_ERRATA_IMITATE=pv_names
r5a=$rc res5a=$res
run "$out/pvnames_check" --inline -- VKBRIDGE_ERRATA_IMITATE=pv_names VKBRIDGE_ERRATA=+pv_names
if echo "$res" | grep -q "^SKIP"; then
  step 0 "inline modules: $res"
else
  [ $r5a != 0 ] && [ $rc = 0 ] && total | grep -q ": names dropped from 2 of 2 module"
  step $? "inline modules: imitated $res5a; with the fix $res ($(total | grep -o 'from [0-9]* of [0-9]* module'))"
fi
# 6
mkdir -p "$cache/dump-spirv"
run "$out/pvnames_check" -- VKBRIDGE_ERRATA=+pv_names VKBRIDGE_HANG_DETECT=1
dumped=$cache/dump-spirv/h-$app_hash.spv
inc_bytes=$(( $(grep -o '0x[0-9a-fA-F]*' ../tests/pvnames_vert.inc | wc -l) * 4 ))
[ $rc = 0 ] && [ -f "$dumped" ] && [ "$(stat -c %s "$dumped")" -lt "$inc_bytes" ] && ! grep -qa TEXCOORD "$dumped"
step $? "the hang detector names the module h-$app_hash (the app's), its dump the driver's code ($(stat -c %s "$dumped" 2>/dev/null) bytes, the app's $inc_bytes; no TEXCOORD left)"
rm -rf "$cache/dump-spirv"

# 7 the corpus
tools=$(ls -d "${ANDROID_HOME:-$HOME/Android/Sdk}"/ndk/*/shader-tools/*/ 2>/dev/null | tail -1)
if command -v spirv-val >/dev/null && command -v spirv-as >/dev/null; then
  run_t=
elif [ -n "$tools" ] && [ -x "$tools/spirv-val" ]; then
  run_t="env PATH=$tools:$PATH"
else
  docker image inspect vkb-spirv-tools >/dev/null 2>&1 ||
    printf 'FROM ubuntu:24.04\nRUN apt-get update && apt-get install -y --no-install-recommends spirv-tools\n' |
    docker build -q -t vkb-spirv-tools - >/dev/null
  run_t="docker run --rm -i -u $(id -u):$(id -g) -v $work:$work -v $PWD/..:$PWD/..:ro"
  IFS=: read -ra extra <<< "${PVNAMES_CORPUS_DIRS:-}"
  for d in "${extra[@]}"; do [ -d "$d" ] && run_t+=" -v $d:$d:ro"; done
  run_t+=" vkb-spirv-tools"
fi
mkdir -p "$work/in" "$work/rw"
for n in vert frag; do
  python3 -c '
import re, struct, sys
w = [int(x, 16) for x in re.findall(r"0x[0-9a-fA-F]+", open(sys.argv[1]).read())]
open(sys.argv[2], "wb").write(struct.pack("<%dI" % len(w), *w))' ../tests/pvnames_$n.inc "$work/in/pvnames_$n.spv"
done
cp ../tests/corpus/*.spv "$work/in/"
$run_t bash -c "for s in $PWD/../tests/corpus/*.spvasm; do spirv-as --target-env vulkan1.1 -o $work/in/\$(basename \$s .spvasm).spv \$s; done"
IFS=: read -ra extra <<< "${PVNAMES_CORPUS_DIRS:-}"
k=0
for d in "${extra[@]}"; do [ -d "$d" ] && find "$d" -name '*.spv' -type f; done | while read -r f; do
  k=$((k+1)); ln -s "$f" "$work/in/x$k-${f##*/}"
done
ls "$work"/in/*.spv > "$work/in.list"
"$out/pvnames_corpus" -q "$work/rw" @"$work/in.list" > "$work/corpus.log"
rc=$?
grep "FAIL" "$work/corpus.log" | head -5
(cd "$work/rw" && ls) | sed "s|^|$work/in/|" > "$work/pairs.list"
val=$($run_t bash -c "ok=0 bad=0 skip=0; while read f; do b=\${f##*/}; if ! spirv-val --target-env vulkan1.3 \"\$f\" >/dev/null 2>&1; then skip=\$((skip+1)); elif spirv-val --target-env vulkan1.3 $work/rw/\$b > $work/val.err 2>&1; then ok=\$((ok+1)); else bad=\$((bad+1)); echo \"invalid after the pass: \$b: \$(head -1 $work/val.err)\" >&2; fi; done; echo \$ok \$bad \$skip" < "$work/pairs.list")
read -r vok vbad vskip <<< "$val"
grep "^TOTAL" "$work/corpus.log" | sed 's/^/    /'
[ $rc = 0 ] && [ "${vbad:-1}" = 0 ] && [ "${vok:-0}" -ge 1 ] && grep -q "with an input and an output of one name" "$work/corpus.log" &&
  ! grep -q ", 0 with an input and an output of one name" "$work/corpus.log"
step $? "corpus: $(grep -o '^TOTAL: [0-9]* modules, [0-9]* with names' "$work/corpus.log"); spirv-val: ${vok:-?} clean, ${vbad:-?} invalid, ${vskip:-?} with an invalid input"

# 8
grep -q "^#define VKB_PROTOCOL_HASH 0xe88f0e43c2b566b9ULL" ../common/gen/vkb_gen.h
step $? "protocol hash $(grep -o '0x[0-9a-f]*' ../common/gen/vkb_gen.h | head -1)"

if [ $fails = 0 ]; then echo "OK: $steps steps"; else echo "FAILED: $fails of $steps steps"; exit 1; fi
