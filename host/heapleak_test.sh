#!/bin/bash
# The server's heap with what Detroit: Become Human did (a player log, G925: the heap 520 MB -> 3.6 GB at
# the game's start, then +~110 MB a minute until the server aborted), tests/heapleak_check.c:
#   1. pools: six empty FREE_DESCRIPTOR_SET pools of 45056 sets asked 20 times each for a set bigger
#      than the pool: refused as before, and at most one overflow pool per app pool made for them
#      (the old server made 64 each: 384, 8 MB apiece in Mali's heap); wanted overflow pools still grow
#      (4 pools' worth, frees, resets).
#   2. frames: 10000 frames, each with sets from a FREE pool, the oversized set again, a waited
#      submission and a new pipeline compiled into the app's own pipeline cache, the cache replaced
#      every 2000 frames; the server's own cache is over its limit (VKBRIDGE_PCACHE_MAX_MB=0.05):
#      the server's anonymous memory from frame 2000 to the end grows by less than HEAPLEAK_MAX_KB
#      (default 4096; the old server kept a copy of every app cache in its own: +~10 MB on RADV),
#      and the server's cache line ("pipeline caches NK") stays where it went over the limit.
# Needs a driver that refuses sets bigger than their pool and keeps pipeline cache data (RADV, the
# default here); lavapipe does neither, so the CI run (host/run_ci_tests.sh) leaves this out.
#   3. nullcache: pipelines made without an app cache (DXVK, vkd3d-proton) past the limit: the
#      server's cache goes back to the file at 1.5 x the limit, stays bounded, and the file's
#      pipelines and this session's repeats are still found in it (creation feedback).
#   4. varcount: a layout with a variable descriptor count refused at 8000 (bigger than the pool)
#      still gets overflow pools for sets of 16 of it once the app's pool is full; 8000 and 9000
#      asked again are refused without new pools (the refusal memo is per layout AND size; on
#      f092d81 and before it was per layout: the set of 16 was refused).
# Fails on the server before the fix (5bfb052): 448 overflow pools, the cache 172K -> 774K; and on
# a4e691e (merges stopped, nothing else): the nullcache cache 35K -> 894K, never back to the file.
# Prints one line per step and "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
work=$(mktemp -d)
fails=0 steps=0
step() { steps=$((steps+1)); if [ "$1" = 0 ]; then echo "  ok   $2"; else echo "  FAIL $2"; fails=$((fails+1)); fi; }
srv=
start_server() { # $1: name, rest: env for the server; CDIR=<name>: the cache directory of another step
    local name=$1; shift
    sock=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-heapleak-$$-$name.sock
    env -u VKBRIDGE_FAKE_MISSING -u VKBRIDGE_HIDE_EXTS -u VKBRIDGE_MODE "$@" \
        VK_DRIVER_FILES=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json} VK_ICD_FILENAMES= \
        "$out/vkbridge-server" --socket "$sock" --log "$work/$name.log" --cache-dir "$work/c-${CDIR:-$name}" >/dev/null 2>&1 &
    srv=$!
    for _ in $(seq 50); do [ -S "$sock" ] && break; sleep 0.1; done
}
stop_server() { kill $srv 2>/dev/null; wait $srv 2>/dev/null; rm -f "$sock"; srv=; }
trap '[ -n "$srv" ] && kill $srv 2>/dev/null; [ -n "$KEEP" ] && echo "kept $work" || rm -rf "$work"' EXIT
run_client() { # $1: log name, rest: args
    local name=$1; shift
    VKBRIDGE_SOCKET=$sock VK_DRIVER_FILES=$out/vkbridge_icd.json VK_ICD_FILENAMES= HEAPLEAK_SERVER_PID=$srv \
        timeout 300 "$out/heapleak_check" "$@" >$work/$name.out 2>$work/$name.client.log
}
unset VKBRIDGE_HIDE_EXTS VKBRIDGE_MAX_API VKBRIDGE_SYNC

start_server pools
run_client pools pools
step $? "heapleak_check pools: $(tail -1 $work/pools.out)"
stop_server
made=$(sed -n 's/.*device [0-9]*: \([0-9]*\) overflow descriptor pools made.*/\1/p' $work/pools.log | sort -n | tail -1)
# 6 empty app pools (one each, kept empty) + 3 for the small pool's 4 pools' worth + 1 for the 64 sets
# allocated again after frees (allocations go on from the last overflow pool) + 1 when the set too big
# for it is asked with all of them full
step $([ -n "$made" ] && [ "$made" -le 11 ]; echo $?) "overflow pools made: ${made:-none} (at most 11; the old server: 64 for each empty pool)"
again=$(sed -n 's/.*overflow descriptor pools made, \([0-9]*\) allocation.*/\1/p' $work/pools.log | tail -1)
step $([ "${again:-0}" -ge 120 ]; echo $?) "the oversized sets refused without new pools: ${again:-0} (at least 120)"

start_server varcount
run_client varcount varcount
step $? "heapleak_check varcount: $(grep -E '^FAIL ' $work/varcount.out | head -2 | tr '\n' ' ')$(tail -1 $work/varcount.out)"
vmade=$(sed -n 's/.*device [0-9]*: \([0-9]*\) overflow descriptor pools made.*/\1/p' $work/varcount.log | sort -n | tail -1)
step $([ -n "$vmade" ] && [ "$vmade" -le 2 ]; echo $?) "varcount: overflow pools made: ${vmade:-none} (at most 2: the empty one that refused 8000, used then for the sets of 16)"
stop_server

start_server frames VKBRIDGE_PCACHE_MAX_MB=0.05
run_client frames frames ${HEAPLEAK_FRAMES:-10000}
step $? "heapleak_check frames: $(tail -1 $work/frames.out)"
a=$(sed -n 's/^server anon at 2000: \([0-9]*\) KB/\1/p' $work/frames.out)
b=$(sed -n 's/^server anon at end: \([0-9]*\) KB/\1/p' $work/frames.out)
lim=${HEAPLEAK_MAX_KB:-4096}
step $([ -n "$a" ] && [ -n "$b" ] && [ $((b - a)) -lt $lim ]; echo $?) "server anonymous memory from frame 2000 to the end: ${a:-?} -> ${b:-?} KB (+$((${b:-0} - ${a:-0})) KB, under $lim)"
pc=$(grep -o "pipeline caches [0-9]*K" $work/frames.log | sed 's/[^0-9]//g')
pc_over=$(sed -n 's/.*would be \([0-9]*\) KB.*over the .* limit for one program.*/\1/p' $work/frames.log | head -1)
pc_last=$(echo "$pc" | tail -1)
step $([ -n "$pc_over" ] && [ -n "$pc_last" ] && [ "$pc_last" -le $((${pc_over:-0} + 64)) ]; echo $?) "the server's own cache stays where it went over the limit: ${pc_over:-?}K -> ${pc_last:-?}K"
stop_server

# 3. nullcache: pipelines made without an app cache (DXVK, vkd3d-proton) go into the server's own;
#    past its limit it is never saved, and on the old server it grew by every new pipeline for the
#    rest of the session. A first run saves NULLCACHE_SEEDS pipelines to the file; the second has the
#    limit just over that file, makes a new pipeline every frame (every 50th twice) and every 500th
#    frame 20 of the file's again. The cache must go back to the file once it is half the limit over
#    it, stay bounded, and still give the file's pipelines and this session's repeats (creation
#    feedback, MESA_SHADER_CACHE_DISABLE so that only the pipeline cache can give them).
nf=${HEAPLEAK_NULL_FRAMES:-10000}
start_server nullseed MESA_SHADER_CACHE_DISABLE=true
run_client nullseed nullcache 0 seed
step $? "heapleak_check nullcache seed: $(tail -1 $work/nullseed.out)"
stop_server
f=$(ls "$work"/c-nullseed/*heapleak*.vkpc 2>/dev/null | head -1)
sz=$(stat -c %s "$f" 2>/dev/null || echo 0)
lim_mb=$(awk -v b="$sz" 'BEGIN {printf "%.4f", (b + 8192) / 1048576}')
CDIR=nullseed start_server nullplay MESA_SHADER_CACHE_DISABLE=true VKBRIDGE_CACHE_FEEDBACK=1 VKBRIDGE_PCACHE_MAX_MB=$lim_mb
VKBRIDGE_CACHE_FEEDBACK=1 run_client nullplay nullcache $nf play
step $? "heapleak_check nullcache play: $(tail -1 $work/nullplay.out)"
stop_server
lim_k=$(awk -v m="$lim_mb" 'BEGIN {printf "%d", m * 1024}')
pcs=$(grep -o "pipeline caches [0-9]*K" $work/nullplay.log | sed 's/[^0-9]//g')
pc_max=$(echo "$pcs" | sort -n | tail -1)
pc_last=$(echo "$pcs" | tail -1)
reloads=$(grep -c "past its limit .* back to what the file holds" $work/nullplay.log)
# Between two saves (10 s) the frames add ~2500 pipelines: the bound is the ceiling (1.5 x the
# limit) plus what one save period adds, NULLCACHE_TICK_KB (RADV keeps ~85 bytes a pipeline of
# this shader: ~210 KB). The old server (a4e691e): 35K -> 894K, growing to the end.
tick=${NULLCACHE_TICK_KB:-400}
step $([ "$reloads" -ge 1 ]; echo $?) "the cache went back to the file past the limit (${lim_k}K): $reloads times (at least 1)"
step $([ -n "$pc_max" ] && [ "$pc_max" -le $((lim_k * 3 / 2 + tick)) ]; echo $?) "the server's own cache stays bounded: at most ${pc_max:-?}K, last ${pc_last:-?}K (bound $((lim_k * 3 / 2 + tick))K; the file ${lim_k}K)"
fb=$(sed -n 's/.*device [0-9]*: \([0-9]*\) pipelines built, \([0-9]*\) of them found in the pipeline cache.*/\1 \2/p' $work/nullplay.log | sort -n | tail -1)
built=${fb% *} hits=${fb#* }
sa=$(sed -n 's/^seeds asked again: //p' $work/nullplay.out)
rp=$(sed -n 's/^repeats: //p' $work/nullplay.out)
want=$(( ${sa:-0} + ${rp:-0} * 9 / 10 ))
step $([ -n "$fb" ] && [ -n "$sa" ] && [ "${hits:-0}" -ge "$want" ] && [ "${hits:-0}" -le $((${sa:-0} + ${rp:-0})) ]; echo $?) "still found in the cache: ${hits:-?} of ${built:-?} pipelines (the file's ${sa:-?} asked again + this session's ${rp:-?} repeats; at least $want)"
na=$(sed -n 's/^server anon at 2000: \([0-9]*\) KB/\1/p' $work/nullplay.out)
nb=$(sed -n 's/^server anon at end: \([0-9]*\) KB/\1/p' $work/nullplay.out)
step $([ -n "$na" ] && [ -n "$nb" ] && [ $((nb - na)) -lt $lim ]; echo $?) "server anonymous memory from frame 2000 to the end: ${na:-?} -> ${nb:-?} KB (+$((${nb:-0} - ${na:-0})) KB, under $lim)"

if [ $fails = 0 ]; then echo "OK: $steps steps"; else echo "FAILED: $fails of $steps steps"; fi
[ $fails = 0 ]
