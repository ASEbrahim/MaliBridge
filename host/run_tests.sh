#!/bin/bash
# Host regression suite for the bridge: every automated check, in two server modes -
#   plain   the desktop driver as it is (RADV)
#   mali    the desktop driver with Mali-G720's known gaps imitated (mali_env.sh)
# Needs: make (out/), Proton Experimental for the DXVK check (VKB_WINE_DIR prepared, see README),
# docker image vkb-arch-gamescope for the gamescope/Zink check (skipped if absent).
# Runs side by side with other runs (other worktrees, other agents): each run has a directory of its
# own for its server's socket and log (VKB_RUN_DIR; the log is copied to out/server.log at the end)
# and stops only the servers it started. Two runs in one worktree take turns (out/ is shared).
cd "$(dirname "$0")"
mkdir -p out
exec 9>out/.run_tests.lock
flock -n 9 || { echo "(another run in this worktree; waiting)"; flock 9; }
VKB_RUN_DIR=$(mktemp -d "${XDG_RUNTIME_DIR:-/tmp}/vkbridge-suite-$(basename "$(cd .. && pwd)")-$$.XXXX")
export VKB_RUN_DIR VKBRIDGE_SOCKET=$VKB_RUN_DIR/server.sock VKB_SERVER_LOG=$VKB_RUN_DIR/server.log
unset VKBRIDGE_CACHE_DIR VKB_SERVER_PID
. ./server_start.sh
trap 'vkb_server_stop; cp -f "$VKB_SERVER_LOG" out/server.log 2>/dev/null; rm -rf "$VKB_RUN_DIR"' EXIT
pass=0; fail=0
result() { if [ "$1" = 0 ]; then pass=$((pass+1)); echo "  ok   $2"; else fail=$((fail+1)); echo "  FAIL $2"; fi; }
# A server-side protocol error fails the run even when the desktop driver shrugs it off (a missing
# count in vkBeginCommandBuffer's encoding passed every check on RADV and crashed Mali).
server_errors() {
  local n; n=$(grep -cE "command stream:|malformed request|unknown command id|the driver has no|named ahead failed|bad batch entry" "$VKB_SERVER_LOG" 2>/dev/null)
  [ "${n:-0}" = 0 ] || { echo "  server log: $n protocol errors, first: $(grep -m1 -E "command stream:|malformed request|unknown command id|the driver has no|named ahead failed|bad batch entry" "$VKB_SERVER_LOG")"; return 1; }
}
run_mode() {
  local mode=$1
  echo "== $mode"
  : > "$VKB_SERVER_LOG"
  if [ "$mode" = mali ]; then . ./mali_env.sh >/dev/null; else unset VKBRIDGE_HIDE_EXTS VKBRIDGE_MAX_API; . ./bridge_env.sh >/dev/null; fi
  timeout 120 ./out/vkbridge-smoke >out/t-smoke.log 2>&1; result $? "smoke ($(tail -1 out/t-smoke.log))"
  if [ "$mode" = mali ]; then
    # Mali's memory types (lazymem): images get an ordinary device-local type, never the lazily
    # allocated one (an older client did: image checks failed at "setup", gamescope got -2)
    grep -q "imitating Mali's memory types" "$VKB_SERVER_LOG" && ! grep -q "lazily allocated, imitated" "$VKB_SERVER_LOG" &&
      ! grep -q "no ordinary device-local type" out/t-smoke.log
    result $? "smoke, Mali's memory types: images in ordinary memory ($(grep -m1 -o 'first allocation in memory type [0-9]* (flags 0x[0-9a-f]*, heap [0-9]*): [0-9]* bytes, plain' "$VKB_SERVER_LOG"))"
    # The XRing O3's image requirements (xringmem, issue #1): its images do not list the first
    # ordinary device-local type, so the client's device-local type is backed by one they do list
    # (the client's mask for every image was empty: "no memory type for the image (types 0x0 allowed)")
    VKB_MISSING_ADD=xringmem . ./mali_env.sh >/dev/null
    timeout 120 ./out/vkbridge-smoke >out/t-smoke-xring.log 2>&1 && grep -q "imitating the XRing O3" "$VKB_SERVER_LOG" &&
      ! grep -q "no memory type for the image" out/t-smoke-xring.log && ! grep -q "lazily allocated, imitated" "$VKB_SERVER_LOG"
    result $? "smoke, XRing O3's image requirements: images get a memory type ($(tail -1 out/t-smoke-xring.log))"
    . ./mali_env.sh >/dev/null
    # Other Mali drivers: push descriptors but not robustness2 (nulls in push descriptor writes are
    # replaced too), no multiDrawIndirect (Mali-G57 r32/r38: indirect draws are split)
    VKB_MISSING=robustness2,multidraw . ./mali_env.sh >/dev/null
    VKBRIDGE_HIDE_EXTS=VK_EXT_robustness2,VK_KHR_robustness2 timeout 120 ./out/vkbridge-smoke >out/t-smoke-push.log 2>&1 &&
      grep -q "null descriptors in push descriptor writes replaced" "$VKB_SERVER_LOG" &&
      grep -q "indirect draws split" "$VKB_SERVER_LOG"
    result $? "smoke, native push descriptors without robustness2, no multiDrawIndirect ($(tail -1 out/t-smoke-push.log))"
    . ./mali_env.sh >/dev/null
    # BC transcoded to ETC2/EAC/ASTC (compared by PSNR), then decoded as the fallback does (exact)
    timeout 180 ./out/bcn_check transcode >out/t-bcn.log 2>&1; result $? "bcn_check, BC transcoded ($(tail -1 out/t-bcn.log))"
    VKB_BC_TRANSCODE=0 . ./mali_env.sh >/dev/null
    timeout 180 ./out/bcn_check >out/t-bcn-decode.log 2>&1; result $? "bcn_check, BC decoded ($(tail -1 out/t-bcn-decode.log))"
    . ./mali_env.sh >/dev/null
  else
    timeout 180 ./out/bcn_check >out/t-bcn.log 2>&1; result $? "bcn_check ($(tail -1 out/t-bcn.log))"
  fi
  # Null images of every shape (cube, 3D, 1D, arrays, uint/sint): on mali each gets a stand-in of
  # its binding's shape, from the pipeline's SPIR-V
  if [ -x out/nulldesc_check ]; then
    timeout 60 ./out/nulldesc_check >out/t-nulldesc.log 2>&1 &&
      { [ "$mode" != mali ] || [ "$(grep -c 'null descriptor stand-in made' "$VKB_SERVER_LOG")" -ge 6 ]; }
    result $? "nulldesc_check ($(tail -1 out/t-nulldesc.log)$([ "$mode" = mali ] && echo "; $(grep -c 'null descriptor stand-in made' "$VKB_SERVER_LOG") stand-in shapes"))"
  fi
  if [ -x out/opaque_check ]; then timeout 60 ./out/opaque_check >out/t-opaque.log 2>&1; result $? "opaque_check ($(tail -1 out/t-opaque.log))"; fi
  # Zink's shared linear images (dma-buf, no modifier): Mali refuses them at that tiling (imitated:
  # dmabuflinear) and Skyrim SE exited on its 1D one; the bridge makes them without the handle type
  if [ -x out/dmabuf_image_check ]; then
    timeout 60 ./out/dmabuf_image_check >out/t-dmabuf-image.log 2>&1 &&
      { [ "$mode" != mali ] || grep -q "dma-buf image refused at tiling" "$VKB_SERVER_LOG"; }
    result $? "dmabuf_image_check ($(tail -1 out/t-dmabuf-image.log))"
  fi
  if [ -x out/recycle_check ]; then timeout 120 ./out/recycle_check >out/t-recycle.log 2>&1; result $? "recycle_check ($(tail -1 out/t-recycle.log))"; fi
  # Descriptor traffic, batched (vkbBatch, packed updates) and not; the client's totals in the line
  if [ -x out/desc_traffic_check ]; then
    for nb in 0 1; do
      VKBRIDGE_DESC_BATCH=$((1-nb)) VKBRIDGE_STATS=100000 timeout 120 ./out/desc_traffic_check >out/t-desc-$nb.log 2>&1
      result $? "desc_traffic_check, VKBRIDGE_NO_DESC_BATCH=$nb ($(tail -1 out/t-desc-$nb.log | grep -o '^[A-Z]*'); $(grep -o '[0-9]* messages, [0-9]* KB on the wire' out/t-desc-$nb.log))"
    done
  fi
  # DXVK's frame structure (frames in flight, pools reset per frame, submits from another thread)
  if [ -x out/flicker_check ]; then
    for nb in 0 1; do
      VKBRIDGE_DESC_BATCH=$((1-nb)) timeout 180 ./out/flicker_check >out/t-flicker-$nb.log 2>&1
      result $? "flicker_check, VKBRIDGE_NO_DESC_BATCH=$nb ($(grep -o '[0-9]* draws wrong in [0-9]* frames' out/t-flicker-$nb.log))"
    done
  fi
  if [ -n "${VKB_WINE_DIR:-}" ] && [ -d "$VKB_WINE_DIR/app" ]; then
    ./d3d11_test.sh >out/t-d3d11.log 2>&1; grep -q "^OK:" out/t-d3d11.log; result $? "d3d11_check via DXVK ($(grep -E '^(OK|FAILED)' out/t-d3d11.log))"
    # Every frame read back, through DXVK, with batching on (needs d3d11_flicker.exe in the prefix)
    if [ -f "$VKB_WINE_DIR/app/d3d11_flicker.exe" ]; then
      VKBRIDGE_DESC_BATCH=1 ./d3d11_test.sh d3d11_flicker.exe >out/t-d3d11-flicker.log 2>&1; grep -q "^OK" out/t-d3d11-flicker.log
      result $? "d3d11_flicker via DXVK, batched ($(grep -o '[0-9]* cells wrong in [0-9]* frames' out/t-d3d11-flicker.log))"
    fi
  fi
  # Persistent pipeline cache: Steam app keys, pre-caching taken in, killed games, shared files
  # (a server of its own with a cache directory; once, the mode makes no difference to it)
  if [ "$mode" = plain ] && [ -x out/pcache_check ]; then
    timeout 120 ./pcache_test.sh >out/t-pcache.log 2>&1; result $? "pcache_test ($(tail -1 out/t-pcache.log))"
  fi
  # Waits the driver fails with -3 after a signal (wait_retry, MK-01): every kind the bridge
  # forwards, the bug imitated beneath the fix, with it and without; and the server-side imitation
  # of an older Mali's feature set, both from <cache dir>/env (a server of its own; once)
  if [ "$mode" = plain ]; then
    timeout 300 ./wait_imitate_test.sh >out/t-wait.log 2>&1; result $? "wait_imitate_test ($(tail -1 out/t-wait.log))"
    # A pipeline compile that never ends (the G925's compiler on Stray): interrupted, the call fails
    timeout 400 ./compile_watchdog_test.sh >out/t-spin.log 2>&1; result $? "compile_watchdog_test ($(tail -1 out/t-spin.log))"
    # UE4's layer-only pass-through geometry stage folded into the vertex shader (Stray's stuck
    # pipelines): up front with VKBRIDGE_LAYER_GS_FOLD=1, and as the never-ending compile's fallback
    timeout 300 ./gsfold_test.sh >out/t-gsfold.log 2>&1; result $? "gsfold_test ($(tail -1 out/t-gsfold.log))"
    # The startup probe (imitated failures switch their emulations on, a dead start resumes, a
    # small budget defers to the background) and the session report with its diff tool
    timeout 300 ./probe_report_test.sh >out/t-probe.log 2>&1; result $? "probe_report_test ($(tail -1 out/t-probe.log))"
    # A start that ends inside a probe: a stop or a kill runs it again, an abort or a hang past the
    # watchdog is died and decided; the default budget defers the heavy probes, both parts timed
    timeout 300 ./probe_end_test.sh >out/t-probe-end.log 2>&1; result $? "probe_end_test ($(tail -1 out/t-probe-end.log))"
    # The driver-errata layer: decisions by driver version, mode, probe and switch; a fix with its
    # imitation beneath it, the bridge's own calls through both (a server of its own; once)
    timeout 300 ./errata_test.sh >out/t-errata.log 2>&1; result $? "errata_test ($(tail -1 out/t-errata.log))"
    # MK-03 bitwise_scalar: the miscompile imitated fails bitwise_check, the SPIR-V rewrite over it
    # passes; only modules with the pattern change; the corpus through the pass is spirv-val clean
    timeout 600 ./bitwise_test.sh >out/t-bitwise.log 2>&1; result $? "bitwise_test ($(tail -1 out/t-bitwise.log); $(grep -o 'corpus: [0-9]* modules' out/t-bitwise.log))"
    # shl_barrier: the no-op after constant left shifts, forced on: right values in bitwise_check and
    # vkbridge-smoke, modules named by the app's code, the corpus through the pass spirv-val clean
    timeout 600 ./shlbarrier_test.sh >out/t-shlbarrier.log 2>&1; result $? "shlbarrier_test ($(tail -1 out/t-shlbarrier.log))"
    # MK-32 clamp_zero: the zero limit dropped (imitated) fails clampzero_check, the opaque zero over it
    # passes, narrow from the version gate and wide when forced; the corpus through both is spirv-val clean
    timeout 600 ./clampzero_test.sh >out/t-clampzero.log 2>&1; result $? "clampzero_test ($(tail -1 out/t-clampzero.log); $(grep -o 'corpus, narrow: [0-9]* modules' out/t-clampzero.log))"
    # The image-view errata: MK-07's REMAINING counts and MK-06's view quarantine, each failing with
    # its bug imitated and the fix off and passing with the fix; the quarantine's caps and the
    # server's RSS over 40000 churned views (a server of its own; once)
    timeout 300 ./views_test.sh >out/t-views.log 2>&1; result $? "views_test ($(tail -1 out/t-views.log))"
    # The command-buffer state errata: MK-04's dynamic state sent again after binds and MK-05's
    # split vertex buffer binds, each failing with its bug imitated and the fix off and passing
    # with the fix; the two stride shapes the probe tells apart (a server of its own; once)
    timeout 300 ./cmdstate_test.sh >out/t-cmdstate.log 2>&1; result $? "cmdstate_test ($(tail -1 out/t-cmdstate.log))"
    # MK-16 cull_nontri (a dynamic cull mode culls points and lines): failing with its bug imitated
    # and the fix off, passing with the fix; the version gate (a server of its own; once)
    timeout 300 ./cullmode_test.sh >out/t-cullmode-test.log 2>&1; result $? "cullmode_test ($(tail -1 out/t-cullmode-test.log))"
    # MK-39 dedicated_import (a dedicated struct on an import into a resource not made for its handle
    # type): failing with its bug imitated and the fix off, passing with the fix; the version gate
    timeout 300 ./dedimport_test.sh >out/t-dedimport.log 2>&1; result $? "dedimport_test ($(tail -1 out/t-dedimport.log))"
    # The MK probes: each erratum's bug imitated beneath its probe makes it fail with the bug's
    # pattern and the decision follow (probe-gated errata on in performance mode), none without;
    # the report's errata table; the slow light probes in the background (a server of its own; once)
    timeout 600 ./probe_mk_test.sh >out/t-probe-mk.log 2>&1; result $? "probe_mk_test ($(tail -1 out/t-probe-mk.log))"
    # MK-02 viewport_flip: negative viewports drawn with the bug imitated fail viewport_check (and
    # DXVK's d3d11_check, with VKB_WINE_DIR), the fix over it passes, forced on a correct driver
    # it is invisible; the gate, the cost of the variants, the SPIR-V pass over the corpus
    timeout 900 ./viewport_test.sh >out/t-viewport.log 2>&1; result $? "viewport_test ($(tail -1 out/t-viewport.log))"
    # Safe and performance mode: where the mode comes from, both modes' emulation masks from one
    # probe (not run yet / error: emulated in safe mode only), switched under a running server, the
    # watchdog's timeout, the report's first line, the client's defaults (a server of its own; once)
    timeout 600 ./mode_test.sh >out/t-mode.log 2>&1; result $? "mode_test ($(tail -1 out/t-mode.log))"
    # Per-game presets by Steam app id: two games' own knobs, at once too; the precedence (file,
    # <cache dir>/env, environment, built-in); VKBRIDGE_MODE per game; bad lines warned and skipped;
    # the client's switch through vkbQueryServer; the report's preset per process; a built-in preset
    # only on a GPU it was tested on (a server of its own; once)
    timeout 600 ./presets_test.sh >out/t-presets.log 2>&1; result $? "presets_test ($(tail -1 out/t-presets.log))"
    # vkBeginCommandBuffer's begin info: primary and secondary (inheritance info) round trips; one
    # encoded without its count (the 2026-10-10 copy's bug, SIGSEGV at 0x10 in Mali's
    # vkBeginCommandBuffer) is refused before the driver, its commands dropped (a server of its own; once)
    timeout 120 ./begin_encoding_test.sh >out/t-begin.log 2>&1; result $? "begin_encoding_test ($(tail -1 out/t-begin.log))"
    # Cached readback: the self-test's cached pages, offered per VKBRIDGE_CACHED_READBACK and the
    # mode; the client's frame readback (vkcube's X11 path) and an app's non-coherent cached type
    # read back right through DMA_BUF_IOCTL_SYNC (dma-buf strategy; a server of its own; once)
    timeout 300 ./cached_test.sh >out/t-cached.log 2>&1; result $? "cached_test ($(tail -1 out/t-cached.log))"
  fi
  # Negative and positive viewports, static and dynamic, two of them, points, a geometry stage,
  # pipeline libraries: every pixel against the CPU's (facing, gl_FragCoord, dFdy) - MK-02's check
  # (mali: multiViewport is emulated, one viewport drawn: no two-viewport cases)
  timeout 60 ./out/viewport_check $([ "$mode" = mali ] && echo --no-multi) >out/t-viewport-check.log 2>&1
  result $? "viewport_check ($(tail -1 out/t-viewport-check.log))"
  # Views with VK_REMAINING_* counts (3D images as 2D arrays: MK-07, always fixed) and views made,
  # rendered to and destroyed over and over (MK-06's pattern), through this mode's server
  timeout 60 ./out/view3d_check >out/t-view3d.log 2>&1; result $? "view3d_check ($(tail -1 out/t-view3d.log))"
  timeout 60 ./out/viewchurn_check 500 >out/t-viewchurn.log 2>&1; result $? "viewchurn_check ($(tail -1 out/t-viewchurn.log))"
  # Dynamic state across pipeline binds and vertex strides bound in one call or one per binding
  # (MK-04 and MK-05, both on in safe mode), through this mode's server; on mali the driver lacks
  # maintenance5 (imitated beneath every layer: VK_WHOLE_SIZE in vkCmdBindVertexBuffers2's pSizes
  # binds an empty range, as on the G720 r44p1), and nothing in this mode's run may have sent it
  timeout 60 ./out/dynstate_check >out/t-dynstate.log 2>&1 &&
    { [ "$mode" != mali ] || ! grep -q "VK_WHOLE_SIZE vertex buffer size reached" "$VKB_SERVER_LOG"; }
  result $? "dynstate_check ($(tail -1 out/t-dynstate.log)$([ "$mode" = mali ] && echo "; $(grep -c 'VK_WHOLE_SIZE vertex buffer size reached' "$VKB_SERVER_LOG") VK_WHOLE_SIZE at the driver"))"
  timeout 60 ./out/vbstride_check >out/t-vbstride.log 2>&1; result $? "vbstride_check ($(tail -1 out/t-vbstride.log))"
  # Points and lines with a dynamic cull mode set, triangles culled after them (MK-16, on in safe mode)
  timeout 60 ./out/cullmode_check >out/t-cullmode.log 2>&1; result $? "cullmode_check ($(tail -1 out/t-cullmode.log))"
  # The vertex attribute divisor as vkd3d-proton wants it (EXT name, zero divisor), drawn at
  # divisors 0/2/1: native on plain, emulated on mali
  timeout 60 ./out/divisor_check >out/t-divisor.log 2>&1; result $? "divisor_check ($(tail -1 out/t-divisor.log))"
  # Polygon mode LINE / POINT (wireframe): native on plain; on mali drawn through the line- and
  # point-list variants (the edge list of non-indexed draws, the compute pass's edges of indexed
  # ones, in primaries and in secondaries as DXVK 3 records render passes on a tiler; nothing drawn
  # solid), and the check fails there with the emulation off (VKBRIDGE_POLYGON_EMU=0: solid)
  timeout 60 ./out/polygon_check >out/t-polygon.log 2>&1 &&
    { [ "$mode" != mali ] || { grep -q "polygon mode LINE drawn as lines (first" "$VKB_SERVER_LOG" &&
      grep -q "polygon mode LINE drawn as lines, indexed" "$VKB_SERVER_LOG" &&
      grep -q "polygon mode LINE drawn as lines, indexed, in a secondary command buffer" "$VKB_SERVER_LOG" &&
      grep -q "polygon mode POINT drawn as points" "$VKB_SERVER_LOG" &&
      ! grep -q "polygon mode [A-Z]* drawn solid (" "$VKB_SERVER_LOG"; }; }
  result $? "polygon_check ($(tail -1 out/t-polygon.log))"
  if [ "$mode" = mali ]; then
    VKBRIDGE_POLYGON_EMU=0 . ./mali_env.sh >/dev/null
    timeout 60 ./out/polygon_check >out/t-polygon-off.log 2>&1
    [ $? = 1 ] && grep -q "^FAIL LINE, vkCmdDraw, triangle list" out/t-polygon-off.log
    result $? "polygon_check fails with the emulation off ($(grep -c '^FAIL' out/t-polygon-off.log) checks failed)"
    . ./mali_env.sh >/dev/null
  fi
  timeout 60 vkcube-wayland --c 120 >out/t-vkcube.log 2>&1; result $? "vkcube-wayland (dma-buf)"
  VKBRIDGE_WSI_SHM=1 timeout 60 vkcube-wayland --c 120 >>out/t-vkcube.log 2>&1; result $? "vkcube-wayland (wl_shm)"
  timeout 60 vkcube --c 120 >>out/t-vkcube.log 2>&1; result $? "vkcube (X11)"
  if docker image inspect vkb-arch-gamescope >/dev/null 2>&1; then
    VKB_DOCKER_EXTRA="--device /dev/dri/renderD128" VKBRIDGE_DRM_RENDER=226:128 timeout 120 ./gamescope_test.sh \
      "bash -c 'export MESA_LOADER_DRIVER_OVERRIDE=zink GALLIUM_DRIVER=zink LIBGL_KOPPER_DRI2=true; timeout 20 glxgears 2>&1 | grep -m1 frames'" \
      >out/t-gamescope-$mode.log 2>&1
    grep -q "frames in" out/t-gamescope-$mode.log; result $? "gamescope + Zink glxgears ($(grep -m1 -o '[0-9.]* FPS' out/t-gamescope-$mode.log))"
  fi
}
# Drivers with VK_KHR_vertex_attribute_divisor only (vkb_ext_alias.h): Mali-G925 on Android 16 also
# lacks the zero divisor (emulated whole); a complete KHR-only driver gets the EXT name as an alias
divisor_variants() {
  echo "== KHR-only divisor drivers"
  local l
  l=$(wc -l < "$VKB_SERVER_LOG")
  VKB_MISSING=divisorzero,VK_EXT_vertex_attribute_divisor . ./bridge_env.sh >/dev/null
  VKBRIDGE_HIDE_EXTS=VK_EXT_vertex_attribute_divisor timeout 60 ./out/divisor_check >out/t-divisor-g925.log 2>&1 &&
    VKBRIDGE_HIDE_EXTS=VK_EXT_vertex_attribute_divisor timeout 60 ./out/divisor_check --core14 >>out/t-divisor-g925.log 2>&1 &&
    tail -n +$((l+1)) "$VKB_SERVER_LOG" | grep -q "lacks vertexAttributeInstanceRateZeroDivisor; emulated instead" &&
    tail -n +$((l+1)) "$VKB_SERVER_LOG" | grep -q "VK_EXT_vertex_attribute_divisor enabled as VK_KHR_vertex_attribute_divisor"
  result $? "divisor_check, Mali-G925's KHR divisor without the zero divisor: emulated ($(grep -c '^PASS' out/t-divisor-g925.log) passed)"
  l=$(wc -l < "$VKB_SERVER_LOG")
  VKB_MISSING=VK_EXT_vertex_attribute_divisor . ./bridge_env.sh >/dev/null
  VKBRIDGE_HIDE_EXTS=VK_EXT_vertex_attribute_divisor timeout 60 ./out/divisor_check >out/t-divisor-khr.log 2>&1 &&
    tail -n +$((l+1)) "$VKB_SERVER_LOG" | grep -q "VK_EXT_vertex_attribute_divisor enabled as VK_KHR_vertex_attribute_divisor" &&
    ! tail -n +$((l+1)) "$VKB_SERVER_LOG" | grep -q "emulation 0x[0-9a-f]* installed"
  result $? "divisor_check, complete KHR-only divisor: EXT name aliased, native ($(tail -1 out/t-divisor-khr.log))"
}
run_mode plain
divisor_variants
server_errors; result $? "no protocol errors in the server log (plain)"
run_mode mali
server_errors; result $? "no protocol errors in the server log (mali)"
echo "== $pass passed, $fail failed"
[ "$fail" = 0 ]
