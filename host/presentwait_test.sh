#!/bin/bash
# Present ids and waits (VK_KHR_present_id/present_wait, present_id2/present_wait2; client/wsi.c),
# each step with a server of its own: tests/presentwait_check (ordering, a held frame, steady
# waits, timeout, retire and destroy while a thread waits) on
#   1-2  a headless surface (copy presents: shown when handed over), v1 and v2;
#   3-4  a Wayland surface of the desktop's compositor (dma-buf images, the presenting thread: shown
#        when the compositor says so; a role-less surface is never shown, so the client's 500 ms
#        rule ends those waits), v1 and v2; skipped without a compositor in WAYLAND_DISPLAY;
#   5    VKBRIDGE_PRESENT_WAIT=0 in the guest: nothing offered;
#   6    VKBRIDGE_PRESENT_WAIT=0 in <cache dir>/env (the server's answer): nothing offered;
#   7    the guest's VKBRIDGE_PRESENT_WAIT=1 beats <cache dir>/env's 0;
#   8    VKBRIDGE_PRESENT_WAIT=commit on the Wayland surface: a frame counts as shown once committed
#        (run before 5-7; skipped without a compositor);
#   9    an app whose engine is DXVK is not offered them by default; VKBRIDGE_PRESENT_WAIT=1 offers them.
# Prints one line per step and "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
icd=${VKB_CLIENT_ICD:-$out/vkbridge_icd.json}
work=$(mktemp -d)
sock=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-pw-$$.sock
srv=
fails=0
stop() { [ -n "$srv" ] && kill $srv 2>/dev/null && wait $srv 2>/dev/null; srv=; }
trap 'stop; [ -n "${VKB_KEEP:-}" ] && echo "kept $work" || rm -rf "$work"; rm -f "$sock"' EXIT
start() { # start <name> [env...]: a server with cache $work/c ($work/c/env as written before)
  local name=$1; shift
  stop
  rm -f "$sock"
  mkdir -p "$work/c" "$work/$name"
  sdir=$work/$name
  env -u VKBRIDGE_FAKE_MISSING -u VKBRIDGE_HIDE_EXTS -u VKBRIDGE_ERRATA -u VKBRIDGE_ERRATA_IMITATE -u VKBRIDGE_MODE \
      -u VKBRIDGE_FAKE_DRIVER_VERSION -u VKBRIDGE_ERRATA_PROBE -u VKBRIDGE_HITCH_MS -u VKBRIDGE_HANG_DETECT \
      -u VKBRIDGE_REPORT_DIR -u VKBRIDGE_FRAME_QUEUE -u VKBRIDGE_LATLOG -u VKBRIDGE_PRESENT_WAIT VKBRIDGE_PROBE_BUDGET_S=0 "$@" \
      VK_DRIVER_FILES=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json} VK_ICD_FILENAMES= \
      "$out/vkbridge-server" --socket "$sock" --log "$sdir/server.log" --cache-dir "$work/c" >/dev/null 2>&1 &
  srv=$!
  for _ in $(seq 300); do [ -S "$sock" ] && break; kill -0 $srv 2>/dev/null || break; sleep 0.05; done
}
run() { # run <name> [env...] -- <presentwait_check args>: $res its exit status, $clog the client log
  local name=$1; shift
  local e=()
  while [ "$1" != -- ]; do e+=("$1"); shift; done
  shift
  clog=$work/$name.client.log
  : > "$clog"
  timeout 120 env -u VKBRIDGE_LATLOG -u VKBRIDGE_FRAME_QUEUE -u VKBRIDGE_PRESENT_WAIT "${e[@]}" \
    VKBRIDGE_SOCKET="$sock" VK_DRIVER_FILES="$icd" VK_ICD_FILENAMES= VKBRIDGE_CLIENT_LOG="$clog" VKBRIDGE_LOG_LEVEL=2 \
    "$out/presentwait_check" "$@" >"$work/$name.out" 2>&1
  res=$?
  for _ in $(seq 100); do grep -q "client process [0-9]* gone" "$sdir/server.log" && break; sleep 0.05; done
}
step() { # step <status> <text>
  if [ "$1" = 0 ]; then echo "ok   $2"; else echo "FAIL $2"; fails=$((fails+1)); fi
}
last() { grep -E '^(OK|FAILED|SKIPPED)' "$work/$1.out" | tail -1; }
show_fails() { grep '^FAIL' "$work/$1.out" | sed 's/^/       /'; }

# 1-2 headless
for v in 1 2; do
  a=; [ $v = 2 ] && a=--v2
  start hl$v
  run hl$v -- $a
  [ $res = 0 ] && grep -q 'present wait: [0-9]* waits' "$clog"
  step $? "$v headless, v$v: $(last hl$v)"
  show_fails hl$v
done

# 3-4 Wayland
for v in 1 2; do
  a=; [ $v = 2 ] && a=--v2
  start wl$v
  run wl$v -- --wayland $a
  if grep -q '^SKIPPED' "$work/wl$v.out"; then
    echo "ok   $((v+2)) Wayland, v$v: skipped (no compositor in WAYLAND_DISPLAY)"
    continue
  fi
  [ $res = 0 ]
  step $? "$((v+2)) Wayland, v$v: $(last wl$v)"
  show_fails wl$v
done

# 8 (Wayland) =commit: shown once committed, so the role-less surface's frame 1 is not held 500 ms
if [ -n "${WAYLAND_DISPLAY:-}" ]; then
  start commit
  run commit VKBRIDGE_PRESENT_WAIT=commit -- --wayland
  after=$(grep -o '[0-9.]* ms after the GPU work was let go' "$work/commit.out" | cut -d' ' -f1)
  [ $res = 0 ] && grep -q 'VKBRIDGE_PRESENT_WAIT=commit' "$clog" && awk -v a="${after:-999}" 'BEGIN { exit !(a < 100) }'
  step $? "8 VKBRIDGE_PRESENT_WAIT=commit, Wayland: frame 1 shown ${after:-?} ms after its GPU work ($(last commit))"
  show_fails commit
fi

# 5 off in the guest
start off
run off VKBRIDGE_PRESENT_WAIT=0 -- --off
[ $res = 0 ] && grep -q 'present wait: off (VKBRIDGE_PRESENT_WAIT=0)' "$clog"
step $? "5 VKBRIDGE_PRESENT_WAIT=0, guest: $(last off)"

# 6 off in <cache dir>/env
mkdir -p "$work/c"
echo "VKBRIDGE_PRESENT_WAIT=0" > "$work/c/env"
start envoff
run envoff -- --off
[ $res = 0 ] && grep -q 'present wait: off' "$clog"
step $? "6 VKBRIDGE_PRESENT_WAIT=0 in <cache dir>/env: $(last envoff)"

# 7 the guest's 1 beats the env file's 0
start guest1
run guest1 VKBRIDGE_PRESENT_WAIT=1 --
[ $res = 0 ]
step $? "7 the guest's VKBRIDGE_PRESENT_WAIT=1 over <cache dir>/env's 0: $(last guest1)"
rm -f "$work/c/env"

# 9 DXVK: not offered by default; VKBRIDGE_PRESENT_WAIT=1 offers it
start dxvk
run dxvk -- --off --engine DXVK
[ $res = 0 ] && grep -q 'present wait: not offered to DXVK' "$clog"
step $? "9 engine DXVK, nothing set: $(last dxvk)"
start dxvk1
run dxvk1 VKBRIDGE_PRESENT_WAIT=1 -- --engine DXVK
[ $res = 0 ]
step $? "9 engine DXVK, VKBRIDGE_PRESENT_WAIT=1: $(last dxvk1)"

if [ $fails = 0 ]; then echo "OK: present ids and waits: offered, ordered, timed out, ended by retire and destroy, and off when asked"
else echo "FAILED: $fails step(s)"; exit 1; fi
