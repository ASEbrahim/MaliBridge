#!/bin/bash
# DXVK releases for devprofile_check.sh into $VKB_DEVPROFILE_CACHE/dxvk (Proton Experimental's own is used in place)
set -e
CACHE=${VKB_DEVPROFILE_CACHE:-$HOME/.cache/malibridge-devprofile}
mkdir -p "$CACHE/dxvk"; cd "$CACHE/dxvk"
for v in ${*:-1.10.3 2.6.2 2.7.1}; do
  [ -f "dxvk-$v/x64/dxgi.dll" ] && continue
  curl -sfL -o "dxvk-$v.tar.gz" "https://github.com/doitsujin/dxvk/releases/download/v$v/dxvk-$v.tar.gz"
  tar xzf "dxvk-$v.tar.gz" && rm "dxvk-$v.tar.gz"
  echo "DXVK $v: $CACHE/dxvk/dxvk-$v"
done
