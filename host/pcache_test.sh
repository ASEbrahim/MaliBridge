#!/bin/bash
# Persistent pipeline cache (server_pcache.c) on the desktop driver, with a server of its own
# (its own socket and cache directory; the suite's server is left alone):
#   1  a Steam game's cache is keyed by its app id and saved (vkDestroyDevice);
#   2  a killed game keeps what it compiled, its own pipeline caches included (process cleanup);
#   3  Steam's shader pre-caching (fossilize_replay on shadercache/<id>/...) saves to the game's
#      .dl/.run files; the game takes them in at start and they go once its own file holds them;
#   4  processes saving the same file add to each other's work instead of replacing it;
#   5  only the first two empty caches an app makes start from the program's cache, and
#      EXTERNALLY_SYNCHRONIZED ones (Zink's per-program caches) never;
#   6  a game that had a cache by process name starts from it;
#   7  the cache directory removed under the running server (Android empties an app's cache
#      directory when storage runs out): the next save makes it again, said once, no failed saves;
#   8  a full storage (a 1 MB tmpfs, in a user namespace of its own; skipped where there is none):
#      said once, the server keeps running, and the cache is saved once there is room again.
# Prints one line per step and "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}  # VKB_OUT=out-tsan for the ThreadSanitizer build
work=$(mktemp -d)
cache=$work/c
sock=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-pcache-$$.sock
log=$work/server.log
VK_DRIVER_FILES=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json} VK_ICD_FILENAMES= \
    "$out/vkbridge-server" --socket "$sock" --log "$log" --cache-dir "$cache" >/dev/null 2>&1 &
srv=$!
trap 'kill $srv 2>/dev/null; rm -f "$sock"; rm -rf "$work"' EXIT
for _ in $(seq 50); do [ -S "$sock" ] && break; sleep 0.1; done
export VKBRIDGE_SOCKET=$sock VK_DRIVER_FILES=$out/vkbridge_icd.json VK_ICD_FILENAMES=
unset SteamAppId SteamGameId VKBRIDGE_PCACHE_KEY
fails=0
step() { if [ "$1" = 0 ]; then echo "  ok   $2"; else echo "  FAIL $2"; fails=$((fails+1)); fi; }
size() { local f; f=$(ls "$cache"/*-"$1".vkpc 2>/dev/null | head -1); if [ -n "$f" ]; then stat -c %s "$f"; else echo 0; fi; }
have() { ls "$cache"/*-"$1".vkpc >/dev/null 2>&1; }

# 1
SteamAppId=4242 "$out/pcache_check" --count 40 >$work/1.log 2>&1 && have steam4242 && grep -q "is pcache_check@steam4242" "$log"
step $? "Steam game keyed by app id: $(size steam4242) bytes for 40 pipelines"
s40=$(size steam4242)

# 2 killed: nothing destroyed; pipelines in the device's cache, and in a cache the app made
SteamAppId=4243 "$out/pcache_check" --count 40 --no-destroy >$work/2a.log 2>&1
SteamAppId=4244 "$out/pcache_check" --count 40 --app-caches 1 --no-destroy >$work/2b.log 2>&1
sleep 1
[ "$s40" -gt 0 ] && [ "$(size steam4243)" -ge $((s40 * 9 / 10)) ] && [ "$(size steam4244)" -ge $((s40 * 9 / 10)) ]
step $? "killed game keeps its cache: $(size steam4243) bytes (no cache of its own), $(size steam4244) bytes (its own cache)"

# 3 pre-caching as Steam runs it: the archive path names the game, the process is fossilize_repla
fz=$work/steamapps/shadercache/4245/fozpipelinesv6
mkdir -p "$fz/steamapprun_pipeline_cache.0123456789abcdef"
ln -s "$out/pcache_check" "$work/fossilize_replay"
"$work/fossilize_replay" "$fz/steam_pipeline_cache.foz" --master-process --count 60 >$work/3a.log 2>&1
"$work/fossilize_replay" "$fz/steamapprun_pipeline_cache.0123456789abcdef/steamapp_pipeline_cache.foz" --first 500 --count 20 >$work/3b.log 2>&1
dl=$(size steam4245.dl); run=$(size steam4245.run)
SteamAppId=4245 "$out/pcache_check" --first 0 --count 10 >$work/3c.log 2>&1
grep -q "taken in from .*steam4245.dl.vkpc" "$log" && grep -q "taken in from .*steam4245.run.vkpc" "$log" &&
    [ "$dl" -gt 0 ] && [ "$run" -gt 0 ] && [ "$(size steam4245)" -gt "$dl" ] && [ "$(size steam4245)" -gt "$run" ] && ! have steam4245.dl && ! have steam4245.run
step $? "pre-caching: .dl $dl + .run $run bytes taken in by the game ($(size steam4245) bytes), then removed"

# 4 two processes on one file at once, each saving while the other runs (a file is a header and
#   then its entries: one replacing the other's would leave one process's size)
SteamAppId=4246 "$out/pcache_check" --first 0 --count 40 --hold 12 >$work/4a.log 2>&1 &
a=$!
sleep 2
SteamAppId=4246 "$out/pcache_check" --first 1000 --count 40 --hold 1 >$work/4b.log 2>&1
wait $a
[ "$(size steam4246)" -ge $((s40 * 3 / 2)) ] && grep -q "with what another process saved" "$log"
step $? "two processes, one file: $(size steam4246) bytes (one alone: $s40)"

# 5 seeding: of four empty caches two start from the program's cache; EXTERNALLY_SYNCHRONIZED none
SteamAppId=4242 "$out/pcache_check" --count 1 --app-caches 4 >$work/5a.log 2>&1
SteamAppId=4242 "$out/pcache_check" --count 1 --app-caches 2 --ext-sync >$work/5b.log 2>&1
[ "$(grep -c 'the app makes several pipeline caches' "$log")" = 2 ]
step $? "seeding limited ($(grep -c 'the app makes several pipeline caches' "$log") devices stopped seeding)"

# 6 the cache by process name (before keys) is where a keyed game starts
"$out/pcache_check" --first 2000 --count 20 >$work/6a.log 2>&1
SteamAppId=4247 "$out/pcache_check" --first 2000 --count 20 >$work/6b.log 2>&1
grep -q "steam4247.vkpc ([0-9]* KB loaded from .*pcache_check.vkpc)" "$log"
step $? "a keyed game starts from its cache by process name"

# 7 removed under a running game, as on the tablet: its periodic saves (every 10 s) and the one at
#   its end, then a second removal and a new game (the directory comes back at its device)
SteamAppId=4248 "$out/pcache_check" --first 3000 --count 40 --hold 14 >$work/7a.log 2>&1 &
a=$!
sleep 2
rm -rf "$cache"
wait $a
n7=$(grep -c "cache directory .* was removed; recreated" "$log")
have steam4248 && [ "$n7" = 1 ] && ! grep -q "pipeline cache not saved" "$log"
step $? "cache directory removed under a running game: recreated (said $n7 time(s)), $(size steam4248) bytes saved"
rm -rf "$cache"
SteamAppId=4249 "$out/pcache_check" --first 3100 --count 10 >$work/7b.log 2>&1
have steam4249 && [ "$(grep -c "cache directory .* was removed; recreated" "$log")" = 2 ]
step $? "removed again: recreated at the next game ($(size steam4249) bytes)"

# 8 a full storage
if unshare -rm true 2>/dev/null; then
  unshare -rm bash -c '
    out=$1; work=$2; sock=$3.full
    mkdir -p "$work/full" && mount -t tmpfs -o size=1m tmpfs "$work/full" || exit 2
    cache=$work/full/c; log=$work/server-full.log
    VK_DRIVER_FILES=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json} VK_ICD_FILENAMES= \
        "$out/vkbridge-server" --socket "$sock" --log "$log" --cache-dir "$cache" >/dev/null 2>&1 &
    srv=$!
    trap "kill $srv 2>/dev/null; rm -f $sock" EXIT
    for _ in $(seq 50); do [ -S "$sock" ] && break; sleep 0.1; done
    export VKBRIDGE_SOCKET=$sock VK_DRIVER_FILES=$out/vkbridge_icd.json VK_ICD_FILENAMES= VKBRIDGE_PROBE=0
    dd if=/dev/zero of="$work/full/filler" bs=4k count=1000 2>/dev/null # every block but a few
    SteamAppId=4250 "$out/pcache_check" --first 4000 --count 80 --hold 22 >"$work/8a.log" 2>&1 || exit 3
    SteamAppId=4251 "$out/pcache_check" --first 4100 --count 80 >"$work/8b.log" 2>&1 || exit 4
    nospace=$(grep -c "no space left on the device" "$log"); notsaved=$(grep -c "pipeline cache not saved" "$log")
    rm -f "$work/full/filler"
    SteamAppId=4252 "$out/pcache_check" --first 4200 --count 20 >"$work/8c.log" 2>&1 || exit 5
    kill -0 $srv || exit 6
    ls "$cache"/*-steam4252.vkpc >/dev/null 2>&1 || exit 7
    grep -q "writes go through again" "$log" || exit 8
    echo "$nospace $notsaved" > "$work/8.res"
    [ "$nospace" = 1 ] && [ "$notsaved" = 0 ] || exit 9
  ' _ "$out" "$work" "$sock"
  r=$?
  step $r "full storage: said $(cut -d" " -f1 $work/8.res 2>/dev/null) time(s), $(cut -d" " -f2 $work/8.res 2>/dev/null) other save warnings, saved once there was room (exit $r)"
  [ $r = 0 ] || grep -E "space|not saved|recreated|cache saved" "$work/server-full.log" | head -20
else
  echo "  skip full storage (no user namespaces)"
fi

grep -E "process [0-9]+ is|pipeline cache|taken in|removed|several|cache directory" "$log" > "$out/t-pcache-server.log"
if [ $fails = 0 ]; then echo "OK: persistent pipeline cache"; else echo "FAILED: $fails step(s) (server log in $out/t-pcache-server.log)"; exit 1; fi
