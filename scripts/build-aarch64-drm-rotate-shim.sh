#!/usr/bin/env bash
set -euo pipefail

ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
IMAGE="${MLP1_TOOLCHAIN_IMAGE:-ghcr.io/utility-muffin-research-kitchen/mlp1-toolchain:latest}"
BUILD_DIR="$ROOT/build/mlp1"
OUT_DIR="$BUILD_DIR/compat/drm/aarch64"
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
    bash scripts/build-aarch64-drm-rotate-shim.sh --container
fi

mkdir -p "$OUT_DIR"

libdrm_cflags="$(pkg-config --cflags libdrm 2>/dev/null || true)"
aarch64-buildroot-linux-gnu-gcc -shared -fPIC -O2 -Wall -Wextra -Wpedantic -Wl,--as-needed \
  $libdrm_cflags \
  -o "$OUT_DIR/leaf-drm-rotate.so" "$ROOT/compat/drm/leaf-drm-rotate.c" \
  -ldl -pthread
aarch64-buildroot-linux-gnu-strip "$OUT_DIR/leaf-drm-rotate.so"

cat >"$OUT_DIR/manifest.json" <<'JSON'
{
  "schema": 1,
  "name": "leaf-drm-rotate",
  "arch": "aarch64",
  "source": "compat/drm/leaf-drm-rotate.c",
  "runtime": "librga.so.2",
  "files": [
    "leaf-drm-rotate.so"
  ]
}
JSON

echo "aarch64 DRM rotate shim ready: $OUT_DIR"
