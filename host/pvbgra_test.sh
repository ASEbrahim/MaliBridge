#!/bin/bash
# pv_bgra_store (PV-07, PowerVR): storage writes to a B8G8R8A8 image land in R8G8B8A8 byte order. gamescope composites
# into a B8G8R8A8 storage image whose dma-buf our compositor shows as XR24: on a Pixel 10 Pro XL (DXT 25.1@6794074) every
# pixel came out with red and blue exchanged (Steam's #1A9FFF toggle read back as (255,159,26)).
# The driver's write order is imitated beneath every layer (VKBRIDGE_ERRATA_IMITATE=pv_bgra_store, server_errata_pvbgra.c).
# pvbgra_check does what gamescope does with its output image and prints how the picture leaves it: sampled, raw bytes
# (what a dma-buf consumer reads), copied into a plain BGRA image, and the dma-buf's st_dev:st_ino. "Shown" below is what
# our compositor puts on screen: the raw bytes read as BGRA (XR24), or as RGBA when the server registered that dma-buf
# as holding R8G8B8A8 (vkb_dmabuf_content_format, the compositor's question). Steam's blue = 26,159,255.
# A server of its own per step (own socket and cache directory: a fresh probe profile; switches in <cache>/env):
#   1  as it is (no imitation): the probe passes, the erratum is off, every reading right, no pv_bgra_store line
#      (the path every Mali / other GPU takes)
#   2  the imitation, the fix forced off: the phone's picture (sampled, shown and copied with R and B exchanged);
#      the probe fails with the swap pattern
#   3  the imitation, nothing forced (safe and performance mode): the probe turns the fix on; sampled, shown and
#      copied right; the image made R8G8B8A8, its dma-buf registered, the copy a blit
#   4  the fix forced on, nothing beneath: every reading right as well
#   5  byte for byte: step 1's readings equal those with the fix forced off (nothing changes where the probe passes)
#   6  the register alone (pvbgrareg_unit: keys by open file, dropped with the memory and the device)
# Prints one line per step and "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
work=$(mktemp -d)
cache=$work/c
mkdir -p "$cache"
sock=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-pvbgra-$$.sock
log=$work/server.log
srv=
stop() { [ -n "$srv" ] && kill $srv 2>/dev/null && wait $srv 2>/dev/null; srv=; }
start() { # start [env...]: a server of its own on a fresh cache directory (a fresh probe profile)
  stop
  rm -f "$sock"
  rm -rf "$cache"; mkdir -p "$cache"
  : > "$log"
  env -u VKBRIDGE_FAKE_MISSING -u VKBRIDGE_HIDE_EXTS -u VKBRIDGE_ERRATA -u VKBRIDGE_ERRATA_IMITATE -u VKBRIDGE_MODE \
      -u VKBRIDGE_FAKE_DRIVER_VERSION -u VKBRIDGE_ERRATA_PROBE -u VKBRIDGE_PROBE_FAKE -u VKBRIDGE_FAKE_DRIVER \
      VKBRIDGE_PROBE_BUDGET_S=0 "$@" VK_DRIVER_FILES=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json} VK_ICD_FILENAMES= \
      "$out/vkbridge-server" --socket "$sock" --log "$log" --cache-dir "$cache" >/dev/null 2>&1 &
  srv=$!
  for _ in $(seq 100); do [ -S "$sock" ] && break; kill -0 $srv 2>/dev/null || break; sleep 0.1; done
}
trap 'stop; rm -f "$sock"; [ -n "$KEEP" ] && echo "kept $work" || rm -rf "$work"' EXIT
trap 'exit 143' TERM INT HUP
export VKBRIDGE_SOCKET=$sock VK_DRIVER_FILES=$out/vkbridge_icd.json VK_ICD_FILENAMES=
unset VKBRIDGE_HIDE_EXTS VKBRIDGE_MAX_API
fails=0 steps=0
step() { [ -n "$DBG" ] && [ "$1" != 0 ] && grep -i "pv_bgra\|errata: device" "$log" | cut -c1-250; steps=$((steps+1)); if [ "$1" = 0 ]; then echo "  ok   $2"; else echo "  FAIL $2"; fails=$((fails+1)); fi; }
run() { # run -- env lines: $res the check's RESULT line, $rc its status
  shift
  printf '%s\n' "$@" > "$cache/env"
  timeout 120 "$out/pvbgra_check" >$work/run.log 2>&1
  rc=$?
  sleep 0.2
  res=$(grep "^RESULT\|^FAIL" $work/run.log | tail -1)
}
field() { echo "$res" | sed -n "s/.* $1=\([^ ]*\).*/\1/p"; }
# what our compositor shows from the raw bytes: RGBA where the server named the dma-buf vkbridge-rgba8, else BGRA (XR24)
shown() {
  local raw key b0 b1 b2
  raw=$(field raw); key=$(field dmabuf)
  [ -n "$raw" ] || { echo "-"; return; }
  b0=$((16#${raw:0:2})); b1=$((16#${raw:2:2})); b2=$((16#${raw:4:2}))
  # the compositor runs in another process than the server on DroidDeck's builds: it goes by the dma-buf's name
  if [ "$(field name)" = vkbridge-rgba8 ]; then echo "$b0,$b1,$b2 (named: RGBA)"; else echo "$b2,$b1,$b0"; fi
}
copied() { local c; c=$(field copy); [ -n "$c" ] || { echo "-"; return; }; echo "$((16#${c:4:2})),$((16#${c:2:2})),$((16#${c:0:2}))"; }
BLUE=26,159,255 SWAP=255,159,26
IMI=VKBRIDGE_ERRATA_IMITATE=pv_bgra_store
fixline="pv_bgra_store: image .* B8G8R8A8 with storage made R8G8B8A8"

# 1 as it is
start
run -- VKBRIDGE_MODE=safe
r1=$res
[ $rc = 0 ] && [ "$(field sampled)" = $BLUE ] && [ "$(shown)" = $BLUE ] && [ "$(copied)" = $BLUE ] &&
  ! grep -q "probe: pv_bgra_store " "$log" && grep -q "errata: device .*: on [^;]*; off .*pv_bgra_store" "$log" && [ "$(field name)" != vkbridge-rgba8 ] &&
  ! grep -q "pv_bgra_store:" "$log"
step $? "as it is (not PowerVR): sampled $(field sampled), shown $(shown), copied $(copied); probe not run, the erratum off, no pv_bgra_store line ($res)"
# 1b PowerVR's identity on a driver without the bug: the probe runs and passes, the erratum off
start VKBRIDGE_FAKE_DRIVER=img
run -- VKBRIDGE_MODE=safe
[ $rc = 0 ] && [ "$(shown)" = $BLUE ] && grep -q "probe: pv_bgra_store  *pass" "$log" && grep -q "errata: device .*: on [^;]*; off .*pv_bgra_store" "$log"
step $? "PowerVR identity, no bug beneath: the probe runs and passes, the erratum off ($res)"
# 2 the imitation, the fix forced off: the phone's picture
start $IMI
run -- VKBRIDGE_ERRATA=-pv_bgra_store
[ $rc = 0 ] && [ "$(field sampled)" = $SWAP ] && [ "$(shown)" = $SWAP ] && [ "$(copied)" = $SWAP ] &&
  grep -q "probe: pv_bgra_store  *fail .*R and B exchanged" "$log" &&
  grep -q "errata: pv_bgra_store OFF (VKBRIDGE_ERRATA=-pv_bgra_store; it would be on: probe pv_bgra_store failed" "$log" &&
  ! grep -q "$fixline" "$log"
step $? "PowerVR's write order imitated, fix forced off: sampled $(field sampled), shown $(shown), copied $(copied) - R and B exchanged as on the phone ($res)"
# 3 the imitation, nothing forced: the probe turns the fix on
for mode in safe performance; do
  start $IMI
  run -- VKBRIDGE_MODE=$mode
  key=$(field dmabuf)
  [ $rc = 0 ] && [ "$(field sampled)" = $BLUE ] && [ "${key}" = none -o "$(shown)" = "$BLUE (named: RGBA)" ] && [ "$(copied)" = $BLUE ] &&
    grep -q "errata: pv_bgra_store ON (probe pv_bgra_store failed; $mode mode) - PV-07" "$log" && grep -q "$fixline" "$log" &&
    grep -q "pv_bgra_store: a copy between a converted image and a B8G8R8A8 one made a blit" "$log"
  step $? "the imitation, the probe's failure turns the fix on ($mode mode): sampled $(field sampled), shown $(shown), copied $(copied) ($res)"
done
# 4 forced on, nothing beneath
start
run -- VKBRIDGE_ERRATA=+pv_bgra_store
key=$(field dmabuf)
[ $rc = 0 ] && [ "$(field sampled)" = $BLUE ] && [ "${key}" = none -o "$(shown)" = "$BLUE (named: RGBA)" ] && [ "$(copied)" = $BLUE ] &&
  grep -q "errata: pv_bgra_store ON (VKBRIDGE_ERRATA=+pv_bgra_store[^)]*) - PV-07" "$log" && grep -q "$fixline" "$log"
step $? "forced on over a driver that writes right: sampled $(field sampled), shown $(shown), copied $(copied) ($res)"
# 5 byte for byte where the probe passes
start
run -- VKBRIDGE_ERRATA=-pv_bgra_store
r5=$res
strip() { echo "$1" | sed 's/ dmabuf=[^ ]*//; s/ name=[^ ]*//'; }
[ -n "$r1" ] && [ "$(strip "$r1")" = "$(strip "$r5")" ] && ! grep -q "pv_bgra_store:" "$log"
step $? "byte for byte: step 1's readings equal the fix forced off ($(strip "$r5"))"
# 6 the register alone
"$out/pvbgrareg_unit" >$work/unit.log 2>&1
step $? "the register: $(tail -1 $work/unit.log)"

[ "$(field dmabuf)" = none ] && echo "  note: this driver exports no dma-bufs through the bridge: the shown column read the bytes as XR24 only"
if [ $fails = 0 ]; then echo "OK: $steps steps"; else echo "FAILED: $fails of $steps steps"; exit 1; fi
