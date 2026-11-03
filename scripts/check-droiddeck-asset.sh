#!/bin/bash
# Checks a malibridge-droiddeck.tar.zst the way the app that bundles it does:
#   scripts/check-droiddeck-asset.sh <asset.tar.zst> [tag]
# Layout (regular files only, nothing outside the allow-list), the manifest and its hashes, and the ELF files:
# the guest files need only libc and the loader, the ICD exports the three vk_icd entry points, and no
# glibc symbol is newer than the build DroidDeck links today (ICD 2.38, self-test 2.34).
set -euo pipefail
asset=$1
want_tag=${2:-}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
fail() { echo "FAIL: $*" >&2; exit 1; }

tar --zstd -tvf "$asset" | grep -v '^[-d]' && fail "the archive has entries that are neither files nor directories"
tar --zstd -tf "$asset" | grep -E '(^|/)\.\.(/|$)|^/' && fail "path traversal in the archive"
tar --zstd -xf "$asset" -C "$tmp"

allowed='^(usr/local/lib/vkbridge/libvulkan_droidbridge\.so|usr/local/share/vkbridge/droidbridge_icd\.json|usr/local/bin/droiddeck-vkbridge-smoke|usr/local/share/licenses/malibridge/(LICENSE|NOTICE|THIRD-PARTY)|android/arm64-v8a/libvkbridge_(server|jni)\.so|MALIBRIDGE\.json)$'
required='usr/local/lib/vkbridge/libvulkan_droidbridge.so usr/local/share/vkbridge/droidbridge_icd.json usr/local/bin/droiddeck-vkbridge-smoke usr/local/share/licenses/malibridge/LICENSE usr/local/share/licenses/malibridge/NOTICE android/arm64-v8a/libvkbridge_server.so android/arm64-v8a/libvkbridge_jni.so MALIBRIDGE.json'
(cd "$tmp" && find . -type f | sed 's|^\./||' | sort) > "$tmp.files"
bad=$(grep -Ev "$allowed" "$tmp.files" || true)
[ -z "$bad" ] || fail "files outside the allow-list: $bad"
for f in $required; do grep -qx "$f" "$tmp.files" || fail "missing $f"; done
rm -f "$tmp.files"
[ -z "$(find "$tmp" -type l)" ] || fail "symlink in the archive"

cd "$tmp"
python3 -I - "$want_tag" <<'PY'
import hashlib, json, os, sys
m = json.load(open("MALIBRIDGE.json"))
want = sys.argv[1]
assert isinstance(m["interface"], int), "interface"
if want:
    assert m["tag"] == want, f"manifest tag {m['tag']} != {want}"
assert len(m["commit"]) == 40, "commit"
names = set()
for root, _, fs in os.walk("."):
    for f in fs:
        p = os.path.relpath(os.path.join(root, f), ".")
        if p != "MALIBRIDGE.json":
            names.add(p)
assert names == set(m["files"]), f"manifest files differ: {names ^ set(m['files'])}"
for p, h in m["files"].items():
    assert hashlib.sha256(open(p, "rb").read()).hexdigest() == h, f"hash of {p}"
print("manifest ok:", m["tag"], m["commit"][:12], "interface", m["interface"])
PY

icd=usr/local/lib/vkbridge/libvulkan_droidbridge.so
smoke=usr/local/bin/droiddeck-vkbridge-smoke
for f in $icd $smoke; do
    needed=$(readelf -d "$f" | sed -n 's/.*Shared library: \[\(.*\)\]/\1/p' | sort | tr '\n' ' ')
    [ "$needed" = "ld-linux-aarch64.so.1 libc.so.6 " ] || fail "$f needs: $needed"
    readelf -h "$f" | grep -q 'Machine:.*AArch64' || fail "$f is not aarch64"
done
n=$(readelf --dyn-syms -W $icd | awk '$7!="UND" && $8 ~ /^vk_icd/' | wc -l)
[ "$n" = 3 ] || fail "the ICD exports $n vk_icd* symbols, expected 3"
maxglibc() { readelf --dyn-syms -W "$1" | grep -o 'GLIBC_[0-9.]*' | sed 's/GLIBC_//' | sort -uV | tail -1; }
atmost() { [ "$(printf '%s\n%s\n' "$1" "$2" | sort -V | tail -1)" = "$2" ]; }
g=$(maxglibc $icd);   atmost "$g" 2.38 || fail "the ICD needs GLIBC_$g (limit 2.38)"
s=$(maxglibc $smoke); atmost "$s" 2.34 || fail "the self-test needs GLIBC_$s (limit 2.34)"
for f in android/arm64-v8a/libvkbridge_server.so android/arm64-v8a/libvkbridge_jni.so; do
    readelf -h "$f" | grep -q 'Machine:.*AArch64' || fail "$f is not aarch64"
done
readelf -d android/arm64-v8a/libvkbridge_jni.so | grep -q 'SONAME.*libvkbridge_jni.so' || fail "jni soname"
echo "elf ok: ICD GLIBC_$g, self-test GLIBC_$s, 3 vk_icd exports"
