#!/bin/bash
# Host regression suite for the bridge: every automated check, in two server modes -
#   plain   the desktop driver as it is (RADV)
#   mali    the desktop driver with Mali-G720's known gaps imitated (mali_env.sh)
#   host/run_tests.sh [test ...]   one or more names from --list (default: the whole suite)
# Needs: make (out/), Proton Experimental for the DXVK check (VKB_WINE_DIR prepared, see README),
# docker image vkb-arch-gamescope for the gamescope/Zink check (skipped if absent).
# Runs side by side with other runs (other worktrees, other agents): each run has a directory of its
# own for its server's socket and log (VKB_RUN_DIR; the log is copied to out/server.log at the end)
# and stops only the servers it started. Two runs in one worktree take turns (out/ is shared).
cd "$(dirname "$0")"

# Select individual tests: `run_tests.sh cached_test` or `VKB_ONLY="cached_test divisor_g925"`.
# With no names the whole suite runs. `--list` prints the names.
# The Makefile/CMake `test` targets pass the output directory as the first argument; ignore it.
VKB_TEST_NAMES="smoke smoke_memtypes smoke_xring memclass_xring fdprops_xring smoke_o1 memclass_o1
bgr10 uf11 memclass_g720 smoke_push bcn_transcode bcn_decode bcn_astc texcap bcn nulldesc opaque
planar dmabuf_image fdprops memclass procaddr exitclean recycle desc_traffic flicker d3d11
d3d11_flicker pcache wait_imitate compile_watchdog gsfold probe_report probe_end probe_tx errata
bitwise shlbarrier promote shmbarrier wave32 clampzero views cmdstate vbbake cullmode_test dedimport
stencillayout dynrend probe_mk viewport_test mode presets calts robust2 fp16 begin_encoding cached
heapcap asyncpipe fossilize viewport_check view3d viewchurn dynstate vbstride dedhint cullmode_check
divisor polygon polygon_off vkcube_wayland_dmabuf vkcube_wayland_shm vkcube_x11 gamescope
divisor_g925 divisor_khr report_stall report_stuck report_cpu report_gpumem present_timeout zerocopy_nested server_hup gshide
featclamp envfile unflagged eventstatus query querystuck gsexport vtxlayout beginfail endfail secfail
submitfail waitlost apicap ycbcr_swap driverid vidyn eds compilegate sparse a1zink envswitch poolfull lowmem xclipse_zink syncfd dmabufsync timeline dabreg dmabufahb
xfb xfb_nofold hotpath
poll reqcache syncmirror stale_frames bcn_budget bcn_mixcopy hitch bcbudget latency lagprobe framequeue
presentwait present_block compileburst querystuck_burst"
if [ "${1:-}" = --list ]; then
  printf '%s\n' $VKB_TEST_NAMES
  exit 0
fi
if [ -n "${1:-}" ] && [ -d "$1" ]; then shift; fi
VKB_ONLY=${VKB_ONLY:-$*}
want() {
  [ -z "$VKB_ONLY" ] && return 0
  local w
  for w in ${VKB_ONLY//,/ }; do [ "$w" = "$1" ] && return 0; done
  return 1
}
want_any() {
  [ -z "$VKB_ONLY" ] && return 0
  local k
  for k in "$@"; do want "$k" && return 0; done
  return 1
}
# The wayland vkcube: vulkan-tools' vkcube --wsi wayland, or a separate vkcube-wayland if installed.
# A command string (not a function): `timeout` must be able to exec it.
if command -v vkcube-wayland >/dev/null 2>&1; then
  VKCUBE_WAYLAND="vkcube-wayland"
else
  VKCUBE_WAYLAND="vkcube --wsi wayland"
fi

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
  if want smoke; then
    timeout 120 ./out/vkbridge-smoke >out/t-smoke.log 2>&1; result $? "smoke ($(tail -1 out/t-smoke.log))"
  elif want smoke_memtypes; then
    # The memory-types check reads this run's log.
    timeout 120 ./out/vkbridge-smoke >out/t-smoke.log 2>&1
  fi
  if [ "$mode" = mali ]; then
    # Mali's memory types (lazymem): images get an ordinary device-local type, never the lazily
    # allocated one (an older client did: image checks failed at "setup", gamescope got -2)
    if want smoke_memtypes; then
      grep -q "imitating Mali's memory types" "$VKB_SERVER_LOG" && ! grep -q "lazily allocated, imitated" "$VKB_SERVER_LOG" &&
        ! grep -q "no ordinary device-local type" out/t-smoke.log
      result $? "smoke, Mali's memory types: images in ordinary memory ($(grep -m1 -o 'first allocation in memory type [0-9]* (flags 0x[0-9a-f]*, heap [0-9]*): [0-9]* bytes, plain' "$VKB_SERVER_LOG"))"
    fi
    # The XRing O3's memory table (xringmem), as measured on r56p0: 22 types; images
    # allow 0x1040, buffers 0x4100, shareable indirect buffers 0x1040 and staging buffers 0x3;
    # dma-heap pages only in type 14. v0.5 had one device-local type (type 6's) and one shared
    # type (14): texel buffers had no type at all. v0.5.1 added a device-local type for each
    # backing, but indirect and staging buffers still had no host-visible type ("no memory type
    # for the buffer (types 0x1 allowed)", gamescope's findMemoryType): now they get the shared ones.
    if want_any smoke_xring memclass_xring; then
      VKB_MISSING_ADD=xringmem . ./mali_env.sh >/dev/null
      if want smoke_xring; then
        timeout 120 ./out/vkbridge-smoke >out/t-smoke-xring.log 2>&1 && grep -q "imitating the XRing O3's memory table" "$VKB_SERVER_LOG" &&
          grep -q "resources disagree on device-local types" "$VKB_SERVER_LOG" &&
          ! grep -q "no memory type for the" out/t-smoke-xring.log && ! grep -q "lazily allocated, imitated" "$VKB_SERVER_LOG"
        result $? "smoke, XRing O3's memory table ($(tail -1 out/t-smoke-xring.log))"
      fi
      if want memclass_xring; then
        timeout 60 ./out/memclass_check >out/t-memclass-xring.log 2>&1 && ! grep -q "gets no client type" out/t-memclass-xring.log &&
          grep -q "chosen, types 0x4000 shared" "$VKB_SERVER_LOG"
        result $? "memclass_check, XRing O3's memory table: every image and buffer kind has a type ($(tail -1 out/t-memclass-xring.log))"
      fi
    fi
    # The XRing O1's memory table (o1mem): a plain device-local type 0 (0x1)
    # that images and buffers never allow (0x6). v0.5.2 backed the client's device-local type by
    # type 0, and every image's mask was empty (gamescope's flat texture). Without lazymem: the
    # O1's type 0 is not lazily allocated.
    if want_any smoke_o1 memclass_o1; then
      VKB_MISSING=bc,clip,cull,divisor,maint5,depthclip,push,robustness2,dualsrc,fillmode,multiviewport,maint6,desclimits,opaquefd,dmabuflinear,planarext,o1mem \
        . ./mali_env.sh >/dev/null
      if want smoke_o1; then
        timeout 120 ./out/vkbridge-smoke >out/t-smoke-o1.log 2>&1 && grep -q "imitating the XRing O1's memory table" "$VKB_SERVER_LOG" &&
          ! grep -q "gets no client type" out/t-smoke-o1.log
        result $? "smoke, XRing O1's memory table ($(tail -1 out/t-smoke-o1.log))"
      fi
      if want memclass_o1; then
        timeout 60 ./out/memclass_check >out/t-memclass-o1.log 2>&1 && ! grep -q "gets no client type" out/t-memclass-o1.log
        result $? "memclass_check, XRing O1's memory table: every image and buffer kind has a type ($(tail -1 out/t-memclass-o1.log))"
      fi
      unset VKB_MISSING # an assignment before "." stays in the shell: the tests after it get the usual list
    fi
    # ... with Mali's dma-buf fd properties too (gamescope's DMA-BUF import: a type both allow)
    if want fdprops_xring; then
      VKB_MISSING_ADD=xringmem,dmabuftypes . ./mali_env.sh >/dev/null
      timeout 60 ./out/fdprops_check >out/t-fdprops-xring.log 2>&1 && grep -q "imitating Mali's dma-buf fd properties" "$VKB_SERVER_LOG"
      result $? "fdprops_check, XRing O3's memory table and Mali's fd properties ($(tail -1 out/t-fdprops-xring.log))"
    fi
    # BGRA 10-10-10-2 signed vertex fetch (VKB_EMU_BGR10_VTX): Mali lacks A2R10G10B10_SNORM, which
    # held Zink at OpenGL 3.2; fetched as A2B10G10R10_SNORM with red and blue swapped back in the
    # vertex shader. A GL 3.3 core program reads (204, 0, 51) through Zink with the format imitated
    # missing. The desktop driver offers VK_EXT_vertex_input_dynamic_state, as Mali r48 and later
    # do: Zink gets it, and its vertex input with the format is made static in a pipeline variant
    # (server_vidyn.c); a Vulkan client that is not Zink still lists it.
    if want bgr10; then
      VKB_MISSING_ADD=bgr10vtx . ./mali_env.sh >/dev/null
      MESA_LOADER_DRIVER_OVERRIDE=zink GALLIUM_DRIVER=zink timeout 60 python3 -I ../tests/gl/glbgr10.py >out/t-bgr10.log 2>&1
      timeout 60 vulkaninfo 2>/dev/null | grep -c "VK_EXT_vertex_input_dynamic_state" >out/t-bgr10-vulkaninfo.log
      grep -q "^RESULT ok" out/t-bgr10.log && grep -q "red and blue swapped back in the vertex shader" "$VKB_SERVER_LOG" &&
        grep -q "offered to Zink: the driver's" "$VKB_SERVER_LOG" && grep -q "first pipeline variant with vkCmdSetVertexInputEXT" "$VKB_SERVER_LOG" &&
        [ "$(cat out/t-bgr10-vulkaninfo.log)" -ge 1 ]
      result $? "BGRA 10-10-10-2 vertex fetch through Zink, a pipeline variant ($(grep -o 'RESULT.*' out/t-bgr10.log | cut -c1-60)); vulkaninfo still lists vertex input dynamic state"
      unset VKB_MISSING_ADD
    fi
    # Zink on a driver without VK_EXT_vertex_input_dynamic_state (the G720 r44's): the server's,
    # every draw from a variant. Two vertex layouts with one program, as Mali's Zink sees the
    # device (no pipeline libraries, no extended dynamic state 3), each draw its own colour.
    if want vtxlayout; then
      VKB_MISSING_ADD=vidyn . ./mali_env.sh >/dev/null
      VKBRIDGE_HIDE_EXTS=$VKBRIDGE_HIDE_EXTS,VK_EXT_graphics_pipeline_library,VK_EXT_extended_dynamic_state3 \
        MESA_LOADER_DRIVER_OVERRIDE=zink GALLIUM_DRIVER=zink timeout 60 python3 -I ../tests/gl/glvtxlayout.py >out/t-vtxlayout.log 2>&1
      grep -q "^RESULT ok" out/t-vtxlayout.log && grep -q "offered to Zink: the server's" "$VKB_SERVER_LOG" &&
        grep -q "first pipeline variant with vkCmdSetVertexInputEXT" "$VKB_SERVER_LOG"
      result $? "two vertex layouts through Zink, vertex input dynamic state emulated ($(grep -o 'RESULT.*' out/t-vtxlayout.log | cut -c1-70))"
      unset VKB_MISSING_ADD
    fi
    # A vkBeginCommandBuffer the driver refuses (out of memory: Cyberpunk on the XRing O3): its commands
    # are not replayed and submits leave it out, so the server lives; the smoke test loses that one
    # check's work and passes the ones after it.
    if want beginfail; then
      VKB_SERVER_ENV=VKBRIDGE_TEST_BEGIN_FAIL=2 . ./mali_env.sh >/dev/null
      # the n-th begin / end is the app's: the sync mirror's own command buffers would count too
      VKBRIDGE_SYNC_MIRROR=0 timeout 120 ./out/vkbridge-smoke >out/t-beginfail.log 2>&1
      kill -0 "$VKB_SERVER_PID" 2>/dev/null && grep -q "did not record left out" "$VKB_SERVER_LOG" &&
        grep -q "12 of 13 checks passed" out/t-beginfail.log
      result $? "a refused vkBeginCommandBuffer: server alive, submit leaves it out ($(tail -1 out/t-beginfail.log))"
      unset VKB_SERVER_ENV
    fi
    # The driver's vkEndCommandBuffer failing: the command buffer is left out of submits the same way.
    if want endfail; then
      VKB_SERVER_ENV=VKBRIDGE_TEST_END_FAIL=2 . ./mali_env.sh >/dev/null
      # the n-th begin / end is the app's: the sync mirror's own command buffers would count too
      VKBRIDGE_SYNC_MIRROR=0 timeout 120 ./out/vkbridge-smoke >out/t-endfail.log 2>&1
      kill -0 "$VKB_SERVER_PID" 2>/dev/null && grep -q "did not record left out" "$VKB_SERVER_LOG" &&
        grep -q "12 of 13 checks passed" out/t-endfail.log
      result $? "a failed vkEndCommandBuffer: server alive, submit leaves it out ($(tail -1 out/t-endfail.log))"
      unset VKB_SERVER_ENV
    fi
    # A secondary whose end failed: the primary's vkCmdExecuteCommands leaves it out, the primary runs.
    if want secfail; then
      VKB_SERVER_ENV=VKBRIDGE_TEST_END_FAIL=1 . ./mali_env.sh >/dev/null
      timeout 60 ./out/begin_check ok >out/t-secfail.log 2>&1
      kill -0 "$VKB_SERVER_PID" 2>/dev/null && grep -q "vkCmdExecuteCommands: 1 command buffer(s) the driver did not record left out" "$VKB_SERVER_LOG" &&
        grep -q "PASS primary: vkEndCommandBuffer" out/t-secfail.log && grep -q "FAILED: 1 of 12" out/t-secfail.log
      result $? "a secondary the driver did not record: left out of the primary ($(tail -1 out/t-secfail.log))"
      unset VKB_SERVER_ENV
    fi
    # A Vulkan error the game gets is logged by the client the first time each call returns it (Stray
    # stopped on a device reset with nothing in either log, #21): three submits fail with a lost
    # device (synchronous results, so the client sees them), the client's log has one line for them.
    if want submitfail; then
      VKB_SERVER_ENV=VKBRIDGE_TEST_SUBMIT_FAIL=2 . ./mali_env.sh >/dev/null
      rm -f out/t-submitfail-client.log
      VKBRIDGE_SYNC=1 VKBRIDGE_FRAMES=0 VKBRIDGE_CLIENT_LOG=$PWD/out/t-submitfail-client.log timeout 120 ./out/vkbridge-smoke >out/t-submitfail.log 2>&1
      n=$(grep -c "vkQueueSubmit returned VK_ERROR_DEVICE_LOST (-4) to the game, the first time" out/t-submitfail-client.log 2>/dev/null)
      kill -0 "$VKB_SERVER_PID" 2>/dev/null && [ "$(grep -c "VKBRIDGE_TEST_SUBMIT_FAIL" "$VKB_SERVER_LOG")" = 3 ] && [ "${n:-0}" = 1 ]
      result $? "three failed submits: the client logs the first, once (${n:-0} line(s); $(tail -1 out/t-submitfail.log))"
      unset VKB_SERVER_ENV
    fi
    # B10G11R11 float vertex fetch (VKB_EMU_UF11_VTX): Mali before r49 lacks it, which held Zink below
    # OpenGL 4.4; fetched as R32_UINT and unpacked in the vertex shader. A GL program reads
    # (191, 64, 128) through Zink with the format imitated missing, and Zink still offers 4.4 or newer.
    if want uf11; then
      VKB_MISSING_ADD=uf11vtx . ./mali_env.sh >/dev/null
      MESA_LOADER_DRIVER_OVERRIDE=zink GALLIUM_DRIVER=zink timeout 60 python3 -I ../tests/gl/gluf11.py >out/t-uf11.log 2>&1
      v=$(grep -o 'GL_VERSION_MAX [0-9.]*' out/t-uf11.log | cut -d' ' -f2)
      grep -q "^RESULT ok" out/t-uf11.log && grep -q "unpacked in the vertex shader" "$VKB_SERVER_LOG" &&
        [ "${v%%.*}" -ge 4 ] && [ "${v#*.}" -ge 4 -o "${v%%.*}" -gt 4 ]
      result $? "B10G11R11 float vertex fetch through Zink ($(grep -o 'RESULT.*' out/t-uf11.log | cut -c1-60)); GL $v"
      unset VKB_MISSING_ADD
    fi
    # The G720's and G925's memory table (g720mem): images and buffers agree on type 0, so the
    # client's types are what v0.5 made of it on those tablets, and nothing disagrees
    if want memclass_g720; then
      from=$(($(wc -l <"$VKB_SERVER_LOG") + 1)) # this server's lines only (the log goes on across restarts)
      VKB_MISSING_ADD=g720mem . ./mali_env.sh >/dev/null
      timeout 60 ./out/memclass_check >out/t-memclass-g720.log 2>&1 &&
        grep -qx "memory types: 0:0x1 1:0x11 2:0x21/h1 3:0xf" out/t-memclass-g720.log &&
        tail -n +$from "$VKB_SERVER_LOG" | grep -q "imitating the Mali-G720's memory table" &&
        ! tail -n +$from "$VKB_SERVER_LOG" | grep -q "resources disagree"
      result $? "memclass_check, G720/G925 memory table unchanged ($(grep -m1 '^memory types' out/t-memclass-g720.log); $(tail -1 out/t-memclass-g720.log))"
      . ./mali_env.sh >/dev/null
    fi
    # Other Mali drivers: push descriptors but not robustness2 (nulls in push descriptor writes are
    # replaced too), no multiDrawIndirect (Mali-G57 r32/r38: indirect draws are split)
    if want smoke_push; then
      VKB_MISSING=robustness2,multidraw . ./mali_env.sh >/dev/null
      VKBRIDGE_HIDE_EXTS=VK_EXT_robustness2,VK_KHR_robustness2 timeout 120 ./out/vkbridge-smoke >out/t-smoke-push.log 2>&1 &&
        grep -q "null descriptors in push descriptor writes replaced" "$VKB_SERVER_LOG" &&
        grep -q "indirect draws split" "$VKB_SERVER_LOG"
      result $? "smoke, native push descriptors without robustness2, no multiDrawIndirect ($(tail -1 out/t-smoke-push.log))"
      . ./mali_env.sh >/dev/null
    fi
    # BC transcoded to ETC2/EAC/ASTC (compared by PSNR), then decoded as the fallback does (exact).
    # Without the conversion budget (the default), so that every image goes through its transcoder;
    # then with the budget DS3's preset sets (32:4: some images decoded over it), the same floors
    if want bcn_transcode; then
      VKB_BC_TX_BUDGET=0 . ./mali_env.sh >/dev/null
      timeout 180 ./out/bcn_check transcode >out/t-bcn.log 2>&1; result $? "bcn_check, BC transcoded ($(tail -1 out/t-bcn.log))"
    fi
    if want bcn_budget; then
      VKB_BC_TX_BUDGET=32:4 . ./mali_env.sh >/dev/null
      timeout 180 ./out/bcn_check transcode >out/t-bcn-budget.log 2>&1 && grep -q "BC budget: transcoding" "$VKB_SERVER_LOG"
      result $? "bcn_check, BC transcoded under a 32:4 conversion budget ($(tail -1 out/t-bcn-budget.log); $(grep -o 'so far [0-9]* image(s) ([0-9.]* Mtexels) transcoded, [0-9]* ([0-9.]*) decoded' "$VKB_SERVER_LOG" | tail -1))"
    fi
    # Copies between a decoded and a transcoded image of one BC format (the budget leaves both, and
    # UE4 copies a streamed texture's levels between its old and new image): every copy done (from
    # the transcoded one by a blit, from the decoded one re-encoded), none skipped. Decided by size
    # (VKBRIDGE_BC_TRANSCODE_MAX=256: the 512x512 images decoded), in ASTC 6x6 and 8x8 too, then by
    # the budget itself (a bucket that lets one image through)
    if want bcn_mixcopy; then
      mixed_copies() { # mixed_copies <log from line> <name> <bcn_check args...>
        local from=$1 name=$2 r line; shift 2
        timeout 300 ./out/bcn_check "$@" >out/t-bcn-$name.log 2>&1; r=$?
        for _ in $(seq 50); do tail -n +$from "$VKB_SERVER_LOG" | grep -q "BC copies so far" && break; sleep 0.1; done
        line=$(tail -n +$from "$VKB_SERVER_LOG" | grep -o "BC copies so far: .*" | tail -1)
        [ $r = 0 ] && echo "$line" | grep -q "so far: [1-9][0-9]* from a transcoded.*, [1-9][0-9]* from a decoded.*; 0 skipped"
        result $? "bcn_check $*, $name: copies between decoded and transcoded images ($(tail -1 out/t-bcn-$name.log)$([ $r = 0 ] || grep -m3 -E '^(FAIL|error)' out/t-bcn-$name.log | tr '\n' '|'); $(echo "$line" | grep -o '[0-9]* from a transcoded into a decoded image (blit), [0-9]* from a decoded into a transcoded image (re-encoded); [0-9]* skipped'))"
      }
      VKB_SERVER_ENV=VKBRIDGE_BC_TRANSCODE_MAX=256 VKB_BC_TX_BUDGET=0 . ./mali_env.sh >/dev/null
      mixed_copies $(($(wc -l <"$VKB_SERVER_LOG") + 1)) mixcopy transcode mixcopy
      for n in 6 8; do
        VKB_SERVER_ENV=VKBRIDGE_BC_TRANSCODE_MAX=256 VKB_BC_ASTC_BLOCK=${n}x$n VKB_BC_TX_BUDGET=0 . ./mali_env.sh >/dev/null
        mixed_copies $(($(wc -l <"$VKB_SERVER_LOG") + 1)) mixcopy-astc$n transcode astc$n mixcopy
      done
      unset VKB_SERVER_ENV
      VKB_BC_TX_BUDGET=0.000001:0.000001 . ./mali_env.sh >/dev/null
      mixed_copies $(($(wc -l <"$VKB_SERVER_LOG") + 1)) mixcopy-budget transcode mixcopy
      . ./mali_env.sh >/dev/null
    fi
    if want bcn_decode; then
      VKB_BC_TRANSCODE=0 . ./mali_env.sh >/dev/null
      timeout 180 ./out/bcn_check >out/t-bcn-decode.log 2>&1; result $? "bcn_check, BC decoded ($(tail -1 out/t-bcn-decode.log))"
      . ./mali_env.sh >/dev/null
    fi
    # BC2 / BC3 / BC7 into ASTC 6x6 and 8x8 blocks (VKBRIDGE_BC_ASTC_BLOCK): the same checks by PSNR
    # with those blocks' floors, and the server's images in that format
    if want bcn_astc; then
      for n in 6 8; do
        VKB_BC_ASTC_BLOCK=${n}x$n VKB_BC_TX_BUDGET=0 . ./mali_env.sh >/dev/null
        BCN_NO_LEVELS=1 timeout 300 ./out/bcn_check transcode astc$n >out/t-bcn-astc$n.log 2>&1 &&
          grep -q "ASTC ${n}x$n (VKBRIDGE_BC_ASTC_BLOCK)" "$VKB_SERVER_LOG"
        result $? "bcn_check, BC2/BC3/BC7 transcoded to ASTC ${n}x$n ($(tail -1 out/t-bcn-astc$n.log); BC7 photo $(grep -m1 -o 'BC7 -> ASTC [0-9x]*, photo: PSNR [0-9.]* dB' out/t-bcn-astc$n.log | grep -o '[0-9.]* dB'))"
      done
      . ./mali_env.sh >/dev/null
    fi
    # The texture cap (VKBRIDGE_TEX_MAXDIM): BC textures over 512 created without their top levels
    # (memory, every level's colour through views at each LOD, barriers, copies, textureSize); with
    # ",all" a sampled-only RGBA8 one too (clears, blits and readbacks across the cut); with
    # ",strict" none after a shader reads texture sizes. Without the conversion budget: the sizes
    # compared are of transcoded images, and the budget decodes some of a quick run of images
    if want texcap; then
      for v in "" all strict; do
        VKB_TEX_MAXDIM=512${v:+,$v} VKB_BC_TX_BUDGET=0 . ./mali_env.sh >/dev/null
        timeout 120 ./out/texcap_check $v >out/t-texcap${v:+-$v}.log 2>&1 && grep -q "texture cap: image 2048x2048 (12 mips, format 131) created as 512x512" "$VKB_SERVER_LOG"
        result $? "texcap_check, VKBRIDGE_TEX_MAXDIM=512${v:+,$v} ($(tail -1 out/t-texcap${v:+-$v}.log); $(grep -m1 -o 'uncapped [0-9]* ([0-9.]*%)' out/t-texcap${v:+-$v}.log))"
      done
      . ./mali_env.sh >/dev/null
    fi
  else
    if want bcn; then
      timeout 180 ./out/bcn_check >out/t-bcn.log 2>&1; result $? "bcn_check ($(tail -1 out/t-bcn.log))"
    fi
  fi
  # Null images of every shape (cube, 3D, 1D, arrays, uint/sint): on mali each gets a stand-in of
  # its binding's shape, from the pipeline's SPIR-V
  if want nulldesc && [ -x out/nulldesc_check ]; then
    timeout 60 ./out/nulldesc_check >out/t-nulldesc.log 2>&1 &&
      { [ "$mode" != mali ] || [ "$(grep -c 'null descriptor stand-in made' "$VKB_SERVER_LOG")" -ge 6 ]; }
    result $? "nulldesc_check ($(tail -1 out/t-nulldesc.log)$([ "$mode" = mali ] && echo "; $(grep -c 'null descriptor stand-in made' "$VKB_SERVER_LOG") stand-in shapes"))"
  fi
  if want opaque && [ -x out/opaque_check ]; then timeout 60 ./out/opaque_check >out/t-opaque.log 2>&1; result $? "opaque_check ($(tail -1 out/t-opaque.log))"; fi
  # Status polls sent without the barrier still see the host's own queued sets and resets of the
  # object polled (and give the old answers with VKBRIDGE_POLL_NOBARRIER=0)
  if want poll; then
    timeout 120 ./out/poll_check >out/t-poll.log 2>&1 && VKBRIDGE_POLL_NOBARRIER=0 timeout 120 ./out/poll_check >out/t-poll-off.log 2>&1
    result $? "poll_check ($(tail -1 out/t-poll.log); $(grep -o '[0-9]* not-ready polls' out/t-poll.log), $(grep -o '[0-9]* not-ready polls' out/t-poll-off.log) with the barrier)"
    # the same with the sync mirror off (client/syncmirror.c answers most polls before they reach this path)
    VKBRIDGE_SYNC_MIRROR=0 timeout 120 ./out/poll_check >out/t-poll-nomirror.log 2>&1
    result $? "poll_check, VKBRIDGE_SYNC_MIRROR=0 ($(tail -1 out/t-poll-nomirror.log); $(grep -o '[0-9]* not-ready polls' out/t-poll-nomirror.log))"
  fi
  # Answers the client keeps (image requirements by create info, format and image format
  # properties) are the server's: same create info same answer, other sizes others, refusals kept
  if want reqcache; then
    timeout 120 ./out/reqcache_check >out/t-reqcache.log 2>&1; result $? "reqcache_check ($(tail -1 out/t-reqcache.log))"
  fi
  # NV12 textures as DXVK makes them (MUTABLE | EXTENDED_USAGE): on mali the refusal is imitated
  # (planarext) and the client makes them without the flag; VKBRIDGE_PLANAR_EXT_FIX=0 is the negative
  # control (the refusal reaches the app). 77: the driver has no NV12.
  if want planar && [ -x out/planar_check ]; then
    if [ "$mode" = mali ]; then
      timeout 60 ./out/planar_check --imitated >out/t-planar.log 2>&1; result $? "planar_check, refusal imitated ($(tail -1 out/t-planar.log))"
      VKBRIDGE_PLANAR_EXT_FIX=0 timeout 60 ./out/planar_check --imitated >out/t-planar-nofix.log 2>&1; rc=$?
      [ $rc = 1 ] && grep -q "^FAIL vkGetPhysicalDeviceImageFormatProperties2, NV12 transfer" out/t-planar-nofix.log
      result $? "planar_check, VKBRIDGE_PLANAR_EXT_FIX=0 fails as the G925 did ($(tail -1 out/t-planar-nofix.log))"
    else
      timeout 60 ./out/planar_check >out/t-planar.log 2>&1; rc=$?; [ $rc = 77 ] && rc=0
      result $rc "planar_check ($(tail -1 out/t-planar.log))"
    fi
  fi
  # Zink's shared linear images (dma-buf, no modifier): Mali refuses them at that tiling (imitated:
  # dmabuflinear) and Skyrim SE exited on its 1D one; the bridge makes them without the handle type
  if want dmabuf_image && [ -x out/dmabuf_image_check ]; then
    timeout 60 ./out/dmabuf_image_check >out/t-dmabuf-image.log 2>&1 &&
      { [ "$mode" != mali ] || grep -q "dma-buf image refused at tiling" "$VKB_SERVER_LOG"; }
    result $? "dmabuf_image_check ($(tail -1 out/t-dmabuf-image.log))"
  fi
  # gamescope's export images (its nested backend's flippable images: a LINEAR-modifier list,
  # DMA_BUF, MUTABLE_FORMAT, storage): asked, made, exported and read through the dma-buf
  if want gsexport && [ -x out/gsexport_check ]; then
    timeout 60 ./out/gsexport_check >out/t-gsexport.log 2>&1; result $? "gsexport_check ($(tail -1 out/t-gsexport.log))"
    if [ "$mode" = mali ]; then
      # Mali-G77 r32p1: the export emulation on (dmabufexport; the host's pages are
      # the driver's own dma-bufs) and the driver refusing gamescope's modifier queries (dmabufmod).
      # gamescope aborted on "modifiers.size > 0": the client answers from plain LINEAR tiling,
      # the server makes the image plain LINEAR and reports the LINEAR modifier. gamescope's imports
      # of its clients' LINEAR dma-bufs (sampled, MUTABLE_FORMAT, the explicit LINEAR modifier) are
      # answered importable and made plain LINEAR at the dma-buf's layout too: answered without
      # IMPORTABLE, gamescope made them OPTIMAL, larger than the dma-buf on Mali (S10+, rc1)
      from=$(($(wc -l <"$VKB_SERVER_LOG") + 1))
      VKB_MISSING_ADD=dmabufexport,dmabufmod VKB_SERVER_ENV=VKBRIDGE_TEST_EXPORT_PAGES=driver . ./mali_env.sh >/dev/null
      rm -f out/t-gsexport-r32-client.log
      VKBRIDGE_CLIENT_LOG=$PWD/out/t-gsexport-r32-client.log timeout 60 ./out/gsexport_check >out/t-gsexport-r32.log 2>&1 &&
        tail -n +$from "$VKB_SERVER_LOG" | grep -q "dma-buf export: emulated, pages from the driver's own dma-bufs" &&
        tail -n +$from "$VKB_SERVER_LOG" | grep -q "imitating r32p1's refusal" &&
        [ "$(tail -n +$from "$VKB_SERVER_LOG" | grep -c "dma-buf export image with a LINEAR modifier list .* made as a plain LINEAR image")" = 4 ] &&
        [ "$(tail -n +$from "$VKB_SERVER_LOG" | grep -c "dma-buf import image at the LINEAR modifier .* made as a plain LINEAR image")" = 4 ] &&
        ! tail -n +$from "$VKB_SERVER_LOG" | grep -q "dma-buf import: .* bytes asked for" &&
        grep -q "dma-buf image query refused by the driver: format 44 type 1 tiling 1000158000 usage 0xd flags 0x8 (2 view formats) modifier 0x0: result -11, external features 0x0; answered from plain LINEAR" out/t-gsexport-r32-client.log
      result $? "gsexport_check, r32p1's refused modifier queries with the export emulated ($(tail -1 out/t-gsexport-r32.log))"
      if docker image inspect vkb-arch-gamescope >/dev/null 2>&1; then
        VKB_DOCKER_EXTRA="--device /dev/dri/renderD128" VKBRIDGE_DRM_RENDER=226:128 timeout 120 ./gamescope_test.sh \
          "bash -c 'export MESA_LOADER_DRIVER_OVERRIDE=zink GALLIUM_DRIVER=zink LIBGL_KOPPER_DRI2=true; timeout 20 glxgears 2>&1 | grep -m1 frames'" \
          >out/t-gamescope-r32.log 2>&1
        grep -q "frames in" out/t-gamescope-r32.log && ! grep -q "Assertion" out/t-gamescope-r32.log &&
          tail -n +$from "$VKB_SERVER_LOG" | grep -q "format 44 960x540 usage 0x[0-9a-f]* flags 0x8); made as a plain LINEAR" &&
          tail -n +$from "$VKB_SERVER_LOG" | grep -q "dma-buf import image at the LINEAR modifier refused by the driver's format query (format 44 300x300 usage 0x4"
        result $? "gamescope + Zink glxgears, r32p1's refused modifier queries with the export emulated ($(grep -m1 -o '[0-9.]* FPS' out/t-gamescope-r32.log)$(grep -m1 -o 'Assertion.*' out/t-gamescope-r32.log))"
      fi
      unset VKB_MISSING_ADD VKB_SERVER_ENV
      . ./mali_env.sh >/dev/null
    fi
  fi
  # gamescope's DMA-BUF import for other GPUs (the fd's memoryTypeBits AND the image's): on mali
  # with Mali's fd properties (dmabuftypes: a dma-buf takes only the host-visible cached types,
  # which the client offers as shared) the fd's bits were 0; the server imports in a type it takes
  if want fdprops && [ -x out/fdprops_check ]; then
    if [ "$mode" = mali ]; then
      VKB_MISSING_ADD=dmabuftypes . ./mali_env.sh >/dev/null
      timeout 60 ./out/fdprops_check >out/t-fdprops.log 2>&1 && grep -q "imitating Mali's dma-buf fd properties" "$VKB_SERVER_LOG" &&
        grep -q "dma-buf import: type [0-9]* is not one the dma-buf takes .*; imported in type" "$VKB_SERVER_LOG"
      result $? "fdprops_check, Mali's dma-buf fd properties ($(tail -1 out/t-fdprops.log))"
      . ./mali_env.sh >/dev/null
    else
      timeout 60 ./out/fdprops_check >out/t-fdprops.log 2>&1; result $? "fdprops_check ($(tail -1 out/t-fdprops.log))"
    fi
  fi
  if want memclass && [ -x out/memclass_check ]; then timeout 60 ./out/memclass_check >out/t-memclass.log 2>&1; result $? "memclass_check ($(tail -1 out/t-memclass.log))"; fi
  # Extension commands through the procaddr functions (DroidDeck#165): physical-device ones of
  # extensions the device lacks are safe to call (the server crashed on VK_ARM_tensors' at address
  # 0); vkGetDeviceProcAddr follows the enabled extensions (VK_KHR_calibrated_timestamps alone gave
  # NULL for vkGetCalibratedTimestampsKHR); and with calibrated timestamps hidden from the app
  if want procaddr && [ -x out/procaddr_check ]; then
    timeout 60 ./out/procaddr_check >out/t-procaddr.log 2>&1 && ! grep -q "crashed with signal" "$VKB_SERVER_LOG"
    result $? "procaddr_check ($(tail -1 out/t-procaddr.log))"
    VKBRIDGE_HIDE_EXTS=${VKBRIDGE_HIDE_EXTS:+$VKBRIDGE_HIDE_EXTS,}VK_KHR_calibrated_timestamps,VK_EXT_calibrated_timestamps timeout 60 ./out/procaddr_check >out/t-procaddr-hidden.log 2>&1
    result $? "procaddr_check, calibrated timestamps hidden ($(tail -1 out/t-procaddr-hidden.log))"
  fi
  # Processes gone without destroying their devices while another keeps importing dma-bufs (Tab
  # S10+, v0.5.1 rc1: the server crashed in the second steamwebhelper's cleanup, its null
  # descriptors' buffer destroyed through the dedicated_import erratum without the device's table,
  # after the last device made had been destroyed); with Mali's errata (r44p1), every other round
  # after a device made and destroyed as it should
  if want exitclean && [ -x out/exitclean_check ] && [ "$mode" = mali ]; then
    from=$(($(wc -l <"$VKB_SERVER_LOG") + 1))
    VKB_SERVER_ENV=VKBRIDGE_FAKE_DRIVER_VERSION=r44p1 VKB_MISSING_ADD=dmabuftypes . ./mali_env.sh >/dev/null
    timeout 180 ./out/exitclean_check >out/t-exitclean.log 2>&1 && ! tail -n +$from "$VKB_SERVER_LOG" | grep -q "crashed with signal" &&
      [ "$(tail -n +$from "$VKB_SERVER_LOG" | grep -c 'exited without destroying device')" -ge 12 ] &&
      tail -n +$from "$VKB_SERVER_LOG" | grep -q "null descriptors emulated" &&
      tail -n +$from "$VKB_SERVER_LOG" | grep -q "errata: dedicated_import ON"
    result $? "exitclean_check, processes gone without destroying anything ($(tail -1 out/t-exitclean.log))"
    . ./mali_env.sh >/dev/null
  fi
  if want recycle && [ -x out/recycle_check ]; then timeout 120 ./out/recycle_check >out/t-recycle.log 2>&1; result $? "recycle_check ($(tail -1 out/t-recycle.log))"; fi
  # Descriptor traffic: the default (vkbBatch framing, updates not packed), framing + packing
  # (VKBRIDGE_DESC_PACK=1) and batching off; the client's totals in the line
  desc_modes="default: pack:VKBRIDGE_DESC_PACK=1 off:VKBRIDGE_NO_DESC_BATCH=1"
  if want desc_traffic && [ -x out/desc_traffic_check ]; then
    for m in $desc_modes; do
      n=${m%%:*}; e=${m#*:}
      env $e VKBRIDGE_STATS=100000 timeout 120 ./out/desc_traffic_check >out/t-desc-$n.log 2>&1
      result $? "desc_traffic_check, $n ($(tail -1 out/t-desc-$n.log | grep -o '^[A-Z]*'); $(grep -o '[0-9]* messages, [0-9]* KB on the wire' out/t-desc-$n.log))"
    done
    # framing is on by default: at least 5x fewer messages than with batching off
    md=$(grep -o '[0-9]* messages' out/t-desc-default.log | tail -1 | grep -o '[0-9]*'); mo=$(grep -o '[0-9]* messages' out/t-desc-off.log | tail -1 | grep -o '[0-9]*')
    [ -n "$md" ] && [ -n "$mo" ] && [ $((md * 5)) -lt "$mo" ]
    result $? "descriptor batching on by default ($md messages vs $mo with VKBRIDGE_NO_DESC_BATCH=1)"
  fi
  # DXVK's frame structure (frames in flight, pools reset per frame, submits from another thread)
  if want flicker && [ -x out/flicker_check ]; then
    for m in $desc_modes; do
      n=${m%%:*}; e=${m#*:}
      env $e timeout 180 ./out/flicker_check >out/t-flicker-$n.log 2>&1
      result $? "flicker_check, $n ($(grep -o '[0-9]* draws wrong in [0-9]* frames' out/t-flicker-$n.log))"
    done
  fi
  # What the compositor receives is the frame presented (Elden Ring's back-and-forth pan, 2026-10-28):
  # vkd3d-proton's swapchain pattern, a slow GPU job before each frame, the compositor reading each
  # committed dma-buf at once and again at its release (needs libwayland-server; exit 3 = skipped)
  if want stale_frames && [ -x out/stale_frames_check ]; then
    for nb in 0 1; do
      VKBRIDGE_DESC_BATCH=$((1-nb)) timeout 180 ./out/stale_frames_check --frames 200 --hold-ms 40 >out/t-stale-$nb.log 2>&1; r=$?
      if [ $r = 3 ]; then echo "  skip stale_frames_check ($(tail -1 out/t-stale-$nb.log))"
      else result $r "stale_frames_check, VKBRIDGE_NO_DESC_BATCH=$nb ($(grep -E '^(OK|FAILED)' out/t-stale-$nb.log))"; fi
    done
  fi
  if want d3d11 && [ -n "${VKB_WINE_DIR:-}" ] && [ -d "$VKB_WINE_DIR/app" ]; then
    ./d3d11_test.sh >out/t-d3d11.log 2>&1; grep -q "^OK:" out/t-d3d11.log; result $? "d3d11_check via DXVK ($(grep -E '^(OK|FAILED)' out/t-d3d11.log))"
    # Every frame read back, through DXVK, with batching on (needs d3d11_flicker.exe in the prefix)
    if want d3d11_flicker && [ -f "$VKB_WINE_DIR/app/d3d11_flicker.exe" ]; then
      ./d3d11_test.sh d3d11_flicker.exe >out/t-d3d11-flicker.log 2>&1; grep -q "^OK" out/t-d3d11-flicker.log
      result $? "d3d11_flicker via DXVK, batched by default ($(grep -o '[0-9]* cells wrong in [0-9]* frames' out/t-d3d11-flicker.log))"
      VKBRIDGE_DESC_PACK=1 ./d3d11_test.sh d3d11_flicker.exe >out/t-d3d11-flicker-pack.log 2>&1; grep -q "^OK" out/t-d3d11-flicker-pack.log
      result $? "d3d11_flicker via DXVK, batched + packed ($(grep -o '[0-9]* cells wrong in [0-9]* frames' out/t-d3d11-flicker-pack.log))"
    fi
  fi
  # Persistent pipeline cache: Steam app keys, pre-caching taken in, killed games, shared files
  # (a server of its own with a cache directory; once, the mode makes no difference to it)
  if want pcache && [ "$mode" = plain ] && [ -x out/pcache_check ]; then
    timeout 120 ./pcache_test.sh >out/t-pcache.log 2>&1; result $? "pcache_test ($(tail -1 out/t-pcache.log))"
  fi
  # Waits the driver fails with -3 after a signal (wait_retry, MK-01): every kind the bridge
  # forwards, the bug imitated beneath the fix, with it and without; and the server-side imitation
  # of an older Mali's feature set, both from <cache dir>/env (a server of its own; once)
  if [ "$mode" = plain ]; then
    if want wait_imitate; then
      timeout 300 ./wait_imitate_test.sh >out/t-wait.log 2>&1; result $? "wait_imitate_test ($(tail -1 out/t-wait.log))"
    fi
    # waitlost_test.sh: a timed-out semaphore wait answered VK_ERROR_DEVICE_LOST, checked where the probe saw it
    if want waitlost; then
      timeout 300 ./waitlost_test.sh >out/t-waitlost.log 2>&1; result $? "waitlost_test ($(tail -1 out/t-waitlost.log))"
    fi
    # A pipeline compile that never ends (the G925's compiler on Stray): interrupted, the call fails
    if want compile_watchdog; then
      timeout 400 ./compile_watchdog_test.sh >out/t-spin.log 2>&1; result $? "compile_watchdog_test ($(tail -1 out/t-spin.log))"
    fi
    # UE4's layer-only pass-through geometry stage folded into the vertex shader (Stray's stuck
    # pipelines): up front with VKBRIDGE_LAYER_GS_FOLD=1, and as the never-ending compile's fallback
    if want gsfold; then
      timeout 300 ./gsfold_test.sh >out/t-gsfold.log 2>&1; result $? "gsfold_test ($(tail -1 out/t-gsfold.log))"
    fi
    # The startup probe (imitated failures switch their emulations on, a dead start resumes, a
    # small budget defers to the background) and the session report with its diff tool
    if want probe_report; then
      timeout 300 ./probe_report_test.sh >out/t-probe.log 2>&1; result $? "probe_report_test ($(tail -1 out/t-probe.log))"
    fi
    if want report_stall; then
      timeout 120 ./report_stall_test.sh >out/t-stall.log 2>&1; result $? "report_stall_test ($(tail -1 out/t-stall.log))"
    fi
    # A request the driver has not returned from, named in the report while it runs
    if want report_stuck; then
      timeout 150 ./report_stuck_test.sh >out/t-stuck.log 2>&1; result $? "report_stuck_test ($(tail -1 out/t-stuck.log))"
    fi
    if want report_cpu; then
      timeout 120 ./report_cpu_test.sh >out/t-cpu.log 2>&1; result $? "report_cpu_test ($(tail -1 out/t-cpu.log))"
    fi
    # GPU memory per process with ten programs holding memory at once (eight before a game in Steam)
    if want report_gpumem; then
      timeout 120 ./report_gpumem_test.sh >out/t-gpumem.log 2>&1; result $? "report_gpumem_test ($(tail -1 out/t-gpumem.log))"
    fi
    # A frame held 15 s on the GPU is presented late, not a lost device (Watch_Dogs loading
    # on a Dimensity 8300); the limit from the guest and from <cache dir>/env (a server of its own; once)
    if want present_timeout; then
      timeout 300 ./present_timeout_test.sh >out/t-ptimeout.log 2>&1; result $? "present_timeout_test ($(tail -1 out/t-ptimeout.log))"
    fi
    # The per-hitch line (VKBRIDGE_HITCH_MS): a slow compile, BC conversion, allocation, held queue
    # and CPU sleep each named by their cause; the switch from <cache dir>/env, the server, the
    # guest; off by default; the DXVK config check (a server of its own; once)
    if want hitch; then
      timeout 300 ./hitch_test.sh >out/t-hitch.log 2>&1; result $? "hitch_test ($(tail -1 out/t-hitch.log))"
    fi
    # The conversion budget (VKBRIDGE_BC_TX_BUDGET): a burst of BC uploads on a GPU slowed to a
    # Mali's, without it, with 32:4 (DS3's preset), decoded and unset (off); the contents, the memory cap, query pins
    if want bcbudget; then
      timeout 900 ./bcbudget_test.sh >out/t-bcbudget.log 2>&1; result $? "bcbudget_test ($(tail -1 out/t-bcbudget.log))"
    fi
    # The latency log (VKBRIDGE_LATLOG): one line a frame with every stage, from <cache dir>/env, the
    # server, the guest, a path of its own; off by default; tools/latency_report.py reads it; and
    # tools/lagprobe.exe through DXVK followed press by press (with VKB_WINE_DIR; a server of its own; once)
    if want latency; then
      timeout 120 ./latency_test.sh >out/t-latency.log 2>&1; result $? "latency_test ($(tail -1 out/t-latency.log))"
    fi
    if want lagprobe; then
      timeout 240 ./lagprobe_test.sh >out/t-lagprobe.log 2>&1; result $? "lagprobe_test ($(tail -1 out/t-lagprobe.log))"
    fi
    # The frame queue cap (VKBRIDGE_FRAME_QUEUE): a present on the presenting thread's path returns once
    # at most N-1 earlier frames are rendering; from the guest and <cache dir>/env; off by default and by 0
    if want framequeue; then
      timeout 200 ./framequeue_test.sh >out/t-framequeue.log 2>&1; result $? "framequeue_test ($(tail -1 out/t-framequeue.log))"
    fi
    # Present ids and waits (VK_KHR_present_id/present_wait and id2/wait2, client/wsi.c): a wait returns
    # once that frame is shown, times out, ends on retire and destroy; off with VKBRIDGE_PRESENT_WAIT=0
    if want presentwait; then
      timeout 200 ./presentwait_test.sh >out/t-presentwait.log 2>&1; result $? "presentwait_test ($(tail -1 out/t-presentwait.log))"
    fi
    # A copy present (X11, wl_shm, headless) waits for its frame at most VKBRIDGE_PRESENT_HOLD_MS, then the
    # swapchain's thread shows it: a frame waiting for the presenting thread's own later signal no longer
    # holds that thread until the present timeout; the dma-buf path's presenting thread named its own
    if want present_block; then
      timeout 200 ./present_block_test.sh >out/t-pblock.log 2>&1; result $? "present_block_test ($(tail -1 out/t-pblock.log))"
    fi
    # Under a nested gamescope the gralloc images are offered to the app's compositor, which pairs them
    # with the same dma-bufs gamescope passes on; every off case keeps ordinary dma-bufs
    if want zerocopy_nested; then
      timeout 300 ./zerocopy_nested_test.sh >out/t-zcnested.log 2>&1; result $? "zerocopy_nested_test ($(tail -1 out/t-zcnested.log))"
    fi
    # A hangup does not end the separate server: the app stops it last, after the session's programs
    if want server_hup; then
      timeout 60 ./server_hup_test.sh >out/t-hup.log 2>&1; result $? "server_hup_test ($(tail -1 out/t-hup.log))"
    fi
    # A start that ends inside a probe: a stop or a kill runs it again, an abort or a hang past the
    # watchdog is died and decided; the default budget defers the heavy probes, both parts timed
    if want probe_end; then
      timeout 300 ./probe_end_test.sh >out/t-probe-end.log 2>&1; result $? "probe_end_test ($(tail -1 out/t-probe-end.log))"
    fi
    # The BC transcoder builds: a slow one passes, one past their own limit leaves only its formats
    # decoded, those after a hang are not tried (a server of its own; once)
    if want probe_tx; then
      timeout 300 ./probe_tx_test.sh >out/t-probe-tx.log 2>&1; result $? "probe_tx_test ($(tail -1 out/t-probe-tx.log))"
    fi
    # The driver-errata layer: decisions by driver version, mode, probe and switch; a fix with its
    # imitation beneath it, the bridge's own calls through both (a server of its own; once)
    if want errata; then
      timeout 300 ./errata_test.sh >out/t-errata.log 2>&1; result $? "errata_test ($(tail -1 out/t-errata.log))"
    fi
    # MK-03 bitwise_scalar: the miscompile imitated fails bitwise_check, the SPIR-V rewrite over it
    # passes; only modules with the pattern change; the corpus through the pass is spirv-val clean
    if want bitwise; then
      timeout 600 ./bitwise_test.sh >out/t-bitwise.log 2>&1; result $? "bitwise_test ($(tail -1 out/t-bitwise.log); $(grep -o 'corpus: [0-9]* modules' out/t-bitwise.log))"
    fi
    # shl_barrier: the no-op after constant left shifts, forced on: right values in bitwise_check and
    # vkbridge-smoke, modules named by the app's code, the corpus through the pass spirv-val clean
    if want shlbarrier; then
      timeout 600 ./shlbarrier_test.sh >out/t-shlbarrier.log 2>&1; result $? "shlbarrier_test ($(tail -1 out/t-shlbarrier.log))"
    fi
    # Version promotion: the desktop driver taken as a 1.1 / 1.2 one offered as 1.3 (or 1.2 when a
    # piece is missing), core structures and names through the extensions, SPIR-V lowered (the corpus
    # spirv-val clean), the switch, Mali's gaps on top (a server of its own per step; once)
    if want promote; then
      timeout 600 ./promote_test.sh >out/t-promote.log 2>&1; result $? "promote_test ($(tail -1 out/t-promote.log))"
    fi
    # apicap_test.sh: the Vulkan version cap for Steam mode's processes, and its switch
    if want apicap; then
      timeout 600 ./apicap_test.sh >out/t-apicap.log 2>&1; result $? "apicap_test ($(tail -1 out/t-apicap.log))"
    fi
    # shm_barrier: barriers between shared-memory writes and reads in compute shaders (per-game:
    # RE Engine titles), forced on and from a preset file: right values in shmbarrier_check, the
    # corpus cases' barriers where expected and spirv-val clean
    if want shmbarrier; then
      timeout 600 ./shmbarrier_test.sh >out/t-shmbarrier.log 2>&1; result $? "shmbarrier_test ($(tail -1 out/t-shmbarrier.log))"
    fi
    # wave32: 32-lane subgroups emulated in compute shaders (per-game: RE Engine titles) on llvmpipe's
    # 8-wide subgroups: wave32_check right, allops/wg32 bit-exact with RADV at a required 32, the
    # corpus cases where expected and spirv-val clean
    if want wave32; then
      timeout 600 ./wave32_test.sh >out/t-wave32.log 2>&1; result $? "wave32_test ($(tail -1 out/t-wave32.log))"
    fi
    # The YCbCr sampler swap in compute layouts (Mali, Samsung's Xclipse;): the
    # driver's refusal imitated, pipelines refused with the swap off and built with it on, the
    # first failure logged with its layout's YCbCr samplers; gamescope's composite pipelines too
    if want ycbcr_swap; then
      timeout 600 ./ycbcr_swap_test.sh >out/t-ycbcr-swap.log 2>&1; result $? "ycbcr_swap_test ($(tail -1 out/t-ycbcr-swap.log))"
    fi
    # Arm's driver told by driverID: Mesa PanVK imitated (vendor 0x13B5, a Mesa driverVersion inside
    # the errata's ranges) gets none of Arm's errata or fixes, Arm's driver gets them, and a driver
    # without driverID is told by the vendor ID as before (a server of its own per step; once)
    if want driverid; then
      timeout 300 ./driverid_test.sh >out/t-driverid.log 2>&1; result $? "driverid_test ($(tail -1 out/t-driverid.log))"
    fi
    # MK-32 clamp_zero: the zero limit dropped (imitated) fails clampzero_check, the opaque zero over it
    # passes, narrow from the version gate and wide when forced; the corpus through both is spirv-val clean
    if want clampzero; then
      timeout 600 ./clampzero_test.sh >out/t-clampzero.log 2>&1; result $? "clampzero_test ($(tail -1 out/t-clampzero.log); $(grep -o 'corpus, narrow: [0-9]* modules' out/t-clampzero.log))"
    fi
    # The image-view errata: MK-07's REMAINING counts and MK-06's view quarantine, each failing with
    # its bug imitated and the fix off and passing with the fix; the quarantine's caps and the
    # server's RSS over 40000 churned views (a server of its own; once)
    if want views; then
      timeout 300 ./views_test.sh >out/t-views.log 2>&1; result $? "views_test ($(tail -1 out/t-views.log))"
    fi
    # The command-buffer state errata: MK-04's dynamic state sent again after binds and MK-05's
    # split vertex buffer binds, each failing with its bug imitated and the fix off and passing
    # with the fix; the two stride shapes the probe tells apart (a server of its own; once)
    if want cmdstate; then
      timeout 300 ./cmdstate_test.sh >out/t-cmdstate.log 2>&1; result $? "cmdstate_test ($(tail -1 out/t-cmdstate.log))"
    fi
    if want vbbake; then
      timeout 300 ./vbbake_test.sh >out/t-vbbake.log 2>&1; result $? "vbbake_test ($(tail -1 out/t-vbbake.log))"
    fi
    # Zink's vertex input dynamic state: the driver's, or made static in pipeline variants (the
    # extension and the packed formats imitated missing), one layout after another and in a secondary
    if want vidyn; then
      timeout 600 ./vidyn_test.sh >out/t-vidyn.log 2>&1; result $? "vidyn_test ($(tail -1 out/t-vidyn.log))"
    fi
    # eds_test.sh: extended dynamic state 1 and 2 made static in vertex-input variants where the driver has neither
    if want eds; then
      timeout 600 ./eds_test.sh >out/t-eds.log 2>&1; result $? "eds_test ($(tail -1 out/t-eds.log))"
    fi
    # MK-16 cull_nontri (a dynamic cull mode culls points and lines): failing with its bug imitated
    # and the fix off, passing with the fix; the version gate (a server of its own; once)
    if want cullmode_test; then
      timeout 300 ./cullmode_test.sh >out/t-cullmode-test.log 2>&1; result $? "cullmode_test ($(tail -1 out/t-cullmode-test.log))"
    fi
    # MK-39 dedicated_import (a dedicated struct on an import into a resource not made for its handle
    # type): failing with its bug imitated and the fix off, passing with the fix; the version gate
    if want dedimport; then
      timeout 300 ./dedimport_test.sh >out/t-dedimport.log 2>&1; result $? "dedimport_test ($(tail -1 out/t-dedimport.log))"
    fi
    # MK-11 stencil_layout (a separate stencil layout ignored, Arm 4021056): failing with its bug
    # imitated and the fix off, passing with the fix; the version gate (r41p0-r54p0)
    if want stencillayout; then
      timeout 300 ./stencillayout_test.sh >out/t-stencillayout.log 2>&1; result $? "stencillayout_test ($(tail -1 out/t-stencillayout.log))"
    fi
    # Dynamic rendering as render passes (Zink's requirement; Mali's Vulkan 1.1 drivers lack it): the
    # driver's own, emulated at 1.2, switched off, and on a 1.1 driver promoted to 1.2
    if want dynrend; then
      timeout 300 ./dynrend_test.sh >out/t-dynrend.log 2>&1; result $? "dynrend_test ($(tail -1 out/t-dynrend.log))"
    fi
    # a1zink_test.sh: extended dynamic state hidden and dynamic rendering as render passes, and its switch
    if want a1zink; then
      timeout 300 ./a1zink_test.sh >out/t-a1zink.log 2>&1; result $? "a1zink_test ($(tail -1 out/t-a1zink.log))"
    fi
    # server switches from <cache dir>/env
    if want envswitch; then
      timeout 300 ./envswitch_test.sh >out/t-envswitch.log 2>&1; result $? "envswitch_test ($(tail -1 out/t-envswitch.log))"
    fi
    # The MK probes: each erratum's bug imitated beneath its probe makes it fail with the bug's
    # pattern and the decision follow (probe-gated errata on in performance mode), none without;
    # the report's errata table; the slow light probes in the background (a server of its own; once)
    if want probe_mk; then
      timeout 600 ./probe_mk_test.sh >out/t-probe-mk.log 2>&1; result $? "probe_mk_test ($(tail -1 out/t-probe-mk.log))"
    fi
    # MK-02 viewport_flip: negative viewports drawn with the bug imitated fail viewport_check (and
    # DXVK's d3d11_check, with VKB_WINE_DIR), the fix over it passes, forced on a correct driver
    # it is invisible; the gate, the cost of the variants, the SPIR-V pass over the corpus
    if want viewport_test; then
      timeout 900 ./viewport_test.sh >out/t-viewport.log 2>&1; result $? "viewport_test ($(tail -1 out/t-viewport.log))"
    fi
    # Safe and performance mode: where the mode comes from, both modes' emulation masks from one
    # probe (not run yet / error: emulated in safe mode only), switched under a running server, the
    # watchdog's timeout, the report's first line, the client's defaults (a server of its own; once)
    if want mode; then
      timeout 600 ./mode_test.sh >out/t-mode.log 2>&1; result $? "mode_test ($(tail -1 out/t-mode.log))"
    fi
    # Per-game presets by Steam app id: two games' own knobs, at once too; the precedence (file,
    # <cache dir>/env, environment, built-in); VKBRIDGE_MODE per game; bad lines warned and skipped;
    # the client's switch through vkbQueryServer; the report's preset per process; a built-in preset
    # only on a GPU it was tested on (a server of its own; once)
    if want presets; then
      timeout 600 ./presets_test.sh >out/t-presets.log 2>&1; result $? "presets_test ($(tail -1 out/t-presets.log))"
    fi
    # VKBRIDGE_HIDE_CALIBRATED_TS (opt-in, per game): both names or the EXT name only hidden, from
    # <cache dir>/env and from one game's preset (a server of its own; once)
    if want calts; then
      timeout 300 ./calts_test.sh >out/t-calts.log 2>&1; result $? "calts_test ($(tail -1 out/t-calts.log))"
    fi
    # The Mali-G715 r54p3's robustness2 (nullDescriptor only): with the emulation off DXVK finds no
    # adapter (the Pixel testers' CreateDXGIFactory E_FAIL); on, both robust-access features are
    # reported and taken off before the driver, DXVK's d3d11_check passes (a server of its own; once)
    if want robust2; then
      timeout 600 ./robust2_test.sh >out/t-robust2.log 2>&1; result $? "robust2_test ($(tail -1 out/t-robust2.log))"
    fi
    # MK-20 fp16_hide: a driver computing 16-bit floats wrong (imitated) fails fp16_math and has
    # shaderFloat16 hidden (16-bit storage kept: DXVK requires it), an app enabling it anyway has it taken off;
    # VKBRIDGE_NO_FP16=0/1 from the environment and a game's preset; unchanged on a right driver
    # (a server of its own; once)
    if want fp16; then
      timeout 600 ./fp16_test.sh >out/t-fp16.log 2>&1; result $? "fp16_test ($(tail -1 out/t-fp16.log))"
    fi
    # Issue #11 gs_hide: the Galaxy S10+'s r38p1 beta (imitated: its version faked, a fault in every
    # pipeline with a geometry stage) - geometry shaders hidden from the listed processes only, on that
    # driver only, VKBRIDGE_ERRATA/VKBRIDGE_GS_HIDE_PROCS; gamescope's Xwayland (glamor on Zink, GL 3.0)
    # dies without the fix as on the phone and draws with it (a server of its own per step; once)
    if want gshide; then
      timeout 900 ./gshide_test.sh >out/t-gshide.log 2>&1; result $? "gshide_test ($(tail -1 out/t-gshide.log))"
    fi
    # DXVK 1.x turns on transform feedback's geometryStreams whatever the query said; a driver without
    # it (Mali-G720 r44p1, imitated) refused every D3D10/11 device (Hollow Knight on DXVK 1.10.3, Tab
    # S10+): the bit taken off before the driver, the device made; DXVK 1.10.3's d3d11_check with
    # VKB_DXVK1_WINE_DIR (a server of its own; once)
    if want featclamp; then
      timeout 600 ./featclamp_test.sh >out/t-featclamp.log 2>&1; result $? "featclamp_test ($(tail -1 out/t-featclamp.log))"
    fi
    # vkBeginCommandBuffer's begin info: primary and secondary (inheritance info) round trips; one
    # encoded without its count (the 2026-10-10 copy's bug, SIGSEGV at 0x10 in Mali's
    # vkBeginCommandBuffer) is refused before the driver, its commands dropped (a server of its own; once)
    if want begin_encoding; then
      timeout 120 ./begin_encoding_test.sh >out/t-begin.log 2>&1; result $? "begin_encoding_test ($(tail -1 out/t-begin.log))"
    fi
    if want poolfull; then
      timeout 120 ./poolfull_test.sh >out/t-poolfull.log 2>&1; result $? "poolfull_test ($(tail -1 out/t-poolfull.log))"
    fi
    # Low memory: said once with what the server holds, its memory line marked, idle
    # descriptor overflow pools freed at resets and made again when needed (a server of its own; once)
    if want lowmem; then
      timeout 120 ./lowmem_test.sh >out/t-lowmem.log 2>&1; result $? "lowmem_test ($(tail -1 out/t-lowmem.log))"
    fi
    # the compile gate on its own (server_compile_gate.c): its limit, one at a time while memory is low
    if want compilegate; then
      timeout 120 ./out/compilegate_unit >out/t-compilegate.log 2>&1; result $? "compilegate_unit ($(tail -1 out/t-compilegate.log))"
    fi
    # DX12 reserved buffers (server_emu_sparse.c): an address for vkd3d-proton's sparse buffers on a driver without sparse binding
    if want sparse; then
      timeout 120 ./sparse_test.sh >out/t-sparse.log 2>&1; result $? "sparse_test ($(tail -1 out/t-sparse.log))"
    fi
    # Cached readback: the self-test's cached pages, offered per VKBRIDGE_CACHED_READBACK and the
    # mode; the client's frame readback (vkcube's X11 path) and an app's non-coherent cached type
    # read back right through DMA_BUF_IOCTL_SYNC (dma-buf strategy; a server of its own; once)
    if want cached; then
      timeout 300 ./cached_test.sh >out/t-cached.log 2>&1; result $? "cached_test ($(tail -1 out/t-cached.log))"
    fi
    # What the app is shown: VKBRIDGE_HEAP_MAX_MB (device-local heaps and their budget capped) and
    # VKBRIDGE_SPOOF_GPU (vendor, device, name; driverID and the server's own view real), from the
    # guest, a preset file and a built-in preset; the untested built-in entries not applied (a
    # server of its own; once)
    if want heapcap; then
      timeout 300 ./heapcap_test.sh >out/t-heapcap.log 2>&1; result $? "heapcap_test ($(tail -1 out/t-heapcap.log))"
    fi
    # <cache dir>/env written, changed or removed while the server runs: the first process after
    # it gets the new client switches (vkbQueryServer reads the file again when it changed; a
    # server of its own; once)
    if want envfile; then
      timeout 300 ./envfile_test.sh >out/t-envfile.log 2>&1; result $? "envfile_test ($(tail -1 out/t-envfile.log))"
    fi
    # The Xclipse 530 (Galaxy A55): no host pointers and external buffers in no host-visible type
    # (VKBRIDGE_FAKE_MISSING=hostptr,extbufhv): smoke 6 of 13 with the fallback off, 13 of 13 with
    # buffers made without the handle type; nothing faked, the choice unchanged (a server of its own; once)
    if want unflagged; then
      timeout 400 ./unflagged_test.sh >out/t-unflagged.log 2>&1; result $? "unflagged_test ($(tail -1 out/t-unflagged.log))"
    fi
    # xclipse_zink_test.sh: Zink on Samsung's Xclipse driver without EDS2 (push descriptors kept)
    if want xclipse_zink; then
      timeout 400 ./xclipse_zink_test.sh >out/t-xclipse-zink.log 2>&1; result $? "xclipse_zink_test ($(tail -1 out/t-xclipse-zink.log))"
    fi
    # syncfd_test.sh: sync-fd exports answered within a deadline; a held async submit named in the log
    if want syncfd; then
      timeout 300 ./syncfd_test.sh >out/t-syncfd.log 2>&1; result $? "syncfd_test ($(tail -1 out/t-syncfd.log))"
    fi
    # dmabufsync_test.sh: a frame handed to another process is finished first on kernels without dma-buf sync files
    if want dmabufsync; then
      timeout 300 ./dmabufsync_test.sh >out/t-dmabufsync.log 2>&1; result $? "dmabufsync_test ($(tail -1 out/t-dmabufsync.log))"
    fi
    # timeline_test.sh: timeline semaphores kept by the bridge where the driver has none (Mali r18/r19)
    if want timeline; then
      timeout 300 ./timeline_test.sh >out/t-timeline.log 2>&1; result $? "timeline_test ($(tail -1 out/t-timeline.log))"
    fi
    # dma-buf memory over AHardwareBuffers where the driver has none (Kirin, Maleoon, Venus): the register alone,
    # then gamescope's and Zink's exports and imports with the desktop driver's dma-bufs standing in for the buffers
    if want dabreg; then
      timeout 60 ./out/dabreg_unit >out/t-dabreg.log 2>&1; result $? "dabreg_unit ($(tail -1 out/t-dabreg.log))"
    fi
    if want dmabufahb; then
      timeout 400 ./dmabufahb_test.sh >out/t-dmabufahb.log 2>&1; result $? "dmabufahb_test ($(tail -1 out/t-dmabufahb.log))"
    fi
    # Asynchronous pipeline compiles (VKBRIDGE_ASYNC_COMPILE) with a slow compiler imitated: draws
    # skipped until the pipeline is there, depth-only and full-screen pipelines synchronous, objects
    # destroyed during a compile; off by default; the other checks unchanged through the layer
    if want asyncpipe; then
      timeout 300 ./asyncpipe_test.sh >out/t-asyncpipe.log 2>&1; result $? "asyncpipe_test ($(tail -1 out/t-asyncpipe.log))"
    fi
    # Compile hitches: frames with new pipelines at their draws, a slow compiler imitated, off, =1
    # (draws skipped) and =3 (deferred: none skipped, shorter frames, fewer compile hitch lines);
    # warm runs the same in both (a server of its own; once)
    if want compileburst; then
      timeout 300 ./compileburst_test.sh >out/t-compileburst.log 2>&1; result $? "compileburst_test ($(tail -1 out/t-compileburst.log))"
    fi
    # Steam's shader pre-caching end to end: Steam's fossilize_replay on an archive recorded through
    # the bridge and on one recorded with GPL on the desktop driver, then the game finds those
    # pipelines in its cache (the driver's creation feedback); skipped without Proton or Steam's
    # replayer and layer (a Mali-imitating server of its own; once)
    if want fossilize; then
      timeout 600 ./fossilize_test.sh >out/t-fossilize.log 2>&1; result $? "fossilize_test ($(tail -1 out/t-fossilize.log))"
    fi
  fi
  # Negative and positive viewports, static and dynamic, two of them, points, a geometry stage,
  # pipeline libraries: every pixel against the CPU's (facing, gl_FragCoord, dFdy) - MK-02's check
  # (mali: multiViewport is emulated, one viewport drawn: no two-viewport cases)
  if want viewport_check; then
    timeout 60 ./out/viewport_check $([ "$mode" = mali ] && echo --no-multi) >out/t-viewport-check.log 2>&1
    result $? "viewport_check ($(tail -1 out/t-viewport-check.log))"
  fi
  # Views with VK_REMAINING_* counts (3D images as 2D arrays: MK-07, always fixed) and views made,
  # rendered to and destroyed over and over (MK-06's pattern), through this mode's server
  if want view3d; then
    timeout 60 ./out/view3d_check >out/t-view3d.log 2>&1; result $? "view3d_check ($(tail -1 out/t-view3d.log))"
  fi
  if want viewchurn; then
    timeout 60 ./out/viewchurn_check 500 >out/t-viewchurn.log 2>&1; result $? "viewchurn_check ($(tail -1 out/t-viewchurn.log))"
  fi
  # Dynamic state across pipeline binds and vertex strides bound in one call or one per binding
  # (MK-04 and MK-05, both on in safe mode), through this mode's server; on mali the driver lacks
  # maintenance5 (imitated beneath every layer: VK_WHOLE_SIZE in vkCmdBindVertexBuffers2's pSizes
  # binds an empty range, as on the G720 r44p1), and nothing in this mode's run may have sent it
  if want dynstate; then
    timeout 60 ./out/dynstate_check >out/t-dynstate.log 2>&1 &&
      { [ "$mode" != mali ] || ! grep -q "VK_WHOLE_SIZE vertex buffer size reached" "$VKB_SERVER_LOG"; }
    result $? "dynstate_check ($(tail -1 out/t-dynstate.log)$([ "$mode" = mali ] && echo "; $(grep -c 'VK_WHOLE_SIZE vertex buffer size reached' "$VKB_SERVER_LOG") VK_WHOLE_SIZE at the driver"))"
  fi
  if want vbstride; then
    timeout 60 ./out/vbstride_check >out/t-vbstride.log 2>&1; result $? "vbstride_check ($(tail -1 out/t-vbstride.log))"
  fi
  # The dedicated-allocation hint (client/memory.c) over Mali's preference imitated beneath it:
  # cleared for ordinary buffers and images with VKBRIDGE_DEDICATED_HINT=1, kept with =0 and by
  # default on a GPU that is not Arm's, kept for large and app-external resources either way; and
  # over the driver's own answers, requiresDedicatedAllocation the same with the switch on and off
  if want dedhint && [ -x out/dedhint_check ]; then
    VKBRIDGE_DEDICATED_HINT=1 VKBRIDGE_TEST_PREFER_DEDICATED=1 timeout 60 ./out/dedhint_check clear >out/t-dedhint-1.log 2>&1 &&
      VKBRIDGE_DEDICATED_HINT=0 VKBRIDGE_TEST_PREFER_DEDICATED=1 timeout 60 ./out/dedhint_check pass >out/t-dedhint-0.log 2>&1 &&
      env -u VKBRIDGE_DEDICATED_HINT VKBRIDGE_TEST_PREFER_DEDICATED=1 timeout 60 ./out/dedhint_check pass >out/t-dedhint-def.log 2>&1
    rc=$?
    VKBRIDGE_DEDICATED_HINT=1 timeout 60 ./out/dedhint_check >out/t-dedhint-r1.log 2>&1
    VKBRIDGE_DEDICATED_HINT=0 timeout 60 ./out/dedhint_check >out/t-dedhint-r0.log 2>&1
    [ $rc = 0 ] && [ -n "$(grep -o 'requires [01]' out/t-dedhint-r1.log)" ] &&
      [ "$(grep -o 'requires [01]' out/t-dedhint-r1.log)" = "$(grep -o 'requires [01]' out/t-dedhint-r0.log)" ]
    result $? "dedhint_check (on: $(tail -1 out/t-dedhint-1.log); off: $(tail -1 out/t-dedhint-0.log); default: $(tail -1 out/t-dedhint-def.log); requires unchanged: $(grep -c 'requires 1' out/t-dedhint-r1.log) required)"
  fi
  # Transform feedback as DXVK's D3D11 stream output uses it (two buffers, counters resumed,
  # vkCmdDrawIndirectByteCountEXT), captured by the vertex stage and by DXVK's pass-through point
  # geometry stage; on mali that stage is folded into the vertex shader (Mali's geometry shaders are
  # slow) and must capture the same bytes, and with VKBRIDGE_NO_XFB_FOLD=1 it is kept. 77: no XFB.
  if want xfb; then
    for st in vs gs; do
      timeout 60 ./out/xfb_check $st >out/t-xfb-$st.log 2>&1; rc=$?; [ $rc = 77 ] && rc=0
      [ $rc = 0 ] && [ "$mode" = mali ] && [ $st = gs ] && ! grep -q "skipped" out/t-xfb-$st.log &&
        { grep -q "stream output geometry shaders folded into their vertex shaders" "$VKB_SERVER_LOG" || rc=1; }
      result $rc "xfb_check, $st captures ($(tail -1 out/t-xfb-$st.log))"
    done
  fi
  if want xfb_nofold; then
    if [ "$mode" = mali ]; then
      from=$(($(wc -l <"$VKB_SERVER_LOG") + 1))
      VKB_SERVER_ENV=VKBRIDGE_NO_XFB_FOLD=1 . ./mali_env.sh >/dev/null
      timeout 60 ./out/xfb_check gs >out/t-xfb-nofold.log 2>&1; rc=$?; [ $rc = 77 ] && rc=0
      [ $rc = 0 ] && tail -n +$from "$VKB_SERVER_LOG" | grep -q "stream output geometry shaders folded" && rc=1
      result $rc "xfb_check, gs captures with VKBRIDGE_NO_XFB_FOLD=1: the geometry stage kept ($(tail -1 out/t-xfb-nofold.log))"
      . ./mali_env.sh >/dev/null
    fi
    # Points and lines with a dynamic cull mode set, triangles culled after them (MK-16, on in safe mode)
  fi
  # Points and lines with a dynamic cull mode set, triangles culled after them (MK-16, on in safe mode)
  if want cullmode_check; then
    timeout 60 ./out/cullmode_check >out/t-cullmode.log 2>&1; result $? "cullmode_check ($(tail -1 out/t-cullmode.log))"
  fi
  # The vertex attribute divisor as vkd3d-proton wants it (EXT name, zero divisor), drawn at
  # divisors 0/2/1: native on plain, emulated on mali
  if want divisor; then
    timeout 60 ./out/divisor_check >out/t-divisor.log 2>&1; result $? "divisor_check ($(tail -1 out/t-divisor.log))"
  fi
  # Polygon mode LINE / POINT (wireframe): native on plain; on mali drawn through the line- and
  # point-list variants (the edge list of non-indexed draws, the compute pass's edges of indexed
  # ones, in primaries and in secondaries as DXVK 3 records render passes on a tiler; nothing drawn
  # solid), and the check fails there with the emulation off (VKBRIDGE_POLYGON_EMU=0: solid)
  if want polygon; then
    timeout 60 ./out/polygon_check >out/t-polygon.log 2>&1 &&
      { [ "$mode" != mali ] || { grep -q "polygon mode LINE drawn as lines (first" "$VKB_SERVER_LOG" &&
        grep -q "polygon mode LINE drawn as lines, indexed" "$VKB_SERVER_LOG" &&
        grep -q "polygon mode LINE drawn as lines, indexed, in a secondary command buffer" "$VKB_SERVER_LOG" &&
        grep -q "polygon mode POINT drawn as points" "$VKB_SERVER_LOG" &&
        ! grep -q "polygon mode [A-Z]* drawn solid (" "$VKB_SERVER_LOG"; }; }
    result $? "polygon_check ($(tail -1 out/t-polygon.log))"
  fi
  if want polygon_off && [ "$mode" = mali ]; then
    VKBRIDGE_POLYGON_EMU=0 . ./mali_env.sh >/dev/null
    timeout 60 ./out/polygon_check >out/t-polygon-off.log 2>&1
    [ $? = 1 ] && grep -q "^FAIL LINE, vkCmdDraw, triangle list" out/t-polygon-off.log
    result $? "polygon_check fails with the emulation off ($(grep -c '^FAIL' out/t-polygon-off.log) checks failed)"
    . ./mali_env.sh >/dev/null
  fi
  # vkGetEventStatus after a host vkSetEvent, or after a wait that saw the GPU set the event, inside
  # the 1 ms quiet window an earlier "reset" answer started (client/icd.c): set, never reset
  if want eventstatus; then
    timeout 60 ./out/eventstatus_check 2000 >out/t-eventstatus.log 2>&1 && grep -q "^OK" out/t-eventstatus.log
    result $? "eventstatus_check ($(tail -1 out/t-eventstatus.log))"
  fi
  if want eventstatus; then
    # the same with the sync mirror off: the quiet window's own answers (the mirror answers most polls)
    timeout 60 env VKBRIDGE_SYNC_MIRROR=0 ./out/eventstatus_check 2000 >out/t-eventstatus-off.log 2>&1 && grep -q "^OK" out/t-eventstatus-off.log
    result $? "eventstatus_check, VKBRIDGE_SYNC_MIRROR=0 ($(tail -1 out/t-eventstatus-off.log))"
  fi
  # Fence and event status from shared memory (client/syncmirror.c): every answer as the app must
  # see it, a held submit kept not ready, and 2000 polls each of a done fence, a set event and a timeline value with
  # under 100 round trips between them; the same checks with the mirror off, all on the wire
  if want syncmirror; then
    timeout 60 env VKBRIDGE_STATS=100000 VKBRIDGE_LOG_LEVEL=2 ./out/syncmirror_check 2000 >out/t-syncmirror.log 2>&1 && grep -q "^OK" out/t-syncmirror.log &&
      ! grep -qE "vk(Get(Fence|Event)Status|GetSemaphoreCounterValue) +[0-9]+ sync" out/t-syncmirror.log
    result $? "syncmirror_check ($(tail -1 out/t-syncmirror.log | grep -o 'OK.*\|FAILED.*'); $(grep -o '[0-9]* round trips' out/t-syncmirror.log))"
    timeout 60 env VKBRIDGE_SYNC_MIRROR=0 VKBRIDGE_STATS=100000 ./out/syncmirror_check 2000 >out/t-syncmirror-off.log 2>&1 &&
      grep -q "^OK" out/t-syncmirror-off.log && grep -qE "vkGetFenceStatus +[0-9]{4,} sync" out/t-syncmirror-off.log
    result $? "syncmirror_check, VKBRIDGE_SYNC_MIRROR=0: the same answers from the server ($(grep -o '[0-9]* round trips' out/t-syncmirror-off.log))"
    # The mirror's cost only where it answers something (Portal on the S10+, 2026-11-01: DXVK's fence and submission
    # timeline got 2-3 extra batches on each of ~5 submits a frame and the mirror answered none of its waits): asked
    # about only while held -> mirrored for short tries only; asked about once done (Balatro, vkcube) -> stays mirrored
    VKBRIDGE_LOG_LEVEL=2 timeout 120 ./out/syncmirror_check 1000 unmet >out/t-syncmirror-unmet.log 2>&1 && grep -q "^OK" out/t-syncmirror-unmet.log &&
      n=$(grep -oE "sync mirror: 1000 submits, [0-9]+ of them" out/t-syncmirror-unmet.log | grep -oE "[0-9]+ of them" | grep -oE "^[0-9]+") && [ "${n:-9999}" -le 200 ]
    result $? "syncmirror_check: fences and semaphores the mirror cannot answer stop getting batches ($(grep -o 'sync mirror: [0-9]* submits, [0-9]* of them' out/t-syncmirror-unmet.log | head -1); v0.5.5: all of them)"
    VKBRIDGE_LOG_LEVEL=2 timeout 120 ./out/syncmirror_check 1000 met >out/t-syncmirror-met.log 2>&1 && grep -q "^OK" out/t-syncmirror-met.log &&
      n=$(grep -oE "sync mirror: 1000 submits, [0-9]+ of them" out/t-syncmirror-met.log | grep -oE "[0-9]+ of them" | grep -oE "^[0-9]+") && [ "${n:-0}" -ge 950 ] &&
      grep -qE "sync mirror: [0-9]+ fence, [0-9]+ event and [0-9]{3,} semaphore answers from shared memory" out/t-syncmirror-met.log
    result $? "syncmirror_check: fences and semaphores waited for once done stay mirrored ($(grep -o 'sync mirror: [0-9]* submits, [0-9]* of them' out/t-syncmirror-met.log | head -1); $(grep -o '[0-9]* fence, [0-9]* event and [0-9]* semaphore answers from shared memory' out/t-syncmirror-met.log | head -1))"
  fi
  # Query results answered from the client's mirror (client/query.c), and the mirror never saying
  # "not ready" forever for a query the GPU has done (Deep Rock Galactic's stall on the S11 Ultra,
  # 2026-10-27): a multiview pass's second query, a lost ready-word update (imitated), and DRG's
  # pattern through DXVK on a tiler (no round trips with the mirror on)
  if want query; then
    timeout 120 ./out/query_check >out/t-query.log 2>&1; result $? "query_check ($(tail -1 out/t-query.log))"
    VKBRIDGE_QUERY_MIRROR=0 timeout 120 ./out/query_check >out/t-query-off.log 2>&1; result $? "query_check, mirror off ($(tail -1 out/t-query-off.log))"
    # occlusion from the server (VKBRIDGE_QUERY_MIRROR_OCCLUSION=0, a per-game fallback), timestamps still mirrored
    VKBRIDGE_QUERY_MIRROR_OCCLUSION=0 timeout 120 ./out/query_check >out/t-query-occ0.log 2>&1
    result $? "query_check, VKBRIDGE_QUERY_MIRROR_OCCLUSION=0 ($(tail -1 out/t-query-occ0.log))"
  fi
  if want querystuck; then
    # stale, batman: a reused slot's new value reaching the host late must be answered late, never as the slot's old value
    for c in multiview lost drg stale batman; do
      timeout 60 ./out/querystuck_check $c >out/t-qstuck-$c.log 2>&1
      result $? "querystuck_check $c ($(grep -m1 -E '^(PASS|FAIL) (multiview|lost|Deep|a reused|UE3)' out/t-qstuck-$c.log | cut -d: -f2- | cut -c2-140))"
    done
  fi
  if want querystuck_burst; then
    # DRG's first frame after the loading bar (640 queries in render-pass secondaries of one primary)
    # forwarded intact and never answered by the default fallback; the Ultra's never-available queries
    # imitated, and VKBRIDGE_QUERY_VISIBLE_MS answering only those (client/qvisible.c)
    for c in burst burst-never burst-visible; do
      timeout 60 ./out/querystuck_check $c >out/t-qstuck-$c.log 2>&1
      result $? "querystuck_check $c ($(grep -m1 -E '^(PASS|FAIL) (DRG|the imitated|VKBRIDGE)' out/t-qstuck-$c.log | cut -d: -f2- | cut -c2-140))"
    done
    # v0.5.5's lite-mode query switches with v0.5.4.1's stuck-query check and the fallback: copies after
    # each render pass (VKBRIDGE_QUERY_EARLY=1), and every not-ready poll asked of the server with local
    # timeline waits (VKBRIDGE_QUERY_NOTREADY_WIRE=1 + VKBRIDGE_SEM_WAIT_LOCAL=1; query_check's round-trip
    # counts are not for that arm, so only the stuck cases run there)
    VKBRIDGE_QUERY_EARLY=1 timeout 120 ./out/query_check >out/t-query-early.log 2>&1; result $? "query_check, VKBRIDGE_QUERY_EARLY=1 ($(tail -1 out/t-query-early.log))"
    for c in multiview lost drg burst; do
      VKBRIDGE_QUERY_EARLY=1 timeout 60 ./out/querystuck_check $c >out/t-qstuck-early-$c.log 2>&1
      result $? "querystuck_check $c, VKBRIDGE_QUERY_EARLY=1 ($(tail -1 out/t-qstuck-early-$c.log))"
    done
    for c in multiview lost burst; do
      VKBRIDGE_QUERY_NOTREADY_WIRE=1 VKBRIDGE_SEM_WAIT_LOCAL=1 timeout 60 ./out/querystuck_check $c >out/t-qstuck-nrw-$c.log 2>&1
      result $? "querystuck_check $c, VKBRIDGE_QUERY_NOTREADY_WIRE=1 VKBRIDGE_SEM_WAIT_LOCAL=1 ($(tail -1 out/t-qstuck-nrw-$c.log))"
    done
  fi
  if want vkcube_wayland_dmabuf; then
    timeout 60 $VKCUBE_WAYLAND --c 120 >out/t-vkcube.log 2>&1; r=$?; result $r "vkcube-wayland (dma-buf)$([ $r = 0 ] || echo " exit $r: $(tail -2 out/t-vkcube.log | tr '\n' '|')")"
  fi
  if want vkcube_wayland_shm; then
    VKBRIDGE_WSI_SHM=1 timeout 60 $VKCUBE_WAYLAND --c 120 >>out/t-vkcube.log 2>&1; result $? "vkcube-wayland (wl_shm)"
  fi
  if want vkcube_x11; then
    timeout 60 vkcube --c 120 >>out/t-vkcube.log 2>&1; result $? "vkcube (X11)"
  fi
  if want gamescope && docker image inspect vkb-arch-gamescope >/dev/null 2>&1; then
    # The FPS comes from gamescope's headless backend at a fixed 1000 Hz: a nested window runs at the
    # refresh of the monitor under the pointer (60 or 144 Hz on the same desk), so its FPS said more
    # about the desk than the build. The last of glxgears' three 5 s readings (the first holds startup).
    VKB_GS_HEADLESS=1 VKB_DOCKER_EXTRA="--device /dev/dri/renderD128" VKBRIDGE_DRM_RENDER=226:128 timeout 120 ./gamescope_test.sh \
      "bash -c 'export MESA_LOADER_DRIVER_OVERRIDE=zink GALLIUM_DRIVER=zink LIBGL_KOPPER_DRI2=true; timeout 16 glxgears 2>&1 | grep frames'" \
      >out/t-gamescope-$mode.log 2>&1
    grep -q "frames in" out/t-gamescope-$mode.log
    result $? "gamescope + Zink glxgears, headless 1000 Hz ($(grep -o '[0-9.]* FPS' out/t-gamescope-$mode.log | tail -1))"
    # The nested window on the desktop, a smoke test only (VKB_GS_VISIBLE=1): it passes on its first
    # frames, and its FPS is the monitor's
    if [ "${VKB_GS_VISIBLE:-0}" = 1 ]; then
      VKB_DOCKER_EXTRA="--device /dev/dri/renderD128" VKBRIDGE_DRM_RENDER=226:128 timeout 120 ./gamescope_test.sh \
        "bash -c 'export MESA_LOADER_DRIVER_OVERRIDE=zink GALLIUM_DRIVER=zink LIBGL_KOPPER_DRI2=true; timeout 20 glxgears 2>&1 | grep -m1 frames'" \
        >out/t-gamescope-window-$mode.log 2>&1
      grep -q "frames in" out/t-gamescope-window-$mode.log
      result $? "gamescope + Zink glxgears, nested window (smoke; $(grep -m1 -o '[0-9.]* FPS' out/t-gamescope-window-$mode.log) is the monitor's refresh)"
    fi
  fi
}
# Drivers with VK_KHR_vertex_attribute_divisor only (vkb_ext_alias.h): Mali-G925 on Android 16 also
# lacks the zero divisor (emulated whole); a complete KHR-only driver gets the EXT name as an alias
divisor_variants() {
  echo "== KHR-only divisor drivers"
  local l
  if want divisor_g925; then
    l=$(wc -l < "$VKB_SERVER_LOG")
    VKB_MISSING=divisorzero,VK_EXT_vertex_attribute_divisor . ./bridge_env.sh >/dev/null
    VKBRIDGE_HIDE_EXTS=VK_EXT_vertex_attribute_divisor timeout 60 ./out/divisor_check >out/t-divisor-g925.log 2>&1 &&
      VKBRIDGE_HIDE_EXTS=VK_EXT_vertex_attribute_divisor timeout 60 ./out/divisor_check --core14 >>out/t-divisor-g925.log 2>&1 &&
      tail -n +$((l+1)) "$VKB_SERVER_LOG" | grep -q "lacks vertexAttributeInstanceRateZeroDivisor; emulated instead" &&
      tail -n +$((l+1)) "$VKB_SERVER_LOG" | grep -q "VK_EXT_vertex_attribute_divisor is enabled as VK_KHR_vertex_attribute_divisor"
    result $? "divisor_check, Mali-G925's KHR divisor without the zero divisor: emulated ($(grep -c '^PASS' out/t-divisor-g925.log) passed)"
  fi
  if want divisor_khr; then
    l=$(wc -l < "$VKB_SERVER_LOG")
    VKB_MISSING=VK_EXT_vertex_attribute_divisor . ./bridge_env.sh >/dev/null
    VKBRIDGE_HIDE_EXTS=VK_EXT_vertex_attribute_divisor timeout 60 ./out/divisor_check >out/t-divisor-khr.log 2>&1 &&
      tail -n +$((l+1)) "$VKB_SERVER_LOG" | grep -q "VK_EXT_vertex_attribute_divisor is enabled as VK_KHR_vertex_attribute_divisor" &&
      ! tail -n +$((l+1)) "$VKB_SERVER_LOG" | grep -q "emulation 0x[0-9a-f]* installed"
    result $? "divisor_check, complete KHR-only divisor: EXT name aliased, native ($(tail -1 out/t-divisor-khr.log))"
  fi
}
# The request arena and the null-descriptor set map on their own (no driver): checks, and the
# times against the code before 2026-10-21 (hotpath_bench_old), shown, not judged
if want hotpath; then
  if [ -x out/hotpath_bench ]; then
    timeout 120 ./out/hotpath_bench >out/t-hotpath.log 2>&1
    r=$?
    [ -x out/hotpath_bench_old ] && timeout 120 ./out/hotpath_bench_old >out/t-hotpath-old.log 2>&1
    result $r "hotpath_bench ($(grep -m1 -o '[0-9.]* us per request' out/t-hotpath.log | cut -d' ' -f1-2) per ~600 KiB request, was $(grep -m1 -o '[0-9.]* us per request' out/t-hotpath-old.log 2>/dev/null | cut -d' ' -f1-2); 1M sets: $(grep '1000000 sets' out/t-hotpath.log | grep -o '[0-9.]* ns per lookup'), was $(grep '1000000 sets' out/t-hotpath-old.log 2>/dev/null | grep -o '[0-9.]* ns per lookup'))"
  fi
fi
run_mode plain
if want_any divisor_g925 divisor_khr; then divisor_variants; fi
server_errors; result $? "no protocol errors in the server log (plain)"
run_mode mali
server_errors; result $? "no protocol errors in the server log (mali)"
# wg_init: compute shaders' shared memory zeroed by the shader instead of through its initializers
# (a server of its own with the pass forced on: it is for Arm GPUs); every value reads as zero
: > "$VKB_SERVER_LOG"
vkb_server_start VKBRIDGE_WG_INIT=1
timeout 60 ./out/wginit_check >out/t-wginit.log 2>&1 && { grep -q "^SKIP" out/t-wginit.log || grep -q "wg_init: workgroup memory zeroed" "$VKB_SERVER_LOG"; }
result $? "wginit_check, wg_init forced on ($(tail -1 out/t-wginit.log))"
echo "== $pass passed, $fail failed"
[ "$fail" = 0 ]
