#!/bin/bash
# pv_mutable_linear (PV-02, PowerVR): the image-format query for a LINEAR / DRM-modifier, dma-buf shareable,
# mutable-format RGBA8 / BGRA8 image is refused though the driver makes, exports and imports that image.
# On a Pixel 10 Pro XL (DXT-48-1536, 25.1@6794074, test47; a player log session.log:99-101) gamescope's
# flippable-image query (B8G8R8A8_UNORM, usage 0x6, flags 0x8, 2 view formats, modifiers 0x0 and
# 0x00ffffffffffffff) came back -11 for both, gamescope kept no modifier and aborted ("modifiers.size > 0").
# The driver's answer is imitated beneath every layer (VKBRIDGE_FAKE_MISSING=pvmutable, server_emu.c).
# A server of its own per step (own socket and cache directory: a fresh probe profile; switches in <cache>/env):
#   1  as it is (the desktop driver answers yes): gamescope's query keeps LINEAR; the probe passes; the
#      erratum is off and answers nothing (the path every Mali / other GPU takes)
#   2  PowerVR's refusal imitated, the fix forced off (VKBRIDGE_ERRATA=-pv_mutable_linear) and PR mutable
#      dma-buf retry off (VKBRIDGE_DMABUF_MUTABLE_RETRY=0): gamescope keeps no modifier (the phone's abort); the
#      pattern queries refused, the images made; the probe fails with the bug's pattern
#   2b the refusal, PV-02 forced off, the retry as it is (this driver exports its own dma-bufs: no export
#      emulation): the retry (K11MCH1, PR #31; server_memory.c) asks again without the mutable flag, gamescope
#      keeps LINEAR and its image is made, said by the retry's line; PV-02 answers nothing
#   3  the imitation, nothing forced (safe and performance mode): the probe's failure alone turns the fix on;
#      gamescope keeps the LINEAR modifier and its image is made; the controls (the query without the mutable
#      flag, a mutable RGBA16F query) as the driver says; PV-02 answers first, so the retry over it is silent
#   4  the fix forced on (VKBRIDGE_ERRATA=+pv_mutable_linear) with no refusal beneath: nothing is answered by
#      it, the queries are the driver's own (forced on, it still changes nothing where the driver says yes)
#   5  the protocol hash is test47's (server-side only)
# Needs a driver with VK_EXT_image_drm_format_modifier + dma-bufs (radeon; lavapipe has no modifiers: SKIP).
# Prints one line per step and "OK: ..." / "FAILED: ..." last. Fails on 3c396c2 (test47): steps 1-4 need the probe
# and the erratum, and step 3's gamescope query keeps no modifier there.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
work=$(mktemp -d)
cache=$work/c
mkdir -p "$cache"
sock=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-pvmutable-$$.sock
log=$work/server.log
srv=
stop() { [ -n "$srv" ] && kill $srv 2>/dev/null && wait $srv 2>/dev/null; srv=; }
start() { # start [env...]: a server of its own on a fresh cache directory (a fresh probe profile)
  stop
  rm -f "$sock"
  rm -rf "$cache"; mkdir -p "$cache"
  : > "$log"
  env -u VKBRIDGE_FAKE_MISSING -u VKBRIDGE_HIDE_EXTS -u VKBRIDGE_ERRATA -u VKBRIDGE_ERRATA_IMITATE -u VKBRIDGE_MODE \
      -u VKBRIDGE_FAKE_DRIVER_VERSION -u VKBRIDGE_ERRATA_PROBE -u VKBRIDGE_PROBE_FAKE \
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
step() { [ -n "$DBG" ] && [ "$1" != 0 ] && echo "$new" | grep -i "pv_mutable\|errata: device\|pvmutable" | cut -c1-250; steps=$((steps+1)); if [ "$1" = 0 ]; then echo "  ok   $2"; else echo "  FAIL $2"; fails=$((fails+1)); fi; }
run() { # run <prog> [args] -- env lines: $new the log lines it added, $res its last output line, $rc its status
  local prog=("$1"); shift
  while [ "$1" != -- ]; do prog+=("$1"); shift; done; shift
  printf '%s\n' "$@" > "$cache/env"
  local l; l=$(wc -l < "$log")
  timeout 120 "${prog[@]}" >$work/run.log 2>&1
  rc=$?
  sleep 0.2
  res=$(tail -1 $work/run.log)
  new=$(tail -n +$((l+1)) "$log")
}
FAKE=VKBRIDGE_FAKE_MISSING=pvmutable
answered="errata: pv_mutable_linear: a mutable-format LINEAR dma-buf image query refused by the driver answered with the base format's (0)"

# 1 as it is
start
run "$out/pvmutable_check" --gamescope -- VKBRIDGE_MODE=safe
if echo "$res" | grep -q "^SKIP"; then
  echo "  ok   $res (nothing to test on this driver)"
  echo "OK: skipped"
  exit 0
fi
g1=$res; grc=$rc
run "$out/pvmutable_check" --expect yes -- VKBRIDGE_MODE=safe
[ $grc = 0 ] && echo "$g1" | grep -q "modifier 0x0 query 0;" && [ $rc = 0 ] && grep -q "probe: pv_mutable_linear  *pass" "$log" &&
  ! grep -q "pv_mutable_linear:" "$log" && grep -q "errata: device .*: on [^;]*; off .*pv_mutable_linear" "$log"
step $? "as it is: gamescope keeps LINEAR, probe passes, the erratum off and answering nothing ($g1 | $res)"
# 2 the refusal, fix forced off
start $FAKE
run "$out/pvmutable_check" --gamescope -- VKBRIDGE_ERRATA=-pv_mutable_linear VKBRIDGE_DMABUF_MUTABLE_RETRY=0
g2=$res; grc=$rc
run "$out/pvmutable_check" --expect no -- VKBRIDGE_ERRATA=-pv_mutable_linear VKBRIDGE_DMABUF_MUTABLE_RETRY=0
[ $grc != 0 ] && echo "$g2" | grep -q "modifier 0x0 query -11; modifier 0xffffffffffffff query -11; no modifier kept" &&
  [ $rc = 0 ] && echo "$res" | grep -q "RGBA8 query -11, image 0; BGRA8 query -11, image 0" &&
  grep -q "imitating PowerVR's refusal" "$log" &&
  grep -q "probe: pv_mutable_linear  *fail .*the query refuses an image the driver makes and shares" "$log" &&
  grep -q "errata: pv_mutable_linear OFF (VKBRIDGE_ERRATA=-pv_mutable_linear; it would be on: probe pv_mutable_linear failed" "$log" &&
  ! grep -q "$answered" "$log"
step $? "PowerVR's refusal imitated, fix forced off: gamescope keeps no modifier as on the phone ($g2 | $res)"
# 2b the refusal, PV-02 forced off, PR retry as it is
retried="dma-buf image query at DRM-modifier tiling refused with a mutable format; without it: 0"
start $FAKE
run "$out/pvmutable_check" --gamescope -- VKBRIDGE_ERRATA=-pv_mutable_linear
g2b=$res; grc=$rc
run "$out/pvmutable_check" --expect yes -- VKBRIDGE_ERRATA=-pv_mutable_linear
[ $grc = 0 ] && echo "$g2b" | grep -q "modifier 0x0 query 0; modifier 0xffffffffffffff query -11; 1 modifier(s) kept, image made" &&
  [ $rc = 0 ] && grep -q "$retried" "$log" && ! grep -q "$answered" "$log"
step $? "the refusal, PV-02 forced off: PR retry asks without the mutable flag, gamescope keeps LINEAR ($g2b | $res)"
# 3 the refusal, nothing forced: the probe turns the fix on
for mode in safe performance; do
  start $FAKE
  run "$out/pvmutable_check" --gamescope -- VKBRIDGE_MODE=$mode
  g3=$res; grc=$rc
  run "$out/pvmutable_check" --expect yes -- VKBRIDGE_MODE=$mode
  [ $grc = 0 ] && echo "$g3" | grep -q "modifier 0x0 query 0; modifier 0xffffffffffffff query -11; 1 modifier(s) kept, image made" &&
    [ $rc = 0 ] && grep -q "errata: pv_mutable_linear ON (probe pv_mutable_linear failed; $mode mode) - PV-02" "$log" && grep -q "$answered" "$log" &&
    ! grep -q "refused with a mutable format; without it" "$log"
  step $? "the imitation, the probe's failure turns the fix on ($mode mode): gamescope keeps LINEAR ($g3 | $res)"
done
# 4 forced on, nothing refused beneath: the driver's own answers
start
run "$out/pvmutable_check" --gamescope -- VKBRIDGE_ERRATA=+pv_mutable_linear
g4=$res; grc=$rc
run "$out/pvmutable_check" --expect yes -- VKBRIDGE_ERRATA=+pv_mutable_linear
[ $grc = 0 ] && [ "$g4" = "$g1" ] && [ $rc = 0 ] && grep -q "errata: pv_mutable_linear ON (VKBRIDGE_ERRATA=+pv_mutable_linear; probe pv_mutable_linear passed) - PV-02" "$log" &&
  ! grep -q "$answered" "$log"
step $? "forced on over a driver that says yes: nothing answered by it, same answers as step 1 ($g4)"
# 5
grep -q "^#define VKB_PROTOCOL_HASH 0xe88f0e43c2b566b9ULL" ../common/gen/vkb_gen.h
step $? "protocol hash $(grep -o '0x[0-9a-f]*' ../common/gen/vkb_gen.h | head -1) (test47's)"

if [ $fails = 0 ]; then echo "OK: $steps steps"; else echo "FAILED: $fails of $steps steps"; exit 1; fi
