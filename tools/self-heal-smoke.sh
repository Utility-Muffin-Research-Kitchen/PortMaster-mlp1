#!/bin/bash
set -euo pipefail

PAK_DIR="${1:-build/package/PortMaster.pak}"
PAK_DIR="$(CDPATH= cd -- "$PAK_DIR" && pwd)"
APP="$PAK_DIR/bin/portmaster-mlp1"
SOURCE_LAUNCHER="$PAK_DIR/leaf-platforms/mlp1/emulators/ports/launch.sh"
SOURCE_ICON="$PAK_DIR/res/icon.png"

for required in "$APP" "$SOURCE_LAUNCHER" "$SOURCE_ICON"; do
    test -f "$required" || { echo "missing smoke input: $required" >&2; exit 1; }
done

TMP_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/portmaster-self-heal.XXXXXX")"
trap 'rm -rf "$TMP_ROOT"' EXIT

SD_ROOT="$TMP_ROOT/sd"
PLATFORM_ROOT="$SD_ROOT/.system/leaf/platforms/mlp1"
USERDATA_ROOT="$SD_ROOT/.userdata/mlp1"
LOG_ROOT="$USERDATA_ROOT/logs"
mkdir -p "$PLATFORM_ROOT/defaults" "$USERDATA_ROOT" "$LOG_ROOT"

write_catalog_fixtures() {
    cat >"$PLATFORM_ROOT/defaults/cores.json" <<'JSON'
{
  "version": 2,
  "platform": "mlp1",
  "cores": [
    {
      "id": "fixture",
      "display_name": "Fixture",
      "type": "path",
      "path": "emulators/fixture/launch.sh",
      "platforms": ["mlp1"],
      "status": "missing"
    }
  ]
}
JSON
    cat >"$PLATFORM_ROOT/defaults/systems.json" <<'JSON'
{
  "version": 2,
  "systems": [
    {
      "id": "FIXTURE",
      "name": "Fixture",
      "patterns": ["FIXTURE"],
      "extensions": ["fixture"],
      "archive_extensions": [],
      "archive_inner_extensions": [],
      "archive_mode": "pass_through",
      "file_names": [],
      "ignore_file_names": [],
      "playlist_extensions": [],
      "m3u_generation": "none",
      "default_core": "fixture",
      "alternate_cores": [],
      "rom_root": "Roms/FIXTURE",
      "image_root": "Images/FIXTURE",
      "bios_notes": []
    }
  ]
}
JSON
}

run_manager() {
    local label="$1"
    PLATFORM=mlp1 \
    SDCARD_PATH="$SD_ROOT" \
    USERDATA_PATH="$USERDATA_ROOT" \
    LOGS_PATH="$LOG_ROOT" \
    UMRK_PLATFORM_PATH="$PLATFORM_ROOT" \
    PORTMASTER_MLP1_PAK_DIR="$PAK_DIR" \
        "$APP" --ui-state-text >"$TMP_ROOT/$label.out" 2>"$TMP_ROOT/$label.err"
}

verify_catalog() {
    python3 - "$PLATFORM_ROOT/defaults/cores.json" "$PLATFORM_ROOT/defaults/systems.json" <<'PY'
import json
import sys

cores = json.load(open(sys.argv[1], encoding="utf-8"))["cores"]
systems = json.load(open(sys.argv[2], encoding="utf-8"))["systems"]
ports = [row for row in cores if row.get("id") == "ports"]
assert len(ports) == 1, ports
assert ports[0] == {
    "id": "ports",
    "display_name": "Ports",
    "type": "path",
    "libretro_name": None,
    "file_name": None,
    "config_folder": None,
    "info_name": None,
    "path": "emulators/ports/launch.sh",
    "supports_menu": False,
    "supports_savestate": False,
    "supports_disk_control": False,
    "needs_swap": False,
    "platforms": ["mlp1"],
    "status": "packaged",
}, ports[0]
port_systems = [row for row in systems if row.get("id") == "PORTS"]
assert len(port_systems) == 1, port_systems
assert port_systems[0]["default_core"] == "ports"
assert port_systems[0]["rom_root"] == "Roms/PORTS"
assert port_systems[0]["image_root"] == "Images/PORTS"
PY
}

verify_assets() {
    cmp -s "$SOURCE_LAUNCHER" "$PLATFORM_ROOT/emulators/ports/launch.sh"
    cmp -s "$SOURCE_ICON" "$PLATFORM_ROOT/launcher/res/system_icons/PORTS.png"
    test -x "$PLATFORM_ROOT/emulators/ports/launch.sh"
}

write_catalog_fixtures
run_manager missing
verify_assets
verify_catalog
grep -q 'launcher self-heal applied' "$TMP_ROOT/missing.err"
grep -q 'catalog self-heal applied' "$TMP_ROOT/missing.err"
grep -q 'system icon self-heal applied' "$TMP_ROOT/missing.err"

run_manager correct
verify_assets
verify_catalog
if grep -q 'self-heal applied' "$TMP_ROOT/correct.err"; then
    echo "unchanged integration unexpectedly self-healed" >&2
    sed -n '1,120p' "$TMP_ROOT/correct.err" >&2
    exit 1
fi

printf '%s\n' 'stale launcher' >"$PLATFORM_ROOT/emulators/ports/launch.sh"
printf '%s\n' 'stale icon' >"$PLATFORM_ROOT/launcher/res/system_icons/PORTS.png"
touch -t 200001010000 "$PLATFORM_ROOT/emulators/ports/launch.sh" \
    "$PLATFORM_ROOT/launcher/res/system_icons/PORTS.png"
run_manager stale
verify_assets
grep -q 'launcher self-heal applied' "$TMP_ROOT/stale.err"
grep -q 'system icon self-heal applied' "$TMP_ROOT/stale.err"

printf '%s\n' 'newer but different launcher' >"$PLATFORM_ROOT/emulators/ports/launch.sh"
printf '%s\n' 'newer but different icon' >"$PLATFORM_ROOT/launcher/res/system_icons/PORTS.png"
touch -t 203801010000 "$PLATFORM_ROOT/emulators/ports/launch.sh" \
    "$PLATFORM_ROOT/launcher/res/system_icons/PORTS.png"
run_manager newer-different
verify_assets
grep -q 'launcher self-heal applied' "$TMP_ROOT/newer-different.err"
grep -q 'system icon self-heal applied' "$TMP_ROOT/newer-different.err"

run_manager idempotent
verify_assets
verify_catalog
if grep -q 'self-heal applied' "$TMP_ROOT/idempotent.err"; then
    echo "second steady-state run unexpectedly self-healed" >&2
    sed -n '1,120p' "$TMP_ROOT/idempotent.err" >&2
    exit 1
fi

echo "PortMaster self-heal smoke passed"
