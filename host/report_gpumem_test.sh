#!/bin/bash
# The session report's GPU memory per process (server_memory.c's per-process table) in a crowded
# session, with a server of its own. A Steam session keeps eight Vulkan processes holding memory
# before a game starts; the table had eight places, so the game's report line read 0 MB. Here ten
# programs hold 8 MB each at once:
#   1  every one of them has GPU memory in the report (malibridge-report.json gpu_mem_peak_mb >= 8);
#   2  the report's text says so too ("GPU memory peak 8 MB" or more, no "GPU memory peak 0 MB").
# Prints one line per step and "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
work=$(mktemp -d)
sess=$work/s
mkdir -p "$sess"
sock=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-gpumem-$$.sock
env VKBRIDGE_PROBE=0 VK_DRIVER_FILES=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json} VK_ICD_FILENAMES= \
    "$out/vkbridge-server" --socket "$sock" --log "$sess/vkbridge-server.log" --cache-dir "$work/c" >/dev/null 2>&1 &
srv=$!
for _ in $(seq 50); do [ -S "$sock" ] && break; sleep 0.1; done
progs=()
trap 'kill $srv "${progs[@]}" 2>/dev/null; rm -f "$sock"; [ -n "$KEEP" ] && echo "kept $work" || rm -rf "$work"' EXIT
export VKBRIDGE_SOCKET=$sock VK_DRIVER_FILES=$out/vkbridge_icd.json VK_ICD_FILENAMES=
fails=0 steps=0
step() { steps=$((steps+1)); if [ "$1" = 0 ]; then echo "  ok   $2"; else echo "  FAIL $2"; fails=$((fails+1)); fi; }

for i in $(seq 10); do
    "$out/pcache_check" --count 1 --first "$i" --mem 8 --hold 12 >"$work/p$i.log" 2>&1 &
    progs+=($!)
done
sleep 9   # samples every 2 s; the report is rewritten meanwhile
counts=$(python3 -I - "$sess/malibridge-report.json" <<'PY'
import json, sys
try:
    d = json.load(open(sys.argv[1]))
except Exception:
    print("0 0"); sys.exit()
mine = [p for p in d.get("processes", []) if p.get("comm") == "pcache_check"]
print(len(mine), sum(1 for p in mine if p.get("gpu_mem_peak_mb", 0) >= 8))
PY
)
read -r seen with <<<"$counts"
step $([ "$seen" = 10 ] && [ "$with" = 10 ]; echo $?) "1 $with of $seen programs (10 run) have their 8 MB in the report"
zero=$(grep -c "GPU memory peak 0 MB" "$sess/malibridge-report.md" 2>/dev/null)
some=$(grep -c "GPU memory peak \([89]\|[1-9][0-9]\+\) MB" "$sess/malibridge-report.md" 2>/dev/null)
step $([ "${zero:-0}" = 0 ] && [ "${some:-0}" -ge 10 ]; echo $?) "2 report text: ${some:-0} lines with 8 MB or more, ${zero:-0} with 0 MB"
wait "${progs[@]}" 2>/dev/null; progs=()

if [ $fails = 0 ]; then echo "OK: $steps steps"; else echo "FAILED: $fails of $steps steps"; fi
[ $fails = 0 ]
