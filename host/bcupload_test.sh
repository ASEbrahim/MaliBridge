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
#   4  the conversions measured: the "BC conversions, so far" line counts the replay's copies and texels exactly and
#      the "BC uploads" dispatches, with GPU time read back; VKBRIDGE_BC_GPU_TIME=0: nothing timed, the same texels;
#   5  one dispatch a run of regions (VKBRIDGE_BC_ONE_DISPATCH=1): the same texels in every pattern, with BC1 RGB among
#      them, at lowered run limits and with no table room; under a fifth of the dispatches.
#   6  small images decoded instead of encoded (VKBRIDGE_BC_SMALL_DECODE, server_emu_bcn.c small_take), on a server that
#      transcodes (lavapipe with ETC2/ASTC required): 128x128 textures with =0 go through the transcoders; by default only
#      through the decoders, read back within bcdec's decode, in less GPU time, counted in "BC small decode so far"; 512x512
#      ones (over the rule) the same texels with it on and off; a 1 MB cap (VKBRIDGE_BC_SMALL_DECODE_MB): some decoded, the
#      rest transcoded, the cap said once, the decoded ones' extra memory under it; --query (memory requirements asked
#      first): none decoded by the rule, none bigger than told; --placed (vkd3d without maintenance4: a temporary image sized
#      while the cap is full, the real one made after decoded ones were freed): none bigger than its temporary image.
#   7  (v0.5.5.4: replaced by a default-off check, see step 7 below; what follows describes 0.5.6) the new converter variants by default (one dispatch a run, palette; server_emu_bcn.c fast_*, server_probe.c bc_variants):
#      the startup probe checks them against the old ones; a program whose cache holds no converters (new) builds them in place
#      of the old ones (no old one built), and so does one whose cache holds them (from it); VKBRIDGE_BC_FAST_FRESH=0 builds the
#      old ones and the new ones in the background, used once built - the same texels, fewer dispatches, none built while an
#      upload waited; a variant the probe finds different (imitated) or
#      hanging (imitated) is not used, nor are they with the probe off; with the probe left to the background, the old
#      variants until it has passed them, then the new ones (the same texels).
# Steps 1-5 run with VKBRIDGE_BC_SMALL_DECODE=0, VKBRIDGE_BC_ONE_DISPATCH=0 and VKBRIDGE_BC_PALETTE=0 (they test the
# batches and the converters one at a time); step 7 with the defaults.
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
  [ -z "${KEEPC:-}" ] && rm -rf "$work/c"
  mkdir -p "$work/c"
  slog=$work/$name.server.log; o=$work/$name.out
  env -u VKBRIDGE_FAKE_MISSING -u VKBRIDGE_HIDE_EXTS -u VKBRIDGE_ERRATA -u VKBRIDGE_MODE -u VKBRIDGE_BC_TX_BUDGET -u VKBRIDGE_BC_BATCH \
      -u VKBRIDGE_BC1_ASTC -u VKBRIDGE_BC_GPU_TIME -u VKBRIDGE_BC_ONE_DISPATCH -u VKBRIDGE_TEST_BC_RUN_LIMITS -u VKBRIDGE_BC_TRANSCODE_MAX -u VKBRIDGE_BC_DECODE -u VKBRIDGE_TEST_HITCH_DELAY_MS -u VKBRIDGE_TEST_BC_SLOW \
      -u VKBRIDGE_BC_SMALL_DECODE -u VKBRIDGE_BC_SMALL_DECODE_MB -u VKBRIDGE_BC_TRANSCODE -u vk_require_etc2 -u vk_require_astc \
      -u VKBRIDGE_BC_PALETTE -u VKBRIDGE_PROBE -u VKBRIDGE_PROBE_BCV_FAKE -u VKBRIDGE_FAKE_DRIVER -u VKBRIDGE_BCV_PROBE -u VKBRIDGE_PROBE_TX_TIMEOUT_S -u VKBRIDGE_BC_FAST_FRESH -u WAYLAND_DISPLAY -u DISPLAY \
      VKBRIDGE_PROBE_BUDGET_S=0 VKBRIDGE_FAKE_MISSING=bc VKBRIDGE_BC_SMALL_DECODE=0 VKBRIDGE_BC_ONE_DISPATCH=0 VKBRIDGE_BC_PALETTE=0 "${e[@]}" \
      VK_DRIVER_FILES=${SRV_ICD:-${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json}} VK_ICD_FILENAMES= \
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
  eval "hash_$p=\$h1; disp_$p=\$d1"
  step $st "$num $p: texels ${h1:-?} (batch off: ${h0:-?}); off: $old, frames after the first ${ms0} ms (worst ${w0}); on: $new, frames after the first ${ms1} ms (worst ${w1})"
done

# 4  the conversions measured (server_emu_bcn.c cv_*): the "BC conversions, so far" line of the batched dxvk3 run counts the
#    replay's copies and texels exactly, the dispatches of the "BC uploads" line, and GPU time read from its timestamps; with
#    VKBRIDGE_BC_GPU_TIME=0 nothing is timed and every texel reads back the same
cv() { grep -o "BC conversions, so far: .*" "$1" | tail -1; }
cvn() { echo "$1" | grep -o "$2" | head -1 | grep -o "^[0-9.]*"; }
l=$(cv "$work/dxvk3-new.server.log"); o=$work/dxvk3-new.out; slog=$work/dxvk3-new.server.log
want_c=$(val bc_copies); want_t=$(awk -v t="$(val bc_texels)" 'BEGIN { printf "%.2f", t / 1e6 }'); hn=$(val hash)
c=$(cvn "$l" "[0-9]* copies"); t=$(cvn "$l" "[0-9.]* Mtex"); d=$(cvn "$l" "[0-9]* dispatches in"); g=$(cvn "$l" "GPU [0-9.]* ms" | head -1)
g=$(echo "$l" | grep -o "GPU [0-9.]* ms" | head -1 | grep -o "[0-9.]*"); rd=$(echo "$l" | grep -o "in [0-9]* batches read" | grep -o "[0-9]*")
tm=$(echo "$l" | grep -o "([0-9]* timed" | grep -o "[0-9]*")
[ -n "$l" ] && [ -n "$want_c" ] && [ "$c" = "$want_c" ] && [ "$c" = "$(cnt copies)" ] && [ "$t" = "$want_t" ] && [ "$d" = "$(cnt dispatches)" ] &&
  [ "${rd:-0}" -ge 1 ] && [ "${tm:-0}" -ge "${rd:-0}" ] && awk -v g="${g:-0}" 'BEGIN { exit !(g > 0) }' &&
  grep -q "BC conversions by converter, so far" "$slog" && grep -q "the conversions' GPU time measured" "$slog"
st=$?
f0="copies $c (replay $want_c), $t Mtex (replay $want_t), $d dispatches (BC uploads $(cnt dispatches)), GPU ${g:-?} ms in ${rd:-?} of ${tm:-?} timed batches"
run dxvk3-notime VKBRIDGE_BC_GPU_TIME=0 -- --pattern dxvk3 ${VKB_BCUP_ARGS:-}
l0=$(cv "$slog"); tm0=$(echo "$l0" | grep -o "([0-9]* timed" | grep -o "[0-9]*")
[ $st = 0 ] && [ $res = 0 ] && [ "$(val hash)" = "$hn" ] && [ "$tm0" = 0 ] && grep -q "GPU time not measured (VKBRIDGE_BC_GPU_TIME=0)" "$slog"
step $? "4 conversions measured: $f0; VKBRIDGE_BC_GPU_TIME=0: ${tm0:-?} timed, texels $(val hash)"

# 5  one dispatch a run of regions (VKBRIDGE_BC_ONE_DISPATCH=1, server_emu_bcn.c run_*): every texel the same as with a
#    dispatch a region, in each pattern, with BC1 RGB textures among them (other flags: a new run), at lowered run limits
#    (workgroups, source bytes: VKBRIDGE_TEST_BC_RUN_LIMITS) and with no table room (pieces alone through the same
#    pipelines); dxvk3/mixed/pertex: under a fifth of the dispatches
for p in dxvk3 mixed pertex; do
  run "$p-one" VKBRIDGE_BC_ONE_DISPATCH=1 -- --pattern "$p" ${VKB_BCUP_ARGS:-}
  eval "hw=\$hash_$p; dw=\$disp_$p"
  l=$(cv "$slog")
  n=$(echo "$l" | grep -o "VKBRIDGE_BC_ONE_DISPATCH): [0-9]* runs" | grep -o "[0-9]* runs" | grep -o "[0-9]*")
  [ $res = 0 ] && exact && [ -n "$hw" ] && [ "$(val hash)" = "$hw" ] && [ "${n:-0}" -gt 0 ] && [ $(($(cnt dispatches) * 5)) -le "${dw:-0}" ] &&
    grep -q "one dispatch a run of regions (VKBRIDGE_BC_ONE_DISPATCH" "$slog" && grep -q "(one dispatch a run) built" "$slog"
  step $? "5 $p, one dispatch a run: texels $(val hash) (a dispatch a region: $hw); $(cnt dispatches) dispatches (were $dw) in ${n:-?} runs; $(echo "$l" | grep -o 'a new one at .*' | cut -c1-200)"
done
run rgb-one-off -- --pattern dxvk3 --rgb ${VKB_BCUP_ARGS:-}
hr=$(val hash); r0=$res
run rgb-one VKBRIDGE_BC_ONE_DISPATCH=1 -- --pattern dxvk3 --rgb ${VKB_BCUP_ARGS:-}
l=$(cv "$slog"); fl=$(echo "$l" | grep -o "flags [0-9]*" | head -1 | grep -o "[0-9]*")
[ $r0 = 0 ] && [ $res = 0 ] && exact && [ -n "$hr" ] && [ "$(val hash)" = "$hr" ] && [ "${fl:-0}" -gt 0 ]
step $? "5 dxvk3 with BC1 RGB textures, one dispatch a run: texels $(val hash) (a dispatch a region: $hr); new runs at a flags change: ${fl:-?}"
run lim-one VKBRIDGE_BC_ONE_DISPATCH=1 VKBRIDGE_TEST_BC_RUN_LIMITS=20,12000 -- --pattern dxvk3 ${VKB_BCUP_ARGS:-}
l=$(cv "$slog"); wg=$(echo "$l" | grep -o "workgroup limit [0-9]*" | grep -o "[0-9]*"); rg=$(echo "$l" | grep -o "range limit [0-9]*" | grep -o "[0-9]*")
[ $res = 0 ] && exact && [ "$(val hash)" = "$hash_dxvk3" ] && [ "${wg:-0}" -gt 0 ] && [ "${rg:-0}" -gt 0 ]
step $? "5 dxvk3, runs of up to 20 workgroups and 12000 bytes of source: texels $(val hash); new runs at the workgroup limit ${wg:-?}, at the range limit ${rg:-?}, $(cnt dispatches) dispatches"
run notab-one VKBRIDGE_BC_ONE_DISPATCH=1 VKBRIDGE_TEST_BC_RUN_LIMITS=0,0,notab -- --pattern dxvk3 ${VKB_BCUP_ARGS:-}
l=$(cv "$slog")
[ $res = 0 ] && exact && [ "$(val hash)" = "$hash_dxvk3" ] && [ "$(cnt dispatches)" = "$disp_dxvk3" ] && ! echo "$l" | grep -q "ONE_DISPATCH): [1-9]"
step $? "5 dxvk3, no table room: texels $(val hash); $(cnt dispatches) dispatches (one a region: $disp_dxvk3)"

# 6  small images decoded instead of encoded (VKBRIDGE_BC_SMALL_DECODE): a server that transcodes (lavapipe samples ETC2 and
#    ASTC when required), 100 textures a frame for 2 frames
#    The server needs a GPU that samples ETC2/ASTC (lavapipe does not: there it decodes everything): VKB_BC_TX_ICD, else
#    the PC's RADV when nobody plays a game on it (real-GPU tests only then), else the step is skipped (said).
tx=(vk_require_etc2=true vk_require_astc=true VKBRIDGE_BC_TRANSCODE=1 VKBRIDGE_BC_SMALL_DECODE=1)
small=(--count 100 --frames 2)
icd6=${VKB_BC_TX_ICD:-}
if [ -z "$icd6" ] && [ -f /usr/share/vulkan/icd.d/radeon_icd.json ] && ! pgrep -f '\.exe|steam_app|wine|Roblox' >/dev/null; then
  icd6=/usr/share/vulkan/icd.d/radeon_icd.json
fi
if [ -z "$icd6" ]; then
  echo "skip 6 small decode: no GPU that samples ETC2/ASTC here (set VKB_BC_TX_ICD; RADV not used while a game or Wine runs)"
else
export SRV_ICD=$icd6
run probe6 "${tx[@]}" -- --pattern dxvk3 --count 1 --frames 1
if grep -q "BC emulation: no transcoding" "$slog"; then
  echo "skip 6 small decode: the server's GPU ($icd6) samples neither ETC2 nor ASTC"
else
conv() { grep -o "BC conversions by converter, so far.*" "$slog" | tail -1 | sed 's/^[^)]*): //'; }
gpu() { cv "$slog" | grep -o "GPU [0-9.]* ms" | head -1 | grep -o "[0-9.]*"; }
sd() { grep -o "BC small decode so far: .*" "$slog" | tail -1; }
sdn() { local n; n=$(sd | grep -o "so far: [0-9]* image" | grep -o "[0-9]*"); echo "${n:-0}"; }
run small-off "${tx[@]}" VKBRIDGE_BC_SMALL_DECODE=0 -- --pattern dxvk3 "${small[@]}"
r0=$res; c0=$(conv); g0=$(gpu); h0=$(val hash)
echo "$c0" | grep -q -- "->" && ! echo "$c0" | grep -q " decode"; tx0=$?
run small-on "${tx[@]}" -- --pattern dxvk3 "${small[@]}"
c1=$(conv); g1=$(gpu); n1=$(sdn)
[ $r0 = 0 ] && [ $tx0 = 0 ] && [ $res = 0 ] && echo "$c1" | grep -q " decode" && ! echo "$c1" | grep -q -- "->" &&
  [ "$(val maxdiff_bc1)" -le 5 ] && [ "$(val maxdiff_bc3)" -le 1 ] && [ "${n1:-0}" -gt 0 ] &&
  awk -v a="${g0:-0}" -v b="${g1:-0}" 'BEGIN { exit !(a > 0 && b > 0 && b < a) }' &&
  grep -q "BC small decode: images of up to 65536 texels" "$slog"
step $? "6 128x128, small decode off: $(echo "$c0" | cut -c1-140), GPU ${g0:-?} ms; on: $(echo "$c1" | cut -c1-140), GPU ${g1:-?} ms, maxdiff bc1 $(val maxdiff_bc1) bc3 $(val maxdiff_bc3); $(sd | cut -c1-160)"
run big-off "${tx[@]}" VKBRIDGE_BC_SMALL_DECODE=0 -- --pattern dxvk3 --size 512 --count 10 --frames 1
r0=$res; hb0=$(val hash)
run big-on "${tx[@]}" -- --pattern dxvk3 --size 512 --count 10 --frames 1
[ $r0 = 0 ] && [ $res = 0 ] && [ -n "$hb0" ] && [ "$(val hash)" = "$hb0" ] && [ "$(sdn)" = 0 ] && ! conv | grep -q " decode"
step $? "6 512x512 (over the rule): texels $(val hash) with it on, $hb0 off; $(sd | cut -c1-120)"
run small-cap "${tx[@]}" VKBRIDGE_BC_SMALL_DECODE_MB=1 -- --pattern dxvk3 "${small[@]}"
c=$(conv); live=$(sd | grep -o "cost [0-9.]* MB" | grep -o "[0-9.]*" | head -1); past=$(sd | grep -o "[0-9]* image(s) ([0-9.]* Mtex) transcoded past" | grep -o "^[0-9]*")
[ $res = 0 ] && echo "$c" | grep -q " decode" && echo "$c" | grep -q -- "->" && [ "${past:-0}" -gt 0 ] && [ "$(sdn)" -gt 0 ] &&
  [ "$(grep -c "BC small decode: the cap of 1 MB is full" "$slog")" = 1 ] && awk -v l="${live:-9}" 'BEGIN { exit !(l <= 1.0) }'
step $? "6 cap 1 MB: $(sd | cut -c1-200)"
run small-query "${tx[@]}" -- --pattern dxvk3 "${small[@]}" --query
[ $res = 0 ] && [ "$(val query_bigger)" = 0 ] && [ "$(sdn)" = 0 ] && ! conv | grep -q " decode"
step $? "6 --query: query_bigger=$(val query_bigger); $(sd | cut -c1-200)"
run small-placed "${tx[@]}" VKBRIDGE_BC_SMALL_DECODE_MB=1 -- --pattern dxvk3 --count 1 --frames 1 --placed
[ $res = 0 ] && [ "$(val placed_bigger)" = 0 ] && [ "$(grep -c '^placed shape' "$o")" = 8 ] && grep -q "BC small decode: the cap of 1 MB is full" "$slog"
step $? "6 --placed (a temporary image sized while the cap is full, the real one after): placed_bigger=$(val placed_bigger); $(grep '^placed shape 0' "$o")"
fi
unset SRV_ICD
fi

# 7  the new variants by default (empty = the default)
def7=(VKBRIDGE_BC_ONE_DISPATCH= VKBRIDGE_BC_PALETTE= VKBRIDGE_PROBE_BUDGET_S=200 VKBRIDGE_TEST_BC_GPU=Mali-G925-Immortalis)
runs() { local n; n=$(cv "$slog" | grep -o "VKBRIDGE_BC_ONE_DISPATCH): [0-9]* runs" | grep -o "[0-9]* runs" | grep -o "[0-9]*"); echo "${n:-0}"; }
waited() { grep "(one dispatch a run)" "$slog" | grep " built in " | grep -vc "in the background"; }
# old converters built (a build line with none of the new variants' marks)
oldbuilt() { grep " built in " "$slog" | grep "BC emulation: \(decoder\|transcoder\) pipeline" | grep -v "one dispatch a run\|palette\|VKBRIDGE_BC_FAST" | grep -c .; }
# v0.5.5.4: the new variants are OFF by default in this build (VKBRIDGE_BC_ONE_DISPATCH / _PALETTE / _FAST default 0), so the default
# background mode of 0.5.6's step 7 does not exist here; asked for by name they run inline (step 5). Step 7 checks the default: the
# old variants only, the same texels, a dispatch per region, no runs, and the probe saying the old variants are used.
run def7 "${def7[@]}" -- --pattern dxvk3 ${VKB_BCUP_ARGS:-}
[ $res = 0 ] && exact && [ "$(val hash)" = "$hash_dxvk3" ] && [ "$(runs)" = 0 ] && [ "$(cnt dispatches)" = "$disp_dxvk3" ] &&
  ! grep -q "(one dispatch a run) built in\|palette search) built in" "$slog"
step $? "7 nothing set (default off in v0.5.5.4): texels $(val hash); $(cnt dispatches) dispatches (one a region: $disp_dxvk3), $(runs) runs; $(grep -o 'BC fast converters: [^(]*' "$slog" | head -1)"

# 8  PowerVR (VKBRIDGE_FAKE_DRIVER=img): the probe does not run the new variants there (the Pixel 10 Pro XL's GPU hung in it,
#    2026-11-10, and a PowerVR GPU hang stops the whole phone): old variants only, the same texels and dispatches;
#    VKBRIDGE_BCV_PROBE=1 runs it anyway
run pvr8 "${def7[@]}" VKBRIDGE_FAKE_DRIVER=img -- --pattern dxvk3 ${VKB_BCUP_ARGS:-}
[ $res = 0 ] && exact && [ "$(val hash)" = "$hash_dxvk3" ] && ! grep -q "probe: bc_variants " "$slog" &&
  { grep -q "the startup probe could not check the new converter variants: the old variants only\|the startup probe does not run here; the new converter variants are not used" "$slog" ||
    ! grep -q "new converter variants" "$slog"; } && [ "$(cnt dispatches)" = "$disp_dxvk3" ]  # v0.5.5.4: none wanted, so no line
r8=$?
run pvr8on "${def7[@]}" VKBRIDGE_FAKE_DRIVER=img VKBRIDGE_BCV_PROBE=1 -- --pattern dxvk3 ${VKB_BCUP_ARGS:-}
[ $r8 = 0 ] && [ $res = 0 ] && exact && grep -q "probe: bc_variants  *pass" "$slog"
step $? "8 PowerVR: bc_variants not run, old variants ($(cnt dispatches) dispatches); VKBRIDGE_BCV_PROBE=1 runs it: $(grep -o 'probe: bc_variants .*' "$slog" | cut -c1-100)"

if [ $fails = 0 ]; then echo "OK: back-to-back BC uploads share their barriers; every texel read back the same as with a barrier pair per upload"
else echo "FAILED: $fails step(s)"; exit 1; fi
