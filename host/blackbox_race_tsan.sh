#!/bin/bash
# ThreadSanitizer check of the black box (server_report.c bb_*, with a device create/destroy loop): a
# TSan server (cmake -DTSAN=ON or make -C host TSAN=1, into out-tsan) writes its ring every second while programs create and
# destroy devices for 12 s. FAILED if a report names server_report.c in its SUMMARY line, or the ring did not follow the
# programs. Not part of run_tests.sh (needs the TSan build). Host ICD: VKB_HOST_ICD.
cd "$(dirname "$0")"
out=$PWD/out-tsan
[ -x "$out/vkbridge-server" ] || { echo "FAILED: build first: make -C host TSAN=1"; exit 1; }
logs=$(mktemp -d)
sock=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-bbtsan-$$.sock
TSAN_OPTIONS="log_path=$logs/t halt_on_error=0 history_size=4" VKBRIDGE_PROBE=0 \
  VK_DRIVER_FILES=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json} VK_ICD_FILENAMES= \
  setarch -R "$out/vkbridge-server" --socket "$sock" --log "$logs/vkbridge-server.log" --cache-dir "$logs/c" >"$logs/srv.out" 2>&1 &
srv=$!
trap 'kill $srv 2>/dev/null; rm -f "$sock"' EXIT
for _ in $(seq 100); do [ -S "$sock" ] && break; sleep 0.1; done
end=$((SECONDS + 12)) n=0
while [ $SECONDS -lt $end ]; do
  VKBRIDGE_SOCKET=$sock VK_DRIVER_FILES=$out/vkbridge_icd.json VK_ICD_FILENAMES= timeout 30 setarch -R "$out/pcache_check" \
    --count 1 --first $((n + 1)) --mem 4 >/dev/null 2>&1
  n=$((n + 1))
done
sleep 2
kill $srv; wait $srv 2>/dev/null
named=$(tr -d '\000' < "$logs/vkbridge-blackbox.bin" 2>/dev/null | grep -c pcache_check)
if grep -h 'SUMMARY: ThreadSanitizer' "$logs"/t.* 2>/dev/null | grep -q 'server_report\.c'; then
  grep -h 'SUMMARY: ThreadSanitizer' "$logs"/t.* | grep 'server_report\.c' | sort -u
  echo "FAILED: data race in server_report.c (reports in $logs)"; exit 1
fi
[ "${named:-0}" -ge 3 ] || { echo "FAILED: the ring named the programs in ${named:-0} records (logs in $logs)"; exit 1; }
other=$(grep -h 'SUMMARY: ThreadSanitizer' "$logs"/t.* 2>/dev/null | sort -u | wc -l)
rm -rf "$logs"
echo "OK: $n device create/destroy runs under TSan, $named ring records naming them, no race in server_report.c ($other other report(s))"
