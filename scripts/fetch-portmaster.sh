#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LOCK="$ROOT/locks/portmaster-gui-stable.lock.json"

usage() {
  cat <<'USAGE'
usage:
  scripts/fetch-portmaster.sh --metadata-only
  scripts/fetch-portmaster.sh --download <path>
USAGE
}

mode="${1:-}"
case "$mode" in
  --metadata-only)
    python3 - "$LOCK" <<'PY'
import json, sys
lock = json.load(open(sys.argv[1]))
for key in ("tag", "published_at", "asset", "size", "md5", "sha256", "url"):
    print(f"{key}: {lock.get(key, '')}")
PY
    ;;
  --download)
    dest="${2:-}"
    test -n "$dest" || { usage >&2; exit 1; }
    url="$(python3 - "$LOCK" <<'PY'
import json, sys
print(json.load(open(sys.argv[1]))["url"])
PY
)"
    tmp="$dest.partial"
    rm -f "$tmp"
    curl -fL --retry 3 --connect-timeout 20 -o "$tmp" "$url"
    python3 - "$LOCK" "$tmp" <<'PY'
import hashlib, json, os, sys
lock = json.load(open(sys.argv[1]))
path = sys.argv[2]
size = os.path.getsize(path)
sha = hashlib.sha256(open(path, "rb").read()).hexdigest()
if size != int(lock["size"]):
    raise SystemExit(f"size mismatch: got {size}, expected {lock['size']}")
if sha != lock["sha256"]:
    raise SystemExit(f"sha256 mismatch: got {sha}, expected {lock['sha256']}")
print(f"verified {path}: {size} bytes {sha}")
PY
    mv "$tmp" "$dest"
    ;;
  *)
    usage >&2
    exit 1
    ;;
esac

