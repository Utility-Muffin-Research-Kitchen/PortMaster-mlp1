#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TMP_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/portmaster-godot-hook.XXXXXX")"
trap 'rm -rf "$TMP_ROOT"' EXIT

mkdir -p "$TMP_ROOT/userdata/portmaster/PortMaster"
SDCARD_PATH="$TMP_ROOT" \
USERDATA_PATH="$TMP_ROOT/userdata" \
PORTMASTER_MLP1_DATA_DIR="$TMP_ROOT/userdata/portmaster" \
PORTMASTER_CONTROLFOLDER="$TMP_ROOT/userdata/portmaster/PortMaster" \
  "$ROOT/scripts/write-leaf-runtime-hook.sh"

HOOK="$TMP_ROOT/userdata/portmaster/PortMaster/leaf-armhf-env.sh"
# shellcheck disable=SC1090
source "$HOOK"

unset LEAF_EGL_DRAW_FINISH LEAF_PM_GODOT_DRAW_FINISH
leaf_pm_prepare_godot_runtime_env /tmp/frt_3.5.2
test "$LEAF_EGL_DRAW_FINISH" = 0

unset LEAF_EGL_DRAW_FINISH LEAF_PM_GODOT_DRAW_FINISH
leaf_pm_prepare_godot_runtime_env /tmp/godot43.aarch64
test "$LEAF_EGL_DRAW_FINISH" = "fence:2"

unset LEAF_EGL_DRAW_FINISH
LEAF_PM_GODOT_DRAW_FINISH=1 leaf_pm_prepare_godot_runtime_env /tmp/godot43.aarch64
test "$LEAF_EGL_DRAW_FINISH" = 1

unset LEAF_EGL_DRAW_FINISH
LEAF_PM_GODOT_DRAW_FINISH=0 leaf_pm_prepare_godot_runtime_env /tmp/godot43.aarch64
test "$LEAF_EGL_DRAW_FINISH" = 0

TEST_GPU_DIR="$TMP_ROOT/gpu"
mkdir -p "$TEST_GPU_DIR"
printf '%s\n' '200000000 600000000 800000000' > "$TEST_GPU_DIR/available_frequencies"
printf '%s\n' 200000000 > "$TEST_GPU_DIR/min_freq"
export TEST_GPU_DIR
leaf_pm_godot_gpu_dir() { printf '%s\n' "$TEST_GPU_DIR"; }
caller_traps="$(trap -p EXIT HUP INT TERM)"
unset LEAF_PM_SKIP_WESTONPACK_CLEANUP
# TEST_GPU_DIR is intentionally expanded by the child shell.
# shellcheck disable=SC2016
LEAF_PM_GODOT_GPU_MIN_FREQ=600000000 \
  leaf_pm_run_godot_sdl2_runtime /bin/sh -c \
    'test "$(tr -d "\n" < "$TEST_GPU_DIR/min_freq")" = 600000000' \
    --windowed
test "$(tr -d '\n' < "$TEST_GPU_DIR/min_freq")" = 200000000

# TEST_GPU_DIR is intentionally expanded by the child shell.
# shellcheck disable=SC2016
DEVICE_ARCH=armhf LEAF_PM_GODOT_GPU_MIN_FREQ=600000000 \
  leaf_pm_run_godot_sdl2_runtime /bin/sh -c \
    'test "$(tr -d "\n" < "$TEST_GPU_DIR/min_freq")" = 200000000' \
    --windowed
test "$(tr -d '\n' < "$TEST_GPU_DIR/min_freq")" = 200000000
test "$(trap -p EXIT HUP INT TERM)" = "$caller_traps"
test "$LEAF_PM_SKIP_WESTONPACK_CLEANUP" = 1

if grep -F 'compat/mali-gl' "$HOOK" >/dev/null; then
  echo "Godot hook unexpectedly references the removed Mali GL bundle" >&2
  exit 1
fi
echo "PortMaster Godot hook fixtures passed"
