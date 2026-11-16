#!/bin/bash
# Steam's performance overlay (mangoapp) draws at most VKBRIDGE_HUD_FPS frames a second (server_syncfd.c hud_pace).
# One program (tests/present_path_check order: presents as fast as the fake gamescope takes them, every frame
# cleared to its number), run under two names; the client sends /proc/self/comm, so a copy named "mangoapp" is the
# overlay to the server. One server per step, the fake nested compositor of present_path_test.sh (FIFO every 4 ms):
#   1  named "hudother": 36 frames at the fake's pace (well under 2 s), no 'hud:' line: other programs untouched;
#   2  named "mangoapp", VKBRIDGE_HUD_FPS=12 + VKBRIDGE_ASYNC_ACQUIRE=1 (v0.5.5.4: both off by default): >= 35/12 s and the server says
#      'hud: process N is Steam's performance overlay (mangoapp)'; every frame still shown once, in order, as drawn;
#   3  named "mangoapp", VKBRIDGE_HUD_FPS=0: at the fake's pace again (the switch turns it off);
#   4  named "mangoapp", VKBRIDGE_HUD_FPS=30 (+ async acquire): >= 35/30 s and < 35/12 s (the number is used);
#   5  named "mangoapp", nothing set: at the fake's pace, no 'hud:' line (v0.5.5.4 default off).
# On a server without the cap (old code) step 2 fails: the overlay runs at the fake's pace.
# VKB_CLIENT_ICD=<icd json> as in present_path_test.sh. Prints one line per step and "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
icd=${VKB_CLIENT_ICD:-$out/vkbridge_icd.json}
work=$(mktemp -d "${XDG_RUNTIME_DIR:-/tmp}/vkb-hud.XXXX")
sock=$work/s.sock
srv=
fake=
fails=0
stop() { [ -n "$srv" ] && kill $srv 2>/dev/null && wait $srv 2>/dev/null; srv=; }
stop_fake() { [ -n "$fake" ] && kill $fake 2>/dev/null && wait $fake 2>/dev/null; fake=; }
trap 'stop; stop_fake; [ -n "${VKB_KEEP:-}" ] && echo "kept $work" || rm -rf "$work"' EXIT
if ! command -v python3 >/dev/null; then echo "SKIPPED: no python3"; exit 0; fi
start() { # start <name> [env...]
  stop
  local name=$1; shift
  rm -f "$sock"
  mkdir -p "$work/c"
  env -u VKBRIDGE_HUD_FPS -u VKBRIDGE_FAKE_MISSING -u VKBRIDGE_ASYNC_ACQUIRE -u VKBRIDGE_HITCH_MS -u VKBRIDGE_MODE \
      VKBRIDGE_PROBE_BUDGET_S=0 VKBRIDGE_TEST_SCANOUT=memfd "$@" \
      VK_DRIVER_FILES=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json} VK_ICD_FILENAMES= \
      "$out/vkbridge-server" --socket "$sock" --log "$work/$name.server.log" --cache-dir "$work/c" >/dev/null 2>&1 &
  srv=$!
  for _ in $(seq 300); do [ -S "$sock" ] && break; kill -0 $srv 2>/dev/null || break; sleep 0.05; done
}
fake_start() { # fake_start <log>
  stop_fake
  local log=$1
  : > "$log"
  rm -f "$work/nested.sock" "$work/host.sock" "$work/fake.out"
  python3 -I ../tests/fake_nested_wayland.py --nested "$work/nested.sock" --host "$work/host.sock" --log "$log" \
    --vblank-ms 4 --release-ms 1 --pixels > "$work/fake.out" 2>&1 &
  fake=$!
  for _ in $(seq 400); do grep -q ready "$work/fake.out" 2>/dev/null && break; kill -0 $fake 2>/dev/null || break; sleep 0.05; done
  grep -q ready "$work/fake.out" 2>/dev/null || { echo "FAIL the fake compositor did not start: $(tail -2 "$work/fake.out")"; fails=$((fails+1)); }
}
N=36
run() { # run <step> <program name>: $res, $secs (the program's whole run)
  local name=$1 prog=$2
  cp "$out/present_path_check" "$work/$prog"
  local t0=$(date +%s%N)
  timeout 120 env -u VKBRIDGE_ASYNC_ACQUIRE -u VKBRIDGE_FRAME_QUEUE -u DISPLAY \
    WAYLAND_DISPLAY="$work/nested.sock" VKBRIDGE_HOST_WAYLAND="$work/host.sock" VKBRIDGE_SOCKET="$sock" \
    VK_DRIVER_FILES="$icd" VK_ICD_FILENAMES= VKBRIDGE_CLIENT_LOG="$work/$name.client.log" VKBRIDGE_LOG_LEVEL=2 \
    "$work/$prog" order --frames $N >"$work/$name.out" 2>&1
  res=$?
  secs=$(awk -v a=$t0 -v b=$(date +%s%N) 'BEGIN{printf "%.2f", (b-a)/1e9}')
  sleep 0.2
}
pixels() { # pixels <log> <frames>
  python3 -I - "$1" "$2" <<'PY'
import json, sys
ev = [json.loads(l) for l in open(sys.argv[1]) if l.strip()]
n = int(sys.argv[2])
latch = [e for e in ev if e.get("ev") == "latch" and e.get("who") == "nested"]
if any(e.get("px") is None for e in latch):
    print("pixels not readable"); sys.exit(1)
def frame(px):
    return ((px >> 16) & 0xff) | (((px >> 8) & 0xff) << 8) if (px & 0xff) == 0x5a else -1
shown = [frame(e["px"]) for e in latch]
ok = shown == list(range(n))
print("%d frames shown in order" % len(shown) if ok else "frames shown %s... (expected 0..%d once each)" % (shown[:12], n - 1))
sys.exit(0 if ok else 1)
PY
}
step() { if [ "$1" = 0 ]; then echo "ok   $2"; else echo "FAIL $2"; fails=$((fails+1)); fi; }
hudline() { grep -c "hud: process [0-9]* is Steam's performance overlay (mangoapp)" "$work/$1.server.log"; }
ge() { awk -v a="$1" -v b="$2" 'BEGIN{exit !(a+0 >= b+0)}'; }

# 1 another program: untouched
start other; fake_start "$work/f1.jsonl"; run other hudother
if [ $res = 3 ]; then echo "SKIPPED: $(tail -1 "$work/other.out")"; exit 0; fi
p=$(pixels "$work/f1.jsonl" $N); ps=$?
[ $res = 0 ] && [ $ps = 0 ] && ! ge $secs 2.0 && [ "$(hudline other)" = 0 ]
step $? "1 'hudother': $N frames in $secs s, $p, hud lines $(hudline other)"
base=$secs

# 2 mangoapp, cap 12 asked for by name (v0.5.5.4: the cap is off by default and rides on the async acquire, also off)
start mango VKBRIDGE_HUD_FPS=12 VKBRIDGE_ASYNC_ACQUIRE=1; fake_start "$work/f2.jsonl"; run mango mangoapp
p=$(pixels "$work/f2.jsonl" $N); ps=$?
[ $res = 0 ] && [ $ps = 0 ] && ge $secs $(awk -v n=$N 'BEGIN{print (n-1)/12}') && [ "$(hudline mango)" = 1 ]
step $? "2 'mangoapp' VKBRIDGE_HUD_FPS=12 VKBRIDGE_ASYNC_ACQUIRE=1: $N frames in $secs s (>= $(awk -v n=$N 'BEGIN{printf "%.2f",(n-1)/12}') s at 12 a second), $p, hud lines $(hudline mango)"

# 3 VKBRIDGE_HUD_FPS=0
start mango0 VKBRIDGE_HUD_FPS=0; fake_start "$work/f3.jsonl"; run mango0 mangoapp
p=$(pixels "$work/f3.jsonl" $N); ps=$?
[ $res = 0 ] && [ $ps = 0 ] && ! ge $secs 2.0 && [ "$(hudline mango0)" = 0 ]
step $? "3 'mangoapp' VKBRIDGE_HUD_FPS=0: $N frames in $secs s, $p, hud lines $(hudline mango0)"

# 4 VKBRIDGE_HUD_FPS=30
start mango30 VKBRIDGE_HUD_FPS=30 VKBRIDGE_ASYNC_ACQUIRE=1; fake_start "$work/f4.jsonl"; run mango30 mangoapp
p=$(pixels "$work/f4.jsonl" $N); ps=$?
[ $res = 0 ] && [ $ps = 0 ] && ge $secs $(awk -v n=$N 'BEGIN{print (n-1)/30}') && ! ge $secs $(awk -v n=$N 'BEGIN{print (n-1)/12}') && [ "$(hudline mango30)" = 1 ]
step $? "4 'mangoapp' VKBRIDGE_HUD_FPS=30: $N frames in $secs s (between $(awk -v n=$N 'BEGIN{printf "%.2f",(n-1)/30}') and $(awk -v n=$N 'BEGIN{printf "%.2f",(n-1)/12}') s), $p"

# 5 v0.5.5.4: nothing asked for, the overlay runs at the fake's pace (the cap is off by default) and no 'hud:' line
start mangod; fake_start "$work/f5.jsonl"; run mangod mangoapp
p=$(pixels "$work/f5.jsonl" $N); ps=$?
[ $res = 0 ] && [ $ps = 0 ] && ! ge $secs 2.0 && [ "$(hudline mangod)" = 0 ]
step $? "5 'mangoapp', nothing set (default off in v0.5.5.4): $N frames in $secs s, $p, hud lines $(hudline mangod)"

if [ $fails = 0 ]; then echo "OK: the overlay is capped, other programs and the off switch untouched"; exit 0; fi
echo "FAILED: $fails step(s)"; exit 1
