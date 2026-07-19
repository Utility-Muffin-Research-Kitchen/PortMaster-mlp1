#!/usr/bin/env bash
set -euo pipefail

repo_dir="$(cd "$(dirname "$0")/.." && pwd)"
binary="${SOURCE_TEST_BINARY:-$repo_dir/build/bin/portmaster-mlp1}"
fixture_root="$(mktemp -d "${TMPDIR:-/tmp}/portmaster-sources.XXXXXX")"
fixture_root="$(cd "$fixture_root" && pwd -P)"
trap 'rm -rf "$fixture_root"' EXIT

primary="$fixture_root/primary"
secondary="$fixture_root/secondary"
userdata="$fixture_root/userdata"
mkdir -p "$primary/Roms" "$primary/Images" \
  "$secondary/custom-roms" "$secondary/custom-images" "$userdata"

run_sources() {
  PORTMASTER_MLP1_PAK_DIR="$repo_dir" \
  PLATFORM=mlp1 \
  SDCARD_PATH="$primary" \
  USERDATA_PATH="$userdata" \
  ROMS_PATH="$primary/Roms" \
  IMAGES_PATH="$primary/Images" \
  PORTMASTER_SOURCE_TEST_AVAILABLE="${PORTMASTER_SOURCE_TEST_AVAILABLE:-primary}" \
  PORTMASTER_SOURCE_TEST_FINGERPRINT=cards \
    "$binary" --sources-text
}

output="$(
  SDCARD_PATHS="$primary:$secondary" \
  ROMS_PATHS="$primary/Roms:$secondary/custom-roms" \
  IMAGES_PATHS="$primary/Images:$secondary/custom-images" \
  UMRK_SECONDARY_SDCARD_PATH="$secondary" \
  PORTMASTER_SOURCE_TEST_AVAILABLE=primary,secondary_sd \
    run_sources
)"
grep -F "id=primary configured=yes available=yes root=$primary" <<<"$output" >/dev/null
grep -F "id=secondary_sd configured=yes available=yes root=$secondary" <<<"$output" >/dev/null
grep -F "roms=$secondary/custom-roms images=$secondary/custom-images" <<<"$output" >/dev/null
grep -F "fingerprint=fixture:secondary_sd:cards" <<<"$output" >/dev/null

# Exact mountinfo matching unescapes Linux octal mountpoint fields and captures
# the session mount/device identity. The fixture path keeps this runnable on
# non-Linux development hosts without weakening the device default.
secondary_space="$fixture_root/secondary card"
mkdir -p "$secondary_space/Roms" "$secondary_space/Images"
mountinfo="$fixture_root/mountinfo"
escaped_secondary="${secondary_space// /\\040}"
printf '42 18 179:98 / %s rw - vfat /dev/mmcblk3p1 rw\n' \
  "$escaped_secondary" >"$mountinfo"
mounted="$(
  env -u PORTMASTER_SOURCE_TEST_AVAILABLE \
    PORTMASTER_MLP1_PAK_DIR="$repo_dir" \
    PLATFORM=mlp1 \
    SDCARD_PATH="$primary" \
    USERDATA_PATH="$userdata" \
    ROMS_PATH="$primary/Roms" \
    IMAGES_PATH="$primary/Images" \
    SDCARD_PATHS="$primary:$secondary_space" \
    ROMS_PATHS="$primary/Roms:$secondary_space/Roms" \
    IMAGES_PATHS="$primary/Images:$secondary_space/Images" \
    UMRK_SECONDARY_SDCARD_PATH="$secondary_space" \
    PORTMASTER_SOURCE_TEST_MOUNTINFO="$mountinfo" \
    PORTMASTER_SOURCE_TEST_FINGERPRINT=cards \
      "$binary" --sources-text
)"
grep -F "id=secondary_sd configured=yes available=yes root=$secondary_space" \
  <<<"$mounted" >/dev/null
grep -F "mount_id=42 device=179:98" <<<"$mounted" >/dev/null

# Older wrappers omitted the configured secondary from plural lists. The
# resolver keeps the slot and derives only its missing aligned content paths.
legacy="$(
  SDCARD_PATHS="$primary" \
  ROMS_PATHS="$primary/Roms" \
  IMAGES_PATHS="$primary/Images" \
  UMRK_SECONDARY_SDCARD_PATH="$secondary" \
    run_sources
)"
grep -F "id=secondary_sd configured=yes available=no root=$secondary" <<<"$legacy" >/dev/null
grep -F "roms=$secondary/Roms images=$secondary/Images" <<<"$legacy" >/dev/null

expect_rejected() {
  if "$@" >"$fixture_root/rejected.out" 2>"$fixture_root/rejected.err"; then
    echo "malformed source environment was accepted" >&2
    return 1
  fi
}

expect_rejected env \
  PORTMASTER_MLP1_PAK_DIR="$repo_dir" PLATFORM=mlp1 \
  SDCARD_PATH="$primary" USERDATA_PATH="$userdata" \
  ROMS_PATH="$primary/Roms" IMAGES_PATH="$primary/Images" \
  SDCARD_PATHS="$primary::$secondary" \
  "$binary" --sources-text

expect_rejected env \
  PORTMASTER_MLP1_PAK_DIR="$repo_dir" PLATFORM=mlp1 \
  SDCARD_PATH="$primary" USERDATA_PATH="$userdata" \
  ROMS_PATH="$primary/Roms" IMAGES_PATH="$primary/Images" \
  SDCARD_PATHS="$secondary:$primary" \
  "$binary" --sources-text

expect_rejected env \
  PORTMASTER_MLP1_PAK_DIR="$repo_dir" PLATFORM=mlp1 \
  SDCARD_PATH="$primary" USERDATA_PATH="$userdata" \
  ROMS_PATH="$primary/Roms" IMAGES_PATH="$primary/Images" \
  SDCARD_PATHS="$primary:$primary/." \
  "$binary" --sources-text

expect_rejected env \
  PORTMASTER_MLP1_PAK_DIR="$repo_dir" PLATFORM=mlp1 \
  SDCARD_PATH="$primary" USERDATA_PATH="$userdata" \
  ROMS_PATH="$primary/Roms" IMAGES_PATH="$primary/Images" \
  SDCARD_PATHS="$primary:$secondary" \
  ROMS_PATHS="$primary/Roms" \
  "$binary" --sources-text

echo "PortMaster source fixtures passed"
