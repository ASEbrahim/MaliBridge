#!/bin/bash
# Persistent pipeline cache (server_pcache.c) on the desktop driver, with a server of its own
# (its own socket and cache directory; the suite's server is left alone):
#   1  a Steam game's cache is keyed by its app id and saved (vkDestroyDevice);
#   2  a killed game keeps what it compiled, its own pipeline caches included (process cleanup);
#   3  Steam's shader pre-caching (fossilize_replay on shadercache/<id>/...) saves to the game's
#      .dl/.run files; the game takes them in at start and they go once its own file holds them;
#   4  processes saving the same file add to each other's work instead of replacing it;
#   5  only the first two empty caches an app makes start from the program's cache, and
#      EXTERNALLY_SYNCHRONIZED ones (Zink's per-program caches) never;
#   6  a game that had a cache by process name starts from it;
#   7  the cache directory removed under the running server (Android empties an app's cache
#      directory when storage runs out): the next save makes it again, said once, no failed saves;
#   8  a full storage (a 1 MB tmpfs, in a user namespace of its own; skipped where there is none):
#      said once, the server keeps running, and the cache is saved once there is room again;
#   9  the total limit (VKBRIDGE_PCACHE_TOTAL_MB): at a server's start the files used longest ago
#      go until the rest fit, the newest stay; a killed save's .tmp goes once its process is gone;
#  10  the limit for one program (VKBRIDGE_PCACHE_MAX_MB): a cache growing past it is said once
#      and its file keeps what it had (no failed saves); a file over a lower limit is said and not
#      loaded;
#  11  that limit by default: RAM/64 within 64-128 MB, never over the total limit; the memory
#      free does not lower it;
#  12  short of memory, a save does not merge with another process's file (said once);
#  13  files are read into the heap unless big and on local storage (a mapped page that cannot be
#      read is a SIGBUS in the driver): VKBRIDGE_PCACHE_MMAP=all maps a small one on local storage,
#      not on a stand-in for FUSE (VKBRIDGE_PCACHE_FAKE_FS), not by default; one that cannot be
#      read starts empty, said, and the server keeps running;
#  14  short of memory at the start (under twice the file's size free), the file is not loaded,
#      said, and the game's saves leave it as it was; with memory again it loads in full.
# Prints one line per step and "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}  # VKB_OUT=out-tsan for the ThreadSanitizer build
work=$(mktemp -d)
cache=$work/c
sock=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-pcache-$$.sock
log=$work/server.log
VK_DRIVER_FILES=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json} VK_ICD_FILENAMES= \
    "$out/vkbridge-server" --socket "$sock" --log "$log" --cache-dir "$cache" >/dev/null 2>&1 &
srv=$!
trap 'kill $srv 2>/dev/null; rm -f "$sock"; rm -rf "$work"' EXIT
for _ in $(seq 50); do [ -S "$sock" ] && break; sleep 0.1; done
export VKBRIDGE_SOCKET=$sock VK_DRIVER_FILES=$out/vkbridge_icd.json VK_ICD_FILENAMES=
unset SteamAppId SteamGameId VKBRIDGE_PCACHE_KEY
fails=0
step() { if [ "$1" = 0 ]; then echo "  ok   $2"; else echo "  FAIL $2"; fails=$((fails+1)); fi; }
size() { local f; f=$(ls "$cache"/*-"$1".vkpc 2>/dev/null | head -1); if [ -n "$f" ]; then stat -c %s "$f"; else echo 0; fi; }
have() { ls "$cache"/*-"$1".vkpc >/dev/null 2>&1; }

# 1
SteamAppId=4242 "$out/pcache_check" --count 40 >$work/1.log 2>&1 && have steam4242 && grep -q "is pcache_check@steam4242" "$log"
step $? "Steam game keyed by app id: $(size steam4242) bytes for 40 pipelines"
s40=$(size steam4242)

# 2 killed: nothing destroyed; pipelines in the device's cache, and in a cache the app made
SteamAppId=4243 "$out/pcache_check" --count 40 --no-destroy >$work/2a.log 2>&1
SteamAppId=4244 "$out/pcache_check" --count 40 --app-caches 1 --no-destroy >$work/2b.log 2>&1
sleep 1
[ "$s40" -gt 0 ] && [ "$(size steam4243)" -ge $((s40 * 9 / 10)) ] && [ "$(size steam4244)" -ge $((s40 * 9 / 10)) ]
step $? "killed game keeps its cache: $(size steam4243) bytes (no cache of its own), $(size steam4244) bytes (its own cache)"

# 3 pre-caching as Steam runs it: the archive path names the game, the process is fossilize_repla
fz=$work/steamapps/shadercache/4245/fozpipelinesv6
mkdir -p "$fz/steamapprun_pipeline_cache.0123456789abcdef"
ln -s "$out/pcache_check" "$work/fossilize_replay"
"$work/fossilize_replay" "$fz/steam_pipeline_cache.foz" --master-process --count 60 >$work/3a.log 2>&1
"$work/fossilize_replay" "$fz/steamapprun_pipeline_cache.0123456789abcdef/steamapp_pipeline_cache.foz" --first 500 --count 20 >$work/3b.log 2>&1
dl=$(size steam4245.dl); run=$(size steam4245.run)
SteamAppId=4245 "$out/pcache_check" --first 0 --count 10 >$work/3c.log 2>&1
grep -q "taken in from .*steam4245.dl.vkpc" "$log" && grep -q "taken in from .*steam4245.run.vkpc" "$log" &&
    [ "$dl" -gt 0 ] && [ "$run" -gt 0 ] && [ "$(size steam4245)" -gt "$dl" ] && [ "$(size steam4245)" -gt "$run" ] && ! have steam4245.dl && ! have steam4245.run
step $? "pre-caching: .dl $dl + .run $run bytes taken in by the game ($(size steam4245) bytes), then removed"

# 4 two processes on one file at once, each saving while the other runs (a file is a header and
#   then its entries: one replacing the other's would leave one process's size)
SteamAppId=4246 "$out/pcache_check" --first 0 --count 40 --hold 12 >$work/4a.log 2>&1 &
a=$!
sleep 2
SteamAppId=4246 "$out/pcache_check" --first 1000 --count 40 --hold 1 >$work/4b.log 2>&1
wait $a
[ "$(size steam4246)" -ge $((s40 * 3 / 2)) ] && grep -q "with what another process saved" "$log"
step $? "two processes, one file: $(size steam4246) bytes (one alone: $s40)"

# 5 seeding: of four empty caches two start from the program's cache; EXTERNALLY_SYNCHRONIZED none
SteamAppId=4242 "$out/pcache_check" --count 1 --app-caches 4 >$work/5a.log 2>&1
SteamAppId=4242 "$out/pcache_check" --count 1 --app-caches 2 --ext-sync >$work/5b.log 2>&1
[ "$(grep -c 'the app makes several pipeline caches' "$log")" = 2 ]
step $? "seeding limited ($(grep -c 'the app makes several pipeline caches' "$log") devices stopped seeding)"

# 6 the cache by process name (before keys) is where a keyed game starts
"$out/pcache_check" --first 2000 --count 20 >$work/6a.log 2>&1
SteamAppId=4247 "$out/pcache_check" --first 2000 --count 20 >$work/6b.log 2>&1
grep -q "steam4247.vkpc ([0-9]* KB loaded from .*pcache_check.vkpc)" "$log"
step $? "a keyed game starts from its cache by process name"

# 7 removed under a running game, as on the tablet: its periodic saves (every 10 s) and the one at
#   its end, then a second removal and a new game (the directory comes back at its device)
SteamAppId=4248 "$out/pcache_check" --first 3000 --count 40 --hold 14 >$work/7a.log 2>&1 &
a=$!
sleep 2
rm -rf "$cache"
wait $a
n7=$(grep -c "cache directory .* was removed; recreated" "$log")
have steam4248 && [ "$n7" = 1 ] && ! grep -q "pipeline cache not saved" "$log"
step $? "cache directory removed under a running game: recreated (said $n7 time(s)), $(size steam4248) bytes saved"
rm -rf "$cache"
SteamAppId=4249 "$out/pcache_check" --first 3100 --count 10 >$work/7b.log 2>&1
have steam4249 && [ "$(grep -c "cache directory .* was removed; recreated" "$log")" = 2 ]
step $? "removed again: recreated at the next game ($(size steam4249) bytes)"

# 8 a full storage
if unshare -rm true 2>/dev/null; then
  unshare -rm bash -c '
    out=$1; work=$2; sock=$3.full
    mkdir -p "$work/full" && mount -t tmpfs -o size=1m tmpfs "$work/full" || exit 2
    cache=$work/full/c; log=$work/server-full.log
    VK_DRIVER_FILES=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json} VK_ICD_FILENAMES= \
        "$out/vkbridge-server" --socket "$sock" --log "$log" --cache-dir "$cache" >/dev/null 2>&1 &
    srv=$!
    trap "kill $srv 2>/dev/null; rm -f $sock" EXIT
    for _ in $(seq 50); do [ -S "$sock" ] && break; sleep 0.1; done
    export VKBRIDGE_SOCKET=$sock VK_DRIVER_FILES=$out/vkbridge_icd.json VK_ICD_FILENAMES= VKBRIDGE_PROBE=0
    dd if=/dev/zero of="$work/full/filler" bs=4k count=1000 2>/dev/null # every block but a few
    SteamAppId=4250 "$out/pcache_check" --first 4000 --count 80 --hold 22 >"$work/8a.log" 2>&1 || exit 3
    SteamAppId=4251 "$out/pcache_check" --first 4100 --count 80 >"$work/8b.log" 2>&1 || exit 4
    nospace=$(grep -c "no space left on the device" "$log"); notsaved=$(grep -c "pipeline cache not saved" "$log")
    rm -f "$work/full/filler"
    SteamAppId=4252 "$out/pcache_check" --first 4200 --count 20 >"$work/8c.log" 2>&1 || exit 5
    kill -0 $srv || exit 6
    ls "$cache"/*-steam4252.vkpc >/dev/null 2>&1 || exit 7
    grep -q "writes go through again" "$log" || exit 8
    echo "$nospace $notsaved" > "$work/8.res"
    [ "$nospace" = 1 ] && [ "$notsaved" = 0 ] || exit 9
  ' _ "$out" "$work" "$sock"
  r=$?
  step $r "full storage: said $(cut -d" " -f1 $work/8.res 2>/dev/null) time(s), $(cut -d" " -f2 $work/8.res 2>/dev/null) other save warnings, saved once there was room (exit $r)"
  [ $r = 0 ] || grep -E "space|not saved|recreated|cache saved" "$work/server-full.log" | head -20
else
  echo "  skip full storage (no user namespaces)"
fi

# 9 four 1 MB files of different ages under a 2 MB limit, at another server's start
c9=$work/c9; mkdir -p "$c9"
for i in 1 2 3 4; do head -c 1048576 /dev/zero > "$c9/0000-0000-x-old$i.vkpc"; touch -d "-$((10 - i)) days" "$c9/0000-0000-x-old$i.vkpc"; done
echo keep > "$c9/stuck-pipelines"
sleep 30 & live=$!
head -c 4096 /dev/zero > "$c9/0000-0000-x-old1.vkpc.4194303.1.tmp" # no such process
head -c 4096 /dev/zero > "$c9/0000-0000-x-old2.vkpc.$live.1.tmp"   # a save going on elsewhere
VKBRIDGE_PCACHE_TOTAL_MB=2 VK_DRIVER_FILES=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json} VK_ICD_FILENAMES= \
    "$out/vkbridge-server" --socket "$sock.9" --log "$work/server9.log" --cache-dir "$c9" >/dev/null 2>&1 &
s9=$!
for _ in $(seq 50); do [ -S "$sock.9" ] && break; sleep 0.1; done
kill $s9 2>/dev/null; wait $s9 2>/dev/null; rm -f "$sock.9"
left=$(cd "$c9" && ls *.vkpc 2>/dev/null | tr '\n' ' ')
kill $live 2>/dev/null; wait $live 2>/dev/null
[ "$left" = "0000-0000-x-old3.vkpc 0000-0000-x-old4.vkpc " ] && [ -f "$c9/stuck-pipelines" ] && grep -q "2 removed, 2 MB freed" "$work/server9.log"
step $? "total limit: the two oldest of four 1 MB files removed at a 2 MB limit (left: $left)"
[ ! -f "$c9/0000-0000-x-old1.vkpc.4194303.1.tmp" ] && [ -f "$c9/0000-0000-x-old2.vkpc.$live.1.tmp" ] &&
    grep -q "old1.vkpc.4194303.1.tmp (4 KB) removed: left by a save that did not finish" "$work/server9.log"
step $? "a killed save's .tmp removed, a live process's kept"

# 10 a 4 KB limit: 20 pipelines fit (about 2.3 KB), 60 do not; then a 1 KB limit at another start
srv10() { # srv10 <log> <env...>: a server of its own on $sock.10 with cache $work/c10
  local l=$1; shift
  env "$@" VK_DRIVER_FILES=${VKB_HOST_ICD:-/usr/share/vulkan/icd.d/radeon_icd.json} VK_ICD_FILENAMES= \
      "$out/vkbridge-server" --socket "$sock.10" --log "$l" --cache-dir "$work/c10" >/dev/null 2>&1 &
  s10=$!
  for _ in $(seq 50); do [ -S "$sock.10" ] && break; sleep 0.1; done
}
end10() { kill $s10 2>/dev/null; wait $s10 2>/dev/null; rm -f "$sock.10"; }
size10() { local f; f=$(ls "$work"/c10/*-"$1".vkpc 2>/dev/null | head -1); if [ -n "$f" ]; then stat -c %s "$f"; else echo 0; fi; }
l10=$work/server10.log
srv10 $l10 VKBRIDGE_PCACHE_MAX_MB=0.004 VKBRIDGE_PROBE=0
VKBRIDGE_SOCKET=$sock.10 SteamAppId=4260 "$out/pcache_check" --first 5000 --count 20 >$work/10a.log 2>&1
a10=$(size10 steam4260)
VKBRIDGE_SOCKET=$sock.10 SteamAppId=4260 "$out/pcache_check" --first 5000 --count 60 --hold 12 >$work/10b.log 2>&1
b10=$(size10 steam4260)
end10
nfull=$(grep -c "over the 4 KB limit for one program (VKBRIDGE_PCACHE_MAX_MB): the file keeps what it has" $l10)
[ "$a10" -gt 0 ] && [ "$a10" -le 4194 ] && [ "$b10" = "$a10" ] && [ "$nfull" = 1 ] && ! grep -q "pipeline cache not saved" $l10 &&
    grep -q "steam4260.vkpc (0 KB loaded), limit 4 KB" $l10
step $? "one program's limit: $a10 bytes kept, 60 pipelines over it said $nfull time(s) (file $b10 bytes)"
l10b=$work/server10b.log
srv10 $l10b VKBRIDGE_PCACHE_MAX_MB=0.001 VKBRIDGE_PROBE=0
VKBRIDGE_SOCKET=$sock.10 SteamAppId=4260 "$out/pcache_check" --first 5000 --count 2 >$work/10c.log 2>&1
end10
grep -q "steam4260.vkpc is [0-9]* KB, over the 1 KB limit for one program (VKBRIDGE_PCACHE_MAX_MB): not loaded" $l10b
step $? "a file over a lower limit: said, not loaded"

# 11 the default by RAM (VKBRIDGE_FAKE_MEMTOTAL_MB), under a smaller total limit, and by the memory
#    free (VKBRIDGE_FAKE_MEMAVAIL_MB; plenty unless given, so the PC's own state does not count)
lim11=
for ram in 12000:128 6000:93 1024:64 12000,100:100 12000/200:128 12000/80:128; do
  r=${ram%%:*}; want=${ram#*:}; tot=; av=100000
  case $r in */*) av=${r#*/}; r=${r%/*};; esac
  case $r in *,*) tot=${r#*,}; r=${r%,*};; esac
  l11=$work/server11-$r-$tot-$av.log
  srv10 $l11 VKBRIDGE_FAKE_MEMTOTAL_MB=$r VKBRIDGE_FAKE_MEMAVAIL_MB=$av ${tot:+VKBRIDGE_PCACHE_TOTAL_MB=$tot} VKBRIDGE_PROBE=0
  VKBRIDGE_SOCKET=$sock.10 SteamAppId=4261 "$out/pcache_check" --first 5100 --count 2 >$work/11.log 2>&1
  end10
  got=$(grep -o "steam4261.vkpc ([0-9]* KB loaded), limit [0-9]* MB" $l11 | grep -o "[0-9]* MB$" | head -1)
  x=${av%100000}; lim11="$lim11 ${r}MB${tot:+/total $tot}${x:+/$x free}:$got"
  [ "$got" = "$want MB" ] || lim11="$lim11(want $want)"
done
! echo "$lim11" | grep -q want
step $? "default limit by RAM:$lim11"

# 12 as 4, two processes on one file, with 10 KB "free": each saves its own, no merge, said once each
l12=$work/server12.log
srv10 $l12 VKBRIDGE_FAKE_MEMAVAIL_MB=0.01 VKBRIDGE_PROBE=0
VKBRIDGE_SOCKET=$sock.10 SteamAppId=4262 "$out/pcache_check" --first 0 --count 40 --hold 12 >$work/12a.log 2>&1 &
a=$!
sleep 2
VKBRIDGE_SOCKET=$sock.10 SteamAppId=4262 "$out/pcache_check" --first 1000 --count 40 --hold 1 >$work/12b.log 2>&1
wait $a
end10
n12=$(grep -c "saved without what another process saved: 10 KB of memory free" $l12)
[ "$n12" -ge 1 ] && ! grep -q "with what another process saved)" $l12 && [ "$(size10 steam4262)" -gt 0 ] && ! grep -q "pipeline cache not saved" $l12
step $? "short of memory: no merge (said $n12 time(s)), $(size10 steam4262) bytes saved"

# 13 mapped or read
fs13=$(stat -f -c %T "$work")
case $fs13 in ext2/ext3|ext4|f2fs|tmpfs|btrfs|xfs) local13=1;; *) local13=0;; esac
load13() { # load13 <appid> <env...>: a first run saves, a second loads; prints the second's load line
  local id=$1; shift
  local l=$work/server13-$id.log
  srv10 $l VKBRIDGE_PROBE=0 "$@"
  VKBRIDGE_SOCKET=$sock.10 SteamAppId=$id "$out/pcache_check" --first 6000 --count 10 >$work/13a.log 2>&1
  VKBRIDGE_SOCKET=$sock.10 SteamAppId=$id "$out/pcache_check" --first 6000 --count 10 >$work/13b.log 2>&1
  kill -0 $s10 2>/dev/null && echo alive
  end10
  grep -o "steam$id.vkpc ([0-9]* KB loaded), limit [^,]*\(, mapped\)\?$" $l | tail -1
}
m13=$(load13 4270 VKBRIDGE_PCACHE_MMAP=all)
f13=$(load13 4271 VKBRIDGE_PCACHE_MMAP=all VKBRIDGE_PCACHE_FAKE_FS=fuse)
d13=$(load13 4272)
mkdir -p "$work"/c10 && for f in "$work"/c10/*-steam4270.vkpc; do d=${f%4270.vkpc}4273.vkpc; mkdir -p "$d"; done
u13=$(load13 4273 VKBRIDGE_PCACHE_MMAP=all)
{ [ $local13 = 0 ] || echo "$m13" | grep -q ", mapped$"; } && echo "$m13" | grep -q "^alive" &&
    ! echo "$f13" | grep -q "mapped" && echo "$f13" | grep -q "KB loaded" &&
    ! echo "$d13" | grep -q "mapped" && echo "$d13" | grep -q "KB loaded" &&
    echo "$u13" | grep -q "^alive" && grep -q "steam4273.vkpc could not be read (Is a directory): starting without it" $work/server13-4273.log
step $? "mapped only where it may ($fs13: all=${m13##*, }; fuse: $(echo "$f13" | grep -c mapped) mapped; default: $(echo "$d13" | grep -c mapped) mapped); an unreadable file starts empty"

# 14 a file saved with plenty free, then a start with 5 KB "free" (a few new pipelines, saved at
#    the end), then plenty again
l14=$work/server14
srv10 $l14-a.log VKBRIDGE_PROBE=0
VKBRIDGE_SOCKET=$sock.10 SteamAppId=4274 "$out/pcache_check" --first 0 --count 40 >$work/14a.log 2>&1
end10
a14=$(size10 steam4274); i14a=$(stat -c %i "$work"/c10/*-steam4274.vkpc | head -1)
srv10 $l14-b.log VKBRIDGE_FAKE_MEMAVAIL_MB=0.005 VKBRIDGE_PROBE=0
VKBRIDGE_SOCKET=$sock.10 SteamAppId=4274 "$out/pcache_check" --first 3000 --count 5 >$work/14b.log 2>&1
end10
b14=$(size10 steam4274); i14b=$(stat -c %i "$work"/c10/*-steam4274.vkpc | head -1)  # a save writes aside and renames over: a new inode
srv10 $l14-c.log VKBRIDGE_PROBE=0
VKBRIDGE_SOCKET=$sock.10 SteamAppId=4274 "$out/pcache_check" --first 0 --count 1 >$work/14c.log 2>&1
end10
c14=$(grep -o "steam4274.vkpc ([0-9]* KB loaded" $l14-c.log | grep -o "[0-9]* KB" | head -1)
[ "$a14" -gt 0 ] && [ "$b14" = "$a14" ] && [ -n "$i14a" ] && [ "$i14b" = "$i14a" ] && grep -q "steam4274.vkpc ([0-9]* KB) not loaded this time" $l14-b.log &&
    grep -q "steam4274.vkpc not saved: the file was not loaded" $l14-b.log && [ "$c14" = "$((a14 >> 10)) KB" ]
step $? "short of memory at the start: $a14 bytes not loaded, $b14 bytes after that run, then $c14 loaded (file $([ "$i14b" = "$i14a" ] && echo untouched || echo rewritten))"

grep -E "process [0-9]+ is|pipeline cache|taken in|removed|several|cache directory" "$log" > "$out/t-pcache-server.log"
if [ $fails = 0 ]; then echo "OK: persistent pipeline cache"; else echo "FAILED: $fails step(s) (server log in $out/t-pcache-server.log)"; exit 1; fi
