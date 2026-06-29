#!/usr/bin/env bash
set -euo pipefail

data_dir="${PORTMASTER_MLP1_DATA_DIR:-${USERDATA_PATH:-${SDCARD_PATH:-/mnt/sdcard}/.userdata/${PLATFORM:-mlp1}}/portmaster}"

echo "data_dir=$data_dir"
test -d "$data_dir" || { echo "not installed: data dir missing"; exit 1; }
test -d "$data_dir/PortMaster" || { echo "not installed: PortMaster tree missing"; exit 1; }
test -f "$data_dir/.leaf/manifest.json" || { echo "warning: Leaf manifest missing"; exit 0; }
echo "installed manifest: $data_dir/.leaf/manifest.json"

