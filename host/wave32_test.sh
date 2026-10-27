#!/bin/bash
# wave32 (server_spvfix.c + spirv_wave32.c) with a server of its own (its own socket and cache
# directory; the switches go in <cache dir>/env or a preset file). The narrow-subgroup GPU is
# llvmpipe (8 lanes: like Mali's 16, narrower than the 32 the shader counts on); RADV (64, or 32
# required) is the reference:
#   1  llvmpipe, off: wave32_check's values wrong (the 32-lane ballot, shuffles, ids), nothing logged;
#   2  llvmpipe, VKBRIDGE_WAVE32=1: every value right, three runs; the device's lines;
#   3  llvmpipe, =1: every subgroup operation the pass does (tests/wave32/allops.spv) and a
#      32-invocation workgroup with an if and a loop on votes (wg32.spv) give exactly what RADV
#      gives with a required subgroup size of 32 (skipped without RADV or that control);
#   4  =1 from a preset file for the game (SteamAppId=883710, <cache dir>/presets/883710.env);
#   5  VKBRIDGE_SHARED_BARRIER_DUMP=1: the module saved as shm-spirv/<hash>-0c0i0e0m-sg-w32.spv;
#      VKBRIDGE_SHARED_BARRIER_DUMP_MAX=0: none saved, and said so;
#   6  RADV (64 lanes), =1: not needed, said so, left alone; =force: right there too;
#   7  vkbridge-smoke with =force: all pass, nothing of its emulated;
#   8  the pass over a corpus (tests/wave32/*.spv, tests/corpus, the check's shader): what each
#      case should get (a ballot in a non-uniform if: left alone; 48 invocations: left alone; a
#      size compare: adapts; BroadcastFirst only: any size; a waterfall loop's BroadcastFirst
#      kept), each rewritten module spirv-val clean, deterministic, unchanged by a
#      second pass.
# spirv-val from PATH, the Android NDK's shader-tools, or the vkb-spirv-tools docker image.
# Prints one line per step and "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
work=$(mktemp -d)
cache=$work/c
mkdir -p "$cache/presets"
sock=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-wave32-$$.sock
log=$work/server.log
lvp=/usr/share/vulkan/icd.d/lvp_icd.json
radv=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json}
srv=
start() { # <host ICD>
  [ -n "$srv" ] && kill $srv 2>/dev/null && wait $srv 2>/dev/null
  rm -f "$sock"
  env -u VKBRIDGE_FAKE_MISSING -u VKBRIDGE_HIDE_EXTS -u VKBRIDGE_ERRATA -u VKBRIDGE_ERRATA_IMITATE -u VKBRIDGE_MODE \
      -u VKBRIDGE_FAKE_DRIVER_VERSION -u VKBRIDGE_ERRATA_PROBE -u VKBRIDGE_SHL_BARRIER -u VKBRIDGE_SPIRV_FIX -u VKBRIDGE_HANG_DETECT \
      -u VKBRIDGE_SHARED_BARRIER -u VKBRIDGE_WAVE32 -u VKBRIDGE_PRESET_FAKE_BUILTIN \
      VK_DRIVER_FILES=$1 VK_ICD_FILENAMES= \
      "$out/vkbridge-server" --socket "$sock" --log "$log" --cache-dir "$cache" >/dev/null 2>&1 &
  srv=$!
  for _ in $(seq 50); do [ -S "$sock" ] && break; sleep 0.1; done
}
trap 'kill $srv 2>/dev/null; rm -f "$sock"; [ -n "$KEEP" ] || rm -rf "$work"' EXIT
export VKBRIDGE_SOCKET=$sock VK_DRIVER_FILES=$out/vkbridge_icd.json VK_ICD_FILENAMES=
unset VKBRIDGE_HIDE_EXTS VKBRIDGE_MAX_API SteamAppId SteamGameId VKBRIDGE_PCACHE_KEY
fails=0 steps=0
step() { steps=$((steps+1)); if [ "$1" = 0 ]; then echo "  ok   $2"; else echo "  FAIL $2"; fails=$((fails+1)); fi; }

# run <program> [args] -- <env lines for <cache dir>/env>
run() {
  local prog=("$1"); shift
  while [ "$1" != -- ]; do prog+=("$1"); shift; done; shift
  printf '%s\n' "$@" > "$cache/env"
  local l; l=$(wc -l < "$log" 2>/dev/null || echo 0)
  timeout 120 "${prog[@]}" >$work/run.log 2>&1
  rc=$?
  res=$(tail -1 $work/run.log)
  for _ in $(seq 40); do grep -q "device [0-9]* destroyed" <(tail -n +$((l+1)) "$log") 2>/dev/null && break; sleep 0.05; done
  new=$(tail -n +$((l+1)) "$log")
}
total() { echo "$new" | grep -o 'wave32: [0-9]* of [0-9]* compute module(s) emulated at 32 lanes, [0-9]* subgroup operation(s), [0-9]* barrier(s)' | tail -1; }

start "$lvp"
# 1
run "$out/wave32_check" --
[ $rc != 0 ] && echo "$res" | grep -q "^FAIL: .*(subgroup size 8)" && ! echo "$new" | grep -qE "wave32( on| not needed|:)"
step $? "llvmpipe, off: wrong as expected ($res)"
# 2
ok2=0 r2=
for k in 1 2 3; do
  run "$out/wave32_check" -- VKBRIDGE_WAVE32=1
  [ $rc = 0 ] || ok2=1
  r2="$r2${r2:+; }$res"
done
[ $ok2 = 0 ] && echo "$new" | grep -q "wave32 on (VKBRIDGE_WAVE32): .* (this GPU's are 8 wide" &&
  echo "$new" | grep -q "wave32: 32-lane subgroups emulated in compute shaders (first module [0-9a-f]*: 3 subgroup operation(s), 4 built-in(s), 24 barrier(s), 256 bytes of shared memory, 64 invocations)" &&
  total | grep -q ": 1 of 1 compute module(s) emulated at 32 lanes, 3 subgroup operation(s), 24 barrier(s)" &&
  echo "$new" | grep -q "1 count on a subgroup size, 0 adapt to it, 0 only BroadcastFirst/Elect; 0 left as they were"
step $? "VKBRIDGE_WAVE32=1, three runs: $(echo "$r2" | tr ';' '\n' | sed 's/^ //' | sort -u | head -2 | tr '\n' ' ')"
# 3
if VK_DRIVER_FILES=$radv timeout 60 "$out/wave32_check" --require 32 >$work/ref.log 2>&1 && grep -q "^PASS" $work/ref.log; then
  ok3=0 d3=
  for m in allops:16 wg32:16; do
    name=${m%:*}
    VK_DRIVER_FILES=$radv timeout 60 "$out/wave32_check" --require 32 --spv ../tests/wave32/$name.spv --groups ${m#*:} --dump $work/ref-$name.bin >/dev/null 2>&1 || ok3=1
    run "$out/wave32_check" --spv ../tests/wave32/$name.spv --groups ${m#*:} --dump $work/lvp-$name.bin -- VKBRIDGE_WAVE32=1
    [ $rc = 0 ] && cmp -s $work/ref-$name.bin $work/lvp-$name.bin || { ok3=1; d3="$d3 $name: $(cmp -l $work/ref-$name.bin $work/lvp-$name.bin 2>/dev/null | wc -l) bytes differ;"; }
  done
  t3=$(total)
  # natively (the bridge's pass off) llvmpipe's are not the 32-lane values
  run "$out/wave32_check" --spv ../tests/wave32/allops.spv --dump $work/nat-allops.bin --
  ! cmp -s $work/ref-allops.bin $work/nat-allops.bin || ok3=1
  [ $ok3 = 0 ]
  step $? "allops (50 operations) and wg32 through the bridge on llvmpipe == RADV with 32 required; natively they differ${d3:+ ($d3)}"
else
  step 0 "allops/wg32 against RADV: SKIP ($(tail -1 $work/ref.log))"
fi
# 4
echo "VKBRIDGE_WAVE32=1" > "$cache/presets/883710.env"
run env SteamAppId=883710 "$out/wave32_check" --
rm -f "$cache/presets/883710.env"
[ $rc = 0 ] && echo "$new" | grep -q "preset for steam883710: VKBRIDGE_WAVE32=1 (from file)" && total | grep -q ": 1 of 1 compute"
step $? "preset file for RE2 (883710): $res; $(total)"
# 5
run "$out/wave32_check" -- VKBRIDGE_WAVE32=1 VKBRIDGE_SHARED_BARRIER_DUMP=1
saved=$(ls "$cache/shm-spirv" 2>/dev/null)
inc_bytes=$(( $(grep -o '0x[0-9a-fA-F]*' ../tests/wave32_comp.inc | wc -l) * 4 ))
[ $rc = 0 ] && [ "$(echo "$saved" | grep -c .)" = 1 ] && echo "$saved" | grep -qE '^[0-9a-f]{16}-0c0i0e0m-sg-w32\.spv$' &&
  [ "$(stat -c %s "$cache/shm-spirv/$saved")" = "$inc_bytes" ]
s5=$?
rm -rf "$cache/shm-spirv"
run "$out/wave32_check" -- VKBRIDGE_WAVE32=1 VKBRIDGE_SHARED_BARRIER_DUMP=1 VKBRIDGE_SHARED_BARRIER_DUMP_MAX=0
[ $s5 = 0 ] && [ $rc = 0 ] && [ -z "$(ls "$cache/shm-spirv" 2>/dev/null)" ] &&
  echo "$new" | grep -q "shm-spirv/: 0 module(s) saved, 1 more not (VKBRIDGE_SHARED_BARRIER_DUMP_MAX=0)"
step $? "VKBRIDGE_SHARED_BARRIER_DUMP=1: saved as shm-spirv/$saved; DUMP_MAX=0: none, said so"
# 6
start "$radv"
run "$out/wave32_check" -- VKBRIDGE_WAVE32=1
r6=$rc res6=$res new6=$new
if echo "$new6" | grep -q "wave32 not needed: subgroups are [0-9]* wide"; then
  run "$out/wave32_check" -- VKBRIDGE_WAVE32=force
  [ $rc = 0 ] && ! echo "$new6" | grep -q "wave32 on" &&
    echo "$new" | grep -q "wave32 on (VKBRIDGE_WAVE32=force)" && total | grep -q ": 1 of 1 compute"
  step $? "RADV, =1: not needed, left alone ($res6); =force: $res"
else
  # VKB_HOST_ICD chose a narrow-subgroup driver (llvmpipe): the "not needed" path cannot be
  # exercised there (step 3 already skips its RADV reference the same way).
  step 0 "RADV, =1: SKIP (the reference driver's subgroups are narrower than 32)"
fi
# 7
run "$out/vkbridge-smoke" -- VKBRIDGE_WAVE32=force
[ $rc = 0 ] && total | grep -qE ": 0 of [0-9]+ compute module\(s\) emulated"
step $? "vkbridge-smoke, =force: $res; $(total)"

# 8 the corpus
if command -v spirv-val >/dev/null; then
  val=spirv-val run_t=
elif ndk=$(ls -d ~/Android/Sdk/ndk/*/shader-tools/linux-x86_64 2>/dev/null | tail -1) && [ -x "$ndk/spirv-val" ]; then
  val=$ndk/spirv-val run_t=
else
  docker image inspect vkb-spirv-tools >/dev/null 2>&1 ||
    printf 'FROM ubuntu:24.04\nRUN apt-get update && apt-get install -y --no-install-recommends spirv-tools\n' |
    docker build -q -t vkb-spirv-tools - >/dev/null
  val=spirv-val run_t="docker run --rm -i -u $(id -u):$(id -g) -v $work:$work vkb-spirv-tools"
fi
mkdir -p "$work/in" "$work/rw"
python3 -c '
import re, struct, sys
w = [int(x, 16) for x in re.findall(r"0x[0-9a-fA-F]+", open(sys.argv[1]).read())]
open(sys.argv[2], "wb").write(struct.pack("<%dI" % len(w), *w))' ../tests/wave32_comp.inc "$work/in/wave32_comp.spv"
cp ../tests/wave32/*.spv ../tests/corpus/*.spv "$work/in/"
ls "$work"/in/*.spv > "$work/in.list"
"$out/wave32_corpus" "$work/rw" @"$work/in.list" > "$work/corpus.log"
rc=$?
grep "FAIL" "$work/corpus.log" | head -5
vres=$($run_t bash -c "ok=0 bad=0; for f in $work/rw/*.spv; do e=vulkan1.1; case \$f in *vmm*) e=vulkan1.2;; *spv16*) e=vulkan1.3;; esac; if $val --target-env \$e \$f > $work/val.err 2>&1; then ok=\$((ok+1)); else bad=\$((bad+1)); echo \"invalid after the rewrite: \${f##*/}: \$(head -1 $work/val.err)\" >&2; fi; done; echo \$ok \$bad")
read -r vok vbad <<< "$vres"
want() { grep -q "^$1: $2$" "$work/corpus.log" || { echo "    want $1: $2, got: $(grep "^$1:" "$work/corpus.log")"; return 1; }; }
ok8=0
want wave32_comp.spv "fixed; 3 op(s), 4 built-in(s), 24 barrier(s), 256 bytes shared; 64 invocations" || ok8=1
want wave32_opt.spv "fixed; 3 op(s), 4 built-in(s), 24 barrier(s), 256 bytes shared; 64 invocations" || ok8=1
want vmm.spv "fixed; 3 op(s), 4 built-in(s), 24 barrier(s), 256 bytes shared; 64 invocations" || ok8=1
want allops.spv "fixed; 50 op(s), 9 built-in(s), 340 barrier(s), 768 bytes shared; 64 invocations" || ok8=1
want allops_spv16.spv "fixed; 50 op(s), 9 built-in(s), 340 barrier(s), 768 bytes shared; 64 invocations" || ok8=1
want wg32.spv "fixed; 4 op(s), 0 built-in(s), 40 barrier(s), 128 bytes shared; 32 invocations" || ok8=1
want waterfall.spv "fixed; 1 op(s), 0 built-in(s), 10 barrier(s), 256 bytes shared; 64 invocations; BroadcastFirst kept" || ok8=1
want divballot.spv "fixed; not rewritten: a subgroup operation where the workgroup may not be together" || ok8=1
want wg48.spv "fixed; not rewritten: a workgroup size that is not a multiple of 32" || ok8=1
want adapts.spv "adapts; untouched" || ok8=1
want anysize.spv "any size; untouched" || ok8=1
nrw=$(ls "$work/rw" | wc -l)
[ $rc = 0 ] && [ $ok8 = 0 ] && [ "${vbad:-1}" = 0 ] && [ "${vok:-0}" = "$nrw" ] && [ "$nrw" = 7 ] &&
  ! grep -v "^\(wave32_comp\|wave32_opt\|vmm\|allops\|allops_spv16\|wg32\|waterfall\|divballot\|wg48\|adapts\|anysize\).spv:\|^TOTAL" "$work/corpus.log" | grep -qv ": none; untouched$"
step $? "corpus: $(tail -1 "$work/corpus.log"); spirv-val: ${vok:-?} clean, ${vbad:-?} invalid"

if [ $fails = 0 ]; then echo "OK: $steps steps"; else echo "FAILED: $fails of $steps steps"; exit 1; fi
