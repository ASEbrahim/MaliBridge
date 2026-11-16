#!/bin/bash
# vkCreateImage / vkCreateBuffer ahead of the process's queued work, and the image's requirements in the create's reply
# (VKBRIDGE_FAST_CREATE, client/memory.c + server_conn.c). A server of its own per step, BC imitated missing (decoded:
# the host's lavapipe samples no ETC2/ASTC, so nothing is transcoded here), every replayed vkCmdFillBuffer held 400 ms
# (VKBRIDGE_TEST_DELAY_CMD/_MS/_REPEAT): tests/fastcreate_check ends such a command buffer on one thread, then creates on
# another ("T <what> <ms>"):
#   v0.5.5.4: fast create is OFF by default in this build; steps 1, 3-7 ask for it on the server (VKBRIDGE_FAST_CREATE=1), 2b checks
#   that nothing set waits as before.
#   1  VKBRIDGE_FAST_CREATE=1: an RGBA8 image, a BC1 image and a buffer are made at once (< 150 ms) while the 400 ms replay runs; an
#      image with VkExternalMemoryImageCreateInfo waits for it (>= 300 ms: the safe subset); 6 rounds of images made while
#      the last round's destroys wait behind held work, bound, filled and read back right; the server's "fast create" line
#      counts creates that ran ahead and ones that waited; no refusals or protocol errors in the logs;
#   2  VKBRIDGE_FAST_CREATE=0 (the server's): everything waits (>= 300 ms), as before; the same rounds right;
#   3  VKBRIDGE_BC_TX_BUDGET=100: the BC1 image waits (the budget's bytes come back with queued destroys), RGBA8 does not;
#   4  VKBRIDGE_BC_SMALL_DECODE=1: the same;
#   5  the guest's VKBRIDGE_FAST_CREATE=0 beats the server's on: everything waits;
#   7  every create sent to run ahead (VKBRIDGE_TEST_FAST_CREATE_ALL=1) to a server with VKBRIDGE_BC_TX_BUDGET=100: the server
#      itself makes the BC1 and the external-memory image wait, RGBA8 and the buffer run ahead (its own check, not the client's);
#   6  requirements (no hold): every REQ line of "fastcreate_check reqs" the same with the switch on (answered from the create's
#      reply) as off (asked of the server), the dedicated hint imitated as on Mali (VKBRIDGE_TEST_PREFER_DEDICATED=1); the
#      client's round trips: vkGetImageMemoryRequirements2 >= 150 off, under 100 on (not listed); with the client's
#      VKBRIDGE_TEST_FAST_CREATE_VERIFY=1 every reply's answer is compared with the server's own asked as before: 0 differ.
# One line per step, "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
icd=${VKB_CLIENT_ICD:-$out/vkbridge_icd.json}
host_icd=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/lvp_icd.json}
work=$(mktemp -d "${XDG_RUNTIME_DIR:-/tmp}/vkb-fcr.XXXX")
sock=$work/s.sock
srv=
stop() { [ -n "$srv" ] && kill $srv 2>/dev/null && wait $srv 2>/dev/null; srv=; rm -f "$sock"; }
trap 'stop; [ -n "${VKB_KEEP:-}" ] && echo "kept $work" || rm -rf "$work"' EXIT
# start <name> [VAR=value ...]: a server of its own, log $work/<name>/server.log
start() {
  local d=$work/$1
  shift
  stop
  mkdir -p "$d/c"
  slog=$d/server.log
  env -u VKBRIDGE_HIDE_EXTS -u VKBRIDGE_ERRATA -u VKBRIDGE_MODE -u VKBRIDGE_FAST_CREATE -u VKBRIDGE_BC_TX_BUDGET -u VKBRIDGE_BC_SMALL_DECODE \
      VKBRIDGE_FAKE_MISSING=bc VKBRIDGE_PROBE_BUDGET_S=0 "$@" VK_DRIVER_FILES=$host_icd VK_ICD_FILENAMES= \
      "$out/vkbridge-server" --socket "$sock" --log "$slog" --cache-dir "$d/c" >/dev/null 2>&1 &
  srv=$!
  for _ in $(seq 300); do [ -S "$sock" ] && break; kill -0 $srv 2>/dev/null || break; sleep 0.05; done
}
HOLD="VKBRIDGE_TEST_DELAY_CMD=vkCmdFillBuffer VKBRIDGE_TEST_DELAY_MS=400 VKBRIDGE_TEST_DELAY_REPEAT=1"
# check <name> <mode> [client VAR=value ...]: fastcreate_check's output in $work/<name>.log
check() {
  local n=$1 m=$2
  shift 2
  env -u VKBRIDGE_FAST_CREATE -u VKBRIDGE_TEST_FAST_CREATE_ALL VKBRIDGE_SOCKET=$sock VK_DRIVER_FILES=$icd VK_ICD_FILENAMES= "$@" timeout 180 "$out/fastcreate_check" "$m" \
    >"$work/$n.log" 2>&1
}
ms() { awk -v w="$2" '$1 == "T" && $2 == w {print int($3)}' "$work/$1.log"; }
fast() { local v; v=$(ms "$1" "$2"); [ -n "$v" ] && [ "$v" -lt 150 ]; }
slow() { local v; v=$(ms "$1" "$2"); [ -n "$v" ] && [ "$v" -ge 300 ]; }
clean() { ! grep -qE "refused the call|malformed|unknown object|bad batch|internal: |never arrived" "$slog" "$work/$1.log"; }
times() { echo "rgba8 $(ms "$1" rgba8) bc1 $(ms "$1" bc1) external $(ms "$1" external) buffer $(ms "$1" buffer) ms; ordering rounds' slowest create $(ms "$1" order-worst) ms"; }
fails=0 steps=0
step() { steps=$((steps+1)); if [ "$1" = 0 ]; then echo "  ok   $2"; else echo "  FAIL $2"; fails=$((fails+1)); fi; }

start on $HOLD VKBRIDGE_FAST_CREATE=1
check on timing
grep -E "^(PASS|FAIL)" "$work/on.log"
line=$(grep -o "fast create on, so far: .*" "$slog" | tail -1)
grep -q "^OK" "$work/on.log" && fast on rgba8 && fast on bc1 && fast on buffer && slow on external && clean on &&
  echo "$line" | grep -qE "image creates: [1-9][0-9]* ran ahead of queued work, [1-9][0-9]* sent to wait for it" &&
  echo "$line" | grep -qE "buffer creates, [1-9][0-9]* ran ahead"
step $? "1 on: $(times on); server: ${line:-no fast create line}"

start off $HOLD VKBRIDGE_FAST_CREATE=0
check off timing
grep -q "^OK" "$work/off.log" && slow off rgba8 && slow off bc1 && slow off buffer && slow off external && clean off &&
  ! grep -qE "[1-9][0-9]* ran ahead" "$slog" && grep -q "fast create: off (VKBRIDGE_FAST_CREATE=0)" "$slog"
step $? "2 VKBRIDGE_FAST_CREATE=0: $(times off)"

# 2b v0.5.5.4: nothing set = off (the server's switch defaults to off in this build): everything waits, nothing ran ahead
start dflt $HOLD
check dflt timing
grep -q "^OK" "$work/dflt.log" && slow dflt rgba8 && slow dflt bc1 && slow dflt buffer && slow dflt external && clean dflt &&
  ! grep -qE "[1-9][0-9]* ran ahead" "$slog"
step $? "2b nothing set (default off in v0.5.5.4): $(times dflt)"

start budget $HOLD VKBRIDGE_FAST_CREATE=1 VKBRIDGE_BC_TX_BUDGET=100
check budget timing
grep -q "^OK" "$work/budget.log" && fast budget rgba8 && slow budget bc1 && clean budget
step $? "3 VKBRIDGE_BC_TX_BUDGET=100: $(times budget); server: $(grep -o "fast create on, so far: .*" "$slog" | tail -1)"

start small $HOLD VKBRIDGE_FAST_CREATE=1 VKBRIDGE_BC_SMALL_DECODE=1
check small timing
grep -q "^OK" "$work/small.log" && fast small rgba8 && slow small bc1 && clean small
step $? "4 VKBRIDGE_BC_SMALL_DECODE=1: $(times small)"

start guest $HOLD VKBRIDGE_FAST_CREATE=1
check guest timing VKBRIDGE_FAST_CREATE=0
grep -q "^OK" "$work/guest.log" && slow guest rgba8 && slow guest bc1 && slow guest buffer && clean guest
step $? "5 the guest's VKBRIDGE_FAST_CREATE=0: $(times guest)"

start guard $HOLD VKBRIDGE_FAST_CREATE=1 VKBRIDGE_BC_TX_BUDGET=100
check guard timing VKBRIDGE_TEST_FAST_CREATE_ALL=1
gline=$(grep -o "fast create on, so far: .*" "$slog" | tail -1)
grep -q "^OK" "$work/guard.log" && fast guard rgba8 && slow guard bc1 && slow guard external && fast guard buffer && clean guard &&
  echo "$gline" | grep -qE "[1-9][0-9]* the server made wait \([1-9][0-9]* not of that kind, [1-9][0-9]* BC under a live-byte rule\)"
step $? "7 every create sent to run ahead (VKBRIDGE_TEST_FAST_CREATE_ALL=1), VKBRIDGE_BC_TX_BUDGET=100: the server's own check: $(times guard); server: ${gline:-none}"

DED="VKBRIDGE_DEDICATED_HINT=1 VKBRIDGE_TEST_PREFER_DEDICATED=1 VKBRIDGE_STATS=3600"
start reqon VKBRIDGE_FAST_CREATE=1
check reqon reqs $DED
check reqver reqs VKBRIDGE_TEST_FAST_CREATE_VERIFY=1
start reqoff VKBRIDGE_FAST_CREATE=0
check reqoff reqs $DED
rt() { grep -E "\]: +vkGetImageMemoryRequirements2 +[0-9]+ sync" "$work/$1.log" | sed -E 's/.*Requirements2 +([0-9]+) sync.*/\1/' | head -1; }
nreq=$(grep -c "^REQ " "$work/reqon.log")
on_rt=$(rt reqon) off_rt=$(rt reqoff)
grep -q "^OK" "$work/reqon.log" && grep -q "^OK" "$work/reqoff.log" && [ "$nreq" -ge 15 ] &&
  diff <(grep "^REQ " "$work/reqon.log") <(grep "^REQ " "$work/reqoff.log") >"$work/req.diff" && [ -z "$on_rt" ] && [ "${off_rt:-0}" -ge 150 ] &&
  grep -q "^OK" "$work/reqver.log" && grep -qE "fast create verify: (1[0-9][0-9]|[2-9][0-9][0-9]) answers from create replies compared with the server's, 0 differ" "$work/reqver.log" &&
  ! grep -q "fast create verify: image" "$work/reqver.log"
step $? "6 requirements: $nreq REQ lines $( [ -s "$work/req.diff" ] && echo DIFFER || echo identical) on vs off; vkGetImageMemoryRequirements2 round trips on ${on_rt:-<100} off ${off_rt:-<100}; VKBRIDGE_TEST_FAST_CREATE_VERIFY=1: $(grep -o "fast create verify: [0-9]* answers.*" "$work/reqver.log" | tail -1)"
[ -s "$work/req.diff" ] && head -20 "$work/req.diff"

[ $fails = 0 ] && echo "OK: fastcreate_test, $steps steps" || echo "FAILED: fastcreate_test, $fails of $steps step(s)"
exit $((fails != 0))
