#!/bin/bash
# vkd3d-proton's reserved buffers on a driver without sparse memory (server_emu_sparse.c), with a
# server of its own imitating one (VKBRIDGE_FAKE_MISSING=sparse):
#   1  sparse_check as engine "vkd3d": sparse queue flag, address at creation, real memory, binds
#      signal their timeline value and fence;
#   2  as another engine: no sparse queue flag;
#   3  VKBRIDGE_SPARSE_MAX_MB=2: the 3 MB reserved buffer is refused, said once.
# Prints one line per step and "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
work=$(mktemp -d)
sock=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-sparse-$$.sock
srv=
start() { # start <env...>
  [ -n "$srv" ] && kill $srv 2>/dev/null && wait $srv 2>/dev/null
  rm -f "$sock"
  env -u VKBRIDGE_HIDE_EXTS -u VKBRIDGE_MODE VKBRIDGE_FAKE_MISSING=sparse VKBRIDGE_PROBE=0 "$@" \
    VK_DRIVER_FILES=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json} VK_ICD_FILENAMES= \
    "$out/vkbridge-server" --socket "$sock" --log "$work/server.log" --cache-dir "$work/c" >/dev/null 2>&1 &
  srv=$!
  for _ in $(seq 50); do [ -S "$sock" ] && break; sleep 0.1; done
}
trap 'kill $srv 2>/dev/null; rm -f "$sock"; [ -n "$KEEP" ] && echo "kept $work" || rm -rf "$work"' EXIT
export VKBRIDGE_SOCKET=$sock VK_DRIVER_FILES=$out/vkbridge_icd.json VK_ICD_FILENAMES=
unset VKBRIDGE_HIDE_EXTS VKBRIDGE_MAX_API VKBRIDGE_SYNC
fails=0 steps=0
step() { steps=$((steps+1)); if [ "$1" = 0 ]; then echo "  ok   $2"; else echo "  FAIL $2"; fails=$((fails+1)); fi; }

start
timeout 60 "$out/sparse_check" vkd3d >$work/1.log 2>&1
step $? "1 vkd3d: $(tail -1 $work/1.log)"
timeout 60 "$out/sparse_check" other >$work/2.log 2>&1
step $? "2 another engine: $(tail -1 $work/2.log)"
start VKBRIDGE_SPARSE_MAX_MB=2
timeout 60 "$out/sparse_check" vkd3d >$work/3.log 2>&1
grep -q "FAIL reserved buffer created (-2)" $work/3.log && [ "$(grep -c "a reserved buffer of 3 MB refused" $work/server.log)" = 1 ]
step $? "3 over the budget: refused, said $(grep -c "reserved buffer of 3 MB refused" $work/server.log) time(s)"

if [ $fails = 0 ]; then echo "OK: $steps steps"; else echo "FAILED: $fails of $steps steps"; fi
[ $fails = 0 ]
