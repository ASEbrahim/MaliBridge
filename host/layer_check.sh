#!/bin/bash
# The layer (VkLayer_malibridge_compat) with a real app on the host.
#
#   host/layer_check.sh [full|quick] [layer-build-dir]
#
#   full   (the default) everything, about a minute
#   quick  the fast parts only, for iterating on the layer: the BCn emulation (the repo's
#          bcn_check, which is the slow part) is left out, the rest is the same
#
# What it checks:
#   1. the layer loads (the loader's own words, and the layer's own log)
#   2. vkcube renders with it, in two modes: the host driver as it is, and with Mali's gaps
#      imitated (VKBRIDGE_FAKE_MISSING: no BCn, no clip/cull distance, no divisor, no
#      maintenance5, no push descriptors, no robustness2, 500,000 descriptors per stage) -
#      the layer's whole point: the emulated features, the shader rewriting, the BC decoder
#   3. the BCn, the push descriptor and the clip distance emulations (the last two in the SPIR-V
#      the driver is given, the first through the repo's own bcn_check)
#   4. the app is shown the layer's capability model: the emulated extensions offered, the
#      descriptor limits raised, and the extensions the clip distance emulation takes away
#
# The WSI is the session's (wayland, else xcb). A display is needed for the vkcube checks; without
# one they are skipped and the capability ones still run.
cd "$(dirname "$0")"
mode=full
case ${1:-} in
    quick) mode=quick; shift ;;
    full) shift ;;
esac
build=${1:-$PWD/../build-linux-x86-64}
out=$PWD/out/layer
mkdir -p "$out"
pass=0; fail=0; skip=0
result() { if [ "$1" = 0 ]; then pass=$((pass+1)); echo "  ok   $2"; else fail=$((fail+1)); echo "  FAIL $2"; fi; }
skipped() { skip=$((skip+1)); echo "  skip $1"; }

lib=$build/libVkLayer_malibridge_compat.so
if [ ! -f "$lib" ]; then
  echo "no layer at $lib; build it first:" >&2
  echo "  cmake -B build-linux-x86-64 -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_BUILD_TYPE=Release && cmake --build build-linux-x86-64" >&2
  exit 1
fi

# The manifest needs an absolute library_path: the loader does not resolve a relative one against
# the manifest's own directory, and the installed one (lib/vkbridge) is not in its search path.
manifest=$out/VkLayer_malibridge_compat.json
cat > "$manifest" <<EOF
{
    "file_format_version": "1.1.2",
    "layer": {
        "name": "VK_LAYER_MALIBRIDGE_compat",
        "type": "GLOBAL",
        "library_path": "$lib",
        "api_version": "1.3.278",
        "implementation_version": "1",
        "description": "MaliBridge Compatibility & Errata Layer for Arm Mali"
    }
}
EOF

export VK_LAYER_PATH=$out
export VK_INSTANCE_LAYERS=VK_LAYER_MALIBRIDGE_compat
# Mali's gaps, as the host tests imitate them (host/mali_env.sh).
mali=bc,clip,cull,divisor,maint5,depthclip,push,robustness2,dualsrc,fillmode,multiviewport,maint6,desclimits,opaquefd

# A minimal app: the layer must be in its path, and the identity it is shown must be the layer's.
inc=-I../third_party/Vulkan-Headers-1.4.341/include
cc -O1 -I"$inc" -o out/layer-app ../tests/layer_app.c -lvulkan 2>/dev/null || {
  echo "cannot build out/layer-app (the Vulkan headers?)" >&2; exit 1; }

# The WSI: the session's wayland, else X.
wsi=""
if [ -n "$WAYLAND_DISPLAY" ] && [ -S "$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY" ]; then wsi=wayland
elif [ -n "$DISPLAY" ]; then wsi=xcb
fi

echo "== the layer loads"
# The smoke test, not vkcube: it needs no display (the CI has none).
VK_LOADER_DEBUG=layer timeout 120 ./out/vkbridge-smoke >out/layer-load.log 2>&1
grep -q 'Insert instance layer "VK_LAYER_MALIBRIDGE_compat"' out/layer-load.log
result $? "the loader inserted the layer"
grep -q "errata: device .*safe mode" out/layer-load.log
result $? "the layer's errata machinery ran"

if [ -z "$wsi" ]; then
  skipped "no display: the vkcube checks need one (wayland or X)"
else
  echo "== vkcube ($wsi)"
  timeout 120 vkcube --wsi "$wsi" --c 10 >out/layer-plain.log 2>&1
  result $? "vkcube without the layer (the baseline)"
  timeout 120 vkcube --wsi "$wsi" --c 10 >out/layer-on.log 2>&1
  result $? "vkcube with the layer"
  grep -q "errata: device .*safe mode" out/layer-on.log
  result $? "the layer was in the path (its log)"

  echo "== vkcube with Mali's gaps imitated"
  VKBRIDGE_FAKE_MISSING=$mali timeout 120 vkcube --wsi "$wsi" --c 10 >out/layer-mali.log 2>&1
  result $? "vkcube with the layer, the GPU taken as a Mali-G720"
  grep -q "BC emulation: BC1 UNORM decodes to" out/layer-mali.log
  result $? "the BC decoder is in place"
  grep -q "descriptor limits emulated" out/layer-mali.log
  result $? "the descriptor limits are emulated"
fi

echo "== the BCn emulation"
if [ "$mode" = quick ]; then
  skipped "the BCn emulation (the repo's bcn_check: the slow part, in the full run)"
else
  # The GPU is taken as having no BCn (as a Mali): the layer decodes the blocks. The repo's own
  # BCn test runs through the layer: every format's texels must match bcdec's CPU decode.
  VKBRIDGE_FAKE_MISSING=bc timeout 900 ./out/bcn_check >out/layer-bcn.log 2>&1
  result $? "bcn_check through the layer ($(tail -1 out/layer-bcn.log))"
  grep -q "texels match bcdec" out/layer-bcn.log
  result $? "the decoded texels match bcdec's"
  grep -q "BC -> BC copy" out/layer-bcn.log
  result $? "the BCn shapes (mip chains, layers, sub-regions, copies) went through"
fi

echo "== the push descriptor emulation"
# The GPU is taken as having no push descriptors (as a Mali-G720): the layer turns a pushed set
# into an ordinary one.
# The repo's own smoke test first: the pushed values must reach the shader, by writes and by a
# template, with a null buffer too. (Its "descriptor pool growth" check is the bridge's own: the
# server grows its pool when the client asks for more sets than it has. The layer has no server
# to grow, and the driver does not either: that check fails natively too.)
VKBRIDGE_FAKE_MISSING=push timeout 300 ./out/vkbridge-smoke >out/layer-smoke.log 2>&1
grep -q "^PASS push descriptors" out/layer-smoke.log
result $? "the pushed values reach the shader ($(grep -m1 'push descriptors' out/layer-smoke.log | cut -c6-))"

echo "== the clip distance emulation"
# The GPU is taken as lacking clip/cull distances (as a Mali-G720): the layer rewrites the shaders
# (the clip becomes a discard in the fragment shader).
if ! command -v spirv-as >/dev/null; then
  skipped "no spirv-as: the clip distance shaders cannot be assembled"
else
  cc -O1 -I"$inc" -o out/clip_pipeline ../tests/clip_pipeline.c -ldl 2>/dev/null || {
    echo "cannot build out/clip_pipeline" >&2; exit 1; }
  cc -O1 -I"$inc" -I../server -o out/clip_check ../tests/clip_check.c ../server/spirv_clip.c \
      ../server/spirv_pervertex.c ../server/spirv_edit.c 2>/dev/null || {
    echo "cannot build out/clip_check" >&2; exit 1; }
  mkdir -p out/clip out/clip-mods
  for s in clip_vs_block clip_vs_whole clip_gs_block clip_fs; do
    spirv-as --target-env vulkan1.1 -o "out/clip/$s.spv" "../tests/corpus/$s.spvasm"
  done
  # The baseline: the driver itself clips.
  env -u VK_INSTANCE_LAYERS timeout 60 ./out/clip_pipeline out/clip/clip_vs_block.spv - out/clip/clip_fs.spv >out/layer-clip-native.log 2>&1
  result $? "natively, the driver clips the quad's left half"
  # The layer, the GPU taken as a Mali-G720: the rewritten shaders must clip.
  rm -f out/clip-mods/*.spv
  VKBRIDGE_FAKE_MISSING=clip,cull VKBRIDGE_DUMP_SPIRV=out/clip-mods timeout 120 \
      ./out/clip_pipeline out/clip/clip_vs_block.spv - out/clip/clip_fs.spv >out/layer-clip.log 2>&1
  result $? "with the layer, the clip distance still clips ($(tail -1 out/layer-clip.log))"
  VKBRIDGE_FAKE_MISSING=clip,cull timeout 120 \
      ./out/clip_pipeline out/clip/clip_vs_block.spv out/clip/clip_gs_block.spv out/clip/clip_fs.spv >out/layer-clip-gs.log 2>&1
  result $? "with the layer, through a geometry shader ($(tail -1 out/layer-clip-gs.log))"
  # The modules the layer gave the driver: no clip/cull built-in left, and the discard.
  if ls out/clip-mods/*.spv >/dev/null 2>&1; then
    ./out/clip_check --scan out/clip-mods/*.spv >out/layer-clip-scan.log 2>&1
    result $? "the modules the layer gave the driver carry no clip/cull built-in ($(tail -1 out/layer-clip-scan.log))"
  else
    skipped "the layer dumped no module: the shaders were not rewritten"
  fi
fi

echo "== the errata"
# The layer runs the driver's errata in-process: the same decisions as the bridge's
# (host/errata_test.sh), from the process's environment.
timeout 600 ./layer_errata_test.sh >out/layer-errata.log 2>&1
result $? "the errata decisions by driver version and mode ($(tail -1 out/layer-errata.log))"
! grep -q "^  FAIL" out/layer-errata.log
result $? "every errata step passed"

echo "== what the app is shown"
# The GPU identity the app is shown: the layer's, not the driver's.
VKBRIDGE_SPOOF_GPU=nvidia timeout 60 ./out/layer-app >out/layer-app.log 2>&1
result $? "a minimal app with the layer (the loader must not take the layer for itself)"
grep -q "vendorID 0x10de" out/layer-app.log
result $? "the app is shown the spoofed GPU (VKBRIDGE_SPOOF_GPU=nvidia)"

# The device-local heap the app is shown is capped (VKBRIDGE_HEAP_MAX_MB), the budget with it.
VKBRIDGE_HEAP_MAX_MB=1024 timeout 120 vulkaninfo >out/layer-info-heap.txt 2>&1
awk '/memoryHeaps\[0\]/,/memoryHeaps\[1\]/' out/layer-info-heap.txt | grep -q "size *= *1073741824"
result $? "the device-local heap is capped to 1024 MB (VKBRIDGE_HEAP_MAX_MB)"

VKBRIDGE_FAKE_MISSING=$mali timeout 180 vulkaninfo >out/layer-info-mali.txt 2>&1
# The emulated extensions are the layer's own (the loader lists them as layer-device extensions).
for e in VK_EXT_robustness2 VK_KHR_maintenance5 VK_KHR_maintenance6 VK_KHR_push_descriptor \
         VK_EXT_vertex_attribute_divisor VK_EXT_depth_clip_enable; do
  grep -q "$e *: extension revision" out/layer-info-mali.txt
  result $? "$e is offered"
done
# The clip distance is emulated: the shaders are rewritten together, so a split pipeline library
# and the shader identifiers are taken away (the client ICD does the same).
! grep -q "VK_EXT_graphics_pipeline_library *: extension revision" out/layer-info-mali.txt
result $? "VK_EXT_graphics_pipeline_library is not offered (the clip distance is emulated)"
! grep -q "VK_EXT_shader_module_identifier *: extension revision" out/layer-info-mali.txt
result $? "VK_EXT_shader_module_identifier is not offered (the clip distance is emulated)"
# The descriptor limits the app is shown are the emulated ones (common/vkb_caps.c: 1 << 20).
grep -q "maxPerStageDescriptorSampledImages *= *1048576" out/layer-info-mali.txt
result $? "the descriptor limits are raised to 1048576"

echo "== $pass passed, $fail failed, $skip skipped"
[ "$fail" = 0 ]
