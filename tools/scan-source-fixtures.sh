#!/usr/bin/env bash
set -euo pipefail

repo_dir="$(cd "$(dirname "$0")/.." && pwd)"
binary="${SCAN_SOURCE_TEST_BINARY:-$repo_dir/build/bin/portmaster-mlp1}"
fixture_root="$(mktemp -d "${TMPDIR:-/tmp}/portmaster-scan-sources.XXXXXX")"
fixture_root="$(cd "$fixture_root" && pwd -P)"
trap 'rm -rf "$fixture_root"' EXIT

primary="$fixture_root/primary"
secondary="$fixture_root/secondary"
userdata="$fixture_root/userdata"
fake_pak="$fixture_root/pak"
scan_log="$fixture_root/scans.log"
mkdir -p \
  "$primary/Roms/PORTS/PrimaryPort" "$primary/Images" \
  "$secondary/Roms/PORTS/SecondaryPort" "$secondary/Images" \
  "$userdata" "$fake_pak/scripts"
cp "$repo_dir/tools/fixtures/fake-scan-and-fix-port-elfs.sh" \
  "$fake_pak/scripts/scan-and-fix-port-elfs.sh"
chmod +x "$fake_pak/scripts/scan-and-fix-port-elfs.sh"

run_scan() {
  PORTMASTER_MLP1_PAK_DIR="$fake_pak" \
  PLATFORM=mlp1 \
  SDCARD_PATH="$primary" \
  USERDATA_PATH="$userdata" \
  ROMS_PATH="$primary/Roms" \
  IMAGES_PATH="$primary/Images" \
  SDCARD_PATHS="$primary:$secondary" \
  ROMS_PATHS="$primary/Roms:$secondary/Roms" \
  IMAGES_PATHS="$primary/Images:$secondary/Images" \
  UMRK_SECONDARY_SDCARD_PATH="$secondary" \
  PORTMASTER_SOURCE_TEST_AVAILABLE=primary,secondary_sd \
  PORTMASTER_SOURCE_TEST_FINGERPRINT=scan-fixture \
  LEAF_PM_SCAN_FIXTURE_LOG="$scan_log" \
    "$binary" --refresh-stale-port-wrappers
}

first="$(run_scan)"
grep -F "refreshed: 2 source(s)" <<<"$first" >/dev/null
[ "$(wc -l <"$scan_log" | tr -d ' ')" = "2" ]
[ -f "$userdata/portmaster/.leaf/armhf-scan.primary.manifest" ]
[ -f "$userdata/portmaster/.leaf/armhf-scan.secondary_sd.manifest" ]
[ -f "$userdata/portmaster/.leaf/port-tree.primary.stamp" ]
[ -f "$userdata/portmaster/.leaf/port-tree.secondary_sd.stamp" ]

: >"$scan_log"
mkdir -p "$secondary/Roms/PORTS/NewSecondaryPort"
second="$(run_scan)"
grep -F "refreshed: 1 source(s)" <<<"$second" >/dev/null
[ "$(wc -l <"$scan_log" | tr -d ' ')" = "1" ]
grep -Fx "$secondary/Roms/PORTS" "$scan_log" >/dev/null

: >"$scan_log"
third="$(run_scan)"
grep -F "refreshed: 0 source(s)" <<<"$third" >/dev/null
[ ! -s "$scan_log" ]

echo "PortMaster per-source scan fixtures passed"
