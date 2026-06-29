#!/usr/bin/env bash
set -euo pipefail

APP_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORKSPACE="$(cd "$APP_DIR/.." && pwd)"
JAWAKA_DIR="${JAWAKA_DIR:-$WORKSPACE/Jawaka}"
PORT="${PAKRAT_LOCAL_PORT:-}"
if [ -z "$PORT" ]; then
    PORT="$(python3 - <<'PY'
import socket

with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
    sock.bind(("127.0.0.1", 0))
    print(sock.getsockname()[1])
PY
)"
fi
FEED_ROOT="${PAKRAT_LOCAL_FEED_ROOT:-$APP_DIR/build/local-pakrat-feed}"
BASE_URL="http://127.0.0.1:$PORT/pakrat/v1/"
BUILD_DIR="${PAKRAT_SMOKE_BUILD:-build/portmaster-pakrat-local-smoke}"

if [ ! -x "$JAWAKA_DIR/scripts/pakrat-state-smoke.sh" ]; then
    echo "missing Pak Rat smoke script: $JAWAKA_DIR/scripts/pakrat-state-smoke.sh" >&2
    exit 2
fi

"$APP_DIR/tools/make-local-pakrat-feed.sh" \
    --feed-root "$FEED_ROOT" \
    --base-url "$BASE_URL"

LOG="$FEED_ROOT/http-server.log"
python3 -m http.server "$PORT" --bind 127.0.0.1 --directory "$FEED_ROOT" >"$LOG" 2>&1 &
SERVER_PID=$!
cleanup() {
    kill "$SERVER_PID" >/dev/null 2>&1 || true
}
trap cleanup EXIT

for _ in $(seq 1 50); do
    if ! kill -0 "$SERVER_PID" >/dev/null 2>&1; then
        cat "$LOG" >&2 || true
        echo "local Pak Rat feed server exited before it became ready" >&2
        exit 1
    fi
    if curl -fsS "$BASE_URL/storefront.json" >/dev/null 2>&1; then
        break
    fi
    sleep 0.1
done

curl -fsS "$BASE_URL/storefront.json" >/dev/null
curl -fsS "$BASE_URL/storefront.json" | grep -F "org.umrk.portmaster" >/dev/null

TMP_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/portmaster-pakrat-smoke.XXXXXX")"
cleanup_tmp() {
    rm -rf "$TMP_ROOT"
}
trap 'cleanup; cleanup_tmp' EXIT

SD_ROOT="$TMP_ROOT/sd"
PLATFORM_ROOT="$SD_ROOT/.system/leaf/platforms/mlp1"
STATE_DIR="$SD_ROOT/.umrk/mlp1"
mkdir -p "$PLATFORM_ROOT" "$STATE_DIR"
printf '{ "managed_apps": [] }\n' >"$PLATFORM_ROOT/manifest.json"

make -C "$JAWAKA_DIR" BUILD="$BUILD_DIR" jawaka-pakrat-smoke >/dev/null
BIN="$JAWAKA_DIR/$BUILD_DIR/bin/jawaka-pakrat-smoke"

run_smoke() {
    PAKRAT_CATALOG_BASE_URL="$BASE_URL" "$BIN" \
        --platform mlp1 \
        --sdcard-root "$SD_ROOT" \
        "$@"
}

fail_with_output() {
    local message="$1"
    local file="${2:-}"
    if [ -n "$file" ] && [ -f "$file" ]; then
        cat "$file" >&2
    fi
    echo "$message" >&2
    exit 1
}

expect_contains() {
    local file="$1"
    local needle="$2"
    local message="$3"
    grep -F "$needle" "$file" >/dev/null ||
        fail_with_output "$message" "$file"
}

LIST_AVAILABLE="$TMP_ROOT/list-available.tsv"
LIST_INSTALLED="$TMP_ROOT/list-installed.tsv"

run_smoke list >"$LIST_AVAILABLE"
expect_contains "$LIST_AVAILABLE" \
    $'available\torg.umrk.portmaster\t0.1.1\t' \
    "local catalog did not expose PortMaster"
expect_contains "$LIST_AVAILABLE" \
    $'managed=0\tpath=Apps/mlp1/PortMaster.pak' \
    "PortMaster local catalog path/state was unexpected"

run_smoke install org.umrk.portmaster >/dev/null
run_smoke list >"$LIST_INSTALLED"
expect_contains "$LIST_INSTALLED" \
    $'installed\torg.umrk.portmaster\t0.1.1\tinstalled=0.1.1' \
    "Pak Rat did not install PortMaster from the local catalog"

test -x "$SD_ROOT/Apps/mlp1/PortMaster.pak/launch.sh" ||
    fail_with_output "installed PortMaster launch.sh is not executable"
test -x "$SD_ROOT/Apps/mlp1/PortMaster.pak/bin/portmaster-mlp1" ||
    fail_with_output "installed PortMaster binary is not executable"
grep -F '"pak_version": "0.1.1"' "$SD_ROOT/Apps/mlp1/PortMaster.pak/pak.json" >/dev/null ||
    fail_with_output "installed PortMaster pak.json version was unexpected"

if grep -F "$BASE_URL" "$SD_ROOT/Apps/mlp1/PortMaster.pak/locks/ui-runtime.lock.json" >/dev/null; then
    echo "Installed UI runtime lock points at the local feed."
else
    echo "Installed UI runtime lock did not point at the local feed; runtime artifact was probably absent." >&2
fi

echo "Local PortMaster Pak Rat smoke passed against $BASE_URL"
