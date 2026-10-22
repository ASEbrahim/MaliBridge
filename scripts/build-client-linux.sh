#!/bin/bash
# Builds the Linux side of the bridge for the guest (glibc, aarch64):
#   scripts/build-client-linux.sh <dest-root> [cc]
# Produces, under <dest-root> (a staging tree copied into the guest's filesystem):
#   usr/local/lib/vkbridge/libvulkan_droidbridge.so   the Vulkan ICD
#   usr/local/share/vkbridge/droidbridge_icd.json     its manifest
#   usr/local/bin/droiddeck-vkbridge-smoke            the self-test (tests/smoke.c)
set -euo pipefail
d=$1
cc=${2:-aarch64-linux-gnu-gcc}
strip=${STRIP:-${cc%gcc}strip}
root=$(cd "$(dirname "$0")/.." && pwd)
inc="-I$root/third_party/Vulkan-Headers-1.4.341/include -I$root/common -I$root/common/gen"
mkdir -p "$d/usr/local/lib/vkbridge" "$d/usr/local/share/vkbridge" "$d/usr/local/bin"
"$cc" -O2 -fPIC -shared -fvisibility=hidden -DVKB_CLIENT -Wall -Wno-unused-parameter -Wno-missing-field-initializers \
    $inc -I"$root/client" -I"$root/client/compat" -pthread \
    -Wl,--no-undefined -Wl,-z,nodelete -Wl,-soname,libvulkan_droidbridge.so \
    -o "$d/usr/local/lib/vkbridge/libvulkan_droidbridge.so" \
    "$root"/client/*.c "$root/common/vkb_wire.c" "$root/common/vkb_caps.c" "$root/common/gen/vkb_gen_structs.c" "$root/common/gen/vkb_gen_client.c" -ldl
"$strip" --strip-unneeded "$d/usr/local/lib/vkbridge/libvulkan_droidbridge.so"
"$cc" -O2 -Wall $inc -I"$root/tests" -o "$d/usr/local/bin/droiddeck-vkbridge-smoke" "$root/tests/smoke.c" -ldl
"$strip" --strip-unneeded "$d/usr/local/bin/droiddeck-vkbridge-smoke"
cat > "$d/usr/local/share/vkbridge/droidbridge_icd.json" <<JSON
{"file_format_version": "1.0.0", "ICD": {"library_path": "/usr/local/lib/vkbridge/libvulkan_droidbridge.so", "api_version": "1.4.341"}}
JSON
echo "built the vkbridge client into $d"
