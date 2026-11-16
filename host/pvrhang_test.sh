#!/bin/bash
# Where two test46 fixes meet: PowerVR's leftover destroy (server_children.c, a player log) with the GPU hang detector on
# (server_hang.c, which the fault-arm from a player log turns on for a game's next launch). The detector makes a fence per
# submission and hooks the driver table above the children wrappers; on both destroy paths it is torn down first, so the
# PowerVR sweep must neither destroy its fences twice nor leave the next device blocked. Runs pvrdestroy_test.sh's 7 steps
# with VKBRIDGE_HANG_DETECT=1 and checks every server log of the run had the detector on and no server crashed.
cd "$(dirname "$0")"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
KEEP=1 VKBRIDGE_HANG_DETECT=1 timeout 280 ./pvrdestroy_test.sh >"$work/run.txt" 2>&1; rc=$?
sed 's/^/  /' "$work/run.txt" | grep -v '^  kept '
kept=$(sed -n 's/^kept //p' "$work/run.txt" | tail -1)
fails=0
[ $rc = 0 ] || { echo "  FAIL pvrdestroy_test with the hang detector on: rc $rc"; fails=$((fails+1)); }
if [ -n "$kept" ] && [ -d "$kept" ]; then
  n=0 on=0 crash=0
  for l in "$kept"/*/server.log; do
    n=$((n+1))
    grep -q "GPU hang detector on" "$l" && on=$((on+1))
    grep -qiE "SIGSEGV|SIGABRT|double free|crash guard: .*fault|use after free" "$l" && { crash=$((crash+1)); echo "  crash-like line in $l: $(grep -iE 'SIGSEGV|SIGABRT|double free|use after free' "$l" | head -1)"; }
  done
  # step 0 (fix off) blocks inside vkDestroyDevice before any later line; every other step's server must have had the detector on
  [ $on -ge $((n-1)) ] && [ $n -ge 7 ] || { echo "  FAIL detector on in $on of $n server logs"; fails=$((fails+1)); }
  [ $crash = 0 ] || fails=$((fails+1))
  echo "  servers $n, detector on in $on, crash-like lines in $crash"
  rm -rf "$kept"
else
  echo "  FAIL no kept work dir"; fails=$((fails+1))
fi
if [ $fails = 0 ]; then echo "OK: PowerVR leftover destroy with the hang detector on (7 steps)"; else echo "FAILED: $fails"; exit 1; fi
