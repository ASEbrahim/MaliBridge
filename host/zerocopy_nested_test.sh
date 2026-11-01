#!/bin/bash
# Zero-copy under a nested gamescope: a program presenting to gamescope's Wayland display
# (no banner_ahb_v1) gets gralloc swapchain images when the app's own compositor (VKBRIDGE_HOST_WAYLAND)
# offers banner_ahb_v1 version 3 with zero-copy on, and offers each one there; gamescope passes the same
# dma-bufs on, and the compositor pairs them. Two fake compositors (tests/fake_nested_wayland.py) stand
# in for gamescope and the app's compositor; the server's host stand-in for gralloc
# (VKBRIDGE_TEST_SCANOUT=1) writes each buffer's dma-buf into the offered socket.
#   1  offered: one offer per image, and the dma-bufs gamescope got are exactly the offered buffers
#      (inodes), with the offered size and row stride; all withdrawn when the swapchain goes;
#      the frames were presented
#   2  zero-copy off in the app's compositor (mode 0): no gralloc images, no offer, frames presented
#   3  an app's compositor without version 3 (version 2): no offer, frames presented
#   4  no VKBRIDGE_HOST_WAYLAND: no connection to the app's compositor at all, frames presented
#   5  VKBRIDGE_WSI_AHB=0: the same
#   6  a server that cannot make gralloc buffers (no stand-in): ordinary dma-bufs, no offer, frames presented
# Prints one line per step and "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
icd=${VKB_CLIENT_ICD:-$out/vkbridge_icd.json}
work=$(mktemp -d)
sock=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-zcnested-$$.sock
srv=
fake=
fails=0
stop() { [ -n "$srv" ] && kill $srv 2>/dev/null && wait $srv 2>/dev/null; srv=; }
stop_fake() { [ -n "$fake" ] && kill $fake 2>/dev/null && wait $fake 2>/dev/null; fake=; }
trap 'stop; stop_fake; rm -rf "$work" "$sock"' EXIT
start() { # start [env...]
  stop
  rm -f "$sock"
  mkdir -p "$work/c"
  env -u VKBRIDGE_FAKE_MISSING -u VKBRIDGE_HIDE_EXTS -u VKBRIDGE_ERRATA -u VKBRIDGE_ERRATA_IMITATE -u VKBRIDGE_MODE \
      -u VKBRIDGE_TEST_SCANOUT VKBRIDGE_PROBE_BUDGET_S=0 "$@" \
      VK_DRIVER_FILES=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json} VK_ICD_FILENAMES= \
      "$out/vkbridge-server" --socket "$sock" --log "$work/server.log" --cache-dir "$work/c" >/dev/null 2>&1 &
  srv=$!
  for _ in $(seq 300); do [ -S "$sock" ] && break; kill -0 $srv 2>/dev/null || break; sleep 0.05; done
}
fake_start() { # fake_start <log> [fake args...]
  stop_fake
  local log=$1; shift
  : > "$log"
  rm -f "$work/nested.sock" "$work/host.sock"
  python3 -I ../tests/fake_nested_wayland.py --nested "$work/nested.sock" --host "$work/host.sock" --log "$log" "$@" \
    > "$work/fake.out" 2>&1 &
  fake=$!
  for _ in $(seq 100); do grep -q ready "$work/fake.out" 2>/dev/null && break; sleep 0.05; done
}
run() { # run <name> [env...]: present_late_check on the "gamescope" display; $res its status
  local name=$1; shift
  : > "$work/$name.log"
  timeout 60 env -u VKBRIDGE_WSI_AHB -u VKBRIDGE_HOST_WAYLAND "$@" WAYLAND_DISPLAY="$work/nested.sock" \
    VKBRIDGE_SOCKET="$sock" VK_DRIVER_FILES="$icd" VKBRIDGE_CLIENT_LOG="$work/$name.log" VKBRIDGE_LOG_LEVEL=2 \
    "$out/present_late_check" ok --wayland --delay-ms 1 --destroy >"$work/$name.out" 2>&1
  res=$?
  sleep 0.3 # the fake's last lines
}
step() { if [ "$1" = 0 ]; then echo "ok   $2"; else echo "FAIL $2"; fails=$((fails+1)); fi; }
count() { grep -c "\"ev\": \"$2\"" "$1"; }
show() { sed 's/^/     /' "$work/$1.out" "$work/$1.log" "$2" | tail -15; }

if ! command -v python3 >/dev/null; then echo "SKIPPED: no python3"; exit 0; fi

# 1 offered
start VKBRIDGE_TEST_SCANOUT=1
fake_start "$work/f1.jsonl"
run t1 VKBRIDGE_HOST_WAYLAND="$work/host.sock"
r=$(python3 -I - "$work/f1.jsonl" <<'EOF'
import json, sys
ev = [json.loads(l) for l in open(sys.argv[1])]
offers = [e for e in ev if e["who"] == "host" and e["ev"] == "offer"]
bufs = [e for e in ev if e["who"] == "nested" and e["ev"] == "buffer"]
gone = {e["id"] for e in ev if e["who"] == "host" and e["ev"] == "withdraw"}
closed = any(e["who"] == "host" and e["ev"] == "disconnect" for e in ev)
offered = {i for o in offers for i in o["inos"]}
ok = (len(offers) >= 3 and len(bufs) == len(offers) and all(len(o["inos"]) == 1 for o in offers)
      and {b["ino"] for b in bufs} == offered and len(offered) == len(offers)
      and all(b["offset"] == 0 and b["modifier"] == 0 for b in bufs)
      and all(any(o["inos"][0] == b["ino"] and o["width"] == b["width"] and o["height"] == b["height"]
                  and o["stride"] * 4 == b["stride"] for o in offers) for b in bufs)
      and gone == {o["id"] for o in offers})
print("%s %d offers, %d buffers passed on, %d withdrawn%s" % ("yes" if ok else "no", len(offers), len(bufs), len(gone),
      ", connection closed" if closed else ""))
EOF
)
[ $res = 0 ] && [ "${r%% *}" = yes ] && grep -q 'takes offered gralloc buffers (zero-copy on)' "$work/t1.log" &&
  grep -q "offered to the app's compositor, which takes them as gamescope passes them on" "$work/t1.log" &&
  grep -q 'presented as gralloc dma-bufs (zero-copy layer)' "$work/t1.log"
step $? "1 under gamescope, the app's compositor zero-copy on: ${r#* } ($(grep -E '^(OK|FAILED)' "$work/t1.out" | tail -1))"
[ "${r%% *}" = yes ] && [ $res = 0 ] || show t1 "$work/f1.jsonl"

# 2 zero-copy off there
fake_start "$work/f2.jsonl" --host-mode 0
run t2 VKBRIDGE_HOST_WAYLAND="$work/host.sock"
[ $res = 0 ] && [ "$(count "$work/f2.jsonl" offer)" = 0 ] && [ "$(count "$work/f2.jsonl" buffer)" -ge 3 ] &&
  grep -q 'takes offered gralloc buffers (zero-copy off)' "$work/t2.log" && grep -q 'images, presented as dma-bufs$' "$work/t2.log"
step $? "2 the app's compositor with zero-copy off: ordinary dma-bufs, $(count "$work/f2.jsonl" offer) offers"
[ $res = 0 ] || show t2 "$work/f2.jsonl"

# 3 no version 3
fake_start "$work/f3.jsonl" --host-version 2
run t3 VKBRIDGE_HOST_WAYLAND="$work/host.sock"
[ $res = 0 ] && [ "$(count "$work/f3.jsonl" offer)" = 0 ] && [ "$(count "$work/f3.jsonl" buffer)" -ge 3 ] &&
  grep -q 'takes no offered gralloc buffers (no banner_ahb_v1 version 3)' "$work/t3.log" &&
  grep -q 'images, presented as dma-bufs$' "$work/t3.log"
step $? "3 an app's compositor with banner_ahb_v1 version 2: no offer, ordinary dma-bufs"
[ $res = 0 ] || show t3 "$work/f3.jsonl"

# 4 no VKBRIDGE_HOST_WAYLAND
fake_start "$work/f4.jsonl"
run t4
[ $res = 0 ] && ! grep -q '"who": "host"' "$work/f4.jsonl" && [ "$(count "$work/f4.jsonl" buffer)" -ge 3 ] &&
  grep -q 'images, presented as dma-bufs$' "$work/t4.log"
step $? "4 no VKBRIDGE_HOST_WAYLAND: the app's compositor never contacted, ordinary dma-bufs"
[ $res = 0 ] || show t4 "$work/f4.jsonl"

# 5 VKBRIDGE_WSI_AHB=0
fake_start "$work/f5.jsonl"
run t5 VKBRIDGE_HOST_WAYLAND="$work/host.sock" VKBRIDGE_WSI_AHB=0
[ $res = 0 ] && ! grep -q '"who": "host"' "$work/f5.jsonl" && grep -q 'images, presented as dma-bufs$' "$work/t5.log"
step $? "5 VKBRIDGE_WSI_AHB=0: the app's compositor never contacted, ordinary dma-bufs"
[ $res = 0 ] || show t5 "$work/f5.jsonl"

# 6 a server without gralloc
start
fake_start "$work/f6.jsonl"
run t6 VKBRIDGE_HOST_WAYLAND="$work/host.sock"
[ $res = 0 ] && [ "$(count "$work/f6.jsonl" offer)" = 0 ] && [ "$(count "$work/f6.jsonl" buffer)" -ge 3 ] &&
  grep -q 'the server cannot allocate AHardwareBuffers' "$work/t6.log" && grep -q 'images, presented as dma-bufs$' "$work/t6.log"
step $? "6 a server that makes no gralloc buffers: ordinary dma-bufs, no offer"
[ $res = 0 ] || show t6 "$work/f6.jsonl"
stop
stop_fake

if [ $fails = 0 ]; then echo "OK: zero-copy under a nested gamescope"; else echo "FAILED: $fails step(s)"; exit 1; fi
