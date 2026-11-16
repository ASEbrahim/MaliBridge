#!/bin/bash
# Steam's shader pre-caching through one server (a player log: 8 fossilize_replay processes, server heap 607M -> 3186M,
# 'pipeline caches 399142K', std::bad_alloc in the driver 0.3 s after a merged save): what the server's pipeline
# caches hold while 8 replay processes compile, on lavapipe with a layer under the server whose pipeline caches hold
# memory as Mali's do (tests/pcache_mem_layer.c, PCMEM_KB per pipeline; lavapipe's own hold none).
#   1  8 replay processes (pcache_check named fossilize_replay, a downloaded archive: key steam4300.dl), 600
#      pipelines each at 10 a second (two background save rounds and the saves at their ends), no cache of their own:
#      the server's heap at its peak over what it is once they are gone = what the caches held; and how much of
#      other processes' work each save loaded ('with what another process saved' lines);
#   2  the same with a cache of their own that they compile into (--app-caches 1);
#   3  the game then starts: what it takes in from the pre-caching (all 4800 pipelines, as before);
#   4  a limit the replay goes over (VKBRIDGE_PCACHE_MAX_MB=24): what is kept is never replaced by one process's
#      own (no file shrinks), and the game takes in what was kept;
#   5  a game's own device (not pre-caching), 600 pipelines with no cache of its own: the same saves and the same
#      file as before (compare the line between builds).
# SRV_OUT=<dir>: the server and client ICD to test (another build's host/out); the test programs and the layer are
# this tree's. Prints one line per step, the numbers, and "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
srv_out=${SRV_OUT:-$out}
LVP=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/lvp_icd.json}
work=$(mktemp -d)
trap 'for p in $srvs; do kill $p 2>/dev/null; done; if [ -n "$KEEP" ]; then echo "kept $work"; else rm -rf "$work"; fi' EXIT
unset SteamAppId SteamGameId VKBRIDGE_PCACHE_KEY WAYLAND_DISPLAY DISPLAY
KB=${PCMEM_KB:-16}
N=${PRECACHE_N:-8}
PER=${PRECACHE_PER:-600}
RATE=${PRECACHE_RATE:-10}
fails=0
srvs=
step() { if [ "$1" = 0 ]; then echo "  ok   $2"; else echo "  FAIL $2"; fails=$((fails+1)); fi; }

# start <tag> [ENV=V ...]: a server of its own (socket, cache dir, log) with the layer under it
start() {
  local tag=$1; shift
  sock=$work/$tag.sock; cache=$work/$tag-c; log=$work/$tag.log
  env "$@" PCMEM_ENTRY=$((KB * 1024)) VK_LAYER_PATH=$out/pcmem VK_INSTANCE_LAYERS=VK_LAYER_VKB_pcache_mem VK_DRIVER_FILES=$LVP VK_ICD_FILENAMES=$LVP \
    VKBRIDGE_PROBE=0 "$srv_out/vkbridge-server" --socket "$sock" --log "$log" --cache-dir "$cache" >$work/$tag.out 2>&1 &
  srv=$!; srvs="$srvs $srv"
  for _ in $(seq 100); do [ -S "$sock" ] && break; sleep 0.1; done
}
stop() { kill $srv 2>/dev/null; wait $srv 2>/dev/null; }
cl() { VKBRIDGE_SOCKET=$sock VK_DRIVER_FILES=$srv_out/vkbridge_icd.json VK_ICD_FILENAMES= "$@"; }
# replay <archive id> <args...>: N processes at once, each its own range of pipelines
replay() {
  local id=$1; shift
  local fz=$work/steamapps/shadercache/$id/fozpipelinesv6 pids= i
  mkdir -p "$fz"; ln -sf "$out/pcache_check" "$work/fossilize_replay"
  for i in $(seq 0 $((N-1))); do
    cl "$work/fossilize_replay" "$fz/steam_pipeline_cache.foz" --first $((i*100000)) --count $PER --rate $RATE --drop "$@" >$work/r-$id-$i.out 2>&1 &
    pids="$pids $!"
  done
  for i in $pids; do wait $i; done
  sleep 6 # a memory line after they are gone
}
# the server's heap (mallinfo, its mem lines; the last one, at the teardown, is left out)
heapmb() { grep -o ' heap [0-9]*M' "$1" | grep -o '[0-9]*' | awk '$1 > 0'; }
pcK() { grep -o 'pipeline caches [0-9]*K' "$1" | grep -o '[0-9]*'; }
maxof() { sort -n | tail -1; }
lastof() { tail -1; }
files() { ls "$cache"/*-"$1"*.vkpc 2>/dev/null; }
sumsize() { local s=0 f; for f in $(files "$1"); do s=$((s + $(stat -c %s "$f"))); done; echo $s; }
# pipelines in a file of the layer's format: (size - the driver's 32-byte header - 16-byte trailer) / entry
entries() { local s; s=$(stat -c %s "$1" 2>/dev/null || echo 0); echo $(( s > 48 ? (s - 48) / (KB * 1024) : 0 )); }

phase() { # <tag> <archive id> <replay args...>: steps 1/2 (+3 for the game)
  local tag=$1 id=$2; shift 2
  start $tag
  replay $id "$@"
  local peak after pc others saves lost
  peak=$(heapmb $log | maxof); after=$(heapmb $log | head -1); pc=$(pcK $log | maxof)
  others=$(grep -c 'with what another process saved' $log)
  saves=$(grep -c 'pipeline cache saved' $log)
  loadKB=$(grep -o 'saved ([0-9]* KB, with what another process saved' $log | grep -o '[0-9]* KB' | grep -o '[0-9]*' | awk '{s+=$1} END {print s+0}')
  savems=$(grep -o 'pipeline cache saved .* in [0-9.]* ms' $log | grep -o 'in [0-9.]* ms' | grep -o '[0-9.]*' | awk '{s+=$1} END {printf "%.0f", s}')
  local made=$((N * PER * KB / 1024))
  echo "  [$tag] replay of $N x $PER pipelines (~$made MB of cache entries): heap first ${after}M, peak ${peak}M (grew ${peak:+$((peak - after))}M); 'pipeline caches' peak ${pc}K; $saves saves, $others merged with another process's file (${loadKB} KB written by those), ${savems} ms saving"
  eval "${tag}_held=$((peak - after)) ${tag}_others=$others ${tag}_made=$made"
  # 3: the game takes in the pre-caching
  cl env SteamAppId=$id "$out/pcache_check" --first 990000 --count 1 >$work/g-$id.out 2>&1
  local gf; gf=$(ls "$cache"/*-steam$id.vkpc 2>/dev/null | head -1)
  local ge; ge=$(entries "$gf")
  local left; left=$(files steam$id. | wc -l)
  echo "  [$tag] the game took in $(grep -c 'taken in from' $log) file(s); its file holds $ge pipelines (of $((N * PER + 1))); pre-caching files left $left"
  eval "${tag}_game=$ge"
  stop
}

# 1, 3
phase none 4300
# 2, 3
phase app 4301 --app-caches 1 --reset-every 50

# 4 over the limit: no file ever shrinks; the game takes in what was kept
start lim VKBRIDGE_PCACHE_MAX_MB=24
( while [ -S "$sock" ]; do echo $(sumsize steam4302.dl); sleep 0.5; done ) >$work/lim-sizes.txt 2>/dev/null &
sampler=$!
replay 4302
kill $sampler 2>/dev/null; wait $sampler 2>/dev/null
# the pre-caching files' total over time: a drop of over a third = work another process saved, replaced
shrank=$(awk '$1 > 0 && p > 0 && $1 < p * 2 / 3 {n++} {if ($1 > 0) p = $1} END {print n+0}' $work/lim-sizes.txt)
kept=$(sumsize steam4302.dl); keptE=0; for f in $(files steam4302.dl); do keptE=$((keptE + $(entries $f))); done
cl env SteamAppId=4302 "$out/pcache_check" --first 990000 --count 1 >$work/g-4302.out 2>&1
gE=$(entries "$(ls "$cache"/*-steam4302.vkpc 2>/dev/null | head -1)")
echo "  [lim] 24 MB limit: pre-caching files $((kept >> 10)) KB ($keptE pipelines); times the files together shrank by over a third: $shrank; the game's file then $gE pipelines"
stop
[ "$keptE" -gt 0 ] && [ "$gE" -ge "$keptE" ] && [ "$shrank" = 0 ]
step $? "over the limit: nothing saved is replaced by one process's own ($shrank shrinking saves), the game takes in all $keptE kept"

# 5 a game's own device: the same as before
start game
cl env SteamAppId=4303 "$out/pcache_check" --first 0 --count $PER --rate 40 --drop --hold 2 >$work/g-4303.out 2>&1
sleep 6
gpeak=$(heapmb $log | maxof); gafter=$(heapmb $log | lastof)
gsaves=$(grep -c 'pipeline cache saved' $log)
gfile=$(entries "$(ls "$cache"/*-steam4303.vkpc 2>/dev/null | head -1)")
echo "  [game] a game's device, $PER pipelines: heap peak ${gpeak}M, $gsaves saves, file $gfile pipelines; lines: $(grep -o 'pipeline cache [^ ]*steam4303.vkpc ([^)]*)' $log | sed 's/.*vkpc //' | head -1)"
stop
[ "$gfile" = "$PER" ]
step $? "a game's own device keeps all its $PER pipelines in its file"

# verdicts for 1-3
# what grows is what was compiled since the last save (30 s of a 60 s replay: about half), not all of it
[ "$none_held" -le $((none_made * 6 / 10)) ] && [ "$none_others" = 0 ]
step $? "1 replay, no caches of its own: the server's heap grew ${none_held} MB of the ${none_made} MB compiled (at most 60%), ${none_others} saves merged another process's file (none)"
[ "$app_held" -le $((app_made * 6 / 10)) ] && [ "$app_others" = 0 ]
step $? "2 replay into caches of its own: grew ${app_held} MB of ${app_made} MB (at most 60%), ${app_others} saves merged another process's file (none)"
[ "$none_game" = $((N * PER + 1)) ] && [ "$app_game" = $((N * PER + 1)) ]
step $? "3 the game takes in every pre-cached pipeline: $none_game and $app_game of $((N * PER + 1))"

if [ "$fails" = 0 ]; then echo "OK: pre-caching memory"; else echo "FAILED: $fails step(s)"; exit 1; fi
