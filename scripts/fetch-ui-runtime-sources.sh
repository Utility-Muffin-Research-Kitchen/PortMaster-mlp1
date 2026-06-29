#!/usr/bin/env bash
set -euo pipefail

ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
LOCK="${UI_RUNTIME_LOCK:-$ROOT/locks/ui-runtime.lock.json}"
OUT_DIR="${1:-$ROOT/build/ui-runtime/sources}"
PYTHON="${PYTHON:-python3}"
INCLUDE_OPTIONAL="${INCLUDE_OPTIONAL:-0}"

need() {
  command -v "$1" >/dev/null 2>&1 || {
    echo "missing required tool: $1" >&2
    exit 1
  }
}

need "$PYTHON"

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

mkdir -p "$OUT_DIR"

plan="$OUT_DIR/.fetch-plan.tsv"
"$PYTHON" - "$LOCK" "$INCLUDE_OPTIONAL" >"$plan" <<'PY'
import json
import sys

lock_path, include_optional_raw = sys.argv[1:3]
include_optional = include_optional_raw.lower() in {"1", "true", "yes", "on"}

with open(lock_path, "r", encoding="utf-8") as fp:
    lock = json.load(fp)

for item in lock.get("source_inputs", []):
    if item.get("type") != "pypi-wheel":
        continue
    if not item.get("default_include", False) and not include_optional:
        continue
    fields = [
        item["bucket"],
        item["project"],
        item["version"],
        item["filename"],
        item["platform"],
        item["python_version"],
        item["implementation"],
        item["abi"],
        str(item["size"]),
        item["sha256"],
    ]
    print("\t".join(fields))
PY

if [ ! -s "$plan" ]; then
  echo "no UI runtime source inputs selected"
  exit 0
fi

while IFS=$'\t' read -r bucket project version filename platform python_version implementation abi expected_size expected_sha; do
  dest="$OUT_DIR/$filename"
  if [ -f "$dest" ] &&
     [ "$(size_of "$dest")" = "$expected_size" ] &&
     [ "$(sha256_of "$dest")" = "$expected_sha" ]; then
    echo "OK $filename"
    continue
  fi

  tmp="$OUT_DIR/.download-$bucket"
  rm -rf "$tmp"
  mkdir -p "$tmp"

  "$PYTHON" -m pip download \
    --only-binary=:all: \
    --platform "$platform" \
    --python-version "$python_version" \
    --implementation "$implementation" \
    --abi "$abi" \
    --dest "$tmp" \
    --no-deps \
    "$project==$version"

  if [ ! -f "$tmp/$filename" ]; then
    echo "pip did not produce expected wheel: $filename" >&2
    find "$tmp" -maxdepth 1 -type f -print >&2
    exit 1
  fi

  actual_size="$(size_of "$tmp/$filename")"
  actual_sha="$(sha256_of "$tmp/$filename")"
  if [ "$actual_size" != "$expected_size" ]; then
    echo "size mismatch for $filename: expected $expected_size got $actual_size" >&2
    exit 1
  fi
  if [ "$actual_sha" != "$expected_sha" ]; then
    echo "sha256 mismatch for $filename: expected $expected_sha got $actual_sha" >&2
    exit 1
  fi

  mv -f "$tmp/$filename" "$dest"
  rm -rf "$tmp"
  echo "OK $filename"
done <"$plan"

report="$OUT_DIR/ui-runtime-sources.report.json"
"$PYTHON" - "$LOCK" "$OUT_DIR" "$report" "$INCLUDE_OPTIONAL" <<'PY'
import hashlib
import json
import os
import sys
from datetime import datetime, timezone

lock_path, out_dir, report_path, include_optional_raw = sys.argv[1:5]
include_optional = include_optional_raw.lower() in {"1", "true", "yes", "on"}

with open(lock_path, "r", encoding="utf-8") as fp:
    lock = json.load(fp)

sources = []
for item in lock.get("source_inputs", []):
    if item.get("type") != "pypi-wheel":
        continue
    if not item.get("default_include", False) and not include_optional:
        continue
    path = os.path.join(out_dir, item["filename"])
    with open(path, "rb") as fp:
        digest = hashlib.sha256(fp.read()).hexdigest()
    sources.append({
        "bucket": item["bucket"],
        "project": item["project"],
        "version": item["version"],
        "filename": item["filename"],
        "size": os.path.getsize(path),
        "sha256": digest,
        "license": item.get("license", ""),
        "url": item.get("url", ""),
    })

report = {
    "schema": 1,
    "product": lock.get("product"),
    "generated_at": datetime.now(timezone.utc).isoformat(),
    "include_optional": include_optional,
    "sources": sources,
}
with open(report_path, "w", encoding="utf-8") as fp:
    json.dump(report, fp, indent=2)
    fp.write("\n")
PY

echo "wrote $report"
