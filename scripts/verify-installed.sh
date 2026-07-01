#!/usr/bin/env bash
set -euo pipefail

platform="${PLATFORM:-mlp1}"
script_dir="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
resolver="$script_dir/resolve-sdcard-root.sh"
if [ ! -f "$resolver" ]; then
  echo "SD resolver missing: $resolver" >&2
  exit 1
fi
if ! sdcard_path="$(PLATFORM="$platform" "$resolver" "$script_dir/.." 2>&1)"; then
  echo "SD root error: $sdcard_path" >&2
  exit 1
fi

data_dir="${PORTMASTER_MLP1_DATA_DIR:-${USERDATA_PATH:-$sdcard_path/.userdata/$platform}/portmaster}"

echo "data_dir=$data_dir"
test -d "$data_dir" || { echo "not installed: data dir missing"; exit 1; }
test -d "$data_dir/PortMaster" || { echo "not installed: PortMaster tree missing"; exit 1; }
test -f "$data_dir/.leaf/manifest.json" || { echo "warning: Leaf manifest missing"; exit 0; }
echo "installed manifest: $data_dir/.leaf/manifest.json"
