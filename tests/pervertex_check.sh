#!/bin/bash
# Per-vertex block rewrite checks on the corpus (pervertex_check.c): the shaders as captured (*.spv)
# and the hand-written cases (pervertex_*.spvasm, assembled here) - rewritten, structurally right,
# valid. Then, with a Vulkan GPU, DRG's geometry shader drawn (pervertex_pipeline.c) natively as
# captured, natively rewritten, and through a host bridge whose server rewrites it
# (../host, built with make). spirv-as/spirv-val from PATH, else from a docker image with
# Ubuntu's spirv-tools (built on first use).
#   pervertex_check.sh [outdir]
set -eu
cd "$(dirname "$0")"
out=${1:-$(mktemp -d)}
mkdir -p "$out"
out=$(cd "$out" && pwd)
if command -v spirv-val >/dev/null; then
    run=
else
    docker image inspect vkb-spirv-tools >/dev/null 2>&1 ||
        printf 'FROM ubuntu:24.04\nRUN apt-get update && apt-get install -y --no-install-recommends spirv-tools\n' |
        docker build -q -t vkb-spirv-tools - >/dev/null
    run="docker run --rm -u $(id -u):$(id -g) -v $PWD/corpus:$PWD/corpus:ro -v $out:$out vkb-spirv-tools"
fi
srv=../server
cc -O1 -Wall -Wextra -o "$out/pervertex_check" pervertex_check.c $srv/spirv_pervertex.c $srv/spirv_edit.c
for s in corpus/*.spvasm; do
    $run spirv-as --target-env vulkan1.1 -o "$out/$(basename "$s" .spvasm).spv" "$PWD/$s"
done
# Geometry/tessellation shaders only (corpus/ultrakill_* are vertex/fragment: clip_check.sh).
cases=("$PWD"/corpus/drg_*_gs.spv "$out"/pervertex_*.spv)
# The inputs must be valid already: a failure after the rewrite is then the rewrite's.
for f in "${cases[@]}"; do $run spirv-val --target-env vulkan1.3 "$f"; done
PERVERTEX_VAL="${run:+$run }spirv-val --target-env vulkan1.3" "$out/pervertex_check" "$out" "${cases[@]}"

[ "${PERVERTEX_NO_GPU:-}" ] && exit 0
cc -O1 -Wall -Wextra -Wno-missing-field-initializers -I../third_party/Vulkan-Headers-1.4.341/include \
    -o "$out/pervertex_pipeline" pervertex_pipeline.c -ldl
vs=$out/drg_dxvk_vs.spv fs=$out/drg_dxvk_fs.spv gs=$PWD/corpus/drg_dxvk_gs.spv
draw() { echo -n "== $1: "; "$out/pervertex_pipeline" "$vs" "$2" "$fs"; }
draw "native, as captured" "$gs"
draw "native, rewritten" "$out/rewritten_drg_dxvk_gs.spv"
if [ -x ../host/out/vkbridge-server ]; then
    (
        export VKBRIDGE_SPIRV_FIX=1
        : > ../host/out/server.log
        . ../host/mali_env.sh >/dev/null
        r=0; draw "bridge (mali_env), server rewrite" "$gs" || r=1
        grep -E "per-vertex|loose built-ins" ../host/out/server.log || true
        vkb_server_stop
        exit $r
    )
fi
