#!/bin/bash
# The server's own switches from <cache dir>/env (vkb_srv_env_global) as well as from its environment.
#   1  no switch (push descriptors taken as missing, VKBRIDGE_FAKE_MISSING=push): the desktop driver's
#      usual memory sharing (dmabuf), an emulation on, crash containment on;
#   2  <cache dir>/env VKBRIDGE_MEMORY=hostptr: only hostptr tried (it fails the self-test on the desktop
#      driver, so the summary has no dmabuf and the device none);
#   3  the server's environment VKBRIDGE_MEMORY=hostptr: the same (the old way still works);
#   4  <cache dir>/env VKBRIDGE_EMULATE=0 (push missing as in 1): emulating 0x0;
#   5  <cache dir>/env VKBRIDGE_NO_CRASH_GUARD=1: the "crash containment off" line once a pipeline is made.
# Prints one line per step and "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
work=$(mktemp -d)
cache=$work/c
sock=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-envswitch-$$.sock
log=$work/server.log
srv=
start() { # start <env file contents> [VAR=value ...]
  [ -n "$srv" ] && kill $srv 2>/dev/null && wait $srv 2>/dev/null
  rm -rf "$cache" "$sock"
  mkdir -p "$cache"
  [ -n "$1" ] && printf '%s\n' "$1" > "$cache/env"
  shift
  : > "$log"
  env -u VKBRIDGE_MEMORY -u VKBRIDGE_EMULATE -u VKBRIDGE_NO_CRASH_GUARD -u VKBRIDGE_FAKE_MISSING -u VKBRIDGE_MODE "$@" \
      VK_DRIVER_FILES=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json} VK_ICD_FILENAMES= \
      "$out/vkbridge-server" --socket "$sock" --log "$log" --cache-dir "$cache" >/dev/null 2>&1 &
  srv=$!
  for _ in $(seq 50); do [ -S "$sock" ] && break; sleep 0.1; done
  # one client: the self-test has run once a device is listed
  VKBRIDGE_SOCKET=$sock VK_DRIVER_FILES=$out/vkbridge_icd.json VK_ICD_FILENAMES= timeout 60 vulkaninfo --summary >/dev/null 2>&1
}
stop() { [ -n "$srv" ] && kill $srv 2>/dev/null && wait $srv 2>/dev/null; srv=; }
trap 'kill $srv 2>/dev/null; rm -f "$sock"; [ -n "$KEEP" ] || rm -rf "$work"' EXIT
fails=0 steps=0
step() { steps=$((steps+1)); if [ "$1" = 0 ]; then echo "  ok   $2"; else echo "  FAIL $2"; fails=$((fails+1)); fi; }
sharing() { grep -o 'memory sharing = [a-z]*' "$log" | head -1 | sed 's/.*= //'; }
only_hostptr() { grep -q 'memory sharing summary for .*hostptr: ' "$log" && ! grep -q 'memory sharing summary for .*dmabuf: ' "$log" && [ "$s" != "$base" ]; }
emu() { grep -o 'memory sharing = [a-z]*, missing 0x[0-9a-f]*, emulating 0x[0-9a-f]*' "$log" | head -1 | sed 's/.*emulating //'; }

start "" VKBRIDGE_FAKE_MISSING=push
VKBRIDGE_SOCKET=$sock VK_DRIVER_FILES=$out/vkbridge_icd.json VK_ICD_FILENAMES= timeout 60 "$out/vkbridge-smoke" >/dev/null 2>&1
base=$(sharing) base_emu=$(emu)
stop
[ -n "$base" ] && [ "$base" != hostptr ] && [ -n "$base_emu" ] && [ "$base_emu" != 0x0 ] && ! grep -q "crash containment off" "$log"
step $? "no switch: memory sharing $base, emulating $base_emu, containment on"

start "VKBRIDGE_MEMORY=hostptr"
s=$(sharing)
stop
only_hostptr && grep -q "override from .*env: VKBRIDGE_MEMORY=hostptr" "$log"
step $? "env file VKBRIDGE_MEMORY=hostptr: $s"

start "" VKBRIDGE_MEMORY=hostptr
s=$(sharing)
stop
only_hostptr
step $? "server environment VKBRIDGE_MEMORY=hostptr: $s"

start "VKBRIDGE_EMULATE=0" VKBRIDGE_FAKE_MISSING=push
e=$(emu)
stop
[ "$e" = 0x0 ]
step $? "env file VKBRIDGE_EMULATE=0: emulating $e"

start "VKBRIDGE_NO_CRASH_GUARD=1"
VKBRIDGE_SOCKET=$sock VK_DRIVER_FILES=$out/vkbridge_icd.json VK_ICD_FILENAMES= timeout 60 "$out/vkbridge-smoke" >/dev/null 2>&1
stop
grep -q "driver crash containment off (VKBRIDGE_NO_CRASH_GUARD)" "$log"
step $? "env file VKBRIDGE_NO_CRASH_GUARD=1: containment off"

if [ $fails = 0 ]; then echo "OK: $steps of $steps"; else echo "FAILED: $fails of $steps"; KEEP=1; echo "logs: $work"; fi
[ $fails = 0 ]
