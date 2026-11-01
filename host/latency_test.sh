#!/bin/bash
# The latency log (VKBRIDGE_LATLOG; client/latency.c, server/server_latency.c, tools/latency_report.py),
# each step with a server of its own (socket, cache directory, session folder = its log's directory).
# present_late_check holds its first frame on the GPU for 300 ms, then presents 30 frames 16 ms apart:
#   1  off (nothing set): no vkbridge-latency.log, the client says nothing about it;
#   2  VKBRIDGE_LATLOG=1 in <cache dir>/env, headless surface (the app's present call shows the frame):
#      the session folder's vkbridge-latency.log has one F line for each of the 31 presents, numbered
#      1-31 once each, every stage seen and in order (queued <= server <= gpu_done <= committed); the
#      held frame's queued -> gpu_done is 300 ms or more, the others' under 100 ms;
#   3  VKBRIDGE_LATLOG=1 in the server's environment, a Wayland surface (the presenting thread; skipped
#      without a compositor in WAYLAND_DISPLAY): 31 lines, each marked async and feedback asked, each
#      answered (shown with a time, discarded, or no answer: the surface has no role);
#   4  VKBRIDGE_LATLOG=1 in the guest's environment only: the lines still come (the server writes them);
#   5  VKBRIDGE_LATLOG=0 in the guest beats <cache dir>/env's 1: no lines;
#   6  VKBRIDGE_LATLOG=<path> in the server's environment: the lines go to that file;
#   7  tools/latency_report.py on step 2's log: the stages with 31 frames, the held frame as the
#      server -> GPU done maximum.
# Prints one line per step and "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
icd=${VKB_CLIENT_ICD:-$out/vkbridge_icd.json}
work=$(mktemp -d)
sock=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-lat-$$.sock
srv=
fails=0
stop() { [ -n "$srv" ] && kill $srv 2>/dev/null && wait $srv 2>/dev/null; srv=; }
trap 'stop; [ -n "${VKB_KEEP:-}" ] && echo "kept $work" || rm -rf "$work"; rm -f "$sock"' EXIT
# start <name> [env...]: a server with cache $work/c ($work/c/env as written before), session folder $work/<name>
start() {
  local name=$1; shift
  stop
  rm -f "$sock"
  mkdir -p "$work/c" "$work/$name"
  sdir=$work/$name
  env -u VKBRIDGE_FAKE_MISSING -u VKBRIDGE_HIDE_EXTS -u VKBRIDGE_ERRATA -u VKBRIDGE_ERRATA_IMITATE -u VKBRIDGE_MODE \
      -u VKBRIDGE_FAKE_DRIVER_VERSION -u VKBRIDGE_ERRATA_PROBE -u VKBRIDGE_LATLOG -u VKBRIDGE_HITCH_MS -u VKBRIDGE_HANG_DETECT \
      -u VKBRIDGE_REPORT_DIR VKBRIDGE_PROBE_BUDGET_S=0 "$@" \
      VK_DRIVER_FILES=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json} VK_ICD_FILENAMES= \
      "$out/vkbridge-server" --socket "$sock" --log "$sdir/server.log" --cache-dir "$work/c" >/dev/null 2>&1 &
  srv=$!
  for _ in $(seq 300); do [ -S "$sock" ] && break; kill -0 $srv 2>/dev/null || break; sleep 0.05; done
}
# run <name> [env...] -- <present_late_check args>: $res its exit status, $clog the client log
run() {
  local name=$1; shift
  local e=()
  while [ "$1" != -- ]; do e+=("$1"); shift; done
  shift
  clog=$work/$name.client.log
  : > "$clog"
  timeout 120 env -u VKBRIDGE_LATLOG -u VKBRIDGE_PRESENT_TIMEOUT_S "${e[@]}" \
    VKBRIDGE_SOCKET="$sock" VK_DRIVER_FILES="$icd" VK_ICD_FILENAMES= VKBRIDGE_CLIENT_LOG="$clog" VKBRIDGE_LOG_LEVEL=2 \
    "$out/present_late_check" ok --delay-ms 300 --frames 30 --sleep-ms 16 "$@" >"$work/$name.out" 2>&1
  res=$?
  # the server done with the client (its last lines written) before it is looked at
  for _ in $(seq 100); do grep -q "client process [0-9]* gone" "$sdir/server.log" && break; sleep 0.05; done
}
step() { # step <status> <text>
  if [ "$1" = 0 ]; then echo "ok   $2"; else echo "FAIL $2"; fails=$((fails+1)); fi
}
last() { grep -E '^(OK|FAILED|SKIPPED)' "$work/$1.out" | tail -1; }
flines() { grep -c '^F ' "$1" 2>/dev/null || echo 0; }
# checks <log>: every frame 1-31 once, all four client/server stages seen and in order; prints a reason when not
checks() {
  awk 'BEGIN { bad = "" }
       /^F / { n[$4]++; lines++
               if (!$5 || !$6 || !$7 || !$8) bad = bad " frame " $4 " misses a stage;"
               if ($6 < $5 || $7 < $6 || $8 < $7) bad = bad " frame " $4 " out of order;"
               ms = ($7 - $5) / 1e6
               if ($4 == 1 && ms < 300) bad = bad " the held frame drew in " ms " ms;"
               if ($4 > 1 && ms > 100) bad = bad " frame " $4 " drew in " ms " ms;" }
       END { for (i = 1; i <= 31; i++) if (n[i] != 1) bad = bad " frame " i " seen " n[i] + 0 " times;"
             if (lines != 31) bad = bad " " lines " lines;"
             if (bad != "") { print bad; exit 1 } }' "$1"
}

# 1 off
start off
run off --
[ $res = 0 ] && [ ! -e "$sdir/vkbridge-latency.log" ] && ! grep -q 'latency log' "$clog" "$sdir/server.log"
step $? "1 off: no latency log, nothing said ($(last off))"

# 2 <cache dir>/env, headless
echo "VKBRIDGE_LATLOG=1" > "$work/c/env"
start envfile
run envfile --
lat2=$sdir/vkbridge-latency.log
why=$(checks "$lat2"); r=$?
awk '/^F / && $10 != 0 { f = 1 } END { exit f }' "$lat2"; sync_flags=$? # the app's call: no flag
[ $res = 0 ] && [ $r = 0 ] && [ $sync_flags = 0 ] && head -1 "$lat2" | grep -q '^# vkbridge latency log 1:' &&
  grep -q 'latency log: one line a frame in' "$sdir/server.log"
step $? "2 <cache dir>/env, headless: $(flines "$lat2") lines, frames 1-31 once, stages in order, the held frame $(awk '$1 == "F" && $4 == 1 { printf "%.0f", ($7 - $5) / 1e6 }' "$lat2") ms to draw${why:+; $why} ($(last envfile))"
[ $res = 0 ] && [ $r = 0 ] || { sed 's/^/     /' "$work/envfile.out" | tail -5; head -5 "$lat2" 2>/dev/null | sed 's/^/     /'; }
rm -f "$work/c/env"

# 3 server environment, Wayland
start wayland VKBRIDGE_LATLOG=1
run wayland -- --wayland
if grep -q '^SKIPPED' "$work/wayland.out"; then
  echo "ok   3 Wayland: skipped (no compositor in WAYLAND_DISPLAY)"
else
  lat3=$sdir/vkbridge-latency.log
  why=$(checks "$lat3"); r=$?
  awk 'function bit(x, b) { return int(x / b) % 2 }
       /^F / { if (!bit($10, 1) || !bit($10, 2)) a = 1; if (!$9 && !bit($10, 4) && !bit($10, 8) && !bit($10, 32)) a = 1 } END { exit a }' "$lat3"
  r3=$?
  [ $res = 0 ] && [ $r = 0 ] && [ $r3 = 0 ]
  step $? "3 server environment, Wayland: $(flines "$lat3") lines, async + feedback asked on each, each answered (shown $(awk '/^F / && $9 { s++ } END { print s + 0 }' "$lat3"), discarded $(awk '/^F / && int($10 / 4) % 2 { s++ } END { print s + 0 }' "$lat3"), no answer $(awk '/^F / && int($10 / 8) % 2 { s++ } END { print s + 0 }' "$lat3"))${why:+; $why} ($(last wayland))"
fi

# 4 the guest's environment only
start guest
run guest VKBRIDGE_LATLOG=1 --
why=$(checks "$sdir/vkbridge-latency.log"); r=$?
[ $res = 0 ] && [ $r = 0 ]
step $? "4 guest environment only: $(flines "$sdir/vkbridge-latency.log") lines${why:+; $why}"

# 5 the guest's 0 beats <cache dir>/env
echo "VKBRIDGE_LATLOG=1" > "$work/c/env"
start guestoff
run guestoff VKBRIDGE_LATLOG=0 --
[ $res = 0 ] && [ "$(flines "$sdir/vkbridge-latency.log")" = 0 ]
step $? "5 VKBRIDGE_LATLOG=0 in the guest, 1 in <cache dir>/env: $(flines "$sdir/vkbridge-latency.log") lines"
rm -f "$work/c/env"

# 6 a path of its own
start path VKBRIDGE_LATLOG="$work/own-latency.log"
run path --
why=$(checks "$work/own-latency.log"); r=$?
[ $res = 0 ] && [ $r = 0 ] && [ ! -e "$sdir/vkbridge-latency.log" ]
step $? "6 VKBRIDGE_LATLOG=<path>: $(flines "$work/own-latency.log") lines there, none in the session folder${why:+; $why}"
stop

# 7 the report
python3 ../tools/latency_report.py --json "$work/r.json" "$lat2" > "$work/report.txt" 2>&1
python3 - "$work/r.json" <<'EOF'
import json, sys
r = json.load(open(sys.argv[1]))
b = [v for k, v in r.items() if k.startswith('bridge:present_late')]
assert b and b[0]['frames'] == 31, b
st = {s['stage']: s for s in b[0]['stages']}
for name in ('app present -> server', 'server -> GPU done', 'GPU done -> committed'):
    assert st[name]['n'] == 31, (name, st[name])
assert st['server -> GPU done']['max_ms'] >= 290, st['server -> GPU done']
assert st['server -> GPU done']['median_ms'] < 50, st['server -> GPU done']
EOF
step $? "7 latency_report.py: $(grep -E '^  (stage|server -> GPU done) ' "$work/report.txt" | sed 's/  */ /g' | paste -sd ';')"
[ -s "$work/report.txt" ] && grep -q 'server -> GPU done' "$work/report.txt" || sed 's/^/     /' "$work/report.txt" | tail -8

if [ $fails = 0 ]; then echo "OK: the latency log from <cache dir>/env, the server and the guest, one line a frame with every stage; the report reads it"
else echo "FAILED: $fails step(s)"; exit 1; fi
