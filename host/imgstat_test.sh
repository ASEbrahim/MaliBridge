#!/bin/bash
# Uncompressed images by format in the server's memory line (server_memory.c; ledger 357: a Unity
# game's 6,583 non-BC images, 2.7 GB, filled a 7.2 GB phone and the line said only "images
# 2686M/6630 (BC 5M/47)"). imgstat_check makes 4 RGBA8 images (2048 with 12 levels, 512 with 10,
# 1024 flat, 1024 colour attachment) and one RGBA16F 256 flat and holds them; the line must read
#   other images fmt37 4:<all>M (mips <2048+512>M, over 1024 <2048>M, targets <attachment>M) fmt97 1:...
# with the sizes the driver gave the checker; once it destroys them, the next line has none. A server of its own, without BC (VKBRIDGE_FAKE_MISSING=bc:
# images are accounted where the emulation layer runs, as on Mali). Prints "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
work=$(mktemp -d)
sock=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-imgstat-$$.sock
srv=
stop() { [ -n "$srv" ] && kill $srv 2>/dev/null && wait $srv 2>/dev/null; srv=; }
trap 'stop; rm -f "$sock"; [ -n "$KEEP" ] && echo "kept $work" || rm -rf "${work:?}"' EXIT
mkdir -p "$work/c"
log=$work/server.log
env -u VKBRIDGE_HIDE_EXTS -u VKBRIDGE_MODE -u VKBRIDGE_MEM_STATS -u VKBRIDGE_TEX_MAXDIM \
    VKBRIDGE_FAKE_MISSING=bc VK_DRIVER_FILES=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json} VK_ICD_FILENAMES= \
    "$out/vkbridge-server" --socket "$sock" --log "$log" --cache-dir "$work/c" >/dev/null 2>&1 &
srv=$!
for _ in $(seq 100); do [ -S "$sock" ] && break; kill -0 $srv 2>/dev/null || break; sleep 0.1; done
export VKBRIDGE_SOCKET=$sock VK_DRIVER_FILES=$out/vkbridge_icd.json VK_ICD_FILENAMES=
unset VKBRIDGE_HIDE_EXTS VKBRIDGE_MAX_API VKBRIDGE_SYNC
timeout 60 "$out/imgstat_check" 6 >"$work/check.out" 2>&1; rc=$?
stop
sz() { awk -v n="$1" '$1=="size" && $2==n {print $3}' "$work/check.out"; }
big=$(sz big_mipped) small=$(sz small_mipped) flat=$(sz flat) tgt=$(sz target) half=$(sz half)
if [ $rc != 0 ] || [ -z "$big" ] || [ -z "$half" ]; then
  cat "$work/check.out"; echo "FAILED: imgstat_check rc $rc"; exit 1
fi
mb() { echo $(( $1 >> 20 )); }
all=$(mb $((big + small + flat + tgt))) mips=$(mb $((big + small))) over=$(mb $big) t=$(mb $tgt) h=$(mb $half)
want="other images fmt37 4:${all}M (mips ${mips}M, over 1024 ${over}M, targets ${t}M) fmt97 1:${h}M (mips 0M, over 1024 0M, targets 0M)"
fails=0
if grep -qF "$want" "$log"; then
  echo "  ok   held: $want"
else
  echo "  FAIL held, want: $want"
  echo "       got:  $(grep -o 'images [0-9]*M/[0-9]* .*| +' "$log" | head -1)"; fails=$((fails+1))
fi
last=$(grep 'mem: rss' "$log" | tail -1)
if grep -q destroyed "$work/check.out" && echo "$last" | grep -q 'images 0M/0 ' && ! echo "$last" | grep -q 'other images'; then
  echo "  ok   destroyed: the last line has no images and no per-format entries"
else
  echo "  FAIL destroyed, last line: $(echo "$last" | grep -o 'images [0-9]*M/[0-9]* .*| +')"; fails=$((fails+1))
fi
[ $fails = 0 ] && echo "OK: 2 steps" || { echo "FAILED: $fails of 2 steps"; exit 1; }
