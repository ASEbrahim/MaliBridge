#!/bin/bash
# The compile watchdog and its fallbacks (server_guard.c) on the desktop driver, each run with a
# server of its own. A pipeline compile that never ends (VKBRIDGE_TEST_CRASH_PIPELINE=spin; the
# Mali-G925's compiler spun for ever on Stray's pipelines) is interrupted after
# VKBRIDGE_COMPILE_TIMEOUT_S and rebuilt with a fallback:
#   spin     the marked stage spins whatever is done to it: a marked vertex stage fails after the
#            fallbacks are tried, a marked fragment stage gets the stand-in fragment shader;
#   spinfc2  the marked stage compiles once float_controls2 is stripped (forced in this test;
#            that fallback is opt-in: VKBRIDGE_COMPILE_FALLBACKS=fc2).
# Also: remembered (asked again: the same outcome at once), the stuck-pipelines file and the saved
# SPIR-V. Prints "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
fails=0
run() {
  local mode=$1 work sock srv r
  work=$(mktemp -d)
  sock=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-spin-$$.sock
  VKBRIDGE_COMPILE_FALLBACKS=$([ "$mode" = spinfc2 ] && echo fc2) VKBRIDGE_TEST_CRASH_PIPELINE=$mode VKBRIDGE_COMPILE_WARN_S=1 VKBRIDGE_COMPILE_TIMEOUT_S=3 \
    VK_DRIVER_FILES=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json} VK_ICD_FILENAMES= \
    "$out/vkbridge-server" --socket "$sock" --log "$work/server.log" --cache-dir "$work/c" >/dev/null 2>&1 &
  srv=$!
  for _ in $(seq 50); do [ -S "$sock" ] && break; sleep 0.1; done
  VKBRIDGE_SOCKET=$sock VK_DRIVER_FILES=$out/vkbridge_icd.json VK_ICD_FILENAMES= timeout 180 "$out/crash_contain_check" --$mode >"$work/t.log" 2>&1
  r=$?
  grep -E "^(PASS|FAIL)" "$work/t.log"
  if [ "$mode" = spin ]; then
    [ $r = 0 ] && grep -q " m=0 " "$work/c/stuck-pipelines" && grep -q " m=2 " "$work/c/stuck-pipelines" && [ "$(ls "$work/c/stuck-spirv" | wc -l)" -ge 3 ]
  else
    [ $r = 0 ] && [ "$(grep -c " m=1 " "$work/c/stuck-pipelines")" = 2 ]
  fi
  r=$?
  echo "  $mode: $(tail -1 "$work/t.log"); $(grep -c 'TIMED OUT' "$work/server.log") interrupted; stuck-pipelines: $(sed 's/ #.*//; s/.* m=/m=/' "$work/c/stuck-pipelines" | tr '\n' ' ')"
  kill $srv 2>/dev/null; wait $srv 2>/dev/null; rm -f "$sock"; rm -rf "$work"
  [ $r = 0 ] || fails=$((fails+1))
}
# slow: a compute compile of 3 s under a 2 s limit, three runs sharing the cache directory. The
# first is interrupted (no fallback helps: "fails", t=1); the next run tries it again with 4 s and
# it builds (kept as "built with a longer limit"), and asked again it builds again; the third run
# builds it with 4 s at once.
slow() {
  local work sock srv run res want
  work=$(mktemp -d)
  sock=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-slow-$$.sock
  for run in 1 2 3; do
    VKBRIDGE_TEST_CRASH_PIPELINE=slow VKBRIDGE_TEST_SLOW_S=3 VKBRIDGE_COMPILE_WARN_S=1 VKBRIDGE_COMPILE_TIMEOUT_S=2 \
      VK_DRIVER_FILES=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json} VK_ICD_FILENAMES= \
      "$out/vkbridge-server" --socket "$sock" --log "$work/server$run.log" --cache-dir "$work/c" >/dev/null 2>&1 &
    srv=$!
    for _ in $(seq 50); do [ -S "$sock" ] && break; sleep 0.1; done
    VKBRIDGE_SOCKET=$sock VK_DRIVER_FILES=$out/vkbridge_icd.json VK_ICD_FILENAMES= timeout 120 "$out/crash_contain_check" --slow >"$work/t$run.log" 2>&1
    kill $srv 2>/dev/null; wait $srv 2>/dev/null; rm -f "$sock"
    res=$(grep -o 'RESULT [01] [a-z]*' "$work/t$run.log" | awk '{print $3}' | tr '\n' ' ')
    case $run in 1) want="failed failed " ;; *) want="built built " ;; esac
    echo "  slow run $run: $res(want $want); stuck-pipelines: $(sed 's/ #.*//; s/.* m=/m=/' "$work/c/stuck-pipelines" 2>/dev/null | tr '\n' ' ')"
    [ "$res" = "$want" ] || fails=$((fails+1))
  done
  tail -1 "$work/c/stuck-pipelines" | grep -q " m=5 .* t=2 " || { echo "  slow: no m=5 t=2 line"; fails=$((fails+1)); }
  rm -rf "$work"
}
run spin
run spinfc2
slow
if [ $fails = 0 ]; then echo "OK: spin, spinfc2 and slow (interrupted, fallbacks built, remembered, a slow compute pipeline retried)"; else echo "FAILED: $fails run(s)"; exit 1; fi
