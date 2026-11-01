#!/bin/bash
# The per-hitch line (VKBRIDGE_HITCH_MS; server/server_hitch.c, client/hitch.c) and the DXVK config
# check (client/dxvkcfg.c), each run with a server of its own (socket, cache directory):
#   1  VKBRIDGE_HITCH_MS=150 in <cache dir>/env, frames made slow on purpose (tests/hitch_check.c):
#      a slow compile, a slow BC conversion, a slow 128 MB allocation, a held GPU queue and a CPU
#      sleep each get one hitch line naming compile, bc, alloc, wait and other; no other frame gets
#      one; the compile and the allocation have timing lines; the process's summary counts them;
#   2  VKBRIDGE_HITCH_MS from the server's environment, a held GPU queue with a BC upload in it:
#      the wait is the conversion's (cause bc);
#   3  off (nothing set): no hitch line, nothing turned on;
#   4  VKBRIDGE_HITCH_MS in the guest only: the client turns the server's side on (cause other);
#   5  DXVK config: DXVK_CONFIG's options, and a DXVK log of this run that has one of them in its
#      "Effective configuration": one said applied, the other not there; with DXVK_LOG_LEVEL=none
#      it says it cannot confirm; a process that ends before the last look gets it at its exit;
#   6  DXVK's log written only after the first present, an older run's log beside it: not there yet
#      (the old one skipped, saying why), then applied at a later look.
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

# 2 a held queue converting a texture: bc, from the server's environment
rm -f "$work/c/env"
start gatebc VKBRIDGE_HITCH_MS=150
run gatebc -- gatebc
stop
f=$(frame_of gatebc gatebc); c=$(cause_of "$slog" "$f")
[ $res = 0 ] && [ "$c" = bc ] && grep "hitch: frame=$f " "$slog" | grep -qE "bc_recent_mtex=0\.[1-9]" &&
  grep "hitch: frame=$f " "$slog" | grep -qE "wait_ms=(3[5-9][0-9]|4[0-9][0-9])"
step $? "2 server environment HITCH_MS=150: a held queue converting a BC texture -> ${c:-none} ($(grep -o "hitch: frame=$f .*wait_max=[^ ]*" "$slog" | grep -o "wait_ms=[0-9]*\|bc_recent_mtex=[0-9.]*\|wait_max=[^ ]*" | tr '\n' ' '))"
[ "$c" = bc ] || dump gatebc

# 3 off
start off
run off -- sleep
stop
[ $res = 0 ] && ! grep -q "hitch:\|hitch attribution\|hitches over" "$slog" && ! grep -q "hitch attribution" "$work/off.client.log"
step $? "3 off: no hitch line, nothing turned on ($(last off))"

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

if [ $fails = 0 ]; then echo "OK: hitch lines name compile, bc, alloc, wait and other; HITCH_MS from <cache dir>/env, the server and the guest; DXVK config checked"
else echo "FAILED: $fails step(s)"; exit 1; fi
