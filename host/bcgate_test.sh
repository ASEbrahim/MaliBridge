#!/bin/bash
# BC fast converters' GPU gate (server_probe.c vkb_bc_fast_gate): VKBRIDGE_BC_FAST defaults on, and the bc_variants probe checks the
# BC_FAST variants, only on a GPU measured as strong (G720, G925, Immortalis spellings included); every other GPU (G68, G57,
# G615, an unknown name) behaves exactly as test47 did: no BC_FAST variants, and the probe as test47 ran it (table and palette
# variants only, so its cost is test47's, not the fast set's 65 s on a G68). VKB_T47_OUT=<a test47 (3c396c2) host/out> adds the
# comparison: the same G68-named run on test47's server must choose the same converter variants and probe the same number. An
# VKBRIDGE_BC_FAST wins both ways. VKBRIDGE_TEST_BC_GPU stands in for the GPU's name. Prints "ok"/"FAIL" per case and "OK:"/"FAILED:" last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
icd=${VKB_CLIENT_ICD:-$out/vkbridge_icd.json}
work=$(mktemp -d)
sock=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-bcgate-$$.sock
srv=; fails=0
stop() { [ -n "$srv" ] && kill $srv 2>/dev/null && wait $srv 2>/dev/null; srv=; }
trap 'stop; rm -rf "$work" "$sock"' EXIT
# run <name> <gpu name or -> [server env...]: $slog
run() {
  local name=$1 gpu=$2; shift 2
  local g=(); [ "$gpu" != - ] && g=(VKBRIDGE_TEST_BC_GPU="$gpu")
  stop; rm -f "$sock"; rm -rf "$work/c"; mkdir -p "$work/c"
  slog=$work/$name.server.log
  env -u VKBRIDGE_FAKE_MISSING -u VKBRIDGE_PROBE -u VKBRIDGE_PROBE_BCV_FAKE -u VKBRIDGE_BC_FAST -u VKBRIDGE_TEST_BC_GPU -u VKBRIDGE_BC_ONE_DISPATCH -u VKBRIDGE_BC_PALETTE \
      -u WAYLAND_DISPLAY -u DISPLAY VKBRIDGE_PROBE_BUDGET_S=200 VKBRIDGE_FAKE_MISSING=bc "${g[@]}" "$@" \
      VK_DRIVER_FILES=${SRV_ICD:-${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json}} VK_ICD_FILENAMES= \
      "$out/vkbridge-server" --socket "$sock" --log "$slog" --cache-dir "$work/c" >/dev/null 2>&1 &
  srv=$!
  for _ in $(seq 300); do [ -S "$sock" ] && break; kill -0 $srv 2>/dev/null || break; sleep 0.05; done
  # a client creates the device (the BC emulation starts then)
  timeout 120 env -u WAYLAND_DISPLAY -u DISPLAY VKBRIDGE_SOCKET="$sock" VK_DRIVER_FILES="$icd" VK_ICD_FILENAMES= \
    "$out/bcupload_check" --pattern dxvk3 --count 1 --frames 1 >"$work/$name.out" 2>&1
  for _ in $(seq 100); do grep -q "BC fast converters" "$slog" && break; sleep 0.1; done
  stop
}
probe_ran() { grep -q "probe: bc_variants  *\(pass\|fail\|hang\)" "$slog"; }
case_() { # <name> <gpu> <expect: on|off|forced-on|forced-off> [env...]
  local name=$1 gpu=$2 want=$3; shift 3
  run "$name" "$gpu" "$@"
  local ok=1 line; line=$(grep -o "BC fast converters: .*" "$slog" | head -1)
  case $want in
    on)  grep -q "BC fast converters: on (measured GPU)" "$slog" && probe_ran && grep -q "BC1 -> ETC2 palette search with less state a thread" "$slog" || ok=0;;
    off) grep -q "BC fast converters: off (not in the measured list: " "$slog" && probe_ran && ! grep -q "less state a thread" "$slog" &&
         ! grep -q "BC emulation: new converter variants (.*BC7 -> ASTC fast" "$slog" && ! grep -q "palette fast\|ASTC fast" "$slog" &&
         grep -q "new converter variants (one dispatch a run, palette search)" "$slog" || ok=0;;
    forced-on)  grep -q "BC fast converters: VKBRIDGE_BC_FAST=1 decides" "$slog" && probe_ran && grep -q "BC1 -> ETC2 palette search with less state a thread" "$slog" || ok=0;;
    forced-off) grep -q "BC fast converters: VKBRIDGE_BC_FAST=0 decides" "$slog" && ! grep -q "palette search with less state a thread" "$slog" || ok=0;;
  esac
  if [ $ok = 1 ]; then echo "ok   $name ($gpu): $want; $line"; else echo "FAIL $name ($gpu): wanted $want; $line"; fails=$((fails+1)); grep -h "BC fast\|probe: bc_variants\|startup probe" "$slog" | head -5 | sed 's/^/     /'; fi
}
case_ g925 "Mali-G925-Immortalis MC12" on
case_ g925b "Immortalis-G925" on
case_ g720 "Mali-G720-Immortalis MC10" on
case_ g720b "Immortalis-G720 MC10" on
case_ g68 "Mali-G68" off
case_ g57 "Mali-G57 MC2" off
case_ g615 "Mali-G615 MC6" off
case_ g7205 "Mali-G7205" off
case_ unknown - off
case_ g925-env0 "Mali-G925-Immortalis MC12" forced-off VKBRIDGE_BC_FAST=0
case_ g68-env1 "Mali-G68" forced-on VKBRIDGE_BC_FAST=1
# the converter variants chosen and the probe, as a comparable summary of a log
summ() { { grep -o "BC emulation: new converter variants ([^)]*)" "$1"; grep -o "probe: bc_variants  *[a-z]* *[0-9.]* ms[^;]*[0-9]* same as the old variants[^-]*" "$1" | sed 's/ [0-9.]* ms//; s/  */ /g';
           grep -o "BC emulation: BC1 -> ETC2 and BC4 / BC5 -> EAC search\|BC emulation: BC1 -> ETC2 search the blocks' palettes\|one dispatch a run of regions (VKBRIDGE_BC_ONE_DISPATCH default" "$1"; } | sort; }
if [ -n "$VKB_T47_OUT" ] && [ -x "$VKB_T47_OUT/vkbridge-server" ]; then
  run ours-g68 "Mali-G68"
  cp "$slog" "$work/ours-g68.log"
  o2=$out; out=$VKB_T47_OUT; icd=$VKB_T47_OUT/vkbridge_icd.json
  run t47-g68 -
  cp "$slog" "$work/t47-g68.log"; out=$o2
  a=$(summ "$work/ours-g68.log"); b=$(summ "$work/t47-g68.log")
  tms() { grep -o "probe: bc_variants  *[a-z]* *[0-9.]* ms" "$1" | grep -o "[0-9.]* ms"; }
  # the old variants' count and times: test47 has no BC_FAST variants, ours skips them here
  na=$(grep -o "[0-9]* same as the old variants" "$work/ours-g68.log" | head -1); nb=$(grep -o "[0-9]* same as the old variants" "$work/t47-g68.log" | head -1)
  if [ "$a" = "$b" ] && [ -n "$a" ]; then echo "ok   G68 off == test47: same variants chosen and probed ($na vs $nb); probe $(tms "$work/ours-g68.log") vs test47 $(tms "$work/t47-g68.log")"; echo "$a" | sed 's/^/       /'
  else echo "FAIL G68 off != test47"; diff <(echo "$a") <(echo "$b") | sed 's/^/     /'; fails=$((fails+1)); fi
fi
if [ $fails = 0 ]; then echo "OK: BC fast converters and their probe only on G720 / G925; env override both ways"; exit 0; else echo "FAILED: $fails case(s)"; exit 1; fi
