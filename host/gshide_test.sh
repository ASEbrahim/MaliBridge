#!/bin/bash
# gs_hide (issue #11: server_errata_gs.c) on the desktop driver, with a server of its own per step
# (socket, cache directory, session folder). The Galaxy S10+'s r38p1 beta driver is imitated:
# VKBRIDGE_FAKE_DRIVER_VERSION=v1.r38p1-01bet0 puts the desktop driver in the erratum's range,
# VKBRIDGE_TEST_CRASH_PIPELINE=gs faults inside every pipeline creation with a geometry stage (the
# crash containment catches it, as on the phone). Steps:
#   1  vulkan level (gshide_check, tests/gshide_check.c; its process name is what is matched):
#      a  the beta driver, the process not listed: geometry shaders offered as the driver does
#      b  the beta driver, VKBRIDGE_GS_HIDE_PROCS=gshide_check: geometryShader, geometryStreams,
#         the geometry limits and the subgroup geometry stage hidden, tessellation kept; a device
#         that enables geometryShader anyway has it taken off and is made
#      c  the same list, the released r38p1 ("v1.r38p1-01eac0", the Galaxy G57's): offered
#      d  the same list on the beta driver with VKBRIDGE_ERRATA=-gs_hide: offered
#      e  the same list, VKBRIDGE_ERRATA=+gs_hide on the desktop driver (no fake version): hidden
#      f  "*" from <cache dir>/env: hidden
#   2  Arch's gamescope in docker (vkb-arch-gamescope; skipped if absent), its Xwayland's glamor on
#      Zink through the bridge, in an OpenGL 3.0 context (MESA_GL_VERSION_OVERRIDE=3.0: no core
#      profile, so glamor draws GL_QUADS, and Zink makes geometry shaders for them, as on the
#      phone) with Mali's gaps imitated; X11 drawing (x11perf, from the host) inside:
#      a  -gs_hide: Xwayland's pipelines crash (contained), Xwayland segfaults, gamescope aborts
#         with "X11 I/O error" - the phone's chain; the dumps are named per process and copied
#         into the session folder
#      b  the default (Xwayland listed): no crash in Xwayland, every x11perf test drawn, Xwayland
#         alive; a program not listed (glxgears) keeps geometry shaders (its crashes contained)
# Prints one line per step and "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
here=$PWD
out=${VKB_OUT:-$PWD/out}
work=$(mktemp -d)
sock=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-gshide-$$.sock
icd=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json}
srv=
log=
dir=
stop() { [ -n "$srv" ] && kill $srv 2>/dev/null && wait $srv 2>/dev/null; srv=; }
start() { # start <name> [env...]: a server of its own, cache $work/<name>/c, log $log; $CENV: <cache dir>/env
  dir=$work/$1
  shift
  stop
  rm -f "$sock"
  mkdir -p "$dir/c" "$dir/s"
  [ -n "$CENV" ] && echo "$CENV" > "$dir/c/env"
  log=$dir/s/vkbridge-server.log
  : > "$log"
  env -u VKBRIDGE_FAKE_MISSING -u VKBRIDGE_HIDE_EXTS -u VKBRIDGE_ERRATA -u VKBRIDGE_ERRATA_IMITATE -u VKBRIDGE_GS_HIDE_PROCS \
      -u VKBRIDGE_MODE -u VKBRIDGE_FAKE_DRIVER_VERSION -u VKBRIDGE_ERRATA_PROBE -u VKBRIDGE_PROBE_FAKE -u VKBRIDGE_TEST_CRASH_PIPELINE \
      VKBRIDGE_PROBE_BUDGET_S=0 "$@" VK_DRIVER_FILES=$icd VK_ICD_FILENAMES= \
      "$out/vkbridge-server" --socket "$sock" --log "$log" --cache-dir "$dir/c" >/dev/null 2>&1 &
  srv=$!
  for _ in $(seq 300); do [ -S "$sock" ] && break; kill -0 $srv 2>/dev/null || break; sleep 0.05; done
}
check() { # gshide_check through the bridge; $got: its output, $new: the log lines it added
  local l; l=$(wc -l < "$log")
  got=$(timeout 60 "$out/gshide_check" 2>&1)
  new=$(tail -n +$((l+1)) "$log")
}
f() { echo "$got" | grep -o "$1=[0-9]*" | head -1 | cut -d= -f2; }
offered() { [ "$(echo "$got" | grep -E '^(features|limits|stages):')" = "$(echo "$native" | grep -E '^(features|limits|stages):')" ] && echo "$got" | grep -q "^device: .*: 0$" &&
  ! echo "$new" | grep -q "gs_hide: .*taken off"; }
hidden() {
  for k in v10.geometryShader f2.geometryShader geometryStreams v10.maxGeometryOutputVertices p2.maxGeometryOutputVertices \
           maxGeometryShaderInvocations subgroupGeometry; do [ "$(f $k)" = 0 ] || return 1; done
  [ "$(f tessellationShader)" = "$(echo "$native" | grep -o 'tessellationShader=[0-9]*' | cut -d= -f2)" ] &&
    [ "$(f subgroupVertex)" = 1 ] && echo "$got" | grep -q "^device: .*: 0$" &&
    echo "$new" | grep -q "errata: gs_hide: gshide_check enabled geometryShader anyway; taken off its device"
}
trap 'stop; rm -f "$sock"; [ -n "$KEEP" ] && echo "kept $work" || rm -rf "$work"' EXIT
export VKBRIDGE_SOCKET=$sock VK_ICD_FILENAMES=
unset VKBRIDGE_HIDE_EXTS VKBRIDGE_MAX_API VKBRIDGE_FAKE_MISSING SteamAppId
fails=0 steps=0
step() { steps=$((steps+1)); if [ "$1" = 0 ]; then echo "  ok   $2"; else echo "  FAIL $2"; fails=$((fails+1)); fi; }
summary() { echo "$got" | grep -o 'f2.geometryShader=[0-9]*\|p2.maxGeometryOutputVertices=[0-9]*\|subgroupGeometry=[0-9]*' | tr '\n' ' '; }

native=$(VK_DRIVER_FILES=$icd timeout 60 "$out/gshide_check" 2>&1)
case "$native" in *f2.geometryShader=1*) ;; *) echo "OK: 0 steps (SKIP: the desktop driver has no geometryShader: $native)"; exit 0 ;; esac
export VK_DRIVER_FILES=$out/vkbridge_icd.json
BETA=VKBRIDGE_FAKE_DRIVER_VERSION=v1.r38p1-01bet0

# 1
start a $BETA
check
offered && echo "$new" | grep -q "errata: gs_hide: gshide_check keeps geometry shaders (not in VKBRIDGE_GS_HIDE_PROCS=Xwayland,steam,steamwebhelper)" &&
  grep -q "errata: gs_hide ON (r38p1 in r38p1-r38p1 (a beta build" "$log"
step $? "r38p1 beta, a process not listed (a game): offered as the driver does ($(summary))"
start b $BETA VKBRIDGE_GS_HIDE_PROCS=gshide_check
check
hidden && echo "$new" | grep -q "errata: gs_hide: geometry shaders hidden from gshide_check (r38p1 in r38p1-r38p1"
step $? "r38p1 beta, the process listed: hidden ($(summary)tessellation kept), enabled anyway: taken off, the device made"
start c VKBRIDGE_FAKE_DRIVER_VERSION=v1.r38p1-01eac0 VKBRIDGE_GS_HIDE_PROCS=gshide_check
check
offered && ! grep -q "gs_hide ON" "$log"
step $? "the released r38p1 (01eac0), listed: offered ($(summary))"
start d $BETA VKBRIDGE_GS_HIDE_PROCS=gshide_check VKBRIDGE_ERRATA=-gs_hide
check
offered && grep -q "errata: gs_hide OFF (VKBRIDGE_ERRATA=-gs_hide; it would be on: r38p1" "$log"
step $? "r38p1 beta, listed, VKBRIDGE_ERRATA=-gs_hide: offered ($(summary))"
start e VKBRIDGE_GS_HIDE_PROCS=gshide_check VKBRIDGE_ERRATA=+gs_hide
check
hidden
step $? "the desktop driver, listed, VKBRIDGE_ERRATA=+gs_hide: hidden ($(summary))"
CENV="VKBRIDGE_GS_HIDE_PROCS=*" start f $BETA
check
hidden
step $? "r38p1 beta, VKBRIDGE_GS_HIDE_PROCS=* from <cache dir>/env: hidden ($(summary))"
stop

# 2
if ! docker image inspect vkb-arch-gamescope >/dev/null 2>&1 || [ ! -x /usr/bin/x11perf ] || [ -z "${WAYLAND_DISPLAY:-}" ]; then
  echo "  (skipped: gamescope + Xwayland steps need the vkb-arch-gamescope image, /usr/bin/x11perf and a Wayland desktop)"
else
  cat > "$work/inner.sh" <<'EOF'
sleep 3
timeout 60 x11perf -repeat 1 -time 1 -rect10 -rect100 -trap10 -aatrap10 -copywinwin100 -tilerect100 -seg100 -putimage100 > /tmp/xp.log 2>&1
echo "XPERF rc=$? results=$(grep -cE 'reps @' /tmp/xp.log)"
grep -E "reps @" /tmp/xp.log | sed "s/^/XP: /"
timeout 8 glxgears >/dev/null 2>&1
echo "ALIVE $(x11perf -repeat 1 -time 1 -dot >/dev/null 2>&1 && echo yes || echo no)"
EOF
  R=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
  MALI_MISSING=bc,clip,cull,divisor,maint5,depthclip,push,robustness2,dualsrc,fillmode,multiviewport,maint6,desclimits,opaquefd,lazymem,dmabuflinear,planarext
  MALI_HIDE=VK_KHR_maintenance5,VK_KHR_push_descriptor,VK_EXT_robustness2,VK_KHR_robustness2,VK_EXT_vertex_attribute_divisor,VK_KHR_vertex_attribute_divisor,VK_EXT_depth_clip_enable,VK_KHR_maintenance6
  session() { # session <name> [server env...]: gamescope's output in $dir/gamescope.log
    start "$@" $BETA VKBRIDGE_TEST_CRASH_PIPELINE=gs VKBRIDGE_SPIRV_FIX=1 VKBRIDGE_FAKE_MISSING=$MALI_MISSING vk_require_etc2=true vk_require_astc=true
    chmod 666 "$sock"
    timeout 120 docker run --rm --device /dev/dri/renderD128 --tmpfs /run/xdg:mode=0700 \
      -e XDG_RUNTIME_DIR=/run/xdg -e WAYLAND_DISPLAY=host-wayland -e HOME=/root \
      -v "$R/$WAYLAND_DISPLAY:/run/host-wayland" -v "$sock:/run/vkbridge.sock" -v "$out:$out" \
      -v /usr/bin/x11perf:/usr/bin/x11perf:ro -v "$work/inner.sh:/inner.sh:ro" \
      -e VK_DRIVER_FILES=$out/vkbridge_icd.json -e VKBRIDGE_SOCKET=/run/vkbridge.sock -e VKBRIDGE_DRM_RENDER=226:128 \
      -e VKBRIDGE_HIDE_EXTS=$MALI_HIDE -e VKBRIDGE_MAX_API=1.3 -e VKBRIDGE_LOG_LEVEL=2 \
      -e MESA_LOADER_DRIVER_OVERRIDE=zink -e GALLIUM_DRIVER=zink -e LIBGL_KOPPER_DRI2=true -e MESA_GL_VERSION_OVERRIDE=3.0 \
      vkb-arch-gamescope bash -c "ln -s /run/host-wayland /run/xdg/host-wayland; gamescope --backend wayland --expose-wayland -W 960 -H 540 -w 960 -h 540 -- bash /inner.sh" \
      > "$dir/gamescope.log" 2>&1
    stop
  }
  xw_pid() { grep -m1 -o "process [0-9]* is Xwayland" "$log" | cut -d' ' -f2; }
  session xa VKBRIDGE_ERRATA=-gs_hide
  p=$(xw_pid)
  grep -aq "Caught signal 11" "$dir/gamescope.log" && grep -aq "X11 I/O error" "$dir/gamescope.log" &&
    grep -q "CONTAINED driver crash" "$log" && ls "$dir/c/failed-Xwayland-$p-d"*-gs.spv >/dev/null 2>&1 &&
    ls "$dir/s/failed-spv/failed-Xwayland-$p-d"*-gs.spv >/dev/null 2>&1
  step $? "gamescope, Xwayland's glamor on Zink, -gs_hide: the phone's chain ($(grep -c 'CONTAINED driver crash' "$log") contained crashes, Xwayland segfaults, gamescope aborts); dumps $(ls "$dir/s/failed-spv" 2>/dev/null | head -2 | tr '\n' ' ')in the session folder too"
  session xb
  p=$(xw_pid)
  pg=$(grep -m1 -o "process [0-9]* is glxgears" "$log" | cut -d' ' -f2)
  n=$(grep -aoE "XPERF rc=[0-9]+ results=[0-9]+" "$dir/gamescope.log")
  [ -n "$p" ] && [ "$n" = "XPERF rc=0 results=8" ] && grep -aq "ALIVE yes" "$dir/gamescope.log" && ! grep -aq "Caught signal\|X11 I/O error" "$dir/gamescope.log" &&
    grep -q "errata: gs_hide: geometry shaders hidden from Xwayland" "$log" && ! ls "$dir/c/failed-Xwayland-"* >/dev/null 2>&1 &&
    { [ -z "$pg" ] || ls "$dir/c/failed-glxgears-$pg-"*-gs.spv >/dev/null 2>&1; }
  step $? "the same with gs_hide: Xwayland draws every x11perf test ($n) and lives, no crash in it; glxgears (not listed) keeps geometry shaders ($(ls "$dir/c" | grep -c '^failed-glxgears') dumps from its contained crashes)"
fi

if [ $fails = 0 ]; then echo "OK: $steps steps"; else echo "FAILED: $fails of $steps steps"; exit 1; fi
