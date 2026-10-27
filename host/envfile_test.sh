#!/bin/bash
# <cache dir>/env reaches the very first process after it changed. A process asks the server for its
# client switches (vkbQueryServer: VKBRIDGE_HEAP_MAX_MB, VKBRIDGE_SPOOF_GPU, VKBRIDGE_QUERY_MIRROR, ...)
# before it makes a device, and the server used to read the file again only at vkCreateDevice: the
# first process after the file was written, changed or removed got the values from before. One
# server for the whole test (as on the phone, where it outlives the games); gpuview_check prints the
# GPU and heaps as the app sees them.
#   1  the file there at the server's start: the first process gets it;
#   2  written after the start (no file before): the first process after it gets it;
#   3  changed: the first process after it gets the new values;
#   4  a client switch (VKBRIDGE_QUERY_MIRROR=0) added: the first process after it says so;
#   5  removed (after a process that made a device): the first process after it gets the real GPU
#      and heaps again;
#   6  the file is not read again (nor its lines logged) while it stays the same.
# Prints one line per step and "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
work=$(mktemp -d)
sock=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-envfile-$$.sock
host_icd=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json}
cache=$work/c
log=$work/vkbridge-server.log
srv=
stop() { [ -n "$srv" ] && kill $srv 2>/dev/null && wait $srv 2>/dev/null; srv=; }
start() {
  stop
  rm -f "$sock"
  : > "$log"
  env -u VKBRIDGE_FAKE_MISSING -u VKBRIDGE_HIDE_EXTS -u VKBRIDGE_MODE -u VKBRIDGE_PRESET_FAKE_BUILTIN \
      -u VKBRIDGE_HEAP_MAX_MB -u VKBRIDGE_SPOOF_GPU -u VKBRIDGE_QUERY_MIRROR -u VKBRIDGE_EMULATE -u VKBRIDGE_FAKE_MEMTOTAL_MB \
      -u VKBRIDGE_HITCH_MS VKBRIDGE_PROBE_BUDGET_S=0 VK_DRIVER_FILES=$host_icd VK_ICD_FILENAMES= \
      "$out/vkbridge-server" --socket "$sock" --log "$log" --cache-dir "$cache" >/dev/null 2>&1 &
  srv=$!
  for _ in $(seq 300); do [ -S "$sock" ] && break; kill -0 $srv 2>/dev/null || break; sleep 0.05; done
}
view() { v=$(timeout 60 env VKBRIDGE_LOG_LEVEL=2 "$@" "$out/gpuview_check" 2>&1); rc=$?; }
gpu_of() { echo "$1" | grep -m1 '^gpu '; }
# every device-local heap $2 MB
heaps_are() { local n; n=$(echo "$1" | grep '^heap ' | grep 'local=1' | grep -vc "size_mb=$2 "); [ "$n" = 0 ] && echo "$1" | grep -q 'local=1'; }
trap 'stop; rm -f "$sock"; [ -n "$KEEP" ] && echo "kept $work" || rm -rf "$work"' EXIT
export VKBRIDGE_SOCKET=$sock VK_DRIVER_FILES=$out/vkbridge_icd.json VK_ICD_FILENAMES=
unset VKBRIDGE_HIDE_EXTS VKBRIDGE_MAX_API VKBRIDGE_FAKE_MISSING VKBRIDGE_MODE VKBRIDGE_HEAP_MAX_MB VKBRIDGE_SPOOF_GPU VKBRIDGE_QUERY_MIRROR
unset VKBRIDGE_HITCH_MS SteamAppId SteamGameId VKBRIDGE_PCACHE_KEY
fails=0 steps=0
step() { steps=$((steps+1)); if [ "$1" = 0 ]; then echo "  ok   $2"; else echo "  FAIL $2"; fails=$((fails+1)); fi; }

nat=$(VK_DRIVER_FILES=$host_icd timeout 60 "$out/gpuview_check" 2>&1) || { echo "FAILED: native gpuview_check: $nat"; exit 1; }
real_gpu=$(gpu_of "$nat")
mkdir -p "$cache"

# 1
printf 'VKBRIDGE_HEAP_MAX_MB=1024\nVKBRIDGE_SPOOF_GPU=nvidia\n' > "$cache/env"
start
view
[ $rc = 0 ] && heaps_are "$v" 1024 && gpu_of "$v" | grep -q 'vendor=0x10de'
step $? "1: the file there at the start, first process: $(gpu_of "$v" | cut -c1-60), $(echo "$v" | grep -m1 'local=1' | grep -o 'size_mb=[0-9]*')"

# 2
rm -f "$cache/env"
start
printf 'VKBRIDGE_HEAP_MAX_MB=1024\nVKBRIDGE_SPOOF_GPU=nvidia\n' > "$cache/env"
view
[ $rc = 0 ] && heaps_are "$v" 1024 && gpu_of "$v" | grep -q 'vendor=0x10de'
step $? "2: written after the start, first process after it: $(gpu_of "$v" | cut -c1-60), $(echo "$v" | grep -m1 'local=1' | grep -o 'size_mb=[0-9]*')"

# 3 (replaced, as an editor saves it)
printf 'VKBRIDGE_HEAP_MAX_MB=2048\nVKBRIDGE_SPOOF_GPU=amd\n' > "$cache/env.new" && mv "$cache/env.new" "$cache/env"
view
[ $rc = 0 ] && heaps_are "$v" 2048 && gpu_of "$v" | grep -q 'vendor=0x1002'
step $? "3: changed, first process after it: $(gpu_of "$v" | cut -c1-60), $(echo "$v" | grep -m1 'local=1' | grep -o 'size_mb=[0-9]*')"

# 4
echo 'VKBRIDGE_QUERY_MIRROR=0' >> "$cache/env"
view
[ $rc = 0 ] && echo "$v" | grep -q 'switches from the server.*query mirror off'
step $? "4: a client switch added, first process after it: $(echo "$v" | grep -m1 -o 'switches from the server.*' || echo 'no switches line')"

# 5 (after a process that made a device, which read the file as it was before the removal)
timeout 120 "$out/vkbridge-smoke" >/dev/null 2>&1
rm -f "$cache/env"
view
[ $rc = 0 ] && [ "$(gpu_of "$v")" = "$real_gpu" ] && ! echo "$v" | grep -q 'switches from the server'
step $? "5: removed, first process after it: $(gpu_of "$v" | cut -c1-60)"

# 6
printf 'VKBRIDGE_HEAP_MAX_MB=1024\n' > "$cache/env"
view
n1=$(grep -c 'override from .*VKBRIDGE_HEAP_MAX_MB=1024' "$log")
view
view
n2=$(grep -c 'override from .*VKBRIDGE_HEAP_MAX_MB=1024' "$log")
# gpuview_check makes no device: only vkbQueryServer could read it, and only when it changed
[ $rc = 0 ] && heaps_are "$v" 1024 && [ "$n1" -ge 1 ] && [ $((n2 - n1)) = 0 ]
step $? "6: unchanged: not read again by 2 more processes ($((n2 - n1)) reads; $n1 when it changed)"

stop
if [ $fails = 0 ]; then echo "OK: $steps steps"; else echo "FAILED: $fails of $steps steps"; fi
[ $fails = 0 ]
