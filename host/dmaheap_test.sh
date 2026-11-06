#!/bin/bash
# Phones without /dev/dma_heap (Retroid Pocket 4 Pro's Dimensity 1100, Huawei's Kirin 985; Mali-G77 r32p1/r34p0;
#, ledger row 1, d9d4f11): the driver exports no dma-bufs for images and the bridge's export
# emulation took its pages only from the dma-heap, so gamescope stopped at "no exportable modifier" and Steam
# mode sat at "Starting display and audio". The emulation now takes pages from another source where there is no
# dma-heap (Android's graphics buffers on a phone; on the host VKBRIDGE_TEST_EXPORT_PAGES=driver, the desktop
# driver's own dma-bufs, because the desktop's /dev/dma_heap is root's). VKBRIDGE_FAKE_MISSING=dmaheap imitates
# the phone's missing dma-heap, so this holds on any host whatever its /dev/dma_heap permissions.
# gsexport_check asks and makes the images as gamescope's nested backend does. A server of its own per step:
#   1  no dma-heap, the driver exporting nothing (dmabufexport), other pages: the export is emulated from those
#      pages and gamescope's images are exported (the fix)
#   2  the same with no other pages either: the export emulation is not claimed (no pages to back it), the server
#      says the dma-heap is missing and stays up (the desktop driver exports natively, so gamescope's check still
#      passes here; on the phone it is the "no exportable modifier" case)
#   3  nothing faked about the dma-heap (the host's own state): the faked-missing line does not appear
# Prints one line per step and "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
work=$(mktemp -d)
sock=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-dmaheap-$$.sock
host_icd=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json}
srv=
log=
stop() { [ -n "$srv" ] && kill $srv 2>/dev/null && wait $srv 2>/dev/null; srv=; }
start() { # start <name> [env...]: a server of its own, log $log
  local d=$work/$1
  shift
  stop
  rm -f "$sock"
  mkdir -p "$d/c"
  log=$d/vkbridge-server.log
  : > "$log"
  env -u VKBRIDGE_FAKE_MISSING -u VKBRIDGE_TEST_EXPORT_PAGES -u VKBRIDGE_MEMORY VKBRIDGE_PROBE_BUDGET_S=0 "$@" \
      VK_DRIVER_FILES=$host_icd VK_ICD_FILENAMES= \
      "$out/vkbridge-server" --socket "$sock" --log "$log" --cache-dir "$d/c" >/dev/null 2>&1 &
  srv=$!
  for _ in $(seq 300); do [ -S "$sock" ] && break; kill -0 $srv 2>/dev/null || break; sleep 0.05; done
}
check() { timeout 60 "$out/gsexport_check" >"$work/check-$1.log" 2>&1; echo $? >"$work/check-$1.rc"; }
trap 'stop; rm -f "$sock"; [ -n "$KEEP" ] && echo "kept $work" || rm -rf "$work"' EXIT
export VKBRIDGE_SOCKET=$sock VK_DRIVER_FILES=$out/vkbridge_icd.json VK_ICD_FILENAMES=
unset VKBRIDGE_HIDE_EXTS VKBRIDGE_MAX_API VKBRIDGE_FAKE_MISSING VKBRIDGE_MODE
fails=0 steps=0
step() { steps=$((steps+1)); if [ "$1" = 0 ]; then echo "  ok   $2"; else echo "  FAIL $2"; fails=$((fails+1)); [ -n "$3" ] && sed 's/^/       /' "$3" | tail -15; fi; }

start phone VKBRIDGE_FAKE_MISSING=dmabufexport,dmaheap VKBRIDGE_TEST_EXPORT_PAGES=driver
check phone
[ "$(cat "$work/check-phone.rc")" = 0 ] && grep -q "dmaheap: none, as on a phone without /dev/dma_heap" "$log" &&
  grep -q "dma-buf export: emulated, pages from the driver's own dma-bufs" "$log" &&
  ! grep -q "dmaheap: allocating from" "$log"
step $? "1 no dma-heap, no driver export, other pages: export emulated, gamescope's images exported" "$work/check-phone.log"

start neither VKBRIDGE_FAKE_MISSING=dmabufexport,dmaheap
check neither
grep -q "dmaheap: none, as on a phone without /dev/dma_heap" "$log" &&
  ! grep -q "dma-buf export: emulated" "$log" && kill -0 $srv 2>/dev/null
step $? "2 no dma-heap and no other pages: no export emulation claimed, the server stays up" "$work/check-neither.log"

start host VKBRIDGE_FAKE_MISSING=dmabufexport VKBRIDGE_TEST_EXPORT_PAGES=driver
check host
[ "$(cat "$work/check-host.rc")" = 0 ] && ! grep -q "dmaheap: none, as on a phone without /dev/dma_heap" "$log"
step $? "3 dma-heap not faked: the imitation line is absent (the host's own /dev/dma_heap state decides the rest)" "$work/check-host.log"

[ $fails = 0 ] && echo "OK: $steps steps" || { echo "FAILED: $fails of $steps steps"; exit 1; }
