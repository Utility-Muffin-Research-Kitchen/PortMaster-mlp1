#!/usr/bin/env bash
set -euo pipefail

ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
IMAGE="${MLP1_TOOLCHAIN_IMAGE:-ghcr.io/utility-muffin-research-kitchen/mlp1-toolchain:latest}"
BUILD_DIR="$ROOT/build/mlp1"
OUT_DIR="$BUILD_DIR/compat/port/aarch64"
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
    bash scripts/build-aarch64-port-shim.sh --container
fi

mkdir -p "$OUT_DIR"

aarch64-buildroot-linux-gnu-gcc -shared -fPIC -O2 -Wall -Wextra -Wpedantic -Wl,--as-needed \
  -o "$OUT_DIR/leaf-port-shim.so" "$ROOT/compat/port/sdl-hints.c" "$ROOT/compat/port/pwm-guard.c" \
  -ldl
aarch64-buildroot-linux-gnu-strip "$OUT_DIR/leaf-port-shim.so"

cat >"$OUT_DIR/manifest.json" <<'JSON'
{
  "schema": 1,
  "name": "leaf-port-shim",
  "arch": "aarch64",
  "sources": [
    "compat/port/sdl-hints.c",
    "compat/port/pwm-guard.c"
  ],
  "files": [
    "leaf-port-shim.so"
  ]
}
JSON

echo "aarch64 port shim ready: $OUT_DIR"
