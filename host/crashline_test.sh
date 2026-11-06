#!/bin/bash
# The crash line (server_main.c on_crash, server_crashctx.c) names whose call died and what it was
# given. the one server died in Mali's vkBindImageMemory and the line said only that;
# binds are asynchronous, so nothing tied the death to a program, an image or a memory. 
# a near-null read in the driver's vkCmdDrawIndexed with no word on the bound pipeline.
# VKBRIDGE_TEST_CRASH_CMD makes a client's call of that name fault at the driver boundary.
#   1  a bind (vkbridge-smoke's image): the line names vkbridge-smoke and its pid, the image (extent,
#      format, tiling, usage, the size it needs) and the memory (size, type, how it was made);
#   2  a draw inside a command buffer's replay: the line names the client, the replayed commands and
#      the bound graphics pipeline with its topology and line width;
#   3  the other connection threads' calls in flight are listed ("none" when there are none);
#   4  an allocation (Mali's vkAllocateMemory with two dma-buf allocations in flight): the
#      line names the client, the size, the memory type and how the memory is made;
#   5  the faulting thread is named and the faulting address placed: the mapping it is in, or the
#      mappings around it (a player log/5PA6: SEGV_ACCERR far from every pointer the bridge saw).
# Prints one line per step and "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
work=$(mktemp -d)
trap '[ -n "$srv" ] && kill $srv 2>/dev/null; rm -f "$work"/*.sock; [ -n "$KEEP" ] && echo "kept $work" || rm -rf "$work"' EXIT
fails=0 steps=0
step() { steps=$((steps+1)); if [ "$1" = 0 ]; then echo "  ok   $2"; else echo "  FAIL $2"; fails=$((fails+1)); fi; }

# run <name> <command>: a server whose client's <command> faults; vkbridge-smoke against it
run() {
  local name=$1 cmd=$2 sock=$work/$1.sock
  # the memory calls' faults are contained (server_guard.c): off here, the line is what is tested
  env VKBRIDGE_NO_CRASH_GUARD=1 VKBRIDGE_TEST_CRASH_ACCERR=${VKBRIDGE_TEST_CRASH_ACCERR:-} VKBRIDGE_PROBE=0 VKBRIDGE_TEST_CRASH_CMD=$cmd VK_DRIVER_FILES=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json} VK_ICD_FILENAMES= \
      "$out/vkbridge-server" --socket "$sock" --log "$work/$name.log" --cache-dir "$work/c-$name" >/dev/null 2>"$work/$name.err" &
  srv=$!
  for _ in $(seq 50); do [ -S "$sock" ] && break; sleep 0.1; done
  VKBRIDGE_SOCKET=$sock VK_DRIVER_FILES=$out/vkbridge_icd.json VK_ICD_FILENAMES= "$out/vkbridge-smoke" >"$work/$name.smoke" 2>&1 &
  local cli=$! # the smoke itself (its pid is the one the line must name)
  ( sleep 60; kill $cli $srv ) 2>/dev/null &
  local dog=$!
  wait $cli 2>/dev/null
  kill $dog 2>/dev/null
  echo "$cli" >"$work/$name.pid"
  wait $srv 2>/dev/null
  echo $? >"$work/$name.status"
  srv=
}

run bind vkBindImageMemory
pid=$(cat "$work/bind.pid")
line=$(grep -m1 "crashed with signal" "$work/bind.log")
bind=$(grep -m1 "bind of image" "$work/bind.log")
echo "$line" | grep -q "in vkBindImageMemory from vkbridge-smoke (pid $pid), pc" &&
  echo "$bind" | grep -Eq "image 0x[0-9a-f]+: [0-9]+x[0-9]+x1 format [0-9]+, 1 mip\(s\) 1 layer\(s\), tiling [0-9]+, usage 0x[0-9a-f]+, flags 0x?[0-9a-f]*, external 0x?[0-9a-f]*, needs [1-9][0-9]* bytes" &&
  echo "$bind" | grep -Eq "memory 0x[0-9a-f]+: [1-9][0-9]* bytes, type [0-9]+, [a-z]" && echo "$bind" | grep -q "by pid $pid"
step $? "1 a client's image bind: client, image and memory named (status $(cat "$work/bind.status")): ${line#*E } | ${bind#*E   }"

run draw vkCmdDraw
pid=$(cat "$work/draw.pid")
line=$(grep -m1 "crashed with signal" "$work/draw.log")
state=$(grep -m1 "  state: " "$work/draw.log")
echo "$line" | grep -q "in vkCmdDraw from vkbridge-smoke (pid $pid), pc" && grep -q "replaying command buffer" "$work/draw.log" &&
  echo "$state" | grep -Eq "graphics pipeline 0x[0-9a-f]+ \(made [0-9]+ ms ago by pid $pid; topology [0-9]+, polygon mode [0-9]+, line width"
step $? "2 a replayed draw: client and bound pipeline named: ${line#*E } | ${state#*E   }"

grep -q "  other calls in flight: " "$work/bind.log" && grep -q "  other calls in flight: " "$work/draw.log"
step $? "3 the account ends with the calls in flight ($(grep -m1 "in flight: " "$work/bind.log" | sed 's/.*in flight: //'))"

VKBRIDGE_TEST_CRASH_ACCERR=1 run alloc vkAllocateMemory
pid=$(cat "$work/alloc.pid")
line=$(grep -m1 "crashed with signal" "$work/alloc.log")
alloc=$(grep -m1 "  allocation of " "$work/alloc.log")
echo "$line" | grep -q "in vkAllocateMemory from vkbridge-smoke (pid $pid), pc" &&
  echo "$alloc" | grep -Eq "allocation of [1-9][0-9]* bytes in memory type [0-9]+, (plain|shared|exportable|fd import|export emulation)"
step $? "4 a client's allocation: client, size, type and path named: ${line#*E } | ${alloc#*E   }"

where=$(grep -m1 '  thread [0-9]* "' "$work/bind.log")
acc=$(grep -m1 '  thread [0-9]* "' "$work/alloc.log")
echo "$where" | grep -Eq 'thread [0-9]+ "[^"]*"; address 0x40 is in no mapping \(before [0-9a-f]+-[0-9a-f]+ r' &&
  grep -q "crashed with signal 11 (code 2, address 0x[0-9a-f]*e5e)" "$work/alloc.log" &&
  echo "$acc" | grep -Eq 'address 0x[0-9a-f]+e5e is in [0-9a-f]+-[0-9a-f]+ ---p \(anonymous\)'
step $? "5 the faulting thread and address placed: ${where#*E   } | ${acc#*E   }"

if [ $fails = 0 ]; then echo "OK: $steps steps"; else echo "FAILED: $fails of $steps steps"; [ -n "$KEEP" ] || { echo "--- bind.log tail"; tail -5 "$work/bind.log"; echo "--- draw.log tail"; tail -6 "$work/draw.log"; }; fi
[ $fails = 0 ]
