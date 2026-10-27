#!/bin/bash
# The part of host/run_tests.sh that can run headless, on a software Vulkan implementation
# (lavapipe): no display, no GPU of its own.
# The CI runs it (.github/workflows/tests.yml) with lavapipe as the driver: VK_ICD_FILENAMES and
# VKB_HOST_ICD point at it.
cd "$(dirname "$0")"
mkdir -p out
exec 9>out/.run_ci_tests.lock
flock -n 9 || { echo "(another run in this worktree; waiting)"; flock 9; }
VKB_RUN_DIR=$(mktemp -d "${XDG_RUNTIME_DIR:-/tmp}/vkbridge-ci-$(basename "$(cd .. && pwd)")-$$.XXXX")
export VKB_RUN_DIR VKBRIDGE_SOCKET=$VKB_RUN_DIR/server.sock VKB_SERVER_LOG=$VKB_RUN_DIR/server.log
unset VKBRIDGE_CACHE_DIR VKB_SERVER_PID
. ./server_start.sh
trap 'vkb_server_stop; cp -f "$VKB_SERVER_LOG" out/ci-server.log 2>/dev/null; rm -rf "$VKB_RUN_DIR"' EXIT
pass=0; fail=0
result() {
  if [ "$1" = 0 ]; then
    pass=$((pass+1)); echo "  ok   $2"
  else
    fail=$((fail+1)); echo "  FAIL $2"
    # The server's log (protocol errors) and the check's own log (if it exists) reveal why.
    echo "  server log (last 100 lines):"
    tail -n 100 "$VKB_SERVER_LOG" 2>/dev/null | sed 's/^/    /'
    echo "  server protocol errors: $(grep -cE 'command stream:|malformed request|unknown command id|the driver has no|named ahead failed|bad batch entry' "$VKB_SERVER_LOG" 2>/dev/null)"
  fi
}
# A server-side protocol error fails the run even when the driver shrugs it off (a missing count in
# vkBeginCommandBuffer's encoding passed every check on RADV and crashed Mali).
server_errors() {
  local n; n=$(grep -cE "command stream:|malformed request|unknown command id|the driver has no|named ahead failed|bad batch entry" "$VKB_SERVER_LOG" 2>/dev/null)
  [ "${n:-0}" = 0 ] || { echo "  server log: $n protocol errors, first: $(grep -m1 -E "command stream:|malformed request|unknown command id|the driver has no|named ahead failed|bad batch entry" "$VKB_SERVER_LOG")"; return 1; }
}
run_mode() {
  local mode=$1
  echo "== $mode"
  : > "$VKB_SERVER_LOG"
  if [ "$mode" = mali ]; then . ./mali_env.sh >/dev/null; else unset VKBRIDGE_HIDE_EXTS VKBRIDGE_MAX_API; . ./bridge_env.sh >/dev/null; fi
  timeout 120 ./out/vkbridge-smoke >out/ci-smoke.log 2>&1; result $? "smoke ($(tail -1 out/ci-smoke.log))"
  if [ "$mode" = mali ]; then
    # Mali's memory types (lazymem): images get an ordinary device-local type, never the lazily
    # allocated one (an older client did: image checks failed at "setup", gamescope got -2)
    grep -q "imitating Mali's memory types" "$VKB_SERVER_LOG" && ! grep -q "lazily allocated, imitated" "$VKB_SERVER_LOG" &&
      ! grep -q "no ordinary device-local type" out/ci-smoke.log
    result $? "smoke, Mali's memory types: images in ordinary memory ($(grep -m1 -o 'first allocation in memory type [0-9]* (flags 0x[0-9a-f]*, heap [0-9]*): [0-9]* bytes, plain' "$VKB_SERVER_LOG"))"
    # The XRing O3's memory table (xringmem, issue #1), as measured on r56p0: 22 types; images
    # allow 0x1040, buffers 0x4100, shareable indirect buffers 0x1040 and staging buffers 0x3;
    # dma-heap pages only in type 14. v0.5 had one device-local type (type 6's) and one shared
    # type (14): texel buffers had no type at all. v0.5.1 added a device-local type for each
    # backing, but indirect and staging buffers still had no host-visible type ("no memory type
    # for the buffer (types 0x1 allowed)", gamescope's findMemoryType): now they get the shared ones.
    VKB_MISSING_ADD=xringmem . ./mali_env.sh >/dev/null
    timeout 120 ./out/vkbridge-smoke >out/ci-smoke-xring.log 2>&1 && grep -q "imitating the XRing O3's memory table" "$VKB_SERVER_LOG" &&
      grep -q "resources disagree on device-local types" "$VKB_SERVER_LOG" &&
      ! grep -q "no memory type for the" out/ci-smoke-xring.log && ! grep -q "lazily allocated, imitated" "$VKB_SERVER_LOG"
    result $? "smoke, XRing O3's memory table ($(tail -1 out/ci-smoke-xring.log))"
    timeout 60 ./out/memclass_check >out/ci-memclass-xring.log 2>&1 && ! grep -q "gets no client type" out/ci-memclass-xring.log &&
      grep -q "chosen, types 0x4000 shared" "$VKB_SERVER_LOG"
    result $? "memclass_check, XRing O3's memory table: every image and buffer kind has a type ($(tail -1 out/ci-memclass-xring.log))"
    # The XRing O1's memory table (o1mem, issues #6 and #8): a plain device-local type 0 (0x1)
    # that images and buffers never allow (0x6). v0.5.2 backed the client's device-local type by
    # type 0, and every image's mask was empty (gamescope's flat texture). Without lazymem: the
    # O1's type 0 is not lazily allocated.
    VKB_MISSING=bc,clip,cull,divisor,maint5,depthclip,push,robustness2,dualsrc,fillmode,multiviewport,maint6,desclimits,opaquefd,dmabuflinear,planarext,o1mem \
      . ./mali_env.sh >/dev/null
    timeout 120 ./out/vkbridge-smoke >out/ci-smoke-o1.log 2>&1 && grep -q "imitating the XRing O1's memory table" "$VKB_SERVER_LOG" &&
      ! grep -q "gets no client type" out/ci-smoke-o1.log
    result $? "smoke, XRing O1's memory table ($(tail -1 out/ci-smoke-o1.log))"
    timeout 60 ./out/memclass_check >out/ci-memclass-o1.log 2>&1 && ! grep -q "gets no client type" out/ci-memclass-o1.log
    result $? "memclass_check, XRing O1's memory table: every image and buffer kind has a type ($(tail -1 out/ci-memclass-o1.log))"
    unset VKB_MISSING # an assignment before "." stays in the shell: the tests after it get the usual list
    # The G720's and G925's memory table (g720mem): images and buffers agree on type 0
    from=$(($(wc -l <"$VKB_SERVER_LOG") + 1)) # this server's lines only (the log goes on across restarts)
    VKB_MISSING_ADD=g720mem . ./mali_env.sh >/dev/null
    timeout 60 ./out/memclass_check >out/ci-memclass-g720.log 2>&1 &&
      grep -qx "memory types: 0:0x1 1:0x11 2:0x21/h1 3:0xf" out/ci-memclass-g720.log &&
      tail -n +$from "$VKB_SERVER_LOG" | grep -q "imitating the Mali-G720's memory table" &&
      ! tail -n +$from "$VKB_SERVER_LOG" | grep -q "resources disagree"
    result $? "memclass_check, G720/G925 memory table unchanged ($(grep -m1 '^memory types' out/ci-memclass-g720.log); $(tail -1 out/ci-memclass-g720.log))"
    . ./mali_env.sh >/dev/null
    # Other Mali drivers: push descriptors but not robustness2 (nulls in push descriptor writes are
    # replaced too), no multiDrawIndirect (Mali-G57 r32/r38: indirect draws are split)
    VKB_MISSING=robustness2,multidraw . ./mali_env.sh >/dev/null
    VKBRIDGE_HIDE_EXTS=VK_EXT_robustness2,VK_KHR_robustness2 timeout 120 ./out/vkbridge-smoke >out/ci-smoke-push.log 2>&1 &&
      grep -q "null descriptors in push descriptor writes replaced" "$VKB_SERVER_LOG" &&
      grep -q "indirect draws split" "$VKB_SERVER_LOG"
    result $? "smoke, native push descriptors without robustness2, no multiDrawIndirect ($(tail -1 out/ci-smoke-push.log))"
    . ./mali_env.sh >/dev/null
    # BC decoded as the fallback does (exact). The transcoded variant needs a GPU that samples
    # ETC2/ASTC, which lavapipe does not: it is left to the full suite.
    VKB_BC_TRANSCODE=0 . ./mali_env.sh >/dev/null
    timeout 600 ./out/bcn_check >out/ci-bcn-decode.log 2>&1; result $? "bcn_check, BC decoded ($(tail -1 out/ci-bcn-decode.log))"
    # The texture cap (VKBRIDGE_TEX_MAXDIM): BC textures over 512 created without their top levels
    # (memory, every level's colour through views at each LOD, barriers, copies, textureSize); with
    # ",all" a sampled-only RGBA8 one too (clears, blits and readbacks across the cut); with
    # ",strict" none after a shader reads texture sizes. Lavapipe creates and renders them.
    for v in "" all strict; do
      VKB_TEX_MAXDIM=512${v:+,$v} . ./mali_env.sh >/dev/null
      timeout 120 ./out/texcap_check $v >out/ci-texcap${v:+-$v}.log 2>&1 && grep -q "texture cap: image 2048x2048 (12 mips, format 131) created as 512x512" "$VKB_SERVER_LOG"
      result $? "texcap_check, VKBRIDGE_TEX_MAXDIM=512${v:+,$v} ($(tail -1 out/ci-texcap${v:+-$v}.log); $(grep -m1 -o 'uncapped [0-9]* ([0-9.]*%)' out/ci-texcap${v:+-$v}.log))"
    done
    . ./mali_env.sh >/dev/null
  else
    timeout 600 ./out/bcn_check >out/ci-bcn.log 2>&1; result $? "bcn_check ($(tail -1 out/ci-bcn.log))"
  fi
  # Null images of every shape (cube, 3D, 1D, arrays, uint/sint): on mali each gets a stand-in of
  # its binding's shape, from the pipeline's SPIR-V
  if [ -x out/nulldesc_check ]; then
    timeout 60 ./out/nulldesc_check >out/ci-nulldesc.log 2>&1 &&
      { [ "$mode" != mali ] || [ "$(grep -c 'null descriptor stand-in made' "$VKB_SERVER_LOG")" -ge 6 ]; }
    result $? "nulldesc_check ($(tail -1 out/ci-nulldesc.log)$([ "$mode" = mali ] && echo "; $(grep -c 'null descriptor stand-in made' "$VKB_SERVER_LOG") stand-in shapes"))"
  fi
  if [ -x out/opaque_check ]; then timeout 60 ./out/opaque_check >out/ci-opaque.log 2>&1; result $? "opaque_check ($(tail -1 out/ci-opaque.log))"; fi
  # NV12 textures as DXVK makes them (MUTABLE | EXTENDED_USAGE): on mali the refusal is imitated
  # (planarext) and the client makes them without the flag; 77: the driver has no NV12.
  if [ -x out/planar_check ]; then
    if [ "$mode" = mali ]; then
      timeout 60 ./out/planar_check --imitated >out/ci-planar.log 2>&1; result $? "planar_check, refusal imitated ($(tail -1 out/ci-planar.log))"
      VKBRIDGE_PLANAR_EXT_FIX=0 timeout 60 ./out/planar_check --imitated >out/ci-planar-nofix.log 2>&1; rc=$?
      [ $rc = 1 ] && grep -q "^FAIL vkGetPhysicalDeviceImageFormatProperties2, NV12 transfer" out/ci-planar-nofix.log
      result $? "planar_check, VKBRIDGE_PLANAR_EXT_FIX=0 fails as the G925 did ($(tail -1 out/ci-planar-nofix.log))"
    else
      timeout 60 ./out/planar_check >out/ci-planar.log 2>&1; rc=$?; [ $rc = 77 ] && rc=0
      result $rc "planar_check ($(tail -1 out/ci-planar.log))"
    fi
  fi
  # gamescope's DMA-BUF import for other GPUs: the fd's memoryTypeBits AND the image's. Needs a
  # driver with dma-buf: lavapipe has none
  # if [ -x out/fdprops_check ]; then
  #   if [ "$mode" = mali ]; then
  #     VKB_MISSING_ADD=dmabuftypes . ./mali_env.sh >/dev/null
  #     timeout 60 ./out/fdprops_check >out/ci-fdprops.log 2>&1 && grep -q "imitating Mali's dma-buf fd properties" "$VKB_SERVER_LOG"
  #     result $? "fdprops_check, Mali's dma-buf fd properties ($(tail -1 out/ci-fdprops.log))"
  #     . ./mali_env.sh >/dev/null
  #   else
  #     timeout 60 ./out/fdprops_check >out/ci-fdprops.log 2>&1; result $? "fdprops_check ($(tail -1 out/ci-fdprops.log))"
  #   fi
  # fi
  if [ -x out/memclass_check ]; then timeout 60 ./out/memclass_check >out/ci-memclass.log 2>&1; result $? "memclass_check ($(tail -1 out/ci-memclass.log))"; fi
  # Extension commands through the procaddr functions (DroidDeck#165): physical-device ones of
  # extensions the device lacks are safe to call; vkGetDeviceProcAddr follows the enabled
  # extensions; and with calibrated timestamps hidden from the app
  if [ -x out/procaddr_check ]; then
    timeout 60 ./out/procaddr_check >out/ci-procaddr.log 2>&1 && ! grep -q "crashed with signal" "$VKB_SERVER_LOG"
    result $? "procaddr_check ($(tail -1 out/ci-procaddr.log))"
    VKBRIDGE_HIDE_EXTS=${VKBRIDGE_HIDE_EXTS:+$VKBRIDGE_HIDE_EXTS,}VK_KHR_calibrated_timestamps,VK_EXT_calibrated_timestamps timeout 60 ./out/procaddr_check >out/ci-procaddr-hidden.log 2>&1
    result $? "procaddr_check, calibrated timestamps hidden ($(tail -1 out/ci-procaddr-hidden.log))"
  fi
  # Processes gone without destroying their devices (the S10+ crash: the server crashed in the
  # second steamwebhelper's cleanup). Left out: on lavapipe the server crashes in that scenario
  # (in the null-descriptor emulation's dummies, with no Vulkan call in the frame) - it does not on
  # a real GPU. The full suite has it.
  if [ -x out/recycle_check ]; then timeout 120 ./out/recycle_check >out/ci-recycle.log 2>&1; result $? "recycle_check ($(tail -1 out/ci-recycle.log))"; fi
  # Descriptor traffic, batched (vkbBatch, packed updates) and not
  if [ -x out/desc_traffic_check ]; then
    for nb in 0 1; do
      VKBRIDGE_DESC_BATCH=$((1-nb)) VKBRIDGE_STATS=100000 timeout 120 ./out/desc_traffic_check >out/ci-desc-$nb.log 2>&1
      result $? "desc_traffic_check, VKBRIDGE_NO_DESC_BATCH=$nb ($(grep -o '[0-9]* messages, [0-9]* KB on the wire' out/ci-desc-$nb.log))"
    done
  fi
  # DXVK's frame structure (frames in flight, pools reset per frame, submits from another thread)
  if [ -x out/flicker_check ]; then
    for nb in 0 1; do
      VKBRIDGE_DESC_BATCH=$((1-nb)) timeout 300 ./out/flicker_check >out/ci-flicker-$nb.log 2>&1
      result $? "flicker_check, VKBRIDGE_NO_DESC_BATCH=$nb ($(grep -o '[0-9]* draws wrong in [0-9]* frames' out/ci-flicker-$nb.log))"
    done
  fi
  # The persistent pipeline cache: Steam app keys, pre-caching taken in, killed games, shared files.
  # Left out: lavapipe's pipeline cache comes out empty, so the check's steps fail. The full suite
  # has it.
  # Negative and positive viewports, static and dynamic, two of them, points, a geometry stage,
  # pipeline libraries: every pixel against the CPU's - MK-02's check. Left out for the same
  # reason: lavapipe's graphics pipeline library renders wrong (2 of its 18 checks).
  timeout 60 ./out/view3d_check >out/ci-view3d.log 2>&1; result $? "view3d_check ($(tail -1 out/ci-view3d.log))"
  timeout 120 ./out/viewchurn_check 500 >out/ci-viewchurn.log 2>&1; result $? "viewchurn_check ($(tail -1 out/ci-viewchurn.log))"
  # Dynamic state across pipeline binds and vertex strides bound in one call or one per binding
  timeout 60 ./out/dynstate_check >out/ci-dynstate.log 2>&1 &&
    { [ "$mode" != mali ] || ! grep -q "VK_WHOLE_SIZE vertex buffer size reached" "$VKB_SERVER_LOG"; }
  result $? "dynstate_check ($(tail -1 out/ci-dynstate.log)$([ "$mode" = mali ] && echo "; $(grep -c 'VK_WHOLE_SIZE vertex buffer size reached' "$VKB_SERVER_LOG") VK_WHOLE_SIZE at the driver"))"
  timeout 60 ./out/vbstride_check >out/ci-vbstride.log 2>&1; result $? "vbstride_check ($(tail -1 out/ci-vbstride.log))"
  # The dedicated-allocation hint (client/memory.c) over Mali's preference imitated beneath it
  if [ -x out/dedhint_check ]; then
    VKBRIDGE_DEDICATED_HINT=1 VKBRIDGE_TEST_PREFER_DEDICATED=1 timeout 60 ./out/dedhint_check clear >out/ci-dedhint-1.log 2>&1 &&
      VKBRIDGE_DEDICATED_HINT=0 VKBRIDGE_TEST_PREFER_DEDICATED=1 timeout 60 ./out/dedhint_check pass >out/ci-dedhint-0.log 2>&1 &&
      env -u VKBRIDGE_DEDICATED_HINT VKBRIDGE_TEST_PREFER_DEDICATED=1 timeout 60 ./out/dedhint_check pass >out/ci-dedhint-def.log 2>&1
    rc=$?
    result $? "dedhint_check (on: $(tail -1 out/ci-dedhint-1.log); off: $(tail -1 out/ci-dedhint-0.log); default: $(tail -1 out/ci-dedhint-def.log))"
  fi
  # Points and lines with a dynamic cull mode set, triangles culled after them (MK-16)
  timeout 60 ./out/cullmode_check >out/ci-cullmode.log 2>&1; result $? "cullmode_check ($(tail -1 out/ci-cullmode.log))"
  # The vertex attribute divisor as vkd3d-proton wants it (EXT name, zero divisor), drawn at
  # divisors 0/2/1: native on plain, emulated on mali
  timeout 60 ./out/divisor_check >out/ci-divisor.log 2>&1; result $? "divisor_check ($(tail -1 out/ci-divisor.log))"
  # Polygon mode LINE / POINT (wireframe): native on plain; on mali drawn through the line- and
  # point-list variants
  timeout 60 ./out/polygon_check >out/ci-polygon.log 2>&1 &&
    { [ "$mode" != mali ] || { grep -q "polygon mode LINE drawn as lines (first" "$VKB_SERVER_LOG" &&
      grep -q "polygon mode LINE drawn as lines, indexed" "$VKB_SERVER_LOG" &&
      grep -q "polygon mode LINE drawn as lines, indexed, in a secondary command buffer" "$VKB_SERVER_LOG" &&
      grep -q "polygon mode POINT drawn as points" "$VKB_SERVER_LOG" &&
      ! grep -q "polygon mode [A-Z]* drawn solid (" "$VKB_SERVER_LOG"; }; }
  result $? "polygon_check ($(tail -1 out/ci-polygon.log))"
  # Transform feedback as DXVK's D3D11 stream output uses it (two buffers, counters resumed,
  # vkCmdDrawIndirectByteCountEXT), captured by the vertex stage and by DXVK's pass-through point
  # geometry stage; on mali that stage is folded into the vertex shader and must capture the same
  # bytes, and with VKBRIDGE_NO_XFB_FOLD=1 it is kept. 77: no XFB.
  for st in vs gs; do
    timeout 60 ./out/xfb_check $st >out/ci-xfb-$st.log 2>&1; rc=$?; [ $rc = 77 ] && rc=0
    [ $rc = 0 ] && [ "$mode" = mali ] && [ $st = gs ] && ! grep -q "skipped" out/ci-xfb-$st.log &&
      { grep -q "stream output geometry shaders folded into their vertex shaders" "$VKB_SERVER_LOG" || rc=1; }
    result $rc "xfb_check, $st captures ($(tail -1 out/ci-xfb-$st.log))"
  done
  if [ "$mode" = mali ]; then
    from=$(($(wc -l <"$VKB_SERVER_LOG") + 1))
    VKB_SERVER_ENV=VKBRIDGE_NO_XFB_FOLD=1 . ./mali_env.sh >/dev/null
    timeout 60 ./out/xfb_check gs >out/ci-xfb-nofold.log 2>&1; rc=$?; [ $rc = 77 ] && rc=0
    [ $rc = 0 ] && tail -n +$from "$VKB_SERVER_LOG" | grep -q "stream output geometry shaders folded" && rc=1
    result $rc "xfb_check, gs captures with VKBRIDGE_NO_XFB_FOLD=1: the geometry stage kept ($(tail -1 out/ci-xfb-nofold.log))"
    . ./mali_env.sh >/dev/null
  fi
  # The checks that bring a server of their own: run_tests.sh runs them once, on plain. They write
  # their own sockets and cache directories; VKB_HOST_ICD is enough for each. (Left out with the
  # rest of the hardware-dependent tests: probe_tx_test's BC -> ASTC transcodes, which lavapipe
  # cannot sample; the Zink vertex-format checks (no GL device over lavapipe); fossilize (no
  # Proton); the BC2/BC3/BC7 ASTC transcode checks; and the hotpath benchmark.)
  if [ "$mode" = plain ]; then
    # Version promotion: a 1.1 / 1.2 driver offered as 1.3 (or 1.2 when a piece is missing), core
    # structures and names through the extensions, SPIR-V lowered (the corpus spirv-val clean)
    timeout 600 ./promote_test.sh >out/ci-promote.log 2>&1; result $? "promote_test ($(tail -1 out/ci-promote.log))"
    # shm_barrier: barriers between shared-memory writes and reads in compute shaders
    timeout 600 ./shmbarrier_test.sh >out/ci-shmbarrier.log 2>&1; result $? "shmbarrier_test ($(tail -1 out/ci-shmbarrier.log))"
    # wave32: 32-lane subgroups emulated in compute shaders on lavapipe's 8-wide ones
    timeout 600 ./wave32_test.sh >out/ci-wave32.log 2>&1; result $? "wave32_test ($(tail -1 out/ci-wave32.log))"
    # vb_bake: the G68's last-stride-to-every-binding bug imitated, the variants over it
    timeout 300 ./vbbake_test.sh >out/ci-vbbake.log 2>&1; result $? "vbbake_test ($(tail -1 out/ci-vbbake.log))"
    # MK-11 stencil_layout: a separate stencil layout ignored, the fix's combined one
    timeout 300 ./stencillayout_test.sh >out/ci-stencillayout.log 2>&1; result $? "stencillayout_test ($(tail -1 out/ci-stencillayout.log))"
    # Dynamic rendering as render passes: the driver's own, emulated at 1.2, switched off, promoted
    timeout 300 ./dynrend_test.sh >out/ci-dynrend.log 2>&1; result $? "dynrend_test ($(tail -1 out/ci-dynrend.log))"
    # The G715 r54p3's robustness2 (nullDescriptor only): both robust-access features taken off
    timeout 600 ./robust2_test.sh >out/ci-robust2.log 2>&1; result $? "robust2_test ($(tail -1 out/ci-robust2.log))"
    # MK-20 fp16_hide: a wrong 16-bit driver has shaderFloat16 hidden (storage kept)
    timeout 600 ./fp16_test.sh >out/ci-fp16.log 2>&1; result $? "fp16_test ($(tail -1 out/ci-fp16.log))"
    # Asynchronous pipeline compiles with a slow compiler imitated
    timeout 300 ./asyncpipe_test.sh >out/ci-asyncpipe.log 2>&1; result $? "asyncpipe_test ($(tail -1 out/ci-asyncpipe.log))"
  fi
}
# The KHR-only divisor drivers (host/run_tests.sh has them) are left out: they need a driver whose
# vertex attribute divisor support differs (Mali-G925's KHR divisor without the zero divisor),
# and lavapipe's is the full one. (The prefix assignment before a sourced script also does not
# reach it outside bash's POSIX mode, so the fake list would not arrive at the server either.)
run_mode plain
server_errors; result $? "no protocol errors in the server log (plain)"
run_mode mali
server_errors; result $? "no protocol errors in the server log (mali)"
echo "== $pass passed, $fail failed"
[ "$fail" = 0 ]
