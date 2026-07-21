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

scratch="$(mktemp -d "${TMPDIR:-/tmp}/portmaster-patch-check.XXXXXX")"
trap 'rm -rf "$scratch"' EXIT HUP INT TERM
cp -R "$TREE/." "$scratch/"
TREE="$scratch"

if [ -f "$TREE/pylibs.zip" ]; then
  echo "prepare: extracting pylibs.zip"
  rm -rf "$TREE/pylibs"
  unzip -q -o "$TREE/pylibs.zip" -d "$TREE"
fi

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
    0003-leaf-preserve-pysdl2-env.patch)
      if grep -q 'PYSDL2_DLL_PATH:-' "$TREE/PortMaster.sh"; then
        echo "already applied"
        continue
      fi
      ;;
    0004-leaf-harbourmaster-device.patch)
      if [ -f "$TREE/pylibs/harbourmaster/hardware.py" ] &&
        grep -q 'leaf-mlp1' "$TREE/pylibs/harbourmaster/hardware.py"; then
        echo "already applied"
        continue
      fi
      ;;
    0008-leaf-quote-esudo-check.patch)
      if grep -q 'ESUDO:-' "$TREE/control.txt"; then
        echo "already applied"
        continue
      fi
      ;;
    0009-leaf-multi-source-inventory.patch)
      if [ -f "$TREE/pylibs/harbourmaster/leaf_sources.py" ] &&
        grep -q 'LEAF_PM_SELECTED_SOURCE_ID' \
          "$TREE/pylibs/harbourmaster/leaf_sources.py"; then
        echo "already applied"
        continue
      fi
      ;;
    0010-leaf-ignore-move-staging.patch)
      if grep -q 'leaf_reserved' \
        "$TREE/pylibs/harbourmaster/harbour.py"; then
        echo "already applied"
        continue
      fi
      ;;
  esac
  (cd "$TREE" && patch -p0 -i "$patch" >/dev/null)
done
