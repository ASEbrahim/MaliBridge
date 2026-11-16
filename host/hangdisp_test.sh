#!/bin/bash
# The GPU hang detector's report for a stuck DISPATCH (server_hang.c): The Witcher 3 on the G925 hangs in a
# light-culling compute shader whose loop count is read from a vkd3d-proton bindless CBV (a storage buffer picked by a
# push-constant root table index); the report gave push constants and descriptor slots for draws only. With a server of
# its own, VKBRIDGE_HANG_DETECT=1 and VKBRIDGE_HANG_AFTER_MS=100 in <cache dir>/env, hangdisp_check runs 3 submissions of
# a ~1 s dispatch (tests/hangdisp_check.c); the report must name:
#   1  the dispatch (4 x 4 x 1) with its compute shader's module hash
#   2  its compute push constants as u32 words (0:7 4:5)
#   3  the compute sets bound
#   4  the buffer descriptor the shader read: pc u32 at 4 = 5, + 3 -> set 1 binding 2 [8], offset 256 range 2048
#      (128 vec4s), the buffer's size 4096
#   5  the check's own result as the CPU's (the detector changes nothing it runs)
#   6  VKBRIDGE_HANG_SUBST=1 swaps a module by the hash of the code the driver gets (hang-subst/h-<hash>.spv)
# GPU: VKB_HOST_ICD (lavapipe while the owner plays). Prints one line per step and "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
work=$(mktemp -d)
cache=$work/c
mkdir -p "$cache"
sock=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-hangdisp-$$.sock
log=$work/server.log
printf '%s\n' VKBRIDGE_HANG_DETECT=1 VKBRIDGE_HANG_AFTER_MS=100 > "$cache/env"
env -u VKBRIDGE_FAKE_MISSING -u VKBRIDGE_HIDE_EXTS -u VKBRIDGE_ERRATA -u VKBRIDGE_ERRATA_IMITATE -u VKBRIDGE_MODE \
    -u VKBRIDGE_SKIP_SHADERS VK_DRIVER_FILES=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json} VK_ICD_FILENAMES= \
    "$out/vkbridge-server" --socket "$sock" --log "$log" --cache-dir "$cache" >/dev/null 2>&1 &
srv=$!
trap 'kill $srv 2>/dev/null; rm -f "$sock"; [ -n "$KEEP" ] || rm -rf "$work"' EXIT
for _ in $(seq 50); do [ -S "$sock" ] && break; sleep 0.1; done
fails=0 steps=0
step() { steps=$((steps+1)); if [ "$1" = 0 ]; then echo "  ok   $2"; else echo "  FAIL $2"; fails=$((fails+1)); fi; }

VKBRIDGE_SOCKET=$sock VK_DRIVER_FILES=$out/vkbridge_icd.json VK_ICD_FILENAMES= timeout 120 "$out/hangdisp_check" >"$work/run.log" 2>&1
rc=$?
res=$(tail -1 "$work/run.log")
sleep 0.5
grep -q "GPU hang detector on" "$log" || echo "  (the server log has no 'GPU hang detector on' line)"
hash=$(python3 -c '
import struct, re, sys
words = [int(x, 16) for x in re.findall(r"0x[0-9a-fA-F]+", open(sys.argv[1]).read())]
h = 0xcbf29ce484222325
for b in struct.pack("<%dI" % len(words), *words):
    h = ((h ^ b) * 0x100000001b3) & 0xffffffffffffffff
print("%016x" % h)' ../tests/hangdisp_comp.inc)
grep -qE "HANG   dispatch 4 x 4 x 1, pipeline [0-9a-f]+ cs=h-$hash" "$log"
step $? "1 the stuck dispatch named: 4 x 4 x 1, cs=h-$hash"
grep -qE "HANG     compute push constants \(8 B, u32\): 0:7 4:5$" "$log"
step $? "2 its compute push constants: 0:7 4:5"
grep -qE "HANG     compute sets bound: [0-9a-f]+ [0-9a-f]+ 0 0" "$log"
step $? "3 the compute sets bound"
grep -qE "HANG       pc u32 at 4 = 5, \+ 3: set 1 binding 2 \[8\] = buffer [0-9a-f]+ offset 256 range 2048 \(128 vec4s\); buffer size 4096 usage 0x20" "$log"
step $? "4 the CBV it read: pc u32 at 4 = 5 + 3 -> set 1 binding 2 [8], offset 256 range 2048, buffer size 4096"
[ $rc = 0 ] && echo "$res" | grep -q '^PASS'
step $? "5 the check's result as the CPU's ($res)"
# 6 VKBRIDGE_HANG_SUBST=1: hang-subst/h-<hash>.spv (the no-loop shader, tests/hangdisp_noloop.comp) goes to the driver in place of the
#   module of that hash: the server says so, the dispatch is no longer reported stuck, and the check sees the substitute's value (o[0] = 7)
kill $srv 2>/dev/null; wait $srv 2>/dev/null; rm -f "$sock"
mkdir -p "$cache/hang-subst"
python3 -c '
import re, struct, sys
w = [int(x, 16) for x in re.findall(r"0x[0-9a-fA-F]+", open(sys.argv[1]).read())]
open(sys.argv[2], "wb").write(struct.pack("<%dI" % len(w), *w))' ../tests/hangdisp_noloop_comp.inc "$cache/hang-subst/h-$hash.spv"
printf '%s\n' VKBRIDGE_HANG_DETECT=1 VKBRIDGE_HANG_AFTER_MS=100 VKBRIDGE_HANG_SUBST=1 > "$cache/env"
l0=$(wc -l < "$log")
env -u VKBRIDGE_FAKE_MISSING -u VKBRIDGE_HIDE_EXTS -u VKBRIDGE_ERRATA -u VKBRIDGE_ERRATA_IMITATE -u VKBRIDGE_MODE \
    -u VKBRIDGE_SKIP_SHADERS VK_DRIVER_FILES=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json} VK_ICD_FILENAMES= \
    "$out/vkbridge-server" --socket "$sock" --log "$log" --cache-dir "$cache" >/dev/null 2>&1 &
srv=$!
for _ in $(seq 50); do [ -S "$sock" ] && break; sleep 0.1; done
VKBRIDGE_SOCKET=$sock VK_DRIVER_FILES=$out/vkbridge_icd.json VK_ICD_FILENAMES= timeout 120 "$out/hangdisp_check" >"$work/run6.log" 2>&1
res6=$(tail -1 "$work/run6.log")
sleep 0.5
new6=$(tail -n +$((l0+1)) "$log")
echo "$new6" | grep -q "HANG subst: module h-$hash ([0-9]* bytes) replaced by hang-subst/h-$hash.spv" && \
  ! echo "$new6" | grep -qE "HANG   dispatch 4 x 4 x 1" && echo "$res6" | grep -q "o\[0\] = 0x7, want"
step $? "6 VKBRIDGE_HANG_SUBST: the module replaced by hang-subst/h-$hash.spv, no stuck dispatch, the substitute's value ($res6)"
if [ $fails != 0 ]; then
  echo "  server HANG lines:"; grep -E "HANG" "$log" | head -20 | sed 's/^/    /'
fi
if [ $fails = 0 ]; then echo "OK: a stuck dispatch reported with its push constants and the CBV it read ($steps steps)"; else echo "FAILED: $fails of $steps"; exit 1; fi
