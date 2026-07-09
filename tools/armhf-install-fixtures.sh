#!/usr/bin/env bash
set -euo pipefail

APP_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="$APP_DIR/build/bin/portmaster-mlp1"

make -C "$APP_DIR" native >/dev/null

TMP_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/portmaster-armhf-fixtures.XXXXXX")"
SERVER_PID=""
cleanup() {
    if [ -n "$SERVER_PID" ]; then
        kill "$SERVER_PID" >/dev/null 2>&1 || true
    fi
    rm -rf "$TMP_ROOT"
}
trap cleanup EXIT

SD_ROOT="$TMP_ROOT/sd"
USERDATA="$TMP_ROOT/userdata"
PAK_DIR="$TMP_ROOT/PortMaster.pak"
FEED="$TMP_ROOT/feed"
PAYLOAD="$TMP_ROOT/payload"
mkdir -p "$SD_ROOT" "$USERDATA" "$PAK_DIR/locks" "$FEED" \
    "$PAYLOAD/bin" "$PAYLOAD/lib"

VERSION="fixture-armhf-v1"
printf '%s\n' '#!/bin/sh' 'exit 0' >"$PAYLOAD/bin/leaf-armhf-run"
printf '%s\n' '#!/bin/sh' 'exit 0' >"$PAYLOAD/bin/box86"
printf '%s\n' 'fixture loader' >"$PAYLOAD/lib/ld-linux-armhf.so.3"
chmod 755 "$PAYLOAD/bin/leaf-armhf-run" "$PAYLOAD/bin/box86"
printf '{"version":"%s","files":[]}\n' "$VERSION" \
    >"$PAYLOAD/.leaf-armhf-compat-manifest.json"

ARCHIVE_NAME="armhf-fixture.zip"
MANIFEST_NAME="armhf-fixture.json"
(cd "$PAYLOAD" && zip -qr "$FEED/$ARCHIVE_NAME" .)
cp "$PAYLOAD/.leaf-armhf-compat-manifest.json" "$FEED/$MANIFEST_NAME"

sha256_file() {
    shasum -a 256 "$1" | awk '{print $1}'
}

PORT="$(python3 - <<'PY'
import socket
with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
    sock.bind(("127.0.0.1", 0))
    print(sock.getsockname()[1])
PY
)"
BASE_URL="http://127.0.0.1:$PORT"

write_lock() {
    local archive_sha="$1"
    cat >"$PAK_DIR/locks/armhf-compat.lock.json" <<JSON
{
  "schema": 1,
  "status": "fixture",
  "install_dir": "\$USERDATA_PATH/portmaster/compat/armhf",
  "builder": {"version": "$VERSION"},
  "artifacts": [
    {
      "name": "$ARCHIVE_NAME",
      "url": "$BASE_URL/$ARCHIVE_NAME",
      "size": $(wc -c <"$FEED/$ARCHIVE_NAME" | tr -d ' '),
      "sha256": "$archive_sha",
      "manifest": {
        "filename": "$MANIFEST_NAME",
        "url": "$BASE_URL/$MANIFEST_NAME",
        "size": $(wc -c <"$FEED/$MANIFEST_NAME" | tr -d ' '),
        "sha256": "$(sha256_file "$FEED/$MANIFEST_NAME")"
      }
    }
  ]
}
JSON
}

write_lock "$(sha256_file "$FEED/$ARCHIVE_NAME")"

python3 -m http.server "$PORT" --bind 127.0.0.1 --directory "$FEED" \
    >"$TMP_ROOT/http.log" 2>&1 &
SERVER_PID=$!
for _ in $(seq 1 50); do
    if curl -fsS "$BASE_URL/" >/dev/null 2>&1; then
        break
    fi
    sleep 0.1
done

run_install() {
    LEAF_PM_ALLOW_HTTP_ARMHF_COMPAT=1 \
    SDCARD_PATH="$SD_ROOT" \
    USERDATA_PATH="$USERDATA" \
    PORTMASTER_MLP1_PAK_DIR="$PAK_DIR" \
        "$BIN" --install-armhf-compat
}

TARGET="$USERDATA/portmaster/compat/armhf"
run_install >/dev/null 2>>"$TMP_ROOT/manager.err"
test -x "$TARGET/bin/leaf-armhf-run"
test -x "$TARGET/bin/box86"
test -f "$TARGET/lib/ld-linux-armhf.so.3"
grep -F "\"version\":\"$VERSION\"" \
    "$TARGET/.leaf-armhf-compat-manifest.json" >/dev/null

requests_before="$(grep -c "GET /$ARCHIVE_NAME" "$TMP_ROOT/http.log" || true)"
run_install >/dev/null 2>>"$TMP_ROOT/manager.err"
requests_after="$(grep -c "GET /$ARCHIVE_NAME" "$TMP_ROOT/http.log" || true)"
test "$requests_before" = "$requests_after"

printf '%s\n' 'preserve me' >"$TARGET/local-marker"
rm -f "$TARGET/bin/box86"
write_lock "0000000000000000000000000000000000000000000000000000000000000000"
if run_install >"$TMP_ROOT/bad.out" 2>"$TMP_ROOT/bad.err"; then
    echo "armhf install unexpectedly accepted the wrong archive hash" >&2
    exit 1
fi
grep -F "sha256 mismatch" "$TMP_ROOT/bad.err" >/dev/null
grep -F "preserve me" "$TARGET/local-marker" >/dev/null

write_lock "$(sha256_file "$FEED/$ARCHIVE_NAME")"
run_install >/dev/null 2>>"$TMP_ROOT/manager.err"
test -x "$TARGET/bin/box86"
test ! -e "$TARGET/local-marker"

echo "PortMaster armhf install fixtures passed"
