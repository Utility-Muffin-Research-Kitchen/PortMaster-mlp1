#!/usr/bin/env bash
set -euo pipefail

repo_dir="$(cd "$(dirname "$0")/.." && pwd)"
binary="${ARTWORK_TEST_BINARY:-$repo_dir/build/bin/portmaster-mlp1}"
cover_png="$repo_dir/../Catastrophe/res/demo_icon.png"
fallback_png="$repo_dir/../Catastrophe/res/assets/preview.png"
custom_png="$repo_dir/../Catastrophe/res/assets/assets@1x.png"

for path in "$binary" "$cover_png" "$fallback_png" "$custom_png"; do
  test -f "$path" || {
    echo "missing artwork fixture dependency: $path" >&2
    exit 1
  }
done

fixture_root="$(mktemp -d "${TMPDIR:-/tmp}/portmaster-artwork.XXXXXX")"
trap 'rm -rf "$fixture_root"' EXIT
sd="$fixture_root/sd"
userdata="$fixture_root/userdata"
ports="$sd/Roms/PORTS"
images="$sd/Images/PORTS"
cache="$userdata/portmaster/PortMaster/config/images_pm"
mkdir -p "$ports" "$images" "$cache"

run_artwork() {
  PORTMASTER_MLP1_PAK_DIR="$repo_dir" \
  SDCARD_PATH="$sd" \
  USERDATA_PATH="$userdata" \
  ROMS_PATH="$sd/Roms" \
  IMAGES_PATH="$sd/Images" \
    "$binary" "$1"
}

write_script() {
  printf '#!/bin/sh\nexit 0\n' >"$ports/$1"
  chmod 755 "$ports/$1"
}

# Bully-style installed metadata: the bundled gameinfo mapping must beat cache.
mkdir -p "$ports/bully"
write_script "Bully.sh"
cp "$cover_png" "$ports/bully/cover.png"
cp "$fallback_png" "$ports/bully/screenshot.png"
cp "$fallback_png" "$cache/bully.screenshot.png"
cat >"$ports/bully/port.json" <<'JSON'
{
  "name": "bully.zip",
  "items": ["Bully.sh", "bully/"],
  "items_opt": null,
  "files": {
    "port.json": "bully/port.json",
    "Bully.sh": "Bully.sh",
    "bully/": "bully/"
  },
  "attr": {"image": {}}
}
JSON
cat >"$ports/bully/gameinfo.xml" <<'XML'
<?xml version="1.0" encoding="utf-8"?>
<gameList>
  <game>
    <path>./Bully.sh</path>
    <image>./bully/cover.png</image>
  </game>
</gameList>
XML

# One package owns two launchers and maps a distinct cover to each launcher.
mkdir -p "$ports/multi"
write_script "Multi One.sh"
write_script "Multi Two.sh"
cp "$cover_png" "$ports/multi/one.png"
cp "$fallback_png" "$ports/multi/two.png"
cat >"$ports/multi/port.json" <<'JSON'
{
  "name": "multi.zip",
  "items": ["Multi One.sh", "Multi Two.sh", "multi"],
  "files": {
    "port.json": "multi/port.json",
    "Multi One.sh": "Multi One.sh",
    "Multi Two.sh": "Multi Two.sh",
    "multi": "multi"
  },
  "attr": {"image": {}}
}
JSON
cat >"$ports/multi/gameinfo.xml" <<'XML'
<gameList>
  <game><path>./Multi One.sh</path><image>./multi/one.png</image></game>
  <game><path>./Multi Two.sh</path><image>./multi/two.png</image></game>
</gameList>
XML

# Two generic covers are ambiguous, so the package archive cache screenshot wins.
mkdir -p "$ports/ambiguous" "$ports/ambiguous-extra"
write_script "Ambiguous.sh"
cp "$cover_png" "$ports/ambiguous/cover.png"
cp "$cover_png" "$ports/ambiguous-extra/cover.jpg"
cp "$fallback_png" "$cache/ambiguous.screenshot.png"
cat >"$ports/ambiguous/port.json" <<'JSON'
{
  "name": "ambiguous.zip",
  "items": ["Ambiguous.sh", "ambiguous", "ambiguous-extra"],
  "files": {
    "port.json": "ambiguous/port.json",
    "Ambiguous.sh": "Ambiguous.sh",
    "ambiguous": "ambiguous",
    "ambiguous-extra": "ambiguous-extra"
  },
  "attr": {"image": {}}
}
JSON

# Traversal in gameinfo is rejected; it cannot escape package ownership.
mkdir -p "$ports/unsafe"
write_script "Unsafe.sh"
cp "$fallback_png" "$cache/unsafe.screenshot.png"
cp "$cover_png" "$ports/outside.png"
cat >"$ports/unsafe/port.json" <<'JSON'
{
  "name": "unsafe.zip",
  "items": ["Unsafe.sh", "unsafe"],
  "files": {
    "port.json": "unsafe/port.json",
    "Unsafe.sh": "Unsafe.sh",
    "unsafe": "unsafe"
  },
  "attr": {"image": {}}
}
JSON
cat >"$ports/unsafe/gameinfo.xml" <<'XML'
<gameList>
  <game><path>./Unsafe.sh</path><image>../outside.png</image></game>
</gameList>
XML

# A symlinked mapped image is never accepted, even when its bytes are valid.
mkdir -p "$ports/symlink"
write_script "Symlink.sh"
ln -s "$cover_png" "$ports/symlink/cover.png"
cp "$fallback_png" "$cache/symlink.screenshot.png"
cat >"$ports/symlink/port.json" <<'JSON'
{
  "name": "symlink.zip",
  "items": ["Symlink.sh", "symlink"],
  "files": {
    "port.json": "symlink/port.json",
    "Symlink.sh": "Symlink.sh",
    "symlink": "symlink"
  },
  "attr": {"image": {}}
}
JSON
cat >"$ports/symlink/gameinfo.xml" <<'XML'
<gameList>
  <game><path>./Symlink.sh</path><image>./symlink/cover.png</image></game>
</gameList>
XML

# DTD/entity declarations make the whole mapping file ineligible.
mkdir -p "$ports/doctype"
write_script "Doctype.sh"
cp "$cover_png" "$ports/doctype/mapped.png"
cp "$fallback_png" "$cache/doctype.screenshot.png"
cat >"$ports/doctype/port.json" <<'JSON'
{
  "name": "doctype.zip",
  "items": ["Doctype.sh", "doctype"],
  "files": {
    "port.json": "doctype/port.json",
    "Doctype.sh": "Doctype.sh",
    "doctype": "doctype"
  },
  "attr": {"image": {}}
}
JSON
cat >"$ports/doctype/gameinfo.xml" <<'XML'
<!DOCTYPE gameList [<!ENTITY cover "./doctype/mapped.png">]>
<gameList>
  <game><path>./Doctype.sh</path><image>&cover;</image></game>
</gameList>
XML

# Duplicate normalized launcher mappings are rejected rather than guessed.
mkdir -p "$ports/duplicate"
write_script "Duplicate.sh"
cp "$cover_png" "$ports/duplicate/one.png"
cp "$custom_png" "$ports/duplicate/two.png"
cp "$fallback_png" "$cache/duplicate.screenshot.png"
cat >"$ports/duplicate/port.json" <<'JSON'
{
  "name": "duplicate.zip",
  "items": ["Duplicate.sh", "duplicate"],
  "files": {
    "port.json": "duplicate/port.json",
    "Duplicate.sh": "Duplicate.sh",
    "duplicate": "duplicate"
  },
  "attr": {"image": {}}
}
JSON
cat >"$ports/duplicate/gameinfo.xml" <<'XML'
<gameList>
  <game><path>./Duplicate.sh</path><image>./duplicate/one.png</image></game>
  <game><path>Duplicate.sh</path><image>./duplicate/two.png</image></game>
</gameList>
XML

# An unreadable/corrupt higher-precedence candidate falls through to cache.
mkdir -p "$ports/corrupt"
write_script "Corrupt.sh"
printf 'not an image\n' >"$ports/corrupt/cover.png"
cp "$fallback_png" "$cache/corrupt.screenshot.png"
cat >"$ports/corrupt/port.json" <<'JSON'
{
  "name": "corrupt.zip",
  "items": ["Corrupt.sh", "corrupt"],
  "files": {
    "port.json": "corrupt/port.json",
    "Corrupt.sh": "Corrupt.sh",
    "corrupt": "corrupt"
  },
  "attr": {"image": {}}
}
JSON

first_output="$(run_artwork --sync-port-artwork)"
echo "$first_output"
grep -q 'scanned=9 synced=9' <<<"$first_output"
cmp "$images/Bully.png" "$cover_png"
cmp "$images/Multi One.png" "$cover_png"
cmp "$images/Multi Two.png" "$fallback_png"
cmp "$images/Ambiguous.png" "$fallback_png"
cmp "$images/Unsafe.png" "$fallback_png"
cmp "$images/Symlink.png" "$fallback_png"
cmp "$images/Doctype.png" "$fallback_png"
cmp "$images/Duplicate.png" "$fallback_png"
cmp "$images/Corrupt.png" "$fallback_png"
grep -q '"Bully.png"' "$images/.portmaster-artwork.json"
grep -q '"source_kind".*"installed"' "$images/.portmaster-artwork.json"
grep -q '"source_relpath".*"PORTS/bully/cover.png"' "$images/.portmaster-artwork.json"

missing_output="$(run_artwork --sync-port-artwork)"
echo "$missing_output"
grep -q 'scanned=9 synced=0 skipped_existing=9' <<<"$missing_output"

# Managed refresh updates tracked output but preserves a digest mismatch as custom.
cp "$custom_png" "$images/Bully.png"
managed_output="$(run_artwork --refresh-port-artwork)"
echo "$managed_output"
grep -q 'preserved_custom=1' <<<"$managed_output"
cmp "$images/Bully.png" "$custom_png"

# Replace All is the explicit migration path for old/untracked or custom output.
replace_output="$(run_artwork --replace-port-artwork)"
echo "$replace_output"
grep -q 'scanned=9 synced=9' <<<"$replace_output"
cmp "$images/Bully.png" "$cover_png"

# Recover the target-rename/manifest-commit crash window idempotently.
command -v jq >/dev/null || {
  echo "jq is required for artwork pending-record fixtures" >&2
  exit 1
}
jq '.entries["Bully.png"]' "$images/.portmaster-artwork.json" \
  >"$images/.portmaster-artwork.pending.json"
jq 'del(.entries["Bully.png"])' "$images/.portmaster-artwork.json" \
  >"$images/.portmaster-artwork.json.partial"
mv "$images/.portmaster-artwork.json.partial" "$images/.portmaster-artwork.json"
recovery_output="$(run_artwork --sync-port-artwork)"
echo "$recovery_output"
test ! -e "$images/.portmaster-artwork.pending.json"
grep -q '"Bully.png"' "$images/.portmaster-artwork.json"
cmp "$images/Bully.png" "$cover_png"

# A configured and explicitly available Secondary source is scanned without
# redirecting its launcher or artwork paths through the Primary source.
secondary="$fixture_root/secondary"
secondary_ports="$secondary/Roms/PORTS"
secondary_images="$secondary/Images/PORTS"
mkdir -p "$secondary_ports/secondary-game" "$secondary_images"
printf '#!/bin/sh\nexit 0\n' >"$secondary_ports/Bully.sh"
chmod 755 "$secondary_ports/Bully.sh"
cp "$custom_png" "$secondary_ports/secondary-game/cover.png"
cat >"$secondary_ports/secondary-game/port.json" <<'JSON'
{
  "name": "secondary-game.zip",
  "items": ["Bully.sh", "secondary-game"],
  "files": {
    "port.json": "secondary-game/port.json",
    "Bully.sh": "Bully.sh",
    "secondary-game": "secondary-game"
  },
  "attr": {"image": {}}
}
JSON
cat >"$secondary_ports/secondary-game/gameinfo.xml" <<'XML'
<gameList>
  <game><path>./Bully.sh</path><image>./secondary-game/cover.png</image></game>
</gameList>
XML
dual_output="$(
  SDCARD_PATHS="$sd:$secondary" \
  ROMS_PATHS="$sd/Roms:$secondary/Roms" \
  IMAGES_PATHS="$sd/Images:$secondary/Images" \
  UMRK_SECONDARY_SDCARD_PATH="$secondary" \
  PORTMASTER_SOURCE_TEST_AVAILABLE=primary,secondary_sd \
    run_artwork --sync-port-artwork
)"
echo "$dual_output"
grep -q 'scanned=10 synced=1 skipped_existing=9' <<<"$dual_output"
cmp "$secondary_images/Bully.png" "$custom_png"
grep -q '"source_id".*"secondary_sd"' \
  "$secondary_images/.portmaster-artwork.json"

secondary_manifest_before="$(shasum -a 256 \
  "$secondary_images/.portmaster-artwork.json" | awk '{print $1}')"
unavailable_output="$(
  SDCARD_PATHS="$sd:$secondary" \
  ROMS_PATHS="$sd/Roms:$secondary/Roms" \
  IMAGES_PATHS="$sd/Images:$secondary/Images" \
  UMRK_SECONDARY_SDCARD_PATH="$secondary" \
  PORTMASTER_SOURCE_TEST_AVAILABLE=primary \
    run_artwork --refresh-port-artwork
)"
grep -q 'scanned=9' <<<"$unavailable_output"
secondary_manifest_after="$(shasum -a 256 \
  "$secondary_images/.portmaster-artwork.json" | awk '{print $1}')"
test "$secondary_manifest_before" = "$secondary_manifest_after"

echo "artwork fixtures: PASS"
