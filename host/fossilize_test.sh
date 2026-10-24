#!/bin/bash
# Steam's shader pre-caching end to end, with Steam's own fossilize_replay and a DXVK game
# (d3d11_check.exe under Proton), through a server imitating Mali (mali_env.sh) with a cache
# directory of its own:
#   1  the game recorded through the bridge by Steam's fossilize layer (what Steam keeps of a game
#      run on this device: shadercache/<id>/fozpipelinesv6/steamapprun_pipeline_cache.*/), replayed
#      as Steam replays it (--master-process: the replayer's child processes compile): every
#      pipeline in it compiles into the game's .run file, and the game's next start takes it in and
#      finds those pipelines in the cache (the driver's creation feedback, server_guard.c, turned on
#      here with VKBRIDGE_CACHE_FEEDBACK=1);
#   2  the same game recorded on the desktop driver itself, which has graphics pipeline libraries
#      (DXVK's GPL path: library pipelines with INDEPENDENT_SETS layouts, beside the optimized
#      pipelines it builds whole), as an archive Steam downloads (shadercache/<id>/fozpipelinesv6/
#      steam_pipeline_cache.foz): the library pipelines are skipped (the bridge offers no GPL) and the
#      whole pipelines compile into the .dl file, and they are the ones the game asks for here (how
#      many of them DXVK's background compiler finished before the short run ended varies);
#   3  a game without pre-caching finds none of them (the control).
# Needs VKB_WINE_DIR (d3d11_test.sh), Steam's fossilize_replay (VKB_FOSSILIZE_REPLAY, else Steam's
# ubuntu12_64 one) and, for 2, Steam's fossilize layer; prints "OK: skipped (...)" without them.
cd "$(dirname "$0")"
replay=${VKB_FOSSILIZE_REPLAY:-}
for c in "$HOME/.local/share/Steam/ubuntu12_64/fossilize_replay" "$HOME/.steam/steam/ubuntu12_64/fossilize_replay"; do
  [ -z "$replay" ] && [ -x "$c" ] && replay=$c
done
[ -n "${VKB_WINE_DIR:-}" ] && [ -f "$VKB_WINE_DIR/app/d3d11_check.exe" ] || { echo "OK: skipped (no VKB_WINE_DIR with d3d11_check.exe)"; exit 0; }
[ -n "$replay" ] && [ -x "$replay" ] || { echo "OK: skipped (no fossilize_replay; VKB_FOSSILIZE_REPLAY names one)"; exit 0; }
layer=0
for j in "$HOME/.local/share/vulkan/implicit_layer.d/steamfossilize_x86_64.json" /usr/share/vulkan/implicit_layer.d/steamfossilize_x86_64.json; do
  [ -f "$j" ] && layer=1
done
[ "$layer" = 1 ] || { echo "OK: skipped (no Steam fossilize layer to record with)"; exit 0; }

work=$(mktemp -d "${TMPDIR:-/tmp}/vkb-foz-work.XXXX")
run=$(mktemp -d "${XDG_RUNTIME_DIR:-/tmp}/vkb-foz.XXXX")
mkdir -p "$work/rec" # the layer makes no directories
export VKBRIDGE_SOCKET=$run/s.sock VKB_SERVER_LOG=$work/server.log
unset SteamAppId SteamGameId VKBRIDGE_PCACHE_KEY VKBRIDGE_CACHE_DIR VKB_SERVER_PID
. ./server_start.sh
trap 'vkb_server_stop; [ -n "${VKB_KEEP:-}" ] || rm -rf "$work"; rm -rf "$run"' EXIT
# creation feedback (off by default) counts the cache hits
VKB_SERVER_ENV="VKBRIDGE_CACHE_DIR=$work/c MESA_SHADER_CACHE_DISABLE=true VKBRIDGE_CACHE_FEEDBACK=1" . ./mali_env.sh >/dev/null
fails=0
step() { if [ "$1" = 0 ]; then echo "  ok   $2"; else echo "  FAIL $2"; fails=$((fails+1)); fi; }
# The devices whose pipeline cache is the file of key $1 (steam<id>, steam<id>.run, ...) and their
# "N pipelines built, M of them found in the pipeline cache" lines, summed: "M/N"
hits() {
  awk -v k="-$1.vkpc" '
    $4 == "device" && $6 == "pipeline" && $7 == "cache" && index($0, k) {dev[$5] = 1}
    /pipelines built, .* found in the pipeline cache/ && ($5 in dev) {m += $9; n += $6}
    END {print m + 0 "/" n + 0}' "$VKB_SERVER_LOG"
}
replay_into() { # archive: as Steam runs it on the tablet
  "$replay" "$1" --master-process --num-threads 2 --spirv-val --timeout-seconds 30 >"$2" 2>&1
}
size() { local f; f=$(ls "$work/c"/*-"$1".vkpc 2>/dev/null | head -1); if [ -n "$f" ]; then stat -c %s "$f"; else echo 0; fi; }

# 1 recorded through the bridge, replayed into .run
SteamAppId=4269 ENABLE_VK_LAYER_VALVE_steam_fossilize_1=1 STEAM_FOSSILIZE_DUMP_PATH=$work/rec/run ./d3d11_test.sh >$work/1a.log 2>&1
fz=$work/steamapps/shadercache/4270/fozpipelinesv6/steamapprun_pipeline_cache.0123456789abcdef
mkdir -p "$fz" && cp "$(ls "$work"/rec/run.*.foz | head -1)" "$fz/steamapp_pipeline_cache.foz" 2>/dev/null
replay_into "$fz/steamapp_pipeline_cache.foz" "$work/1b.log"
played=$(grep -o "Playing back [0-9]* graphics" "$work/1b.log" | awk '{s += $3} END {print s + 0}')
skipped=$(grep -c "not supported\|Skipping" "$work/1b.log")
grep -q "is fossilize_repla@steam4270.run" "$VKB_SERVER_LOG" && [ "$(size steam4270.run)" -gt 0 ] && [ "$played" -gt 0 ] && [ "$skipped" = 0 ]
step $? "recorded through the bridge: $played graphics pipelines replayed into steam4270.run ($(size steam4270.run) bytes), $skipped skipped"
SteamAppId=4270 ./d3d11_test.sh >$work/1c.log 2>&1
h1=$(hits steam4270)
grep -q "^OK" "$work/1c.log" && grep -q "taken in from .*steam4270.run.vkpc" "$VKB_SERVER_LOG"
step $? "the game took the .run file in ($(grep -o '^OK.*' "$work/1c.log"))"

# 3 the control: the same game, nothing pre-cached
SteamAppId=4272 ./d3d11_test.sh >$work/3.log 2>&1
h3=$(hits steam4272)
[ "${h1%/*}" -ge $((${h3%/*} + played)) ]
step $? "found in the cache: $h1 pipelines after pre-caching, $h3 without (+$played wanted)"

# 2 recorded on the desktop driver with GPL (descriptor buffers off: the bucket of Steam's
#   downloads for a GPU without them), replayed into .dl
VK_DRIVER_FILES=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json} DXVK_CONFIG="dxvk.enableDescriptorBuffer=False;dxvk.enableDescriptorHeap=False" \
  ENABLE_VK_LAYER_VALVE_steam_fossilize_1=1 STEAM_FOSSILIZE_DUMP_PATH=$work/rec/gpl ./d3d11_test.sh >$work/2a.log 2>&1
fz=$work/steamapps/shadercache/4271/fozpipelinesv6
mkdir -p "$fz" && cp "$(ls "$work"/rec/gpl.*.foz | head -1)" "$fz/steam_pipeline_cache.foz" 2>/dev/null
replay_into "$fz/steam_pipeline_cache.foz" "$work/2b.log"
played2=$(grep -o "Playing back [0-9]* graphics" "$work/2b.log" | awk '{s += $3} END {print s + 0}')
libs=$(grep -o "Pipeline layout [0-9a-f]* is not supported" "$work/2b.log" | sort -u | wc -l)
grep -q "is fossilize_repla@steam4271.dl" "$VKB_SERVER_LOG" && [ "$(size steam4271.dl)" -gt 0 ] && [ "$played2" -gt 0 ] && [ "$libs" -gt 0 ]
step $? "recorded with GPL: $played2 whole pipelines replayed into steam4271.dl, $libs INDEPENDENT_SETS layouts (library pipelines) skipped"
SteamAppId=4271 ./d3d11_test.sh >$work/2c.log 2>&1
h2=$(hits steam4271)
grep -q "^OK" "$work/2c.log" && grep -q "taken in from .*steam4271.dl.vkpc" "$VKB_SERVER_LOG" && [ "${h2%/*}" -ge $((${h3%/*} + played2)) ]
step $? "the game found the downloaded archive's pipelines: $h2 in the cache ($h3 without)"

if [ $fails = 0 ]; then echo "OK: pre-caching saves the game $played of its pipelines' compiles (bridge archive), $((${h2%/*} - ${h3%/*})) (GPL archive)"; else
  echo "FAILED: $fails step(s)"; grep -E "is .*@steam|taken in|pipelines built" "$VKB_SERVER_LOG" | tail -20; exit 1; fi
