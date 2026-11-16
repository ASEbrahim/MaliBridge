#!/bin/bash
# A game's first BC burst (server_emu_bcn.c fast_settle_now, fast_*): FAST_BG built the old converter first (the upload waiting)
# and the new one (one dispatch a run, palette, BC_FAST) behind it, so the first burst of a game new to the driver ran one
# dispatch a copy (S11 Ultra G925 K1d, A35 G68 K1a: 8370 for 8370). VKBRIDGE_TEST_BC_FAST_HOLD=1 keeps every new variant built in
# the background unbuilt for the whole run (a burst that comes before their compile).
#   1  a cache from an older bridge (old + one-dispatch converters, no line beside it), BC_FAST on, its BC_FAST variants held
#      (VKBRIDGE_TEST_BC_FAST_HOLD=2): FAST_BG as before, and
#      while a BC_FAST variant is unbuilt the converter BC_FAST=0 takes (one dispatch a run), not one a copy.
#   2  a new program, the probe's pass known, BC_FAST on: the new variants in place of the old ones (BC_FAST ones included).
#   3  the same with VKBRIDGE_BC_FAST=0, and on a GPU outside the gate (Mali-G68): in place too (no BC_FAST variant).
#   4  the line beside the cache: it holds the new set -> in place (cache hits); it holds the old ones only -> FAST_BG as before.
# Needs a server GPU that samples ETC2/ASTC (the transcoders are what has BC_FAST variants): VKB_BC_TX_ICD, else the PC's RADV
# when no game or Wine runs, else skipped (said), as bcupload_test.sh step 6. Prints one line per step and "OK:"/"FAILED:" last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
icd=${VKB_CLIENT_ICD:-$out/vkbridge_icd.json}
work=$(mktemp -d)
sock=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-bcfirst-$$.sock
srv=; fails=0
stop() { [ -n "$srv" ] && kill $srv 2>/dev/null && wait $srv 2>/dev/null; srv=; }
trap 'stop; rm -rf "$work" "$sock"' EXIT
txicd=${VKB_BC_TX_ICD:-}
if [ -z "$txicd" ] && [ -f /usr/share/vulkan/icd.d/radeon_icd.json ] && ! pgrep -f '\.exe|steam_app|wine|Roblox' >/dev/null; then
  txicd=/usr/share/vulkan/icd.d/radeon_icd.json
fi
[ -z "$txicd" ] && { echo "OK: skipped (no GPU that samples ETC2/ASTC here: set VKB_BC_TX_ICD; RADV not used while a game or Wine runs)"; exit 0; }
G925="Mali-G925-Immortalis MC12"
# run <name> <cachedir> [server env...]: $res $slog $o
run() {
  local name=$1 cdir=$2; shift 2
  stop; rm -f "$sock"; mkdir -p "$cdir"
  slog=$work/$name.server.log; o=$work/$name.out
  env -u VKBRIDGE_FAKE_MISSING -u VKBRIDGE_PROBE -u VKBRIDGE_PROBE_BCV_FAKE -u VKBRIDGE_BC_FAST -u VKBRIDGE_TEST_BC_GPU -u VKBRIDGE_BC_ONE_DISPATCH \
      -u VKBRIDGE_BC_PALETTE -u VKBRIDGE_BC_SMALL_DECODE -u VKBRIDGE_BC_TRANSCODE -u VKBRIDGE_BC_BATCH -u VKBRIDGE_TEST_BC_FAST_HOLD -u VKBRIDGE_NO_PCACHE \
      -u WAYLAND_DISPLAY -u DISPLAY VKBRIDGE_PROBE_BUDGET_S=200 VKBRIDGE_FAKE_MISSING=bc vk_require_etc2=true vk_require_astc=true \
      VKBRIDGE_BC_TRANSCODE=1 VKBRIDGE_BC_SMALL_DECODE=0 VKBRIDGE_TEST_BC_GPU="$G925" "$@" \
      VK_DRIVER_FILES="$txicd" VK_ICD_FILENAMES= "$out/vkbridge-server" --socket "$sock" --log "$slog" --cache-dir "$cdir" >/dev/null 2>&1 &
  srv=$!
  for _ in $(seq 600); do [ -S "$sock" ] && break; kill -0 $srv 2>/dev/null || break; sleep 0.05; done
  timeout 600 env -u WAYLAND_DISPLAY -u DISPLAY VKBRIDGE_SOCKET="$sock" VK_DRIVER_FILES="$icd" VK_ICD_FILENAMES= \
    "$out/bcupload_check" --pattern dxvk3 >"$o" 2>&1
  res=$?
  for _ in $(seq 200); do grep -q "BC uploads so far" "$slog" && break; sleep 0.05; done
  sleep 0.3 # the device's end saves its pipeline cache
  stop
  [ -n "${VKB_KEEP:-}" ] && cp "$slog" "$o" "$VKB_KEEP/" 2>/dev/null
}
step() { if [ "$1" = 0 ]; then echo "ok   $2"; else echo "FAIL $2"; fails=$((fails+1)); grep -h "BC uploads so far\|BC emulation: \(new conv\|this prog\|the startup\)" "$slog" | tail -4 | cut -c1-220 | sed 's/^/     /'; fi; }
val() { grep -o "$1=[0-9a-z.]*" "$o" | head -1 | cut -d= -f2; }
l_up() { grep -o "BC uploads so far: .*" "$slog" | tail -1; }
copies() { l_up | grep -o "so far: [0-9]* copies" | grep -o "[0-9]*"; }
disp() { l_up | grep -o "[0-9]* dispatches" | grep -o "[0-9]*"; }
# old converters built (a build line with none of the new variants' marks)
oldbuilt() { grep " built in " "$slog" | grep "BC emulation: transcoder pipeline" | grep -v "one dispatch a run\|palette\|VKBRIDGE_BC_FAST" | grep -c .; }
fastbuilt() { grep " built in " "$slog" | grep "BC emulation: transcoder pipeline" | grep -c "VKBRIDGE_BC_FAST"; }
progcache() { ls "$1"/*-bcupload_check*.vkpc 2>/dev/null; }

run probe "$work/p" VKBRIDGE_BC_FAST=0
if grep -q "BC emulation: no transcoding" "$slog"; then echo "OK: skipped (the server's GPU, $txicd, samples neither ETC2 nor ASTC)"; exit 0; fi

# 1  played before with BC_FAST=0 by a bridge before this one (test47: the old and the one-dispatch converters in its cache, no
#    line beside it): FAST_BG as before (no hit made a compile), BC_FAST on, only its BC_FAST variants held (=2): BC_FAST=0's
#    converter meanwhile
c1=$work/c1
run u0 "$c1" VKBRIDGE_BC_FAST=0 VKBRIDGE_BC_FAST_FRESH=0
run u0b "$c1" VKBRIDGE_BC_FAST=0 VKBRIDGE_BC_FAST_FRESH=0
d0=$(disp); n0=$(copies); h0=$(val hash)
rm -f "$c1"/*.vkpc.bc
run u1 "$c1" VKBRIDGE_TEST_BC_FAST_HOLD=2
d1=$(disp); n1=$(copies)
[ $res = 0 ] && [ -n "$h0" ] && [ "$(val hash)" = "$h0" ] && [ -n "$d0" ] && [ -n "$d1" ] && [ "$n1" = "$n0" ] && [ "$d0" -lt "$n0" ] &&
  [ "$d1" -le $((d0 + d0 / 4 + 2)) ] && [ "$(fastbuilt)" = 0 ] && ! grep -q "built in place of the old ones" "$slog"
step $? "1 played with BC_FAST=0 before, BC_FAST on, its variants held: $d1 dispatches for $n1 copies (BC_FAST=0 on the same cache: $d0 for $n0); texels $(val hash) ($h0); BC_FAST variants built $(fastbuilt)"

# 2-3  a new program, the probe's pass known (its verdict kept, the program's cache removed), the new variants' background
#      builds held: BC_FAST on (G925), BC_FAST=0, a GPU outside the gate (G68): the first burst converts with the new variants,
#      built in place of the old ones (no old one built), as many dispatches as BC_FAST=0 on a warm cache
c2=$work/c2
run n0 "$c2" VKBRIDGE_BC_FAST=0
for v in "2 n1 VKBRIDGE_BC_FAST_FRESH=" "3 n-f0 VKBRIDGE_BC_FAST=0" "3 n-g68 VKBRIDGE_TEST_BC_GPU=Mali-G68"; do
  set -- $v
  rm -f $(progcache "$c2") "$c2"/*.vkpc.bc
  run "$2" "$c2" VKBRIDGE_TEST_BC_FAST_HOLD=1 "$3"
  dn=$(disp); nn=$(copies)
  want_fast=0; [ "$1" = 2 ] && want_fast=1
  [ $res = 0 ] && [ "$(val hash)" = "$h0" ] && [ -n "$dn" ] && [ "$nn" = "$n0" ] && [ "$dn" -le $((d0 + d0 / 4 + 2)) ] && [ "$(oldbuilt)" = 0 ] &&
    if [ $want_fast = 1 ]; then [ "$(fastbuilt)" -gt 0 ]; else [ "$(fastbuilt)" = 0 ]; fi
  step $? "$1 a new program, $3 (background builds held): $dn dispatches for $nn copies; old converters built $(oldbuilt), BC_FAST ones $(fastbuilt); texels $(val hash)"
done

# 4  what the program's cache holds (the line beside it): the new set -> in place from the cache; the old ones only (a session
#    without the probe's pass) -> FAST_BG as before, the old ones from the cache, no hit made a compile
rm -f $(progcache "$c2") "$c2"/*.vkpc.bc
run m1 "$c2"
run m2 "$c2" VKBRIDGE_TEST_BC_FAST_HOLD=1
[ $res = 0 ] && [ "$(val hash)" = "$h0" ] && grep -q "pipeline cache holds the new converter variants" "$slog" && [ "$(oldbuilt)" = 0 ] && [ "$(disp)" -le $((d0 + d0 / 4 + 2)) ]
step $? "4 the cache holds the new set: in place: $(disp) dispatches, old converters built $(oldbuilt)"
rm -f $(progcache "$c2") "$c2"/*.vkpc.bc
run m3 "$c2" VKBRIDGE_BC_FAST_FRESH=0 VKBRIDGE_TEST_BC_FAST_HOLD=1
run m4 "$c2"
[ $res = 0 ] && [ "$(val hash)" = "$h0" ] && ! grep -q "built in place of the old ones" "$slog" && [ "$(oldbuilt)" -gt 0 ]
step $? "4 the cache holds the old ones only: FAST_BG as before, old converters built $(oldbuilt) ($(grep 'BC emulation: transcoder pipeline' "$slog" | grep ' built in ' | grep -v 'one dispatch a run\|palette\|VKBRIDGE_BC_FAST' | grep -o 'in [0-9.]* ms' | tr '\n' ' ' | cut -c1-90))"
if [ $fails = 0 ]; then echo "OK: a game's first burst converts with the new variants on every GPU ($d1 / $dn dispatches vs $d0, $n0 copies)"; exit 0
else echo "FAILED: $fails step(s)"; exit 1; fi
