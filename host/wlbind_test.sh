#!/bin/bash
# The swapchain's Wayland globals' first events (client/wsi_wayland.c wl_connect): the dma-buf formats and
# modifiers and the wl_shm formats are taken whatever roundtrip they arrive in. The fake gamescope
# (tests/fake_nested_wayland.py --late-registry-done) answers the registry's first sync only after it answered
# the client's binds, so those events come in the same roundtrip as the globals: the order a client sees when
# another of its threads reads the connection then (an old swapchain's presenting thread during a recreate;
# present_path_test steps 8/19 failed ~1 run in 10 so: "the compositor takes no XR24 dma-buf", then no swapchain).
# present_late_check --wayland --rgba, one server and one fake per step:
#   1  wl_shm (VKBRIDGE_WSI_SHM=1), the ordinary order: the compositor's XBGR8888 taken, not swizzled;
#   2  the same with the formats in the registry's roundtrip;
#   3  dma-bufs (the server's memfd stand-ins for gralloc, VKBRIDGE_TEST_SCANOUT=memfd, as present_path_test),
#      the ordinary order: presented as dma-bufs, the fake got the buffers;
#   4  the same with the formats and modifiers in the registry's roundtrip.
# 3 and 4 are not run (said so) where the server binary has no memfd stand-in.
#   5  the fake compositor outlives a client that closes before its answers went out: Mesa's device_select layer
#      closes its own look at the display right after a sync's done, and the fake sends done and delete_id
#      apart; it died there (BrokenPipeError) and the program's own connection then found "no Wayland compositor".
# Prints one line per step and "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
icd=${VKB_CLIENT_ICD:-$out/vkbridge_icd.json}
work=$(mktemp -d "${XDG_RUNTIME_DIR:-/tmp}/vkb-wlb.XXXX") # short: socket paths
sock=$work/s.sock
srv=
fake=
fails=0
stop() { [ -n "$srv" ] && kill $srv 2>/dev/null && wait $srv 2>/dev/null; srv=; }
stop_fake() { [ -n "$fake" ] && kill $fake 2>/dev/null && wait $fake 2>/dev/null; fake=; }
trap 'stop; stop_fake; [ -n "${VKB_KEEP:-}" ] && echo "kept $work" || rm -rf "$work"' EXIT
start() { # start <name> [env...]: a server of its own
  stop
  local name=$1; shift
  rm -f "$sock"
  mkdir -p "$work/c"
  env -u VKBRIDGE_FAKE_MISSING -u VKBRIDGE_HIDE_EXTS -u VKBRIDGE_ERRATA -u VKBRIDGE_ERRATA_IMITATE -u VKBRIDGE_MODE \
      -u VKBRIDGE_TEST_SCANOUT VKBRIDGE_PROBE_BUDGET_S=0 "$@" \
      VK_DRIVER_FILES=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json} VK_ICD_FILENAMES= \
      "$out/vkbridge-server" --socket "$sock" --log "$work/$name.server.log" --cache-dir "$work/c" >/dev/null 2>&1 &
  srv=$!
  for _ in $(seq 300); do [ -S "$sock" ] && break; kill -0 $srv 2>/dev/null || break; sleep 0.05; done
}
fake_start() { # fake_start <name> [fake args...]: the fake's log $work/<name>.jsonl, its output $work/<name>.fake
  stop_fake
  local name=$1; shift
  : > "$work/$name.jsonl"
  rm -f "$work/nested.sock" "$work/host.sock"
  python3 -I ../tests/fake_nested_wayland.py --nested "$work/nested.sock" --host "$work/host.sock" --log "$work/$name.jsonl" "$@" \
    > "$work/$name.fake" 2>&1 &
  fake=$!
  for _ in $(seq 400); do grep -q ready "$work/$name.fake" 2>/dev/null && break; kill -0 $fake 2>/dev/null || break; sleep 0.05; done
  grep -q ready "$work/$name.fake" 2>/dev/null || { echo "FAIL the fake compositor did not start: $(tail -2 "$work/$name.fake")"; fails=$((fails+1)); }
}
run() { # run <name> [env...]: present_late_check on the fake gamescope's display; $res its status, $clog the client log
  local name=$1; shift
  clog=$work/$name.client.log
  : > "$clog"
  timeout 60 env -u VKBRIDGE_WSI_AHB -u VKBRIDGE_WSI_SHM -u VKBRIDGE_HOST_WAYLAND -u DISPLAY "$@" WAYLAND_DISPLAY="$work/nested.sock" \
    VKBRIDGE_SOCKET="$sock" VK_DRIVER_FILES="$icd" VK_ICD_FILENAMES= VKBRIDGE_CLIENT_LOG="$clog" VKBRIDGE_LOG_LEVEL=2 \
    "$out/present_late_check" ok --wayland --rgba --delay-ms 1 >"$work/$name.out" 2>&1
  res=$?
  sleep 0.2 # the fake's last lines
}
step() { if [ "$1" = 0 ]; then echo "ok   $2"; else echo "FAIL $2"; fails=$((fails+1)); fi; }
last() { grep -E '^(OK|FAILED|SKIPPED)' "$work/$1.out" | tail -1; }
show() { sed 's/^/     /' "$work/$1.out" | tail -6; grep -E 'Wayland|error|warn' "$work/$1.client.log" 2>/dev/null | head -6 | sed 's/^/     client: /'; }
count() { grep -c "\"ev\": \"$2\"" "$work/$1.jsonl"; }
shmfmt() { grep -o 'through wl_shm (format 0x[0-9a-f]*[^)]*)' "$work/$1.client.log" | head -1; }
# XBGR8888 (0x34324258) for the RGBA swapchain: the compositor's formats were taken; XRGB8888 swizzled (0x1) means none were
shm_ok() { [ $res = 0 ] && grep -q 'through wl_shm (format 0x34324258)' "$work/$1.client.log" &&
  grep -q '"ev": "shm_buffer".*"format": 875709016' "$work/$1.jsonl" && ! grep -q '"ev": "shm_buffer".*"format": 1}' "$work/$1.jsonl"; }
dmabuf_ok() { [ $res = 0 ] && grep -q 'presented as gralloc dma-bufs' "$work/$1.client.log" && [ "$(count "$1" buffer)" -ge 3 ] &&
  ! grep -q 'takes no .* dma-buf' "$work/$1.client.log"; }

if ! command -v python3 >/dev/null; then echo "SKIPPED: no python3"; exit 0; fi

start shm
fake_start shm --shm
run shm VKBRIDGE_WSI_SHM=1
shm_ok shm; s=$?
step $s "1 wl_shm, formats after the registry's roundtrip: $(shmfmt shm), $(last shm)"; [ $s = 0 ] || show shm

start shmlate
fake_start shmlate --shm --late-registry-done
run shmlate VKBRIDGE_WSI_SHM=1
shm_ok shmlate; s=$?
step $s "2 wl_shm, formats in the registry's roundtrip: $(shmfmt shmlate), $(last shmlate)"; [ $s = 0 ] || show shmlate

# The memfd stand-in (server/server_memory.c test_scanout_alloc) is in the server binary or not: decided by
# that, not by step 3's result, so a failing step 3 is never taken for "not here".
if ! grep -aq 'VKBRIDGE_TEST_SCANOUT=%s' "$out/vkbridge-server"; then
  echo "not run 3, 4 dma-bufs: this server has no memfd stand-in for gralloc (lavapipe gives no dma-bufs)"
else
  start dmabuf VKBRIDGE_TEST_SCANOUT=memfd
  fake_start dmabuf
  run dmabuf VKBRIDGE_HOST_WAYLAND="$work/host.sock"
  dmabuf_ok dmabuf; s=$?
  step $s "3 dma-bufs, formats after the registry's roundtrip: $(count dmabuf buffer) buffers, $(last dmabuf)"; [ $s = 0 ] || show dmabuf
  start dmabuflate VKBRIDGE_TEST_SCANOUT=memfd
  fake_start dmabuflate --late-registry-done
  run dmabuflate VKBRIDGE_HOST_WAYLAND="$work/host.sock"
  dmabuf_ok dmabuflate; s=$?
  step $s "4 dma-bufs, formats and modifiers in the registry's roundtrip: $(count dmabuflate buffer) buffers, $(last dmabuflate)"
  [ $s = 0 ] || show dmabuflate
fi
start gone
fake_start gone --shm
python3 -I - "$work/nested.sock" <<'EOF'
import socket, struct, sys
s = socket.socket(socket.AF_UNIX)
s.connect(sys.argv[1])
s.sendall(b"".join(struct.pack("<III", 1, 12 << 16, 2 + i) for i in range(5000)))  # wl_display.sync; the answers not read
s.close()
EOF
sleep 0.3
run gone VKBRIDGE_WSI_SHM=1
shm_ok gone; s=$?
step $s "5 a client gone before its answers, the next client served: $(shmfmt gone), $(last gone)"
[ $s = 0 ] || { show gone; tail -2 "$work/gone.fake" | sed 's/^/     fake: /'; }
stop
stop_fake

if [ $fails = 0 ]; then echo "OK: the Wayland globals' first events taken in any roundtrip"; else echo "FAILED: $fails step(s)"; exit 1; fi
