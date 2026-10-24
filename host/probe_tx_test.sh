#!/bin/bash
# The startup probe's BC transcoder builds (server_probe.c p_bc_transcoders), on the desktop driver
# with BC taken as missing (so BC is transcoded, as on Mali) and a server of its own; a slow build
# imitated with VKBRIDGE_PROBE_TX_FAKE (the Mali-G1-Ultra took over 4 s over BC7->ASTC, which was
# taken as a hang, and BC2 and BC3 were decoded with it):
#   1  a build slower than the probes' limit but under the transcoders' own: passes, said slow,
#      every BC format transcoded;
#   2  a build past the transcoders' limit: a hang, said as a compile; only BC7 decoded (BC2 and
#      BC3 still go to ASTC), and the device's log says which formats are decoded;
#   3  a hang on an earlier transcoder: those after it are not tried, and decoded too.
# Prints one line per step and "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
work=$(mktemp -d)
sock=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-probetx-$$.sock
srv=
start() { # start <name> [env...]
  local name=$1
  shift
  stop
  rm -f "$sock"
  mkdir -p "$work/$name"
  env VKBRIDGE_PROBE_TIMEOUT_S=1 VKBRIDGE_PROBE_BUDGET_S=0 VKBRIDGE_FAKE_MISSING=bc VKBRIDGE_BC_TRANSCODE=1 \
    vk_require_etc2=true vk_require_astc=true "$@" VK_DRIVER_FILES=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json} VK_ICD_FILENAMES= \
    "$out/vkbridge-server" --socket "$sock" --log "$work/$name/server.log" --cache-dir "$work/$name/c" >/dev/null 2>&1 &
  srv=$!
  for _ in $(seq 300); do [ -S "$sock" ] && break; kill -0 $srv 2>/dev/null || break; sleep 0.1; done
  timeout 60 "$out/vkbridge-smoke" >"$work/$name/smoke.log" 2>&1
}
stop() { [ -n "$srv" ] && kill $srv 2>/dev/null && wait $srv 2>/dev/null; srv=; }
trap 'stop; rm -f "$sock"; [ -n "$KEEP" ] && echo "kept $work" || rm -rf "$work"' EXIT
export VKBRIDGE_SOCKET=$sock VK_DRIVER_FILES=$out/vkbridge_icd.json VK_ICD_FILENAMES=
unset VKBRIDGE_HIDE_EXTS VKBRIDGE_FAKE_MISSING VKBRIDGE_PROBE_TX_FAKE VKBRIDGE_PROBE_TX_TIMEOUT_S
fails=0
step() { if [ "$1" = 0 ]; then echo "  ok   $2"; else echo "  FAIL $2"; fails=$((fails+1)); fi; }
probe_line() { grep -m1 -o '"name": "bc_transcoders", "status": "[a-z]*"' $work/$1/c/probe-*.json 2>/dev/null | grep -o '[a-z]*"$' | tr -d '"' | head -1; }
detail() { grep -m1 -o '"name": "bc_transcoders".*' $work/$1/c/probe-*.json 2>/dev/null | grep -o '"detail": "[^"]*"' | cut -d'"' -f4 | head -1; }
tx() { grep -m1 -o "BC emulation: transcoding (BC format -> format):[^,]*" $work/$1/server.log | sed 's/.*format)://'; }
has() { tx $1 | grep -qw -- "$2->[0-9]*"; } # has <name> <BC format>: transcoded

# 1 BC7->ASTC 2.5 s longer: over the probes' 1 s, under the transcoders' 20 s
start slow VKBRIDGE_PROBE_TX_FAKE=BC7-\>ASTC:2500
[ "$(probe_line slow)" = pass ] && detail slow | grep -q "BC7->ASTC [0-9]* ms (slow)" && has slow 145 && has slow 137 && has slow 135 &&
  ! grep -q "decoded, not transcoded" $work/slow/server.log
step $? "a slow build passes: $(detail slow | grep -o 'BC7->ASTC[^,]*'); transcoded:$(tx slow)"

# 2 the same past a 2 s limit for the transcoders
start hang VKBRIDGE_PROBE_TX_FAKE=BC7-\>ASTC:5000 VKBRIDGE_PROBE_TX_TIMEOUT_S=2
[ "$(probe_line hang)" = hang ] && detail hang | grep -q "BC7->ASTC HANG (still compiling after 2 s, interrupted; a compile, no GPU work)" &&
  ! has hang 145 && ! has hang 146 && has hang 135 && has hang 137 && has hang 131 &&
  grep -q "BC emulation: formats 145 146 decoded, not transcoded" $work/hang/server.log
step $? "a build past the limit: $(detail hang | grep -o 'BC7->ASTC HANG[^)]*)'); transcoded:$(tx hang)"

# 3 a hang on BC2->ASTC: the rest not tried, and decoded
start early VKBRIDGE_PROBE_TX_FAKE=BC2-\>ASTC:5000 VKBRIDGE_PROBE_TX_TIMEOUT_S=2
[ "$(probe_line early)" = hang ] && detail early | grep -q "BC7->ASTC not tried" && has early 131 && ! has early 135 && ! has early 139 &&
  ! has early 143 && ! has early 145
step $? "an early hang: the rest not tried ($(detail early | grep -o 'BC3->ASTC not tried.*not tried')); transcoded:$(tx early)"

if [ $fails = 0 ]; then echo "OK: BC transcoder probe"; else echo "FAILED: $fails step(s)"; exit 1; fi
