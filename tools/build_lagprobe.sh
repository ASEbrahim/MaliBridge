#!/bin/bash
# Builds tools/lagprobe.exe (the camera latency test, tools/lagprobe.c) with mingw-w64.
#   tools/build_lagprobe.sh [out.exe]
# MINGW_CC names the compiler (default x86_64-w64-mingw32-gcc); MINGW_ROOT, a mingw-w64 tree unpacked
# from its packages without installing them (its usr/ inside), is used when given.
set -e
here=$(cd "$(dirname "$0")" && pwd)
out=${1:-$here/lagprobe.exe}
cc=${MINGW_CC:-x86_64-w64-mingw32-gcc}
flags=()
if [ -n "${MINGW_ROOT:-}" ]; then
  r=$MINGW_ROOT/usr
  gv=$(ls "$r/lib/gcc/x86_64-w64-mingw32" | head -1)
  [ -x "$r/bin/$cc" ] || cc=$(ls "$r"/bin/x86_64-w64-mingw32-gcc-[0-9]* | head -1)
  case $cc in /*) ;; *) cc=$r/bin/$cc ;; esac
  flags=(-B"$r/lib/gcc/x86_64-w64-mingw32/$gv/" -B"$r/libexec/gcc/x86_64-w64-mingw32/$gv/" -B"$r/x86_64-w64-mingw32/bin/"
         -isystem "$r/x86_64-w64-mingw32/include" -L"$r/x86_64-w64-mingw32/lib" -L"$r/lib/gcc/x86_64-w64-mingw32/$gv")
fi
"$cc" "${flags[@]}" -O2 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-cast-function-type \
  "$here/lagprobe.c" -o "$out" -ld3d11 -ldxgi -lwinmm -mwindows -static-libgcc
echo "built $out ($(stat -c %s "$out") bytes)"
# lagprobe12.exe: the same on D3D12 (vkd3d-proton), next to it
out12=${LAGPROBE12_OUT:-$(dirname "$out")/lagprobe12.exe}
"$cc" "${flags[@]}" -O2 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-cast-function-type \
  "$here/lagprobe12.c" -o "$out12" -ld3d12 -ldxgi -lwinmm -mwindows -static-libgcc
echo "built $out12 ($(stat -c %s "$out12") bytes)"
