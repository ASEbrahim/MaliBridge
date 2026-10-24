#!/bin/bash
# Compiles the wave32 corpus (host/wave32_test.sh) and the check's shader (../wave32_comp.inc).
# Needs glslc (PATH, or the Android NDK's shader-tools).
set -eu
cd "$(dirname "$0")"
glslc=$(command -v glslc || ls -d ~/Android/Sdk/ndk/*/shader-tools/linux-x86_64/glslc | tail -1)
for f in allops wg32 divballot adapts anysize wg48 waterfall; do "$glslc" -O0 --target-env=vulkan1.1 -fshader-stage=comp $f.comp -o $f.spv; done
"$glslc" -O0 --target-env=vulkan1.2 -fshader-stage=comp vmm.comp -o vmm.spv
"$glslc" -O --target-env=vulkan1.1 -fshader-stage=comp ../shaders/wave32.comp -o wave32_opt.spv
"$glslc" -O --target-env=vulkan1.3 -fshader-stage=comp allops.comp -o allops_spv16.spv
"$glslc" -O0 --target-env=vulkan1.1 -fshader-stage=comp ../shaders/wave32.comp -mfmt=num -o ../wave32_comp.inc
