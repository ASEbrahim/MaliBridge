#!/bin/bash
# The bridge's VK_EXT_memory_budget where the driver has none (server_memory.c memb_fill, VKB_EMU_MEM_BUDGET;
# every Mali), on the desktop driver with its own extension hidden (VKBRIDGE_FAKE_MISSING=membudget) and
# MemAvailable read from a file the check program writes (VKBRIDGE_FAKE_MEMAVAIL_FILE). A server per step:
#   1  engine DXVK, 256 MB allocated, hold 0: offered, vkCreateDevice with it works; 8000 MB free: every
#      heap's budget its size, usage 0 (what DXVK shows without the extension); 500: device-local budget the
#      1 GB floor, usage the process's 256 MB; 900: still low (leaves over 1024); 1100: as at 8000; 600: low
#      again; entering and leaving logged;
#   2  1280 MB allocated, reserve 512, 700 free: budget 1280 + 700 - 512 = 1468 MB, usage 1280;
#   3  the default hold (2 s): one query at 500 MB free is not low yet;
#   4  engine vkd3d: offered; no engine, or engine "mesa zink": not offered (unchanged);
#   5  VKBRIDGE_EMU_MEMBUDGET=0 or VKBRIDGE_BUDGET_LOW_MB=0: not offered / never low;
#   6  the driver's own extension (nothing hidden): offered by the driver, the bridge's path not taken
#      (nothing read from the file, nothing logged);
#   (steps 1-5 force the offer with VKBRIDGE_EMU_MEMBUDGET=1: the PC has more than 12800 MB of RAM, no default cap)
# Players' fingerprints (MemTotal, MemAvailable at the game's start, its GPU memory and the free memory / swap in
# its last minutes, from their server logs; VKBRIDGE_FAKE_MEMTOTAL_MB / _MEMAVAIL_MB / _SWAPTOTAL_MB / _SWAPFREE_MB):
#   7  vivo V2515 11350 MB, 2850 free at the start, the server killed: offered (started short), shown
#      heap 2560; 2850 free -> budget 2560 usage 0; 2304 MB held, 327 free -> 2304 + 327 - 1536 = 1095;
#   8  same phone, 3507 free at the start: offered, heap 3072; 2880 held, 467 free -> 1811;
#   9  Ultra 12 GB 11245 MB, 3270 at the start, swap 0 of 12287, the app killed at 834 free: 1431 free
#      (over an eighth of RAM) not low; 834 -> low by the swap rule: 2368 + 834 - 1536 = 1666; 1500 (swap still
#      used up, under a sixth) still low; 2000 -> normal;
#  10  (Pixel 9a 7322 MB, 3174 at the start, MemAvailable 0 and swap 10 of 3661 at the kill): 0 free -> low,
#      1280 held: the 1 GB floor, usage 1280;
#  11  no change where the start was not short: S10+ (11525 MB, 4174 free at the start), Ultra 16 GB (15245 MB,
#      no cap, 2359 free), an explicit VKBRIDGE_HEAP_MAX_MB=4096 (2850 free): not offered;
# Prints one line per step and "OK: ..." / "FAILED: ..." last. VKB_OUT: the build to test (before/after).
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
chk=${MBC_BIN:-$out/membudget_check}
work=$(mktemp -d)
sock=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-membudget-$$.sock
host_icd=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/lvp_icd.json}
srv=
log=
stop() { [ -n "$srv" ] && kill $srv 2>/dev/null && wait $srv 2>/dev/null; srv=; }
start() { # start <name> [env...]: a server of its own, log $log
  local d=$work/$1
  shift
  stop
  rm -f "$sock"
  mkdir -p "$d/c" "$d/s"
  log=$d/s/vkbridge-server.log
  : > "$log"
  env -u VKBRIDGE_FAKE_MISSING -u VKBRIDGE_HIDE_EXTS -u VKBRIDGE_MODE -u VKBRIDGE_HEAP_MAX_MB -u VKBRIDGE_FAKE_MEMAVAIL_MB \
      VKBRIDGE_PROBE_BUDGET_S=0 VKBRIDGE_FAKE_MEMAVAIL_FILE=$work/avail "$@" VK_DRIVER_FILES=$host_icd VK_ICD_FILENAMES= \
      "$out/vkbridge-server" --socket "$sock" --log "$log" --cache-dir "$d/c" >/dev/null 2>&1 &
  srv=$!
  for _ in $(seq 300); do [ -S "$sock" ] && break; kill -0 $srv 2>/dev/null || break; sleep 0.05; done
}
# run [env...]: the check program through the bridge; $v its output, $rc its status
run() { v=$(timeout 60 env VKBRIDGE_LOG_LEVEL=2 MBC_AVAIL_FILE=$work/avail "$@" "$chk" 2>&1); rc=$?; }
# field <avail> <local 0|1> <budget_mb|usage_mb|size_mb>: the first such heap's value at that step
field() { echo "$v" | grep "^step avail=$1 " | grep "local=$2" | head -1 | grep -o "$3=[0-9]*" | cut -d= -f2; }
# nonlocal_ok <avail>: every heap that is not device-local: budget = size, usage 0
nonlocal_ok() {
  local ok=0
  while read -r line; do
    local s b u
    s=$(echo "$line" | grep -o 'size_mb=[0-9]*' | cut -d= -f2); b=$(echo "$line" | grep -o 'budget_mb=[0-9]*' | cut -d= -f2)
    u=$(echo "$line" | grep -o 'usage_mb=[0-9]*' | cut -d= -f2)
    [ "$b" = "$s" ] && [ "$u" = 0 ] || ok=1
  done < <(echo "$v" | grep "^step avail=$1 " | grep 'local=0')
  return $ok
}
near() { [ -n "$1" ] && [ "$1" -ge $(($2 - 4)) ] && [ "$1" -le $(($2 + $3)) ]; }
trap 'stop; rm -f "$sock"; [ -n "$KEEP" ] && echo "kept $work" || rm -rf "$work"' EXIT
export VKBRIDGE_SOCKET=$sock VK_DRIVER_FILES=$out/vkbridge_icd.json VK_ICD_FILENAMES=
unset VKBRIDGE_HIDE_EXTS VKBRIDGE_MAX_API VKBRIDGE_FAKE_MISSING VKBRIDGE_MODE VKBRIDGE_HEAP_MAX_MB
steps=0 fails=0
step() { steps=$((steps+1)); if [ "$1" = 0 ]; then echo "  ok   $steps $2"; else echo "  FAIL $steps $2"; fails=$((fails+1)); fi; }

# 1
r=
start s1 VKBRIDGE_FAKE_MISSING=membudget VKBRIDGE_EMU_MEMBUDGET=1 VKBRIDGE_BUDGET_LOW_HOLD_MS=0
run MBC_ENGINE=DXVK MBC_ALLOC_MB=256 MBC_STEPS=8000,500,900,1100,600
[ $rc = 0 ] || r="$r rc$rc"
echo "$v" | grep -q '^ext=yes' || r="$r not-offered"
echo "$v" | grep -q '^device=ok' || r="$r device"
sz=$(field 8000 1 size_mb)
[ -n "$sz" ] && [ "$(field 8000 1 budget_mb)" = "$sz" ] && [ "$(field 8000 1 usage_mb)" = 0 ] || r="$r 8000($(field 8000 1 budget_mb)/$(field 8000 1 usage_mb) of $sz)"
[ "$(field 500 1 budget_mb)" = 1024 ] && near "$(field 500 1 usage_mb)" 256 64 || r="$r 500($(field 500 1 budget_mb)/$(field 500 1 usage_mb))"
[ "$(field 900 1 budget_mb)" = 1024 ] && near "$(field 900 1 usage_mb)" 256 64 || r="$r 900($(field 900 1 budget_mb)/$(field 900 1 usage_mb))"
[ "$(field 1100 1 budget_mb)" = "$sz" ] && [ "$(field 1100 1 usage_mb)" = 0 ] || r="$r 1100($(field 1100 1 budget_mb)/$(field 1100 1 usage_mb))"
[ "$(field 600 1 budget_mb)" = 1024 ] || r="$r 600($(field 600 1 budget_mb))"
for a in 8000 500 900 1100 600; do nonlocal_ok $a || r="$r nonlocal@$a"; done
[ "$(grep -c 'memory budget: 500 MB free (under 768 MB)' "$log")" = 1 ] || r="$r enter-log"
[ "$(grep -c 'memory budget: 1100 MB free again' "$log")" = 1 ] || r="$r leave-log"
rt=$(echo "$v" | grep -o 'rt_us=[0-9.]*' | cut -d= -f2)
step $([ -z "$r" ] && echo 0 || echo 1) "DXVK: offered, device made with it; 8000 free -> $(field 8000 1 budget_mb)/$(field 8000 1 usage_mb) MB (heap $sz), 500 -> $(field 500 1 budget_mb)/$(field 500 1 usage_mb), 900 -> $(field 900 1 budget_mb), 1100 -> $(field 1100 1 budget_mb)/$(field 1100 1 usage_mb), 600 -> $(field 600 1 budget_mb) (budget/usage); one query ${rt:-?} us${r:+ (failed:$r)}"

# 2
r=
start s2 VKBRIDGE_FAKE_MISSING=membudget VKBRIDGE_EMU_MEMBUDGET=1 VKBRIDGE_BUDGET_LOW_HOLD_MS=0 VKBRIDGE_BUDGET_RESERVE_MB=512
run MBC_ALLOC_MB=1280 MBC_STEPS=700
[ $rc = 0 ] && near "$(field 700 1 budget_mb)" 1468 64 && near "$(field 700 1 usage_mb)" 1280 64 || r=" $(field 700 1 budget_mb)/$(field 700 1 usage_mb) rc$rc"
step $([ -z "$r" ] && echo 0 || echo 1) "1280 MB held, reserve 512, 700 free: budget $(field 700 1 budget_mb) (want 1468), usage $(field 700 1 usage_mb)${r:+ (failed:$r)}"

# 3
r=
start s3 VKBRIDGE_FAKE_MISSING=membudget VKBRIDGE_EMU_MEMBUDGET=1
run MBC_STEPS=500
[ $rc = 0 ] && [ -n "$(field 500 1 size_mb)" ] && [ "$(field 500 1 budget_mb)" = "$(field 500 1 size_mb)" ] && [ "$(field 500 1 usage_mb)" = 0 ] || r=" $(field 500 1 budget_mb)"
grep -q 'MB free (under' "$log" && r="$r logged"
step $([ -z "$r" ] && echo 0 || echo 1) "default 2 s hold: a single query at 500 MB free is not low yet${r:+ (failed:$r)}"

# 4
r=
start s4 VKBRIDGE_FAKE_MISSING=membudget VKBRIDGE_EMU_MEMBUDGET=1
run MBC_ENGINE=vkd3d; echo "$v" | grep -q '^ext=yes' || r="$r vkd3d"
run MBC_ENGINE=-; echo "$v" | grep -q '^ext=no' || r="$r none"
run "MBC_ENGINE=mesa zink"; echo "$v" | grep -q '^ext=no' || r="$r zink"
step $([ -z "$r" ] && echo 0 || echo 1) "vkd3d offered; no engine and Zink not offered${r:+ (failed:$r)}"

# 5
r=
start s5 VKBRIDGE_FAKE_MISSING=membudget VKBRIDGE_EMU_MEMBUDGET=0
run; echo "$v" | grep -q '^ext=no' || r="$r emu0"
start s5b VKBRIDGE_FAKE_MISSING=membudget VKBRIDGE_EMU_MEMBUDGET=1 VKBRIDGE_BUDGET_LOW_MB=0 VKBRIDGE_BUDGET_LOW_HOLD_MS=0
run MBC_STEPS=300
[ "$(field 300 1 budget_mb)" = "$(field 300 1 size_mb)" ] && [ "$(field 300 1 usage_mb)" = 0 ] || r="$r low0($(field 300 1 budget_mb))"
step $([ -z "$r" ] && echo 0 || echo 1) "VKBRIDGE_EMU_MEMBUDGET=0: not offered; VKBRIDGE_BUDGET_LOW_MB=0: never low${r:+ (failed:$r)}"

# 6: the driver as it is, so without the suite's Mali profile (host/mali_profile.sh, default common): the profile
# hides VK_EXT_memory_budget as every Mali lacks it, and this step's subject is a driver that has it
r=
mp_saved=$(declare -p LD_PRELOAD VKB_PROFILE_FAKE_MISSING VKB_PROFILE_HIDE_EXTS VKB_PROFILE_FAKE_API VKB_PROFILE_NOT_IMITATED 2>/dev/null)
mp_pre=; IFS=: read -ra mp_libs <<< "${LD_PRELOAD:-}"; for l in "${mp_libs[@]}"; do case $l in ""|*/libvkb_mali_profile.so) ;; *) mp_pre=${mp_pre:+$mp_pre:}$l ;; esac; done
unset VKB_PROFILE_FAKE_MISSING VKB_PROFILE_HIDE_EXTS VKB_PROFILE_FAKE_API VKB_PROFILE_NOT_IMITATED
if [ -n "$mp_pre" ]; then export LD_PRELOAD=$mp_pre; else unset LD_PRELOAD; fi
start s6 VKBRIDGE_BUDGET_LOW_HOLD_MS=0 VKBRIDGE_FAKE_MEMAVAIL_MB=1000000
run MBC_STEPS=300
eval "$mp_saved"
echo "$v" | grep -q '^ext=yes' || r="$r no-driver-ext"
grep -q 'MB free (under' "$log" && r="$r bridge-path"
step $([ -z "$r" ] && echo 0 || echo 1) "driver's own extension: the bridge's path not taken (driver budget $(field 300 1 budget_mb), usage $(field 300 1 usage_mb))${r:+ (failed:$r)}"

# players: a server per player, its RAM and the memory free when the game started (the process's first query)
player() { local n=$1; shift; start $n VKBRIDGE_FAKE_MISSING=membudget VKBRIDGE_BUDGET_LOW_HOLD_MS=0 "$@"; }
# 7 a player log
r=
player e78j VKBRIDGE_FAKE_MEMTOTAL_MB=11350 VKBRIDGE_FAKE_MEMAVAIL_MB=2850
run MBC_ALLOC_MB=2304 MBC_STEPS=2850,327
echo "$v" | grep -q '^ext=yes' || r="$r not-offered"
[ "$(field 2850 1 size_mb)" = 2560 ] && [ "$(field 2850 1 budget_mb)" = 2560 ] && [ "$(field 2850 1 usage_mb)" = 0 ] || r="$r 2850($(field 2850 1 budget_mb)/$(field 2850 1 usage_mb) of $(field 2850 1 size_mb))"
near "$(field 327 1 budget_mb)" 1095 8 && near "$(field 327 1 usage_mb)" 2304 8 || r="$r 327($(field 327 1 budget_mb)/$(field 327 1 usage_mb))"
grep -q 'heap cap: process .* at most 2560 MB, not 4096: 2850 MB free' "$log" || r="$r cap-log"
step $([ -z "$r" ] && echo 0 || echo 1) "11350 MB, 2850 free at start: heap $(field 2850 1 size_mb), 2850 free -> $(field 2850 1 budget_mb)/$(field 2850 1 usage_mb), 2304 held + 327 free -> $(field 327 1 budget_mb)/$(field 327 1 usage_mb) (want 1095/2304)${r:+ (failed:$r)}"

# 8 a player log
r=
player quwd VKBRIDGE_FAKE_MEMTOTAL_MB=11350 VKBRIDGE_FAKE_MEMAVAIL_MB=3507
run MBC_ALLOC_MB=2880 MBC_STEPS=467
echo "$v" | grep -q '^ext=yes' || r="$r not-offered"
[ "$(field 467 1 size_mb)" = 3072 ] && near "$(field 467 1 budget_mb)" 1811 8 && near "$(field 467 1 usage_mb)" 2880 8 || r="$r 467($(field 467 1 budget_mb)/$(field 467 1 usage_mb) of $(field 467 1 size_mb))"
step $([ -z "$r" ] && echo 0 || echo 1) "3507 free at start: heap $(field 467 1 size_mb), 2880 held + 467 free -> $(field 467 1 budget_mb)/$(field 467 1 usage_mb) (want 1811/2880)${r:+ (failed:$r)}"

# 9 swap used up
r=
player ems5 VKBRIDGE_FAKE_MEMTOTAL_MB=11245 VKBRIDGE_FAKE_MEMAVAIL_MB=3270 VKBRIDGE_FAKE_SWAPTOTAL_MB=12287 VKBRIDGE_FAKE_SWAPFREE_MB=0
run MBC_ALLOC_MB=2368 MBC_STEPS=1431,834,1500,2000
echo "$v" | grep -q '^ext=yes' || r="$r not-offered"
hs=$(field 1431 1 size_mb)
[ "$hs" = 3072 ] && [ "$(field 1431 1 budget_mb)" = "$hs" ] && [ "$(field 1431 1 usage_mb)" = 0 ] || r="$r 1431($(field 1431 1 budget_mb) of $hs)"
near "$(field 834 1 budget_mb)" 1666 8 && near "$(field 834 1 usage_mb)" 2368 8 || r="$r 834($(field 834 1 budget_mb)/$(field 834 1 usage_mb))"
near "$(field 1500 1 budget_mb)" 2332 8 || r="$r 1500($(field 1500 1 budget_mb))"
[ "$(field 2000 1 budget_mb)" = "$hs" ] && [ "$(field 2000 1 usage_mb)" = 0 ] || r="$r 2000($(field 2000 1 budget_mb))"
grep -q 'memory budget: 834 MB free (under 768 MB, or swap used up)' "$log" || r="$r enter-log"
step $([ -z "$r" ] && echo 0 || echo 1) "swap 0 of 12287: 1431 free -> $(field 1431 1 budget_mb) (heap $hs, not low), 834 -> $(field 834 1 budget_mb)/$(field 834 1 usage_mb) (want 1666/2368), 1500 -> $(field 1500 1 budget_mb) (still low), 2000 -> $(field 2000 1 budget_mb)${r:+ (failed:$r)}"

# 10 : MemAvailable 0
r=
player p4 VKBRIDGE_FAKE_MEMTOTAL_MB=7322 VKBRIDGE_FAKE_MEMAVAIL_MB=3174 VKBRIDGE_FAKE_SWAPTOTAL_MB=3661 VKBRIDGE_FAKE_SWAPFREE_MB=10
run MBC_ALLOC_MB=1280 MBC_STEPS=0
echo "$v" | grep -q '^ext=yes' || r="$r not-offered"
[ "$(field 0 1 budget_mb)" = 1024 ] && near "$(field 0 1 usage_mb)" 1280 8 || r="$r 0($(field 0 1 budget_mb)/$(field 0 1 usage_mb) of $(field 0 1 size_mb))"
step $([ -z "$r" ] && echo 0 || echo 1) "Pixel 9a (7322 MB, 0 free): heap $(field 0 1 size_mb), 1280 held -> $(field 0 1 budget_mb)/$(field 0 1 usage_mb) (want 1024/1280)${r:+ (failed:$r)}"

# 11 no change where the start was not short
r=
player s10 VKBRIDGE_FAKE_MEMTOTAL_MB=11525 VKBRIDGE_FAKE_MEMAVAIL_MB=4174
run MBC_STEPS=300; echo "$v" | grep -q '^ext=no' || r="$r s10+"
player u16 VKBRIDGE_FAKE_MEMTOTAL_MB=15245 VKBRIDGE_FAKE_MEMAVAIL_MB=2359
run MBC_STEPS=300; echo "$v" | grep -q '^ext=no' || r="$r ultra16"
player expl VKBRIDGE_FAKE_MEMTOTAL_MB=11350 VKBRIDGE_FAKE_MEMAVAIL_MB=2850 VKBRIDGE_HEAP_MAX_MB=4096
run MBC_STEPS=300; echo "$v" | grep -q '^ext=no' || r="$r explicit-cap"
step $([ -z "$r" ] && echo 0 || echo 1) "not offered without a short start: S10+ 4174 free, Ultra 16 GB (no cap), explicit VKBRIDGE_HEAP_MAX_MB${r:+ (failed:$r)}"
stop

if [ $fails = 0 ]; then echo "OK: $steps steps"; else echo "FAILED: $fails of $steps steps"; exit 1; fi
