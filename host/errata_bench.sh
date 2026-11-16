#!/bin/bash
# dyn_reemit's cost (MK-04, server_errata_cmd.c), shown, not judged: dynstate_check bench (two
# pipelines with DXVK's dynamic state list, binds alternating, one draw each) through a server of
# its own per arm, ABAB, and the server's CPU time (utime+stime from /proc) per bind.
#   ./errata_bench.sh [binds] [pairs] [arm=SERVER_ENV ...]
# Default arms: off (VKBRIDGE_ERRATA=-dyn_reemit), on (VKBRIDGE_ERRATA=+dyn_reemit). An arm is
# name=VAR=value[|VAR=value]. Driver: VKB_HOST_ICD (lavapipe in CI); profile VKB_MALI_PROFILE (default off:
# the bench needs Vulkan 1.3).
cd "$(dirname "$0")"
binds=${1:-20000}; pairs=${2:-3}; shift 2 2>/dev/null
arms=("$@"); [ ${#arms[@]} = 0 ] && arms=("off=VKBRIDGE_ERRATA=-dyn_reemit" "on=VKBRIDGE_ERRATA=+dyn_reemit")
VKB_RUN_DIR=$(mktemp -d "${XDG_RUNTIME_DIR:-/tmp}/vkbridge-ebench-$$.XXXX")
export VKBRIDGE_SOCKET=$VKB_RUN_DIR/server.sock VKB_SERVER_LOG=$VKB_RUN_DIR/server.log
unset VKBRIDGE_CACHE_DIR VKB_SERVER_PID
. ./server_start.sh
VKB_MALI_PROFILE=${VKB_MALI_PROFILE:-off} . ./mali_profile.sh
trap 'vkb_server_stop; rm -rf "$VKB_RUN_DIR"' EXIT
cpu() { awk '{print $14+$15}' /proc/$1/stat; }
tck=$(getconf CLK_TCK)
for p in $(seq "$pairs"); do
  for a in "${arms[@]}"; do
    name=${a%%=*}; envs=${a#*=}
    IFS='|' read -r -a ev <<< "$envs"
    : > "$VKB_SERVER_LOG"
    vkb_server_start "${ev[@]}"
    timeout 300 ./out/dynstate_check bench 200 1 >/dev/null 2>&1   # warm: probes, pipeline cache
    c0=$(cpu $VKB_SERVER_PID)
    line=$(timeout 300 ./out/dynstate_check bench "$binds" 10 | grep '^bench:')
    c1=$(cpu $VKB_SERVER_PID)
    on=$(grep -o 'errata: dyn_reemit ON[^-]*' "$VKB_SERVER_LOG" | tail -1 | cut -c1-120); [ -n "$on" ] || on="dyn_reemit off"
    awk -v n="$name" -v l="$line" -v c=$((c1-c0)) -v t="$tck" -v b="$binds" -v o="$on" \
      'BEGIN { printf "%-6s pair %s: %s; server CPU %.0f ns per bind [%s]\n", n, "'"$p"'", l, c / t * 1e9 / (b * 10), o }'
    vkb_server_stop
  done
done
