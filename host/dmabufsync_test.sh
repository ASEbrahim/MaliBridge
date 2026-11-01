#!/bin/bash
# Implicit sync for dma-bufs on kernels without dma-buf sync files (client/dmabuf_sync.c): 
# Galaxy Tab S9 FE+ (kernel 5.15) and a Kirin 9000 (5.10) showed Steam's GPU-drawn menus with parts
# of the frame missing at random: Zink's attach of the frame's end to the dma-buf does nothing
# there, so Xwayland and gamescope took the buffer while the GPU still drew it. The host kernel has
# the ioctls; VKBRIDGE_DMABUF_SYNC_FAKE_OLD=1 makes the client take it as one without them. A server
# of its own; tests/dmabufsync_check.c per step:
#   1  this kernel as it is: no wait (the kernel's own attach works), the sync file real;
#   2  a 5.x kernel imitated: the dma-buf export after the sync-file export returns with the frame
#      done; a sync-file export kept by the app, and a dma-buf export alone, do not wait;
#   3  the same with the feature off (VKBRIDGE_DMABUF_SYNC_MS=0): no wait (what the devices had);
#   4  the bound: VKBRIDGE_DMABUF_SYNC_MS=50 gives up at about 50 ms;
#   5  the switch from the server (<cache dir>/env's VKBRIDGE_DMABUF_SYNC_FAKE_OLD=1, nothing in the
#      app's environment): as 2.
# Prints one line per step and "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
work=$(mktemp -d)
sock=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-dmabufsync-$$.sock
host_icd=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json}
srv=
log=
stop() { [ -n "$srv" ] && kill $srv 2>/dev/null && wait $srv 2>/dev/null; srv=; }
start() { # start <name> [env...]: a server of its own, log $log, cache dir $cdir
  local d=$work/$1
  shift
  stop
  rm -f "$sock"
  cdir=$d/c
  mkdir -p "$cdir"
  [ -n "${ENVFILE:-}" ] && printf '%s\n' "$ENVFILE" >"$cdir/env"
  log=$d/vkbridge-server.log
  : > "$log"
  env -u VKBRIDGE_FAKE_MISSING -u VKBRIDGE_DMABUF_SYNC_MS -u VKBRIDGE_DMABUF_SYNC_FAKE_OLD \
      VKBRIDGE_PROBE_BUDGET_S=0 "$@" VK_DRIVER_FILES=$host_icd VK_ICD_FILENAMES= \
      "$out/vkbridge-server" --socket "$sock" --log "$log" --cache-dir "$cdir" >/dev/null 2>&1 &
  srv=$!
  for _ in $(seq 300); do [ -S "$sock" ] && break; kill -0 $srv 2>/dev/null || break; sleep 0.05; done
}
check() { # check <mode> <name> [env...]
  local m=$1 n=$2
  shift 2
  env "$@" timeout 120 "$out/dmabufsync_check" "$m" >"$work/check-$n.log" 2>&1
  tail -1 "$work/check-$n.log"
}
trap 'stop; rm -f "$sock"; [ -n "$KEEP" ] && echo "kept $work" || rm -rf "$work"' EXIT
export VKBRIDGE_SOCKET=$sock VK_DRIVER_FILES=$out/vkbridge_icd.json VK_ICD_FILENAMES=
unset VKBRIDGE_HIDE_EXTS VKBRIDGE_MAX_API VKBRIDGE_FAKE_MISSING VKBRIDGE_MODE VKBRIDGE_DMABUF_SYNC_MS VKBRIDGE_DMABUF_SYNC_FAKE_OLD
fails=0 steps=0
step() { steps=$((steps+1)); if [ "$1" = 0 ]; then echo "  ok   $2"; else echo "  FAIL $2"; fails=$((fails+1)); fi; }

start plain
c=$(check nowait host)
case "$c" in
SKIP*) echo "SKIP: $c"; exit 0 ;;
esac
grep -q "^OK" <<<"$c" && ! grep -q "implicit sync" "$work/check-host.log"
step $? "this kernel (dma-buf sync files): no wait, a real sync file, nothing said ($c)"

c=$(check wait old VKBRIDGE_DMABUF_SYNC_FAKE_OLD=1)
grep -q "^OK" <<<"$c" && grep -q "emulation: implicit sync for dma-bufs: the kernel has no dma-buf sync files" "$work/check-old.log"
step $? "a 5.x kernel imitated: the frame is done before it is handed over; the app's own sync file and a lone export do not wait ($c)"

c=$(check nowait off VKBRIDGE_DMABUF_SYNC_FAKE_OLD=1 VKBRIDGE_DMABUF_SYNC_MS=0)
grep -q "^OK" <<<"$c"; step $? "the same with VKBRIDGE_DMABUF_SYNC_MS=0: handed over unfinished, as before ($c)"

c=$(check timeout bound VKBRIDGE_DMABUF_SYNC_FAKE_OLD=1 VKBRIDGE_DMABUF_SYNC_MS=50)
grep -q "^OK" <<<"$c"; step $? "VKBRIDGE_DMABUF_SYNC_MS=50: the wait gives up at the bound ($c)"

ENVFILE=VKBRIDGE_DMABUF_SYNC_FAKE_OLD=1 start envfile
c=$(check wait envfile)
grep -q "^OK" <<<"$c"; step $? "VKBRIDGE_DMABUF_SYNC_FAKE_OLD=1 from <cache dir>/env reaches the app ($c)"
ENVFILE=VKBRIDGE_DMABUF_SYNC_MS=0 start envoff
c=$(check nowait envoff VKBRIDGE_DMABUF_SYNC_FAKE_OLD=1)
grep -q "^OK" <<<"$c"; step $? "VKBRIDGE_DMABUF_SYNC_MS=0 from <cache dir>/env turns it off ($c)"
stop

if [ $fails = 0 ]; then echo "OK: $steps steps"; else echo "FAILED: $fails of $steps steps"; [ -n "$KEEP" ] || KEEP=1; exit 1; fi
