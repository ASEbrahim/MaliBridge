#!/bin/bash
# Clip/cull distance demotion checks (clip_check.c): ULTRAKILL's Unity vertex shader (ClipDistance
# in gl_PerVertex, no ClipDistance capability; crashed Mali's compiler), the per-vertex corpus
# after the fold, and the hand-written clip_*.spvasm cases - rewritten, no clip/cull built-in
# left, valid. Then, with a Vulkan GPU, a quad whose left half a clip plane removes, drawn
# natively and through a host bridge imitating Mali (clip/cull emulated): VS block, VS whole
# stores, VS -> GS block, VS -> loose GS (folded) (clip_pipeline.c). spirv-as/spirv-val from
# PATH, else from the vkb-spirv-tools docker image (see pervertex_check.sh).
#   clip_check.sh [outdir]
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
cc -O1 -Wall -Wextra -o "$out/clip_check" clip_check.c $srv/spirv_clip.c $srv/spirv_pervertex.c $srv/spirv_edit.c
for s in corpus/clip_*.spvasm corpus/pervertex_gs_whole.spvasm; do
    $run spirv-as --target-env vulkan1.1 -o "$out/$(basename "$s" .spvasm).spv" "$PWD/$s"
done
cases=("$PWD"/corpus/ultrakill_unity_vs_clipblock.spv "$PWD"/corpus/drg_dxvk_gs.spv "$out"/pervertex_gs_whole.spv "$out"/clip_*.spv)
# The inputs must be valid already: a failure after the rewrite is then the rewrite's.
for f in "${cases[@]}"; do $run spirv-val --target-env vulkan1.3 "$f"; done
CLIP_VAL="${run:+$run }spirv-val --target-env vulkan1.3" "$out/clip_check" "$out" "${cases[@]}"

[ "${CLIP_NO_GPU:-}" ] && exit 0
cc -O1 -Wall -Wextra -Wno-missing-field-initializers -I../third_party/Vulkan-Headers-1.4.341/include \
    -o "$out/clip_pipeline" clip_pipeline.c -ldl
fs=$out/clip_fs.spv
draws() {
    local r=0
    for d in "VS block:$out/clip_vs_block.spv:-" "VS whole stores:$out/clip_vs_whole.spv:-" \
             "VS block -> GS block:$out/clip_vs_block.spv:$out/clip_gs_block.spv" \
             "VS block -> loose GS:$out/clip_vs_block.spv:$out/pervertex_gs_whole.spv"; do
        IFS=: read -r name vs gs <<<"$d"
        echo -n "== $1, $name: "
        "$out/clip_pipeline" "$vs" "$gs" "$fs" || r=1
    done
    return $r
}
draws native
srvdir=../host/out
if [ -x $srvdir/vkbridge-server ]; then
    # A private server (its own socket and log): the shared one may be in use.
    # The client sees the features (emulated), and RADV would clip anyway: the modules the server
    # gives the driver (VKBRIDGE_DUMP_SPIRV) must carry no clip/cull built-in, and the discard.
    sock=$out/vkb-clip.sock log=$out/server-clip.log dump=$out/driver-modules
    rm -rf "$sock" "$dump"
    mkdir -p "$dump"
    VKBRIDGE_SPIRV_FIX=1 VKBRIDGE_FAKE_MISSING=clip,cull VKBRIDGE_DUMP_SPIRV=$dump \
        VK_DRIVER_FILES=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json} \
        "$srvdir/vkbridge-server" --socket "$sock" --log "$log" >/dev/null 2>&1 &
    pid=$!
    for _ in $(seq 50); do [ -S "$sock" ] && break; sleep 0.1; done
    r=0
    VKBRIDGE_SOCKET=$sock VK_DRIVER_FILES=$(cd $srvdir && pwd)/vkbridge_icd.json draws "bridge (clip,cull emulated)" || r=1
    kill $pid 2>/dev/null
    grep -E "emulation|per-vertex" "$log" || true
    "$out/clip_check" --scan "$dump"/*.spv || r=1
    exit $r
fi
