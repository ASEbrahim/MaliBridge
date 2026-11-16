#!/bin/bash
# The BC converters' correctness probe (server_probe.c bc_variants) is paid once per GPU + driver + converter set, not at every
# bridge build (the device profile's key holds the bridge build; the verdict's own file, bcv-*.txt, does not):
#   1  first start in a cache dir: the probe builds and compares every variant ("pass N ms"), a verdict file is left;
#   2  a later start whose profile is stale (a probe-affecting setting changed, as a new bridge build does): bc_variants
#      reads the kept verdict, builds nothing, and takes under a fifth of run 1 (and under 500 ms);
#   3  another driver (VKBRIDGE_TEST_BCV_SALT stands in for a changed driver version / pipeline-cache UUID): probes again;
#   4  a kept verdict that turned a variant off ("flags 0x40000", written by hand) is honoured: warned, one dispatch a run off, the
#      old converters stay (safe default); a damaged file is ignored (probed again); an imitated difference or hang leaves no file.
# Prints one line per step and "OK: ..." / "FAILED: ..." last. Needs the same server as bcupload_test.sh (VKB_HOST_ICD).
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
icd=${VKB_CLIENT_ICD:-$out/vkbridge_icd.json}
work=$(mktemp -d)
sock=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-bcprobecache-$$.sock
srv=
fails=0
stop() { [ -n "$srv" ] && kill $srv 2>/dev/null && wait $srv 2>/dev/null; srv=; }
trap 'stop; rm -rf "$work" "$sock"' EXIT
# run <name> <cachedir> [server env...]: $slog
run() {
  local name=$1 cdir=$2; shift 2
  stop; rm -f "$sock"; mkdir -p "$cdir"
  slog=$work/$name.server.log
  env -u VKBRIDGE_FAKE_MISSING -u VKBRIDGE_PROBE -u VKBRIDGE_PROBE_BCV_FAKE -u VKBRIDGE_PROBE_TX_TIMEOUT_S -u VKBRIDGE_BC_ONE_DISPATCH -u VKBRIDGE_BC_PALETTE \
      -u WAYLAND_DISPLAY -u DISPLAY VKBRIDGE_PROBE_BUDGET_S=200 VKBRIDGE_TEST_BC_GPU=Mali-G925-Immortalis VKBRIDGE_FAKE_MISSING=bc "$@" \
      VK_DRIVER_FILES=${SRV_ICD:-${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json}} VK_ICD_FILENAMES= \
      "$out/vkbridge-server" --socket "$sock" --log "$slog" --cache-dir "$cdir" >/dev/null 2>&1 &
  srv=$!
  for _ in $(seq 300); do [ -S "$sock" ] && break; kill -0 $srv 2>/dev/null || break; sleep 0.05; done
  # the probe runs inside the start (budget 200 s): the socket listens after it; the line is in the log
  for _ in $(seq 400); do grep -q "probe: bc_variants" "$slog" && break; sleep 0.1; done
  sleep 0.3
  stop
}
step() { if [ "$1" = 0 ]; then echo "ok   $2"; else echo "FAIL $2"; fails=$((fails+1)); grep -h "probe: bc_variants\|bcv\|verdict" "$slog" | tail -3 | sed 's/^/     /'; fi; }
bcv_line() { grep -o "probe: bc_variants .*" "$slog" | tail -1; }
bcv_ms() { bcv_line | grep -o "[a-z]* *[0-9.]* ms" | head -1 | grep -o "[0-9.]* ms" | grep -o "[0-9.]*"; }
vfile() { ls "$1"/bcv-*.txt 2>/dev/null | head -1; }

c1=$work/c1
run first "$c1"
t1=$(bcv_ms)
step $([ -n "$t1" ] && bcv_line | grep -q "pass.* [0-9]* same as the old variants" && ! bcv_line | grep -q "kept verdict" && [ -n "$(vfile "$c1")" ]; echo $?) \
  "1 first start: probed in ${t1:-?} ms, verdict file $(basename "$(vfile "$c1")" 2>/dev/null); $(bcv_line | cut -c1-110)"
run second "$c1" VKBRIDGE_PROBE_TX_TIMEOUT_S=21
t2=$(bcv_ms)
f=$(vfile "$c1"); [ -n "$f" ] && cp "$f" "$work/verdict.keep"
step $([ -n "$t2" ] && bcv_line | grep -q "kept verdict" && awk -v a="${t1:-0}" -v b="${t2:-1e9}" 'BEGIN { exit !(b < 500 && b * 5 < a) }'; echo $?) \
  "2 later start, stale profile: ${t2:-?} ms (first ${t1:-?} ms); $(bcv_line | cut -c1-110)"
run third "$c1" VKBRIDGE_PROBE_TX_TIMEOUT_S=22 VKBRIDGE_TEST_BCV_SALT=driver2
t3=$(bcv_ms)
step $([ -n "$t3" ] && ! bcv_line | grep -q "kept verdict" && bcv_line | grep -q "pass" && awk -v a="${t2:-0}" -v b="${t3:-0}" 'BEGIN { exit !(b > a * 3) }'; echo $?) \
  "3 another driver (salt): probed again, ${t3:-?} ms"
# 4a a kept verdict with a variant off
c2=$work/c2; mkdir -p "$c2"
if [ -n "$f" ]; then
  head -1 "$work/verdict.keep" > "$c2/$(basename "$f")"; echo "flags 0x40000" >> "$c2/$(basename "$f")"
fi
run kept-off "$c2" VKBRIDGE_PROBE_TX_TIMEOUT_S=23
step $(bcv_line | grep -q "kept verdict" && bcv_line | grep -q "one dispatch a run" && grep -q "a kept verdict says these converter variants differed" "$slog"; echo $?) \
  "4a kept verdict with the table variants off: honoured and warned; $(bcv_line | cut -c1-140)"
# 4b a damaged file
c3=$work/c3; mkdir -p "$c3"
[ -n "$f" ] && echo "garbage" > "$c3/$(basename "$f")"
run damaged "$c3" VKBRIDGE_PROBE_TX_TIMEOUT_S=24
step $(bcv_line | grep -q "pass" && ! bcv_line | grep -q "kept verdict"; echo $?) "4b damaged verdict file: probed (safe default)"
# 4c imitated difference / hang: no file
c4=$work/c4
run differ "$c4" "VKBRIDGE_PROBE_BCV_FAKE=BC3 decode table:differ"
step $(bcv_line | grep -q "BC3 decode table: DIFFERS" && [ -z "$(vfile "$c4")" ]; echo $?) "4c imitated difference: no verdict kept; $(bcv_line | cut -c1-120)"
if [ $fails = 0 ]; then echo "OK: probe paid once ($t1 ms), then ${t2} ms; another driver probes again; kept verdicts honoured"; else echo "FAILED: $fails"; exit 1; fi
