#!/bin/bash
# vkd3d-proton's descriptor heap on a driver whose descriptor pools cost host memory per descriptor
# (Pixel 10 Pro XL, PowerVR DXT 25.1, 2026-11-10: RE3's six 1,000,000-descriptor sets took the
# server's heap 313 MB -> 6,472 MB in 10 s; Android's low-memory killer ended the app and gamescope
# aborted "IWaitable hung up"). The server imitates that cost (VKBRIDGE_FAKE_DESCHEAP=64: 64 bytes a
# descriptor, resident) and PowerVR's identity (VKBRIDGE_FAKE_DRIVER=img); tests/desc_limits_check
# makes vkd3d's sets (1,000,000 each, update after bind, variable count, shader and host sets, writes
# and copies up to 999,999, reads back indices 2, 5, 7) under the engine name "vkd3d".
# One server per step; the server's peak resident memory (VmHWM) is read before it is stopped:
#   1  PowerVR + the imitation, VKBRIDGE_DESC_LIMIT=0 (no cut): the peak is high (the imitation is real), big pools logged
#   2  the same with a 48 MB budget: the server measures the cost (64 bytes), cuts vkd3d's sets and pools, its
#      peak is under a third of step 1's, the check passes (low indices read back), the writes past the cut are said
#   3  no PowerVR identity (Mali's way), imitation on: no probe, nothing cut, the check passes
#   4  PowerVR without the imitation (a cheap driver): measured, "made as asked", nothing cut, the check passes
#   5  the cut learns what the program writes: step 2's cache dir holds its reach (desc-reach-<program>.txt, element 999,999 past
#      the cut); the next start with the same budget raises the cut from step 2's (up to twice the budget), says so, drops fewer
#   6  a program whose last run stayed under the cut: nothing raised (the reach file says so), the cut as in step 2
# Prints one line per step and "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
work=$(mktemp -d)
sock=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-pvrdesc-$$.sock
srv=
stop() { [ -n "$srv" ] && kill -9 $srv 2>/dev/null && wait $srv 2>/dev/null; srv=; }
trap 'stop; rm -f "$sock"; [ -n "$KEEP" ] && echo "kept $work" || rm -rf "$work"' EXIT
trap 'exit 143' TERM INT HUP
start() { # start <name> [env...]
  local d=$work/$1
  shift
  stop
  rm -f "$sock"
  mkdir -p "$d/c"
  log=$d/server.log
  env -u VKBRIDGE_FAKE_MISSING -u VKBRIDGE_FAKE_DRIVER -u VKBRIDGE_FAKE_DESCHEAP -u VKBRIDGE_DESC_LIMIT -u VKBRIDGE_DESC_BUDGET_MB \
      -u VKBRIDGE_HIDE_EXTS -u VKBRIDGE_MODE -u VKBRIDGE_DESC_CUT "$@" \
      VK_DRIVER_FILES=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/lvp_icd.json} VK_ICD_FILENAMES= \
      "$out/vkbridge-server" --socket "$sock" --log "$log" --cache-dir "${CACHE:-$d/c}" >/dev/null 2>&1 &
  srv=$!
  for _ in $(seq 100); do [ -S "$sock" ] && break; kill -0 $srv 2>/dev/null || break; sleep 0.1; done
  cl=$d/client.out
}
hwm() { awk '/^VmHWM/ {print int($2/1024)}' /proc/$srv/status 2>/dev/null; }
check() { DESC_CHECK_ENGINE=vkd3d timeout 300 "$out/desc_limits_check" >$cl 2>&1; }
read_ok() { grep -q "^PASS dispatch reads index 2" $cl && grep -q "^PASS dispatch reads index 5" $cl && grep -q "^PASS dispatch reads index 7" $cl && grep -q "^PASS vkd3d.s CBV set and raw SSBO set" $cl; }
export VKBRIDGE_SOCKET=$sock VK_DRIVER_FILES=$out/vkbridge_icd.json VK_ICD_FILENAMES=
unset VKBRIDGE_HIDE_EXTS VKBRIDGE_MAX_API VKBRIDGE_SYNC
fails=0 steps=0
step() { steps=$((steps+1)); if [ "$1" = 0 ]; then echo "  ok   $2"; else echo "  FAIL $2"; fails=$((fails+1)); fi; }

# 1
start nocut VKBRIDGE_FAKE_DRIVER=img VKBRIDGE_FAKE_DESCHEAP=64 VKBRIDGE_DESC_LIMIT=0
check; rc=$?; peak2=$(hwm)
big=$(grep -c "descriptor pool of [0-9]* descriptors (type" "$log")
[ "${peak2:-0}" -ge 300 ] && [ "$big" -ge 1 ] && ! grep -q "a descriptor costs the server" "$log"
step $? "PowerVR + 64 B a descriptor, VKBRIDGE_DESC_LIMIT=0: check rc $rc, server peak ${peak2} MB (the imitation's cost), big pools logged $big, no probe"

# 2
start cut VKBRIDGE_FAKE_DRIVER=img VKBRIDGE_FAKE_DESCHEAP=64 VKBRIDGE_DESC_BUDGET_MB=48
check; rc=$?; peak=$(hwm)
probe=$(grep -m1 -o 'a descriptor costs the server .*' "$log")
small=$(grep -c "made smaller (VKBRIDGE_DESC_LIMIT)\|made smaller" "$log")
past=$(grep -c "past the descriptors the GPU allows; dropped" "$log")
read_ok && [ -n "$peak" ] && [ -n "$peak2" ] && [ $((peak * 3)) -lt "$peak2" ] && echo "$probe" | grep -q "cut to" && [ "$small" -ge 1 ] && [ "$past" -ge 1 ]
step $? "PowerVR + 64 B a descriptor, budget 48 MB: check rc $rc ($(tail -1 $cl)), server peak ${peak} MB, '$probe', pools made smaller $small, writes past the cut said $past"

# 3
start mali VKBRIDGE_FAKE_DESCHEAP=64
check; rc=$?; peak3=$(hwm)
read_ok && ! grep -q "a descriptor costs the server\|made smaller\|big pools and sets logged" "$log"
step $? "no PowerVR identity: check rc $rc ($(tail -1 $cl)), peak ${peak3} MB, no probe, nothing cut, nothing watched"

# 4
start cheap VKBRIDGE_FAKE_DRIVER=img
check; rc=$?
probe4=$(grep -m1 -o 'a descriptor costs the server .*' "$log")
read_ok && echo "$probe4" | grep -q "made as asked" && ! grep -q "made smaller" "$log"
step $? "PowerVR, cheap descriptors: check rc $rc ($(tail -1 $cl)), '$probe4', nothing made smaller"

# 5 (the cut of step 2, then the same budget again on step 2's cache dir)
cut2=$(grep -m1 -o 'cut to [0-9]*' "$work/cut/server.log" | grep -o '[0-9]*$')
rf=$(ls "$work"/cut/c/desc-reach-*.txt 2>/dev/null | head -1)
rline=$(cat "$rf" 2>/dev/null)
CACHE=$work/cut/c start learn VKBRIDGE_FAKE_DRIVER=img VKBRIDGE_FAKE_DESCHEAP=64 VKBRIDGE_DESC_BUDGET_MB=48
check; rc=$?
probe5=$(grep -m1 -o 'a descriptor costs the server .*' "$log")
cut5=$(echo "$probe5" | grep -o 'cut to [0-9]*' | grep -o '[0-9]*$')
past2=$(grep -c "past the descriptors the GPU allows; dropped" "$work/cut/server.log"); past5=$(grep -c "past the descriptors the GPU allows; dropped" "$log")
read_ok && [ -n "$rf" ] && echo "$rline" | grep -q "^reach 1000000 cap $cut2" && [ -n "$cut5" ] && [ "$cut5" -gt "$cut2" ] &&
  [ "$cut5" -le $((cut2 * 2 + 8192)) ] && echo "$probe5" | grep -q "raised from $cut2: the last run reached element 999999 (cut $cut2 then)"
step $? "the cut learns the reach: step 2 kept '$rline' ($(basename "$rf")), the next start cuts to $cut5 (step 2: $cut2): '$probe5'; check rc $rc; drop lines $past2 -> $past5"

# 6 (a reach under the cut: written by hand as a run that stayed low would)
mkdir -p "$work/low/c"; echo "reach 5000 cap $cut2" > "$work/low/c/$(basename "$rf")"
CACHE=$work/low/c start low VKBRIDGE_FAKE_DRIVER=img VKBRIDGE_FAKE_DESCHEAP=64 VKBRIDGE_DESC_BUDGET_MB=48
check; rc=$?
probe6=$(grep -m1 -o 'a descriptor costs the server .*' "$log")
read_ok && echo "$probe6" | grep -q "cut to $cut2 (six of them fit the budget)" && ! echo "$probe6" | grep -q raised
step $? "a last run under the cut: nothing raised: '$probe6'"
stop
if [ $fails = 0 ]; then echo "OK: $steps steps"; else echo "FAILED: $fails of $steps steps"; fi
[ $fails = 0 ]
