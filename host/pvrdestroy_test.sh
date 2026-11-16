#!/bin/bash
# A device ended with render objects alive in it, on a driver that cannot take that (a player log, Pixel 10
# PowerVR, test43: the self-test's vkDestroyDevice never returned, gamescope's vkCreateDevice waited behind
# it, Steam never started). The server imitates that driver (VKBRIDGE_FAKE_MISSING=pvrdestroy:
# vkDestroyDevice with a live image/view/render pass/framebuffer blocks for good, and so does every
# device made after it); tests/pvrdestroy_check.c and the self-test (vkbridge-smoke) end their devices
# in each way a process can, and after each a second process must get a device (as gamescope).
# One server per step:
#   0  imitation, VKBRIDGE_DESTROY_CHILDREN=0 (the fix off): the leaking destroy hangs (the imitation works)
#   1  imitation, "leak" (vkDestroyDevice with all alive, as the self-test): returns; the server says what
#      it destroyed first (images, views, render passes, framebuffers...); the next process gets a device
#   2  imitation, "exit" (_exit with all alive: the process cleanup): the next process gets a device
#   3  imitation, "kill" (SIGKILL mid-session): the next process gets a device
#   4  imitation, "clean" (the app destroys everything): nothing said, the next process gets a device
#   5  imitation, vkbridge-smoke (the self-test as DroidDeck runs it, leaking): returns; the next gets a device
#   6  no imitation (lavapipe as itself, not PowerVR): leak + exit as before: no PowerVR line, nothing
#      destroyed for the app, the process cleanup names no image kind (the Mali path is not taken)
# Fails on 645895d (steps 1, 2, 3, 5: the destroy or the next vkCreateDevice never returns).
# Prints one line per step and "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
work=$(mktemp -d)
sock=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-pvrdestroy-$$.sock
srv=
stop() { [ -n "$srv" ] && kill -9 $srv 2>/dev/null && wait $srv 2>/dev/null; srv=; }
cpid=
# the "kill" helper never outlives the test (it also ends itself after 60 s: alarm in pvrdestroy_check)
reap() { [ -n "$cpid" ] && kill -9 $cpid 2>/dev/null && wait $cpid 2>/dev/null; cpid=; }
trap 'reap; stop; rm -f "$sock"; [ -n "$KEEP" ] && echo "kept $work" || rm -rf "$work"' EXIT
trap 'exit 143' TERM INT HUP # so the EXIT trap reaps the helper and the server under timeout too
start() { # start <name> [env...]
  local d=$work/$1
  shift
  stop
  rm -f "$sock"
  mkdir -p "$d/c"
  log=$d/server.log
  env -u VKBRIDGE_FAKE_MISSING -u VKBRIDGE_HIDE_EXTS -u VKBRIDGE_MODE -u VKBRIDGE_EXIT_CHILDREN -u VKBRIDGE_DESTROY_CHILDREN "$@" \
      VK_DRIVER_FILES=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json} VK_ICD_FILENAMES= \
      "$out/vkbridge-server" --socket "$sock" --log "$log" --cache-dir "$d/c" >/dev/null 2>&1 &
  srv=$!
  for _ in $(seq 100); do [ -S "$sock" ] && break; kill -0 $srv 2>/dev/null || break; sleep 0.1; done
  cl=$d/client.out
  : >$cl
}
export VKBRIDGE_SOCKET=$sock VK_DRIVER_FILES=$out/vkbridge_icd.json VK_ICD_FILENAMES=
unset VKBRIDGE_HIDE_EXTS VKBRIDGE_MAX_API VKBRIDGE_SYNC
fails=0 steps=0
step() { steps=$((steps+1)); if [ "$1" = 0 ]; then echo "  ok   $2"; else echo "  FAIL $2"; fails=$((fails+1)); fi; }
FAKE=VKBRIDGE_FAKE_MISSING=pvrdestroy
# the second process (gamescope's place): a device made and destroyed within 20 s
next_ok() { timeout 20 "$out/pvrdestroy_check" clean >>$cl 2>&1 && grep -q "^OK: device destroyed (clean)" $cl; }
hung() { grep -c "imitated PowerVR driver" "$log"; } # a call the imitation blocked

# 0
start off $FAKE VKBRIDGE_DESTROY_CHILDREN=0
timeout 10 "$out/pvrdestroy_check" leak >>$cl 2>&1; rc=$?
[ $rc = 124 ] && grep -q "imitating a PowerVR driver" "$log" && [ "$(hung)" -ge 1 ]
step $? "imitation with the fix off: the leaking destroy hangs (client rc $rc, calls blocked $(hung))"

# 1
start leak $FAKE
timeout 20 "$out/pvrdestroy_check" leak >>$cl 2>&1; rc=$?
said=$(grep -o "destroyed first:.*" "$log" | head -1)
next_ok; nx=$?
[ $rc = 0 ] && [ $nx = 0 ] && [ "$(hung)" = 0 ] && echo "$said" | grep -q "image(s)" && echo "$said" | grep -q "framebuffer(s)" && grep -q "PowerVR: what" "$log"
step $? "leak: destroy rc $rc, next device rc $nx, hangs $(hung), said: ${said:-nothing}"

# 2
start exit $FAKE
timeout 20 "$out/pvrdestroy_check" exit >>$cl 2>&1; rc=$?
for _ in $(seq 50); do grep -q "left alive by its process" "$log" && break; sleep 0.1; done
said=$(grep -o "left alive by its process.*" "$log" | head -1)
next_ok; nx=$?
[ $rc = 0 ] && [ $nx = 0 ] && [ "$(hung)" = 0 ] && echo "$said" | grep -q "image(s)"
step $? "exit: next device rc $nx, hangs $(hung), said: ${said:-nothing}"

# 3
start kill $FAKE
"$out/pvrdestroy_check" kill >>$cl 2>&1 &
cpid=$!
for _ in $(seq 100); do grep -q "^made:" $cl && break; sleep 0.1; done
grep -q "^made:" $cl; made=$?
kill -9 $cpid 2>/dev/null; wait $cpid 2>/dev/null
kill -0 $cpid 2>/dev/null && { echo "  FAIL kill: helper $cpid still alive after SIGKILL"; made=1; }
cpid=
for _ in $(seq 50); do grep -q "left alive by its process" "$log" && break; sleep 0.1; done
said=$(grep -o "left alive by its process.*" "$log" | head -1)
next_ok; nx=$?
[ $made = 0 ] && [ $nx = 0 ] && [ "$(hung)" = 0 ] && echo "$said" | grep -q "image(s)"
step $? "kill (SIGKILL after 'made': $made): next device rc $nx, hangs $(hung), said: ${said:-nothing}"

# 4
start clean $FAKE
next_ok; rc=$?
next_ok; nx=$?
[ $rc = 0 ] && [ $nx = 0 ] && [ "$(hung)" = 0 ] && ! grep -q "destroyed first:\|left alive by its process" "$log"
step $? "clean: rc $rc, next device rc $nx, hangs $(hung), nothing destroyed for the app"

# 5
start smoke $FAKE
timeout 120 "$out/vkbridge-smoke" >>$cl 2>&1; rc=$?
said=$(grep -o "destroyed first:.*" "$log" | head -1)
next_ok; nx=$?
[ $rc != 124 ] && [ $nx = 0 ] && [ "$(hung)" = 0 ] && grep -q "checks passed" $cl
step $? "self-test (vkbridge-smoke, leaking): rc $rc ($(grep -o '[0-9]* of [0-9]* checks passed' $cl)), next device rc $nx, hangs $(hung), said: ${said:-nothing}"

# 6
start lavapipe
timeout 20 "$out/pvrdestroy_check" leak >>$cl 2>&1; rc=$?
timeout 20 "$out/pvrdestroy_check" exit >>$cl 2>&1; rc2=$?
sleep 1
next_ok; nx=$?
[ $rc = 0 ] && [ $rc2 = 0 ] && [ $nx = 0 ] && ! grep -q "PowerVR: what\|destroyed first:\|imitating a PowerVR" "$log" && ! grep "left alive by its process" "$log" | grep -q "image\|view\|render pass\|framebuffer\|sampler\|buffer\|fence\|semaphore\|event"
step $? "not PowerVR: leak rc $rc, exit rc $rc2, next rc $nx, no PowerVR line, nothing destroyed for the app, no new kinds at cleanup"
stop

if [ $fails = 0 ]; then echo "OK: $steps steps"; else echo "FAILED: $fails of $steps steps"; exit 1; fi
