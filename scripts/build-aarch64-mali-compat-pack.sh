#!/usr/bin/env bash
set -euo pipefail

ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
LOCK="$ROOT/locks/aarch64-mali.lock.json"
BUILD_DIR="$ROOT/build/mlp1"
WORK_DIR="$ROOT/build/aarch64-mali"
OUT_DIR="$BUILD_DIR/compat/mali/aarch64"
LICENSE_DIR="$BUILD_DIR/licenses/mali"

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

filename="$(read_lock asset.filename)"
url="$(read_lock asset.url)"
expected_size="$(read_lock asset.size)"
expected_sha256="$(read_lock asset.sha256)"
license_url="$(read_lock license.url)"
commit="$(read_lock release.commit)"
variant="$(read_lock name)"

mkdir -p "$WORK_DIR" "$OUT_DIR" "$LICENSE_DIR"
deb="$WORK_DIR/$filename"

if [ ! -f "$deb" ] || [ "$(wc -c <"$deb" | tr -d ' ')" != "$expected_size" ]; then
  tmp="$deb.tmp.$$"
  curl -fL --retry 3 --connect-timeout 20 -o "$tmp" "$url"
  mv "$tmp" "$deb"
fi

actual_sha256="$(shasum -a 256 "$deb" | awk '{print $1}')"
if [ "$actual_sha256" != "$expected_sha256" ]; then
  echo "sha256 mismatch for $filename" >&2
  echo "expected: $expected_sha256" >&2
  echo "actual:   $actual_sha256" >&2
  exit 1
fi

extract="$WORK_DIR/extract"
rm -rf "$extract"
mkdir -p "$extract"
(cd "$extract" && ar x "$deb")
data_tar="$(find "$extract" -maxdepth 1 -name 'data.tar.*' -print -quit)"
test -n "$data_tar" || { echo "deb missing data.tar payload" >&2; exit 1; }
(cd "$extract" && tar -xf "$data_tar")

src_lib="$extract/usr/lib/aarch64-linux-gnu"
test -f "$src_lib/libmali.so.1.9.0" || { echo "missing libmali.so.1.9.0 in deb" >&2; exit 1; }
test -f "$src_lib/libmali-hook.so.1.9.0" || { echo "missing libmali-hook.so.1.9.0 in deb" >&2; exit 1; }

cp -f "$src_lib/libmali.so.1.9.0" "$OUT_DIR/libmali.so.1"
cp -f "$src_lib/libmali-hook.so.1.9.0" "$OUT_DIR/libmali-hook.so.1"
chmod 755 "$OUT_DIR/libmali.so.1" "$OUT_DIR/libmali-hook.so.1"

license_path="$LICENSE_DIR/libmali-rockchip-debian-copyright"
curl -fL --retry 3 --connect-timeout 20 -o "$license_path.tmp.$$" "$license_url"
mv "$license_path.tmp.$$" "$license_path"

lib_sha256="$(shasum -a 256 "$OUT_DIR/libmali.so.1" | awk '{print $1}')"
hook_sha256="$(shasum -a 256 "$OUT_DIR/libmali-hook.so.1" | awk '{print $1}')"
license_sha256="$(shasum -a 256 "$license_path" | awk '{print $1}')"

cat >"$OUT_DIR/manifest.json" <<EOF
{
  "schema": 1,
  "source": "https://github.com/tsukumijima/libmali-rockchip",
  "commit": "$commit",
  "variant": "$variant",
  "architecture": "arm64",
  "deb": {
    "filename": "$filename",
    "url": "$url",
    "size": $expected_size,
    "sha256": "$expected_sha256"
  },
  "files": [
    {
      "path": "libmali.so.1",
      "sha256": "$lib_sha256"
    },
    {
      "path": "libmali-hook.so.1",
      "sha256": "$hook_sha256"
    }
  ],
  "license": {
    "path": "LICENSES/mali/libmali-rockchip-debian-copyright",
    "sha256": "$license_sha256"
  }
}
EOF

echo "aarch64 Mali compat ready: $OUT_DIR"
