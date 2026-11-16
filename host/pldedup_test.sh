#!/bin/bash
# Pipeline layouts made the same way share one driver layout on PowerVR (Pixel 10 Pro XL, DXT 25.1,
# 2026-11-11: the driver keeps 40 bytes a stage for every uniform/storage buffer descriptor of a pipeline layout's
# sets, 305 MB for vkd3d-proton's six 1,000,000 sets; RE3's root-signature layouts took the server's heap 207 MB ->
# 3.2 GB in one second of vkCreatePipelineLayout). tests/pldedup_check makes two layouts from the same create info,
# one with another push range, uses the second after the first is destroyed, makes the first again and ends its
# device with it alive. One server per step:
#   1  PowerVR identity (VKBRIDGE_FAKE_DRIVER=img): the two get one handle, the other range its own, the pipeline
#      made after one destroy is VK_SUCCESS; two layouts over two sampler set layouts made alike get one handle and
#      work after the first set layout is destroyed; over vkd3d-like costly sets (2026-11-12, a player log: RE3's 29 driver
#      layouts differed only in their push range) the three graphics layouts get one handle, the two compute ones
#      another, and a dispatch through them reads 222111 (pushes on the wide layouts reach the shader); the server counts
#      11 asked, 6 made, 5 shared, 2 made with one wide push range, 3 shared over one
#   2  no PowerVR identity (lavapipe as itself): two handles, nothing shared, nothing said (Mali's path unchanged)
#   3  PowerVR identity, VKBRIDGE_PL_SHARE=0: two handles, nothing said (the switch)
#   4  PowerVR identity, VKBRIDGE_PL_WIDE=0: shared only when alike (D graphics different), dispatch still right
# Fails on 829cd70 (step 1: "A different", no line); on f3342b9 (step 1: "D graphics different").
# Prints one line per step and "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
work=$(mktemp -d)
sock=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-pldedup-$$.sock
srv=
stop() { [ -n "$srv" ] && kill -9 $srv 2>/dev/null && wait $srv 2>/dev/null; srv=; }
trap 'stop; rm -f "$sock"; [ -n "$KEEP" ] && echo "kept $work" || rm -rf "$work"' EXIT
trap 'exit 143' TERM INT HUP
start() { # start <name> [env...]
  local d=$work/$1
  shift
  stop
  rm -f "$sock"
  mkdir -p "$d/c"
  log=$d/server.log
  env -u VKBRIDGE_FAKE_MISSING -u VKBRIDGE_FAKE_DRIVER -u VKBRIDGE_PL_SHARE -u VKBRIDGE_PL_WIDE -u VKBRIDGE_HIDE_EXTS -u VKBRIDGE_MODE "$@" \
      VK_DRIVER_FILES=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/lvp_icd.json} VK_ICD_FILENAMES= \
      "$out/vkbridge-server" --socket "$sock" --log "$log" --cache-dir "$d/c" >/dev/null 2>&1 &
  srv=$!
  for _ in $(seq 100); do [ -S "$sock" ] && break; kill -0 $srv 2>/dev/null || break; sleep 0.1; done
  cl=$d/client.out
}
check() { timeout 60 "$out/pldedup_check" >$cl 2>&1; rc=$?; sleep 0.3; }
export VKBRIDGE_SOCKET=$sock VK_DRIVER_FILES=$out/vkbridge_icd.json VK_ICD_FILENAMES=
unset VKBRIDGE_HIDE_EXTS VKBRIDGE_MAX_API VKBRIDGE_SYNC
fails=0 steps=0
step() { steps=$((steps+1)); if [ "$1" = 0 ]; then echo "  ok   $2"; else echo "  FAIL $2"; fails=$((fails+1)); fi; }
said() { grep -c "made like one alive\|share one driver layout" "$log"; }
wides() { grep -c "wide push range" "$log"; }
dread() { grep -m1 '^D dispatch' $cl; }

# 1
start pvr VKBRIDGE_FAKE_DRIVER=img
check
sum=$(grep -m1 -o 'pipeline layouts: [0-9]* asked, [0-9]* made by the driver, [0-9]* shared' "$log")
wsum=$(grep -m1 -o '[0-9]* made with one wide push range, [0-9]* shared over one' "$log")
grep -q "^A same" $cl && grep -q "^B differs" $cl && grep -q "^pipeline after one destroy: 0" $cl && grep -q "^C same" $cl &&
  grep -q "^pipeline after its set layout went: 0" $cl && grep -q "^D graphics same" $cl && grep -q "^D compute same" $cl &&
  grep -q "^D kinds differ" $cl && grep -q "^D dispatch read 222111" $cl && grep -q "^OK" $cl &&
  grep -q "made like one alive" "$log" && [ "$sum" = "pipeline layouts: 11 asked, 6 made by the driver, 5 shared" ] &&
  [ "$wsum" = "2 made with one wide push range, 3 shared over one" ] && ! grep -q " E " "$log"
step $? "PowerVR identity: rc $rc, $(grep -m1 '^A ' $cl), $(grep -m1 '^B ' $cl), $(grep -m1 '^pipeline' $cl), $(grep -m1 '^C ' $cl), $(grep -m1 'set layout went' $cl), $(grep -m1 '^D graphics' $cl), $(grep -m1 '^D compute' $cl), $(grep -m1 '^D kinds' $cl), $(dread), '$sum', '$wsum', errors $(grep -c ' E ' "$log")"

# 2
start mali
check
grep -q "^A different" $cl && grep -q "^C different" $cl && grep -q "^D graphics different" $cl && grep -q "^D dispatch read 222111" $cl &&
  grep -q "^OK" $cl && [ "$(said)" = 0 ] && [ "$(wides)" = 0 ]
step $? "no PowerVR identity: rc $rc, $(grep -m1 '^A ' $cl), $(grep -m1 '^pipeline' $cl), $(grep -m1 '^D graphics' $cl), $(dread), sharing lines $(said), wide lines $(wides)"

# 3
start off VKBRIDGE_FAKE_DRIVER=img VKBRIDGE_PL_SHARE=0
check
grep -q "^A different" $cl && grep -q "^D graphics different" $cl && grep -q "^D dispatch read 222111" $cl && grep -q "^OK" $cl &&
  [ "$(said)" = 0 ]
step $? "PowerVR identity, VKBRIDGE_PL_SHARE=0: rc $rc, $(grep -m1 '^A ' $cl), $(grep -m1 '^D graphics' $cl), $(dread), sharing lines $(said)"

# 4
start nowide VKBRIDGE_FAKE_DRIVER=img VKBRIDGE_PL_WIDE=0
check
grep -q "^A same" $cl && grep -q "^D graphics different" $cl && grep -q "^D dispatch read 222111" $cl && grep -q "^OK" $cl && [ "$(wides)" = 0 ]
step $? "PowerVR identity, VKBRIDGE_PL_WIDE=0: rc $rc, $(grep -m1 '^A ' $cl), $(grep -m1 '^D graphics' $cl), $(dread), wide lines $(wides)"
stop
if [ $fails = 0 ]; then echo "OK: $steps steps"; else echo "FAILED: $fails of $steps steps"; fi
[ $fails = 0 ]
