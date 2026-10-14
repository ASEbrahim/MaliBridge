#!/bin/bash
# Builds the bridge server for the device (bionic, arm64-v8a, API 26) with the NDK's clang:
#   scripts/build-server-android.sh <ndk-dir> <jniLibs-out-dir>
# Two outputs under <jniLibs-out-dir>/arm64-v8a:
#   libvkbridge_server.so  a standalone executable, named like a library so that Android extracts
#                          it into the native library directory, the one place an app may exec from
#   libvkbridge_jni.so     the same server as a JNI library the app loads into its own process
set -euo pipefail
ndk=$1
out=$2/arm64-v8a
root=$(cd "$(dirname "$0")/.." && pwd)
srv=$root/server
cc=$(ls -d "$ndk"/toolchains/llvm/prebuilt/*/bin/aarch64-linux-android26-clang | head -1)
[ -x "$cc" ] || { echo "no NDK clang under $ndk" >&2; exit 1; }
mkdir -p "$out"
# the build named in the startup probe's device profile and the session report
git=$(git -C "$root" describe --always --dirty 2>/dev/null || echo unknown)
"$cc" -O2 -g0 -fPIE -pie -Wall -Wno-unused-parameter -Wno-missing-field-initializers \
    -I"$root/third_party/Vulkan-Headers-1.4.341/include" -I"$root/common" -I"$root/common/gen" -I"$srv" \
    -Wl,-z,max-page-size=16384 -Wl,--build-id=none -DVKB_GIT="\"$git\"" \
    -o "$out/libvkbridge_server.so" \
    "$srv"/*.c "$root/common/vkb_wire.c" "$root/common/gen/vkb_gen_structs.c" "$root/common/gen/vkb_gen_server.c" \
    -ldl -llog -lm
"$(dirname "$cc")/llvm-strip" --strip-unneeded "$out/libvkbridge_server.so"
# In-process is the default on the device: only there does Android hand the server the same
# driver the app gets (VkBridgeNative, server_main.c).
"$cc" -O2 -g0 -fPIC -shared -DVKB_JNI -Wall -Wno-unused-parameter -Wno-missing-field-initializers \
    -I"$root/third_party/Vulkan-Headers-1.4.341/include" -I"$root/common" -I"$root/common/gen" -I"$srv" \
    -Wl,-z,max-page-size=16384 -Wl,--build-id=none -Wl,--no-undefined -Wl,-soname,libvkbridge_jni.so -DVKB_GIT="\"$git\"" \
    -o "$out/libvkbridge_jni.so" \
    "$srv"/*.c "$root/common/vkb_wire.c" "$root/common/gen/vkb_gen_structs.c" "$root/common/gen/vkb_gen_server.c" \
    -ldl -llog -lm
"$(dirname "$cc")/llvm-strip" --strip-unneeded "$out/libvkbridge_jni.so"
echo "built $out/libvkbridge_server.so and libvkbridge_jni.so"
