#!/bin/bash
# Checks that a built bridge library still carries the diagnostics that answer "where did the frame go":
#   scripts/check-instrumentation.sh [--server] <elf>...
# Every file must carry the VKBRIDGE_LATLOG and VKBRIDGE_STATS switches; with --server it must also
# carry the latency log's own header line, so that what is checked is the log (server_latency.c) and
# not just a table naming the switch.
# Both are compiled in unconditionally, but they only help if the binary that reaches the device has
# them. A build from an older checkout or a package built from the wrong source directory ships without
# them silently: the knobs are set, nothing is logged, and no measurement can be taken (the CMF's
# v0.5.5.3 server, 2026-11-20: fence wait 2.60 ms and render_scene 3.39 ms of a 31.8 ms frame, with no
# per-frame log to say what the rest of the frame was doing).
# Reads the ELF's string tables, so it runs before or after strip: the literals stay either way.
set -euo pipefail

server=0
if [ "${1:-}" = --server ]; then server=1; shift; fi
[ $# -gt 0 ] || { echo "usage: $0 [--server] <elf>..." >&2; exit 2; }

fail=0
for f in "$@"; do
    [ -f "$f" ] || { echo "FAIL: $f: no such file" >&2; fail=1; continue; }
    readelf -h "$f" >/dev/null 2>&1 || { echo "FAIL: $f: not an ELF file" >&2; fail=1; continue; }
    missing=
    grep -aq 'VKBRIDGE_LATLOG' "$f" || missing="$missing VKBRIDGE_LATLOG"
    grep -aq 'VKBRIDGE_STATS' "$f" || missing="$missing VKBRIDGE_STATS"
    [ "$server" = 0 ] || grep -aq 'vkbridge latency log' "$f" || missing="$missing latency-log-writer"
    if [ -n "$missing" ]; then
        echo "FAIL: $f has no:$missing -- built without the per-frame instrumentation?" >&2
        fail=1
    else
        echo "ok: $f carries$( [ "$server" = 0 ] || echo ' the latency log and') VKBRIDGE_LATLOG VKBRIDGE_STATS"
    fi
done
exit $fail
