#!/bin/bash
# Format properties asked while emulated formats are described (client/emu.c -> client/physdev.c cache): a server with BC and
# the BGR10/UF11 vertex formats imitated missing (VKBRIDGE_FAKE_MISSING=bc,bgr10vtx,uf11vtx), tests/fmtcache_check asking 12 formats
# x 4 query shapes x 250 rounds (12,000 queries; 9,000 of them for the emulated formats' v2 path that went to the server each time):
#   1  every answer of rounds 1.. equals round 0's (the check's own FAIL lines);
#   2  round trips (VKBRIDGE_STATS lists a command from 100 on): vkGetPhysicalDeviceFormatProperties absent (before the cache: ~6,000, the
#      emulated BC formats' view-format queries, 8 formats x 3 shapes x 250 rounds); ...Properties2 at most 1,100 (what may still
#      reach the server: the 4 non-BC formats with a chain the cache does not know, 4 x 250, and one per distinct key).
# One line per step, "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
icd=${VKB_CLIENT_ICD:-$out/vkbridge_icd.json}
work=$(mktemp -d "${XDG_RUNTIME_DIR:-/tmp}/vkb-fc.XXXX")
sock=$work/s.sock
srv=
trap '[ -n "$srv" ] && kill $srv 2>/dev/null && wait $srv 2>/dev/null; [ -n "${VKB_KEEP:-}" ] && echo "kept $work" || rm -rf "$work"' EXIT
mkdir -p "$work/c"
env -u VKBRIDGE_HIDE_EXTS -u VKBRIDGE_ERRATA -u VKBRIDGE_MODE VKBRIDGE_FAKE_MISSING=bc,bgr10vtx,uf11vtx VKBRIDGE_PROBE_BUDGET_S=0 \
    VK_DRIVER_FILES=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json} VK_ICD_FILENAMES= \
    "$out/vkbridge-server" --socket "$sock" --log "$work/server.log" --cache-dir "$work/c" >/dev/null 2>&1 &
srv=$!
for _ in $(seq 300); do [ -S "$sock" ] && break; kill -0 $srv 2>/dev/null || break; sleep 0.05; done
[ -S "$sock" ] || { echo "FAILED: server did not start"; exit 1; }
VKBRIDGE_SOCKET=$sock VK_DRIVER_FILES=$icd VK_ICD_FILENAMES= VKBRIDGE_STATS=3600 timeout 120 "$out/fmtcache_check" >"$work/check.log" 2>&1
rc=$?
cat "$work/check.log" | grep -E "^(PASS|FAIL)"
[ $rc = 0 ] && echo "OK: step 1, answers identical" || echo "FAILED: step 1 (exit $rc)"
fails=$((rc != 0))
n1=$(grep -E "\]: +vkGetPhysicalDeviceFormatProperties +[0-9]+ sync" "$work/check.log" | sed -E 's/.*Properties +([0-9]+) sync.*/\1/' | head -1)
n2=$(grep -E "\]: +vkGetPhysicalDeviceFormatProperties2 +[0-9]+ sync" "$work/check.log" | sed -E 's/.*Properties2 +([0-9]+) sync.*/\1/' | head -1)
echo "round trips: FormatProperties ${n1:-<100} FormatProperties2 ${n2:-<100}"
if [ -z "$n1" ] && { [ -z "$n2" ] || [ "$n2" -le 1100 ]; }; then echo "OK: step 2, round trips"; else echo "FAILED: step 2, round trips"; fails=1; fi
[ $fails = 0 ] && echo "OK: fmtcache_test" || echo "FAILED: fmtcache_test"
exit $fails
