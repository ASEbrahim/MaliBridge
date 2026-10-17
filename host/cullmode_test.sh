#!/bin/bash
# cull_nontri (MK-16, server_errata_cmd.c) on the desktop driver, with a server of its own (its own
# socket and cache directory; the switches go in <cache dir>/env, re-read at each device):
#   cull_nontri, cullmode_check:
#     1  on (always, safe mode): every case passes;
#     2  the bug imitated beneath, fix off: the point and line cases fail (the draws culled away);
#     3  the bug imitated, fix on: all pass, cull modes sent as NONE for points and lines;
#     4  performance mode: off on r52p0 (not installed), on on r49p1 (below r52);
#     5  stacked with dyn_reemit (MK-04) over its imitation: dynstate_check and cullmode_check pass;
#   and:
#     6  the protocol hash is unchanged (server-side only).
# Prints one line per step and "OK: ..." / "FAILED: ..." last.
cd "$(dirname "$0")"
out=${VKB_OUT:-$PWD/out}
work=$(mktemp -d)
cache=$work/c
mkdir -p "$cache"
sock=${XDG_RUNTIME_DIR:-/tmp}/vkbridge-cullded-$$.sock
log=$work/server.log
srv=
start() {
  [ -n "$srv" ] && kill $srv 2>/dev/null && wait $srv 2>/dev/null
  rm -f "$sock"
  env -u VKBRIDGE_FAKE_MISSING -u VKBRIDGE_HIDE_EXTS -u VKBRIDGE_ERRATA -u VKBRIDGE_ERRATA_IMITATE -u VKBRIDGE_MODE \
      -u VKBRIDGE_FAKE_DRIVER_VERSION -u VKBRIDGE_ERRATA_PROBE "$@" \
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

# run <env lines...> -- <program args...>: $rc, $res (last line), $new (the server's log lines),
# $out_log (the program's output)
run() {
  local envs=()
  while [ "$1" != -- ]; do envs+=("$1"); shift; done
  shift
  printf '%s\n' "${envs[@]}" > "$cache/env"
  local l; l=$(wc -l < "$log")
  timeout 120 "$out/$@" >$work/run.log 2>&1
  rc=$?
  res=$(tail -1 $work/run.log)
  new=$(tail -n +$((l+1)) "$log")
}
on_set() { echo "$new" | grep -o 'errata: device .*: on [^;]*' | tail -1 | sed 's/.*: on //'; }
has() { case ",$1," in *",$2,"*) return 0 ;; esac; return 1; }

start
# 1
run VKBRIDGE_MODE=safe -- cullmode_check
[ $rc = 0 ] && echo "$new" | grep -q "errata: cull_nontri ON (always; safe mode) - MK-16"
step $? "cull_nontri on (always, safe mode): $res"
# 2
run VKBRIDGE_ERRATA=-cull_nontri VKBRIDGE_ERRATA_IMITATE=cull_nontri -- cullmode_check
[ $rc != 0 ] && echo "$res" | grep -q "^FAIL: 4 of 5 checks failed" && grep -q "^PASS vkCreateDevice" $work/run.log &&
  echo "$new" | grep -q "errata: cull_nontri bug imitated beneath the fix"
step $? "the bug imitated, no fix: $res"
# 3
run VKBRIDGE_ERRATA_IMITATE=cull_nontri -- cullmode_check
[ $rc = 0 ]
step $? "the bug imitated, the fix over it: $res"
# 4
run VKBRIDGE_MODE=performance VKBRIDGE_FAKE_DRIVER_VERSION=r52p0 -- cullmode_check
r4=$rc; on4=$(on_set)
run VKBRIDGE_MODE=performance VKBRIDGE_FAKE_DRIVER_VERSION=r49p1 -- cullmode_check
[ $r4 = 0 ] && ! has "$on4" cull_nontri && [ $rc = 0 ] && has "$(on_set)" cull_nontri &&
  echo "$new" | grep -q "errata: cull_nontri ON (r49p1 in r0p0-r51 (ANGLE: below r52"
step $? "performance mode: off on r52p0, on on r49p1 ($(echo "$new" | grep -o 'errata: cull_nontri ON ([^)]*' | cut -c1-60)...)"
# 5
run VKBRIDGE_ERRATA_IMITATE=dyn_reemit,cull_nontri -- dynstate_check
r5=$rc res5=$res
run VKBRIDGE_ERRATA_IMITATE=dyn_reemit,cull_nontri -- cullmode_check
[ $r5 = 0 ] && [ $rc = 0 ]
step $? "with dyn_reemit, both imitated: dynstate_check $res5; cullmode_check $res"

# 6
grep -q "^#define VKB_PROTOCOL_HASH 0x4a9da939fdf65896ULL" ../common/gen/vkb_gen.h
step $? "protocol hash $(grep -o '0x[0-9a-f]*' ../common/gen/vkb_gen.h | head -1)"

if [ $fails = 0 ]; then echo "OK: $steps steps"; else echo "FAILED: $fails of $steps steps"; exit 1; fi
