#!/bin/bash
# What the stuck-pipelines list (server_guard.c) keeps between runs. The compile watchdog cannot tell
# a slow compile from a spinning one when it cuts a call off, so a line in the list is a suspect, not
# a verdict: the constant-colour stand-in draws wrong, and it must not stay for good on a pipeline the
# driver can build. Each step runs one server after another on one cache directory (one boot each):
#   1 slow: a compile of 3 s under a limit of 2 s: stand-in in the first run, the program's own pipeline
#     in the second (compiled again with 4 s, kept as "built with a longer limit") and the third;
#   2 hang: a compile that never ends: stand-in, tried again with 4 s and 8 s (one interrupt per run),
#     then the stand-in at once with no interrupt: a driver hang stays recorded;
#   3 a run stops trying the list again after its first interrupt: two hanging pipelines, one interrupt;
#   4 a pipeline with no fallback that the driver builds when asked again (a player log's two: "the driver
#     built it this time" at every start) is off the list for good: no later run asks again;
#   5 a shader module on the list: was refused without a word for ever; the driver is asked again,
#     and one out of tries is refused with a line in the log.
# Prints "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
fails=0
sock=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-stuckretry-$$.sock
work=$(mktemp -d)
trap 'rm -rf "$work"; rm -f "$sock"' EXIT
# boot <check arg> VAR=value...: a server of its own on $work/c, the check against it, then both gone.
# Leaves the RESULT words in $res, the interrupts in $cut, the run's log in $work/server.log.
boot() {
  local arg=$1 srv; shift
  rm -f "$sock"
  : > "$work/server.log" # the server appends
  env "$@" VKBRIDGE_COMPILE_WARN_S=1 VK_DRIVER_FILES=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json} VK_ICD_FILENAMES= \
    "$out/vkbridge-server" --socket "$sock" --log "$work/server.log" --cache-dir "$work/c" >/dev/null 2>&1 &
  srv=$!
  for _ in $(seq 50); do [ -S "$sock" ] && break; sleep 0.1; done
  VKBRIDGE_SOCKET=$sock VK_DRIVER_FILES=$out/vkbridge_icd.json VK_ICD_FILENAMES= timeout 120 "$out/crash_contain_check" "$arg" >"$work/t.log" 2>&1
  kill $srv 2>/dev/null; wait $srv 2>/dev/null
  res=$(grep -o '^RESULT [0-9]* [a-z]*' "$work/t.log" | awk '{print $3}' | tr '\n' ' ')
  cut=$(grep -c 'TIMED OUT' "$work/server.log")
}
list() { sed 's/ #.*//; s/.* m=/m=/' "$work/c/stuck-pipelines" 2>/dev/null | tr '\n' ' '; }
step() { # step <0|1> <text>
  if [ "$1" = 0 ]; then echo "  ok   $2"; else echo "  FAIL $2"; fails=$((fails+1)); fi
}
slowenv="VKBRIDGE_TEST_CRASH_PIPELINE=slow VKBRIDGE_TEST_SLOW_S=3 VKBRIDGE_COMPILE_TIMEOUT_S=2"
spinenv="VKBRIDGE_TEST_CRASH_PIPELINE=spin VKBRIDGE_COMPILE_TIMEOUT_S=1"

# 1 slow
rm -rf "$work/c"; bad=
boot --standin $slowenv
echo "  slow boot 1: $res($cut interrupted) list: $(list)"
[ "$res" = "standin standin " ] && [ "$cut" = 1 ] && grep -q " m=2 v=3 t=1 " "$work/c/stuck-pipelines" || bad="$bad boot1"
boot --standin $slowenv
echo "  slow boot 2: $res($cut interrupted) list: $(list)"
[ "$res" = "real real " ] && [ "$cut" = 0 ] || bad="$bad boot2"
tail -1 "$work/c/stuck-pipelines" | grep -q " m=5 v=3 t=2 " || bad="$bad boot2-list"
boot --standin $slowenv
echo "  slow boot 3: $res($cut interrupted)"
[ "$res" = "real real " ] && [ "$cut" = 0 ] && ! grep -q "compiled again" "$work/server.log" || bad="$bad boot3"
step $([ -z "$bad" ] && echo 0 || echo 1) "a compile slower than the limit that does finish: the real pipeline from the next run on$bad"

# 2 hang
rm -rf "$work/c"; bad=
boot --standin $spinenv
echo "  hang boot 1: $res($cut interrupted)"
[ "$res" = "standin standin " ] && [ "$cut" = 1 ] || bad="$bad boot1"
boot --standin $spinenv
echo "  hang boot 2: $res($cut interrupted) list: $(list)"
[ "$res" = "standin standin " ] && [ "$cut" = 1 ] && grep -q "compiled again with 2 s" "$work/server.log" || bad="$bad boot2"
boot --standin $spinenv
echo "  hang boot 3: $res($cut interrupted)"
[ "$res" = "standin standin " ] && [ "$cut" = 1 ] && grep -q "compiled again with 4 s" "$work/server.log" || bad="$bad boot3"
boot --standin $spinenv
echo "  hang boot 4: $res($cut interrupted)"
[ "$res" = "standin standin " ] && [ "$cut" = 0 ] && ! grep -q "compiled again" "$work/server.log" || bad="$bad boot4"
grep -q "RESULT 0 standin 0\." "$work/t.log" || bad="$bad boot4-not-at-once"
step $([ -z "$bad" ] && echo 0 || echo 1) "a compile that never ends: stays recorded, tried again twice, then the stand-in at once$bad"

# 3 one interrupt per run
rm -rf "$work/c"; bad=
boot --standin2 $spinenv
echo "  two hangs boot 1: $res($cut interrupted)"
[ "$cut" = 2 ] || bad="$bad boot1"
boot --standin2 $spinenv
echo "  two hangs boot 2: $res($cut interrupted)"
[ "$res" = "standin standin standin standin " ] && [ "$cut" = 1 ] || bad="$bad boot2"
step $([ -z "$bad" ] && echo 0 || echo 1) "a run's first interrupted retry ends the retries of that run$bad"

# 4 no fallback, built when asked again (the vertex stage: a constant-colour fragment does not help)
rm -rf "$work/c"; bad=
boot --again VKBRIDGE_TEST_CRASH_PIPELINE=spin VKBRIDGE_COMPILE_TIMEOUT_S=1
grep -q " m=0 v=3 t=1 " "$work/c/stuck-pipelines" || bad="$bad boot1"
boot --again VKBRIDGE_COMPILE_TIMEOUT_S=1
echo "  asked again boot 2: $res list: $(list)"
[ "$res" = "built built " ] && grep -q "driver built it this time" "$work/server.log" || bad="$bad boot2"
tail -1 "$work/c/stuck-pipelines" | grep -q " m=5 v=3 t=1 " || bad="$bad boot2-list"
boot --again VKBRIDGE_COMPILE_TIMEOUT_S=1
echo "  asked again boot 3: $res"
[ "$res" = "built built " ] && ! grep -q "never finished compiling" "$work/server.log" || bad="$bad boot3"
step $([ -z "$bad" ] && echo 0 || echo 1) "a pipeline the driver built when asked again leaves the list$bad"

# 5 a module on the list (its hash from a pipeline's note; every module of the check is built plainly here)
rm -rf "$work/c"; bad=
boot --standin $slowenv
h=$(sed -n 's/.*0x10=h-\([0-9a-f]*\).*/\1/p' "$work/c/stuck-pipelines" | head -1)
[ -n "$h" ] || bad="$bad no-hash"
echo "k-$h m=0 v=3 t=1 # fails: vkCreateShaderModule, stages 0x0=h-$h" > "$work/c/stuck-pipelines"
boot --standin VKBRIDGE_COMPILE_TIMEOUT_S=2
echo "  module boot 2: $res list: $(list)"
[ "$res" = "real real " ] || bad="$bad boot2"
grep -q "a shader module the driver never finished compiling: the driver is asked again" "$work/server.log" || bad="$bad boot2-line"
tail -1 "$work/c/stuck-pipelines" | grep -q " m=5 v=3 t=1 " || bad="$bad boot2-list"
# ... and one whose tries are used up (a module that hung each time) is refused at once, and it says so
echo "k-$h m=0 v=3 t=3 # fails: vkCreateShaderModule, stages 0x0=h-$h" > "$work/c/stuck-pipelines"
boot --standin VKBRIDGE_COMPILE_TIMEOUT_S=2
echo "  module boot 3: $res"
[ "$res" = "failed failed " ] && grep -q "shader module the driver never finished compiling was asked for again: refused" "$work/server.log" || bad="$bad used-up"
step $([ -z "$bad" ] && echo 0 || echo 1) "a shader module on the list is asked of the driver again, and leaves the list$bad"

if [ $fails = 0 ]; then echo "OK: 5 steps"; else echo "FAILED: $fails of 5 steps"; exit 1; fi
