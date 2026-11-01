#!/bin/bash
# The BC conversion budget (VKBRIDGE_BC_TX_BUDGET, server/server_emu_bcn.c) on a burst of BC uploads
# (tests/bcburst_check.c: 32 textures of 1024x1024 with every level, half BC7, half BC1, in one
# frame, then 20 frames of 2), each run with a server of its own that transcodes BC with its GPU
# slowed to a Mali-G720's (VKBRIDGE_TEST_BC_SLOW=8: the burst's transcodes take about what the
# G720's warm BC7 2048x2048 level does a texel) and the per-hitch line at 100 ms:
#   1  no budget (0): the burst frame is one long frame, its hitch line says cause=bc;
#   2  the budget DS3's preset sets (32:4; off by default): the burst frame has no hitch line and is
#      under 40% of 1's; the load (burst plus streaming) no longer; some images decoded, every
#      texture at its PSNR floor;
#   3  everything decoded (VKBRIDGE_BC_TRANSCODE=0), for reference;
#   4  VKBRIDGE_BC_TX_FALLBACK_MB=16: past the cap images are transcoded, the decoded ones' extra
#      memory stays near 16 MB;
#   5  memory requirements asked before each image (--query, vkGetDeviceImageMemoryRequirements):
#      nothing decoded over the budget, no image needs more memory than it was told;
#   6  nothing set: no budget (the default), as 1.
# 2, 4 and 5 run with VKBRIDGE_BC_TX_BUDGET=32:4.
# Prints one line per step and "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
icd=${VKB_CLIENT_ICD:-$out/vkbridge_icd.json}
work=$(mktemp -d)
sock=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-bcbudget-$$.sock
srv=
fails=0
bud=32:4 # what Dark Souls III's built-in preset sets (server/presets_builtin.inc)
stop() { [ -n "$srv" ] && kill $srv 2>/dev/null && wait $srv 2>/dev/null; srv=; }
trap 'stop; rm -rf "$work" "$sock"' EXIT
# run <name> [server env...] -- [bcburst_check args]: $res, $slog, $o (the check's output)
run() {
  local name=$1; shift
  local e=()
  while [ "$1" != -- ]; do e+=("$1"); shift; done
  shift
  stop
  rm -f "$sock"
  rm -rf "$work/c"; mkdir -p "$work/c"
  slog=$work/$name.server.log; o=$work/$name.out
  env -u VKBRIDGE_FAKE_MISSING -u VKBRIDGE_HIDE_EXTS -u VKBRIDGE_ERRATA -u VKBRIDGE_MODE -u VKBRIDGE_BC_TX_BUDGET -u VKBRIDGE_BC_TX_FALLBACK_MB \
      -u VKBRIDGE_BC1_ASTC -u VKBRIDGE_BC_TRANSCODE_MAX -u VKBRIDGE_BC_DECODE -u VKBRIDGE_TEST_HITCH_DELAY_MS \
      VKBRIDGE_PROBE_BUDGET_S=0 VKBRIDGE_FAKE_MISSING=bc vk_require_etc2=true vk_require_astc=true VKBRIDGE_BC_TRANSCODE=1 \
      VKBRIDGE_HITCH_MS=100 VKBRIDGE_TEST_BC_SLOW=8 "${e[@]}" \
      VK_DRIVER_FILES=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json} VK_ICD_FILENAMES= \
      "$out/vkbridge-server" --socket "$sock" --log "$slog" --cache-dir "$work/c" >/dev/null 2>&1 &
  srv=$!
  for _ in $(seq 300); do [ -S "$sock" ] && break; kill -0 $srv 2>/dev/null || break; sleep 0.05; done
  timeout 300 env -u VKBRIDGE_HITCH_MS VKBRIDGE_SOCKET="$sock" VK_DRIVER_FILES="$icd" VK_ICD_FILENAMES= \
    VKBRIDGE_CLIENT_LOG="$work/$name.client.log" "$out/bcburst_check" --frames 20 "$@" >"$o" 2>&1
  res=$?
  for _ in $(seq 100); do grep -q "client process [0-9]* gone" "$slog" && break; sleep 0.05; done
  stop
}
step() { if [ "$1" = 0 ]; then echo "ok   $2"; else echo "FAIL $2"; fails=$((fails+1)); dump; fi; }
val() { grep -o "$1=[0-9.]*" "$o" | head -1 | cut -d= -f2; }       # val <key>: from the RESULT line
lt() { awk -v a="$1" -v b="$2" 'BEGIN { exit !(a + 0 < b + 0) }'; } # lt a b: a < b
cause_of() { grep -o "hitch: frame=$2 ms=[0-9]* cause=[a-z]*" "$1" | sed 's/.*cause=//'; }
totals() { grep -o "BC budget: so far .*" "$slog" | tail -1; }
dump() { tail -4 "$o" | sed 's/^/     /'; grep -h "BC budget\|hitch:" "$slog" | tail -6 | sed 's/^/     /'; }

# 1 no budget
run off VKBRIDGE_BC_TX_BUDGET=0 --
f=$(val burst_frame); off_ms=$(val burst_ms); off_load=$(val load_ms); c=$(cause_of "$slog" "$f")
[ $res = 0 ] && [ "$c" = bc ] && ! lt "$off_ms" 200 && ! grep -q "BC budget: transcoding" "$slog"
step $? "1 no budget: burst frame $f ${off_ms} ms, hitch cause ${c:-none}; load ${off_load} ms"

# 2 the budget DS3's built-in preset sets
run budget VKBRIDGE_BC_TX_BUDGET=$bud --
f=$(val burst_frame); ms=$(val burst_ms); load=$(val load_ms); c=$(cause_of "$slog" "$f")
dec=$(totals | grep -o "[0-9]* ([0-9.]*) decoded" | cut -d' ' -f1)
[ $res = 0 ] && [ -z "$c" ] && lt "$ms" "$(awk -v a="$off_ms" 'BEGIN { print a * 0.4 }')" && ! lt "$off_load" "$load" && [ "${dec:-0}" -gt 0 ] &&
  [ "$(val below_floor)" = 0 ] && grep -q "BC budget: transcoding 32.0 million texels a second, 4.0 at once" "$slog"
step $? "2 budget $bud: burst frame ${ms} ms (no hitch line: ${c:-none}), load ${load} ms; $(val textures) textures, min PSNR $(val min_psnr) dB; $(totals | sed 's/BC budget: so far //')"

# 3 decoded, for reference
run decoded VKBRIDGE_BC_TRANSCODE=0 --
[ $res = 0 ] && [ "$(val exact)" = "$(val textures)" ]
step $? "3 every texture decoded: burst frame $(val burst_ms) ms, load $(val load_ms) ms, $(val exact) of $(val textures) exact"

# 4 the decoded images' memory cap
run cap VKBRIDGE_BC_TX_BUDGET=$bud VKBRIDGE_BC_TX_FALLBACK_MB=16 --
past=$(totals | grep -o "[0-9]* transcoded past" | cut -d' ' -f1); live=$(totals | grep -o "cost [0-9]* MB more" | cut -d' ' -f2)
[ $res = 0 ] && [ "${past:-0}" -gt 0 ] && [ "${live:-99}" -le 16 ] && [ "$(val below_floor)" = 0 ]
step $? "4 VKBRIDGE_BC_TX_FALLBACK_MB=16: ${past:-0} images transcoded past the cap, decoded ones cost ${live:-?} MB more; burst frame $(val burst_ms) ms"

# 5 shapes whose memory requirements were asked: never decoded
run query VKBRIDGE_BC_TX_BUDGET=$bud -- --query
dec=$(totals | grep -o "[0-9]* ([0-9.]*) decoded" | cut -d' ' -f1)
[ $res = 0 ] && [ "${dec:-1}" = 0 ] && [ "$(val query_bigger)" = 0 ]
step $? "5 --query: ${dec:-?} decoded over the budget, $(val query_bigger) images bigger than queried ($(tail -1 "$o"))"

# 6 the default: off
run default --
[ $res = 0 ] && ! grep -q "BC budget: transcoding" "$slog" && [ -z "$(totals)" ]
step $? "6 nothing set: no budget (burst frame $(val burst_ms) ms, hitch cause $(cause_of "$slog" "$(val burst_frame)"))"

if [ $fails = 0 ]; then echo "OK: a 32-texture BC burst ${off_ms} -> ${ms} ms in its frame under a $bud budget (off by default), load ${off_load} -> ${load} ms; contents at their floors; cap and query pins hold"
else echo "FAILED: $fails step(s)"; exit 1; fi
