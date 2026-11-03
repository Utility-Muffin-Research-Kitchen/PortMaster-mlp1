#!/usr/bin/env bash
set -euo pipefail

# Builds the 32-bit copy of compat/sdl2/leaf-sdl-hints.c. The runtime hook
# preloads compat/sdl2/$PLATFORM/leaf-sdl-hints.so with a literal $PLATFORM,
# which the dynamic loader expands per process: "aarch64" for 64-bit programs
# and "v8l" for 32-bit ones on MLP1's arm64 kernel. Shipping both keeps armhf
# launches from logging a "wrong ELF class" preload error.

ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
IMAGE="${PM_ARMHF_BUILD_IMAGE:-debian:bookworm-slim}"
BUILD_DIR="$ROOT/build/mlp1"
OUT_DIR="$BUILD_DIR/compat/sdl2/v8l"
SOURCE="$ROOT/compat/sdl2/leaf-sdl-hints.c"
container=0

if [[ "${1:-}" == "--container" ]]; then
  container=1
  shift
fi

if [[ "$container" == "0" ]]; then
  if [[ -f "$OUT_DIR/leaf-sdl-hints.so" && "$OUT_DIR/leaf-sdl-hints.so" -nt "$SOURCE" &&
        "$OUT_DIR/leaf-sdl-hints.so" -nt "$0" ]]; then
    echo "armhf SDL hint shim up to date: $OUT_DIR"
    exit 0
  fi
  exec docker run --rm \
    -v "$ROOT":/work \
    -w /work \
    "$IMAGE" \
    bash scripts/build-armhf-sdl-hints-shim.sh --container
fi

export DEBIAN_FRONTEND=noninteractive
apt-get update >/dev/null
apt-get install -y --no-install-recommends gcc-arm-linux-gnueabihf libc6-dev-armhf-cross >/dev/null

mkdir -p "$OUT_DIR"

arm-linux-gnueabihf-gcc -shared -fPIC -O2 -Wall -Wextra -Wpedantic -Wl,--as-needed \
  -o "$OUT_DIR/leaf-sdl-hints.so" "$SOURCE" \
  -ldl
arm-linux-gnueabihf-strip "$OUT_DIR/leaf-sdl-hints.so"

cat >"$OUT_DIR/manifest.json" <<'JSON'
{
  "schema": 1,
  "name": "leaf-sdl-hints",
  "arch": "armhf",
  "source": "compat/sdl2/leaf-sdl-hints.c",
  "files": [
    "leaf-sdl-hints.so"
  ]
}
JSON

echo "armhf SDL hint shim ready: $OUT_DIR"
