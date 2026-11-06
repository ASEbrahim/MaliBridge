#!/bin/bash
# Many small BC textures streamed in (tests/bcupload_check.c, GTA V's way through DXVK 3.1.1: 400
# textures of 128x128 / 128x64 with every level a frame, 5 frames, one copy per level back to
# back), each run with a server of its own that converts BC (VKBRIDGE_FAKE_MISSING=bc), once with
# back-to-back uploads batched (the default, server_emu_bcn.c bc_batch) and once with
# VKBRIDGE_BC_BATCH=0 (a barrier pair per upload, as before):
#   1  dxvk3: every texel read back the same in both (hash), within bcdec's decode where decoded;
#      the same dispatches; batched: at most a twentieth of the barriers, fewer copy commands and
#      restores of the app's compute state;
#   2  mixed: dxvk3 with RGBA8 uploads between the BC ones and a fill every 7th BC texture (a
#      command that flushes the batch): the same texels; fewer barriers;
#   3  pertex: a barrier before and after every texture (nothing to batch): the same texels and the
#      same counts in both.
# The counts come from the server's "BC uploads so far" line. Prints one line per step and
# "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
icd=${VKB_CLIENT_ICD:-$out/vkbridge_icd.json}
work=$(mktemp -d)
sock=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-bcupload-$$.sock
srv=
fails=0
stop() { [ -n "$srv" ] && kill $srv 2>/dev/null && wait $srv 2>/dev/null; srv=; }
trap 'stop; rm -rf "$work" "$sock"' EXIT
# run <name> [server env...] -- [bcupload_check args]: $res, $slog, $o
run() {
  local name=$1; shift
  local e=()
  while [ "$1" != -- ]; do e+=("$1"); shift; done
  shift
  stop
  rm -f "$sock"
  rm -rf "$work/c"; mkdir -p "$work/c"
  slog=$work/$name.server.log; o=$work/$name.out
  env -u VKBRIDGE_FAKE_MISSING -u VKBRIDGE_HIDE_EXTS -u VKBRIDGE_ERRATA -u VKBRIDGE_MODE -u VKBRIDGE_BC_TX_BUDGET -u VKBRIDGE_BC_BATCH \
      -u VKBRIDGE_BC1_ASTC -u VKBRIDGE_BC_TRANSCODE_MAX -u VKBRIDGE_BC_DECODE -u VKBRIDGE_TEST_HITCH_DELAY_MS -u VKBRIDGE_TEST_BC_SLOW \
      -u WAYLAND_DISPLAY -u DISPLAY \
      VKBRIDGE_PROBE_BUDGET_S=0 VKBRIDGE_FAKE_MISSING=bc "${e[@]}" \
      VK_DRIVER_FILES=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json} VK_ICD_FILENAMES= \
      "$out/vkbridge-server" --socket "$sock" --log "$slog" --cache-dir "$work/c" >/dev/null 2>&1 &
  srv=$!
  for _ in $(seq 300); do [ -S "$sock" ] && break; kill -0 $srv 2>/dev/null || break; sleep 0.05; done
  timeout 600 env -u WAYLAND_DISPLAY -u DISPLAY VKBRIDGE_SOCKET="$sock" VK_DRIVER_FILES="$icd" VK_ICD_FILENAMES= \
    VKBRIDGE_CLIENT_LOG="$work/$name.client.log" "$out/bcupload_check" "$@" >"$o" 2>&1
  res=$?
  for _ in $(seq 200); do grep -q "BC uploads so far" "$slog" && break; sleep 0.05; done
  stop
  [ -n "${VKB_KEEP:-}" ] && cp "$slog" "$o" "$VKB_KEEP/" 2>/dev/null
}
step() { if [ "$1" = 0 ]; then echo "ok   $2"; else echo "FAIL $2"; fails=$((fails+1)); dump; fi; }
val() { grep -o "$1=[0-9a-z.]*" "$o" | head -1 | cut -d= -f2; }
# cnt <what>: from the server's last "BC uploads so far" line (copies, barriers, dispatches, commands, restores)
cnt() {
  local l
  l=$(grep -o "BC uploads so far: .*" "$slog" | tail -1)
  case $1 in
    copies) echo "$l" | grep -o "so far: [0-9]* copies" | grep -o "[0-9]*";;
    barriers) echo "$l" | grep -o "[0-9]* barriers" | grep -o "[0-9]*";;
    dispatches) echo "$l" | grep -o "[0-9]* dispatches" | grep -o "[0-9]*";;
    commands) echo "$l" | grep -o "[0-9]* copy commands" | grep -o "^[0-9]*";;
    restores) echo "$l" | grep -o "[0-9]* restores" | grep -o "[0-9]*";;
  esac
}
counts() { echo "$(cnt copies) copies: $(cnt barriers) barriers, $(cnt dispatches) dispatches, $(cnt commands) copy commands, $(cnt restores) restores"; }
dump() { tail -3 "$o" | sed 's/^/     /'; grep -h "BC uploads\|BC emulation" "$slog" | tail -4 | sed 's/^/     /'; }
decoded() { grep -q "BC emulation: no transcoding" "$slog"; }
# exact: within bcdec's decode where the server decodes (BC1 UNORM through R5G6B5: 5)
exact() { ! decoded || { [ "$(val maxdiff_bc1)" -le 5 ] && [ "$(val maxdiff_bc3)" -le 1 ]; }; }

for p in dxvk3 mixed pertex; do
  run "$p-old" VKBRIDGE_BC_BATCH=0 -- --pattern "$p" ${VKB_BCUP_ARGS:-}
  r0=$res; h0=$(val hash); ms0=$(val steady_ms); w0=$(val worst_frame_ms); ex0=$(exact; echo $?)
  b0=$(cnt barriers); d0=$(cnt dispatches); c0=$(cnt commands); s0=$(cnt restores); n0=$(cnt copies); old="$(counts)"
  grep -q "a barrier pair per upload (VKBRIDGE_BC_BATCH=0)" "$slog"; sw0=$?
  run "$p-new" -- --pattern "$p" ${VKB_BCUP_ARGS:-}
  r1=$res; h1=$(val hash); ms1=$(val steady_ms); w1=$(val worst_frame_ms); ex1=$(exact; echo $?)
  b1=$(cnt barriers); d1=$(cnt dispatches); c1=$(cnt commands); s1=$(cnt restores); n1=$(cnt copies); new="$(counts)"
  grep -q "uploads recorded back to back share one barrier pair" "$slog"; sw1=$?
  base=0
  [ $r0 = 0 ] && [ $r1 = 0 ] && [ -n "$h0" ] && [ "$h0" = "$h1" ] && [ "$ex0" = 0 ] && [ "$ex1" = 0 ] && [ $sw0 = 0 ] && [ $sw1 = 0 ] &&
    [ -n "$b0" ] && [ -n "$b1" ] && [ "$d0" = "$d1" ] && [ "$n0" = "$n1" ] || base=1
  case $p in
    dxvk3) [ $base = 0 ] && [ $((b1 * 20)) -le "$b0" ] && [ "$c1" -lt "$c0" ] && [ "$s1" -lt "$s0" ]; st=$?; num=1;;
    mixed) [ $base = 0 ] && [ "$b1" -lt "$b0" ]; st=$?; num=2;;
    pertex) [ $base = 0 ] && [ "$b0" = "$b1" ] && [ "$c0" = "$c1" ] && [ "$s0" = "$s1" ]; st=$?; num=3;;
  esac
  step $st "$num $p: texels ${h1:-?} (batch off: ${h0:-?}); off: $old, frames after the first ${ms0} ms (worst ${w0}); on: $new, frames after the first ${ms1} ms (worst ${w1})"
done

if [ $fails = 0 ]; then echo "OK: back-to-back BC uploads share their barriers; every texel read back the same as with a barrier pair per upload"
else echo "FAILED: $fails step(s)"; exit 1; fi
