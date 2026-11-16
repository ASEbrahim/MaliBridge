#!/bin/bash
# The "adapter:" line (client/physdev.c adapter_line): once per
# physical device, the GPU and heaps the app is shown after every spoof and cap, next to the real GPU, so a preset a
# game picked from the adapter can be explained from the log. gpuview_check prints what the app itself reads.
#   1  nothing spoofed: the line's shown name/vendor/device and heaps are gpuview_check's, spoof=off, heap_cap=0M,
#      real = shown;
#   2  the guest's VKBRIDGE_SPOOF_GPU=amd + VKBRIDGE_HEAP_MAX_MB=1024: shown = AMD's ids (as gpuview_check reads),
#      real = the host GPU, every local heap 1024M, spoof=amd(guest), heap_cap_from=guest;
#   3  the same from the server (<cache dir>/env: nvidia, 2048): spoof=nvidia(server), heap_cap_from=server;
#   4  one line per physical device per process (gpuview_check asks for the properties many times; the loader asks
#      every device the server shows, so a host with two GPUs gets two lines), none repeated.
# Prints one line per step and "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
work=$(mktemp -d)
sock=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-adapter-$$.sock
host_icd=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json}
cache=$work/c
srv=
stop() { [ -n "$srv" ] && kill $srv 2>/dev/null && wait $srv 2>/dev/null; srv=; }
start() {
  stop
  rm -f "$sock"
  env -u VKBRIDGE_FAKE_MISSING -u VKBRIDGE_HIDE_EXTS -u VKBRIDGE_MODE -u VKBRIDGE_HEAP_MAX_MB -u VKBRIDGE_SPOOF_GPU \
      VKBRIDGE_PROBE_BUDGET_S=0 VK_DRIVER_FILES=$host_icd VK_ICD_FILENAMES= \
      "$out/vkbridge-server" --socket "$sock" --log "$work/server.log" --cache-dir "$cache" >/dev/null 2>&1 &
  srv=$!
  for _ in $(seq 300); do [ -S "$sock" ] && break; kill -0 $srv 2>/dev/null || break; sleep 0.05; done
}
trap 'stop; rm -f "$sock"; [ -n "$KEEP" ] && echo "kept $work" || rm -rf "$work"' EXIT
export VKBRIDGE_SOCKET=$sock VK_DRIVER_FILES=$out/vkbridge_icd.json VK_ICD_FILENAMES=
unset VKBRIDGE_HIDE_EXTS VKBRIDGE_MAX_API VKBRIDGE_FAKE_MISSING VKBRIDGE_MODE VKBRIDGE_HEAP_MAX_MB VKBRIDGE_SPOOF_GPU
fails=0 steps=0
step() { steps=$((steps+1)); if [ "$1" = 0 ]; then echo "  ok   $2"; else echo "  FAIL $2"; fails=$((fails+1)); fi; }
# view [env...]: gpuview_check through the bridge at the default log level; $v its output with the client's log (stderr)
view() { v=$(timeout 60 env -u VKBRIDGE_LOG_LEVEL -u VKBRIDGE_DEBUG "$@" "$out/gpuview_check" 2>&1); rc=$?; a=$(echo "$v" | grep -o 'adapter: .*' | head -1); }
kv() { echo "$a" | grep -o " $1=[^ ]*" | head -1 | cut -d= -f2-; }
# the shown part's heaps as "size,size,..." and gpuview's
line_heaps() { kv heaps | tr ',' '\n' | cut -d: -f2 | tr -d M | paste -sd,; }
view_heaps() { echo "$v" | grep '^heap ' | grep -o 'size_mb=[0-9]*' | cut -d= -f2 | paste -sd,; }
nat=$(VK_DRIVER_FILES=$host_icd timeout 60 "$out/gpuview_check" 2>&1) || { echo "FAILED: native gpuview_check: $nat"; exit 1; }
real_name=$(echo "$nat" | grep -m1 '^gpu ' | sed 's/.* name=//')
mkdir -p "$cache"

# 1
start
view
g=$(echo "$v" | grep -m1 '^gpu ')
[ $rc = 0 ] && [ -n "$a" ] && [ "$(kv vendor)" = "$(echo "$g" | grep -o 'vendor=[^ ]*' | cut -d= -f2)" ] &&
  [ "$(kv device)" = "$(echo "$g" | grep -o 'device=[^ ]*' | cut -d= -f2)" ] && echo "$a" | grep -qF "shown name=\"${g#*name=}\"" &&
  [ "$(line_heaps)" = "$(view_heaps)" ] && [ "$(kv spoof)" = off ] && [ "$(kv heap_cap)" = 0M ] && echo "$a" | grep -qF "real name=\"$real_name\""
step $? "1 nothing spoofed: ${a:-no adapter line}"

# 2
view VKBRIDGE_SPOOF_GPU=amd VKBRIDGE_HEAP_MAX_MB=1024
g=$(echo "$v" | grep -m1 '^gpu ')
locals=$(kv heaps | tr ',' '\n' | grep ':local$' | grep -vc ':1024M:local$')
[ $rc = 0 ] && [ "$(kv vendor)" = 0x1002 ] && echo "$g" | grep -q 'vendor=0x1002' && [ "$(line_heaps)" = "$(view_heaps)" ] && [ "$locals" = 0 ] &&
  [ "$(kv spoof)" = "amd(guest)" ] && [ "$(kv heap_cap)" = 1024M ] && [ "$(kv heap_cap_from)" = guest ] && echo "$a" | grep -qF "real name=\"$real_name\""
step $? "2 guest spoof amd + heap 1024: ${a:-no adapter line}"

# 3
printf 'VKBRIDGE_HEAP_MAX_MB=2048\nVKBRIDGE_SPOOF_GPU=nvidia\n' > "$cache/env"
view
g=$(echo "$v" | grep -m1 '^gpu ')
[ $rc = 0 ] && [ "$(kv vendor)" = 0x10de ] && echo "$g" | grep -q 'vendor=0x10de' && [ "$(line_heaps)" = "$(view_heaps)" ] &&
  [ "$(kv spoof)" = "nvidia(server)" ] && [ "$(kv heap_cap)" = 2048M ] && [ "$(kv heap_cap_from)" = server ]
step $? "3 server spoof nvidia + heap 2048: ${a:-no adapter line}"
rm -f "$cache/env"

# 4 (the loader asks every physical device for its properties: a line for each GPU the server shows, none twice)
n=$(echo "$v" | grep -c 'adapter: ')
dup=$(echo "$v" | grep -o 'adapter: .*' | sort | uniq -d | wc -l)
real=$(echo "$v" | grep -o 'real name="[^"]*"' | sort -u | wc -l)
[ "$n" -ge 1 ] && [ "$dup" = 0 ] && [ "$real" = "$n" ]
step $? "4 one line per physical device: $n line(s) for $real GPU(s), $dup repeated"

stop
if [ $fails = 0 ]; then echo "OK: $steps steps"; else echo "FAILED: $fails of $steps steps"; exit 1; fi
