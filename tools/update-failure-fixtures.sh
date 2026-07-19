#!/usr/bin/env bash
set -euo pipefail

APP_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="$APP_DIR/build/bin/portmaster-mlp1"

make -C "$APP_DIR" native >/dev/null

TMP_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/portmaster-update-fixtures.XXXXXX")"
SERVER_PID=""
FAILED=0
on_error() {
    local line="$1"
    FAILED=1
    echo "update failure fixture failed at line $line" >&2
    echo "temp root: $TMP_ROOT" >&2
    if [ -f /tmp/pm-fixture.err ]; then
        echo "last stderr:" >&2
        cat /tmp/pm-fixture.err >&2
    fi
}
cleanup() {
    if [ -n "$SERVER_PID" ]; then
        kill "$SERVER_PID" >/dev/null 2>&1 || true
    fi
    if [ "$FAILED" -eq 0 ]; then
        rm -rf "$TMP_ROOT"
    fi
}
trap cleanup EXIT
trap 'on_error "$LINENO"' ERR

SD_ROOT="$TMP_ROOT/sd"
USERDATA="$TMP_ROOT/userdata"
FEED="$TMP_ROOT/feed"
mkdir -p "$SD_ROOT" "$USERDATA" "$FEED"

PORT="$(python3 - <<'PY'
import socket

with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
    sock.bind(("127.0.0.1", 0))
    print(sock.getsockname()[1])
PY
)"
BASE_URL="http://127.0.0.1:$PORT"

python3 -m http.server "$PORT" --bind 127.0.0.1 --directory "$FEED" \
    >"$TMP_ROOT/http.log" 2>&1 &
SERVER_PID=$!
for _ in $(seq 1 50); do
    if ! kill -0 "$SERVER_PID" >/dev/null 2>&1; then
        cat "$TMP_ROOT/http.log" >&2 || true
        echo "fixture HTTP server exited before ready" >&2
        exit 1
    fi
    if curl -fsS "$BASE_URL/" >/dev/null 2>&1; then
        break
    fi
    sleep 0.1
done

run_pm() {
    SDCARD_PATH="$SD_ROOT" \
    USERDATA_PATH="$USERDATA" \
    PORTMASTER_MLP1_PAK_DIR="$APP_DIR" \
    "$BIN" "$@"
}

run_update_env() {
    local metadata="$1"
    shift
    LEAF_PM_ALLOW_HTTP_UPDATE_METADATA=1 \
    LEAF_PM_UPDATE_VERSION_URL="$BASE_URL/$metadata" \
    SDCARD_PATH="$SD_ROOT" \
    USERDATA_PATH="$USERDATA" \
    PORTMASTER_MLP1_PAK_DIR="$APP_DIR" \
    "$BIN" "$@"
}

write_json() {
    local path="$1"
    local text="$2"
    printf '%s\n' "$text" >"$FEED/$path"
}

md5_file() {
    python3 - "$1" <<'PY'
import hashlib
import sys

with open(sys.argv[1], "rb") as fp:
    print(hashlib.md5(fp.read()).hexdigest())
PY
}

lower_installed_version() {
    python3 - "$USERDATA/portmaster/PortMaster/pugwash" <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
text = path.read_text()
text = text.replace("PORTMASTER_VERSION = '2026.06.23-0015'",
                    "PORTMASTER_VERSION = '2000.01.01-0000'")
path.write_text(text)
PY
}

state_path="$USERDATA/portmaster/.leaf/gui-update-state.json"
update_log="$USERDATA/portmaster/.leaf/logs/update.log"

echo "Installing locked PortMaster fixture"
run_pm --install-portmaster >/dev/null
echo "Verifying repeated repatches are idempotent"
run_pm --repatch-portmaster >/dev/null
run_pm --repatch-portmaster >/dev/null
lower_installed_version
printf 'live-marker\n' >"$USERDATA/portmaster/PortMaster/.leaf-fixture-marker"

write_json malformed.json '{ this is not json'
if run_update_env malformed.json --check-portmaster-update >/tmp/pm-fixture.out 2>/tmp/pm-fixture.err; then
    echo "malformed metadata unexpectedly succeeded" >&2
    exit 1
fi

write_json missing-md5.json \
    '{"stable":{"version":"2999.01.01-0000","url":"https://example.invalid/PortMaster.zip"}}'
if run_update_env missing-md5.json --check-portmaster-update >/tmp/pm-fixture.out 2>/tmp/pm-fixture.err; then
    echo "missing md5 metadata unexpectedly succeeded" >&2
    exit 1
fi

lock_url="$(python3 - "$APP_DIR/locks/portmaster-gui-stable.lock.json" <<'PY'
import json
import sys

print(json.load(open(sys.argv[1]))["url"])
PY
)"
write_json wrong-md5.json \
    "{\"stable\":{\"version\":\"2999.01.02-0000\",\"url\":\"$lock_url\",\"md5\":\"00000000000000000000000000000000\"}}"
if run_update_env wrong-md5.json --update-portmaster >/tmp/pm-fixture.out 2>/tmp/pm-fixture.err; then
    echo "wrong md5 update unexpectedly succeeded" >&2
    exit 1
fi
grep -F "md5 mismatch" /tmp/pm-fixture.err >/dev/null
test -f "$USERDATA/portmaster/PortMaster/.leaf-fixture-marker"

tag="2999.01.03-0000"
downloads="$USERDATA/portmaster/.leaf/downloads"
mkdir -p "$downloads"
candidate_root="$TMP_ROOT/candidate"
mkdir -p "$candidate_root"
cp -R "$USERDATA/portmaster/PortMaster" "$candidate_root/PortMaster"
rm -f "$candidate_root/PortMaster/pugwash"
candidate_zip="$downloads/PortMaster-$tag.zip"
(cd "$candidate_root" && zip -qr "$candidate_zip" PortMaster)
candidate_md5="$(md5_file "$candidate_zip")"
write_json missing-pugwash.json \
    "{\"stable\":{\"version\":\"$tag\",\"url\":\"https://example.invalid/PortMaster.zip\",\"md5\":\"$candidate_md5\"}}"

if run_update_env missing-pugwash.json --update-portmaster >/tmp/pm-fixture.out 2>/tmp/pm-fixture.err; then
    echo "missing pugwash candidate unexpectedly succeeded" >&2
    exit 1
fi
grep -F "0007-leaf-disable-self-update.patch failed" /tmp/pm-fixture.err >/dev/null
test -f "$USERDATA/portmaster/PortMaster/.leaf-fixture-marker"
python3 - "$state_path" "$tag" <<'PY'
import json
import sys

with open(sys.argv[1]) as fp:
    state = json.load(fp)
if state.get("failed_version") != sys.argv[2]:
    raise SystemExit(f"failed_version was {state.get('failed_version')!r}")
PY

cached="$(run_update_env missing-pugwash.json --check-portmaster-update-cached)"
printf '%s\n' "$cached" | grep -F "Prompt: no" >/dev/null

python3 - "$state_path" <<'PY'
import json
import sys
from pathlib import Path

path = Path(sys.argv[1])
state = json.loads(path.read_text())
state["failed_patch_fingerprint"] = "old-fingerprint"
path.write_text(json.dumps(state, indent=2) + "\n")
PY
cached_retry="$(run_update_env missing-pugwash.json --check-portmaster-update-cached)"
printf '%s\n' "$cached_retry" | grep -F "Prompt: yes" >/dev/null

grep -F "event=check-failed" "$update_log" >/dev/null
grep -F "event=apply-failed" "$update_log" >/dev/null
grep -F "event=failure-recorded" "$update_log" >/dev/null

echo "PortMaster update failure fixtures passed"
