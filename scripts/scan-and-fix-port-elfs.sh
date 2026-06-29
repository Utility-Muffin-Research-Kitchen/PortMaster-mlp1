#!/usr/bin/env bash
set -euo pipefail

ports_dir="${1:-${ROMS_PATH:-${SDCARD_PATH:-/mnt/sdcard}/Roms}/PORTS}"
test -d "$ports_dir" || { echo "ports dir missing: $ports_dir" >&2; exit 1; }

find "$ports_dir" -type f -perm -111 -print0 | while IFS= read -r -d '' file; do
  if file "$file" 2>/dev/null | grep -q 'ELF 32-bit.*ARM'; then
    echo "armhf candidate: $file"
  fi
done

