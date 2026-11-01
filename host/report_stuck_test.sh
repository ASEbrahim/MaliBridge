#!/bin/bash
# The session report's line for a request the driver has not returned from (server_report.c,
# VKBRIDGE_REPORT_STUCK_S), and its "stuck_call" in malibridge-report.json, which the app's
# loading screen reads to say which stage stalled (Xwayland in vkGetSemaphoreFdKHR for
# 400 s, the screen said only "Waiting for Steam's first frame"). A server of its own holds one
# request of syncfd_check's "stall" step (VKBRIDGE_TEST_DELAY_CMD) for 14 s; the report is read
# while it is held and after:
#   1  a held vkQueueSubmit, 3 s limit: the report names it ("has run N s in the GPU driver"),
#      the JSON has stuck_call {call vkQueueSubmit, s >= 3, gpu_wait false};
#   2  once it has returned: no such line, stuck_call null;
#   3  a held vkDeviceWaitIdle: said as a wait for the GPU (gpu_wait true);
#   4  VKBRIDGE_REPORT_STUCK_S=0: never named.
# Prints one line per step and "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
host_icd=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json}
work=$(mktemp -d)
sock=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-stuck-$$.sock
srv= prog=
stop() {
  [ -n "$prog" ] && { kill $prog 2>/dev/null; wait $prog 2>/dev/null; }
  [ -n "$srv" ] && { kill $srv 2>/dev/null; wait $srv 2>/dev/null; }
  prog= srv=
  rm -f "$sock"
}
start() { # start <name> [VAR=value ...]: a server whose report goes to $work/<name>
  stop
  sess=$work/$1; shift
  mkdir -p "$sess/c"
  env -u VKBRIDGE_TEST_DELAY_CMD -u VKBRIDGE_TEST_DELAY_MS -u VKBRIDGE_REPORT_STUCK_S -u VKBRIDGE_SYNCFD_EXPORT_MS \
      -u VKBRIDGE_SYNCFD_ORDER_MS VKBRIDGE_PROBE=0 VKBRIDGE_PROBE_BUDGET_S=0 "$@" VK_DRIVER_FILES=$host_icd VK_ICD_FILENAMES= \
      "$out/vkbridge-server" --socket "$sock" --log "$sess/vkbridge-server.log" --cache-dir "$sess/c" >/dev/null 2>&1 &
  srv=$!
  for _ in $(seq 300); do [ -S "$sock" ] && break; kill -0 $srv 2>/dev/null || break; sleep 0.05; done
}
run_stall() { timeout 60 "$out/syncfd_check" stall >"$sess/check.log" 2>&1 & prog=$!; }
stuck_json() { # the program's stuck_call as "call s gpu_wait", or "null"
  python3 -I - "$sess/malibridge-report.json" <<'EOF'
import json, sys
try:
    d = json.load(open(sys.argv[1]))
except Exception as e:
    print("unreadable"); sys.exit(0)
for p in d.get("processes", []):
    if p.get("comm") == "syncfd_check" or p.get("name") == "syncfd_check":
        s = p.get("stuck_call", "absent")
        print("null" if s is None else s if s == "absent" else "%s %.0f %s" % (s["call"], s["s"], str(s["gpu_wait"]).lower()))
        break
else:
    print("no process")
EOF
}
trap 'stop; [ -n "$KEEP" ] && echo "kept $work" || rm -rf "$work"' EXIT
export VKBRIDGE_SOCKET=$sock VK_DRIVER_FILES=$out/vkbridge_icd.json VK_ICD_FILENAMES=
unset VKBRIDGE_HIDE_EXTS VKBRIDGE_MAX_API VKBRIDGE_FAKE_MISSING VKBRIDGE_MODE
fails=0 steps=0
step() { steps=$((steps+1)); if [ "$1" = 0 ]; then echo "  ok   $2"; else echo "  FAIL $2"; fails=$((fails+1)); fi; }

# 1, 2
start submit VKBRIDGE_REPORT_STUCK_S=3 VKBRIDGE_TEST_DELAY_CMD=vkQueueSubmit VKBRIDGE_TEST_DELAY_MS=14000
run_stall
sleep 10
j1=$(stuck_json)
l1=$(grep -o "syncfd_check[^|]*: vkQueueSubmit has run [0-9]* s in the GPU driver and not returned (still running)" "$sess/malibridge-report.md" | head -1)
read -r c1 s1 w1 <<<"$j1"
[ "$c1" = vkQueueSubmit ] && [ "${s1:-0}" -ge 3 ] && [ "$w1" = false ] && [ -n "$l1" ]
step $? "1 held vkQueueSubmit: named while it runs (json: $j1; md: ${l1:-no line})"
wait $prog 2>/dev/null; prog=
sleep 6
j2=$(stuck_json)
[ "$j2" = null ] && ! grep -q "has run [0-9]* s in the GPU driver" "$sess/malibridge-report.md"
step $? "2 returned: no longer named (json: $j2)"

# 3
start wait VKBRIDGE_REPORT_STUCK_S=3 VKBRIDGE_TEST_DELAY_CMD=vkDeviceWaitIdle VKBRIDGE_TEST_DELAY_MS=14000
run_stall
sleep 10
j3=$(stuck_json)
l3=$(grep -o "vkDeviceWaitIdle has waited [0-9]* s for the GPU and not returned (still waiting)" "$sess/malibridge-report.md" | head -1)
read -r c3 s3 w3 <<<"$j3"
[ "$c3" = vkDeviceWaitIdle ] && [ "$w3" = true ] && [ -n "$l3" ]
step $? "3 held vkDeviceWaitIdle: said as a wait for the GPU (json: $j3; md: ${l3:-no line})"
wait $prog 2>/dev/null; prog=

# 4
start off VKBRIDGE_REPORT_STUCK_S=0 VKBRIDGE_TEST_DELAY_CMD=vkQueueSubmit VKBRIDGE_TEST_DELAY_MS=14000
run_stall
sleep 10
j4=$(stuck_json)
[ "$j4" = null ] && ! grep -q "and not returned" "$sess/malibridge-report.md"
step $? "4 VKBRIDGE_REPORT_STUCK_S=0: never named (json: $j4)"
wait $prog 2>/dev/null; prog=

if [ $fails = 0 ]; then echo "OK: $steps steps"; else echo "FAILED: $fails of $steps steps"; fi
[ $fails = 0 ]
