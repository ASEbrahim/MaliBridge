#!/bin/bash
# The client's async queue lock under a DXVK-like load (tests/asynclock_check: dxvk-cs queuing nonstop, dxvk-queue's synchronous
# calls writing the queue out, dxvk-submit's submits, the flusher). RE3 / Tomb Raider / BL2 logs: dxvk-cs waited 1.1-1.4 s of every
# 10 s on this lock, while its holder wrote the queue to the socket (the flusher, a synchronous caller) and could block there.
# Arms: the old way (VKBRIDGE_AQ_LOCKED_WRITES=1: every socket write under the lock) and the default, each with the server stopped
# for 20 ms every 150 ms (a server thread behind: writes block) and without.
#   1  every arm: the program's checks pass (the event set last reads set: order kept), dxvk-cs has its async lines
#   2  server stopped now and then: dxvk-cs's lock wait is at most a fifth of the old way's
#   3  no stops: dxvk-cs's lock wait is at most a third of the old way's, or under 20 ms in all
#   4  dxvk-cs's calls a second not below 90 % of the old way's (no throughput lost), both shapes
#   5  the flusher's own line ("async flusher ... thread vkb-flush") in the new-way arms; no request lost (server log)
#   6  v0.5.5.4: nothing set runs the old way (VKBRIDGE_AQ_LOCKED_WRITES defaults to on here; the arms 'stall'/'def' ask for =0)
# One line per step, "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
icd=${VKB_CLIENT_ICD:-$out/vkbridge_icd.json}
work=$(mktemp -d "${XDG_RUNTIME_DIR:-/tmp}/vkb-al.XXXX")
sock=$work/s.sock
srv=
fails=0
stop() { [ -n "$srv" ] && kill -CONT $srv 2>/dev/null; [ -n "$srv" ] && kill $srv 2>/dev/null && wait $srv 2>/dev/null; srv=; }
trap 'stop; [ -n "${VKB_KEEP:-}" ] && echo "kept $work" || rm -rf "$work"' EXIT
mkdir -p "$work/c"
declare -A LOCK FLUSH CPS RES LINES FL
run() {
  local name=$1 stalls=$2; shift 2
  rm -f "$sock"
  env -u VKBRIDGE_FAKE_MISSING -u VKBRIDGE_HIDE_EXTS -u VKBRIDGE_MODE VKBRIDGE_PROBE_BUDGET_S=0 \
      VK_DRIVER_FILES=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json} VK_ICD_FILENAMES= \
      "$out/vkbridge-server" --socket "$sock" --log "$work/$name.server.log" --cache-dir "$work/c" >/dev/null 2>&1 &
  srv=$!
  for _ in $(seq 300); do [ -S "$sock" ] && break; sleep 0.05; done
  local hold=
  if [ "$stalls" = 1 ]; then
    ( sleep 0.3; for _ in $(seq 30); do kill -STOP $srv 2>/dev/null; sleep 0.02; kill -CONT $srv 2>/dev/null; sleep 0.13; done ) &
    hold=$!
  fi
  env "$@" VKBRIDGE_SOCKET="$sock" VK_DRIVER_FILES="$icd" VK_ICD_FILENAMES= VKBRIDGE_WAIT_STATS=1 VKBRIDGE_ASYNC_TIMER=1 \
      VKBRIDGE_CLIENT_LOG="$work/$name.client.log" VKBRIDGE_LOG_LEVEL=2 timeout 120 "$out/asynclock_check" ${runms:-4000} >"$work/$name.out" 2>&1
  RES[$name]=$?
  [ -n "$hold" ] && wait $hold 2>/dev/null
  stop
  local l="async sends over .*thread dxvk-cs"
  LINES[$name]=$(grep -c "$l" "$work/$name.client.log" 2>/dev/null)
  LOCK[$name]=$(grep "$l" "$work/$name.client.log" 2>/dev/null | grep -o "lock wait [0-9.]* ms" | awk '{s+=$3} END {printf "%.1f", s+0}')
  FLUSH[$name]=$(grep "$l" "$work/$name.client.log" 2>/dev/null | grep -o "flush [0-9.]* ms" | awk '{s+=$2} END {printf "%.1f", s+0}')
  CPS[$name]=$(grep -o "calls_per_s=[0-9]*" "$work/$name.out" | cut -d= -f2)
  FL[$name]=$(grep -c "async flusher over .*thread vkb-flush" "$work/$name.client.log" 2>/dev/null)
  grep -q "never arrived" "$work/$name.server.log" && RES[$name]=9
}
step() { if [ "$1" = 0 ]; then echo "ok   $2"; else echo "FAIL $2"; fails=$((fails+1)); fi; }

run oldstall 1 VKBRIDGE_AQ_LOCKED_WRITES=1
run stall 1 VKBRIDGE_AQ_LOCKED_WRITES=0
run old 0 VKBRIDGE_AQ_LOCKED_WRITES=1
run def 0 VKBRIDGE_AQ_LOCKED_WRITES=0
run dflt 0
st=0
for a in oldstall stall old def; do
  { [ "${RES[$a]}" = 0 ] && [ "${LINES[$a]:-0}" -gt 0 ]; } || st=1
done
step $st "1 every arm passes and has dxvk-cs lines (exit oldstall ${RES[oldstall]} stall ${RES[stall]} old ${RES[old]} def ${RES[def]})"
[ $st = 0 ] || for a in oldstall stall old def; do tail -2 "$work/$a.out"; done

awk -v n="${LOCK[stall]:-1e9}" -v o="${LOCK[oldstall]:-0}" 'BEGIN {exit !(n <= o / 5)}'; st=$?
step $st "2 server stopped 20 ms every 150 ms: dxvk-cs lock wait ${LOCK[stall]} ms (old way ${LOCK[oldstall]} ms; at most a fifth); its own flushes ${FLUSH[stall]} ms (old ${FLUSH[oldstall]})"

awk -v n="${LOCK[def]:-1e9}" -v o="${LOCK[old]:-0}" 'BEGIN {exit !(n <= o / 3 || n < 20)}'; st=$?
step $st "3 no stops: dxvk-cs lock wait ${LOCK[def]} ms (old way ${LOCK[old]} ms; a third, or < 20 ms); its own flushes ${FLUSH[def]} ms (old ${FLUSH[old]})"

awk -v a="${CPS[stall]:-0}" -v b="${CPS[oldstall]:-1}" -v c="${CPS[def]:-0}" -v d="${CPS[old]:-1}" 'BEGIN {exit !(a >= 0.9 * b && c >= 0.9 * d)}'; st=$?
step $st "4 dxvk-cs calls/s: stops ${CPS[stall]} (old ${CPS[oldstall]}), no stops ${CPS[def]} (old ${CPS[old]}); not below 90 %"

{ [ "${FL[stall]:-0}" -gt 0 ] && [ "${FL[def]:-0}" -gt 0 ]; }; st=$?
step $st "5 the flusher's own lines: ${FL[stall]} and ${FL[def]}; no request lost"
[ $st = 0 ] || grep -h "flusher" "$work/def.client.log" | tail -2

# 6 v0.5.5.4: with nothing set the old way runs (the locked writes are the default in this build): its lock wait is the old way's
awk -v n="${LOCK[dflt]:-0}" -v o="${LOCK[old]:-1e9}" 'BEGIN {exit !(n >= o / 2)}'; st=$?
step $st "6 nothing set (default = locked writes in v0.5.5.4): dxvk-cs lock wait ${LOCK[dflt]} ms (old way ${LOCK[old]} ms; at least half of it)"

if [ $fails = 0 ]; then echo "OK: queue writes outside the lock: dxvk-cs no longer waits behind a write, order kept, same throughput"
else echo "FAILED: $fails step(s)"; exit 1; fi
