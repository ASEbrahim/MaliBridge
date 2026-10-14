#!/bin/bash
# The compile watchdog (server_guard.c) on the desktop driver, with a server of its own: a
# pipeline compile that never ends (VKBRIDGE_TEST_CRASH_PIPELINE=spin; the Mali-G925's compiler
# spun for ever on Stray's pipelines) is interrupted after VKBRIDGE_COMPILE_TIMEOUT_S, fails, is
# remembered (asked again: fails at once) and leaves the device working. Also checks the
# stuck-pipelines file and the saved SPIR-V. Prints "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
work=$(mktemp -d)
sock=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-spin-$$.sock
VKBRIDGE_TEST_CRASH_PIPELINE=spin VKBRIDGE_COMPILE_WARN_S=1 VKBRIDGE_COMPILE_TIMEOUT_S=3 \
  VK_DRIVER_FILES=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json} VK_ICD_FILENAMES= \
  "$out/vkbridge-server" --socket "$sock" --log "$work/server.log" --cache-dir "$work/c" >/dev/null 2>&1 &
srv=$!
trap 'kill $srv 2>/dev/null; rm -f "$sock"; rm -rf "$work"' EXIT
for _ in $(seq 50); do [ -S "$sock" ] && break; sleep 0.1; done
VKBRIDGE_SOCKET=$sock VK_DRIVER_FILES=$out/vkbridge_icd.json VK_ICD_FILENAMES= timeout 120 "$out/crash_contain_check" --spin >"$work/t.log" 2>&1
r=$?
cat "$work/t.log"
[ $r = 0 ] && [ "$(grep -c 'TIMED OUT' "$work/server.log")" = 2 ] && [ "$(wc -l < "$work/c/stuck-pipelines")" = 2 ] &&
  [ "$(ls "$work/c/stuck-spirv" | wc -l)" = 3 ]
r=$?
grep -E "TIMED OUT" "$work/server.log" | cut -c1-200
if [ $r = 0 ]; then echo "OK: $(tail -1 "$work/t.log"), 2 interrupted, remembered, SPIR-V saved"; else echo "FAILED"; exit 1; fi
