#!/bin/bash
# PowerVR version ranges (server_errata.c: an erratum range has a vendor; PowerVR's are in changelists,
# the 6908880 of the driverInfo "25.3@6908880") on the desktop driver with the PowerVR driver faked
# (VKBRIDGE_FAKE_DRIVER_VERSION=pvr25.3@<CL>, or pvr<CL>). No real erratum has a PowerVR range yet, so the
# no-op "example" erratum carries ranges no real driver is in (CL 1, r999p0), as the pattern of the first:
#   1  a PowerVR driver inside its range (pvr25.3@1, pvr1): example on, the reason "CL 1 in CL 1-1 (...)",
#      in safe and performance mode alike; the driver named "PowerVR driver 25.3 (CL 1) ... its own CL ranges only";
#   2  outside it (CL 2, the real 25.3@6908880, no changelist at all, an unreadable value warned about):
#      example off; and no Arm range reads a PowerVR driver (the Arm-gated errata stay as on any other driver);
#   3  Arm's r999p0 inside its range: example on with "r999p0 in r999p0-r999"; an Arm driver never reads
#      the PowerVR range (r1p0 is not CL 1: example off);
#   4  the same under the PowerVR identity of the host tests (VKBRIDGE_FAKE_DRIVER=img-full, CL 6908880): off;
#   5  no PowerVR range shows in the log of a driver outside every range;
#   6  the protocol hash is unchanged.
# Prints one line per step and "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
work=$(mktemp -d)
cache=$work/c
mkdir -p "$cache"
sock=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-pvrange-$$.sock
log=$work/server.log
srv=
start() { # start [env...]
  [ -n "$srv" ] && kill $srv 2>/dev/null && wait $srv 2>/dev/null
  rm -f "$sock"
  rm -rf "$cache"; mkdir -p "$cache"
  : > "$log"
  env -u VKBRIDGE_FAKE_MISSING -u VKBRIDGE_HIDE_EXTS -u VKBRIDGE_ERRATA -u VKBRIDGE_ERRATA_IMITATE -u VKBRIDGE_MODE \
      -u VKBRIDGE_FAKE_DRIVER_VERSION -u VKBRIDGE_ERRATA_PROBE -u VKBRIDGE_PROBE_FAKE VKBRIDGE_PROBE_BUDGET_S=0 "$@" \
      VK_DRIVER_FILES=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json} VK_ICD_FILENAMES= \
      "$out/vkbridge-server" --socket "$sock" --log "$log" --cache-dir "$cache" >/dev/null 2>&1 &
  srv=$!
  for _ in $(seq 100); do [ -S "$sock" ] && break; sleep 0.1; done
}
trap 'kill $srv 2>/dev/null; rm -f "$sock"; [ -n "$KEEP" ] || rm -rf "$work"' EXIT
export VKBRIDGE_SOCKET=$sock VK_DRIVER_FILES=$out/vkbridge_icd.json VK_ICD_FILENAMES=
unset VKBRIDGE_HIDE_EXTS VKBRIDGE_MAX_API
fails=0 steps=0
step() { steps=$((steps+1)); if [ "$1" = 0 ]; then echo "  ok   $2"; else echo "  FAIL $2"; fails=$((fails+1)); fi; }
# One device with <cache dir>/env = the arguments; $new: its log lines
device() {
  printf '%s\n' "$@" > "$cache/env"
  local l; l=$(wc -l < "$log")
  timeout 120 "$out/vkbridge-smoke" >$work/smoke.log 2>&1
  new=$(tail -n +$((l+1)) "$log")
}
on_set() { echo "$new" | grep -o 'errata: device .*: on [^;]*' | tail -1 | sed 's/.*: on //'; }
base_safe=wait_retry,vb_split,view_quarantine,view_counts,cull_nontri # test49: dyn_reemit only where its probe has not passed (G_PROBE_UNVERIFIED)
base_perf=wait_retry,vb_split,view_counts
# a PowerVR driver (faked) also has pv_view3d_base on, in both modes: its multiview rule (PV-04), whatever the probe says
pvr_safe=$base_safe,pv_view3d_base
pvr_perf=$base_perf,pv_view3d_base

start
# 1
for mode in safe performance; do
  device VKBRIDGE_MODE=$mode VKBRIDGE_FAKE_DRIVER_VERSION=pvr25.3@1
  got=$(on_set)
  base=$pvr_safe; [ $mode = performance ] && base=$pvr_perf
  [ "$got" = "$base,example" ] && echo "$new" | grep -q "errata: example ON (CL 1 in CL 1-1 (a test range no real PowerVR driver is in); $mode mode)" &&
    echo "$new" | grep -q "PowerVR driver 25.3 (CL 1), not Arm's: its own CL ranges only"
  step $? "pvr25.3@1, $mode mode: example on by its CL range ($got)"
done
device VKBRIDGE_FAKE_DRIVER_VERSION=pvr1
[ "$(on_set)" = "$pvr_safe,example" ] && echo "$new" | grep -q "PowerVR driver (CL 1), not Arm's: its own CL ranges only"
step $? "pvr1 (no release, the changelist alone): example on"
# 2
for v in pvr25.3@2 pvr25.3@6908880 pvr25.3@0 pvrxyz; do
  device VKBRIDGE_FAKE_DRIVER_VERSION=$v
  got=$(on_set)
  [ "$got" = "$pvr_safe" ] && ! echo "$new" | grep -q "errata: example ON"
  step $? "$v: outside the range or unknown, example off, the Arm-gated errata not read ($got)"
done
echo "$new" | grep -q "VKBRIDGE_FAKE_DRIVER_VERSION=pvrxyz is not pvr<release>@<changelist> or pvr<changelist>; the version counts as unknown"
step $? "an unreadable value warned about"
# 3
device VKBRIDGE_FAKE_DRIVER_VERSION=r999p0
got=$(on_set)
echo "$got" | grep -q "example" && echo "$new" | grep -q "errata: example ON (r999p0 in r999p0-r999 (a test range no real Arm driver is in); safe mode)"
step $? "Arm r999p0: example on by its Arm range, as before ($got)"
device VKBRIDGE_FAKE_DRIVER_VERSION=r1p0
echo "$(on_set)" | grep -q "example"
[ $? != 0 ]; step $? "Arm r1p0 (not CL 1): the PowerVR range is not read"
# 4
start VKBRIDGE_FAKE_DRIVER=img-full VKBRIDGE_ZINK_PROFILE_PROBE=0
device
got=$(on_set)
[ "$got" = "$pvr_safe" ] && echo "$new" | grep -q "PowerVR driver 25.3 (CL 6908880), not Arm's: its own CL ranges only"
step $? "PowerVR identity (VKBRIDGE_FAKE_DRIVER=img-full): example off ($got)"
# 5
if [ -x "$out/vkbridge-server" ]; then
  ! grep -q "CL [0-9]*-" "$log"
  step $? "no PowerVR range in the log of a driver outside every range"
fi
# 6
grep -q "^#define VKB_PROTOCOL_HASH 0xe88f0e43c2b566b9ULL" ../common/gen/vkb_gen.h
step $? "protocol hash $(grep -o '0x[0-9a-f]*' ../common/gen/vkb_gen.h | head -1)"

if [ $fails = 0 ]; then echo "OK: $steps steps"; else echo "FAILED: $fails of $steps steps"; exit 1; fi
