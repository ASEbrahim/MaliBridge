#!/bin/bash
# Per-game presets (server_presets.c) on the desktop driver, with a server of its own per step
# (socket, cache directory, session folder); the clients' Steam app ids from SteamAppId:
#   1  two clients with different app ids get their own presets (<cache dir>/presets/<appid>.env):
#      one after the other and both at once, each device with its own compile watchdog limit and
#      only its game's pipeline cache off; a client without an app id gets neither; one log line
#      per game; the session report lists each process's preset (Markdown and JSON);
#   2  precedence, one key (VKBRIDGE_COMPILE_TIMEOUT_S): the built-in preset (imitated:
#      VKBRIDGE_PRESET_FAKE_BUILTIN) over the mode's default, the file over the built-in preset
#      key by key, the server's environment over both, <cache dir>/env over that;
#   3  VKBRIDGE_MODE in a preset: that game's devices in that mode over Setup's switch (their
#      errata and watchdog by it), another game's in Setup's;
#   4  bad lines (not KEY=VALUE, a switch that is not per-game, too long) ignored with a warning,
#      the good lines around them taken;
#   5  the client's switch (VKBRIDGE_DESC_BATCH) from a preset reaches the client through
#      vkbQueryServer (fewer messages), and the guest's own VKBRIDGE_DESC_BATCH=0 still wins;
#   6  the protocol hash is unchanged.
# Prints one line per step and "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
work=$(mktemp -d)
sock=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-presets-$$.sock
srv=
log=
stop() { [ -n "$srv" ] && kill $srv 2>/dev/null && wait $srv 2>/dev/null; srv=; }
# start <name> [env...]: a server of its own, cache $work/<name>/c (kept between starts of one
# name), log $log (new each start)
start() {
  local d=$work/$1
  shift
  stop
  rm -f "$sock"
  mkdir -p "$d/c/presets" "$d/s"
  log=$d/s/vkbridge-server.log
  : > "$log"
  rm -f "$d"/s/malibridge-report.*
  env -u VKBRIDGE_FAKE_MISSING -u VKBRIDGE_HIDE_EXTS -u VKBRIDGE_ERRATA -u VKBRIDGE_ERRATA_IMITATE -u VKBRIDGE_ERRATA_IMITATE_VB \
      -u VKBRIDGE_MODE -u VKBRIDGE_FAKE_DRIVER_VERSION -u VKBRIDGE_ERRATA_PROBE -u VKBRIDGE_PROBE_FAKE -u VKBRIDGE_WAIT_FAKE_ERROR \
      -u VKBRIDGE_COMPILE_TIMEOUT_S -u VKBRIDGE_COMPILE_WARN_S -u VKBRIDGE_EMULATE -u VKBRIDGE_PROBE -u VKBRIDGE_NO_PCACHE \
      -u VKBRIDGE_DESC_BATCH -u VKBRIDGE_NO_DESC_BATCH -u VKBRIDGE_QUERY_MIRROR -u VKBRIDGE_PRESET_FAKE_BUILTIN \
      VKBRIDGE_PROBE_BUDGET_S=0 "$@" VK_DRIVER_FILES=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json} VK_ICD_FILENAMES= \
      "$out/vkbridge-server" --socket "$sock" --log "$log" --cache-dir "$d/c" >/dev/null 2>&1 &
  srv=$!
  for _ in $(seq 300); do [ -S "$sock" ] && break; kill -0 $srv 2>/dev/null || break; sleep 0.05; done
}
# device [env...]: a device (the smoke test); $new: the log lines it added
device() {
  local l; l=$(wc -l < "$log")
  timeout 120 env "$@" "$out/vkbridge-smoke" >"$work/smoke.log" 2>&1
  new=$(tail -n +$((l+1)) "$log")
}
report() { # report <name>: waits for the session report's Markdown; prints its path
  local r=$work/$1/s/malibridge-report.md
  for _ in $(seq 100); do grep -q "^## Startup probe" "$r" 2>/dev/null && break; sleep 0.1; done
  echo "$r"
}
limit_of() { echo "$new" | grep -o 'compile watchdog: device [0-9]* interrupts after [0-9]* s ([^)]*)' | tail -1 | sed 's/.*after //'; }
trap 'stop; rm -f "$sock"; [ -n "$KEEP" ] && echo "kept $work" || rm -rf "$work"' EXIT
export VKBRIDGE_SOCKET=$sock VK_DRIVER_FILES=$out/vkbridge_icd.json VK_ICD_FILENAMES=
unset VKBRIDGE_HIDE_EXTS VKBRIDGE_MAX_API VKBRIDGE_FAKE_MISSING VKBRIDGE_MODE VKBRIDGE_DESC_BATCH VKBRIDGE_NO_DESC_BATCH VKBRIDGE_QUERY_MIRROR
unset SteamAppId SteamGameId VKBRIDGE_PCACHE_KEY
fails=0 steps=0
step() { steps=$((steps+1)); if [ "$1" = 0 ]; then echo "  ok   $2"; else echo "  FAIL $2"; fails=$((fails+1)); fi; }

# 1
mkdir -p $work/two/c/presets
printf '# Stardew-like\nVKBRIDGE_COMPILE_TIMEOUT_S=33\nVKBRIDGE_NO_PCACHE=1\n' > $work/two/c/presets/111.env
printf 'VKBRIDGE_COMPILE_TIMEOUT_S=44\n' > $work/two/c/presets/222.env
start two
device SteamAppId=111; a=$(limit_of); a_pc=$(echo "$new" | grep -c "persistent pipeline cache off (VKBRIDGE_NO_PCACHE)")
device SteamAppId=222; b=$(limit_of); b_pc=$(echo "$new" | grep -c "persistent pipeline cache off (VKBRIDGE_NO_PCACHE)")
device; n=$(limit_of); n_pc=$(echo "$new" | grep -c "persistent pipeline cache off")
[ "$a" = "33 s (VKBRIDGE_COMPILE_TIMEOUT_S, the game's preset file)" ] && [ "$b" = "44 s (VKBRIDGE_COMPILE_TIMEOUT_S, the game's preset file)" ] &&
  [ -z "$n" ] && [ $a_pc$b_pc$n_pc = 100 ]
r1=$?
# both at once: one device each, neither's preset in the other's
l=$(wc -l < "$log")
SteamAppId=111 timeout 120 "$out/vkbridge-smoke" >$work/p1.log 2>&1 & p1=$!
SteamAppId=222 timeout 120 "$out/vkbridge-smoke" >$work/p2.log 2>&1 & p2=$!
wait $p1; e1=$?; wait $p2; e2=$?
new=$(tail -n +$((l+1)) "$log")
[ $e1$e2 = 00 ] && [ "$(echo "$new" | grep -c 'interrupts after 33 s')" = 1 ] && [ "$(echo "$new" | grep -c 'interrupts after 44 s')" = 1 ] &&
  [ "$(echo "$new" | grep -c 'persistent pipeline cache off')" = 1 ]
r2=$?
[ "$(grep -c 'preset for steam111: VKBRIDGE_COMPILE_TIMEOUT_S=33 VKBRIDGE_NO_PCACHE=1 (from file)$' $log)" = 1 ] &&
  [ "$(grep -c 'preset for steam222: VKBRIDGE_COMPILE_TIMEOUT_S=44 (from file)$' $log)" = 1 ]
r3=$?
rep=$(report two)
for _ in $(seq 50); do grep -q "preset: none" $rep 2>/dev/null && break; sleep 0.1; done
grep -q '^- preset: `VKBRIDGE_COMPILE_TIMEOUT_S=33 VKBRIDGE_NO_PCACHE=1 (from file)`$' $rep && grep -q '^- preset: `VKBRIDGE_COMPILE_TIMEOUT_S=44 (from file)`$' $rep &&
  grep -q '^- preset: none$' $rep &&
  python3 -c '
import json, sys
ps = {p["key"]: p["preset"] for p in json.load(open(sys.argv[1]))["processes"]}
assert ps.get("steam111") == "VKBRIDGE_COMPILE_TIMEOUT_S=33 VKBRIDGE_NO_PCACHE=1 (from file)", ps
assert ps.get("steam222") == "VKBRIDGE_COMPILE_TIMEOUT_S=44 (from file)" and ps.get("") == "", ps' ${rep%.md}.json
r4=$?
[ $r1$r2$r3$r4 = 0000 ]
step $? "two app ids: ${a%% (*} and ${b%% (*}, pipeline cache off for 111 only, none without an id ($r1); at once ($r2); logged once each ($r3); in the report ($r4)"

# 2
fake="VKBRIDGE_PRESET_FAKE_BUILTIN=333:VKBRIDGE_COMPILE_TIMEOUT_S=51 VKBRIDGE_NO_PCACHE=1;999:VKBRIDGE_COMPILE_TIMEOUT_S=9"
start prec "$fake"
device SteamAppId=333; p1=$(limit_of)
echo "$new" | grep -q "persistent pipeline cache off"; p1pc=$?
grep -q 'preset for steam333: VKBRIDGE_COMPILE_TIMEOUT_S=51 VKBRIDGE_NO_PCACHE=1 (from built-in)$' $log; p1l=$?
echo "VKBRIDGE_COMPILE_TIMEOUT_S=52" > $work/prec/c/presets/333.env
start prec "$fake"
device SteamAppId=333; p2=$(limit_of)
echo "$new" | grep -q "persistent pipeline cache off"; p2pc=$?
grep -q 'preset for steam333: VKBRIDGE_COMPILE_TIMEOUT_S=52 (from file), VKBRIDGE_NO_PCACHE=1 (from built-in)$' $log; p2l=$?
start prec "$fake" VKBRIDGE_COMPILE_TIMEOUT_S=53
device SteamAppId=333; p3=$(limit_of)
grep -q "interrupt after 53 s (VKBRIDGE_COMPILE_TIMEOUT_S;" $log && grep -q "; overridden: VKBRIDGE_COMPILE_TIMEOUT_S=53 from the environment$" $log; p3l=$?
echo "VKBRIDGE_COMPILE_TIMEOUT_S=54" > $work/prec/c/env
start prec "$fake" VKBRIDGE_COMPILE_TIMEOUT_S=53
device SteamAppId=333; p4=$(limit_of)
grep -q "interrupt after 54 s (VKBRIDGE_COMPILE_TIMEOUT_S;" $log && ! grep -q "interrupt after 53 s" $log; p4l=$?
rm -f $work/prec/c/env $work/prec/c/presets/333.env
start prec
device SteamAppId=333; p5=$(limit_of)
grep -q "interrupt after 10 s (safe mode" $log && ! grep -q "preset for" $log; p5l=$?
[ "$p1" = "51 s (VKBRIDGE_COMPILE_TIMEOUT_S, the game's built-in preset)" ] && [ $p1pc$p1l = 00 ] &&
  [ "$p2" = "52 s (VKBRIDGE_COMPILE_TIMEOUT_S, the game's preset file)" ] && [ $p2pc$p2l = 00 ] &&
  [ -z "$p3" ] && [ $p3l = 0 ] && [ -z "$p4" ] && [ $p4l = 0 ] && [ -z "$p5" ] && [ $p5l = 0 ]
step $? "precedence: built-in ${p1%% (*}, file ${p2%% (*} (built-in's other key kept), environment 53 ($p3l), <cache dir>/env 54 ($p4l), none: the mode's 10 s ($p5l)"

# 3
mkdir -p $work/mode/c/presets
echo safe > $work/mode/c/mode
echo "VKBRIDGE_MODE=performance" > $work/mode/c/presets/555.env
start mode "VKBRIDGE_PRESET_FAKE_BUILTIN=556:VKBRIDGE_MODE=performance"
device SteamAppId=555
echo "$new" | grep -q "mode: performance (the game's preset file)" && echo "$new" | grep -q "errata: device .*performance mode" &&
  echo "$new" | grep -q "compile watchdog: interrupt after 20 s now (performance mode)"; m1l=$?
device SteamAppId=777; m2=$(limit_of)
echo "$new" | grep -q "mode: safe (Setup) - changed" && echo "$new" | grep -q "errata: device .*safe mode"; m2l=$?
device SteamAppId=556
echo "$new" | grep -q "mode: performance (the game's built-in preset)"; m3l=$?
echo "VKBRIDGE_MODE=safe" > $work/mode/c/env
start mode
device SteamAppId=555
echo "$new" | grep -q "mode: safe (VKBRIDGE_MODE in <cache dir>/env)" || grep -q "mode: safe (VKBRIDGE_MODE in <cache dir>/env)" $log; m4l=$?
[ -z "$m2" ] && [ $m1l$m2l$m3l$m4l = 0000 ]
step $? "VKBRIDGE_MODE in a preset: 555 performance, its errata and 20 s watchdog ($m1l), 777 Setup's safe ($m2l), built-in ($m3l), <cache dir>/env still wins ($m4l)"

# 4
mkdir -p $work/bad/c/presets
{
  echo "VKBRIDGE_COMPILE_TIMEOUT_S=61"
  echo "this is not a setting"
  echo "lower_case=1"
  echo "=5"
  echo "VKBRIDGE_LAYER_GS_FOLD=1"
  printf 'VKBRIDGE_ERRATA=%0600d\n' 0
  echo "   VKBRIDGE_NO_PCACHE=1   "
} > $work/bad/c/presets/888.env
start bad
device SteamAppId=888; b1=$(limit_of)
f=$work/bad/c/presets/888.env
grep -q "preset: $f line 2: \"this is not a setting\" is not KEY=VALUE; ignored" $log && grep -q "preset: $f line 3: \"lower_case=1\" is not KEY=VALUE" $log &&
  grep -q "preset: $f line 4: \"=5\" is not KEY=VALUE" $log && grep -q "preset: $f line 5: VKBRIDGE_LAYER_GS_FOLD is not a per-game switch" $log &&
  grep -q "preset: $f line 6: .* is \(too long\|not KEY=VALUE\)" $log && echo "$new" | grep -q "persistent pipeline cache off" &&
  [ "$b1" = "61 s (VKBRIDGE_COMPILE_TIMEOUT_S, the game's preset file)" ] &&
  grep -q 'preset for steam888: VKBRIDGE_COMPILE_TIMEOUT_S=61 VKBRIDGE_NO_PCACHE=1 (from file)$' $log
r1=$?
device SteamAppId=888
[ "$(grep -c "preset: $f line 2:" $log)" = 1 ]
r2=$?
[ $r1$r2 = 00 ]
step $? "bad lines: $(grep -c "^.*preset: $f line" $log) warnings, the good lines taken ($r1), each warned once ($r2)"

# 5
mkdir -p $work/client/c/presets
echo "VKBRIDGE_DESC_BATCH=1" > $work/client/c/presets/444.env
start client
msgs() { VKBRIDGE_STATS=100000 timeout 120 env "$@" "$out/desc_traffic_check" 2>&1 | grep -o '[0-9]* messages' | head -1; }
mp=$(msgs SteamAppId=444); mn=$(msgs SteamAppId=445); mg=$(msgs SteamAppId=444 VKBRIDGE_DESC_BATCH=0); mb=$(msgs SteamAppId=445 VKBRIDGE_DESC_BATCH=1)
# batched: a few hundred messages (not always the same count), unbatched: tens of thousands
[ -n "$mp" ] && [ -n "$mn" ] && [ -n "$mb" ] && [ $(( ${mp% *} * 10 )) -lt ${mn% *} ] && [ $(( ${mb% *} * 10 )) -lt ${mn% *} ] && [ "$mg" = "$mn" ]
step $? "client switch: VKBRIDGE_DESC_BATCH=1 in 444's preset $mp (guest's =1: $mb), none $mn, guest's =0 wins $mg"

# 6
stop
grep -q "^#define VKB_PROTOCOL_HASH 0x4a9da939fdf65896ULL" ../common/gen/vkb_gen.h
step $? "protocol hash $(grep -o '0x[0-9a-f]*' ../common/gen/vkb_gen.h | head -1)"

if [ $fails = 0 ]; then echo "OK: $steps steps"; else echo "FAILED: $fails of $steps steps"; exit 1; fi
