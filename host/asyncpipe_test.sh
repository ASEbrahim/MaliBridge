#!/bin/bash
# The asynchronous pipeline compile (server_async.c, VKBRIDGE_ASYNC_COMPILE) on the desktop driver,
# with a server of its own (its own socket and cache directory; the switches go in <cache dir>/env,
# re-read at each device) and a slow compiler imitated (VKBRIDGE_ASYNC_COMPILE_IMITATE_MS=400):
#   1  the shader scan alone (effects_corpus);
#   2  off (the default): the colour pipeline's create waits for the compile, drawn at once;
#   3  on: a colour pipeline comes back at once and its draws are skipped until it is compiled;
#      depth-only and no-vertex-input pipelines stay synchronous; modules and the layout destroyed
#      during the compile, a pipeline destroyed while compiling, one left at vkDestroyDevice; the
#      server's summary counts the skipped draws and the synchronous reasons;
#      =2: the pipeline without vertex input goes to the background too;
#   4  on, a compile within the wait: the real pipeline back, drawn at once;
#   5  on with no imitated delay and a long wait (every pipeline ready in time): smoke, cullmode,
#      dynstate and flicker checks pass through the layer's wrappers unchanged;
#   6  the server is alive and logged no protocol errors; the protocol hash is unchanged.
# Prints one line per step and "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
work=$(mktemp -d)
cache=$work/c
mkdir -p "$cache"
sock=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-async-$$.sock
log=$work/server.log
srv=
start() {
  [ -n "$srv" ] && kill $srv 2>/dev/null && wait $srv 2>/dev/null
  rm -f "$sock"
  env -u VKBRIDGE_FAKE_MISSING -u VKBRIDGE_HIDE_EXTS -u VKBRIDGE_ERRATA -u VKBRIDGE_ERRATA_IMITATE -u VKBRIDGE_MODE \
      -u VKBRIDGE_ASYNC_COMPILE -u VKBRIDGE_ASYNC_COMPILE_IMITATE_MS -u VKBRIDGE_ASYNC_COMPILE_WAIT_MS "$@" \
      VK_DRIVER_FILES=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json} VK_ICD_FILENAMES= \
      "$out/vkbridge-server" --socket "$sock" --log "$log" --cache-dir "$cache" >/dev/null 2>&1 &
  srv=$!
  for _ in $(seq 50); do [ -S "$sock" ] && break; sleep 0.1; done
}
trap 'kill $srv 2>/dev/null; rm -f "$sock"; [ -n "$KEEP" ] || rm -rf "$work"' EXIT
export VKBRIDGE_SOCKET=$sock VK_DRIVER_FILES=$out/vkbridge_icd.json VK_ICD_FILENAMES=
unset VKBRIDGE_HIDE_EXTS VKBRIDGE_MAX_API
fails=0 steps=0
step() { steps=$((steps+1)); if [ "$1" = 0 ]; then echo "  ok   $2"; else echo "  FAIL $2"; fails=$((fails+1)); fi; }

# run <env lines...> -- <program args...>: $rc, $res (last line), $new (the server's log lines)
run() {
  local envs=()
  while [ "$1" != -- ]; do envs+=("$1"); shift; done
  shift
  printf '%s\n' "${envs[@]}" > "$cache/env"
  local l; l=$(wc -l < "$log")
  timeout 120 "$out/$@" >$work/run.log 2>&1
  rc=$?
  res=$(tail -1 $work/run.log)
  new=$(tail -n +$((l+1)) "$log")
}

# 1
"$out/effects_corpus" >$work/fx.log 2>&1
step $? "effects_corpus: $(tail -1 $work/fx.log)"

start
# 2
run VKBRIDGE_ASYNC_COMPILE_IMITATE_MS=400 -- asyncpipe_check sync
[ $rc = 0 ] && echo "$new" | grep -q "graphics pipeline compiles slowed down by 400 ms" && ! echo "$new" | grep -q "asynchronous pipeline compile on"
step $? "off: $(grep -m1 '^[PF]A[SI][SL] ' $work/run.log | cut -c1-140)"
# 3
run VKBRIDGE_ASYNC_COMPILE=1 VKBRIDGE_ASYNC_COMPILE_IMITATE_MS=400 -- asyncpipe_check async
sum=$(echo "$new" | grep -o 'async compile, device [0-9]*: .*' | tail -1)
[ $rc = 0 ] && echo "$new" | grep -q "asynchronous pipeline compile on" &&
  echo "$sum" | grep -qE ' [1-9][0-9]* draws skipped' && echo "$sum" | grep -q 'no colour output 1' && echo "$sum" | grep -q 'no vertex input 1'
step $? "on, slow compiles: $res"
grep '^[PF]A[SI][SL] ' $work/run.log | sed 's/^/       /'
echo "       server: ${sum:-no summary}" | cut -c1-260
# 3b
run VKBRIDGE_ASYNC_COMPILE=2 VKBRIDGE_ASYNC_COMPILE_IMITATE_MS=400 -- asyncpipe_check async2
sum=$(echo "$new" | grep -o 'async compile, device [0-9]*: .*' | tail -1)
[ $rc = 0 ] && echo "$new" | grep -q "pipelines without vertex input too" && echo "$sum" | grep -q 'no colour output 1' &&
  ! echo "$sum" | grep -q 'no vertex input'
step $? "on (2), pipelines without vertex input in the background too: $res"
# 4
run VKBRIDGE_ASYNC_COMPILE=1 VKBRIDGE_ASYNC_COMPILE_WAIT_MS=2000 -- asyncpipe_check quick
sum=$(echo "$new" | grep -o 'async compile, device [0-9]*: .*' | tail -1)
[ $rc = 0 ] && echo "$sum" | grep -q ' 0 pipelines in the background' && echo "$sum" | grep -qE ' [1-9][0-9]* ready within'
step $? "on, compiled within the wait: $res"
# 5
r5=0 res5=
for p in vkbridge-smoke cullmode_check dynstate_check flicker_check; do
  run VKBRIDGE_ASYNC_COMPILE=1 VKBRIDGE_ASYNC_COMPILE_WAIT_MS=60000 -- $p
  [ $rc = 0 ] || r5=1
  res5="$res5 $p: $(echo "$res" | cut -c1-40);"
done
step $r5 "on, every pipeline in time:$res5"

# 6
kill -0 $srv 2>/dev/null && ! grep -qE "command stream:|malformed request|unknown command id|the driver has no|caught signal|contained crash" "$log"
step $? "server alive, no protocol errors or crashes"
grep -q "^#define VKB_PROTOCOL_HASH 0x4a9da939fdf65896ULL" ../common/gen/vkb_gen.h
step $? "protocol hash $(grep -o '0x[0-9a-f]*' ../common/gen/vkb_gen.h | head -1)"

if [ $fails = 0 ]; then echo "OK: $steps steps"; else echo "FAILED: $fails of $steps steps"; exit 1; fi
