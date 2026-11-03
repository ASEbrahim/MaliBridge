#!/bin/bash
# Builds the release asset DroidDeck pins: one deterministic .tar.zst with the bridge for the device and the guest.
#   scripts/package-droiddeck-asset.sh <ndk-dir> <out-dir> [source-dir]
# <source-dir> is the checkout of the release tag to build (default: the checkout this script is in). The
# script itself may come from a newer checkout than the source, so old tags can be packaged too.
# Writes <out-dir>/malibridge-droiddeck.tar.zst and .sha256.
# Needs: git (full history and tags), zstd, GNU tar, the NDK, aarch64-linux-gnu-gcc (or CC_AARCH64).
set -euo pipefail

# The contract with the app that bundles this asset. Bump it when the bridge changes anything the app
# depends on: the JNI signature of VkBridgeNative.nativeStart, the server arguments (--socket --log
# --cache-dir), the environment names the app sets, the socket or ICD paths, the compositor ABI
# (BRIDGE_PAGES_ABI) or the banner_ahb_v1 version. The app refuses an asset with another number.
INTERFACE=${MALIBRIDGE_INTERFACE:-1}

ndk=$(cd "$1" && pwd)
mkdir -p "$2"
out=$(cd "$2" && pwd)
here=$(cd "$(dirname "$0")/.." && pwd)
src=$(cd "${3:-$here}" && pwd)
cc=${CC_AARCH64:-aarch64-linux-gnu-gcc}

for f in scripts/build-server-android.sh scripts/build-client-linux.sh LICENSE NOTICE; do
    [ -e "$src/$f" ] || { echo "$src has no $f" >&2; exit 1; }
done
tag=$(git -C "$src" describe --tags --exact-match --match 'v[0-9]*' 2>/dev/null) || {
    echo "$src is not at a release tag (git describe --exact-match failed); the build names itself from the tag" >&2; exit 1; }
if [ -n "$(git -C "$src" status --porcelain --untracked-files=no)" ]; then
    echo "$src has uncommitted changes" >&2; exit 1
fi
commit=$(git -C "$src" rev-parse HEAD)
export SOURCE_DATE_EPOCH=${SOURCE_DATE_EPOCH:-$(git -C "$src" log -1 --format=%ct)}
ndkver=$(basename "$ndk")
ccver=$("$cc" -dumpfullversion)

stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
r=$stage/root
mkdir -p "$r"

# 1. the server for the device (bionic, NDK clang)
"$src/scripts/build-server-android.sh" "$ndk" "$stage/jni" >&2
mkdir -p "$r/android/arm64-v8a"
cp "$stage/jni/arm64-v8a/libvkbridge_server.so" "$stage/jni/arm64-v8a/libvkbridge_jni.so" "$r/android/arm64-v8a/"

# 2. the Vulkan ICD and the self-test for the guest (glibc, aarch64)
"$src/scripts/build-client-linux.sh" "$r" "$cc" >&2

# 3. licences
lic=$r/usr/local/share/licenses/malibridge
mkdir -p "$lic"
cp "$src/LICENSE" "$src/NOTICE" "$lic/"
[ -e "$src/CREDITS.md" ] && cp "$src/CREDITS.md" "$lic/THIRD-PARTY"

# 4. the manifest: tag, commit, interface and the hash of every file in the asset
(
    cd "$r"
    files=$(find . -type f ! -name MALIBRIDGE.json | LC_ALL=C sort | sed 's|^\./||' | while read -r f; do
        printf '    "%s": "%s",\n' "$f" "$(sha256sum "$f" | cut -d' ' -f1)"
    done | sed '$ s/,$//')
    cat > MALIBRIDGE.json <<JSON
{
  "version": "${tag#v}",
  "tag": "$tag",
  "commit": "$commit",
  "interface": $INTERFACE,
  "ndk": "$ndkver",
  "cc": "$ccver",
  "files": {
$files
  }
}
JSON
)

# 5. a deterministic archive: sorted names, fixed times and owners, no checksum in the zstd frame
# Directory entries and modes are normalised so that the result does not depend on the umask of the builder.
find "$r" -type d -exec chmod 755 {} +
find "$r" -type f -exec chmod 644 {} +
chmod 755 "$r/usr/local/bin/droiddeck-vkbridge-smoke"
tar --sort=name --mtime="@$SOURCE_DATE_EPOCH" --owner=0 --group=0 --numeric-owner --format=gnu \
    -C "$r" -cf "$stage/asset.tar" .
zstd -19 --no-check -q -f -o "$out/malibridge-droiddeck.tar.zst" "$stage/asset.tar"
(cd "$out" && sha256sum malibridge-droiddeck.tar.zst > malibridge-droiddeck.tar.zst.sha256)
echo "wrote $out/malibridge-droiddeck.tar.zst ($(stat -c %s "$out/malibridge-droiddeck.tar.zst") bytes) for $tag $commit"
