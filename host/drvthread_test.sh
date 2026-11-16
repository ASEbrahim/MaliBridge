#!/bin/bash
# A crash on the driver's OWN thread while pipelines compile (a player log, Street Fighter 6 on a Mali-G1-Ultra
# r54p1: SIGSEGV at 0x8 in libGLES_mali.so on thread "mali-compiler", two vkCreateGraphicsPipelines in flight;
# a player log on a POCO X7 Pro the same). The crash guard's arm is per thread (server_guard.c
# vkb_guard_fault), so such a fault cannot be contained and the process dies. What the bridge can do is
# remember which compiles were in flight, so the next start gives them the stand-in the compile watchdog
# gives hung ones (the stuck-pipelines list) and the game gets past the spot.
# Imitation: VKBRIDGE_TEST_CRASH_PIPELINE=worker (server_guard.c test_fault): the marked pipeline's compile
# has a thread named "mali-compiler" fault inside libc while the call waits for it. Each step runs one
# server after another on one cache directory (one boot each), with crash_contain_check --standin
# (the pipeline whose fragment stage is marked, asked for twice: real / standin / failed):
#   1 the run after the crash gets past it: the stand-in at once, no second crash, a line for it;
#   2 a suspect the driver builds later leaves the list (the real pipeline; a later run does not ask);
#   3 a suspect that crashes the driver again when asked keeps the stand-in for good (no third crash);
#   4 a server killed (SIGKILL, as a low-memory kill) while a compile is in flight leaves no suspect;
#   5 a fault in the server's own code while a compile is in flight leaves no suspect (not the compile's).
# Prints "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
fails=0
sock=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-drvthread-$$.sock
work=$(mktemp -d)
trap 'rm -rf "$work"; rm -f "$sock"' EXIT
icd=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json}
# start VAR=value...: a server of its own on $work/c (pid in $srv)
start() {
  rm -f "$sock"
  : > "$work/server.log" # the server appends
  env "$@" VK_DRIVER_FILES=$icd VK_ICD_FILENAMES= \
    "$out/vkbridge-server" --socket "$sock" --log "$work/server.log" --cache-dir "$work/c" >/dev/null 2>&1 &
  srv=$!
  for _ in $(seq 50); do [ -S "$sock" ] && break; sleep 0.1; done
}
# boot VAR=value...: start, crash_contain_check --standin against it, then both gone. RESULT words in $res.
boot() {
  start "$@"
  VKBRIDGE_SOCKET=$sock VK_DRIVER_FILES=$out/vkbridge_icd.json VK_ICD_FILENAMES= timeout 120 "$out/crash_contain_check" --standin >"$work/t.log" 2>&1
  kill $srv 2>/dev/null; wait $srv 2>/dev/null
  res=$(grep -o '^RESULT [0-9]* [a-z]*' "$work/t.log" | awk '{print $3}' | tr '\n' ' ')
  crashed=$(grep -c 'crashed with signal' "$work/server.log")
}
list() { sed 's/ #.*//; s/.* m=/m=/' "$work/c/stuck-pipelines" 2>/dev/null | tr '\n' ' '; }
step() { # step <0|1> <text>
  if [ "$1" = 0 ]; then echo "  ok   $2"; else echo "  FAIL $2"; fails=$((fails+1)); fi
}
wenv="VKBRIDGE_TEST_CRASH_PIPELINE=worker"

# 1 the crash, then the run after it (the driver still crashes on that pipeline)
rm -rf "$work/c"; bad=
boot $wenv
echo "  crash boot 1: '$res' crashed=$crashed list: $(list)"
[ "$crashed" = 1 ] || bad="$bad boot1-no-crash"
grep -q "marked as suspects" "$work/server.log" || bad="$bad boot1-no-line"
boot $wenv
echo "  crash boot 2: '$res' crashed=$crashed list: $(list)"
[ "$res" = "standin standin " ] && [ "$crashed" = 0 ] || bad="$bad boot2"
grep -q "was being compiled when the driver crashed" "$work/server.log" || bad="$bad boot2-line"
step $([ -z "$bad" ] && echo 0 || echo 1) "the run after a crash on the driver's thread gets past it with the stand-in$bad"

# 2 the driver builds it later (a race of two compiles, the other one the culprit): off the list
boot
echo "  cleared boot 3: '$res' crashed=$crashed list: $(list)"
[ "$res" = "real real " ] && [ "$crashed" = 0 ] || bad="$bad boot3"
grep -q "no longer a crash suspect" "$work/server.log" || bad="$bad boot3-line"
tail -1 "$work/c/stuck-pipelines" 2>/dev/null | grep -q " m=5 v=3 t=1" || bad="$bad boot3-list"
boot
echo "  cleared boot 4: '$res' crashed=$crashed"
[ "$res" = "real real " ] && ! grep -qE "asked again|never finished compiling|crashed in an earlier run" "$work/server.log" || bad="$bad boot4"
step $([ -z "$bad" ] && echo 0 || echo 1) "a suspect the driver builds when asked again leaves the list$bad"

# 3 the driver crashes on it each time: one try, then the stand-in for good
rm -rf "$work/c"; bad=
boot $wenv; boot $wenv
[ "$res" = "standin standin " ] || bad="$bad boot2"
boot $wenv
echo "  again boot 3: '$res' crashed=$crashed list: $(list)"
[ "$crashed" = 1 ] || bad="$bad boot3-no-try"
boot $wenv
echo "  again boot 4: '$res' crashed=$crashed list: $(list)"
[ "$res" = "standin standin " ] && [ "$crashed" = 0 ] && grep -q "for good" "$work/server.log" || bad="$bad boot4"
boot $wenv
echo "  again boot 5: '$res' crashed=$crashed"
[ "$res" = "standin standin " ] && [ "$crashed" = 0 ] || bad="$bad boot5"
step $([ -z "$bad" ] && echo 0 || echo 1) "a suspect that crashes the driver again when asked keeps the stand-in for good$bad"

# 4 killed (not crashed) while a compile is in flight: nothing is remembered
rm -rf "$work/c"; bad=
start VKBRIDGE_TEST_CRASH_PIPELINE=slow VKBRIDGE_TEST_SLOW_S=30 VKBRIDGE_COMPILE_TIMEOUT_S=0
VKBRIDGE_SOCKET=$sock VK_DRIVER_FILES=$out/vkbridge_icd.json VK_ICD_FILENAMES= timeout 60 "$out/crash_contain_check" --standin >"$work/t.log" 2>&1 &
cl=$!
for _ in $(seq 100); do grep -q "compiling slowly on purpose" "$work/server.log" && break; sleep 0.1; done
grep -q "compiling slowly on purpose" "$work/server.log" || bad="$bad no-compile-in-flight"
kill -9 $srv; wait $srv 2>/dev/null; kill $cl 2>/dev/null; wait $cl 2>/dev/null
echo "  killed: list: '$(list)'"
grep -q "x=" "$work/c/stuck-pipelines" 2>/dev/null && bad="$bad suspect-written"
boot
[ "$res" = "real real " ] || bad="$bad next-run"
step $([ -z "$bad" ] && echo 0 || echo 1) "a server killed during a compile leaves no suspect$bad"

# 5 a fault in the server's own code on another thread while a compile is in flight: not the compile's
rm -rf "$work/c"; bad=
boot VKBRIDGE_TEST_CRASH_PIPELINE=workerself
echo "  own-code crash: crashed=$crashed list: '$(list)'"
[ "$crashed" = 1 ] || bad="$bad no-crash"
grep -q "x=" "$work/c/stuck-pipelines" 2>/dev/null && bad="$bad suspect-written"
boot
[ "$res" = "real real " ] || bad="$bad next-run"
step $([ -z "$bad" ] && echo 0 || echo 1) "a fault in the server's own code leaves no suspect$bad"

if [ $fails = 0 ]; then echo "OK: 5 steps"; else echo "FAILED: $fails of 5 steps"; exit 1; fi
