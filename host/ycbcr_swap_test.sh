#!/bin/bash
# The YCbCr sampler swap in compute layouts (server_spvfix.c, VKBRIDGE_YCBCR_SWAP) on the desktop
# driver, with the refusal of Mali and Samsung's Xclipse 960 imitated beneath it
# (VKBRIDGE_FAKE_MISSING=ycbcrcompute: compute pipelines whose layout reaches the driver with
# immutable YCbCr samplers fail with VK_ERROR_UNKNOWN, as gamescope's 135 composite pipelines did
# on the Xclipse, MaliBridge#20). A server of its own per step (the imitation is read once per server):
#   1  as it is (the desktop GPU): no swap, ycbcr_compute_check builds everything, nothing logged;
#   2  imitated, VKBRIDGE_YCBCR_SWAP=0: the YCbCr pipeline refused, the plain ones built; the first
#      failure logged with its layout's one YCbCr sampler;
#   3  imitated, VKBRIDGE_YCBCR_SWAP=1: the sampler swapped, every pipeline built, no failure;
#   4  imitated, as on Mali (VKBRIDGE_SPIRV_FIX=1, no swap switch): swapped as before;
#   5  imitated, VKBRIDGE_SPIRV_FIX=1 with VKBRIDGE_YCBCR_SWAP=0: the switch wins, refused;
#   6  gamescope (docker image vkb-arch-gamescope, skipped if absent) with vkcube, imitated: its
#      "vkCreateComputePipelines failed" lines with the swap off (every composite pipeline: one
#      layout holds them all), none with it on; the server logs the first failure once.
# Prints one line per step and "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
work=$(mktemp -d)
cache=$work/c
mkdir -p "$cache"
sock=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-ycbcr-$$.sock
log=$work/server.log
srv=
stop() { [ -n "$srv" ] && kill $srv 2>/dev/null && wait $srv 2>/dev/null; srv=; }
# start <VAR=value...>: a new server with these in its environment, a fresh log
start() {
  stop
  rm -f "$sock" "$log"
  env -u VKBRIDGE_FAKE_MISSING -u VKBRIDGE_HIDE_EXTS -u VKBRIDGE_ERRATA -u VKBRIDGE_ERRATA_IMITATE -u VKBRIDGE_MODE \
      -u VKBRIDGE_SPIRV_FIX -u VKBRIDGE_YCBCR_SWAP -u VKBRIDGE_SHL_BARRIER -u VKBRIDGE_NO_CRASH_GUARD "$@" \
      VK_DRIVER_FILES=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json} VK_ICD_FILENAMES= \
      "$out/vkbridge-server" --socket "$sock" --log "$log" --cache-dir "$cache" >/dev/null 2>&1 &
  srv=$!
  for _ in $(seq 50); do [ -S "$sock" ] && break; sleep 0.1; done
}
trap 'stop; rm -f "$sock"; [ -n "$KEEP" ] || rm -rf "$work"' EXIT
export VKBRIDGE_SOCKET=$sock VK_DRIVER_FILES=$out/vkbridge_icd.json VK_ICD_FILENAMES=
unset VKBRIDGE_HIDE_EXTS VKBRIDGE_MAX_API
fails=0 steps=0
step() { steps=$((steps+1)); if [ "$1" = 0 ]; then echo "  ok   $2"; else echo "  FAIL $2"; fails=$((fails+1)); fi; }
# check [--expect-fail]: ycbcr_compute_check through the server; rc, res (its last line)
check() {
  timeout 120 "$out/ycbcr_compute_check" "$@" >$work/run.log 2>&1
  rc=$?
  res=$(tail -1 $work/run.log)
  sleep 0.2
}
failed_line() { grep -o "compute pipeline failed ([-0-9]*): layout has [0-9]* immutable YCbCr sampler(s)" "$log" | head -1; }

# 1
start
check
if [ $rc = 77 ]; then echo "SKIP: $res"; exit 0; fi
[ $rc = 0 ] && ! grep -q "YCbCr compute samplers\|immutable YCbCr samplers replaced\|compute pipeline failed" "$log"
step $? "as it is: nothing swapped, all built ($res)"
# 2
start VKBRIDGE_FAKE_MISSING=ycbcrcompute VKBRIDGE_YCBCR_SWAP=0
check --expect-fail
[ $rc = 0 ] && grep -q "imitating a driver that cannot build compute pipelines with immutable YCbCr samplers" "$log" &&
  grep -q "YCbCr compute samplers kept (VKBRIDGE_YCBCR_SWAP)" "$log" &&
  failed_line | grep -q "failed (-13): layout has 1 immutable" && [ "$(grep -c 'compute pipeline failed' "$log")" = 1 ]
step $? "imitated, swap off: refused ($res); logged: $(failed_line)"
# 3
start VKBRIDGE_FAKE_MISSING=ycbcrcompute VKBRIDGE_YCBCR_SWAP=1
check
[ $rc = 0 ] && grep -q "YCbCr compute samplers swapped (VKBRIDGE_YCBCR_SWAP)" "$log" &&
  grep -q "compute layout: immutable YCbCr samplers replaced (this GPU cannot build such pipelines)" "$log" &&
  ! grep -q "compute pipeline failed" "$log"
step $? "imitated, swap on: all built ($res)"
# 4
start VKBRIDGE_FAKE_MISSING=ycbcrcompute VKBRIDGE_SPIRV_FIX=1
check
[ $rc = 0 ] && grep -q "Mali SPIR-V fixes on, YCbCr compute samplers swapped," "$log" && ! grep -q "compute pipeline failed" "$log"
step $? "imitated, as on Mali (VKBRIDGE_SPIRV_FIX=1): swapped as before ($res)"
# 5
start VKBRIDGE_FAKE_MISSING=ycbcrcompute VKBRIDGE_SPIRV_FIX=1 VKBRIDGE_YCBCR_SWAP=0
check --expect-fail
[ $rc = 0 ] && grep -q "Mali SPIR-V fixes on, YCbCr compute samplers kept (VKBRIDGE_YCBCR_SWAP)" "$log" && [ -n "$(failed_line)" ]
step $? "imitated, VKBRIDGE_SPIRV_FIX=1 with VKBRIDGE_YCBCR_SWAP=0: refused ($res)"
# 6
if docker image inspect vkb-arch-gamescope >/dev/null 2>&1; then
  gs=()
  for swap in 0 1; do
    start VKBRIDGE_FAKE_MISSING=ycbcrcompute VKBRIDGE_YCBCR_SWAP=$swap
    VKB_DOCKER_EXTRA="--device /dev/dri/renderD128" VKBRIDGE_DRM_RENDER=226:128 timeout 120 ./gamescope_test.sh \
      "bash -c 'timeout 20 vkcube --c 60 >/dev/null 2>&1; echo vkcube exit \$?'" >$work/gs-$swap.log 2>&1
    gs+=("$(grep -c 'vkCreateComputePipelines failed' $work/gs-$swap.log) $(grep -c 'compute pipeline failed' "$log") $(grep -c 'vkcube exit 0' $work/gs-$swap.log)")
  done
  read -r f0 l0 v0 <<< "${gs[0]}"
  read -r f1 l1 v1 <<< "${gs[1]}"
  [ "$f0" -gt 0 ] && [ "$l0" = 1 ] && [ "$f1" = 0 ] && [ "$l1" = 0 ]
  step $? "gamescope + vkcube, imitated: $f0 failed compute pipelines with the swap off, $f1 with it on (vkcube finished: $v0 / $v1; shown only, it sometimes hangs nested with or without the bridge's changes)"
else
  echo "  skip gamescope (no vkb-arch-gamescope image)"
fi

if [ $fails = 0 ]; then echo "OK: $steps steps"; else echo "FAILED: $fails of $steps steps"; exit 1; fi
