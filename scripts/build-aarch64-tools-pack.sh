#!/usr/bin/env bash
set -euo pipefail

ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
LOCK="$ROOT/locks/aarch64-tools.lock.json"
IMAGE="${MLP1_TOOLCHAIN_IMAGE:-ghcr.io/utility-muffin-research-kitchen/mlp1-toolchain:latest}"
BUILD_DIR="$ROOT/build/mlp1"
WORK_DIR="$ROOT/build/aarch64-tools"
OUT_DIR="$BUILD_DIR/compat/tools/aarch64"
LICENSE_DIR="$BUILD_DIR/licenses/rsync"
container=0

if [[ "${1:-}" == "--container" ]]; then
  container=1
  shift
fi

if [[ "$container" == "0" ]]; then
  exec docker run --rm \
    -v "$ROOT":/work \
    -w /work \
    "$IMAGE" \
    bash scripts/build-aarch64-tools-pack.sh --container
fi

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
if isinstance(value, list):
    print("\n".join(value))
else:
    print(value)
PY
}

download_source() {
  local name="$1"
  local url="$2"
  local expected_size="$3"
  local expected_sha256="$4"
  local tarball="$WORK_DIR/downloads/$name"

  if [ ! -f "$tarball" ] || [ "$(wc -c <"$tarball" | tr -d ' ')" != "$expected_size" ]; then
    local tmp="$tarball.tmp.$$"
    curl -fL --retry 3 --connect-timeout 20 -o "$tmp" "$url"
    mv "$tmp" "$tarball"
  fi

  local actual_sha256
  actual_sha256="$(shasum -a 256 "$tarball" | awk '{print $1}')"
  if [ "$actual_sha256" != "$expected_sha256" ]; then
    echo "sha256 mismatch for $name" >&2
    echo "expected: $expected_sha256" >&2
    echo "actual:   $actual_sha256" >&2
    exit 1
  fi

  printf '%s\n' "$tarball"
}

mkdir -p "$WORK_DIR/downloads" "$OUT_DIR/bin" "$LICENSE_DIR"

container_work="$(mktemp -d /tmp/leaf-aarch64-tools.XXXXXX)"
trap 'rm -rf "$container_work"' EXIT

rsync_version="$(read_lock tools.rsync.version)"
rsync_url="$(read_lock tools.rsync.source.url)"
rsync_expected_size="$(read_lock tools.rsync.source.size)"
rsync_expected_sha256="$(read_lock tools.rsync.source.sha256)"
rsync_tarball="$(download_source "rsync-$rsync_version.tar.gz" "$rsync_url" "$rsync_expected_size" "$rsync_expected_sha256")"
rsync_src_dir="$container_work/rsync-$rsync_version"
mkdir -p "$rsync_src_dir"
tar -xf "$rsync_tarball" -C "$rsync_src_dir" --strip-components=1
(
  cd "$rsync_src_dir"
  ./configure \
    --host=aarch64-buildroot-linux-gnu \
    --prefix=/usr \
    --disable-md2man \
    --disable-ipv6 \
    --disable-openssl \
    --disable-xxhash \
    --disable-zstd \
    --disable-lz4 \
    --disable-acl-support \
    --disable-xattr-support \
    --disable-iconv \
    --with-included-popt \
    --with-included-zlib \
    CC=aarch64-buildroot-linux-gnu-gcc \
    CFLAGS="-Os"
  make -j"$(nproc)" rsync
  aarch64-buildroot-linux-gnu-strip rsync
)

cp -f "$rsync_src_dir/rsync" "$OUT_DIR/bin/rsync"
chmod 755 "$OUT_DIR/bin/rsync"
cp -f "$rsync_src_dir/COPYING" "$LICENSE_DIR/COPYING"

zip_version="$(read_lock tools.zip.version)"
zip_url="$(read_lock tools.zip.source.url)"
zip_expected_size="$(read_lock tools.zip.source.size)"
zip_expected_sha256="$(read_lock tools.zip.source.sha256)"
zip_tarball="$(download_source "zip30.tar.gz" "$zip_url" "$zip_expected_size" "$zip_expected_sha256")"
zip_src_dir="$container_work/zip-$zip_version"
mkdir -p "$zip_src_dir"
tar -xf "$zip_tarball" -C "$zip_src_dir" --strip-components=1
(
  cd "$zip_src_dir"
  make -f unix/Makefile generic CC=aarch64-buildroot-linux-gnu-gcc
  aarch64-buildroot-linux-gnu-strip zip
)

cp -f "$zip_src_dir/zip" "$OUT_DIR/bin/zip"
chmod 755 "$OUT_DIR/bin/zip"
mkdir -p "$BUILD_DIR/licenses/zip"
cp -f "$zip_src_dir/LICENSE" "$BUILD_DIR/licenses/zip/LICENSE"

rsync_binary_sha256="$(shasum -a 256 "$OUT_DIR/bin/rsync" | awk '{print $1}')"
rsync_binary_size="$(wc -c <"$OUT_DIR/bin/rsync" | tr -d ' ')"
rsync_license_sha256="$(shasum -a 256 "$LICENSE_DIR/COPYING" | awk '{print $1}')"
zip_binary_sha256="$(shasum -a 256 "$OUT_DIR/bin/zip" | awk '{print $1}')"
zip_binary_size="$(wc -c <"$OUT_DIR/bin/zip" | tr -d ' ')"
zip_license_sha256="$(shasum -a 256 "$BUILD_DIR/licenses/zip/LICENSE" | awk '{print $1}')"

cat >"$OUT_DIR/manifest.json" <<EOF
{
  "schema": 1,
  "kind": "source-built-native-tools",
  "architecture": "aarch64",
  "tools": [
    {
      "name": "rsync",
      "version": "$rsync_version",
      "path": "bin/rsync",
      "size": $rsync_binary_size,
      "sha256": "$rsync_binary_sha256",
      "source": {
        "url": "$rsync_url",
        "size": $rsync_expected_size,
        "sha256": "$rsync_expected_sha256"
      },
      "license": {
        "spdx": "GPL-3.0-or-later",
        "path": "LICENSES/rsync/COPYING",
        "sha256": "$rsync_license_sha256"
      }
    },
    {
      "name": "zip",
      "version": "$zip_version",
      "path": "bin/zip",
      "size": $zip_binary_size,
      "sha256": "$zip_binary_sha256",
      "source": {
        "url": "$zip_url",
        "size": $zip_expected_size,
        "sha256": "$zip_expected_sha256"
      },
      "license": {
        "spdx": "Info-ZIP",
        "path": "LICENSES/zip/LICENSE",
        "sha256": "$zip_license_sha256"
      }
    }
  ]
}
EOF

echo "aarch64 native tools ready: $OUT_DIR"
