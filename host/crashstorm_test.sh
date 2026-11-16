#!/bin/bash
# a player log (Kirin 9020, Maleoon 920): 65 short-lived steamwebhelper processes each faulted once in the driver;
# 64 were contained, the 64th line said "containment now off", the 65th killed the bridge server and the session.
# A crashed client that is gone must not use up the server's crash budget for everyone else.
# 70 clients in a row whose vkAllocateMemory faults (VKBRIDGE_TEST_CRASH_CMD, contained by server_guard.c), each
# ends; then one plain client. Steps:
#   1  the server is still alive after 70 contained faults from 70 dead clients
#   2  every one of the 70 was contained (70 CONTAINED lines) and no "containment now off" line
#   3  a crashed client's faults leave the count when its device goes (the release line)
# Prints one line per step and "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
N=${N:-70}
work=$(mktemp -d)
trap '[ -n "$srv" ] && kill $srv 2>/dev/null; [ -n "$KEEP" ] && echo "kept $work" || rm -rf "$work"' EXIT
fails=0 steps=0
step() { steps=$((steps+1)); if [ "$1" = 0 ]; then echo "  ok   $2"; else echo "  FAIL $2"; fails=$((fails+1)); fi; }
sock=$work/s.sock
env VKBRIDGE_PROBE=0 VKBRIDGE_TEST_CRASH_CMD=vkAllocateMemory VK_DRIVER_FILES=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json} VK_ICD_FILENAMES= \
    "$out/vkbridge-server" --socket "$sock" --log "$work/s.log" --cache-dir "$work/c" >/dev/null 2>"$work/s.err" &
srv=$!
for _ in $(seq 50); do [ -S "$sock" ] && break; sleep 0.1; done
for i in $(seq "$N"); do
  kill -0 $srv 2>/dev/null || break
  timeout 30 env VKBRIDGE_SOCKET=$sock VK_DRIVER_FILES=$out/vkbridge_icd.json VK_ICD_FILENAMES= "$out/vkbridge-smoke" >"$work/c$i.out" 2>&1
done
sleep 0.5
kill -0 $srv 2>/dev/null; alive=$?
step $alive "1 the server lives after $N clients that each faulted once (last client $i)"
n=$(grep -c "CONTAINED driver crash" "$work/s.log"); off=$(grep -c "containment now off" "$work/s.log")
[ "$n" -ge "$N" ] && [ "$off" = 0 ]; step $? "2 $n of $N contained, 'containment now off' lines: $off"
grep -q "no longer count" "$work/s.log"; step $? "3 release line: $(grep -m1 'no longer count' "$work/s.log" | sed 's/.*I //')"
kill $srv 2>/dev/null; srv=
if [ $fails = 0 ]; then echo "OK: crashstorm $steps steps"; else echo "FAILED: crashstorm $fails of $steps"; exit 1; fi
