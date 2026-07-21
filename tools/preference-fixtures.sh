#!/usr/bin/env bash
set -euo pipefail

repo_dir="$(cd "$(dirname "$0")/.." && pwd)"
binary="${PREFERENCE_TEST_BINARY:-$repo_dir/build/bin/portmaster-mlp1}"
fixture_root="$(mktemp -d "${TMPDIR:-/tmp}/portmaster-preferences.XXXXXX")"
fixture_root="$(cd "$fixture_root" && pwd -P)"
trap 'rm -rf "$fixture_root"' EXIT

primary="$fixture_root/primary"
secondary="$fixture_root/secondary"
userdata="$fixture_root/userdata"
mkdir -p "$primary/Roms" "$primary/Images" \
  "$secondary/Roms" "$secondary/Images" "$userdata"

run_binary() {
  PORTMASTER_MLP1_PAK_DIR="$repo_dir" \
  PLATFORM=mlp1 \
  SDCARD_PATH="$primary" \
  USERDATA_PATH="$userdata" \
  ROMS_PATH="$primary/Roms" \
  IMAGES_PATH="$primary/Images" \
  SDCARD_PATHS="$primary:$secondary" \
  ROMS_PATHS="$primary/Roms:$secondary/Roms" \
  IMAGES_PATHS="$primary/Images:$secondary/Images" \
  UMRK_SECONDARY_SDCARD_PATH="$secondary" \
  PORTMASTER_SOURCE_TEST_FINGERPRINT=preferences \
    "$binary" "$@"
}

default="$(
  PORTMASTER_SOURCE_TEST_AVAILABLE=primary \
    run_binary --install-source
)"
grep -F "preferred=primary effective=primary available=yes" \
  <<<"$default" >/dev/null

PORTMASTER_SOURCE_TEST_AVAILABLE=primary,secondary_sd \
  run_binary --set-install-source secondary_sd >/dev/null
preference="$userdata/portmaster/.leaf/preferences.json"
test "$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["install_source"])' \
  "$preference")" = "secondary_sd"

secondary_selected="$(
  PORTMASTER_SOURCE_TEST_AVAILABLE=primary,secondary_sd \
    run_binary --install-source
)"
grep -F "preferred=secondary_sd effective=secondary_sd available=yes root=$secondary" \
  <<<"$secondary_selected" >/dev/null

if PORTMASTER_SOURCE_TEST_AVAILABLE=primary \
  run_binary --install-source >"$fixture_root/missing.out" 2>"$fixture_root/missing.err"; then
  echo "missing preferred secondary was silently accepted" >&2
  exit 1
fi
grep -F "Secondary SD is preferred but is not mounted" \
  "$fixture_root/missing.err" >/dev/null
test "$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["install_source"])' \
  "$preference")" = "secondary_sd"

if PORTMASTER_SOURCE_TEST_AVAILABLE=primary \
  run_binary --set-install-source invalid >"$fixture_root/invalid.out" 2>"$fixture_root/invalid.err"; then
  echo "invalid install source was accepted" >&2
  exit 1
fi
grep -F "unknown install source" "$fixture_root/invalid.err" >/dev/null

echo "install-source preference fixtures: PASS"
