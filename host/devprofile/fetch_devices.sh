#!/bin/bash
# The profiles of devices.txt into <dir> (default $VKB_DEVPROFILE_CACHE/profiles)
set -e
here=$(cd "$(dirname "$0")" && pwd)
dir=${1:-${VKB_DEVPROFILE_CACHE:-$HOME/.cache/malibridge-devprofile}/profiles}
mkdir -p "$dir"
grep -v '^#' "$here/devices.txt" | while read -r n id g; do
  [ -n "$n" ] || continue
  [ -s "$dir/$n.json" ] || python3 -I "$here/fetch_gpuinfo.py" "$id" "$dir/$n.json" --name "$g"
done
