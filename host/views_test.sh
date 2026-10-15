#!/bin/bash
# The image-view errata (server_errata_views.c) on the desktop driver, with a server of its own (its
# own socket and cache directory; the switches go in <cache dir>/env, re-read at each device):
#   view_counts (MK-07), view3d_check:
#     1  on (always): every case passes, the 3D image's views are given explicit counts;
#     2  the bug imitated beneath the fix, fix off: the 3D cases fail;
#     3  the bug imitated, fix on: all pass, nothing reaches the imitation;
#     4  fix off, no imitation: all pass (the desktop driver resolves REMAINING itself);
#   view_quarantine (MK-06), viewchurn_check:
#     5  the bug imitated, r44p1 (off): handle values come back on other images and are wrong;
#     6  the bug imitated, r49p1 beta (on by the version gate): all right, views held back;
#     7  10000 iterations and 10000 with images made and destroyed: the quarantine stays at its
#        caps (4096 views, 1024 images), the server's RSS does not grow from the first run to the
#        next ones;
#     8  a small quarantine (VKBRIDGE_VIEW_QUARANTINE=64, so 16 images): the caps hold;
#     9  the other checks with view_quarantine on: smoke, recycle_check, nulldesc_check, view3d;
#  10  the protocol hash is unchanged (the layer is server-side only).
# Prints one line per step and "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
work=$(mktemp -d)
cache=$work/c
mkdir -p "$cache"
sock=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-views-$$.sock
log=$work/server.log
srv=
start() {
  [ -n "$srv" ] && kill $srv 2>/dev/null && wait $srv 2>/dev/null
  rm -f "$sock"
  env -u VKBRIDGE_FAKE_MISSING -u VKBRIDGE_HIDE_EXTS -u VKBRIDGE_ERRATA -u VKBRIDGE_ERRATA_IMITATE -u VKBRIDGE_MODE \
      -u VKBRIDGE_FAKE_DRIVER_VERSION -u VKBRIDGE_ERRATA_PROBE -u VKBRIDGE_VIEW_QUARANTINE "$@" \
      VK_DRIVER_FILES=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json} VK_ICD_FILENAMES= \
      "$out/vkbridge-server" --socket "$sock" --log "$log" --cache-dir "$cache" >/dev/null 2>&1 &
  srv=$!
  for _ in $(seq 50); do [ -S "$sock" ] && break; sleep 0.1; done
}
trap 'kill $srv 2>/dev/null; rm -f "$sock"; [ -n "$KEEP" ] || rm -rf "$work"' EXIT
export VKBRIDGE_SOCKET=$sock VK_DRIVER_FILES=$out/vkbridge_icd.json VK_ICD_FILENAMES=
unset VKBRIDGE_HIDE_EXTS VKBRIDGE_MAX_API
fails=0 steps=0
step() { steps=$((steps+1)); if [ "$1" = 0 ]; then echo "  ok   $2"; else echo "  FAIL $2"; fails=$((fails+1)); fi; }

# run <env lines...> -- <program args...>: one program with <cache dir>/env = the env lines;
# $rc its status, $res its last line, $new the server log lines it made
run() {
  local envs=()
  while [ "$1" != -- ]; do envs+=("$1"); shift; done
  shift
  printf '%s\n' "${envs[@]}" > "$cache/env"
  local l; l=$(wc -l < "$log")
  timeout 300 "$out/$@" >$work/run.log 2>&1
  rc=$?
  res=$(tail -1 $work/run.log)
  sleep 0.2 # the device's last log lines
  new=$(tail -n +$((l+1)) "$log")
}
lg() { echo "$new" | grep -o "$1" | tail -1; }
rss() { awk '/VmRSS/ {print $2}' /proc/$srv/status; }

start
# view_counts (MK-07)
run -- view3d_check
[ $rc = 0 ] && echo "$new" | grep -q "errata: view_counts ON (always" &&
  echo "$new" | grep -q "view_counts: a 2D_ARRAY view of a 3D image (4x4x4, 1 mips) with base layer 1, VK_REMAINING_ARRAY_LAYERS: 3 layer(s) given explicitly"
step $? "view_counts on: $res; $(lg 'view_counts: [0-9]* view(s) given explicit counts ([0-9]* of 3D[^)]*)')"
run VKBRIDGE_ERRATA=-view_counts VKBRIDGE_ERRATA_IMITATE=view_counts -- view3d_check
nfail=$(grep -c '^FAIL ' $work/run.log)
[ $rc != 0 ] && [ "$nfail" = 3 ] && grep -q '^FAIL 3D 4x4x4 as 2D array from slice 1, REMAINING layers: .*mip 0 Vvv\.' $work/run.log &&
  grep -q '^PASS 3D 4x4x4 as 2D array from slice 1, explicit 3 layers' $work/run.log
step $? "the bug imitated, fix off: $nfail cases fail as Mali's would ($(grep -o '^FAIL 3D 4x4x4 as 2D array from slice 1.*rendered: [^(]*' $work/run.log))"
run VKBRIDGE_ERRATA_IMITATE=view_counts -- view3d_check
[ $rc = 0 ] && echo "$new" | grep -q "errata: view_counts bug imitated beneath the fix" && echo "$new" | grep -q "; 0 imitated"
step $? "the bug imitated, fix on: $res; $(lg '[0-9]* imitated')"
run VKBRIDGE_ERRATA=-view_counts -- view3d_check
[ $rc = 0 ]
step $? "fix off, no imitation (the desktop driver's own REMAINING): $res"

# view_quarantine (MK-06)
run VKBRIDGE_FAKE_DRIVER_VERSION=r44p1 VKBRIDGE_ERRATA_IMITATE=view_quarantine -- viewchurn_check 500
[ $rc != 0 ] && ! echo "$new" | grep -q "errata: view_quarantine ON" && echo "$new" | grep -q "a view handle value came back on another image"
step $? "the bug imitated, r44p1 (off): $res; $(lg '[0-9]* stale hit(s) imitated')"
run VKBRIDGE_FAKE_DRIVER_VERSION=v1.r49p1-03bet0 VKBRIDGE_ERRATA_IMITATE=view_quarantine -- viewchurn_check 500
[ $rc = 0 ] && echo "$new" | grep -q "errata: view_quarantine ON (r49p1 in r49p1-r49p2" && echo "$new" | grep -q "; 0 stale hit(s) imitated" &&
  echo "$new" | grep -q "view_quarantine: 500 view(s) held back"
step $? "the bug imitated, r49p1 beta (on): $res; $(lg 'view_quarantine: [0-9]* view(s) held back')"
run VKBRIDGE_FAKE_DRIVER_VERSION=r49p1 VKBRIDGE_ERRATA_IMITATE=view_quarantine -- viewchurn_check 10000
r0=$(rss); ok=$rc
held=$(lg 'at most [0-9]* held (of [0-9]*)')
run VKBRIDGE_FAKE_DRIVER_VERSION=r49p1 VKBRIDGE_ERRATA_IMITATE=view_quarantine -- viewchurn_check 10000 --images
ok=$((ok|rc)); r1=$(rss)
defer=$(lg 'image(s) deferred, at most [0-9]* at once (of [0-9]*)')
run VKBRIDGE_FAKE_DRIVER_VERSION=r49p1 -- viewchurn_check 10000 --images
ok=$((ok|rc)); r2=$(rss)
run VKBRIDGE_FAKE_DRIVER_VERSION=r49p1 -- viewchurn_check 10000
ok=$((ok|rc)); r3=$(rss)
[ $ok = 0 ] && [ "$held" = "at most 4096 held (of 4096)" ] && echo "$defer" | grep -qE "at most [0-9]+ at once \(of 1024\)" &&
  [ "$(echo "$defer" | grep -oE 'most [0-9]+' | grep -oE '[0-9]+')" -le 1024 ] && [ $((r3 - r0)) -lt 8192 ]
step $? "10000 iterations x 4: $held, $defer; server RSS ${r0} -> ${r1} -> ${r2} -> ${r3} KB ($(tail -1 $work/run.log))"
run VKBRIDGE_FAKE_DRIVER_VERSION=r49p1 VKBRIDGE_VIEW_QUARANTINE=64 -- viewchurn_check 2000 --images
held=$(lg 'at most [0-9]* held (of 64)' | grep -oE 'most [0-9]+' | grep -oE '[0-9]+')
defer=$(lg 'at most [0-9]* at once (of 16)' | grep -oE 'most [0-9]+' | grep -oE '[0-9]+')
[ $rc = 0 ] && [ -n "$held" ] && [ "$held" -le 64 ] && [ -n "$defer" ] && [ "$defer" -le 16 ] && [ "$defer" -ge 8 ]
step $? "VKBRIDGE_VIEW_QUARANTINE=64: at most $held views held (of 64), $defer images (of 16) ($res)"
# the other checks with the quarantine on: results unchanged
for t in vkbridge-smoke recycle_check nulldesc_check view3d_check; do
  run VKBRIDGE_ERRATA=+view_quarantine -- $t
  [ $rc = 0 ] && echo "$new" | grep -q "errata: view_quarantine ON (VKBRIDGE_ERRATA=+view_quarantine[;)]"
  step $? "$t with +view_quarantine: $res"
done

grep -q "^#define VKB_PROTOCOL_HASH 0x4a9da939fdf65896ULL" ../common/gen/vkb_gen.h
step $? "protocol hash $(grep -o '0x[0-9a-f]*' ../common/gen/vkb_gen.h | head -1)"

if [ $fails = 0 ]; then echo "OK: $steps steps"; else echo "FAILED: $fails of $steps steps"; exit 1; fi
