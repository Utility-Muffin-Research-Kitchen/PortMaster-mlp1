#!/usr/bin/env bash
set -euo pipefail

APP_DIR="$(cd "$(dirname "$0")/.." && pwd)"
WORKSPACE="$(cd "$APP_DIR/.." && pwd)"
IMAGE="${MLP1_TOOLCHAIN_IMAGE:-ghcr.io/utility-muffin-research-kitchen/mlp1-toolchain:latest}"

echo "=== Building PortMaster-mlp1 for MLP1 (workspace: $WORKSPACE) ==="
docker run --rm -v "$WORKSPACE":/workspace -w /workspace/PortMaster-mlp1 "$IMAGE" \
	make -C ports/mlp1

