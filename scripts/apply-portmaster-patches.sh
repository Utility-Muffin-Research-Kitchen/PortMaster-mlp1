#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MODE="${1:-}"
TREE="${2:-}"

if [ "$MODE" != "--dry-run" ] || [ -z "$TREE" ]; then
  echo "usage: scripts/apply-portmaster-patches.sh --dry-run <extracted-portmaster-tree>" >&2
  exit 1
fi

test -d "$TREE" || { echo "missing tree: $TREE" >&2; exit 1; }

shopt -s nullglob
patches=("$ROOT"/patches/portmaster-gui/mlp1/*.patch)
if [ "${#patches[@]}" -eq 0 ]; then
  echo "no MLP1 PortMaster patches yet"
  exit 0
fi

for patch in "${patches[@]}"; do
  echo "dry-run: $(basename "$patch")"
  case "$(basename "$patch")" in
    0001-leaf-controlfolder-env.patch)
      if grep -q 'PORTMASTER_CONTROLFOLDER' "$TREE/control.txt"; then
        echo "already applied"
        continue
      fi
      ;;
    0002-leaf-device-info-env.patch)
      if grep -q 'PORTMASTER_LEAF_DEVICE_INFO' "$TREE/device_info.txt"; then
        echo "already applied"
        continue
      fi
      ;;
  esac
  (cd "$TREE" && patch --dry-run -p0 -i "$patch" >/dev/null)
done
