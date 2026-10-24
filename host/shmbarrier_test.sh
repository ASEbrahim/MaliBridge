#!/bin/bash
# shm_barrier (server_spvfix.c + spirv_shmbarrier.c) on the desktop driver, with a server of its
# own (its own socket and cache directory; the switches go in <cache dir>/env or a preset file):
#   1  shmbarrier_check as it is: shm_barrier off, nothing rewritten (the check's reads race, so
#      its result is only shown);
#   2  VKBRIDGE_SHARED_BARRIER=1: the module rewritten (3 control barriers at the top level, 3 at
#      entries, 1 memory barrier), the device's total logged; the tree sum's loop still races
#      (level 1 cannot order its iterations), so the result is only shown;
#   2b VKBRIDGE_SHARED_BARRIER=2: every value and sum right, five runs; the loop found uniform;
#   3  level 2 with the module inline in the pipeline's stage (maintenance5);
#   4  level 2 from a preset file for the game (SteamAppId=883710, <cache dir>/presets/883710.env);
#      the built-in RE2 entry (no GPU tested) is not applied by itself, and says the line to copy;
#   4b VKBRIDGE_SHARED_BARRIER_DUMP=1: each compute module with shared memory saved as the app
#      gave it (<cache dir>/shm-spirv/<app hash>-<c>c<i>i<e>e<m>m.spv, for a look at its pattern);
#   4c VKBRIDGE_SHARED_BARRIER_ONLY / _SKIP: only the listed app hashes (or their first hex digits)
#      rewritten, or all but the skipped ones; a list that names none = dump only;
#   4d VKBRIDGE_SPIRV_SUBST=1: <cache dir>/spv-subst/<app hash>.spv used in place of the module
#      (then rewritten as the app's); a file that is not SPIR-V not used; nothing without the switch;
#   4e VKBRIDGE_SHARED_BARRIER_LOCKSTEP=all: logged; the check's loops are not RE2's kind: as level 2;
#   5  vkbridge-smoke with it on: all pass, nothing of its rewritten (no shared memory);
#   6  the pass over the corpus (tests/shmbarrier/*.spvasm, tests/corpus, the check's shader), at
#      level 1, level 2 and level 2 with the lockstep loops (-L: RE2's ballot loop pattern given a
#      uniform trip count, then control barriers inside it; a while loop of the same kind left alone): the expected barriers per case, each rewritten module spirv-val clean, deterministic,
#      unchanged by a second pass; modules without shared memory untouched.
# spirv-as/spirv-val from PATH, else from the vkb-spirv-tools docker image (built on first use).
# Prints one line per step and "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
work=$(mktemp -d)
cache=$work/c
mkdir -p "$cache/presets"
sock=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-shmbarrier-$$.sock
log=$work/server.log
srv=
start() {
  [ -n "$srv" ] && kill $srv 2>/dev/null && wait $srv 2>/dev/null
  rm -f "$sock"
  env -u VKBRIDGE_FAKE_MISSING -u VKBRIDGE_HIDE_EXTS -u VKBRIDGE_ERRATA -u VKBRIDGE_ERRATA_IMITATE -u VKBRIDGE_MODE \
      -u VKBRIDGE_FAKE_DRIVER_VERSION -u VKBRIDGE_ERRATA_PROBE -u VKBRIDGE_SHL_BARRIER -u VKBRIDGE_SPIRV_FIX -u VKBRIDGE_HANG_DETECT \
      -u VKBRIDGE_SHARED_BARRIER -u VKBRIDGE_PRESET_FAKE_BUILTIN \
      VK_DRIVER_FILES=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json} VK_ICD_FILENAMES= \
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
  local l; l=$(wc -l < "$log")
  timeout 120 "${prog[@]}" >$work/run.log 2>&1
  rc=$?
  res=$(tail -1 $work/run.log)
  for _ in $(seq 20); do grep -q "shm_barrier: [0-9]* of" "$log" 2>/dev/null && [ "$(wc -l < "$log")" -gt "$l" ] && break; sleep 0.05; done
  new=$(tail -n +$((l+1)) "$log")
}
total() { echo "$new" | grep -o 'shm_barrier: [0-9]* of [0-9]* module(s) rewritten, [0-9]* control and [0-9]* memory barrier(s)' | tail -1; }

start
# 1
run "$out/shmbarrier_check" --
! echo "$new" | grep -q "shm_barrier" && [ -z "$(total)" ]
step $? "as it is: off, untouched (the racing check: $res)"
# 2
run "$out/shmbarrier_check" -- VKBRIDGE_SHARED_BARRIER=1
echo "$new" | grep -q "shm_barrier on (VKBRIDGE_SHARED_BARRIER)" &&
  echo "$new" | grep -q "shm_barrier: barriers between shared-memory writes and reads in compute shaders, level 1 (first module: 3 control, 0 inside uniform constructs, 3 at a construct's entry, 1 memory)" &&
  total | grep -q ": 1 of 1 module(s) rewritten, 6 control and 1 memory" &&
  echo "$new" | grep -q "shm_barrier: .*; 1 compute module(s) with shared memory, 0 of them with subgroup operations (0 adapt to the subgroup size), 0 with only BroadcastFirst/Elect (any size); level 1: "
step $? "VKBRIDGE_SHARED_BARRIER=1: $(total) (the loop still races: $res)"
# 2b
ok2=0 r2=
for k in 1 2 3 4 5; do
  run "$out/shmbarrier_check" -- VKBRIDGE_SHARED_BARRIER=2
  [ $rc = 0 ] || ok2=1
  r2="$r2${r2:+; }$res"
done
[ $ok2 = 0 ] && echo "$new" | grep -q "shm_barrier on, level 2 (VKBRIDGE_SHARED_BARRIER)" &&
  total | grep -q ": 1 of 1 module(s) rewritten, 6 control and 1 memory" &&
  echo "$new" | grep -q "level 2: 0 control barrier(s) inside uniform constructs, 1 of 4 construct(s) uniform"
step $? "VKBRIDGE_SHARED_BARRIER=2, five runs: $(echo "$r2" | sort -u | head -2)"
# 3
run "$out/shmbarrier_check" --inline -- VKBRIDGE_SHARED_BARRIER=2
if echo "$res" | grep -q "^SKIP"; then step 0 "inline module: $res"
else [ $rc = 0 ] && total | grep -q ": 1 of 1 module(s) rewritten, 6 control"; step $? "inline module, level 2: $res; $(total)"; fi
# 4
echo "VKBRIDGE_SHARED_BARRIER=2" > "$cache/presets/883710.env"
run env SteamAppId=883710 "$out/shmbarrier_check" --
r4=$rc res4=$res new4=$new t4=$(total)
rm -f "$cache/presets/883710.env"
run env SteamAppId=883710 "$out/shmbarrier_check" --
[ $r4 = 0 ] && echo "$new4" | grep -q "preset for steam883710: VKBRIDGE_SHARED_BARRIER=2 (from file)" &&
  echo "$t4" | grep -q ": 1 of 1 module(s) rewritten" && [ -z "$(total)" ] &&
  grep -q "preset for steam883710: a built-in preset exists but was not tested on .*VKBRIDGE_SHARED_BARRIER=2" "$log"
step $? "preset file for RE2 (883710): $res4; $t4; the built-in entry alone: not applied, its line said"
# 4b
run "$out/shmbarrier_check" -- VKBRIDGE_SHARED_BARRIER=2 VKBRIDGE_SHARED_BARRIER_DUMP=1
saved=$(ls "$cache/shm-spirv" 2>/dev/null)
inc_bytes=$(( $(grep -o '0x[0-9a-fA-F]*' ../tests/shmbarrier_comp.inc | wc -l) * 4 ))
[ $rc = 0 ] && [ "$(echo "$saved" | grep -c .)" = 1 ] && echo "$saved" | grep -qE '^[0-9a-f]{16}-3c0i3e1m\.spv$' &&
  [ "$(stat -c %s "$cache/shm-spirv/$saved")" = "$inc_bytes" ] && total | grep -q ": 1 of 1 module(s) rewritten"
step $? "VKBRIDGE_SHARED_BARRIER_DUMP=1: the app's module saved as shm-spirv/$saved"
hash=${saved%%-*}
mkdir -p "$cache/spv-subst"
cp "$cache/shm-spirv/$saved" "$work/check.spv"
rm -rf "$cache/shm-spirv"
# 4c
run "$out/shmbarrier_check" -- VKBRIDGE_SHARED_BARRIER=2 VKBRIDGE_SHARED_BARRIER_ONLY=${hash:0:8}
t_only=$(total)
run "$out/shmbarrier_check" -- VKBRIDGE_SHARED_BARRIER=2 VKBRIDGE_SHARED_BARRIER_ONLY=0000,ffff VKBRIDGE_SHARED_BARRIER_DUMP=1
t_none=$(total) n_none=$new d_none=$(ls "$cache/shm-spirv" 2>/dev/null)
run "$out/shmbarrier_check" -- VKBRIDGE_SHARED_BARRIER=2 VKBRIDGE_SHARED_BARRIER_SKIP=$hash
echo "$t_only" | grep -q ": 1 of 1 module(s) rewritten" && echo "$t_none" | grep -q ": 0 of 1 module(s) rewritten, 0 control and 0 memory" &&
  echo "$n_none" | grep -q "shm_barrier only for VKBRIDGE_SHARED_BARRIER_ONLY=0000,ffff" &&
  echo "$n_none" | grep -q "shm_barrier: 1 module(s) left as they were by VKBRIDGE_SHARED_BARRIER_ONLY/_SKIP" &&
  echo "$d_none" | grep -qE "^$hash-0c0i0e0m\.spv$" &&
  total | grep -q ": 0 of 1 module(s) rewritten" && echo "$new" | grep -q "not for VKBRIDGE_SHARED_BARRIER_SKIP=$hash"
step $? "VKBRIDGE_SHARED_BARRIER_ONLY/_SKIP by app hash: only ${hash:0:8}: $t_only; only others: nothing rewritten (dumped as $d_none); skipped: nothing"
rm -rf "$cache/shm-spirv"
# 4d
cp "$work/check.spv" "$cache/spv-subst/$hash.spv"
run "$out/shmbarrier_check" -- VKBRIDGE_SPIRV_SUBST=1 VKBRIDGE_SHARED_BARRIER=2 VKBRIDGE_SHARED_BARRIER_DUMP=1
n_sub=$new t_sub=$(total) d_sub=$(ls "$cache/shm-spirv" 2>/dev/null)
rm -rf "$cache/shm-spirv"
mkdir -p "$work/sub" && "$out/shmbarrier_corpus" -2 "$work/sub" "$work/check.spv" >/dev/null
cp "$work/sub/check.spv" "$cache/spv-subst/$hash.spv"
run "$out/shmbarrier_check" -- VKBRIDGE_SPIRV_SUBST=1
r_fix=$rc res_fix=$res
printf 'not spir-v, not spir-v' > "$cache/spv-subst/$hash.spv"
run "$out/shmbarrier_check" -- VKBRIDGE_SPIRV_SUBST=1
n_bad=$new
run "$out/shmbarrier_check" -- VKBRIDGE_SHARED_BARRIER=2
n_off=$new
rm -rf "$cache/spv-subst"
echo "$n_sub" | grep -q "SPIR-V subst on (VKBRIDGE_SPIRV_SUBST)" && echo "$n_sub" | grep -q "SPIR-V subst: module $hash (.* bytes) replaced by spv-subst/$hash.spv" &&
  echo "$n_sub" | grep -q "SPIR-V subst: 1 module(s) replaced" && echo "$t_sub" | grep -q ": 1 of 1 module(s) rewritten" &&
  echo "$d_sub" | grep -qE "^$hash-" && echo "$n_bad" | grep -q "SPIR-V subst: .*/spv-subst/$hash.spv is not a SPIR-V module; not used" &&
  ! echo "$n_off" | grep -q "SPIR-V subst" && [ $r_fix = 0 ]
step $? "VKBRIDGE_SPIRV_SUBST=1: spv-subst/$hash.spv used in place of the module (named as the app's, then rewritten: $t_sub); the level-2 output put there, shm_barrier off: $res_fix; a bad file not used; off without the switch"
# 4e
run "$out/shmbarrier_check" -- VKBRIDGE_SHARED_BARRIER=2 VKBRIDGE_SHARED_BARRIER_LOCKSTEP=all
[ $rc = 0 ] && echo "$new" | grep -q "shm_barrier lockstep loops for VKBRIDGE_SHARED_BARRIER_LOCKSTEP=all" &&
  echo "$new" | grep -q "shm_barrier: lockstep: 0 module(s), 0 loop(s) given a uniform trip count" && total | grep -q ": 1 of 1 module(s) rewritten, 6 control"
step $? "VKBRIDGE_SHARED_BARRIER_LOCKSTEP=all: the check's loops are not of that kind, left as level 2 has them: $res"
# 5
run "$out/vkbridge-smoke" -- VKBRIDGE_SHARED_BARRIER=1
[ $rc = 0 ] && total | grep -qE ": 0 of [1-9][0-9]* module\(s\) rewritten"
step $? "vkbridge-smoke: $res; $(total)"

# 6 the corpus
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
open(sys.argv[2], "wb").write(struct.pack("<%dI" % len(w), *w))' ../tests/shmbarrier_comp.inc "$work/in/shmbarrier_comp.spv"
cp ../tests/corpus/*.spv "$work/in/"
$run_t bash -c "for s in $PWD/../tests/corpus/*.spvasm $PWD/../tests/shmbarrier/*.spvasm; do spirv-as --target-env vulkan1.1 -o $work/in/\$(basename \$s .spvasm).spv \$s; done"
ls "$work"/in/*.spv > "$work/in.list"
"$out/shmbarrier_corpus" "$work/rw" @"$work/in.list" > "$work/corpus.log"
rc=$?
mkdir -p "$work/rw2" "$work/rwL"
"$out/shmbarrier_corpus" -2 "$work/rw2" @"$work/in.list" > "$work/corpus2.log" || rc=1
"$out/shmbarrier_corpus" -2 -L "$work/rwL" @"$work/in.list" > "$work/corpusL.log" || rc=1
grep "FAIL\|not rewritten" "$work/corpus.log" | head -5
val=$($run_t bash -c "ok=0 bad=0; for f in $work/rw/*.spv $work/rw2/*.spv $work/rwL/*.spv; do if spirv-val --target-env vulkan1.1 \$f > $work/val.err 2>&1; then ok=\$((ok+1)); else bad=\$((bad+1)); echo \"invalid after the rewrite: \${f##*/}: \$(head -1 $work/val.err)\" >&2; fi; done; echo \$ok \$bad")
read -r vok vbad <<< "$val"
want() { grep -q "^$1: $2; 1 entry point(s)$" "$work/corpus.log" || { echo "    want $1: $2, got: $(grep "^$1:" "$work/corpus.log")"; return 1; }; }
ok6=0
want top.spv "1 control, 0 entry, 0 memory" || ok6=1
want sel.spv "0 control, 1 entry, 0 memory" || ok6=1
want inner.spv "1 control, 0 entry, 1 memory" || ok6=1
want loop.spv "0 control, 1 entry, 0 memory" || ok6=1
want early.spv "0 control, 0 entry, 1 memory" || ok6=1
want war.spv "2 control, 0 entry, 0 memory" || ok6=1
want call.spv "1 control, 0 entry, 0 memory" || ok6=1
grep -q "^subgroup.spv: 1 control, 0 entry, 0 memory; 1 entry point(s); subgroup operations$" "$work/corpus.log" || ok6=1
want shmbarrier_comp.spv "3 control, 3 entry, 1 memory" || ok6=1
want uloop.spv "1 control, 1 entry, 1 memory" || ok6=1
want dxvkloop.spv "0 control, 0 entry, 1 memory" || ok6=1
want divloop.spv "0 control, 0 entry, 1 memory" || ok6=1
want2() { grep -q "^$1: $2; 1 entry point(s)$" "$work/corpus2.log" || { echo "    level 2: want $1: $2, got: $(grep "^$1:" "$work/corpus2.log")"; return 1; }; }
want2 dxvkloop.spv "0 control, 2 inside, 0 entry, 0 memory; 1 of 1 construct(s) uniform" || ok6=1
want2 uloop.spv "1 control, 0 inside, 1 entry, 1 memory; 1 of 2 construct(s) uniform" || ok6=1
want2 divloop.spv "0 control, 0 inside, 0 entry, 2 memory; 0 of 1 construct(s) uniform" || ok6=1
want2 loop.spv "0 control, 0 inside, 1 entry, 0 memory; 1 of 1 construct(s) uniform" || ok6=1
want2 sel.spv "0 control, 0 inside, 1 entry, 0 memory; 0 of 1 construct(s) uniform" || ok6=1
want2 top.spv "1 control, 0 inside, 0 entry, 0 memory; 0 of 0 construct(s) uniform" || ok6=1
want2 shmbarrier_comp.spv "3 control, 0 inside, 3 entry, 1 memory; 1 of 4 construct(s) uniform" || ok6=1
want2 lockstep.spv "0 control, 0 inside, 0 entry, 3 memory; 0 of 5 construct(s) uniform" || ok6=1
want2 lockstep_top.spv "0 control, 0 inside, 0 entry, 3 memory; 0 of 5 construct(s) uniform" || ok6=1
grep -q "^synced.spv: untouched" "$work/corpus.log" || ok6=1
# -L: only lockstep.spv's loop changes, and then level 2 gets its 3 control barriers in; nothing else differs
grep -q "^lockstep.spv: lockstep: 1 loop(s)$" "$work/corpusL.log" &&
  grep -q "^lockstep.spv: 0 control, 3 inside, 0 entry, 0 memory; 2 of 5 construct(s) uniform; 1 entry point(s)$" "$work/corpusL.log" &&
  [ "$(grep -c "lockstep: " "$work/corpusL.log")" = 1 ] &&
  [ "$(grep -v "^lockstep" "$work/corpusL.log")" = "$(grep -v "^lockstep" "$work/corpus2.log")" ] || { echo "    -L: $(grep "^lockstep" "$work/corpusL.log" | tr '\n' ' ')"; ok6=1; }
nrw=$(ls "$work/rw" | wc -l) nrw2=$(ls "$work/rw2" | wc -l) nrwL=$(ls "$work/rwL" | wc -l)
[ $rc = 0 ] && [ $ok6 = 0 ] && [ "${vbad:-1}" = 0 ] && [ "${vok:-0}" = $((nrw + nrw2 + nrwL)) ] && [ "$nrw" = 14 ] && [ "$nrw2" = 14 ] && [ "$nrwL" = 14 ]
step $? "corpus: $(grep -o '[0-9]* modules, [0-9]* rewritten' "$work/corpus.log") at each level; spirv-val: ${vok:-?} clean, ${vbad:-?} invalid"

if [ $fails = 0 ]; then echo "OK: $steps steps"; else echo "FAILED: $fails of $steps steps"; exit 1; fi
