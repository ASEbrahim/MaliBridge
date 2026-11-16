#!/bin/bash
# What the server does with a device after its loss (a player log: POCO X6 Pro, Mali-G615 r44p1, Resident Evil
# Village; the device was lost 25 s into play and the game went on compiling and submitting for a minute):
#   - its pipeline cache is never written or merged again: the file stays as it was last saved before the loss
#     (a player log saved 68 MB and 74 MB of a lost device's cache over the one from before, and the next start loaded it);
#     said once per device;
#   - the asynchronous calls answered VK_ERROR_DEVICE_LOST (the client had already said VK_SUCCESS) are counted,
#     not logged one line each (a player log: 42867 such lines, 65% of the server log): the first one, a count at most every
#     10 s, the total when the device goes.
# pcache_check (tests/pcache_check.c) compiles pipelines, waits until the test has seen its cache saved, loses its
# device (VKBRIDGE_TEST_LOST, server_lost.c), compiles as many again, floods 2 x FLOOD empty submissions 12 s apart
# (a periodic save is due meanwhile) and tears down.
#   1  the server's cache: the file after the run is the file from before the loss (bytes and time), no save of
#      that device after the loss line, the "not saved: the device was lost" line once;
#   2  the asynchronous lost answers: at most one line each, a count line for the second flood, and the device's
#      total at its end covers both floods;
#   3  the app's own pipeline caches (--app-caches 1: merged into the server's when compiled into and when they go):
#      the file stays as it was too;
#   4  control, same runs without the loss: the pipelines compiled after the hold do reach the file (it is written
#      again), so steps 1 and 3 can fail on this driver.
# VKBRIDGE_TEST_PCACHE_GROW=1 (server_pcache.c): lavapipe's cache data keeps one size whatever is compiled, so no save
# after the first would ever happen on it; the imitation makes each pipeline count as growth (the bytes written are
# still the driver's own). On RADV it adds to the real growth.
# Prints one line per step and "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
FLOOD=${FLOOD:-5000}
work=$(mktemp -d)
srv=
trap '[ -n "$srv" ] && kill $srv 2>/dev/null; rm -f "$work"/*.sock; [ -n "$KEEP" ] && echo "kept $work" || rm -rf "$work"' EXIT
fails=0 steps=0
step() { steps=$((steps+1)); if [ "$1" = 0 ]; then echo "  ok   $2"; else echo "  FAIL $2"; fails=$((fails+1)); fi; }

# start <name> VAR=value...
start() {
  local name=$1; shift
  sock=$work/$name.sock log=$work/$name.log cache=$work/c-$name
  env VKBRIDGE_PROBE=0 VKBRIDGE_TEST_PCACHE_GROW=1 "$@" VK_DRIVER_FILES=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json} VK_ICD_FILENAMES= \
      "$out/vkbridge-server" --socket "$sock" --log "$log" --cache-dir "$cache" >/dev/null 2>&1 &
  srv=$!
  for _ in $(seq 50); do [ -S "$sock" ] && break; sleep 0.1; done
}
stop() { kill $srv 2>/dev/null; wait $srv 2>/dev/null; srv=; }
file() { ls "$cache"/*-pcache_check.vkpc 2>/dev/null | head -1; }
ident() { local f; f=$(file); [ -n "$f" ] && echo "$(stat -c '%s %i %y' "$f") $(sha1sum <"$f" | cut -c1-12)"; }

# run <name> <pcache_check args...>: the program in the background; when the server says its cache was saved, the
# trigger file lets it lose its device; the file's identity is taken at "lost at round" (before any later save).
run() {
  local name=$1; shift
  rm -f "$work/$name.go"
  VKBRIDGE_SOCKET=$sock VK_DRIVER_FILES=$out/vkbridge_icd.json VK_ICD_FILENAMES= timeout 120 \
    "$out/pcache_check" --lose-when "$work/$name.go" "$@" >"$work/$name.out" 2>&1 &
  local pid=$!
  for _ in $(seq 400); do grep -q "pipeline cache saved" "$log" && break; kill -0 $pid 2>/dev/null || break; sleep 0.1; done
  touch "$work/$name.go"
  before=
  for _ in $(seq 300); do grep -q "lost at round\|not lost in" "$work/$name.out" && break; kill -0 $pid 2>/dev/null || break; sleep 0.05; done
  before=$(ident)
  wait $pid; rc=$?
  sleep 0.3
  after=$(ident)
}
# lines of the log after the loss (the latch's line); the control has none: all of it after the trigger
after_loss() { awk '/ is lost \(/{on=1} on' "$log"; }

start srv VKBRIDGE_TEST_LOST=3 VKBRIDGE_TEST_LOST_COMM=pcache_check
run one --count 40 --flood "$FLOOD" --after-hold 12
dev=$(grep -m1 -o 'device [0-9]* (pid [0-9]*) is lost' "$log" | awk '{print $2}')
nsave=$(after_loss | grep -c "device $dev: pipeline cache saved")
nskip=$(grep -c "device $dev (pid [0-9]*): pipeline cache .* not saved .*: the device was lost" "$log")
[ "$rc" = 0 ] && grep -q "lost at round" "$work/one.out" && [ -n "$dev" ] && [ -n "$before" ] && [ "$before" = "$after" ] && [ "$nsave" = 0 ] && [ "$nskip" = 1 ]
step $? "1 a lost device's cache is not saved: file before the loss [$before], after the run [$after]; saves after the loss $nsave, 'not saved' lines $nskip ($(tail -1 "$work/one.out")): $(grep -m1 'not saved .*: the device was lost' "$log" | sed 's/.*vkbridge-server . //' | cut -c1-160)"
nline=$(grep -c "asynchronous vkQueueSubmit returned -4" "$log")
nsum=$(grep -c "more asynchronous call(s) answered VK_ERROR_DEVICE_LOST" "$log")
tot=$(grep -o "device $dev: [0-9]* asynchronous call(s) answered VK_ERROR_DEVICE_LOST after its loss" "$log" | awk '{print $3}')
[ "$nline" -le 1 ] && [ "$nsum" -ge 1 ] && [ "$nsum" -le 3 ] && [ "${tot:-0}" -ge $((2*FLOOD)) ]
step $? "2 asynchronous lost answers counted: $nline line(s) one each, $nsum count line(s), total at the device's end ${tot:-none} (>= $((2*FLOOD))): $(grep -m1 'more asynchronous call(s) answered' "$log" | sed 's/.*vkbridge-server . //' | cut -c1-160)"
stop

start app VKBRIDGE_TEST_LOST=3 VKBRIDGE_TEST_LOST_COMM=pcache_check
run app --count 40 --app-caches 1 --flood 10 --after-hold 12
dev=$(grep -m1 -o 'device [0-9]* (pid [0-9]*) is lost' "$log" | awk '{print $2}')
nsave=$(after_loss | grep -c "device $dev: pipeline cache saved")
[ "$rc" = 0 ] && [ -n "$dev" ] && [ -n "$before" ] && [ "$before" = "$after" ] && [ "$nsave" = 0 ]
step $? "3 the app's own caches are not merged in after the loss: file before [$before], after [$after]; saves after the loss $nsave ($(tail -1 "$work/app.out"))"
stop

# control: nothing is lost (another program name), the same runs
start ctl VKBRIDGE_TEST_LOST=3 VKBRIDGE_TEST_LOST_COMM=nothing_named_so
run ctl --count 40 --flood 10 --after-hold 12
c1b=$before c1a=$after r1=$rc
stop
start ctlapp VKBRIDGE_TEST_LOST=3 VKBRIDGE_TEST_LOST_COMM=nothing_named_so
run ctlapp --count 40 --app-caches 1 --flood 10 --after-hold 12
c2b=$before c2a=$after r2=$rc
stop
[ "$r1" = 0 ] && [ "$r2" = 0 ] && [ -n "$c1b" ] && [ -n "$c2b" ] && [ -n "$c1a" ] && [ -n "$c2a" ] && [ "$c1a" != "$c1b" ] && [ "$c2a" != "$c2b" ]
step $? "4 control without a loss: the file is written again after the hold (server's cache [$c1b] -> [$c1a], app caches [$c2b] -> [$c2a])"

if [ $fails = 0 ]; then echo "OK: $steps steps"; else echo "FAILED: $fails of $steps steps"; [ -n "$KEEP" ] || for f in "$work"/*.log; do echo "--- $f"; grep -E " E | W " "$f" | tail -6; done; fi
[ $fails = 0 ]
