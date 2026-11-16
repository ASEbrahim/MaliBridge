#!/bin/bash
# A driver older than Vulkan 1.1 under a 1.1 loader (a player log: Note 8 Mali-G71, Android 9): the
# loader hands out no 1.1 core physical-device entry point, and the test49 server called
# vkGetPhysicalDeviceProperties2 through NULL right after 'self-test: loader Vulkan 1.1' (SIGSEGV
# pc 0x0, server_instance.c vkb_selftest_all). VKBRIDGE_FAKE_DRIVER=arm10 imitates that driver.
#   1  --selftest on arm10: no crash; one 'GPU driver is too old' line naming the Vulkan version and
#      the missing entry point; exit status 1 (nothing to serve);
#   2  as a server on arm10: exits 1 before making its socket, the same line, no crash line;
#   3  the desktop driver as it is: --selftest exits 0 and says nothing of an old driver.
# Prints one line per step and "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
work=$(mktemp -d)
trap '[ -n "$KEEP" ] && echo "kept $work" || rm -rf "$work"' EXIT
fails=0 steps=0
step() { steps=$((steps+1)); if [ "$1" = 0 ]; then echo "  ok   $2"; else echo "  FAIL $2"; fails=$((fails+1)); fi; }
# run <name> <fake or ""> <extra args...>: the server, its exit status in $work/<name>.status
run() {
  local name=$1 fake=$2; shift 2
  mkdir -p "$work/c-$name"
  env -u VKBRIDGE_FAKE_DRIVER -u VKBRIDGE_FAKE_MISSING ${fake:+VKBRIDGE_FAKE_DRIVER=$fake} VKBRIDGE_PROBE=0 \
      VK_DRIVER_FILES=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/lvp_icd.json} VK_ICD_FILENAMES= \
      timeout 60 "$out/vkbridge-server" --socket "$work/$name.sock" --log "$work/$name.log" --cache-dir "$work/c-$name" "$@" \
      >/dev/null 2>"$work/$name.err"
  echo $? >"$work/$name.status"
}
too_old="GPU driver is too old"

run self arm10 --selftest
st=$(cat "$work/self.status"); line=$(grep -m1 "$too_old" "$work/self.log")
[ "$st" = 1 ] && ! grep -q "crashed with signal" "$work/self.log" &&
  echo "$line" | grep -q "Vulkan 1.0" && echo "$line" | grep -q "vkGetPhysicalDeviceProperties2"
step $? "1 arm10 --selftest: status $st (want 1), no crash, line: ${line#*E }"

run serve arm10
st=$(cat "$work/serve.status")
[ "$st" = 1 ] && [ ! -S "$work/serve.sock" ] && grep -q "$too_old" "$work/serve.log" && ! grep -q "crashed with signal" "$work/serve.log"
step $? "2 arm10 as a server: status $st (want 1), no socket, the line, no crash"

run plain "" --selftest
st=$(cat "$work/plain.status")
[ "$st" = 0 ] && ! grep -q "$too_old" "$work/plain.log"
step $? "3 the desktop driver: --selftest status $st (want 0), no old-driver line"

[ $fails = 0 ] && echo "OK: an old driver ends with a clear line ($steps steps)" || echo "FAILED: $fails of $steps step(s)"
[ $fails = 0 ]
