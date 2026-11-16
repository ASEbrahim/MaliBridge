#!/bin/bash
# The crash line (server_main.c on_crash, server_crashctx.c) names whose call died and what it was
# given. a player log: the one server died in Mali's vkBindImageMemory and the line said only that;
# binds are asynchronous, so nothing tied the death to a program, an image or a memory. a player log:
# a near-null read in the driver's vkCmdDrawIndexed with no word on the bound pipeline.
# VKBRIDGE_TEST_CRASH_CMD makes a client's call of that name fault at the driver boundary.
#   1  a bind (vkbridge-smoke's image): the line names vkbridge-smoke and its pid, the image (extent,
#      format, tiling, usage, the size it needs) and the memory (size, type, how it was made);
#   2  a draw inside a command buffer's replay: the line names the client, the replayed commands and
#      the bound graphics pipeline with its topology and line width;
#   3  the other connection threads' calls in flight are listed ("none" when there are none);
#   4  an allocation (a player log: Mali's vkAllocateMemory with two dma-buf allocations in flight): the
#      line names the client, the size, the memory type and how the memory is made;
#   5  the faulting thread is named and the faulting address placed: the mapping it is in, or the
#      mappings around it (a player log: SEGV_ACCERR far from every pointer the bridge saw);
#   6  every frame as "crash-frame #NN pc 0x<offset> <module> (BuildId: <id>)" (server_crashsym.c):
#      the module is vkbridge-server with the build id readelf reads from the binary, frame #00 symbolizes (addr2line)
#      to test_fault_at and a later one to run_request;
#   7  a call through a NULL entry point (VKBRIDGE_TEST_CRASH_NULL=1, a player log's pc 0x0): frame #00 is "pc 0x0 (no
#      module)", frame #01 is the return address and symbolizes to the function that made the call (test_fault_at).
#   8  six threads fault at the same moment (VKBRIDGE_TEST_CRASH_THREADS=6, as a device loss's driver threads do, a player log):
#      the log has no NUL byte, every crash-frame line is whole, and each thread's frames run #00, #01, ... unbroken
#      (one static buffer shared by every crashing thread garbled them: lost_test step 3 read the log as binary).
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
  env VKBRIDGE_NO_CRASH_GUARD=1 VKBRIDGE_TEST_CRASH_ACCERR=${VKBRIDGE_TEST_CRASH_ACCERR:-} VKBRIDGE_TEST_CRASH_NULL=${VKBRIDGE_TEST_CRASH_NULL:-} VKBRIDGE_PROBE=0 VKBRIDGE_TEST_CRASH_CMD=$cmd VK_DRIVER_FILES=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json} VK_ICD_FILENAMES= \
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

# 6 symbolizable frames (the bind's crash)
bid=$(readelf -n "$out/vkbridge-server" 2>/dev/null | sed -n 's/.*Build ID: *//p' | head -1)
sym() { addr2line -f -e "$out/vkbridge-server" "$1" 2>/dev/null | head -1; } # sym <offset>: the function
f0=$(grep -m1 "crash-frame #00 " "$work/bind.log")
o0=$(echo "$f0" | grep -o 'pc 0x[0-9a-f]*' | cut -d' ' -f2)
nfr=$(grep -c "crash-frame #[0-9][0-9] pc 0x[0-9a-f]\{16\} vkbridge-server (BuildId: $bid) via=unwind" "$work/bind.log")
rr=$(grep "crash-frame #" "$work/bind.log" | grep -o 'pc 0x[0-9a-f]*' | cut -d' ' -f2 | while read -r a; do sym $(printf '0x%x' $((a - 1))); done | grep -c '^run_request$')
[ -n "$bid" ] && [ "$nfr" -ge 3 ] && sym "$o0" | grep -Eq '^test_fault(_at)?$' && [ "$rr" -ge 1 ]
step $? "6 symbolizable frames: $nfr with build id ${bid:-?}; #00 $(sym "$o0") (${f0#*E }); run_request in $rr frame(s)"

# 7 a call through a NULL entry point
VKBRIDGE_TEST_CRASH_NULL=1 run nullcall vkBindImageMemory
g0=$(grep -m1 "crash-frame #00 " "$work/nullcall.log")
g1=$(grep -m1 "crash-frame #01 " "$work/nullcall.log")
o1=$(echo "$g1" | grep -o 'pc 0x[0-9a-f]*' | cut -d' ' -f2)
echo "$g0" | grep -q "crash-frame #00 pc 0x0000000000000000 (no module) via=pc" && echo "$g1" | grep -q " vkbridge-server (BuildId: $bid) via=lr" &&
  sym $(printf '0x%x' $((o1 - 1))) | grep -Eq '^test_fault(_at)?$'
step $? "7 NULL call: ${g0#*E } | ${g1#*E } -> $(sym $(printf '0x%x' $((${o1:-1} - 1))))"

# 8 several threads crash together
VKBRIDGE_TEST_CRASH_THREADS=6 run multi vkBindImageMemory
nul=$(tr -cd '\000' < "$work/multi.log" | wc -c)
bad=$(grep -a 'crash-frame' "$work/multi.log" | grep -avcE '^(vkbridge-server E crash-frame #[0-9]{2} pc 0x[0-9a-f]{16} (\(no module\)|[^ ]+ \(BuildId: [0-9a-fnoe]+\)) via=(unwind|pc|lr|fp)|vkbridge-server E crash-frame: thread [0-9]+ also crashed \(frames skipped: thread [0-9]+ writes its own\))$')
# every frame block numbered 00, 01, ... in order (a restart at #00 begins the next block)
seq=$(grep -ao 'crash-frame #[0-9][0-9]' "$work/multi.log" | awk '{ n = $2; sub("#", "", n); n += 0; if (n != 0 && n != prev + 1) b++; prev = n } END { print b + 0 }')
nfr=$(grep -ac 'crash-frame #00 ' "$work/multi.log")
[ "$nul" = 0 ] && [ "$bad" = 0 ] && [ "$seq" = 0 ] && [ "$nfr" -ge 1 ]
step $? "8 six threads crash together: NUL bytes $nul, broken crash-frame lines $bad, out-of-order frames $seq, frame blocks $nfr, skipped notes $(grep -ac 'frames skipped' "$work/multi.log"), crash lines $(grep -ac 'crashed with signal' "$work/multi.log")"

if [ $fails = 0 ]; then echo "OK: $steps steps"; else echo "FAILED: $fails of $steps steps"; [ -n "$KEEP" ] || { echo "--- bind.log tail"; tail -5 "$work/bind.log"; echo "--- draw.log tail"; tail -6 "$work/draw.log"; }; fi
[ $fails = 0 ]
