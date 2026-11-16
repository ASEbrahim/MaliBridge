#!/bin/bash
# The black box (test 10): the server writes one 256-byte text record a second into
# vkbridge-blackbox.bin next to its log (a ring of 256), so a killed server or a restarted phone leaves its last seconds.
#   1  while a program holds a device: whole records, the newest under 1.5 s old, the program named in one;
#   2  SIGKILL: the last record is at most 1.1 s older than the kill;
#   3  no fsync/fdatasync in the report code, the write after the report lock is let go, the ring bounded at 64 KB.
# Prints one line per step and "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
work=$(mktemp -d)
sess=$work/s
mkdir -p "$sess"
sock=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-blackbox-$$.sock
env VKBRIDGE_PROBE=0 VK_DRIVER_FILES=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json} VK_ICD_FILENAMES= \
    "$out/vkbridge-server" --socket "$sock" --log "$sess/vkbridge-server.log" --cache-dir "$work/c" >/dev/null 2>&1 &
srv=$!
for _ in $(seq 50); do [ -S "$sock" ] && break; sleep 0.1; done
prog=
trap 'kill $srv $prog 2>/dev/null; rm -f "$sock"; [ -n "$KEEP" ] && echo "kept $work" || rm -rf "$work"' EXIT
export VKBRIDGE_SOCKET=$sock VK_DRIVER_FILES=$out/vkbridge_icd.json VK_ICD_FILENAMES=
fails=0 steps=0
step() { steps=$((steps+1)); if [ "$1" = 0 ]; then echo "  ok   $2"; else echo "  FAIL $2"; fails=$((fails+1)); fi; }
check() { python3 -I - "$sess/vkbridge-blackbox.bin" "$@" <<'PY'
import sys, time
path, mode = sys.argv[1], sys.argv[2]
try:
    data = open(path, "rb").read()
except OSError as e:
    print("1 no ring: %s" % e); sys.exit()
recs = [data[i:i + 256] for i in range(0, len(data), 256)]
good = [r for r in recs if len(r) == 256 and r.endswith(b"\n") and r[:1].isdigit()]
walls = sorted(int(r.split()[0]) for r in good)
if mode == "live":
    now = time.time() * 1000
    named = sum(1 for r in good if b"pcache_check" in r)
    ok = len(data) % 256 == 0 and len(data) <= 65536 and len(good) >= 3 and now - walls[-1] < 1500 and named >= 1
    print("%d %d records, newest %.0f ms old, %d naming pcache_check" % (0 if ok else 1, len(good), now - walls[-1] if walls else -1, named))
else:
    killed = int(sys.argv[3])
    gap = killed - walls[-1] if walls else 10 ** 9
    print("%d last record %d ms before the kill" % (0 if gap <= 1100 else 1, gap))
PY
}
"$out/pcache_check" --count 1 --first 1 --mem 8 --hold 10 >"$work/p.log" 2>&1 &
prog=$!
sleep 4.5
read -r rc text <<<"$(check live)"
step "$rc" "1 $text"
sleep 0.$((RANDOM % 9 + 1))
kill -9 $srv
killed=$(date +%s%3N)
wait $srv 2>/dev/null
read -r rc text <<<"$(check killed "$killed")"
step "$rc" "2 $text"
src=../server/server_report.c
syncs=$(grep -c 'fsync\|fdatasync' $src)
order=$(python3 -I - "$src" <<'PY'
import sys
s = open(sys.argv[1]).read()
tick = s[s.index("static void bb_tick(void)"):]
tick = tick[:tick.index("\n}\n")]
rec = s[s.index("static int bb_record(char *rec)"):]
rec = rec[:rec.index("\n}\n")]
ok = "rlock" not in tick and tick.index("bb_record(rec)") < tick.index("pwrite(") and rec.rindex("pthread_mutex_unlock(&rlock)") < rec.rindex("return BB_REC")
print(0 if ok else 1)
PY
)
step $([ "$syncs" = 0 ] && [ "$order" = 0 ]; echo $?) "3 no sync call in the report code ($syncs), the write outside the report lock ($order = 0)"

if [ $fails = 0 ]; then echo "OK: $steps steps"; else echo "FAILED: $fails of $steps steps"; fi
[ $fails = 0 ]
