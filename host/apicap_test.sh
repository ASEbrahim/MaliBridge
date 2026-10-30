#!/bin/bash
# server_apicap.c's Vulkan version cap for Steam mode's processes per driver version and switch, a server per step.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
work=$(mktemp -d)
cache=$work/c
mkdir -p "$cache" "$work/bin"
sock=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-apicap-$$.sock
log=$work/server.log
srv=
vi=$(command -v vulkaninfo) || { echo "FAILED: no vulkaninfo"; exit 1; }
for n in Xwayland steam steamwebhelper gamescope vulkaninfo; do cp "$vi" "$work/bin/$n"; done
start() {
  [ -n "$srv" ] && kill $srv 2>/dev/null && wait $srv 2>/dev/null
  rm -f "$sock"
  : > "$log"
  env -u VKBRIDGE_FAKE_MISSING -u VKBRIDGE_HIDE_EXTS -u VKBRIDGE_FAKE_API -u VKBRIDGE_PROMOTE_API -u VKBRIDGE_MODE \
      -u VKBRIDGE_STEAM_API_CAP -u VKBRIDGE_STEAM_API_CAP_GAMESCOPE -u VKBRIDGE_FAKE_DRIVER_VERSION -u VKBRIDGE_EXPORT_CHECK -u VKBRIDGE_SERVER_HIDE_EXTS -u VKBRIDGE_A1_ZINK "$@" \
      VK_DRIVER_FILES=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json} VK_ICD_FILENAMES= \
      "$out/vkbridge-server" --socket "$sock" --log "$log" --cache-dir "$cache" >/dev/null 2>&1 &
  srv=$!
  for _ in $(seq 50); do [ -S "$sock" ] && break; sleep 0.1; done
}
trap 'kill $srv 2>/dev/null; rm -f "$sock"; [ -n "$KEEP" ] || rm -rf "$work"' EXIT
export VKBRIDGE_SOCKET=$sock VK_DRIVER_FILES=$out/vkbridge_icd.json VK_ICD_FILENAMES=
unset VKBRIDGE_HIDE_EXTS VKBRIDGE_MAX_API
fails=0 steps=0
step() { steps=$((steps+1)); if [ "$1" = 0 ]; then echo "  ok   $2"; else echo "  FAIL $2"; fails=$((fails+1)); fi; }
# The Vulkan minor version process $1 is shown ("" when vulkaninfo failed)
api() { timeout 60 "$work/bin/$1" --summary 2>/dev/null | grep -m1 -o "apiVersion *= *1\.[0-9]*" | sed 's/.*1\./1./'; }
start VKBRIDGE_FAKE_DRIVER_VERSION=r44p1
drv=$(api vulkaninfo)
[ -n "$drv" ] || { echo "FAILED: vulkaninfo through the bridge printed no apiVersion"; exit 1; }
caps() { grep -c "api cap: " "$log"; }
all() { echo "Xwayland $(api Xwayland) steam $(api steam) steamwebhelper $(api steamwebhelper) gamescope $(api gamescope) game $(api vulkaninfo)"; }

# 0
start VKBRIDGE_FAKE_DRIVER_VERSION=r40p0:jm
r=$(all)
[ "$r" = "Xwayland 1.1 steam 1.1 steamwebhelper 1.1 gamescope 1.2 game $drv" ] &&
  grep -q "api cap: Xwayland, steam and steamwebhelper are shown Vulkan 1.1, gamescope 1.2, games $drv (Arm driver r40, job manager, Vulkan $drv to apps, VK_EXT_extended_dynamic_state offered; VKBRIDGE_STEAM_API_CAP=0 turns it off)" "$log" &&
  [ "$(caps)" = 1 ]
step $? "r40p0 job manager (the RG556), by default: $r; $(caps) api cap line"
# 1
start VKBRIDGE_FAKE_DRIVER_VERSION=r40p0:jm VKBRIDGE_STEAM_API_CAP=0
r=$(all)
[ "$r" = "Xwayland $drv steam $drv steamwebhelper $drv gamescope $drv game $drv" ] && [ "$(caps)" = 0 ]
step $? "... VKBRIDGE_STEAM_API_CAP=0: $r"
# 2
start VKBRIDGE_FAKE_DRIVER_VERSION=r44p1
r=$(all)
[ "$r" = "Xwayland $drv steam $drv steamwebhelper $drv gamescope $drv game $drv" ] && [ "$(caps)" = 0 ]
step $? "r44p1 (CSF): gate off: $r"
# 3
start VKBRIDGE_FAKE_DRIVER_VERSION=r44p1 VKBRIDGE_STEAM_API_CAP=1.1
r=$(all)
[ "$r" = "Xwayland 1.1 steam 1.1 steamwebhelper 1.1 gamescope 1.2 game $drv" ] &&
  grep -q "api cap: Xwayland, steam and steamwebhelper are shown Vulkan 1.1, gamescope 1.2, games $drv (VKBRIDGE_STEAM_API_CAP=1.1; " "$log" &&
  [ "$(caps)" = 1 ]
step $? "... VKBRIDGE_STEAM_API_CAP=1.1: $r"
# 4
start VKBRIDGE_FAKE_DRIVER_VERSION=r40p0:jm VKBRIDGE_STEAM_API_CAP_GAMESCOPE=1.1
r=$(all)
[ "$r" = "Xwayland 1.1 steam 1.1 steamwebhelper 1.1 gamescope 1.1 game $drv" ]
step $? "r40p0 job manager, VKBRIDGE_STEAM_API_CAP_GAMESCOPE=1.1: $r"
# 5
start VKBRIDGE_FAKE_DRIVER_VERSION=r38p0:jm VKBRIDGE_FAKE_API=1.1 VKBRIDGE_SERVER_HIDE_EXTS=VK_EXT_extended_dynamic_state
r=$(all)
case "$r" in "Xwayland 1.1 steam 1.1 steamwebhelper 1.1 gamescope "*" game 1.1") true ;; *) false ;; esac && [ "$(caps)" = 0 ] &&
  ! grep -q "a1: " "$log"
step $? "r38p0 job manager as a Vulkan 1.1 driver without the extension (the S21's case): gate off: $r"
# 5b
start VKBRIDGE_FAKE_DRIVER_VERSION=r38p1-01bet0:jm VKBRIDGE_FAKE_API=1.1
r=$(all)
case "$r" in "Xwayland 1.1 steam 1.1 steamwebhelper 1.1 gamescope "*" game 1.1") true ;; *) false ;; esac &&
  grep -q "a1: .*(Arm driver r38, job manager, Vulkan 1.1 to apps, VK_EXT_extended_dynamic_state offered; " "$log"
step $? "r38p1 beta job manager as a Vulkan 1.1 driver with the extension (#11's S10 / Note10+): gate on: $r"
# 6
start VKBRIDGE_FAKE_DRIVER_VERSION=r40p0:jm VKBRIDGE_STEAM_API_CAP=1.x9
r=$(all)
[ "$r" = "Xwayland $drv steam $drv steamwebhelper $drv gamescope $drv game $drv" ] &&
  grep -q "api cap: VKBRIDGE_STEAM_API_CAP=1.x9 is not 1.1, 1.2, 1.3, 0 or off; no cap" "$log"
step $? "VKBRIDGE_STEAM_API_CAP=1.x9: warned, $r"
# 7
start VKBRIDGE_FAKE_DRIVER_VERSION=r38p1:jm
r=$(all)
[ "$r" = "Xwayland 1.1 steam 1.1 steamwebhelper 1.1 gamescope 1.2 game $drv" ] &&
  grep -q "api cap: .*(Arm driver r38, job manager, Vulkan $drv to apps, VK_EXT_extended_dynamic_state offered; " "$log" && [ "$(caps)" = 1 ]
step $? "r38p1 job manager at the driver's version (A16 / Tab S9 FE / F15 / A54), by default: $r"
# 8
for v in r54p1 r49p1:csf r54p1:jm; do
  start VKBRIDGE_FAKE_DRIVER_VERSION=$v
  r=$(all)
  [ "$r" = "Xwayland $drv steam $drv steamwebhelper $drv gamescope $drv game $drv" ] && [ "$(caps)" = 0 ]
  step $? "$v: nothing capped: $r"
done

if [ $fails = 0 ]; then echo "OK: $steps steps"; else echo "FAILED: $fails of $steps steps"; exit 1; fi
