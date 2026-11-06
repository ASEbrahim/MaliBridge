#!/bin/bash
# What a process leaves in the server when it ends without destroying its device (server_children.c;
# a player log, Tab S11 G925: the server's heap 195 -> 617 -> 1037 -> 1446 MB after three Skyrim SE runs,
# each "exited without destroying device"), tests/exitheap_check.c against one server per step:
#   1  exit x3 (pools, pipelines, command buffers alive at _exit): the server's heap (its memory
#      lines) after the third run within 8 MB of after the first; each cleanup says what it destroyed
#      ("left alive by its process ... 96 pipeline(s), 16 descriptor pool(s)"); no crash;
#   2  destroy x2 (the app destroys everything): nothing said, heap as flat, the next run still works;
#   3  VKBRIDGE_EXIT_CHILDREN=0, exit x3: the old cleanup: nothing said (and on lavapipe the heap
#      grows by ~70 MB a run, printed, not judged).
# Fails on 0fcf6d1 (no server_children.c): step 1 heap 72 -> 216 MB, nothing said.
# Prints one line per step and "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
work=$(mktemp -d)
sock=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-exitheap-$$.sock
srv=
stop() { [ -n "$srv" ] && kill $srv 2>/dev/null && wait $srv 2>/dev/null; srv=; }
trap 'stop; rm -f "$sock"; [ -n "$KEEP" ] && echo "kept $work" || rm -rf "$work"' EXIT
start() { # start <name> [env...]
  local d=$work/$1
  shift
  stop
  rm -f "$sock"
  mkdir -p "$d/c"
  log=$d/server.log
  env -u VKBRIDGE_FAKE_MISSING -u VKBRIDGE_HIDE_EXTS -u VKBRIDGE_MODE -u VKBRIDGE_MEM_STATS -u VKBRIDGE_EXIT_CHILDREN "$@" \
      VK_DRIVER_FILES=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json} VK_ICD_FILENAMES= \
      "$out/vkbridge-server" --socket "$sock" --log "$log" --cache-dir "$d/c" >/dev/null 2>&1 &
  srv=$!
  for _ in $(seq 100); do [ -S "$sock" ] && break; kill -0 $srv 2>/dev/null || break; sleep 0.1; done
}
export VKBRIDGE_SOCKET=$sock VK_DRIVER_FILES=$out/vkbridge_icd.json VK_ICD_FILENAMES=
unset VKBRIDGE_HIDE_EXTS VKBRIDGE_MAX_API VKBRIDGE_SYNC
fails=0 steps=0
step() { steps=$((steps+1)); if [ "$1" = 0 ]; then echo "  ok   $2"; else echo "  FAIL $2"; fails=$((fails+1)); fi; }
# the heap of the server's last memory line (they come every 5 s while anything changes)
heap() { sleep 6; grep -o ' heap [0-9]*M' "$log" | tail -1 | grep -o '[0-9]*'; }
runs() { # runs <mode> <n>: heaps after each run, space separated; rc != 0 if a run failed
  local h= rc=0
  for i in $(seq $2); do
    timeout 120 "$out/exitheap_check" $1 >>$work/client.out 2>&1 || { [ "$1" = destroy ] && rc=1; }
    grep -q "^made: 16 pools, 32768 sets, 96 pipelines, 256 command buffers" $work/client.out || rc=1
    h="$h $(heap)"
  done
  echo $h
  return $rc
}

# 1
start exit
: >$work/client.out
h=$(runs exit 3); rc=$?
said=$(grep -c "left alive by its process, destroyed before the device: .*96 pipeline(s), 16 descriptor pool(s)" "$log")
crash=$(grep -c "crashed with signal" "$log")
set -- $h
[ $rc = 0 ] && [ $# = 3 ] && [ $(( $3 - $1 )) -le 8 ] && [ "$said" = 3 ] && [ "$crash" = 0 ] && kill -0 $srv 2>/dev/null
step $? "exit x3: heap after each run:${h:+ $h} MB (third within 8 of the first), cleanups that said what they destroyed: $said of 3, crashes $crash"

# 2
start destroy
: >$work/client.out
h=$(runs destroy 2); rc=$?
set -- $h
[ $rc = 0 ] && [ $# = 2 ] && [ $(( $2 - $1 )) -le 8 ] && ! grep -q "left alive by its process" "$log" && grep -q "^OK: destroyed" $work/client.out
step $? "destroy x2: heap $h MB, nothing destroyed for the app (it did it itself)"

# 3
start off VKBRIDGE_EXIT_CHILDREN=0
: >$work/client.out
h=$(runs exit 3); rc=$?
[ $rc = 0 ] && ! grep -q "left alive by its process" "$log" && [ "$(grep -c 'cleaning up' "$log")" -ge 3 ]
step $? "VKBRIDGE_EXIT_CHILDREN=0, exit x3: the old cleanup, heap $h MB (not judged)"
stop

if [ $fails = 0 ]; then echo "OK: $steps steps"; else echo "FAILED: $fails of $steps steps"; exit 1; fi
