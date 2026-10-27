#!/bin/bash
# A frame whose GPU work takes longer than 10 s is presented late, not a lost device (Watch_Dogs
# on a Dimensity 8300 lost its device while loading, from the client's own 10 s limit;
# client/wsi.c vkb_wsi_wait_frame), with a server of its own (socket, cache directory):
#   1  headless surface (the synchronous present): a frame held 15 s is presented after 15 s, the
#      next frames too; the client warns once that a frame is slow and once that it came late;
#   2  Wayland surface, dma-buf images (the presenting thread): the same (skipped without a
#      compositor in WAYLAND_DISPLAY);
#   3  the limit in the guest (VKBRIDGE_PRESENT_TIMEOUT_S=3, a frame held 6 s): the device is lost,
#      said with the limit;
#   4  the limit from <cache dir>/env, through the server (Wayland, else headless): the same.
# VKB_CLIENT_ICD=<icd json> runs the checks against another client build (an older one fails 1 and 2).
# Prints one line per step and "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
icd=${VKB_CLIENT_ICD:-$out/vkbridge_icd.json}
work=$(mktemp -d)
sock=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-ptimeout-$$.sock
srv=
fails=0
stop() { [ -n "$srv" ] && kill $srv 2>/dev/null && wait $srv 2>/dev/null; srv=; }
trap 'stop; rm -rf "$work" "$sock"' EXIT
# start [env...]: a server with cache $work/c ($work/c/env as written before), log $work/server.log
start() {
  stop
  rm -f "$sock"
  mkdir -p "$work/c"
  env -u VKBRIDGE_FAKE_MISSING -u VKBRIDGE_HIDE_EXTS -u VKBRIDGE_ERRATA -u VKBRIDGE_ERRATA_IMITATE -u VKBRIDGE_MODE \
      -u VKBRIDGE_FAKE_DRIVER_VERSION -u VKBRIDGE_ERRATA_PROBE -u VKBRIDGE_PRESENT_TIMEOUT_S -u VKBRIDGE_HANG_DETECT \
      VKBRIDGE_PROBE_BUDGET_S=0 "$@" VK_DRIVER_FILES=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json} VK_ICD_FILENAMES= \
      "$out/vkbridge-server" --socket "$sock" --log "$work/server.log" --cache-dir "$work/c" >/dev/null 2>&1 &
  srv=$!
  for _ in $(seq 300); do [ -S "$sock" ] && break; kill -0 $srv 2>/dev/null || break; sleep 0.05; done
}
# check <name> <client log> [env...] -- <present_late_check args>: $res its exit status
check() {
  local name=$1 clog=$2; shift 2
  local e=()
  while [ "$1" != -- ]; do e+=("$1"); shift; done
  shift
  : > "$clog"
  timeout 90 env -u VKBRIDGE_PRESENT_TIMEOUT_S "${e[@]}" VKBRIDGE_SOCKET="$sock" VK_DRIVER_FILES="$icd" VKBRIDGE_CLIENT_LOG="$clog" VKBRIDGE_LOG_LEVEL=2 \
    "$out/present_late_check" "$@" >"$work/$name.out" 2>&1
  res=$?
}
step() { # step <status> <text>
  if [ "$1" = 0 ]; then echo "ok   $2"; else echo "FAIL $2"; fails=$((fails+1)); fi
}
last() { grep -E '^(OK|FAILED|SKIPPED)' "$work/$1.out" | tail -1; }

start
# 1 headless
check headless "$work/c1.log" -- ok --delay-ms 15000
[ $res = 0 ] && grep -q 'images, headless$' "$work/c1.log" && [ "$(grep -c 'a frame is taking more than 10 s to render' "$work/c1.log")" = 1 ] &&
  grep -q 'the slow frame rendered after 1[5-9]\.[0-9] s and was presented late' "$work/c1.log" &&
  ! grep -q 'device is reported lost' "$work/c1.log"
step $? "1 headless: a frame held 15 s presented late ($(last headless); $(grep -o 'rendered after [0-9.]* s' "$work/c1.log"))"
[ $res = 0 ] || sed 's/^/     /' "$work/headless.out" "$work/c1.log" | tail -12

# 2 Wayland (the presenting thread)
wl=0
if [ -n "${WAYLAND_DISPLAY:-}" ]; then
  check wayland "$work/c2.log" -- ok --wayland --delay-ms 15000
  if [ $res = 3 ]; then
    echo "skip 2 Wayland: $(last wayland)"
  else
    wl=1
    [ $res = 0 ] && grep -q 'images, presented as dma-bufs$' "$work/c2.log" &&
      grep -q 'the slow frame rendered after 1[5-9]\.[0-9] s and was presented late' "$work/c2.log" &&
      ! grep -q 'device is reported lost' "$work/c2.log"
    step $? "2 Wayland, dma-buf: a frame held 15 s presented late ($(last wayland); $(grep -o 'rendered after [0-9.]* s' "$work/c2.log"))"
    [ $res = 0 ] || sed 's/^/     /' "$work/wayland.out" "$work/c2.log" | tail -12
  fi
else
  echo "skip 2 Wayland: no WAYLAND_DISPLAY"
fi

# 3 the limit in the guest
check guest "$work/c3.log" VKBRIDGE_PRESENT_TIMEOUT_S=3 -- lost --delay-ms 6000
[ $res = 0 ] && grep -q 'a frame took more than 3 s to render; the device is reported lost' "$work/c3.log"
step $? "3 VKBRIDGE_PRESENT_TIMEOUT_S=3 in the guest, a frame held 6 s: lost ($(last guest))"

# 4 the limit from <cache dir>/env, through the server
echo "VKBRIDGE_PRESENT_TIMEOUT_S=3" > "$work/c/env"
start
if [ $wl = 1 ]; then a=--wayland; else a=; fi
check cacheenv "$work/c4.log" -- lost $a --delay-ms 6000
[ $res = 0 ] && grep -q 'a frame took more than 3 s to render; the device is reported lost' "$work/c4.log"
step $? "4 VKBRIDGE_PRESENT_TIMEOUT_S=3 in <cache dir>/env, ${a:-headless}: lost ($(last cacheenv))"
stop

if [ $fails = 0 ]; then echo "OK: slow frames presented late; the limit from the guest and from <cache dir>/env"
else echo "FAILED: $fails step(s)"; exit 1; fi
