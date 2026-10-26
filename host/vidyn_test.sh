#!/bin/bash
# VK_EXT_vertex_input_dynamic_state for Zink (server_vidyn.c): vkCmdSetVertexInputEXT's vertex input
# made static in pipeline variants where the driver lacks the extension or a vertex format is
# emulated. vidyn_check draws with one pipeline and four vertex layouts (two plain, one
# B10G11R11_UFLOAT, one A2R10G10B10_SNORM), one of them again after a bind and one in a secondary.
#     1  the desktop driver itself: vidyn_check passes (the check is right)
#     2  through the bridge as Zink: the driver's extension, no layer, passes
#     3  VKBRIDGE_FAKE_MISSING=vidyn: the server's extension, every draw from a variant (4 made, the
#        driver gets no vkCmdSetVertexInputEXT), graphics pipeline libraries hidden from Zink;
#        the same with asynchronous compiles on
#     4  both packed formats imitated missing: the driver's extension, variants for the two
#        layouts with them only (2), the plain ones passed straight to the driver
#     5  the extension and both formats imitated missing: 4 variants
#     6  a client that is not Zink with vidyn imitated missing: no extension (SKIP)
#     7  VKBRIDGE_EMU_VIDYN=0 with BGRA 10-10-10-2 imitated missing: hidden from Zink as before (SKIP)
# Prints one line per step and "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
icd=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json}
work=$(mktemp -d)
sock=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-vidyn-$$.sock
log=$work/server.log
touch "$log"
srv=
trap 'kill $srv 2>/dev/null; rm -f "$sock"; [ -n "$KEEP" ] || rm -rf "$work"' EXIT
fails=0 steps=0
step() { steps=$((steps+1)); if [ "$1" = 0 ]; then echo "  ok   $2"; else echo "  FAIL $2"; fails=$((fails+1)); fi; }

# server <VAR=value...>: a server of its own (VKBRIDGE_FAKE_MISSING is read once per server)
server() {
  [ -n "$srv" ] && kill $srv 2>/dev/null && wait $srv 2>/dev/null
  rm -f "$sock"
  rm -rf "$work/c" && mkdir -p "$work/c"
  env -u VKBRIDGE_FAKE_MISSING -u VKBRIDGE_HIDE_EXTS -u VKBRIDGE_ERRATA -u VKBRIDGE_ERRATA_IMITATE -u VKBRIDGE_MODE \
      -u VKBRIDGE_FAKE_DRIVER_VERSION -u VKBRIDGE_EMU_VIDYN -u VKBRIDGE_ASYNC_COMPILE "$@" \
      VK_DRIVER_FILES=$icd VK_ICD_FILENAMES= "$out/vkbridge-server" --socket "$sock" --log "$log" --cache-dir "$work/c" >/dev/null 2>&1 &
  srv=$!
  for _ in $(seq 100); do [ -S "$sock" ] && break; sleep 0.1; done
}

# run [engine] [<env line>...]: vidyn_check through the bridge, <cache dir>/env = the env lines;
# $rc, $res (its last line), $new (the server log lines it made)
run() {
  local engine=${1:-mesa zink}
  shift
  printf '%s\n' "$@" > "$work/c/env"
  local l; l=$(wc -l < "$log")
  VKBRIDGE_SOCKET=$sock VK_DRIVER_FILES=$out/vkbridge_icd.json VK_ICD_FILENAMES= VKBRIDGE_HIDE_EXTS= VKBRIDGE_MAX_API= \
    timeout 120 "$out/vidyn_check" "$engine" >$work/run.log 2>&1
  rc=$?
  res=$(tail -1 $work/run.log)
  sleep 0.3 # the device's last log lines
  new=$(tail -n +$((l+1)) "$log")
}
lg() { echo "$new" | grep -o "$1" | tail -1; }
summary='[0-9]* pipeline(s) kept.*[0-9]* variant(s) made (at most [0-9]* for one pipeline), [0-9]* failed, [0-9]* variant bind(s), [0-9]* draw(s) dropped; vkCmdSetVertexInputEXT: [0-9]* passed to the driver, [0-9]* held'

VK_DRIVER_FILES=$icd VK_ICD_FILENAMES= timeout 120 "$out/vidyn_check" >$work/run.log 2>&1
step $? "the desktop driver itself: $(tail -1 $work/run.log)"

server
run
[ $rc = 0 ] && ! grep -q "^SKIP" $work/run.log && echo "$new" | grep -q "for Zink: the driver's own" && ! echo "$new" | grep -q "pipeline(s) kept"
step $? "as Zink, the driver's extension: $res"

server VKBRIDGE_FAKE_MISSING=vidyn
run
[ $rc = 0 ] && ! grep -q "^SKIP" $work/run.log && grep -q "without VK_EXT_graphics_pipeline_library" $work/run.log &&
  echo "$new" | grep -q "1 pipeline(s) kept.* 4 variant(s) made (at most 4 for one pipeline), 0 failed.* 0 draw(s) dropped; vkCmdSetVertexInputEXT: 0 passed to the driver, 6 held"
step $? "vidyn imitated missing: $res; $(lg "$summary")"
run "mesa zink" VKBRIDGE_ASYNC_COMPILE=1
[ $rc = 0 ] && ! grep -q "^SKIP" $work/run.log && echo "$new" | grep -q "asynchronous pipeline compile on" &&
  echo "$new" | grep -q " 4 variant(s) made"
step $? "the same, asynchronous compiles on: $res"
run dxvk
[ $rc = 0 ] && grep -q "^SKIP the extension is not offered" $work/run.log
step $? "vidyn imitated missing, a client that is not Zink: $(grep -m1 '^SKIP' $work/run.log)"

server VKBRIDGE_FAKE_MISSING=uf11vtx,bgr10vtx
run
[ $rc = 0 ] && ! grep -q "^SKIP" $work/run.log && echo "$new" | grep -q "for Zink: the driver's, with pipeline variants" &&
  echo "$new" | grep -q "1 pipeline(s) kept.* 2 variant(s) made (at most 2 for one pipeline), 0 failed.* 0 draw(s) dropped; vkCmdSetVertexInputEXT: 4 passed to the driver, 2 held" &&
  echo "$new" | grep -q "unpacked in the vertex shader" && echo "$new" | grep -q "red and blue swapped back in the vertex shader"
step $? "packed formats imitated missing: $res; $(lg "$summary")"

server VKBRIDGE_FAKE_MISSING=vidyn,uf11vtx,bgr10vtx
run
[ $rc = 0 ] && ! grep -q "^SKIP" $work/run.log &&
  echo "$new" | grep -q "1 pipeline(s) kept.* 4 variant(s) made (at most 4 for one pipeline), 0 failed.* 0 draw(s) dropped; vkCmdSetVertexInputEXT: 0 passed to the driver, 6 held"
step $? "vidyn and packed formats imitated missing: $res; $(lg "$summary")"

server VKBRIDGE_FAKE_MISSING=bgr10vtx VKBRIDGE_EMU_VIDYN=0
run
[ $rc = 0 ] && grep -q "^SKIP the extension is not offered" $work/run.log && echo "$new" | grep -q "hidden from Zink (packed vertex fetch emulated)"
step $? "VKBRIDGE_EMU_VIDYN=0, BGRA 10-10-10-2 imitated missing: hidden from Zink as before ($(grep -m1 '^SKIP' $work/run.log))"

if [ $fails = 0 ]; then echo "OK: $steps steps"; else echo "FAILED: $fails of $steps steps"; exit 1; fi
