#!/bin/bash
# tools/lagprobe.exe through Proton Experimental's wine and DXVK and the bridge, on a virtual X
# display (Xvfb) with eight mouse presses made by xdotool, the latency log on (a server of its own):
#   1  lagprobe runs its 600 frames at 60 fps and logs them, and a P line for each press and release;
#   2  the server's latency log has a line for each of its presents (process lagprobe.exe);
#   3  tools/latency_report.py lines lagprobe's frames up with the bridge's (frame offset found,
#      the clock exact through the C lines) and follows all eight presses from the game's sample to
#      the commit; the game's own sample -> Present() call is under 5 ms.
# Needs VKB_WINE_DIR prepared as for d3d11_test.sh, with lagprobe.exe in its app/ (built by
# tools/build_lagprobe.sh), and Xvfb and xdotool; skipped otherwise. LP_ARGS: more lagprobe arguments
# (--gpu 400 --gpu-size 4096 for a GPU-bound run). Prints one line per step and "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
P="$HOME/.steam/steam/steamapps/common/Proton - Experimental/files"
W=${VKB_WINE_DIR:-}
if [ -z "$W" ] || [ ! -f "$W/app/lagprobe.exe" ] || [ ! -x "$P/bin/wine" ] || ! command -v Xvfb >/dev/null || ! command -v xdotool >/dev/null; then
  echo "OK: skipped (needs VKB_WINE_DIR with app/lagprobe.exe, Proton Experimental, Xvfb and xdotool)"
  exit 0
fi
work=$(mktemp -d)
sock=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-lagprobe-$$.sock
srv= xv= presser=
fails=0
disp=:$((90 + $$ % 900))
cleanup() {
  [ -n "$presser" ] && kill $presser 2>/dev/null
  [ -n "$srv" ] && kill $srv 2>/dev/null && wait $srv 2>/dev/null
  [ -n "$xv" ] && kill $xv 2>/dev/null && wait $xv 2>/dev/null
  WINEPREFIX=$W/pfx "$P/bin/wineserver" -k 2>/dev/null
  rm -f "$sock"
  [ -n "${VKB_KEEP:-}" ] && echo "kept $work" || rm -rf "$work"
}
trap cleanup EXIT
step() { # step <status> <text>
  if [ "$1" = 0 ]; then echo "ok   $2"; else echo "FAIL $2"; fails=$((fails+1)); fi
}

Xvfb $disp -screen 0 1280x720x24 >/dev/null 2>&1 &
xv=$!
mkdir -p "$work/c" "$work/s"
env -u VKBRIDGE_FAKE_MISSING -u VKBRIDGE_HIDE_EXTS -u VKBRIDGE_ERRATA -u VKBRIDGE_MODE -u VKBRIDGE_REPORT_DIR \
    VKBRIDGE_LATLOG=1 VKBRIDGE_PROBE_BUDGET_S=0 VK_DRIVER_FILES=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json} VK_ICD_FILENAMES= \
    "$out/vkbridge-server" --socket "$sock" --log "$work/s/server.log" --cache-dir "$work/c" >/dev/null 2>&1 &
srv=$!
for _ in $(seq 300); do [ -S "$sock" ] && break; kill -0 $srv 2>/dev/null || break; sleep 0.05; done
# eight presses of 250 ms, 700 ms apart, once lagprobe is drawing
( sleep 5; for _ in 1 2 3 4 5 6 7 8; do DISPLAY=$disp xdotool mousemove 300 300 mousedown 1; sleep 0.25; DISPLAY=$disp xdotool mouseup 1; sleep 0.45; done ) &
presser=$!
lp=$work/lagprobe.log
(cd "$W/app" && DISPLAY=$disp WAYLAND_DISPLAY= WINEPREFIX=$W/pfx WINEDEBUG=-all WINEDLLOVERRIDES="d3d11,dxgi=n,b" DXVK_LOG_PATH=$work \
  VKBRIDGE_SOCKET="$sock" VK_DRIVER_FILES="$out/vkbridge_icd.json" VK_ICD_FILENAMES= VKBRIDGE_CLIENT_LOG="$work/client.log" \
  timeout 120 "$P/bin/wine" lagprobe.exe --windowed --cap 60 --frames 600 ${LP_ARGS:-} --log "Z:$lp" >"$work/wine.out" 2>&1)
res=$?
wait $presser 2>/dev/null
presser=
for _ in $(seq 100); do grep -q "client process [0-9]* gone" "$work/s/server.log" && break; sleep 0.05; done
kill $srv 2>/dev/null && wait $srv 2>/dev/null
srv=

nf=$(grep -c '^F ' "$lp" 2>/dev/null); np=$(grep -c '^P [0-9]* [0-9]* 1 ' "$lp" 2>/dev/null); nr=$(grep -c '^P [0-9]* [0-9]* 0 ' "$lp" 2>/dev/null)
[ $res = 0 ] && [ "${nf:-0}" = 600 ] && [ "${np:-0}" = 8 ] && [ "${nr:-0}" = 8 ]
step $? "1 lagprobe: exit $res, ${nf:-0} frames, ${np:-0} presses, ${nr:-0} releases"
[ $res = 0 ] || tail -5 "$work/wine.out" | sed 's/^/     /'

lat=$work/s/vkbridge-latency.log
nb=$(grep -c '^F [0-9]* lagprobe.exe ' "$lat" 2>/dev/null)
[ "${nb:-0}" -ge 595 ]
step $? "2 the server's latency log: ${nb:-0} lines for lagprobe.exe, $(grep -c '^C ' "$lat" 2>/dev/null) clock lines"

python3 ../tools/latency_report.py --json "$work/r.json" "$lat" "$lp" >"$work/report.txt" 2>&1
python3 - "$work/r.json" <<'EOF'
import json, sys
r = json.load(open(sys.argv[1]))
p = [v for k, v in r.items() if k.startswith('lagprobe:')]
assert p and p[0]['presses'] == 8 and p[0]['clock'].startswith('exact'), p
st = {s['stage']: s for s in p[0]['stages']}
for name in ('game sample -> Present() call', 'Present() call -> bridge app present (DXVK)', 'app present -> server',
             'server -> GPU done', 'GPU done -> committed'):
    assert st[name]['n'] == 8, (name, st.get(name))
assert st['game sample -> Present() call']['median_ms'] < 5, st['game sample -> Present() call']
EOF
step $? "3 latency_report.py: $(grep -o 'frame n = lagprobe frame [-+0-9]*, clock offset [-0-9.]* ms, [a-z]*' "$work/report.txt"); $(grep -E '^  (Present\(\) call -> bridge app present|GPU done -> committed)' "$work/report.txt" | sed 's/  */ /g' | paste -sd ';')"
[ -s "$work/r.json" ] || sed 's/^/     /' "$work/report.txt" | tail -8

if [ $fails = 0 ]; then echo "OK: lagprobe through DXVK and the bridge, its presses followed into the latency log"
else echo "FAILED: $fails step(s)"; exit 1; fi
