#!/usr/bin/env bash
set -euo pipefail

ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
LOCK="${UI_RUNTIME_LOCK:-$ROOT/locks/ui-runtime.lock.json}"
SPRUCE_ROOT="${SPRUCE_PORTMASTER_ROOT:-/Volumes/Storage/GitHub/spruceOS/App/PortMaster}"
OUT_DIR="${1:-$ROOT/build/ui-runtime/reference}"
SOURCES_DIR="${UI_RUNTIME_SOURCES_DIR:-$ROOT/build/ui-runtime/sources}"
PYTHON="${PYTHON:-python3}"
INCLUDE_PILLOW="${INCLUDE_PILLOW:-0}"

need() {
  command -v "$1" >/dev/null 2>&1 || {
    echo "missing required tool: $1" >&2
    exit 1
  }
}

need "$PYTHON"
need bsdtar
need zip

sha256_of() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" | awk '{print $1}'
  else
    shasum -a 256 "$1" | awk '{print $1}'
  fi
}

size_of() {
  if stat -f %z "$1" >/dev/null 2>&1; then
    stat -f %z "$1"
  else
    stat -c %s "$1"
  fi
}

bool_is_true() {
  case "${1:-}" in
    1|true|TRUE|yes|YES|on|ON) return 0 ;;
    *) return 1 ;;
  esac
}

spruce_archive="$SPRUCE_ROOT/portmaster.7z"
if [ ! -f "$spruce_archive" ]; then
  echo "missing Spruce reference archive: $spruce_archive" >&2
  exit 1
fi

expected_spruce_sha="$("$PYTHON" - "$LOCK" <<'PY'
import json
import sys
with open(sys.argv[1], "r", encoding="utf-8") as fp:
    print(json.load(fp)["spruce_reference"]["portmaster_7z_sha256"])
PY
)"
actual_spruce_sha="$(sha256_of "$spruce_archive")"
if [ "$actual_spruce_sha" != "$expected_spruce_sha" ]; then
  echo "Spruce reference archive hash mismatch" >&2
  echo "expected $expected_spruce_sha" >&2
  echo "actual   $actual_spruce_sha" >&2
  exit 1
fi

if bool_is_true "$INCLUDE_PILLOW"; then
  INCLUDE_OPTIONAL=1 "$ROOT/scripts/fetch-ui-runtime-sources.sh" "$SOURCES_DIR"
else
  "$ROOT/scripts/fetch-ui-runtime-sources.sh" "$SOURCES_DIR"
fi

wheel_plan="$OUT_DIR/.wheel-plan.tsv"
mkdir -p "$OUT_DIR"
"$PYTHON" - "$LOCK" "$INCLUDE_PILLOW" >"$wheel_plan" <<'PY'
import json
import sys

lock_path, include_pillow_raw = sys.argv[1:3]
include_pillow = include_pillow_raw.lower() in {"1", "true", "yes", "on"}
with open(lock_path, "r", encoding="utf-8") as fp:
    lock = json.load(fp)
for item in lock.get("source_inputs", []):
    if item.get("type") != "pypi-wheel":
        continue
    if item["bucket"] == "pillow" and not include_pillow:
        continue
    if item["bucket"] not in {"pysdl2-dll", "pillow"}:
        continue
    print("\t".join([item["bucket"], item["filename"], item["sha256"]]))
PY

tmp="$(mktemp -d "${TMPDIR:-/tmp}/pm-ui-runtime.XXXXXX")"
cleanup() {
  rm -rf "$tmp"
}
trap cleanup EXIT

mkdir -p "$tmp/extract" "$OUT_DIR/root"
rm -rf "$OUT_DIR/root"
mkdir -p "$OUT_DIR/root/portmaster"

bsdtar -xf "$spruce_archive" -C "$tmp/extract"
src="$tmp/extract/portmaster"
if [ ! -f "$src/bin/python3" ]; then
  echo "Spruce reference archive did not contain portmaster/bin/python3" >&2
  exit 1
fi

for rel in bin include lib share; do
  if [ -e "$src/$rel" ]; then
    cp -R "$src/$rel" "$OUT_DIR/root/portmaster/"
  fi
done

site="$OUT_DIR/root/portmaster/lib/python3.10/site-packages"
mkdir -p "$site"

while IFS=$'\t' read -r bucket filename expected_sha; do
  wheel="$SOURCES_DIR/$filename"
  if [ ! -f "$wheel" ]; then
    echo "missing fetched wheel: $wheel" >&2
    exit 1
  fi
  actual_sha="$(sha256_of "$wheel")"
  if [ "$actual_sha" != "$expected_sha" ]; then
    echo "wheel hash mismatch for $filename" >&2
    echo "expected $expected_sha" >&2
    echo "actual   $actual_sha" >&2
    exit 1
  fi
  "$PYTHON" -m zipfile -e "$wheel" "$site"
done <"$wheel_plan"

if [ -d "$site/sdl2dll" ]; then
  rm -rf "$OUT_DIR/root/portmaster/lib/sdl2dll"
  cp -R "$site/sdl2dll" "$OUT_DIR/root/portmaster/lib/sdl2dll"
fi
if [ -d "$site/pysdl2_dll-2.32.0.dist-info" ]; then
  rm -rf "$OUT_DIR/root/portmaster/lib/pysdl2_dll-2.32.0.dist-info"
  cp -R "$site/pysdl2_dll-2.32.0.dist-info" "$OUT_DIR/root/portmaster/lib/"
fi

chmod 755 "$OUT_DIR/root/portmaster/bin/python" \
          "$OUT_DIR/root/portmaster/bin/python3" \
          "$OUT_DIR/root/portmaster/bin/python3.10" 2>/dev/null || true

runtime_manifest="$OUT_DIR/root/portmaster/.leaf-runtime-manifest.json"
"$PYTHON" - "$LOCK" "$SOURCES_DIR" "$runtime_manifest" "$spruce_archive" "$INCLUDE_PILLOW" <<'PY'
import hashlib
import json
import os
import sys
from datetime import datetime, timezone

lock_path, sources_dir, manifest_path, spruce_archive, include_pillow_raw = sys.argv[1:6]
include_pillow = include_pillow_raw.lower() in {"1", "true", "yes", "on"}

def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as fp:
        for chunk in iter(lambda: fp.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()

with open(lock_path, "r", encoding="utf-8") as fp:
    lock = json.load(fp)

sources = [{
    "bucket": "cpython-reference",
    "type": "spruce-reference-archive",
    "path": spruce_archive,
    "size": os.path.getsize(spruce_archive),
    "sha256": sha256(spruce_archive),
    "production": False,
}]
for item in lock.get("source_inputs", []):
    if item.get("type") != "pypi-wheel":
        continue
    if item["bucket"] == "pillow" and not include_pillow:
        continue
    if item["bucket"] not in {"pysdl2-dll", "pillow"}:
        continue
    path = os.path.join(sources_dir, item["filename"])
    sources.append({
        "bucket": item["bucket"],
        "type": item["type"],
        "project": item["project"],
        "version": item["version"],
        "filename": item["filename"],
        "size": os.path.getsize(path),
        "sha256": sha256(path),
        "license": item.get("license", ""),
        "url": item.get("url", ""),
        "production": item["bucket"] != "pillow" or include_pillow,
    })

manifest = {
    "schema": 1,
    "product": lock.get("product"),
    "kind": "reference-runtime",
    "production": False,
    "note": "Uses Spruce CPython runtime as a reference input for device smoke; not a production Leaf supply-chain artifact.",
    "generated_at": datetime.now(timezone.utc).isoformat(),
    "target": lock.get("target", {}),
    "sources": sources,
}
with open(manifest_path, "w", encoding="utf-8") as fp:
    json.dump(manifest, fp, indent=2)
    fp.write("\n")
PY

artifact="$OUT_DIR/portmaster-mlp1-ui-runtime-python310-aarch64-reference.zip"
manifest="$OUT_DIR/portmaster-mlp1-ui-runtime-python310-aarch64-reference.json"
rm -f "$artifact" "$manifest"
(cd "$OUT_DIR/root" && zip -qr "$artifact" portmaster)

"$PYTHON" - "$runtime_manifest" "$artifact" "$manifest" "$OUT_DIR/root/portmaster" <<'PY'
import hashlib
import json
import os
import sys
from datetime import datetime, timezone

runtime_manifest, artifact_path, manifest_path, runtime_root = sys.argv[1:5]

def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as fp:
        for chunk in iter(lambda: fp.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()

with open(runtime_manifest, "r", encoding="utf-8") as fp:
    manifest = json.load(fp)

file_count = 0
installed_size = 0
for dirpath, _, filenames in os.walk(runtime_root):
    for name in filenames:
        path = os.path.join(dirpath, name)
        file_count += 1
        installed_size += os.path.getsize(path)

manifest["artifact"] = {
    "path": artifact_path,
    "filename": os.path.basename(artifact_path),
    "size": os.path.getsize(artifact_path),
    "sha256": sha256(artifact_path),
    "installed_file_count": file_count,
    "installed_size": installed_size,
}
manifest["generated_at"] = datetime.now(timezone.utc).isoformat()

with open(manifest_path, "w", encoding="utf-8") as fp:
    json.dump(manifest, fp, indent=2)
    fp.write("\n")
PY

printf '%s  %s\n' "$(sha256_of "$artifact")" "$artifact"
echo "wrote $manifest"
