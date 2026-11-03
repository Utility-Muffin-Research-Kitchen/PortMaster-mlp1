#!/usr/bin/env bash
set -euo pipefail

ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
IMAGE="${MLP1_TOOLCHAIN_IMAGE:-ghcr.io/utility-muffin-research-kitchen/mlp1-toolchain:latest}"
BUILD_DIR="$ROOT/build/mlp1"
OUT_DIR="$BUILD_DIR/compat/sdl2/aarch64"
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
    bash scripts/build-aarch64-sdl2-fullscreen-shim.sh --container
fi

mkdir -p "$OUT_DIR"

aarch64-buildroot-linux-gnu-gcc -shared -fPIC -O2 -Wall -Wextra -Wpedantic -Wl,--as-needed \
  -o "$OUT_DIR/leaf-sdl2-fullscreen.so" "$ROOT/compat/sdl2/leaf-sdl2-fullscreen.c" \
  -ldl
aarch64-buildroot-linux-gnu-strip "$OUT_DIR/leaf-sdl2-fullscreen.so"

cat >"$OUT_DIR/manifest.json" <<'JSON'
{
  "schema": 1,
  "name": "leaf-sdl2-fullscreen",
  "arch": "aarch64",
  "source": "compat/sdl2/leaf-sdl2-fullscreen.c",
  "files": [
    "leaf-sdl2-fullscreen.so"
  ]
}
JSON

echo "aarch64 SDL2 fullscreen shim ready: $OUT_DIR"
