#!/usr/bin/env bash
set -euo pipefail

ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
LOCK="$ROOT/locks/aarch64-mali.lock.json"
BUILD_DIR="$ROOT/build/mlp1"
WORK_DIR="$ROOT/build/aarch64-mali"
OUT_DIR="$BUILD_DIR/compat/mali/aarch64"

read_lock() {
  python3 - "$LOCK" "$1" <<'PY'
import json
import sys

path, key = sys.argv[1], sys.argv[2]
with open(path, "r", encoding="utf-8") as f:
    data = json.load(f)

value = data
for part in key.split("."):
    value = value[part]
print(value)
PY
}

download_checked() {
  local url="$1"
  local path="$2"
  local expected_size="$3"
  local expected_sha256="$4"

  mkdir -p "$(dirname -- "$path")"
  if [ ! -f "$path" ] || [ "$(wc -c <"$path" | tr -d ' ')" != "$expected_size" ]; then
    local tmp="$path.tmp.$$"
    curl -fL --retry 3 --connect-timeout 20 -o "$tmp" "$url"
    mv "$tmp" "$path"
  fi

  local actual_sha256
  actual_sha256="$(shasum -a 256 "$path" | awk '{print $1}')"
  if [ "$actual_sha256" != "$expected_sha256" ]; then
    echo "sha256 mismatch for $(basename -- "$path")" >&2
    echo "expected: $expected_sha256" >&2
    echo "actual:   $actual_sha256" >&2
    exit 1
  fi
}

repo="$(read_lock release.repo)"
branch="$(read_lock release.branch)"
commit="$(read_lock release.commit)"
variant="$(read_lock name)"
architecture="$(read_lock architecture)"
asset_filename="$(read_lock asset.filename)"
asset_path="$(read_lock asset.path)"
asset_url="$(read_lock asset.url)"
asset_size="$(read_lock asset.size)"
asset_sha256="$(read_lock asset.sha256)"
icd_filename="$(read_lock icd.filename)"
icd_library_path="$(read_lock icd.library_path)"
icd_api_version="$(read_lock icd.api_version)"
license_filename="$(read_lock license.filename)"
license_url="$(read_lock license.url)"
license_size="$(read_lock license.size)"
license_sha256_expected="$(read_lock license.sha256)"
license_installed_path="$(read_lock license.installed_path)"

mkdir -p "$WORK_DIR"
asset="$WORK_DIR/$asset_filename"
license_src="$WORK_DIR/$license_filename"

download_checked "$asset_url" "$asset" "$asset_size" "$asset_sha256"
download_checked "$license_url" "$license_src" "$license_size" "$license_sha256_expected"

rm -rf "$OUT_DIR"
mkdir -p "$OUT_DIR"

cp -f "$asset" "$OUT_DIR/libmali.so.1"
chmod 755 "$OUT_DIR/libmali.so.1"

cat >"$OUT_DIR/$icd_filename" <<EOF
{
  "file_format_version": "1.0.0",
  "ICD": {
    "library_path": "$icd_library_path",
    "api_version": "$icd_api_version"
  }
}
EOF
chmod 644 "$OUT_DIR/$icd_filename"

license_rel="${license_installed_path#LICENSES/}"
license_path="$BUILD_DIR/licenses/$license_rel"
mkdir -p "$(dirname -- "$license_path")"
rm -f "$BUILD_DIR/licenses/mali/libmali-rockchip-debian-copyright"
cp -f "$license_src" "$license_path"
chmod 644 "$license_path"

lib_sha256="$(shasum -a 256 "$OUT_DIR/libmali.so.1" | awk '{print $1}')"
icd_sha256="$(shasum -a 256 "$OUT_DIR/$icd_filename" | awk '{print $1}')"
license_sha256="$(shasum -a 256 "$license_path" | awk '{print $1}')"

cat >"$OUT_DIR/manifest.json" <<EOF
{
  "schema": 1,
  "source": "https://github.com/$repo",
  "branch": "$branch",
  "commit": "$commit",
  "variant": "$variant",
  "architecture": "$architecture",
  "asset": {
    "filename": "$asset_filename",
    "path": "$asset_path",
    "url": "$asset_url",
    "size": $asset_size,
    "sha256": "$asset_sha256"
  },
  "files": [
    {
      "path": "libmali.so.1",
      "source": "$asset_path",
      "sha256": "$lib_sha256"
    },
    {
      "path": "$icd_filename",
      "sha256": "$icd_sha256"
    }
  ],
  "license": {
    "path": "$license_installed_path",
    "url": "$license_url",
    "size": $license_size,
    "sha256": "$license_sha256"
  }
}
EOF

echo "aarch64 Mali g29 compat ready: $OUT_DIR"
