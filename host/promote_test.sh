#!/bin/bash
# Vulkan version promotion (server_promote.c, spirv_downgrade.c) on the desktop driver taken as an
# older one (VKBRIDGE_FAKE_API), with a server of its own for each step (the version is decided at
# the server's start):
#   0  a "1.1 driver" not on the tested list, by default: stays 1.1, the log says what it would offer,
#      and apps-vulkan says 1.1, promotion=off;
#   1  ... on the tested list (VKBRIDGE_PROMOTE_TESTED): offered as 1.3; promote_check 1.3 (core
#      structures, core names, no extensions enabled, SPIR-V 1.6 lowered to 1.4, dynamic
#      rendering/sync2/copy2); the steps below turn it on with VKBRIDGE_PROMOTE_API=1;
#   2  ... without VK_KHR_dynamic_rendering: offered as 1.2, and the log says what 1.3 lacks;
#      <cache dir>/apps-vulkan says 1.2 on a 1.1 driver, promotion=on (the launcher's DXVK choice);
#   3  ... VKBRIDGE_PROMOTE_API=1.2: 1.2;  4  ... VKBRIDGE_PROMOTE_API=0: 1.1, nothing installed;
#   5  a "1.2 driver": 1.3, SPIR-V 1.6 lowered to 1.5; without VK_EXT_texel_buffer_alignment the
#      1.3 texel alignments are the 1.0 limit, not 0;
#   6  the driver as it is (1.4): nothing promoted, promote_check 1.4; apps-vulkan 1.4, shaderInt64 1;
#   7  vkbridge-smoke on the "1.1 driver": 13 of 13, its timeline semaphore check run (it asks for 1.2);
#   8  the "1.1 driver" with Mali-G720's gaps imitated as well (mali_env.sh's list): promote_check 1.3
#      and vkbridge-smoke;
#   9  the lowering over tests/downgrade (SPIR-V 1.5 / 1.6 from glslc, one by hand): each module's
#      expected rewrite, every output spirv-val clean for Vulkan 1.1 + SPIR-V 1.4 (1.6 ones also to 1.5
#      for Vulkan 1.2), a second pass changing nothing.
# spirv-as/spirv-val from PATH, else from the vkb-spirv-tools docker image (built on first use).
# Prints one line per step and "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
work=$(mktemp -d)
cache=$work/c
mkdir -p "$cache"
sock=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-promote-$$.sock
log=$work/server.log
srv=
start() {
  [ -n "$srv" ] && kill $srv 2>/dev/null && wait $srv 2>/dev/null
  rm -f "$sock"
  : > "$log"
  env -u VKBRIDGE_FAKE_MISSING -u VKBRIDGE_HIDE_EXTS -u VKBRIDGE_FAKE_API -u VKBRIDGE_PROMOTE_API -u VKBRIDGE_PROMOTE_TESTED -u VKBRIDGE_MODE "$@" \
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
run() {
  timeout 120 "$@" >$work/run.log 2>&1
  rc=$?
  res=$(tail -1 $work/run.log)
  [ $rc = 0 ] || grep '^FAIL' $work/run.log | head -3 | sed 's/^/       /'
}
offered() { grep -m1 -o "offered as Vulkan 1\.[0-9] on its Vulkan 1\.[0-9] driver" "$log"; }

# 0
start VKBRIDGE_FAKE_API=1.1
run "$out/promote_check" 1.1
[ $rc = 0 ] && grep -q "RADV NAVI31) would offer Vulkan 1.3 on its Vulkan 1.1 driver (VKBRIDGE_FAKE_API); off: not tested on this driver" "$log" &&
  ! grep -q "offered as\|lowered to\|promoted to" "$log" &&
  grep -q "^api=1\.1\." "$cache/apps-vulkan" && grep -q "^driver_api=1\.1\." "$cache/apps-vulkan" && grep -qx "promotion=off" "$cache/apps-vulkan"
step $? "a 1.1 driver not tested: $(grep -m1 -o 'would offer Vulkan 1\.[0-9]' "$log"), stays 1.1, nothing lowered, apps-vulkan $(grep '^api=' "$cache/apps-vulkan") $(grep '^promotion=' "$cache/apps-vulkan"); $res"
# 1
start VKBRIDGE_FAKE_API=1.1 VKBRIDGE_PROMOTE_TESTED=Mali-G57,NAVI31
run "$out/promote_check" 1.3
[ $rc = 0 ] && grep -q "RADV NAVI31) offered as Vulkan 1.3 on its Vulkan 1.1 driver (VKBRIDGE_FAKE_API) (tested on this driver" "$log" &&
  grep -q "vkCreateDevice: promoted to Vulkan 1.3 on a 1.1 driver: 1.3, 1.2 feature structures turned into the extensions'" "$log" &&
  grep -q "promotion: SPIR-V 1.6 lowered to 1.4 (1 module(s) so far; this one: 1 extension(s) declared, 0 layer capabilities, 1 dynamic broadcast" "$log"
step $? "a 1.1 driver with the extensions: $(offered); $res"
# 2
start VKBRIDGE_FAKE_API=1.1 VKBRIDGE_PROMOTE_API=1 VKBRIDGE_FAKE_MISSING=VK_KHR_dynamic_rendering
run "$out/promote_check" 1.2
[ $rc = 0 ] && grep -q "RADV NAVI31) offered as Vulkan 1.2 on its Vulkan 1.1 driver" "$log" &&
  grep -q "RADV NAVI31): Vulkan 1.3 lacks VK_KHR_dynamic_rendering (dynamicRendering)$" "$log" &&
  grep -q "promotion: SPIR-V 1.5 lowered to 1.4" "$log"
r2=$?
for _ in $(seq 100); do [ -f "$work/malibridge-report.json" ] && grep -q '"offered_api"' "$work/malibridge-report.json" && break; sleep 0.1; done
[ $r2 = 0 ] && grep -q "^| GPU 0 to apps | Vulkan 1.2 (version promotion, server_promote.c); Vulkan 1.3 lacks VK_KHR_dynamic_rendering (dynamicRendering) |" \
  "$work/malibridge-report.md" && grep -q '"offered_api": "1.2", "promotion_lacks": "Vulkan 1.3 lacks VK_KHR_dynamic_rendering' "$work/malibridge-report.json" &&
  grep -q "^device=.*RADV NAVI31)$" "$cache/apps-vulkan" && grep -q "^api=1\.2\." "$cache/apps-vulkan" &&
  grep -q "^driver_api=1\.1\." "$cache/apps-vulkan" && grep -qx "promotion=on" "$cache/apps-vulkan" && grep -qx "shader_int64=1" "$cache/apps-vulkan" &&
  grep -q "^max_push_constants=[0-9]" "$cache/apps-vulkan" && grep -q "^server=" "$cache/apps-vulkan"
step $? "... without VK_KHR_dynamic_rendering: $(offered), \"$(grep -m1 -o 'Vulkan 1.3 lacks .*' "$log")\", in the session report and apps-vulkan too; $res"
# 3
start VKBRIDGE_FAKE_API=1.1 VKBRIDGE_PROMOTE_API=1.2
run "$out/promote_check" 1.2
[ $rc = 0 ] && grep -q "RADV NAVI31) offered as Vulkan 1.2 on its Vulkan 1.1 driver (VKBRIDGE_FAKE_API) (VKBRIDGE_PROMOTE_API" "$log"
step $? "... VKBRIDGE_PROMOTE_API=1.2: $(offered); $res"
# 4
start VKBRIDGE_FAKE_API=1.1 VKBRIDGE_PROMOTE_API=0
run "$out/promote_check" 1.1
[ $rc = 0 ] && grep -q "RADV NAVI31) stays at Vulkan 1.1 (VKBRIDGE_PROMOTE_API=0)" "$log" && ! grep -q "lowered to\|promoted to" "$log"
step $? "... VKBRIDGE_PROMOTE_API=0: stays at 1.1, nothing lowered; $res"
# 5
start VKBRIDGE_FAKE_API=1.2 VKBRIDGE_PROMOTE_API=1
run "$out/promote_check" 1.3
[ $rc = 0 ] && grep -q "RADV NAVI31) offered as Vulkan 1.3 on its Vulkan 1.2 driver" "$log" && grep -q "promotion: SPIR-V 1.6 lowered to 1.5" "$log"
step $? "a 1.2 driver: $(offered), SPIR-V 1.6 lowered to 1.5; $res"
start VKBRIDGE_FAKE_API=1.2 VKBRIDGE_PROMOTE_API=1 VKBRIDGE_FAKE_MISSING=VK_EXT_texel_buffer_alignment
run "$out/promote_check" 1.3
[ $rc = 0 ] && grep -q "RADV NAVI31) offered as Vulkan 1.3 on its Vulkan 1.2 driver" "$log" && grep -q "^PASS texel alignments" $work/run.log
step $? "... without VK_EXT_texel_buffer_alignment: $(grep -m1 -o 'PASS texel alignments.*' $work/run.log); $res"
# 6
start
run "$out/promote_check" 1.4
[ $rc = 0 ] && ! grep -q "offered as\|lowered to\|promoted to" "$log" && grep -q "^api=1\.4\." "$cache/apps-vulkan" &&
  grep -qx "shader_int64=1" "$cache/apps-vulkan" && [ "$(sed -n 's/^max_push_constants=//p' "$cache/apps-vulkan")" -ge 256 ]
step $? "the driver as it is (1.4): nothing promoted, apps-vulkan $(grep '^api=' "$cache/apps-vulkan"); $res"
# 7
start VKBRIDGE_FAKE_API=1.1 VKBRIDGE_PROMOTE_API=1
run "$out/vkbridge-smoke"
[ $rc = 0 ] && grep -q "PASS timeline semaphore" $work/run.log && ! grep -q "timeline semaphore: skipped" $work/run.log
step $? "vkbridge-smoke on a 1.1 driver: $res ($(grep -o 'PASS timeline semaphore[^(]*' $work/run.log | head -1))"
# 8 (mali_env.sh's VKBRIDGE_FAKE_MISSING and the client's hidden extensions)
start VKBRIDGE_FAKE_API=1.1 VKBRIDGE_PROMOTE_API=1 vk_require_etc2=true vk_require_astc=true \
  VKBRIDGE_FAKE_MISSING=bc,clip,cull,divisor,maint5,depthclip,push,robustness2,dualsrc,fillmode,multiviewport,maint6,desclimits,opaquefd,lazymem,dmabuflinear,planarext
export VKBRIDGE_HIDE_EXTS=VK_KHR_maintenance5,VK_KHR_push_descriptor,VK_EXT_robustness2,VK_KHR_robustness2,VK_EXT_vertex_attribute_divisor,VK_KHR_vertex_attribute_divisor,VK_EXT_depth_clip_enable,VK_KHR_maintenance6
run "$out/promote_check" 1.3
r8=$rc res8=$res
run "$out/vkbridge-smoke"
[ $r8 = 0 ] && [ $rc = 0 ] && grep -q "PASS timeline semaphore" $work/run.log
step $? "the 1.1 driver with Mali's gaps: $res8; vkbridge-smoke $res"
unset VKBRIDGE_HIDE_EXTS
kill $srv 2>/dev/null; wait $srv 2>/dev/null; srv=

# 9 the corpus
if command -v spirv-val >/dev/null && command -v spirv-as >/dev/null; then
  run_t=
else
  docker image inspect vkb-spirv-tools >/dev/null 2>&1 ||
    printf 'FROM ubuntu:24.04\nRUN apt-get update && apt-get install -y --no-install-recommends spirv-tools\n' |
    docker build -q -t vkb-spirv-tools - >/dev/null
  run_t="docker run --rm -i -u $(id -u):$(id -g) -v $work:$work -v $PWD/..:$PWD/..:ro vkb-spirv-tools"
fi
mkdir -p "$work/in" "$work/rw4" "$work/rw5"
$run_t bash -c "for s in $PWD/../tests/downgrade/*.spvasm; do b=\$(basename \$s .spvasm); spirv-as --target-env spv1.\${b##*_1} -o $work/in/\$b.spv \$s || echo \"cannot assemble \$b\"; done" >"$work/as.log" 2>&1
"$out/downgrade_corpus" 1.4 "$work/rw4" "$work"/in/*.spv >"$work/c4.log"; rc4=$?
"$out/downgrade_corpus" 1.5 "$work/rw5" "$work"/in/*_16.spv >"$work/c5.log"; rc5=$?
val=$($run_t bash -c "ok=0 bad=0; for f in $work/rw4/*.spv; do if spirv-val --target-env vulkan1.1spv1.4 \$f >/dev/null 2>$work/v.err; then ok=\$((ok+1)); else bad=\$((bad+1)); echo \"invalid: \${f##*/}: \$(head -1 $work/v.err)\" >&2; fi; done;
  for f in $work/rw5/*.spv; do if spirv-val --target-env vulkan1.2 \$f >/dev/null 2>$work/v.err; then ok=\$((ok+1)); else bad=\$((bad+1)); echo \"invalid (1.5): \${f##*/}: \$(head -1 $work/v.err)\" >&2; fi; done; echo \$ok \$bad")
read -r vok vbad <<< "$val"
c4=$work/c4.log
[ $rc4 = 0 ] && [ $rc5 = 0 ] && [ ! -s "$work/as.log" ] && [ "${vbad:-1}" = 0 ] && [ "${vok:-0}" = 9 ] &&
  grep -qx "bcast_15.spv: 1.5 -> 1.4, 1 extension(s), 0 layer capabilities, 1 broadcast(s) made shuffles" "$c4" &&
  grep -qx "di_bda_15.spv: 1.5 -> 1.4, 4 extension(s), 0 layer capabilities, 0 broadcast(s) made shuffles" "$c4" &&
  grep -qx "layer_15.spv: 1.5 -> 1.4, 1 extension(s), 2 layer capabilities, 0 broadcast(s) made shuffles" "$c4" &&
  grep -qx "demote_16.spv: 1.6 -> 1.4, 0 extension(s), 0 layer capabilities, 0 broadcast(s) made shuffles" "$c4" &&
  grep -qx "dot_16.spv: 1.6 -> 1.4, 1 extension(s), 0 layer capabilities, 0 broadcast(s) made shuffles" "$c4" &&
  grep -qx "terminate_16.spv: 1.6 -> 1.4, 1 extension(s), 0 layer capabilities, 0 broadcast(s) made shuffles" "$c4" &&
  grep -qx "plain_14.spv: at or below the target" "$c4"
step $? "the corpus: $(grep '^TOTAL' "$c4"); spirv-val: ${vok:-?} clean, ${vbad:-?} invalid$( [ -s "$work/as.log" ] && echo "; $(head -1 "$work/as.log")")"

if [ $fails = 0 ]; then echo "OK: $steps steps"; else echo "FAILED: $fails of $steps steps"; exit 1; fi
