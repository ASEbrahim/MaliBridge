#!/bin/bash
# pv_view3d_base (PV-04, PowerVR DXT 25.1: a 2D-array view of a 3D image from slice 1 on renders into slices
# 0..n-1, explicit counts too; server_errata_view3d.c, the probe view3d_layers in server_probe.c) on the desktop
# driver, with a server of its own per step (its own socket and cache directory; the switches go in <cache dir>/env).
# pv3d_check renders through such views (render passes, a secondary command buffer, an imageless framebuffer,
# dynamic rendering; loadOp CLEAR and LOAD; partial render areas; a mip level; layered draws whose slice gl_Layer picks,
# a framebuffer of fewer layers than the view, multiview with a hole in the mask; reads of the 3D image inside the pass:
# a feedback loop after a barrier, a later subpass sampling the first's) and reads every texel back, also sampled.
#   1  as it is: the probe passes, the fix is off, every case passes;
#   2  the bug imitated (VKBRIDGE_ERRATA_IMITATE=pv_view3d_base: views of two or more slices start at slice 0),
#      the fix forced off: the probe sees the pattern and says which forms it proved, the multi-slice cases fail,
#      the one-slice and from-slice-0 ones pass;
#   3  the same, nothing forced (safe and performance mode): the fix turns on, every case passes, the multi-slice
#      views go through the copy form (no alias);
#   3c the in-pass reads split their passes (feedback loop, later subpass), none left unsplit; with the splits off
#      (VKBRIDGE_PV3D_NOSPLIT=1) exactly those cases fail;
#   3b under the PowerVR identity (VKBRIDGE_FAKE_DRIVER=img): the same;
#   4  VKBRIDGE_PV3D_IMITATE=array (one-slice 2D-array views broken too): fix off: those fail as well; fix on: all
#      pass, one-slice array views through a 2D view of the slice (alias), multi-slice ones copied;
#   5  VKBRIDGE_PV3D_IMITATE=all (2D views of a slice too): fix off: everything from slice 1 on fails; fix on: all
#      pass, one-slice views copied as well (no alias);
#   6  the fix forced on over a passing probe (the copy form for every view): every case passes;
#   7  another bug's pattern (view_counts' imitation, explicit counts right): pv_view3d_base stays off;
#   8  PowerVR CXTP 25.3's other form (VKBRIDGE_PV3D_IMITATE=fb: the base layer is right, feedback loops into a 3D image's
#      2D-array view read what the image held before the pass): the probe passes its other sub-cases and fails the feedback
#      loop one (the plain 2D-array control and the copy form right), fix off: exactly the feedback-loop cases fail; fix on
#      (safe and performance mode): all pass, only the feedback-loop passes copied (no plain pass is), none via the other
#      view type; the generic form of the check's loop passes is in the corpus counted by name;
#   9  the multiview rule (VKBRIDGE_FAKE_DRIVER=img, the probe passing, no imitation): the fix is on for PowerVR whatever the
#      probe says, the multiview passes (render pass and dynamic rendering) are copied and no other pass is, every case right;
#      the rule is logged once; off the PowerVR identity nothing is on;
#   9b both together (feedback-loop imitation under the PowerVR identity): feedback-loop and multiview passes copied;
#   11 pv_fbloop_split (PV-06, PowerVR CXTP 25.3: a feedback loop reads wrong in any image, right with the pass ended at the barrier and
#      begun again with LOAD): VKBRIDGE_ERRATA_IMITATE=pv_fbloop_split makes loops into plain images read stale; the probe's plain
#      loop (sub-case fb_control) fails and its split form (fb_split) is right, flag 0x80000000; fix off: exactly the check's plain
#      loop cases fail; on (safe and performance mode, also under the PowerVR identity): all pass but the barrier in a secondary
#      command buffer (the documented limit), only the passes with a feedback-loop layout split (9), warned once; the fix forced on
#      over a passing probe is right without the imitation; the PowerVR identity alone and no imitation leave it off; with PV-04's
#      loop imitation (step 8) also on, PV-04's splits are the same as there;
#   10 the protocol hash is unchanged.
# Prints one line per step and "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
work=$(mktemp -d)
cache=$work/c
mkdir -p "$cache"
sock=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-pv3d-$$.sock
log=$work/server.log
srv=
start() { # start [env...]: a server of its own on a fresh cache directory (a fresh probe profile)
  [ -n "$srv" ] && kill $srv 2>/dev/null && wait $srv 2>/dev/null
  rm -f "$sock"
  rm -rf "$cache"; mkdir -p "$cache"
  : > "$log"
  env -u VKBRIDGE_FAKE_MISSING -u VKBRIDGE_HIDE_EXTS -u VKBRIDGE_ERRATA -u VKBRIDGE_ERRATA_IMITATE -u VKBRIDGE_MODE \
      -u VKBRIDGE_FAKE_DRIVER_VERSION -u VKBRIDGE_ERRATA_PROBE -u VKBRIDGE_PROBE_FAKE -u VKBRIDGE_PV3D_IMITATE -u VKBRIDGE_PV3D_NOSPLIT \
      VKBRIDGE_PROBE_BUDGET_S=0 "$@" VK_DRIVER_FILES=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json} VK_ICD_FILENAMES= \
      "$out/vkbridge-server" --socket "$sock" --log "$log" --cache-dir "$cache" >/dev/null 2>&1 &
  srv=$!
  for _ in $(seq 100); do [ -S "$sock" ] && break; sleep 0.1; done
}
trap 'kill $srv 2>/dev/null; rm -f "$sock"; [ -n "$KEEP" ] || rm -rf "$work"' EXIT
export VKBRIDGE_SOCKET=$sock VK_DRIVER_FILES=$out/vkbridge_icd.json VK_ICD_FILENAMES=
unset VKBRIDGE_HIDE_EXTS VKBRIDGE_MAX_API
fails=0 steps=0
step() { [ -n "$DBG" ] && [ "$1" != 0 ] && echo "$new" | grep -i "pv_view3d\|errata: device" | cut -c1-300; steps=$((steps+1)); if [ "$1" = 0 ]; then echo "  ok   $2"; else echo "  FAIL $2"; fails=$((fails+1)); fi; }
run() { # run [env lines] -- -- env: $new the log lines it added, $res its last output line, $rc its status
  local envs=()
  while [ "$1" != -- ]; do envs+=("$1"); shift; done
  printf '%s\n' "${envs[@]}" > "$cache/env"
  local l; l=$(wc -l < "$log")
  timeout 120 "$out/pv3d_check" >$work/run.log 2>&1
  rc=$?
  sleep 0.3
  res=$(tail -1 $work/run.log)
  new=$(tail -n +$((l+1)) "$log")
  nfail=$(grep -c '^FAIL ' $work/run.log)
  # the device's closing line: views through the other view type, through a 2D-array image of their own
  alias=$(echo "$new" | sed -n 's/.*pv_view3d_base: \([0-9]*\) view(s) rendered through a 2D view.*their slice, \([0-9]*\) through.*/\1/p' | tail -1)
  shadow=$(echo "$new" | sed -n 's/.*pv_view3d_base: \([0-9]*\) view(s) rendered through a 2D view.*their slice, \([0-9]*\) through.*/\2/p' | tail -1)
  splits=$(echo "$new" | sed -n 's/.*pv_view3d_base: .* \([0-9]*\) split(s) for reads inside a pass, \([0-9]*\) not split.*/\1/p' | tail -1)
  unsplit=$(echo "$new" | sed -n 's/.*pv_view3d_base: .* \([0-9]*\) split(s) for reads inside a pass, \([0-9]*\) not split.*/\2/p' | tail -1)
}
# no one-slice and no from-slice-0 case fails (lines of the check carry "one-slice", "2D view" and "from 0")
single_ok() { ! grep '^FAIL ' $work/run.log | grep -q "one-slice\|2D view\|from 0"; }

# 1
start
run VKBRIDGE_MODE=safe --
if echo "$res" | grep -q "no physical device"; then echo "OK: skipped"; exit 0; fi
if echo "$res" | grep -q "^OK: skipped"; then echo "OK: skipped ($(head -1 $work/run.log))"; exit 0; fi
[ $rc = 0 ] && grep -q "probe: view3d_layers  *pass" "$log" && ! echo "$new" | grep -q "pv_view3d_base:" &&
  echo "$new" | grep -q "errata: device .*off .*pv_view3d_base"
step $? "as it is: probe passes, fix off, every case right ($res)"
# 2
start VKBRIDGE_ERRATA_IMITATE=pv_view3d_base
run VKBRIDGE_ERRATA=-pv_view3d_base --
[ $rc != 0 ] && [ "$nfail" -ge 11 ] && single_ok &&
  grep -q "probe: view3d_layers fail (VK_REMAINING_ARRAY_LAYERS: slices WWW- (want -WWW); explicit count 3 (view_counts' form): slices WWW- (want -WWW); one-slice 2D-array view: slices -W-- (want -W--); 2D view of slice 1: slices -W-- (want -W--); 3 slices through a copied image: slices -WWW (want -WWW) (W cleared by the view, - untouched); feedback loop: 3D view ????, copied -bbb, 2D array bbb, 2D array split bbb (want -bbb -bbb bbb bbb: b read and swapped) - the explicit count is wrong too: view_counts does not fix it here - pv_view3d_base (PV-04): one-slice views need nothing; multi-slice views are copied" "$log" &&
  echo "$new" | grep -q "errata: pv_view3d_base OFF (VKBRIDGE_ERRATA=-pv_view3d_base; it would be on: probe view3d_layers failed"
step $? "the bug imitated, fix off: the probe sees the pattern and the forms ($(grep -o 'pv_view3d_base (PV-04): [^"]*' "$log" | head -1)); $nfail multi-slice cases fail, the one-slice ones pass"
# 3
for mode in safe performance; do
  start VKBRIDGE_ERRATA_IMITATE=pv_view3d_base
  run VKBRIDGE_MODE=$mode --
  [ $rc = 0 ] && echo "$new" | grep -q "errata: pv_view3d_base ON (probe view3d_layers failed; $mode mode) - PV-04" &&
    echo "$new" | grep -q "errata: pv_view3d_base: forms proven by the probe: one-slice 2D-array view keeps its base, 2D view of a slice keeps its base, copy through a 2D-array image right; one-slice views from slice 1 on: kept as they are (the driver keeps their base layer); multi-slice views from slice 1 on: rendered through a 2D-array image of their own, copied in before and out after each pass - PV-04" &&
    [ "${alias:-x}" = 0 ] && [ "${shadow:-0}" -ge 8 ]
  step $? "the bug imitated, the probe's failure turns the fix on ($mode mode): every case right; $alias via the other view type, $shadow copied ($res)"
done
# 3c the reads inside a pass (a feedback loop, a later subpass) are what the splits are for; without them exactly those fail
start VKBRIDGE_ERRATA_IMITATE=pv_view3d_base
run VKBRIDGE_MODE=safe --
[ $rc = 0 ] && [ "${splits:-0}" -ge 4 ] && [ "${unsplit:-x}" = 0 ] &&
  echo "$new" | grep -q "pv_view3d_base: a pass rendering a 3D image through a copy, with the attachment in a layout shaders may read it in, is split"
step $? "reads of the 3D image inside a pass (feedback loop, a later subpass): $splits split(s), $unsplit not split, every case right ($res)"
run VKBRIDGE_MODE=safe VKBRIDGE_PV3D_NOSPLIT=1 --
nf=$(grep '^FAIL ' $work/run.log | wc -l)
[ $rc != 0 ] && [ "$nf" -ge 4 ] && ! grep '^FAIL ' $work/run.log | grep -qv "feedback loop\|subpass 1 samples" &&
  grep '^FAIL ' $work/run.log | grep -q "subpass 1 samples" && echo "$new" | grep -q "was not split at a .*(VKBRIDGE_PV3D_NOSPLIT)"
step $? "the same with the splits off (VKBRIDGE_PV3D_NOSPLIT=1): exactly the $nf in-pass read cases fail (stale slices), the rest pass"
# 3b
start VKBRIDGE_ERRATA_IMITATE=pv_view3d_base VKBRIDGE_FAKE_DRIVER=img
run VKBRIDGE_MODE=safe --
[ $rc = 0 ] && echo "$new" | grep -q "errata: pv_view3d_base ON (probe view3d_layers failed; safe mode) - PV-04" && ! echo "$new" | grep -q "errata: [a-z_]* ON (r[0-9]*p[0-9]*" &&
  [ "${shadow:-0}" -ge 8 ]
step $? "PowerVR identity (VKBRIDGE_FAKE_DRIVER=img): the same, no version gate with it ($res)"
# 4
start VKBRIDGE_ERRATA_IMITATE=pv_view3d_base VKBRIDGE_PV3D_IMITATE=array
run VKBRIDGE_ERRATA=-pv_view3d_base VKBRIDGE_PV3D_IMITATE=array --
off4=$nfail
[ $rc != 0 ] && grep '^FAIL ' $work/run.log | grep -q "pass: one-slice 2D_ARRAY view of slice 3" && grep '^PASS ' $work/run.log | grep -q "pass: 2D view of slice 4" &&
  grep -q "one-slice 2D-array view: slices W--- (want -W--); 2D view of slice 1: slices -W-- (want -W--); 3 slices through a copied image: slices -WWW (want -WWW) (W cleared by the view, - untouched); feedback loop: 3D view ????, copied -bbb, 2D array bbb, 2D array split bbb (want -bbb -bbb bbb bbb: b read and swapped) - the explicit count is wrong too: view_counts does not fix it here - pv_view3d_base (PV-04): one-slice views are rendered through a 2D view of the slice; multi-slice views are copied" "$log"
step $? "one-slice 2D-array views broken too, fix off: $off4 cases fail, the probe says one-slice views go through a 2D view"
run VKBRIDGE_MODE=safe VKBRIDGE_PV3D_IMITATE=array --
[ $rc = 0 ] && echo "$new" | grep -q "errata: pv_view3d_base: a 2D_ARRAY view of a 3D image from slice [0-9]* (1 slice) is rendered through a 2D view of the same slice" &&
  echo "$new" | grep -q "one-slice 2D-array view ignores it, 2D view of a slice keeps its base, copy through a 2D-array image right; one-slice views from slice 1 on: rendered through a 2D view of the slice" &&
  [ "${alias:-0}" -ge 3 ] && [ "${shadow:-0}" -ge 8 ]
step $? "the same, fix on: every case right; $alias one-slice views through a 2D view of the slice, $shadow copied ($res)"
# 5
start VKBRIDGE_ERRATA_IMITATE=pv_view3d_base VKBRIDGE_PV3D_IMITATE=all
run VKBRIDGE_ERRATA=-pv_view3d_base VKBRIDGE_PV3D_IMITATE=all --
off5=$nfail
[ $rc != 0 ] && grep '^FAIL ' $work/run.log | grep -q "pass: 2D view of slice 4" && grep '^PASS ' $work/run.log | grep -q "pass: 3 slices from 0" &&
  grep -q "one-slice 2D-array view: slices W--- (want -W--); 2D view of slice 1: slices W--- (want -W--); 3 slices through a copied image: slices -WWW (want -WWW) (W cleared by the view, - untouched); feedback loop: 3D view ????, copied -bbb, 2D array bbb, 2D array split bbb (want -bbb -bbb bbb bbb: b read and swapped) - the explicit count is wrong too: view_counts does not fix it here - pv_view3d_base (PV-04): one-slice views are copied; multi-slice views are copied" "$log"
step $? "2D views broken too, fix off: $off5 cases fail, the probe says one-slice views are copied"
run VKBRIDGE_MODE=safe VKBRIDGE_PV3D_IMITATE=all --
[ $rc = 0 ] && [ "${alias:-x}" = 0 ] && [ "${shadow:-0}" -ge 14 ] &&
  echo "$new" | grep -q "one-slice 2D-array view ignores it, 2D view of a slice ignores it, copy through a 2D-array image right; one-slice views from slice 1 on: rendered through a 2D-array image of their own"
step $? "the same, fix on: every case right; $alias via the other view type, $shadow copied ($res)"
# 6
start
run VKBRIDGE_ERRATA=+pv_view3d_base --
[ $rc = 0 ] && echo "$new" | grep -q "errata: pv_view3d_base ON (VKBRIDGE_ERRATA=+pv_view3d_base; probe view3d_layers passed) - PV-04" &&
  echo "$new" | grep -q "copy through a 2D-array image right (assumed: VKBRIDGE_ERRATA=+pv_view3d_base)" && [ "${alias:-x}" = 0 ] && [ "${shadow:-0}" -ge 14 ]
step $? "the fix forced on over a passing probe: the copy form for every view, every case right; $shadow copied ($res)"
# 7
start VKBRIDGE_ERRATA_IMITATE=view_counts
run VKBRIDGE_MODE=performance --
[ $rc = 0 ] && grep -q "probe: view3d_layers fail" "$log" && ! grep -q "pv_view3d_base (PV-04)" "$log" && ! echo "$new" | grep -q "pv_view3d_base ON" &&
  echo "$new" | grep -q "errata: device .*off .*pv_view3d_base"
step $? "view_counts' pattern (explicit counts right): pv_view3d_base stays off ($res)"
# 8 CXTP 25.3's form: feedback loops into a 3D image's 2D-array view read stale slices, the base layer is right
fbl_cases=$(grep -c "feedback loop" ../tests/pv3d_check.c)
start VKBRIDGE_ERRATA_IMITATE=pv_view3d_base VKBRIDGE_PV3D_IMITATE=fb
run VKBRIDGE_ERRATA=-pv_view3d_base VKBRIDGE_PV3D_IMITATE=fb --
nf8=$nfail
[ $rc != 0 ] && [ "$nf8" -ge 4 ] && ! grep '^FAIL ' $work/run.log | grep -qv "feedback loop" && grep '^PASS ' $work/run.log | grep -q "subpass 1 samples" &&
  grep '^PASS ' $work/run.log | grep -q "pass: 3 slices from 2, loadOp CLEAR" &&
  grep -q "W probe: view3d_layers fail (VK_REMAINING_ARRAY_LAYERS: slices -WWW (want -WWW); explicit count 3 (view_counts' form): slices -WWW (want -WWW); one-slice 2D-array view: slices -W-- (want -W--); 2D view of slice 1: slices -W-- (want -W--); 3 slices through a copied image: slices -WWW (want -WWW) (W cleared by the view, - untouched); feedback loop: 3D view -???, copied -bbb, 2D array bbb, 2D array split bbb (want -bbb -bbb bbb bbb: b read and swapped) - pv_view3d_base (PV-04): feedback loops in such views are copied" "$log" &&
  echo "$new" | grep -q "errata: pv_view3d_base OFF (VKBRIDGE_ERRATA=-pv_view3d_base; it would be on: probe view3d_layers failed"
step $? "feedback loops wrong in 3D-image views (VKBRIDGE_PV3D_IMITATE=fb), fix off: the probe passes its base-layer sub-cases and sees the loop ($(grep -o 'feedback loop: 3D view [^(]*' "$log" | head -1)); $nf8 loop cases fail, nothing else does"
for mode in safe performance; do
  start VKBRIDGE_ERRATA_IMITATE=pv_view3d_base VKBRIDGE_PV3D_IMITATE=fb
  run VKBRIDGE_MODE=$mode VKBRIDGE_PV3D_IMITATE=fb --
  redir=$(echo "$new" | sed -n 's/.*pv_view3d_base: .* \([0-9]*\) render pass(es) and \([0-9]*\) dynamic rendering pass(es) redirected.*/\1+\2/p' | tail -1)
  mvfb=$(echo "$new" | sed -n 's/.*imitated; \([0-9]*\) multiview pass(es) and \([0-9]*\) feedback-loop pass(es) among them.*/\1 \2/p' | tail -1)
  [ $rc = 0 ] && echo "$new" | grep -q "errata: pv_view3d_base ON (probe view3d_layers failed; $mode mode) - PV-04" &&
    echo "$new" | grep -q "errata: pv_view3d_base: feedback loops (a colour attachment in ATTACHMENT_FEEDBACK_LOOP_OPTIMAL or RENDERING_LOCAL_READ layout) into a 2D-array view of a 3D image read wrong here and right in a plain 2D-array image and through a copy (probe view3d_layers): such passes are rendered through a 2D-array image of their own, split at their barriers - PV-04" &&
    echo "$new" | grep -q "errata: pv_view3d_base: a feedback loop into a 2D_ARRAY view of a 3D image (8x8, mip 0) from slice [0-9]*, [0-9]* slice(s), is rendered through a 2D-array image of its own" &&
    [ "$redir" = "$((nf8 - 1))+1" ] && [ "$mvfb" = "0 $nf8" ] && [ "${alias:-x}" = 0 ] && [ "${splits:-0}" -ge 4 ] && [ "${unsplit:-x}" = 0 ]
  step $? "the same, nothing forced ($mode mode): the fix turns on, every case right; only the loop passes are copied ($redir render pass + dynamic redirected, $mvfb multiview / loop passes), none via the other view type, $splits split(s) ($res)"
done
# 9 the multiview rule on PowerVR's driver: on whatever the probe says, multiview passes only
start VKBRIDGE_FAKE_DRIVER=img
run VKBRIDGE_MODE=safe --
redir=$(echo "$new" | sed -n 's/.*pv_view3d_base: .* \([0-9]*\) render pass(es) and \([0-9]*\) dynamic rendering pass(es) redirected.*/\1+\2/p' | tail -1)
mvfb=$(echo "$new" | sed -n 's/.*imitated; \([0-9]*\) multiview pass(es) and \([0-9]*\) feedback-loop pass(es) among them.*/\1 \2/p' | tail -1)
[ $rc = 0 ] && grep -q "probe: view3d_layers  *pass" "$log" &&
  echo "$new" | grep -q "errata: pv_view3d_base ON (PowerVR driver; safe mode; probe view3d_layers passed) - PV-04" &&
  echo "$new" | grep -q "errata: pv_view3d_base: PowerVR's driver: multiview (a view mask) into a 2D-array view of a 3D image faults in it (CXTP 25.3, DXT 24.3/25.1), no probe can tell: such passes take the copy form" &&
  echo "$new" | grep -q "errata: pv_view3d_base: the base-layer bug was not seen" &&
  [ "$(echo "$new" | grep -c 'errata: pv_view3d_base: multiview (view mask) into a 2D_ARRAY view of a 3D image')" = 1 ] &&
  [ "$redir" = "1+1" ] && [ "$mvfb" = "2 0" ] && [ "${alias:-x}" = 0 ] && [ "${shadow:-0}" -ge 2 ]
step $? "PowerVR identity, the probe passing: the multiview rule turns the fix on (logged once), the multiview passes are copied ($redir render pass + dynamic redirected, $mvfb multiview / loop passes) and no other, every case right ($res)"
start VKBRIDGE_FAKE_DRIVER=img
run VKBRIDGE_MODE=performance VKBRIDGE_ERRATA=-pv_view3d_base --
[ $rc = 0 ] && echo "$new" | grep -q "errata: pv_view3d_base OFF (VKBRIDGE_ERRATA=-pv_view3d_base; it would be on: PowerVR driver; performance mode" &&
  ! echo "$new" | grep -q "pv_view3d_base: multiview"
step $? "the rule is a fix like the others: VKBRIDGE_ERRATA=-pv_view3d_base turns it off ($res)"
# 9b both together
start VKBRIDGE_FAKE_DRIVER=img VKBRIDGE_ERRATA_IMITATE=pv_view3d_base VKBRIDGE_PV3D_IMITATE=fb
run VKBRIDGE_MODE=safe VKBRIDGE_PV3D_IMITATE=fb --
redir=$(echo "$new" | sed -n 's/.*pv_view3d_base: .* \([0-9]*\) render pass(es) and \([0-9]*\) dynamic rendering pass(es) redirected.*/\1+\2/p' | tail -1)
mvfb=$(echo "$new" | sed -n 's/.*imitated; \([0-9]*\) multiview pass(es) and \([0-9]*\) feedback-loop pass(es) among them.*/\1 \2/p' | tail -1)
[ $rc = 0 ] && echo "$new" | grep -q "errata: pv_view3d_base ON (probe view3d_layers failed; safe mode) - PV-04" &&
  [ "$mvfb" = "2 $nf8" ] && [ "$redir" = "$nf8+2" ]
step $? "loop imitation under the PowerVR identity: loop and multiview passes copied ($redir, $mvfb multiview / loop passes), every case right ($res)"
# 11 pv_fbloop_split (PV-06, PowerVR CXTP 25.3: feedback loops read wrong in any image, right with the pass ended at the barrier and
# begun again): the imitation (VKBRIDGE_ERRATA_IMITATE=pv_fbloop_split) makes loops into plain images with the feedback-loop usage read
# what the image held before the pass or at its last end. The check's plain cases: 6 loops (4 render passes, a 2D view, a mip, one dynamic
# rendering, the barrier in a secondary command buffer) and 3 other plain passes, one of them with a GENERAL layout and a barrier.
plain_loops=$(grep -c '^        {"plain .*feedback loop' ../tests/pv3d_check.c)
fbs_line() { echo "$new" | sed -n 's/.*pv_fbloop_split: \([0-9]*\) render pass(es) and \([0-9]*\) dynamic rendering pass(es) with an attachment in a feedback-loop layout, \([0-9]*\) split(s) at a barrier, \([0-9]*\) not split.*/\1 \2 \3 \4/p' | tail -1; }
start VKBRIDGE_ERRATA_IMITATE=pv_fbloop_split
run VKBRIDGE_ERRATA=-pv_fbloop_split --
nf11=$nfail
[ $rc != 0 ] && [ "$nf11" = "$plain_loops" ] && ! grep '^FAIL ' $work/run.log | grep -qv "^FAIL plain .*feedback loop" &&
  grep -q "probe: view3d_layers fail (.*feedback loop: 3D view -bbb, copied -bbb, 2D array ???, 2D array split bbb (want -bbb -bbb bbb bbb: b read and swapped) - pv_fbloop_split (PV-06): feedback-loop passes are split at their barriers \[the bug imitated beneath" "$log" &&
  cat $cache/probe-*.json | grep -q '"name": "view3d_layers", "status": "fail", .*"want": "0x80000000", "cases": "0x1ff", "failed": "0x80"' &&
  echo "$new" | grep -q "errata: pv_fbloop_split OFF (VKBRIDGE_ERRATA=-pv_fbloop_split; it would be on: probe view3d_layers failed" &&
  ! echo "$new" | grep -q "pv_view3d_base ON"
step $? "plain feedback loops wrong (VKBRIDGE_ERRATA_IMITATE=pv_fbloop_split), fix off: the probe's plain loop fails and its split form is right (flag 0x80000000, sub-case fb_control), exactly the $nf11 plain loop cases fail"
for mode in safe performance; do
  start VKBRIDGE_ERRATA_IMITATE=pv_fbloop_split
  run VKBRIDGE_MODE=$mode --
  nfp=$nfail
  f=($(fbs_line))
  # every pass with an attachment in a feedback-loop layout (8 render passes, 2 dynamic) is split at its barrier but the one with the
  # barrier in a secondary command buffer (the documented limit, the only case left failing); no other pass is split
  [ "$nfp" = 1 ] && grep '^FAIL ' $work/run.log | grep -q "^FAIL plain pass: feedback loop, the draws and the barrier in a secondary command buffer" &&
    echo "$new" | grep -q "errata: pv_fbloop_split ON (probe view3d_layers failed; $mode mode) - PV-06" &&
    echo "$new" | grep -q "errata: pv_fbloop_split: a pass with an attachment in a feedback-loop layout is split at a pipeline barrier: ended there and begun again" &&
    [ "$(echo "$new" | grep -c 'pv_fbloop_split: a pass with an attachment in a feedback-loop layout is split at')" = 1 ] &&
    [ "${f[0]}" = 8 ] && [ "${f[1]}" = 2 ] && [ "${f[2]}" = 9 ] && [ "${f[3]}" = 0 ]
  step $? "the same, nothing forced ($mode mode): the probe turns the fix on; ${f[0]} render pass(es) + ${f[1]} dynamic, ${f[2]} split (logged once), the secondary command buffer's barrier the one limit ($nfp failing case)"
done
start VKBRIDGE_ERRATA_IMITATE=pv_fbloop_split VKBRIDGE_FAKE_DRIVER=img
run VKBRIDGE_MODE=safe --
[ "$nfail" = 1 ] && echo "$new" | grep -q "errata: pv_fbloop_split ON (probe view3d_layers failed; safe mode) - PV-06" &&
  ! echo "$new" | grep -q "errata: [a-z_]* ON (r[0-9]*p[0-9]*"
step $? "the same under the PowerVR identity: on by the probe, no version gate"
# the fix over a passing probe, no imitation: the splits themselves are right on a driver without the bug; the one limit warned once
start
run VKBRIDGE_ERRATA=+pv_fbloop_split --
f=($(fbs_line))
[ $rc = 0 ] && echo "$new" | grep -q "errata: pv_fbloop_split ON (VKBRIDGE_ERRATA=+pv_fbloop_split; probe view3d_layers passed) - PV-06" &&
  [ "${f[0]}" = 8 ] && [ "${f[1]}" = 2 ] && [ "${f[2]}" = 9 ] && [ "${f[3]}" = 1 ] &&
  [ "$(echo "$new" | grep -c 'pv_fbloop_split: a pipeline barrier inside a secondary command buffer')" = 1 ] && ! echo "$new" | grep -q "pv_view3d_base:"
step $? "the fix forced on over a passing probe: every case right with $((f[2])) split(s) of ${f[0]}+${f[1]} passes, ${f[3]} not split (the secondary command buffer, warned once) ($res)"
# nothing changes without the imitation or the fix: (step 1) the probe passes, the fix off; a PowerVR identity alone does not turn it on
start VKBRIDGE_FAKE_DRIVER=img
run VKBRIDGE_MODE=safe --
[ $rc = 0 ] && grep -q "probe: view3d_layers  *pass" "$log" && ! echo "$new" | grep -q "pv_fbloop_split ON\|pv_fbloop_split:" &&
  echo "$new" | grep -q "errata: device .*off .*pv_fbloop_split"
step $? "no imitation, the PowerVR identity alone: the probe passes, pv_fbloop_split stays off, nothing is split ($res)"
# the loop imitation of the 3D views (step 8) with pv_fbloop_split on too: PV-04's passes are split once (its splits, copies included), the
# plain ones by pv_fbloop_split: the same 4 split(s) as step 8, 0 not split
start VKBRIDGE_ERRATA_IMITATE=pv_view3d_base VKBRIDGE_PV3D_IMITATE=fb
run VKBRIDGE_MODE=safe VKBRIDGE_PV3D_IMITATE=fb VKBRIDGE_ERRATA=+pv_fbloop_split --
f=($(fbs_line))
[ $rc = 0 ] && [ "${splits:-0}" = 4 ] && [ "${unsplit:-x}" = 0 ] && [ "${f[2]}" = 5 ] && [ "${f[3]}" = 1 ] &&
  echo "$new" | grep -q "errata: pv_view3d_base ON (probe view3d_layers failed; safe mode) - PV-04" &&
  echo "$new" | grep -q "errata: pv_fbloop_split ON (VKBRIDGE_ERRATA=+pv_fbloop_split"
step $? "PV-04's loop imitation with pv_fbloop_split on too: PV-04's passes split once ($splits split(s), $unsplit not split, as in step 8), the plain loops by pv_fbloop_split (${f[2]} split(s), ${f[3]} not split) ($res)"
# 10
grep -q "^#define VKB_PROTOCOL_HASH 0xe88f0e43c2b566b9ULL" ../common/gen/vkb_gen.h
step $? "protocol hash $(grep -o '0x[0-9a-f]*' ../common/gen/vkb_gen.h | head -1)"

if [ $fails = 0 ]; then echo "OK: $steps steps"; else echo "FAILED: $fails of $steps steps"; exit 1; fi
