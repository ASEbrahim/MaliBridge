#!/bin/bash
# Sync-fd exports with a deadline (server_syncfd.c), a server per step.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
work=$(mktemp -d)
sock=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-syncfd-$$.sock
host_icd=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json}
srv=
log=
stop() { [ -n "$srv" ] && kill $srv 2>/dev/null && wait $srv 2>/dev/null; srv=; }
start() { # start <name> [env...]: a server of its own, log $log
  local d=$work/$1
  shift
  stop
  rm -f "$sock"
  mkdir -p "$d/c"
  log=$d/vkbridge-server.log
  : > "$log"
  env -u VKBRIDGE_FAKE_MISSING -u VKBRIDGE_SYNCFD_EXPORT_MS -u VKBRIDGE_TEST_DELAY_CMD -u VKBRIDGE_TEST_DELAY_MS \
      VKBRIDGE_PROBE_BUDGET_S=0 "$@" VK_DRIVER_FILES=$host_icd VK_ICD_FILENAMES= \
      "$out/vkbridge-server" --socket "$sock" --log "$log" --cache-dir "$d/c" >/dev/null 2>&1 &
  srv=$!
  for _ in $(seq 300); do [ -S "$sock" ] && break; kill -0 $srv 2>/dev/null || break; sleep 0.05; done
}
check() { timeout 60 "$out/syncfd_check" "$1" >"$work/check-$2.log" 2>&1; tail -1 "$work/check-$2.log"; }
trap 'stop; rm -f "$sock"; [ -n "$KEEP" ] && echo "kept $work" || rm -rf "$work"' EXIT
export VKBRIDGE_SOCKET=$sock VK_DRIVER_FILES=$out/vkbridge_icd.json VK_ICD_FILENAMES=
unset VKBRIDGE_HIDE_EXTS VKBRIDGE_MAX_API VKBRIDGE_FAKE_MISSING VKBRIDGE_MODE
fails=0 steps=0
step() { steps=$((steps+1)); if [ "$1" = 0 ]; then echo "  ok   $2"; else echo "  FAIL $2"; fails=$((fails+1)); fi; }

# 1
start off VKBRIDGE_SYNCFD_EXPORT_MS=0
c1=$(check direct off)
case "$c1" in
SKIP*) echo "  skip the driver does not block a sync-fd export ($c1)"; noblock=1 ;;
*) grep -q "^OK" <<<"$c1" && ! grep -q "sync-fd export:" "$log"
   step $? "the bound off: the export waits for the held batch, as Xwayland did ($c1; $(grep -m1 'held batch' "$work/check-off.log" | sed 's/^ *//'))" ;;
esac

# 2
if [ -z "$noblock" ]; then
  start bounded VKBRIDGE_SYNCFD_EXPORT_MS=500
  c2=$(check bounded bounded)
  sleep 1 # the driver's call returns about 3 s in; its line follows
  grep -q "^OK" <<<"$c2" && grep -q "sync-fd export: answered within 500 ms (VKBRIDGE_SYNCFD_EXPORT_MS)" "$log" &&
    grep -q "sync-fd export: the driver's vkGetSemaphoreFdKHR has not returned in 500 ms (pid [0-9]* syncfd_check" "$log" &&
    grep -q "a destroy of a handle whose export is still in the driver held back" "$log" &&
    grep -q "sync-fd export: the driver's vkGetSemaphoreFdKHR returned after [23]\.[0-9] s (0; 1 so far), its fd closed; the held-back destroy done" "$log" &&
    ! grep -q "left alive" "$log"
  step $? "VKBRIDGE_SYNCFD_EXPORT_MS=500: answered at the deadline with fd -1, the destroy done after the driver's call ($c2; $(grep -m1 'held batch' "$work/check-bounded.log" | sed 's/^ *//'))"
fi

# 3
start stall VKBRIDGE_TEST_DELAY_CMD=vkQueueSubmit VKBRIDGE_TEST_DELAY_MS=6500
c3=$(check stall stall)
grep -q "^OK" <<<"$c3" && grep -q "test: vkQueueSubmit held 6500 ms" "$log" &&
  grep -q "process [0-9]* (syncfd_check): vkDeviceWaitIdle has waited 5 s for the process's earlier requests (up to [0-9]*, [0-9]* done); running: vkQueueSubmit for [4-6]\.[0-9] s;" "$log"
step $? "a held asynchronous submit: the waiting call's line names it ($c3; $(grep -m1 -o 'vkDeviceWaitIdle has waited.*' "$log"))"

# 4
start default
c4=$(check any default)
grep -q "^OK" <<<"$c4" && ! grep -q "sync-fd export:" "$log"
step $? "nothing set, not Samsung's driver: no bound ($c4)"
stop

if [ $fails = 0 ]; then echo "OK: $steps steps"; else echo "FAILED: $fails of $steps steps"; exit 1; fi
