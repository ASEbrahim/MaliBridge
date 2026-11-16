#!/bin/bash
# The Pixel 10 Pro XL's black second start (a player log, PowerVR DXT 25.1@6794074): the driver cannot
# reload the pipeline cache it wrote for gamescope (~6 MB): handed back, a build faults in the driver
# (NULL+0x930) and the lock it held stays held, so every later compute pipeline of that device spins
# until the compile watchdog (sessions 05, 07), or vkCreatePipelineCache itself allocates until the
# phone has no memory left; gamescope has no composite pipelines, the screen is black.
# The imitation takes the first form: one of gamescope's pipelines refused (-13) at a first start
# faults once its cache was made from saved data.
# Imitated beneath every layer (VKBRIDGE_FAKE_MISSING=ycbcrcache, server_spvfix.c) on the desktop
# driver, with ycbcr_compute_check as gamescope (the YCbCr pipeline first, then plain ones, all
# through the server's cache). A server per start; starts in one cache directory share the files:
#   1  the fix forced off (VKBRIDGE_YCBCR_SWAP=0), PowerVR's driver with VKBRIDGE_PCACHE_RELOAD=1 (the set-aside
#      acts on Imagination's driver only, and there only when saved data is handed back):
#      1a first start: the YCbCr pipeline refused, the plain ones built, the cache saved;
#      1b second start: the cache loaded, the YCbCr build faults, the next build spins and is cut off
#         (the phone's black start, reproduced); the cache is set aside (<name>.wedged.vkpc) and
#         nothing of this run is saved;
#      1c third start: starts without that file and builds as 1a (no fault, no timeout; what timed out in
#         1b was not noted as stuck, it waited on the lock the fault left);
#   2  PowerVR's driver (VKBRIDGE_FAKE_DRIVER=img, driverID IMAGINATION_PROPRIETARY), nothing forced,
#      four starts: no saved data is handed to it (the server says so), nothing is saved; every start
#      builds as a first one (the YCbCr pipeline refused, the plain ones built), no fault, no timeout;
#   3  Mali's way (VKBRIDGE_SPIRV_FIX=1, no imitation), two starts: the second start loads the saved
#      cache, nothing set aside (another driver keeps its cache);
#   4  PowerVR's driver with VKBRIDGE_PCACHE_RELOAD=1 (no imitation), two starts: the switch wins, the
#      second start loads the cache.
#   5  any other driver (no FAKE_DRIVER: Mali's way), the fix forced off, two starts: the second start loads
#      the cache, faults and spins as in 1b, and the cache is NOT set aside (kept as before this change).
# Prints one line per step and "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
work=$(mktemp -d)
sock=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-pcwedge-$$.sock
log=$work/server.log
srv=
stop() {
  if [ -n "$srv" ]; then
    kill $srv 2>/dev/null
    for _ in $(seq 50); do kill -0 $srv 2>/dev/null || break; sleep 0.1; done
    kill -9 $srv 2>/dev/null
    wait $srv 2>/dev/null
  fi
  srv=
}
# start <cache dir> <VAR=value...>: a new server with these in its environment, a fresh log
start() {
  stop
  local cache=$1
  shift
  mkdir -p "$cache"
  rm -f "$sock" "$log"
  env -u VKBRIDGE_FAKE_MISSING -u VKBRIDGE_HIDE_EXTS -u VKBRIDGE_ERRATA -u VKBRIDGE_ERRATA_IMITATE -u VKBRIDGE_MODE \
      -u VKBRIDGE_SPIRV_FIX -u VKBRIDGE_YCBCR_SWAP -u VKBRIDGE_SHL_BARRIER -u VKBRIDGE_NO_CRASH_GUARD -u VKBRIDGE_NO_PCACHE -u VKBRIDGE_FAKE_DRIVER -u VKBRIDGE_PCACHE_RELOAD \
      VKBRIDGE_TEST_PCACHE_GROW=1 VKBRIDGE_COMPILE_WARN_S=1 VKBRIDGE_COMPILE_TIMEOUT_S=2 VKBRIDGE_PROBE_BUDGET_S=0 "$@" \
      VK_DRIVER_FILES=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json} VK_ICD_FILENAMES= \
      timeout 300 "$out/vkbridge-server" --socket "$sock" --log "$log" --cache-dir "$cache" >/dev/null 2>&1 &
  srv=$!
  for _ in $(seq 100); do [ -S "$sock" ] && break; sleep 0.1; done
}
trap 'stop; rm -f "$sock"; [ -n "$KEEP" ] || rm -rf "$work"' EXIT INT TERM
export VKBRIDGE_SOCKET=$sock VK_DRIVER_FILES=$out/vkbridge_icd.json VK_ICD_FILENAMES=
unset VKBRIDGE_HIDE_EXTS VKBRIDGE_MAX_API
fails=0 steps=0
step() { steps=$((steps+1)); if [ "$1" = 0 ]; then echo "  ok   $2"; else echo "  FAIL $2"; fails=$((fails+1)); fi; }
# check [--expect-fail]: ycbcr_compute_check through the server, then the server stopped (its log
# then holds the device's end and its save); rc, res (the check's last line)
check() {
  timeout 120 "$out/ycbcr_compute_check" "$@" >$work/run.log 2>&1
  rc=$?
  res=$(tail -1 $work/run.log)
  sleep 0.5
  stop
  cp "$log" "$work/last.log" 2>/dev/null
  nstart=$((nstart+1))
  cp "$log" "$work/start$nstart.log" 2>/dev/null
  cp "$work/run.log" "$work/start$nstart.run" 2>/dev/null
}
nstart=0
has() { grep -q "$1" "$work/last.log"; }
faulted() { has "CONTAINED driver crash: signal 11 (address 0x930)"; }
spun() { has "TIMED OUT: vkCreateComputePipelines"; }
# the check's device was handed its saved cache (the "read and handed" line comes only with data)
loaded() {
  local d
  d=$(grep -o "device [0-9]*: pipeline cache [^ ]*ycbcr_compute[^ ]*\.vkpc (" "$work/last.log" | head -1 | grep -o "^device [0-9]*")
  [ -n "$d" ] && grep -q "$d: pipeline cache of [0-9]* KB read and handed to the driver" "$work/last.log"
}
set_aside() { has "set aside as .*wedged.vkpc"; }

# 1
c1=$work/c1
start "$c1" VKBRIDGE_FAKE_MISSING=ycbcrcache VKBRIDGE_YCBCR_SWAP=0 VKBRIDGE_FAKE_DRIVER=img VKBRIDGE_PCACHE_RELOAD=1
check --expect-fail
if [ $rc = 77 ]; then echo "SKIP: $res"; exit 0; fi
[ $rc = 0 ] && ! faulted && ! spun && has "imitating a driver that cannot build compute pipelines with immutable YCbCr samplers, and faults" &&
  ls "$c1"/*ycbcr_compute*.vkpc >/dev/null 2>&1
step $? "1a fix off, first start: the YCbCr pipeline refused, the plain ones built, the cache saved ($res)"
start "$c1" VKBRIDGE_FAKE_MISSING=ycbcrcache VKBRIDGE_YCBCR_SWAP=0 VKBRIDGE_FAKE_DRIVER=img VKBRIDGE_PCACHE_RELOAD=1
check --expect-fail
r1b=$rc
faulted && spun && loaded
step $? "1b fix off, second start: the cache loaded, the YCbCr build faults, the next build spins (the phone's black start, check rc $r1b)"
set_aside && ls "$c1"/*ycbcr_compute*.wedged.vkpc >/dev/null 2>&1 && ! ls "$c1"/*ycbcr_compute_c.vkpc "$c1"/*ycbcr_compute.vkpc >/dev/null 2>&1 &&
  ! grep -q "device [0-9]*: pipeline cache saved" <(grep -v "probe" "$work/last.log" | sed -n '/set aside/,$p')
step $? "1b the wedged cache set aside, nothing of that run saved after it ($(grep -o 'set aside as [^:]*' "$work/last.log" | head -1 | sed 's|.*/||'))"
start "$c1" VKBRIDGE_FAKE_MISSING=ycbcrcache VKBRIDGE_YCBCR_SWAP=0 VKBRIDGE_FAKE_DRIVER=img VKBRIDGE_PCACHE_RELOAD=1
check --expect-fail
[ $rc = 0 ] && ! faulted && ! spun
step $? "1c fix off, third start: without the set-aside file it builds as the first start ($res)"

# 2
c2=$work/c2
for n in 1 2 3 4; do
  start "$c2" VKBRIDGE_FAKE_MISSING=ycbcrcache VKBRIDGE_FAKE_DRIVER=img
  check --expect-fail
  ok=1
  [ $rc = 0 ] && ! faulted && ! spun && ! set_aside && ! loaded && has "imitating PowerVR's driver" &&
    has "saved pipeline caches are not handed to this driver" && ! grep -q "ycbcr_compute.*: pipeline cache saved\|device [0-9]*: pipeline cache saved" "$work/last.log" && ok=0
  step $ok "2 PowerVR's driver, start $n: nothing saved or handed back, built as a first start, no fault, no timeout ($res)"
done
ls "$c2"/*ycbcr_compute*.vkpc >/dev/null 2>&1
[ $? != 0 ]
step $? "2 no pipeline cache file written for the program under PowerVR's driver"

# 3
c3=$work/c3
for n in 1 2; do
  start "$c3" VKBRIDGE_SPIRV_FIX=1
  check
  ok=1
  [ $rc = 0 ] && ! faulted && ! spun && ! set_aside && ok=0
  if [ $ok = 0 ] && [ $n = 2 ]; then loaded || ok=1; fi
  step $ok "3 Mali's way, start $n: built$([ $n = 2 ] && echo ', the saved cache loaded'), nothing set aside ($res)"
done

# 4
c4=$work/c4
for n in 1 2; do
  start "$c4" VKBRIDGE_FAKE_DRIVER=img VKBRIDGE_PCACHE_RELOAD=1
  check
  ok=1
  [ $rc = 0 ] && ! faulted && ! spun && ok=0
  if [ $ok = 0 ] && [ $n = 2 ]; then loaded || ok=1; fi
  step $ok "4 PowerVR's driver with VKBRIDGE_PCACHE_RELOAD=1, start $n: built$([ $n = 2 ] && echo ', the switch wins: the saved cache loaded') ($res)"
done

# 5
c5=$work/c5
start "$c5" VKBRIDGE_FAKE_MISSING=ycbcrcache VKBRIDGE_YCBCR_SWAP=0
check --expect-fail
[ $rc = 0 ] && ! faulted && ! spun && ls "$c5"/*ycbcr_compute*.vkpc >/dev/null 2>&1
step $? "5 other driver, first start: built, the cache saved ($res)"
start "$c5" VKBRIDGE_FAKE_MISSING=ycbcrcache VKBRIDGE_YCBCR_SWAP=0
check --expect-fail
faulted && spun && loaded && ! set_aside && ! ls "$c5"/*wedged.vkpc >/dev/null 2>&1 && ls "$c5"/*ycbcr_compute*.vkpc >/dev/null 2>&1
step $? "5 other driver, second start: loaded, faulted, spun, and the cache kept, not set aside (as before)"

if [ $fails = 0 ]; then echo "OK: $steps steps"; exit 0; fi
echo "FAILED: $fails of $steps"
[ -n "$KEEP" ] && echo "kept: $work"
exit 1
