#!/bin/bash
# ThreadSanitizer check of the session report's globals (server_report.c: dirty, rdir): a TSan
# server (make -C host TSAN=1, into out-tsan) runs report_stuck_test, whose server rewrites the
# report from its report thread while request threads log. FAILED if any report names
# server_report.c in its SUMMARY line (before the fix: "data race ... server_report.c:1587 in
# report_main"). Not part of run_tests.sh (needs the TSan build). Host ICD: VKB_HOST_ICD.
cd "$(dirname "$0")"
out=$PWD/out-tsan
[ -x "$out/vkbridge-server" ] || { echo "FAILED: build first: make -C host TSAN=1"; exit 1; }
logs=$(mktemp -d)
VKB_OUT=$out TSAN_OPTIONS="log_path=$logs/t halt_on_error=0 history_size=4" \
  setarch -R ./report_stuck_test.sh >"$logs/run.log" 2>&1 || { tail -3 "$logs/run.log"; echo "FAILED: report_stuck_test"; exit 1; }
if grep -h 'SUMMARY: ThreadSanitizer' "$logs"/t.* 2>/dev/null | grep -q 'server_report\.c'; then
  grep -h 'SUMMARY: ThreadSanitizer' "$logs"/t.* | grep 'server_report\.c' | sort -u
  echo "FAILED: data race in server_report.c (reports in $logs)"; exit 1
fi
rm -rf "$logs"
echo "OK: report_stuck_test under TSan, no race in server_report.c"
