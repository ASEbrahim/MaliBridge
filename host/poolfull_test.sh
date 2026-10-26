#!/bin/bash
# Descriptor pools that are simply full vs sets no pool can hold (a server of its own):
#   poolfull_check passes (2048 sets a round, the oversized sets refused on fresh and grown pools),
#   and the client's log has exactly one info line for the full pools and no warning for them,
#   and one warning for each of the three oversized sets.
# Prints one line per step and "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
work=$(mktemp -d)
sock=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-poolfull-$$.sock
env -u VKBRIDGE_FAKE_MISSING -u VKBRIDGE_HIDE_EXTS -u VKBRIDGE_MODE \
    VK_DRIVER_FILES=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json} VK_ICD_FILENAMES= \
    "$out/vkbridge-server" --socket "$sock" --log "$work/server.log" --cache-dir "$work/c" >/dev/null 2>&1 &
srv=$!
for _ in $(seq 50); do [ -S "$sock" ] && break; sleep 0.1; done
trap 'kill $srv 2>/dev/null; rm -f "$sock"; [ -n "$KEEP" ] || rm -rf "$work"' EXIT
export VKBRIDGE_SOCKET=$sock VK_DRIVER_FILES=$out/vkbridge_icd.json VK_ICD_FILENAMES=
unset VKBRIDGE_HIDE_EXTS VKBRIDGE_MAX_API VKBRIDGE_SYNC
fails=0 steps=0
step() { steps=$((steps+1)); if [ "$1" = 0 ]; then echo "  ok   $2"; else echo "  FAIL $2"; fails=$((fails+1)); fi; }

VKBRIDGE_LOG_LEVEL=2 timeout 60 "$out/poolfull_check" >$work/out.log 2>$work/client.log
step $? "poolfull_check: $(tail -1 $work/out.log)"
full=$(grep -c "descriptor pool full after growing" $work/client.log)
step $([ "$full" = 1 ]; echo $?) "one info line for the full pools ($full)"
warned=$(grep -c "vkAllocateDescriptorSets refused" $work/client.log)
step $([ "$warned" = 3 ]; echo $?) "a warning for each oversized set, none for the full pools ($warned)"

if [ $fails = 0 ]; then echo "OK: $steps steps"; else echo "FAILED: $fails of $steps steps"; fi
[ $fails = 0 ]
