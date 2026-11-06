#!/bin/bash
# The per-hitch line (VKBRIDGE_HITCH_MS; server/server_hitch.c, client/hitch.c) and the DXVK config
# check (client/dxvkcfg.c), each run with a server of its own (socket, cache directory):
#   1  VKBRIDGE_HITCH_MS=150 in <cache dir>/env, frames made slow on purpose (tests/hitch_check.c):
#      a slow compile, a slow BC conversion, a slow 128 MB allocation, a held GPU queue and a CPU
#      sleep each get one hitch line naming compile, bc, alloc, wait and other; no other frame gets
#      one; the compile and the allocation have timing lines; the process's summary counts them;
#   2  VKBRIDGE_HITCH_MS from the server's environment, a GPU queue held 400 ms with one small BC
#      upload in it (256x256, as a Mali-G615 MC6): the conversion cannot take a fifth of the wait, so
#      the cause is wait (bc_recent_n=1, its estimate in bc_gpu_est_ms; the summary counts it as a
#      wait after conversions too small to explain it) - before 2026-11-05 any texel made it bc
#; 2b the same upload as 80 conversions' GPU work (VKBRIDGE_TEST_BC_SLOW=1,80, the
#      decoder's dispatch recorded 80 times): its estimate passes a fifth of the wait -> bc;
#   3  on by default (2026-11-04): nothing set -> 100 ms, the slow frame's line and a frames line;
#      VKBRIDGE_HITCH_MS=0 in <cache dir>/env: no hitch or frames line, nothing turned on; =0 in the
#      guest only: the client sends nothing (no hitch or frames line);
#   4  VKBRIDGE_HITCH_MS in the guest only: the client turns the server's side on (cause other);
#   5  DXVK config: DXVK_CONFIG's options, and a DXVK log of this run that has one of them in its
#      "Effective configuration": one said applied, the other not there; with DXVK_LOG_LEVEL=none
#      it says it cannot confirm; a process that ends before the last look gets it at its exit;
#   6  DXVK's log written only after the first present, an older run's log beside it: not there yet
#      (the old one skipped, saying why), then applied at a later look;
#   7  the frames lines and the wait lines, defaults (periods shortened: VKBRIDGE_TEST_HITCH_PERIODS):
#      the slow frames' window has a line of its own naming their causes, smooth windows are merged
#      (windows=N), a "so far" line while the process runs, a client "waits over 10.0s" line tagged
#      info at the default log level; VKBRIDGE_WAIT_STATS=0: no wait line;
#   8  hitch lines rate-limited: 24 slow frames in a row with a burst of 4 -> fewer lines, every one
#      counted (summary, hitch_lines_skipped in the frames lines).
#   9  the session suspended (SIGSTOP of the server and the game for 1.5 s, then SIGCONT): the frame that
#      spans it is a "suspended:" line, not a hitch; suspended_ms in the frames line and the summary;
# Prints one line per step and "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
icd=${VKB_CLIENT_ICD:-$out/vkbridge_icd.json}
work=$(mktemp -d)
sock=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-hitch-$$.sock
srv=
fails=0
stop() { [ -n "$srv" ] && kill $srv 2>/dev/null && wait $srv 2>/dev/null; srv=; }
trap 'stop; rm -rf "$work" "$sock"' EXIT
# start <name> [env...]: a server with cache $work/c ($work/c/env as written before), log $work/<name>.server.log
start() {
  local name=$1; shift
  stop
  rm -f "$sock"
  mkdir -p "$work/c"
  slog=$work/$name.server.log
  env -u VKBRIDGE_FAKE_MISSING -u VKBRIDGE_HIDE_EXTS -u VKBRIDGE_ERRATA -u VKBRIDGE_ERRATA_IMITATE -u VKBRIDGE_MODE \
      -u VKBRIDGE_FAKE_DRIVER_VERSION -u VKBRIDGE_ERRATA_PROBE -u VKBRIDGE_HITCH_MS -u VKBRIDGE_HANG_DETECT -u VKBRIDGE_TEST_HITCH_DELAY_MS \
      VKBRIDGE_PROBE_BUDGET_S=0 VKBRIDGE_FAKE_MISSING=bc VKBRIDGE_BC_TRANSCODE=0 "$@" \
      VK_DRIVER_FILES=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json} VK_ICD_FILENAMES= \
      "$out/vkbridge-server" --socket "$sock" --log "$slog" --cache-dir "$work/c" >/dev/null 2>&1 &
  srv=$!
  for _ in $(seq 300); do [ -S "$sock" ] && break; kill -0 $srv 2>/dev/null || break; sleep 0.05; done
}
# run <name> [env...] -- <hitch_check args>: $res its exit status, $clog the client log
run() {
  local name=$1; shift
  local e=()
  while [ "$1" != -- ]; do e+=("$1"); shift; done
  shift
  clog=$work/$name.client.log
  : > "$clog"
  timeout 120 env -u VKBRIDGE_HITCH_MS -u DXVK_CONFIG -u DXVK_CONFIG_FILE -u DXVK_LOG_LEVEL -u DXVK_LOG_PATH "${e[@]}" \
    VKBRIDGE_SOCKET="$sock" VK_DRIVER_FILES="$icd" VK_ICD_FILENAMES= VKBRIDGE_CLIENT_LOG="$clog" VKBRIDGE_LOG_LEVEL=2 \
    "$out/hitch_check" "$@" >"$work/$name.out" 2>&1
  res=$?
}
# the server done with the client (its summary line written) before it is stopped
gone() { for _ in $(seq 100); do grep -q "client process [0-9]* gone" "$slog" && return; sleep 0.05; done; }
step() { # step <status> <text>
  if [ "$1" = 0 ]; then echo "ok   $2"; else echo "FAIL $2"; fails=$((fails+1)); fi
}
last() { grep -E '^(OK|FAILED)' "$work/$1.out" | tail -1; }
frame_of() { grep -o "^action $2 frame [0-9]*" "$work/$1.out" | awk '{print $4}'; }
# cause_of <server log> <frame>: the cause= of that frame's hitch line
cause_of() { grep -o "hitch: frame=$2 ms=[0-9]* cause=[a-z]*" "$1" | sed 's/.*cause=//'; }
dump() { sed 's/^/     /' "$work/$1.out" | tail -8; grep -h "hitch\|compile:\|allocated in" "$work/$1.server.log" | sed 's/^/     /' | tail -12; }

# 1 every class, HITCH_MS from <cache dir>/env
mkdir -p "$work/c" && echo "VKBRIDGE_HITCH_MS=150" > "$work/c/env"
start classes VKBRIDGE_TEST_CRASH_PIPELINE=slow VKBRIDGE_TEST_SLOW_S=0.4 VKBRIDGE_TEST_HITCH_DELAY_MS=400
run classes -- compile bc alloc gate sleep
gone; stop
got=; ok=0
[ $res = 0 ] || ok=1
for a in compile:compile bc:bc alloc:alloc gate:wait sleep:other; do
  f=$(frame_of classes "${a%%:*}"); c=$(cause_of "$slog" "$f")
  got="$got ${a%%:*}->${c:-none}"
  [ -n "$f" ] && [ "$c" = "${a##*:}" ] || ok=1
done
n=$(grep -c "hitch: frame=" "$slog")
[ "$n" = 5 ] || ok=1
grep -q "hitch attribution on for process" "$slog" && grep -qE "compile: compute pipelines \(1\) in 4[0-9][0-9]\.[0-9] ms" "$slog" &&
  grep -qE "128 MB allocated in memory type [0-9]+ \(heap [0-9]+\) in 4[0-9][0-9]\.[0-9] ms" "$slog" &&
  grep -q "hitches over 150 ms in process [0-9]* (hitch_check): 5 (compile 1, bc 1, alloc 1, pcache 0, wait 1, other 1)" "$slog" || ok=1
step $ok "1 <cache dir>/env HITCH_MS=150: each slow frame named by its cause:$got; $n hitch lines ($(last classes))"
[ $ok = 0 ] || dump classes

# 2 a held queue with one small BC upload: wait (the conversion's GPU time cannot explain it)
rm -f "$work/c/env"
start gatebc VKBRIDGE_HITCH_MS=150 VKBRIDGE_TEST_BC_GPU_NAME=Mali-G615_MC6
run gatebc -- gatebc
gone; stop
f=$(frame_of gatebc gatebc); c=$(cause_of "$slog" "$f"); hl=$(grep "hitch: frame=$f " "$slog")
est=$(echo "$hl" | grep -o "bc_gpu_est_ms=[0-9]*" | cut -d= -f2)
[ $res = 0 ] && [ "$c" = wait ] && echo "$hl" | grep -qE "bc_recent_mtex=0\.[1-9]" && echo "$hl" | grep -q "bc_recent_n=1 " &&
  [ -n "$est" ] && [ "$est" -lt 20 ] && echo "$hl" | grep -qE "wait_ms=(3[5-9][0-9]|4[0-9][0-9])" &&
  grep -q "hitches over 150 ms in process [0-9]* (hitch_check): .*wait 1, .*; 1 of the waits came after BC conversions too small to explain them" "$slog"
step $? "2 server environment HITCH_MS=150: a held queue with one small BC upload -> ${c:-none} ($(echo "$hl" | grep -o "wait_ms=[0-9]*\|bc_recent_mtex=[0-9.]*\|bc_recent_n=[0-9]*\|bc_gpu_est_ms=[0-9]*\|wait_max=[^ ]*" | tr '\n' ' '))"
[ "$c" = wait ] || dump gatebc

# 2b the same with the conversion's GPU work 80 times over: its estimate passes a fifth of the wait -> bc
start gatebc80 VKBRIDGE_HITCH_MS=150 VKBRIDGE_TEST_BC_GPU_NAME=Mali-G615_MC6 VKBRIDGE_TEST_BC_SLOW=1,80
run gatebc80 -- gatebc
gone; stop
f=$(frame_of gatebc80 gatebc); c=$(cause_of "$slog" "$f"); hl=$(grep "hitch: frame=$f " "$slog")
est=$(echo "$hl" | grep -o "bc_gpu_est_ms=[0-9]*" | cut -d= -f2)
w=$(echo "$hl" | grep -o " wait_ms=[0-9]*" | cut -d= -f2)
[ $res = 0 ] && [ "$c" = bc ] && [ -n "$est" ] && [ -n "$w" ] && [ $((est * 5)) -ge "$w" ] &&
  ! grep -q "too small to explain them" "$slog"
step $? "2b the same upload as 80 conversions' GPU work -> ${c:-none} (bc_gpu_est_ms=${est:-?} wait_ms=${w:-?})"
[ "$c" = bc ] || dump gatebc80

# 3 on by default; off by <cache dir>/env or by the guest
start default
run default -- sleep
gone; stop
f=$(frame_of default sleep); c=$(cause_of "$slog" "$f")
[ $res = 0 ] && [ "$c" = other ] && grep -q "hitch attribution on for process [0-9]* (hitch_check): frames over 100 ms" "$slog"
st=$?
step $st "3a nothing set: on at 100 ms, sleep -> ${c:-none}"
[ $st = 0 ] || dump default
echo "VKBRIDGE_HITCH_MS=0" > "$work/c/env"
start off
run off -- sleep
gone; stop
[ $res = 0 ] && ! grep -q "hitch:\|hitch attribution\|hitches over\|frames: " "$slog" && ! grep -q "hitch attribution" "$work/off.client.log"
step $? "3b VKBRIDGE_HITCH_MS=0 in <cache dir>/env: no hitch or frames line, nothing turned on ($(last off))"
rm -f "$work/c/env"
start guestoff
run guestoff VKBRIDGE_HITCH_MS=0 -- sleep
gone; stop
[ $res = 0 ] && ! grep -q "hitch:\|hitches over\|frames: " "$slog" && ! grep -q "hitch attribution" "$work/guestoff.client.log"
step $? "3c VKBRIDGE_HITCH_MS=0 in the guest: the client sends nothing ($(last guestoff))"

# 4 the guest's environment alone
start guest
run guest VKBRIDGE_HITCH_MS=150 -- sleep
stop
f=$(frame_of guest sleep); c=$(cause_of "$slog" "$f")
[ $res = 0 ] && [ "$c" = other ] && grep -q "hitch attribution on for process" "$slog"
step $? "4 guest environment HITCH_MS=150 only: the server's side turned on by the client, sleep -> ${c:-none}"

# 5 DXVK config: what the process was given, and DXVK's own log
mkdir -p "$work/dxvklog"
printf 'info:  Game: hitch_check.exe\ninfo:  DXVK: v2.7.1\ninfo:  Found config env: dxgi.maxFrameLatency = 1; dxvk.fooBar = 2\ninfo:  Effective configuration:\ninfo:    dxgi.maxFrameLatency = 1\ninfo:    d3d11.cachedDynamicResources = a\ninfo:  Built-in extension providers:\n' \
  > "$work/dxvklog/hitch_check_dxgi.log"
start dxvk
run dxvk DXVK_CONFIG="dxgi.maxFrameLatency = 1; dxvk.fooBar = 2" DXVK_LOG_LEVEL=info DXVK_LOG_PATH="$work/dxvklog" VKBRIDGE_TEST_DXVKCFG_RETRY_MS=200,400 -- --engine DXVK --ms 300 sleep sleep
r1=$res; l1=$clog
run dxvkexit DXVK_CONFIG="dxgi.maxFrameLatency = 1; dxvk.fooBar = 2" DXVK_LOG_LEVEL=info DXVK_LOG_PATH="$work/dxvklog" -- --engine DXVK sleep
r2=$res; l2=$clog
run dxvknone DXVK_CONFIG="dxgi.maxFrameLatency = 1" DXVK_LOG_LEVEL=none -- --engine DXVK sleep
stop
[ $r1 = 0 ] && [ $r2 = 0 ] && [ $res = 0 ] &&
  grep -q "DXVK config: not every option in DXVK's log in $work/dxvklog yet; checking again 0.2 and 0.4 s after the first present" "$l1" &&
  grep -q "DXVK config: applied (in DXVK's effective configuration): dxgi.maxFrameLatency=1; not there: dxvk.fooBar=2" "$l2" &&
  grep -q "DXVK config: DXVK's log hitch_check_dxgi.log: .*at the process's exit)" "$l2" &&
  grep -q 'DXVK config: engine DXVK 2.7.1; DXVK_CONFIG "dxgi.maxFrameLatency = 1; dxvk.fooBar = 2" (dxgi.maxFrameLatency=1, dxvk.fooBar=2); dxvk.conf .* not there' "$l1" &&
  grep -q "DXVK config: DXVK's log hitch_check_dxgi.log: found DXVK_CONFIG;" "$l1" &&
  grep -q "DXVK config: applied (in DXVK's effective configuration): dxgi.maxFrameLatency=1; not there: dxvk.fooBar=2" "$l1" &&
  grep -q "DXVK config: not confirmed - DXVK's own log is at DXVK_LOG_LEVEL=none" "$clog"
st=$?
step $st "5 DXVK config: $(grep -o 'applied (in .*' "$l1" | head -1)"
[ $st = 0 ] || grep -h "DXVK config" "$l1" "$l2" "$clog" | sed 's/^/     /'

# 6 DXVK's log only after the first present (DXVK writes it as it goes), an older run's beside it
rm -rf "$work/dxvklog" && mkdir -p "$work/dxvklog"
echo 'info:  Effective configuration:' > "$work/dxvklog/hitch_check_d3d11.log"
touch -d '1 hour ago' "$work/dxvklog/hitch_check_d3d11.log"
start dxvklate
( sleep 0.3
  printf 'info:  Found config env: dxgi.maxFrameLatency = 1\ninfo:  Effective configuration:\ninfo:    dxgi.maxFrameLatency = 1\ninfo:  Built-in extension providers:\n' \
    > "$work/dxvklog/hitch_check_dxgi.log" ) &
writer=$!
run dxvklate DXVK_CONFIG="dxgi.maxFrameLatency = 1" DXVK_LOG_LEVEL=info DXVK_LOG_PATH="$work/dxvklog" VKBRIDGE_TEST_DXVKCFG_RETRY_MS=800,1600 -- --engine DXVK --ms 300 sleep sleep sleep sleep sleep sleep
wait $writer
stop
[ $res = 0 ] &&
  grep -q "DXVK config: no DXVK log of this run in $work/dxvklog yet; skipped hitch_check_d3d11.log (written [0-9]* s before this process started: another run's); checking again 0.8 and 1.6 s after the first present" "$clog" &&
  grep -qE "DXVK config: DXVK's log hitch_check_dxgi.log: found DXVK_CONFIG; \((0\.[89]|1\.[0-9])[0-9]* s after the first present\)" "$clog" &&
  grep -q "DXVK config: skipped hitch_check_d3d11.log (written" "$clog" &&
  grep -q "DXVK config: applied (in DXVK's effective configuration): dxgi.maxFrameLatency=1; not there: none" "$clog"
st=$?
step $st "6 DXVK log after the first present: $(grep -o 'applied (in .*' "$clog" | head -1), at $(grep -o 'found DXVK_CONFIG; ([0-9.]* s' "$clog" | grep -o '[0-9.]* s' | head -1) after the first present"
[ $st = 0 ] || grep -h "DXVK config" "$clog" | sed 's/^/     /'

# 7 frames lines (smooth windows merged per 3 s, "so far" every 2 s) and wait lines, defaults
start frames VKBRIDGE_TEST_HITCH_PERIODS=3,2
run frames VKBRIDGE_LOG_LEVEL=1 -- --ms 300 --steady 11000 sleep gate
gone; stop
own=$(grep -cE "frames: pid=[0-9]+ comm=hitch_check s=1\.[0-9] windows=1 .*over=2 \(100 ms\) hitches=wait:1,other:1" "$slog")
merged=$(grep -cE "frames: pid=[0-9]+ comm=hitch_check s=[0-9.]+ windows=([2-9]|[1-9][0-9]) .*over=0 \(100 ms\) hitches=none" "$slog")
total=$(grep -c "frames: " "$slog")
[ $res = 0 ] && [ "$own" = 1 ] && [ "$merged" -ge 2 ] && [ "$total" -le 8 ] &&
  grep -q "hitches over 100 ms in process [0-9]* (hitch_check) so far: 2 (compile 0, bc 0, alloc 0, pcache 0, wait 1, other 1); worst 3[0-9][0-9] ms" "$slog" &&
  grep -qE "vkbridge-client info \[hitch_check [0-9]+\]: waits over 10\.[0-9]s, thread hitch_check \([0-9]+\): [0-9.]+ ms in [0-9]+ round trips; vkWaitForFences" "$clog"
st=$?
step $st "7 frames lines: $own for the slow window, $merged merged smooth, $total in all over 11 s; so-far line $(grep -c 'so far:' "$slog"); wait line: $(grep -o 'waits over [0-9.]*s' "$clog" | head -1)"
[ $st = 0 ] || { grep -h "frames:\|so far" "$slog" | cut -c1-200 | sed 's/^/     /'; tail -3 "$clog" | sed 's/^/     /'; }
start nowait
run nowait VKBRIDGE_WAIT_STATS=0 -- --steady 11000 sleep
stop
[ $res = 0 ] && ! grep -q "waits over" "$clog"
step $? "7b VKBRIDGE_WAIT_STATS=0: no wait line"

# 8 the hitch lines' rate limit: 24 slow frames back to back, a burst of 4
start rate VKBRIDGE_TEST_HITCH_PERIODS=3,2,4
run rate -- --ms 120 sleep sleep sleep sleep sleep sleep sleep sleep sleep sleep sleep sleep sleep sleep sleep sleep sleep sleep sleep sleep sleep sleep sleep sleep
gone; stop
n=$(grep -c "hitch: frame=" "$slog")
inwin=$(grep -o "hitch_lines_skipped=[0-9]*" "$slog" | cut -d= -f2 | awk '{s+=$1} END {print s+0}')
skipped=$(grep -o "hitches over 100 ms in process [0-9]* (hitch_check): 24 (.*; [0-9]* hitch lines left out" "$slog" | grep -o "[0-9]* hitch lines left" | cut -d' ' -f1)
[ $res = 0 ] && [ "$n" -ge 4 ] && [ "$n" -le 12 ] && [ $((n + ${skipped:-99})) = 24 ] && [ "$inwin" -ge 1 ] && [ "$inwin" -le "${skipped:-0}" ]
step $? "8 rate limit: 24 hitches, $n lines, ${skipped:-?} left out (the summary; $inwin of them named in frames lines)"

# 9 a session suspend (the app SIGSTOPs the server and the game, then SIGCONTs them: GTA5.exe in a player log
#   had 2812 and 45016 ms "hitches" from it): the frame that spans it is reported as suspended, not a
#   hitch (no hitch line near the suspend's length), the frames line says suspended_ms, the summary
#   counts it apart. Real signals, nothing imitated.
start susp VKBRIDGE_HITCH_MS=500
clog=$work/susp.client.log; : > "$clog"
env -u VKBRIDGE_HITCH_MS VKBRIDGE_SOCKET="$sock" VK_DRIVER_FILES="$icd" VK_ICD_FILENAMES= VKBRIDGE_CLIENT_LOG="$clog" VKBRIDGE_LOG_LEVEL=2 \
  "$out/hitch_check" --ms 600 sleep >"$work/susp.out" 2>&1 &
cpid=$!
for _ in $(seq 200); do grep -q "hitch attribution:" "$clog" && break; sleep 0.05; done
sleep 0.15
kill -STOP $srv $cpid
sleep 1.5
kill -CONT $srv $cpid
wait $cpid; res=$?
gone; stop
worst=$(grep -o "hitch: frame=[0-9]* ms=[0-9]*" "$slog" | sed 's/.*ms=//' | sort -n | tail -1)
nsusp=$(grep -c "^.*suspended: frame=" "$slog")
[ $res = 0 ] && [ "${worst:-0}" -lt 1000 ] && [ "$nsusp" -ge 1 ] &&
  grep -qE "suspended: frame=[0-9]+ ms=1[5-9][0-9][0-9] suspended_ms=1[0-9]{3} net_ms=" "$slog" &&
  grep -qE "frames: .* suspended_ms=1[0-9]{3}" "$slog" &&
  grep -q "frames\? spanned a session suspend (1[0-9][0-9][0-9] ms stopped), not counted as hitches" "$slog"
step $? "9 session suspend (SIGSTOP 1.5 s): worst hitch ${worst:-none} ms, $nsusp suspended line(s), frames line with suspended_ms"
grep -h "hitch\|suspended" "$slog" | cut -c1-220 | sed 's/^/     /' | tail -6

if [ $fails = 0 ]; then echo "OK: hitch lines name compile, bc, alloc, wait and other; on by default (100 ms), off by 0; frames lines merged and rate-limited; HITCH_MS from <cache dir>/env, the server and the guest; DXVK config checked"
else echo "FAILED: $fails step(s)"; exit 1; fi
