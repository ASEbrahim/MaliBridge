#!/bin/bash
# What the app is shown (client/physdev.c): VKBRIDGE_HEAP_MAX_MB and VKBRIDGE_SPOOF_GPU, on the
# desktop driver with a server of its own per step (gpuview_check prints the GPU as the app sees
# it, and itself checks that Properties/Properties2 and MemoryProperties/2 agree and that no
# VK_EXT_memory_budget budget is above its heap's size):
#   1  nothing set: the identity and heaps the native driver gives (same vendor, device, name, heaps);
#   2  the guest's VKBRIDGE_HEAP_MAX_MB=1024: every device-local heap 1024 MB, budget at most that,
#      other heaps as they are; =0 off;
#   3  the guest's VKBRIDGE_SPOOF_GPU=nvidia / amd: vendor 0x10de/0x1c03 / 0x1002/0x67df and the
#      name; driverID and driverName stay the real ones; the server's own GPU line stays the real
#      GPU; an unknown value warned, the real GPU shown;
#   4  a preset file (<cache dir>/presets/<appid>.env) with both keys reaches its game only, through
#      vkbQueryServer; the guest's own VKBRIDGE_HEAP_MAX_MB=0 / VKBRIDGE_SPOOF_GPU=off still win;
#   5  a built-in preset (imitated, tested on this GPU) applies both; <cache dir>/env's
#      VKBRIDGE_HEAP_MAX_MB=0 beats it; an untested one is not applied and said with its lines;
#      a bad value in a preset warned and not applied;
#   6  the real built-in entries (presets_builtin.inc) for Garry's Mod, DS:R, Poppy Playtime and
#      The Forest: untested, so not applied on this GPU, and said with their lines;
#   7  the safe-mode default for devices with 12800 MB of RAM or less (VKBRIDGE_FAKE_MEMTOTAL_MB):
#      7680 MB -> 3584, 8704 -> 4096, 5000 -> 2048 (the floor), 11377 (the XRing O3, #18) and
#      12800 -> 4096, logged once; 12801 MB, performance mode, <cache dir>/env's
#      VKBRIDGE_HEAP_MAX_MB=0 and the guest's =0 leave the heaps alone, and an explicit 1024 wins;
#   8  the protocol hash is unchanged;
#   9  the budget follows the memory really free (server_memory.c, VKBRIDGE_FAKE_MEMAVAIL_MB stands in for
#      MemAvailable): plenty free (1 TB) or VKBRIDGE_BUDGET_RESERVE_MB=0: every device-local budget as the
#      driver's (reserve 0, the old behaviour); 6000 MB free: 6000 - 1536 reserve; 3000 MB free with a
#      512 MB reserve: 2488; 2000 MB free: the 1024 MB floor, never below; usage never under the budget's
#      reading and the budget never above the driver's.
#  10  the default cap follows the memory free when the process starts (VKBRIDGE_FAKE_MEMAVAIL_MB/QUWD):
#      11350 MB of RAM with 2850 MB free -> 2560, 3507 -> 3072, 1500 -> 2048 (the floor), 4096 and 1 TB -> 4096 (no
#      change, nothing logged); 7322 MB with 2185 free -> 2048; VKBRIDGE_HEAP_FOLLOW_FREE=0, an explicit
#      VKBRIDGE_HEAP_MAX_MB=4096 in <cache dir>/env and the guest's =0 win; 15000 MB of RAM and performance mode: no
#      cap as before. The lowered cap is logged once per process with the MB free.
# Prints one line per step and "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
work=$(mktemp -d)
sock=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-heapcap-$$.sock
host_icd=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json}
srv=
log=
stop() { [ -n "$srv" ] && kill $srv 2>/dev/null && wait $srv 2>/dev/null; srv=; }
start() { # start <name> [env...]: a server of its own, cache $work/<name>/c (kept), log $log (new)
  local d=$work/$1
  shift
  stop
  rm -f "$sock"
  mkdir -p "$d/c/presets" "$d/s"
  log=$d/s/vkbridge-server.log
  : > "$log"
  env -u VKBRIDGE_FAKE_MISSING -u VKBRIDGE_HIDE_EXTS -u VKBRIDGE_MODE -u VKBRIDGE_PRESET_FAKE_BUILTIN \
      -u VKBRIDGE_HEAP_MAX_MB -u VKBRIDGE_SPOOF_GPU -u VKBRIDGE_EMULATE -u VKBRIDGE_FAKE_MEMTOTAL_MB \
      VKBRIDGE_PROBE_BUDGET_S=0 "$@" VK_DRIVER_FILES=$host_icd VK_ICD_FILENAMES= \
      "$out/vkbridge-server" --socket "$sock" --log "$log" --cache-dir "$d/c" >/dev/null 2>&1 &
  srv=$!
  for _ in $(seq 300); do [ -S "$sock" ] && break; kill -0 $srv 2>/dev/null || break; sleep 0.05; done
}
# view [env...]: gpuview_check through the bridge; $v its output, $rc its status
view() { v=$(timeout 60 env VKBRIDGE_LOG_LEVEL=2 "$@" "$out/gpuview_check" 2>&1); rc=$?; }
gpu_of() { echo "$1" | grep -m1 '^gpu '; }
drv_of() { echo "$1" | grep -m1 '^driver '; }
heaps_of() { echo "$1" | grep '^heap ' | sed 's/ budget_mb=.*//'; }
# every device-local heap $2 MB with a budget at most that (when there is one), the others as in $3 (native)
capped() {
  local out=$1 mb=$2 nat=$3 ok=0 n=0
  while read -r _ i size local budget _; do
    n=$((n+1))
    local ns; ns=$(echo "$nat" | grep "^heap $i " | grep -o 'size_mb=[0-9]*')
    if [ "${local#local=}" = 1 ]; then
      # budget_mb=-: the device shows no VK_EXT_memory_budget (no Mali driver has it)
      [ "${size#size_mb=}" = "$mb" ] && { [ "${budget#budget_mb=}" = - ] || [ "${budget#budget_mb=}" -le "$mb" ] 2>/dev/null; } || ok=1
    else
      [ "$size" = "$ns" ] || ok=1
    fi
  done < <(echo "$out" | grep '^heap ')
  [ $n -gt 0 ] && return $ok
}
trap 'stop; rm -f "$sock"; [ -n "$KEEP" ] && echo "kept $work" || rm -rf "$work"' EXIT
export VKBRIDGE_SOCKET=$sock VK_DRIVER_FILES=$out/vkbridge_icd.json VK_ICD_FILENAMES=
unset VKBRIDGE_HIDE_EXTS VKBRIDGE_MAX_API VKBRIDGE_FAKE_MISSING VKBRIDGE_MODE VKBRIDGE_HEAP_MAX_MB VKBRIDGE_SPOOF_GPU
unset SteamAppId SteamGameId VKBRIDGE_PCACHE_KEY
fails=0 steps=0
step() { steps=$((steps+1)); if [ "$1" = 0 ]; then echo "  ok   $2"; else echo "  FAIL $2"; fails=$((fails+1)); fi; }

nat=$(VK_DRIVER_FILES=$host_icd timeout 60 "$out/gpuview_check" 2>&1) || { echo "FAILED: native gpuview_check: $nat"; exit 1; }
real_gpu=$(gpu_of "$nat")
real_drv=$(drv_of "$nat")
big=$(echo "$nat" | grep 'local=1' | grep -o 'size_mb=[0-9]*' | sed 's/size_mb=//' | sort -n | head -1)

# 1
start plain
view
[ $rc = 0 ] && [ "$(gpu_of "$v")" = "$real_gpu" ] && [ "$(drv_of "$v")" = "$real_drv" ] && [ "$(heaps_of "$v")" = "$(heaps_of "$nat")" ]
step $? "nothing set: ${real_gpu#gpu }, heaps as the driver's ($(echo "$nat" | grep -c '^heap ') heaps; $(tail -1 <<<"$v"))"

# 2
view VKBRIDGE_HEAP_MAX_MB=1024; v1=$v; r1=$rc
capped "$v1" 1024 "$nat"; c1=$?
view VKBRIDGE_HEAP_MAX_MB=0; v0=$v; r0=$rc
[ $r1$c1$r0 = 000 ] && [ "${big:-0}" -gt 1024 ] && [ "$(heaps_of "$v0")" = "$(heaps_of "$nat")" ] &&
  echo "$v1" | grep -q "memory: device-local heap [0-9]* reported as 1024 MB, not [0-9]* MB (the guest's VKBRIDGE_HEAP_MAX_MB)"
step $? "guest VKBRIDGE_HEAP_MAX_MB=1024: device-local heaps $(echo "$v1" | grep 'local=1' | sed 's/.*size_mb=\([0-9]*\).*budget_mb=\([0-9]*\).*/\1 (budget \2)/' | paste -sd, -) MB, was $big; =0 off ($r1$c1$r0)"

# 3
view VKBRIDGE_SPOOF_GPU=nvidia; vn=$v; rn=$rc
view VKBRIDGE_SPOOF_GPU=amd; va=$v; ra=$rc
view VKBRIDGE_SPOOF_GPU=intel; vx=$v; rx=$rc
[ $rn$ra$rx = 000 ] && [ "$(gpu_of "$vn")" = "gpu vendor=0x10de device=0x1c03 name=NVIDIA GeForce GTX 1060 6GB" ] &&
  [ "$(gpu_of "$va")" = "gpu vendor=0x1002 device=0x67df name=AMD Radeon RX 480 Graphics" ] &&
  [ "$(drv_of "$vn")" = "$real_drv" ] && [ "$(drv_of "$va")" = "$real_drv" ] && [ "$(heaps_of "$vn")" = "$(heaps_of "$nat")" ] &&
  [ "$(gpu_of "$vx")" = "$real_gpu" ] && echo "$vx" | grep -q "VKBRIDGE_SPOOF_GPU=intel is not nvidia, amd or off" &&
  echo "$vn" | grep -q "GPU shown to the app as NVIDIA GeForce GTX 1060 6GB (vendor 0x10de, device 0x1c03), not ${real_gpu#*name=}" &&
  grep -q "GPU: ${real_gpu#*name=} (vendor 0x" "$log" && ! grep -q "GeForce\|RX 480" "$log"
step $? "guest VKBRIDGE_SPOOF_GPU: nvidia $(gpu_of "$vn" | cut -d' ' -f2-3), amd $(gpu_of "$va" | cut -d' ' -f2-3), ${real_drv} kept, the server's GPU real, intel warned ($rn$ra$rx)"

# 4
mkdir -p $work/file/c/presets
printf 'VKBRIDGE_HEAP_MAX_MB=2048\nVKBRIDGE_SPOOF_GPU=amd\n' > $work/file/c/presets/4100.env
start file
view SteamAppId=4100; vf=$v; rf=$rc
capped "$vf" 2048 "$nat"; cf=$?
view SteamAppId=4101; vo=$v; ro=$rc
view SteamAppId=4100 VKBRIDGE_HEAP_MAX_MB=0 VKBRIDGE_SPOOF_GPU=off; vg=$v; rg=$rc
[ $rf$cf$ro$rg = 0000 ] && [ "$(gpu_of "$vf" | cut -d' ' -f2)" = vendor=0x1002 ] && [ "$(gpu_of "$vf")" != "$real_gpu" ] &&
  echo "$vf" | grep -q "(the server's VKBRIDGE_HEAP_MAX_MB (the game's preset or <cache dir>/env))" &&
  [ "$(gpu_of "$vo")" = "$real_gpu" ] && [ "$(heaps_of "$vo")" = "$(heaps_of "$nat")" ] &&
  [ "$(gpu_of "$vg")" = "$real_gpu" ] && [ "$(heaps_of "$vg")" = "$(heaps_of "$nat")" ] &&
  grep -q 'preset for steam4100: VKBRIDGE_HEAP_MAX_MB=2048 VKBRIDGE_SPOOF_GPU=amd (from file)$' "$log"
step $? "preset file: 4100 heaps 2048 MB and amd ($rf$cf), 4101 as the driver's ($ro), the guest's =0/off win ($rg)"

# 5
fake="VKBRIDGE_PRESET_FAKE_BUILTIN=4200@RADV,llvmpipe,Mali-G720:VKBRIDGE_HEAP_MAX_MB=3072 VKBRIDGE_SPOOF_GPU=nvidia;4201:VKBRIDGE_HEAP_MAX_MB=3072;4202@RADV,llvmpipe:VKBRIDGE_SPOOF_GPU=voodoo VKBRIDGE_HEAP_MAX_MB=lots"
start bi "$fake"
view SteamAppId=4200; vb=$v; rb=$rc
capped "$vb" 3072 "$nat"; cb=$?
view SteamAppId=4201; vu=$v; ru=$rc
view SteamAppId=4202; vw=$v; rw=$rc
grep -q 'preset for steam4200: VKBRIDGE_HEAP_MAX_MB=3072 VKBRIDGE_SPOOF_GPU=nvidia (from built-in)$' "$log" &&
  grep -q 'preset for steam4201: a built-in preset exists but was not tested on .*; not applied. To try it, copy it to .*/presets/4201.env: VKBRIDGE_HEAP_MAX_MB=3072$' "$log" &&
  grep -q 'VKBRIDGE_SPOOF_GPU=voodoo is not nvidia, amd or off; not applied' "$log" && grep -q 'VKBRIDGE_HEAP_MAX_MB=lots is not a number of MB; not applied' "$log"
l5=$?
echo "VKBRIDGE_HEAP_MAX_MB=0" > $work/bi/c/env
start bi "$fake"
view SteamAppId=4200; ve=$v; re=$rc
[ $rb$cb$ru$rw$re$l5 = 000000 ] && [ "$(gpu_of "$vb" | cut -d' ' -f2)" = vendor=0x10de ] &&
  [ "$(heaps_of "$vu")" = "$(heaps_of "$nat")" ] && [ "$(gpu_of "$vw")" = "$real_gpu" ] && [ "$(heaps_of "$vw")" = "$(heaps_of "$nat")" ] &&
  [ "$(heaps_of "$ve")" = "$(heaps_of "$nat")" ] && [ "$(gpu_of "$ve" | cut -d' ' -f2)" = vendor=0x10de ] &&
  grep -q "overridden: VKBRIDGE_HEAP_MAX_MB=0 from <cache dir>/env" "$log"
step $? "built-in: tested 3072 MB and nvidia ($rb$cb), untested not applied and said ($ru, log $l5), bad values warned ($rw), <cache dir>/env's =0 over it ($re)"

# 6
start real
for id in 4000 570940 1721470 242760; do view SteamAppId=$id; [ $rc = 0 ] && [ "$(heaps_of "$v")" = "$(heaps_of "$nat")" ] || { echo "  $id: $v"; r6=1; }; done
grep -q 'preset for steam4000: a built-in preset exists but was not tested on .*: VKBRIDGE_HEAP_MAX_MB=4096$' "$log" &&
  grep -q 'preset for steam570940: a built-in preset exists but was not tested on .*: VKBRIDGE_HEAP_MAX_MB=4096 VKBRIDGE_BC_TRANSCODE_QUALITY=fast$' "$log" &&
  grep -q 'preset for steam1721470: a built-in preset exists but was not tested on .*: VKBRIDGE_HEAP_MAX_MB=4096 VKBRIDGE_BC_TRANSCODE_QUALITY=fast$' "$log" &&
  grep -q 'preset for steam242760: a built-in preset exists but was not tested on .*: VKBRIDGE_HEAP_MAX_MB=4096$' "$log" && [ -z "$r6" ]
step $? "built-in entries for 4000, 570940, 1721470, 242760: untested, not applied, said with their lines"

# 7
dl='heap cap: safe mode with %s MB of RAM (12800 or less): device-local heaps shown as at most %s MB (VKBRIDGE_HEAP_MAX_MB=0 turns this off)'
r7= d7=
for pair in 7680:3584 8704:4096 5000:2048 11377:4096 12800:4096; do
  ram=${pair%:*} want=${pair#*:}
  start ram$ram VKBRIDGE_FAKE_MEMTOTAL_MB=$ram VKBRIDGE_FAKE_MEMAVAIL_MB=1000000
  view; c=1; [ $rc = 0 ] && capped "$v" $want "$nat" && c=0
  view; [ $rc = 0 ] && capped "$v" $want "$nat" || c=1
  [ $c = 0 ] && [ "${big:-0}" -gt $want ] && [ "$(grep -cF "$(printf "$dl" $ram $want)" "$log")" = 1 ] &&
    echo "$v" | grep -q "reported as $want MB, not [0-9]* MB (the server's VKBRIDGE_HEAP_MAX_MB" || r7="$r7 $ram"
  d7="$d7 $ram->$want"
done
view VKBRIDGE_HEAP_MAX_MB=0; [ $rc = 0 ] && [ "$(heaps_of "$v")" = "$(heaps_of "$nat")" ] || r7="$r7 guest0"
view VKBRIDGE_HEAP_MAX_MB=1024; [ $rc = 0 ] && capped "$v" 1024 "$nat" || r7="$r7 guest1024"
start over VKBRIDGE_FAKE_MEMTOTAL_MB=12801 VKBRIDGE_FAKE_MEMAVAIL_MB=1000000
view; [ $rc = 0 ] && [ "$(heaps_of "$v")" = "$(heaps_of "$nat")" ] && ! grep -q 'heap cap: safe mode' "$log" || r7="$r7 12801"
start perf VKBRIDGE_FAKE_MEMTOTAL_MB=7680 VKBRIDGE_FAKE_MEMAVAIL_MB=1000000 VKBRIDGE_MODE=performance
view; [ $rc = 0 ] && [ "$(heaps_of "$v")" = "$(heaps_of "$nat")" ] && ! grep -q 'heap cap: safe mode' "$log" || r7="$r7 perf"
mkdir -p $work/env0/c && echo "VKBRIDGE_HEAP_MAX_MB=0" > $work/env0/c/env
start env0 VKBRIDGE_FAKE_MEMTOTAL_MB=7680 VKBRIDGE_FAKE_MEMAVAIL_MB=1000000
view; [ $rc = 0 ] && [ "$(heaps_of "$v")" = "$(heaps_of "$nat")" ] && ! grep -q 'heap cap: safe mode' "$log" || r7="$r7 env0"
[ -z "$r7" ]
step $? "safe-mode default for 8 and 12 GB devices:$d7 MB, logged once; 12801 MB, performance mode, <cache dir>/env =0 and the guest's =0 off, the guest's 1024 wins${r7:+ (failed:$r7)}"

# 8
stop
grep -q "^#define VKB_PROTOCOL_HASH 0x339e552905c4871cULL" ../common/gen/vkb_gen.h
step $? "protocol hash $(grep -o '0x[0-9a-f]*' ../common/gen/vkb_gen.h | head -1)"

# 9
# budgets_of <output>: the device-local heaps' budgets in MB, space separated
budgets_of() { echo "$1" | grep 'local=1' | sed 's/.*budget_mb=\([0-9]*\).*/\1/' | paste -sd' ' -; }
# near <value> <want> <slack>: value in [want - 4, want + slack]
near() { [ "$1" -ge $(($2 - 4)) ] && [ "$1" -le $(($2 + $3)) ]; }
r9=
start mem0 VKBRIDGE_FAKE_MEMAVAIL_MB=1000000; view; v_plenty=$v; rc_plenty=$rc
grep -q 'memory budget: heap' "$log" && r9="$r9 plenty-logged"
start mem1 VKBRIDGE_FAKE_MEMAVAIL_MB=2000 VKBRIDGE_BUDGET_RESERVE_MB=0; view; v_off=$v; rc_off=$rc
grep -q 'memory budget: heap' "$log" && r9="$r9 off-logged"
start mem2 VKBRIDGE_FAKE_MEMAVAIL_MB=6000; view; v_6g=$v; rc_6g=$rc
start mem3 VKBRIDGE_FAKE_MEMAVAIL_MB=3000 VKBRIDGE_BUDGET_RESERVE_MB=512; view; v_3g=$v; rc_3g=$rc
start mem4 VKBRIDGE_FAKE_MEMAVAIL_MB=2000; view; v_2g=$v; rc_2g=$rc
grep -q 'memory budget: heap [0-9]* told 1024 MB, not the driver' "$log" || r9="$r9 floor-not-logged"
[ $rc_plenty$rc_off$rc_6g$rc_3g$rc_2g = 00000 ] || r9="$r9 rc"
nb=$(budgets_of "$v_plenty")
[ -n "$nb" ] || r9="$r9 nobudget"
nsz=$(echo "$v_plenty" | grep 'local=1' | grep -o 'size_mb=[0-9]*' | sed 's/size_mb=//' | paste -sd' ' -)
i=0
for b in $nb; do
  i=$((i+1))
  sz=$(echo $nsz | cut -d' ' -f$i)
  off=$(echo "$(budgets_of "$v_off")" | cut -d' ' -f$i)
  b6=$(echo "$(budgets_of "$v_6g")" | cut -d' ' -f$i)
  b3=$(echo "$(budgets_of "$v_3g")" | cut -d' ' -f$i)
  b2=$(echo "$(budgets_of "$v_2g")" | cut -d' ' -f$i)
  # the driver's budget moves a little between runs (RADV: the VRAM in use); never more than 256 MB apart
  [ $((b - off)) -le 256 ] && [ $((off - b)) -le 256 ] || r9="$r9 plenty!=off($b,$off)"
  w6=$((6000 - 1536)); [ $off -lt $w6 ] && w6=$off; [ $sz -lt $w6 ] && w6=$sz
  near $b6 $w6 160 || r9="$r9 6g($b6,want $w6)"
  w3=$((3000 - 512)); [ $off -lt $w3 ] && w3=$off; [ $sz -lt $w3 ] && w3=$sz
  near $b3 $w3 160 || r9="$r9 3g($b3,want $w3)"
  w2=1024; [ $off -lt $w2 ] && w2=$off
  near $b2 $w2 0 || r9="$r9 2g($b2,want $w2)"
  [ $b6 -le $((off + 256)) ] && [ $b3 -le $((off + 256)) ] && [ $b2 -le $((off + 256)) ] || r9="$r9 raised"
done
[ $i -gt 0 ] && [ "${big:-0}" -gt 6000 ] || r9="$r9 heap-too-small($big)"
step $([ -z "$r9" ] && echo 0 || echo 1) "memory budget follows free memory: 1 TB free or reserve 0 = the driver's ($nb), 6000 free -> $(budgets_of "$v_6g"), 3000 free with reserve 512 -> $(budgets_of "$v_3g"), 2000 free -> $(budgets_of "$v_2g") (the 1 GB floor)${r9:+ (failed:$r9)}"

# 10
fl='heap cap: process [0-9]* (gpuview_check) shown device-local heaps of at most %s MB, not %s: %s MB free when it started (VKBRIDGE_HEAP_FOLLOW_FREE=0 turns this off)'
r10= d10=
for t in 11350:2850:2560 11350:3507:3072 11350:1500:2048 11350:4096:4096 11350:1000000:4096 7322:2185:2048; do
  IFS=: read -r ram fr want <<<"$t"
  start fr$ram-$fr VKBRIDGE_FAKE_MEMTOTAL_MB=$ram VKBRIDGE_FAKE_MEMAVAIL_MB=$fr
  view; c=1; [ $rc = 0 ] && capped "$v" $want "$nat" && c=0
  cap=$( [ $ram = 7322 ] && echo 3584 || echo 4096 )
  if [ $want -lt $cap ]; then [ "$(grep -c "$(printf "$fl" $want $cap $fr)" "$log")" = 1 ] || c=1
  else grep -q 'shown device-local heaps of at most' "$log" && c=1; fi
  [ $c = 0 ] || r10="$r10 $ram/$fr"
  d10="$d10 $fr->$(echo "$v" | grep 'local=1' | grep -o 'size_mb=[0-9]*' | sed 's/size_mb=//' | paste -sd, -)"
done
start ffoff VKBRIDGE_FAKE_MEMTOTAL_MB=11350 VKBRIDGE_FAKE_MEMAVAIL_MB=2850 VKBRIDGE_HEAP_FOLLOW_FREE=0
view; [ $rc = 0 ] && capped "$v" 4096 "$nat" && ! grep -q 'shown device-local heaps of at most' "$log" || r10="$r10 follow0"
mkdir -p $work/ffexp/c && echo "VKBRIDGE_HEAP_MAX_MB=4096" > $work/ffexp/c/env
start ffexp VKBRIDGE_FAKE_MEMTOTAL_MB=11350 VKBRIDGE_FAKE_MEMAVAIL_MB=2850
view; [ $rc = 0 ] && capped "$v" 4096 "$nat" && ! grep -q 'shown device-local heaps of at most' "$log" || r10="$r10 explicit"
start ffguest VKBRIDGE_FAKE_MEMTOTAL_MB=11350 VKBRIDGE_FAKE_MEMAVAIL_MB=2850
view VKBRIDGE_HEAP_MAX_MB=0; [ $rc = 0 ] && [ "$(heaps_of "$v")" = "$(heaps_of "$nat")" ] || r10="$r10 guest0"
start ff16 VKBRIDGE_FAKE_MEMTOTAL_MB=15000 VKBRIDGE_FAKE_MEMAVAIL_MB=2850
view; [ $rc = 0 ] && [ "$(heaps_of "$v")" = "$(heaps_of "$nat")" ] && ! grep -q 'heap cap:' "$log" || r10="$r10 16gb"
start ffperf VKBRIDGE_FAKE_MEMTOTAL_MB=11350 VKBRIDGE_FAKE_MEMAVAIL_MB=2850 VKBRIDGE_MODE=performance
view; [ $rc = 0 ] && [ "$(heaps_of "$v")" = "$(heaps_of "$nat")" ] && ! grep -q 'heap cap:' "$log" || r10="$r10 perf"
[ -z "$r10" ]
step $? "default cap follows the memory free at the start (MB free -> heaps):$d10; FOLLOW_FREE=0, explicit 4096, guest 0, 15000 MB RAM, performance: as before${r10:+ (failed:$r10)}"
stop

if [ $fails = 0 ]; then echo "OK: $steps steps"; else echo "FAILED: $fails of $steps steps"; exit 1; fi
