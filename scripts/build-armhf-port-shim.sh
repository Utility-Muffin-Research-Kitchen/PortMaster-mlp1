#!/usr/bin/env bash
set -euo pipefail

# Builds the 32-bit copy of leaf-port-shim.so from compat/port/*.c. The runtime
# hook preloads compat/port/$PLATFORM/leaf-port-shim.so with a literal
# $PLATFORM, which the dynamic loader expands per process: "aarch64" for 64-bit
# programs and "v8l" for 32-bit ones on MLP1's arm64 kernel. Shipping both keeps
# armhf launches from logging a "wrong ELF class" preload error.

ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
IMAGE="${PM_ARMHF_BUILD_IMAGE:-debian:bookworm-slim}"
BUILD_DIR="$ROOT/build/mlp1"
OUT_DIR="$BUILD_DIR/compat/port/v8l"
SOURCES=("$ROOT/compat/port/sdl-hints.c" "$ROOT/compat/port/pwm-guard.c")
container=0

if [[ "${1:-}" == "--container" ]]; then
  container=1
  shift
fi

if [[ "$container" == "0" ]]; then
  up_to_date=0
  if [[ -f "$OUT_DIR/leaf-port-shim.so" && "$OUT_DIR/leaf-port-shim.so" -nt "$0" ]]; then
    up_to_date=1
    for source in "${SOURCES[@]}"; do
      [[ "$OUT_DIR/leaf-port-shim.so" -nt "$source" ]] || up_to_date=0
    done
  fi
  if [[ "$up_to_date" == "1" ]]; then
    echo "armhf port shim up to date: $OUT_DIR"
    exit 0
  fi
  exec docker run --rm \
    -v "$ROOT":/work \
    -w /work \
    "$IMAGE" \
    bash scripts/build-armhf-port-shim.sh --container
fi

export DEBIAN_FRONTEND=noninteractive
apt-get update >/dev/null
apt-get install -y --no-install-recommends gcc-arm-linux-gnueabihf libc6-dev-armhf-cross >/dev/null

mkdir -p "$OUT_DIR"

arm-linux-gnueabihf-gcc -shared -fPIC -O2 -Wall -Wextra -Wpedantic -Wl,--as-needed \
  -o "$OUT_DIR/leaf-port-shim.so" "${SOURCES[@]}" \
  -ldl
arm-linux-gnueabihf-strip "$OUT_DIR/leaf-port-shim.so"

cat >"$OUT_DIR/manifest.json" <<'JSON'
{
  "schema": 1,
  "name": "leaf-port-shim",
  "arch": "armhf",
  "sources": [
    "compat/port/sdl-hints.c",
    "compat/port/pwm-guard.c"
  ],
  "files": [
    "leaf-port-shim.so"
  ]
}
JSON

echo "armhf port shim ready: $OUT_DIR"
