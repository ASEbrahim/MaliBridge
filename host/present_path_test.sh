#!/bin/bash
# The present path of a game under nested gamescope (client/wsi.c, client/wsi_wayland.c): the game's
# thread, the swapchain's presenting thread and the compositor. A fake gamescope and a fake app compositor
# (tests/fake_nested_wayland.py) take the frames; the server's gralloc stand-in (VKBRIDGE_TEST_SCANOUT=memfd)
# gives the swapchain dma-buf images even on a driver that exports none (lavapipe without /dev/udmabuf), and
# lets the fake read each frame's pixels. tests/present_path_check, one server per step:
#   v0.5.5.4: the release thread and the async acquire are OFF by default in this build; steps 1, 3 and 13 ask for them by name
#   (VKBRIDGE_PRESENT_RELEASE_THREAD=1 / VKBRIDGE_ASYNC_ACQUIRE=1) and step 2b checks that nothing set takes the old path.
#   1  release thread: frame 2's GPU work held 600 ms, the compositor releases image 0 ~50 ms after frame 1's
#      commit: the 4th acquire returns once the release came, not once frame 2 rendered
#      (VKBRIDGE_PRESENT_RELEASE_THREAD; before it the presenting thread took releases only between frames);
#   2  VKBRIDGE_PRESENT_RELEASE_THREAD=0: the acquire waits for frame 2, as before (the switch works both ways);
#   3  async acquire: the server holds every vkQueueSubmit 3 ms; the acquire's cost on the game's thread stays
#      under 1 ms a frame (VKBRIDGE_ASYNC_ACQUIRE: no round trip waiting for the server's queue);
#   4  VKBRIDGE_ASYNC_ACQUIRE=0: the acquire waits for the server's queue, as before;
#   5  order and pixels (gamescope's FIFO every 4 ms): every frame shown once, in the order presented, with the
#      number the app drew; no image changed while the compositor held it; no acquire gave an image the app held;
#   6  the same with the acquire signalling a fence (polled, then waited) instead of a semaphore;
#   7  the same with both switches off (the old path);
#   8  recreate: a swapchain made from the old one while releases are pending: the old one answers out of date,
#      is destroyed, the new one presents;
#   9  destroy right after presents, frame 2 held 300 ms and the releases 300 ms late: no hang;
#  10  the compositor drops the connection after 20 commits: an error, no hang, destroy returns;
#  11  the device lost at the 30th submission: an error, no hang, destroy returns;
#  12  idle: a live swapchain with nothing presented uses (almost) no CPU.
# Under the common Mali profile (host/mali_profile.sh: what every Mali has, Vulkan 1.1 as r18-r40 drivers show games:
# no timeline semaphore), present_path_check --v11 (frame 2 held by the server's replay instead of a timeline wait):
#  13  the release thread and the async acquire work on 1.1: as 1, and each decision said once at info;
#  14  both switches 0 there: the old wait, as 2;
#  15  order and pixels; 16 with acquire fences; 17 with the driver refusing the signalled sync-fd import
#      (VKBRIDGE_FAKE_MISSING=syncfdimport): the server's empty-submit fallback (the client's old one) signals
#      them, said once at info (17f: the fences); 18 both switches 0 (the old path); 19 recreate.
# PACE=1: the micro-test after the steps (numbers, not pass/fail): present_path_check lat for each
# "vblank_ms:cpu_ms:cpu2_ms:gpu_ms" in PACE_POINTS (CPU work before and after the acquire), both switches off and on: the game thread's ms per frame in
# vkAcquireNextImageKHR and vkQueuePresentKHR, and the frames per second the fake gamescope showed.
# VKB_CLIENT_ICD=<icd json> runs it against another client build (the steps that need the switches fail
# on one without them: 1 and 3). Prints one line per step and "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
icd=${VKB_CLIENT_ICD:-$out/vkbridge_icd.json}
work=$(mktemp -d "${XDG_RUNTIME_DIR:-/tmp}/vkb-pp.XXXX") # short: socket paths
sock=$work/s.sock
srv=
fake=
fails=0
stop() { [ -n "$srv" ] && kill $srv 2>/dev/null && wait $srv 2>/dev/null; srv=; }
stop_fake() { [ -n "$fake" ] && kill $fake 2>/dev/null && wait $fake 2>/dev/null; fake=; }
trap 'stop; stop_fake; [ -n "${VKB_KEEP:-}" ] && echo "kept $work" || rm -rf "$work"' EXIT
start() { # start <name> [env...]: a server of its own
  stop
  local name=$1; shift
  rm -f "$sock"
  mkdir -p "$work/c"
  env -u VKBRIDGE_FAKE_MISSING -u VKBRIDGE_HIDE_EXTS -u VKBRIDGE_ERRATA -u VKBRIDGE_ERRATA_IMITATE -u VKBRIDGE_MODE \
      -u VKBRIDGE_FRAME_QUEUE -u VKBRIDGE_LATLOG -u VKBRIDGE_HITCH_MS -u VKBRIDGE_TEST_DELAY_CMD -u VKBRIDGE_TEST_DELAY_MS \
      -u VKBRIDGE_TEST_DELAY_REPEAT -u VKBRIDGE_TEST_LOST -u VKBRIDGE_TEST_LOST_COMM -u VKBRIDGE_PRESENT_RELEASE_THREAD \
      -u VKBRIDGE_ASYNC_ACQUIRE VKBRIDGE_PROBE_BUDGET_S=0 VKBRIDGE_TEST_SCANOUT=memfd "$@" \
      VK_DRIVER_FILES=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json} VK_ICD_FILENAMES= \
      "$out/vkbridge-server" --socket "$sock" --log "$work/$name.server.log" --cache-dir "$work/c" >/dev/null 2>&1 &
  srv=$!
  for _ in $(seq 300); do [ -S "$sock" ] && break; kill -0 $srv 2>/dev/null || break; sleep 0.05; done
}
fake_start() { # fake_start <log> [fake args...]
  stop_fake
  local log=$1; shift
  : > "$log"
  rm -f "$work/nested.sock" "$work/host.sock" "$work/fake.out" # the last fake's "ready" is not this one's
  python3 -I ../tests/fake_nested_wayland.py --nested "$work/nested.sock" --host "$work/host.sock" --log "$log" "$@" \
    > "$work/fake.out" 2>&1 &
  fake=$!
  for _ in $(seq 400); do grep -q ready "$work/fake.out" 2>/dev/null && break; kill -0 $fake 2>/dev/null || break; sleep 0.05; done
  grep -q ready "$work/fake.out" 2>/dev/null || { echo "FAIL the fake compositor did not start: $(tail -2 "$work/fake.out")"; fails=$((fails+1)); }
}
run() { # run <name> [env...] -- <present_path_check args>: $res its status, $clog the client log
  local name=$1; shift
  local e=()
  while [ "$1" != -- ]; do e+=("$1"); shift; done
  shift
  clog=$work/$name.client.log
  : > "$clog"
  timeout 120 env -u VKBRIDGE_WSI_AHB -u VKBRIDGE_WSI_SHM -u VKBRIDGE_PRESENT_RELEASE_THREAD -u VKBRIDGE_ASYNC_ACQUIRE \
    -u VKBRIDGE_FRAME_QUEUE -u VKBRIDGE_LATLOG -u VKBRIDGE_PRESENT_TIMEOUT_S -u DISPLAY "${e[@]}" \
    WAYLAND_DISPLAY="$work/nested.sock" VKBRIDGE_HOST_WAYLAND="$work/host.sock" VKBRIDGE_SOCKET="$sock" \
    VK_DRIVER_FILES="$icd" VK_ICD_FILENAMES= VKBRIDGE_CLIENT_LOG="$clog" VKBRIDGE_LOG_LEVEL=2 \
    "$out/present_path_check" "$@" >"$work/$name.out" 2>&1
  res=$?
  sleep 0.2 # the fake's last lines
}
step() { if [ "$1" = 0 ]; then echo "ok   $2"; else echo "FAIL $2"; fails=$((fails+1)); fi; }
last() { grep -E '^(OK|FAILED|SKIPPED)' "$work/$1.out" | tail -1; }
show() { sed 's/^/     /' "$work/$1.out" | tail -8; grep -E 'error|warn' "$work/$1.client.log" 2>/dev/null | head -5 | sed 's/^/     client: /'
  grep -v '^ready$' "$work/fake.out" 2>/dev/null | tail -3 | sed 's/^/     fake: /'; }
acq() { grep -o 'game thread per frame: acquire [0-9.]* ms' "$work/$1.out" | awk '{ print $(NF-1) }'; }
gralloc() { grep -q 'presented as gralloc dma-bufs' "$work/$1.client.log"; }
# The fake's log: every frame 0..n-1 shown once in order with its pixels; every release shows what was latched.
pixels() { # pixels <log> <frames>: prints a summary; status 0 when all holds
  python3 -I - "$1" "$2" <<'PY'
import json, sys
ev = [json.loads(l) for l in open(sys.argv[1]) if l.strip()]
n = int(sys.argv[2])
latch = [e for e in ev if e.get("ev") == "latch" and e.get("who") == "nested"]
rel = [e for e in ev if e.get("ev") == "release" and e.get("who") == "nested"]
bad = []
if any(e.get("px") is None for e in latch):
    print("pixels not readable (no memfd stand-in)"); sys.exit(1)
def frame(px):
    return ((px >> 16) & 0xff) | (((px >> 8) & 0xff) << 8) if (px & 0xff) == 0x5a else -1
shown = [frame(e["px"]) for e in latch]
if shown != list(range(n)):
    bad.append("frames shown %s... (expected 0..%d once each, in order)" % (shown[:12], n - 1))
on = {}
torn = 0
for e in ev:
    if e.get("who") != "nested":
        continue
    if e.get("ev") == "latch":
        on[e["buf"]] = e["px"]
    elif e.get("ev") == "release":
        if on.get(e["buf"]) is not None and e["px"] != on[e["buf"]]:
            torn += 1
if torn:
    bad.append("%d image(s) changed while the compositor held them" % torn)
print("%d frames shown in order, %d releases, pixels as drawn" % (len(shown), len(rel)) if not bad else "; ".join(bad))
sys.exit(1 if bad else 0)
PY
}

if ! command -v python3 >/dev/null; then echo "SKIPPED: no python3"; exit 0; fi

# 1 release thread
start block
fake_start "$work/f1.jsonl" --release-delay-ms 50
run block VKBRIDGE_PRESENT_RELEASE_THREAD=1 VKBRIDGE_ASYNC_ACQUIRE=1 -- block --hold-ms 600
if [ $res = 3 ] && grep -q ready "$work/fake.out"; then echo "SKIPPED: $(last block)"; exit 0; fi
[ $res = 0 ] && gralloc block
s=$?; step $s "1 a release while the presenting thread waits for a frame's GPU work: $(last block)"; [ $s = 0 ] || show block

# 2 its switch off
start blockoff
fake_start "$work/f2.jsonl" --release-delay-ms 50
run blockoff VKBRIDGE_PRESENT_RELEASE_THREAD=0 -- block --hold-ms 600
[ $res = 1 ] && grep -q 'FAILED: the 4th acquire waited' "$work/blockoff.out"
s=$?; step $s "2 VKBRIDGE_PRESENT_RELEASE_THREAD=0: the acquire waits for frame 2's GPU work, as before ($(last blockoff))"; [ $s = 0 ] || show blockoff

# 2b v0.5.5.4: nothing set = the old path (the release thread and the async acquire are off by default in this build)
start blockdflt
fake_start "$work/f2b.jsonl" --release-delay-ms 50
run blockdflt -- block --hold-ms 600
[ $res = 1 ] && grep -q 'FAILED: the 4th acquire waited' "$work/blockdflt.out"
s=$?; step $s "2b nothing set (default off in v0.5.5.4): the acquire waits for frame 2's GPU work, as before ($(last blockdflt))"; [ $s = 0 ] || show blockdflt

# 3 async acquire, a slow server
start import VKBRIDGE_TEST_DELAY_CMD=vkQueueSubmit VKBRIDGE_TEST_DELAY_MS=3 VKBRIDGE_TEST_DELAY_REPEAT=1
fake_start "$work/f3.jsonl"
run import VKBRIDGE_ASYNC_ACQUIRE=1 VKBRIDGE_PRESENT_RELEASE_THREAD=1 -- import --max-acquire-ms 1.0
[ $res = 0 ]
s=$?; step $s "3 every server submit held 3 ms: the acquire on the game's thread: $(last import)"; [ $s = 0 ] || show import

# 4 its switch off
start importoff VKBRIDGE_TEST_DELAY_CMD=vkQueueSubmit VKBRIDGE_TEST_DELAY_MS=3 VKBRIDGE_TEST_DELAY_REPEAT=1
fake_start "$work/f4.jsonl"
run importoff VKBRIDGE_ASYNC_ACQUIRE=0 -- import --max-acquire-ms 1.0
[ $res = 1 ] && grep -q 'FAILED: the acquire took' "$work/importoff.out"
s=$?; step $s "4 VKBRIDGE_ASYNC_ACQUIRE=0: the acquire waits for the server's queue, as before ($(acq importoff) ms a frame)"; [ $s = 0 ] || show importoff

# 5-7 order and pixels
for v in order:: orderfence:--acq-fence: orderoff::VKBRIDGE_PRESENT_RELEASE_THREAD=0,VKBRIDGE_ASYNC_ACQUIRE=0; do
  name=${v%%:*}; rest=${v#*:}; arg=${rest%%:*}; envs=${rest#*:}
  start $name
  fake_start "$work/$name.jsonl" --vblank-ms 4 --release-ms 1 --pixels
  run $name ${envs//,/ } -- order --frames 120 $arg
  p=$(pixels "$work/$name.jsonl" 120); ps=$?
  [ $res = 0 ] && [ $ps = 0 ]
  s=$?; step $s "$(case $name in order) echo 5;; orderfence) echo 6;; *) echo 7;; esac) order${arg:+ $arg}${envs:+ ($envs)}: $p ($(last $name))"
  [ $s = 0 ] || show $name
done

# 8 recreate while releases are pending
start recreate
fake_start "$work/f8.jsonl" --release-delay-ms 80
run recreate -- recreate
[ $res = 0 ]
s=$?; step $s "8 recreate: $(last recreate)"; [ $s = 0 ] || show recreate

# 9 destroy right after presents
start destroy
fake_start "$work/f9.jsonl" --release-delay-ms 300
run destroy -- destroy --hold-ms 300
[ $res = 0 ]
s=$?; step $s "9 destroy: $(last destroy)"; [ $s = 0 ] || show destroy

# 10 the compositor gone
start lostconn
fake_start "$work/f10.jsonl" --drop-after 20 --vblank-ms 4
run lostconn -- lost
[ $res = 0 ]
s=$?; step $s "10 the compositor dropped the connection: $(last lostconn)"; [ $s = 0 ] || show lostconn

# 11 the device lost
start lostdev VKBRIDGE_TEST_LOST=30 VKBRIDGE_TEST_LOST_COMM=present_path_ch
fake_start "$work/f11.jsonl" --vblank-ms 4
run lostdev -- lost
[ $res = 0 ]
s=$?; step $s "11 the device lost: $(last lostdev)"; [ $s = 0 ] || show lostdev

# 12 idle
start idle
fake_start "$work/f12.jsonl"
run idle -- idle
[ $res = 0 ]
s=$?; step $s "12 idle: $(last idle)"; [ $s = 0 ] || show idle

# 13-19 Vulkan 1.1 under the common Mali profile (the shell's own profile put back after)
if [ ! -f "$out/libvkb_mali_profile.so" ]; then
  step 1 "13-19 the common Mali profile: $out/libvkb_mali_profile.so missing (make -C host)"
else
  saved=$(export -p | grep -E ' (LD_PRELOAD|VKB_PROFILE_[A-Z_]*|VKB_MALI_PROFILE_CUR)=' || true)
  VKB_MALI_PROFILE=common . ./mali_profile.sh >/dev/null
  start v11block VKBRIDGE_TEST_DELAY_CMD=vkCmdSetViewport VKBRIDGE_TEST_DELAY_MS=600
  fake_start "$work/f13.jsonl" --release-delay-ms 50
  run v11block VKBRIDGE_PRESENT_RELEASE_THREAD=1 VKBRIDGE_ASYNC_ACQUIRE=1 -- block --v11 --hold-ms 600
  said=$(grep -c 'images given back as the compositor releases them' "$clog")/$(grep -c 'acquire: the image.s semaphore and fence signalled without a round trip' "$clog")
  [ $res = 0 ] && gralloc v11block && [ "$said" = 1/1 ] && grep -q 'instance created (Vulkan 1.1)' "$work/v11block.out" &&
    grep -q 'test: vkCmdSetViewport held 600 ms' "$work/v11block.server.log"
  s=$?; step $s "13 Vulkan 1.1 (common Mali profile): $(last v11block); decisions said at info: ${said}"; [ $s = 0 ] || show v11block
  start v11blockoff VKBRIDGE_TEST_DELAY_CMD=vkCmdSetViewport VKBRIDGE_TEST_DELAY_MS=600
  fake_start "$work/f14.jsonl" --release-delay-ms 50
  run v11blockoff VKBRIDGE_PRESENT_RELEASE_THREAD=0 VKBRIDGE_ASYNC_ACQUIRE=0 -- block --v11 --hold-ms 600
  [ $res = 1 ] && grep -q 'FAILED: the 4th acquire waited' "$work/v11blockoff.out"
  s=$?; step $s "14 Vulkan 1.1, both switches 0: the old wait ($(last v11blockoff))"; [ $s = 0 ] || show v11blockoff
  for v in "v11order 15 - -" "v11fence 16 --acq-fence -" "v11refuse 17 - VKBRIDGE_FAKE_MISSING=syncfdimport" \
           "v11refusef 17f --acq-fence VKBRIDGE_FAKE_MISSING=syncfdimport" "v11off 18 - -"; do
    read -r name n arg senv <<<"$v"
    [ "$arg" = - ] && arg=
    [ "$senv" = - ] && senv=
    # v0.5.5.4: both switches are off by default here, so 15-17f ask for them by name; 18 sets them 0 (the old path)
    cenv="VKBRIDGE_PRESENT_RELEASE_THREAD=1 VKBRIDGE_ASYNC_ACQUIRE=1"; [ $name = v11off ] && cenv="VKBRIDGE_PRESENT_RELEASE_THREAD=0 VKBRIDGE_ASYNC_ACQUIRE=0"
    start $name $senv
    fake_start "$work/$name.jsonl" --vblank-ms 4 --release-ms 1 --pixels
    run $name $cenv -- order --v11 --frames 120 $arg
    p=$(pixels "$work/$name.jsonl" 120); ps=$?
    fb=$(grep -c 'acquire: the driver took no signalled sync fd' "$work/$name.server.log")
    want=0; [ "${name#v11refuse}" != $name ] && want=1
    # A driver without VK_KHR_external_semaphore_fd / _fence_fd (Ubuntu 26.04's lavapipe, Mesa 26.0.8: GitHub's runner)
    # imports nothing: there the empty-submit fallback is the right path, said once per kind (every Mali in the profiles has both).
    noimp=$(grep -cE 'extensions: .*-KHR_external_(semaphore|fence)_fd' "$work/$name.server.log")
    [ "$want" = 0 ] && [ "$noimp" != 0 ] && [ "$fb" -ge 1 ] && [ "$fb" -le 2 ] && want=$fb && p="$p; the driver has no sync-fd import"
    [ $res = 0 ] && [ $ps = 0 ] && [ "$fb" = $want ]
    s=$?; step $s "$n Vulkan 1.1 order${arg:+ $arg}${senv:+ ($senv)}${cenv:+ ($cenv)}: $p; fallback said at info $fb time(s) ($(last $name))"
    [ $s = 0 ] || show $name
  done
  start v11recreate
  fake_start "$work/f19.jsonl" --release-delay-ms 80
  run v11recreate -- recreate --v11
  [ $res = 0 ]
  s=$?; step $s "19 Vulkan 1.1 recreate: $(last v11recreate)"; [ $s = 0 ] || show v11recreate
  unset LD_PRELOAD VKB_MALI_PROFILE_CUR $(compgen -v VKB_PROFILE_)
  eval "$saved"
fi

if [ -n "${PACE:-}" ]; then
  for pt in ${PACE_POINTS:-8.333:3:0.5:7.5 8.333:4:1:8 8.333:2:1:9 8.333:2:1:10 16.667:6:1:12 16.667:8:2:16 16.667:4:2:18}; do
    IFS=: read -r vb c c2 g <<<"$pt"
    for sw in 0 1; do
      start pace
      fake_start "$work/p.jsonl" --vblank-ms "$vb" --release-ms 1
      run pace$sw VKBRIDGE_PRESENT_RELEASE_THREAD=$sw VKBRIDGE_ASYNC_ACQUIRE=$sw -- lat --frames ${PACE_FRAMES:-480} --cpu-ms "$c" --cpu2-ms "$c2" --gpu-ms "$g"
      shown=$(python3 -I - "$work/p.jsonl" <<'PY'
import json, sys
t = [json.loads(l)["t"] for l in open(sys.argv[1]) if '"latch"' in l]
t = t[60:-5] if len(t) > 80 else t
print("%.1f" % ((len(t) - 1) / (t[-1] - t[0])) if len(t) > 2 else "?")
PY
)
      echo "micro vblank=${vb}ms cpu=${c}+${c2}ms gpu=${g}ms switches=$sw: $(grep -o 'game thread per frame: .*' "$work/pace$sw.out" | sed 's/game thread per frame: //'); shown ${shown} fps [$(last pace$sw)]"
    done
  done
fi
stop
stop_fake

if [ $fails = 0 ]; then echo "OK: the present path's checks passed"; else echo "FAILED: $fails step(s)"; exit 1; fi
