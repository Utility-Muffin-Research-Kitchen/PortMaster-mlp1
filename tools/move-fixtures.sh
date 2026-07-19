#!/usr/bin/env bash
set -euo pipefail

repo_dir="$(cd "$(dirname "$0")/.." && pwd)"
binary="${MOVE_TEST_BINARY:-$repo_dir/build/bin/portmaster-mlp1}"
temp="$(mktemp -d "${TMPDIR:-/tmp}/portmaster-move-fixtures.XXXXXX")"
inhibitor_pid=""
cleanup() {
  if [ -n "$inhibitor_pid" ]; then
    kill "$inhibitor_pid" 2>/dev/null || true
  fi
  pkill -f "$temp/fake-jawaka-inhibitctl" 2>/dev/null || true
  rm -rf "$temp"
}
trap cleanup EXIT HUP INT TERM

primary="$temp/primary"
secondary="$temp/secondary"
userdata="$primary/.userdata/mlp1"
primary_ports="$primary/Roms/PORTS"
secondary_ports="$secondary/Roms/PORTS"
primary_images="$primary/Images/PORTS"
secondary_images="$secondary/Images/PORTS"
fake_state="$temp/jawaka"
mkdir -p "$primary_ports/alpha" "$primary_ports/alpha-data" \
  "$primary_ports/shared-one" "$primary_ports/shared-two" "$primary_ports/shared" \
  "$primary_ports/unsafe" "$primary_images" "$secondary" \
  "$userdata" "$fake_state"

printf '#!/bin/sh\nexit 0\n' >"$primary_ports/Alpha One.sh"
printf '#!/bin/sh\nexit 0\n' >"$primary_ports/Alpha Two Renamed.sh"
printf 'game-data\n' >"$primary_ports/alpha-data/game.bin"
printf 'cover-one\n' >"$primary_images/Alpha One.png"
printf 'cover-two\n' >"$primary_images/Alpha Two Renamed.png"
printf '#!/bin/sh\nexit 0\n' >"$primary_ports/Manual.sh"
chmod +x "$primary_ports/"*.sh

cat >"$primary_ports/alpha/port.json" <<'JSON'
{
  "name": "alpha.zip",
  "attr": {"title": "Alpha Collection"},
  "items": ["Alpha One.sh", "Alpha Two.sh", "alpha-data"],
  "items_opt": ["optional-missing"],
  "files": {
    "port.json": "alpha/port.json",
    "Alpha One.sh": "Alpha One.sh",
    "Alpha Two.sh": "Alpha Two Renamed.sh",
    "alpha-data/": "alpha-data/"
  }
}
JSON

for package in shared-one shared-two; do
  script="${package^}.sh"
  printf '#!/bin/sh\nexit 0\n' >"$primary_ports/$script"
  chmod +x "$primary_ports/$script"
  cat >"$primary_ports/$package/port.json" <<JSON
{
  "name": "$package.zip",
  "files": {
    "port.json": "$package/port.json",
    "launcher": "$script",
    "shared": "shared"
  }
}
JSON
done
printf 'shared\n' >"$primary_ports/shared/data"

printf '#!/bin/sh\nexit 0\n' >"$primary_ports/Unsafe.sh"
chmod +x "$primary_ports/Unsafe.sh"
cat >"$primary_ports/unsafe/port.json" <<'JSON'
{
  "name": "unsafe.zip",
  "files": {
    "port.json": "unsafe/port.json",
    "launcher": "Unsafe.sh",
    "escape": "../escape"
  }
}
JSON

cat >"$primary_images/.portmaster-artwork.json" <<'JSON'
{
  "version": 1,
  "entries": {
    "Alpha One.png": {
      "target": "Alpha One.png",
      "source_kind": "installed",
      "source_id": "primary",
      "source_root": "roms",
      "source_relpath": "alpha-data/cover.png",
      "source_sha256": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
      "output_sha256": "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
    },
    "Alpha Two Renamed.png": {
      "target": "Alpha Two Renamed.png",
      "source_kind": "cache",
      "source_id": "primary",
      "source_root": "cache",
      "source_relpath": "alpha.screenshot.png",
      "source_sha256": "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc",
      "output_sha256": "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd"
    }
  }
}
JSON

cat >"$temp/fake-jawaka-platformctl" <<'SH'
#!/usr/bin/env bash
set -euo pipefail
command="${1:-}"
shift || true
state_dir="${FAKE_JAWAKA_DIR:?}"
generation_file="$state_dir/generation"
[ -f "$generation_file" ] || printf '1\n' >"$generation_file"
generation="$(cat "$generation_file")"
capability="${FAKE_JAWAKA_CAPABILITY:-true}"
status_json() {
  local operation="$1"
  local state_file="$state_dir/$operation.state"
  local mapping_file="$state_dir/$operation.mapping"
  local ticket_file="$state_dir/$operation.ticket"
  local state="prepared" mapping=0 ticket=0
  [ -f "$state_file" ] && state="$(cat "$state_file")"
  [ -f "$mapping_file" ] && mapping="$(cat "$mapping_file")"
  [ -f "$ticket_file" ] && ticket="$(cat "$ticket_file")"
  printf '{"type":"library-relocate-status","operation_id":"%s","state":"%s","expected_generation":1,"mapping_generation":%s,"scan_ticket_generation":%s,"library_generation":%s,"item_count":2}\n' \
    "$operation" "$state" "$mapping" "$ticket" "$(cat "$generation_file")"
}
case "$command" in
  capabilities)
    printf '{"type":"capabilities","relocate-games-v1":%s}\n' "$capability"
    ;;
  request)
    printf '{"type":"library-status","generation":%s,"scan_running":false}\n' "$generation"
    ;;
  relocate-prepare)
    operation="$1"
    expected="$2"
    [ "$expected" = "$generation" ]
    printf 'prepared\n' >"$state_dir/$operation.state"
    printf '0\n' >"$state_dir/$operation.mapping"
    printf '0\n' >"$state_dir/$operation.ticket"
    status_json "$operation"
    ;;
  relocate-commit)
    operation="$1"
    state="$(cat "$state_dir/$operation.state")"
    if [ "$state" = prepared ]; then
      mapping=$((generation + 1))
      printf '%s\n' "$mapping" >"$generation_file"
      printf '%s\n' "$mapping" >"$state_dir/$operation.mapping"
      printf 'committed\n' >"$state_dir/$operation.state"
    fi
    status_json "$operation"
    ;;
  relocate-revert)
    operation="$1"
    state="$(cat "$state_dir/$operation.state")"
    if [ "$state" = committed ]; then
      generation="$(cat "$generation_file")"
      mapping=$((generation + 1))
      printf '%s\n' "$mapping" >"$generation_file"
      printf '%s\n' "$mapping" >"$state_dir/$operation.mapping"
      printf 'reverted\n' >"$state_dir/$operation.state"
    fi
    status_json "$operation"
    ;;
  relocate-abort)
    operation="$1"
    printf 'aborted\n' >"$state_dir/$operation.state"
    status_json "$operation"
    ;;
  relocate-finish)
    operation="$1"
    mapping="$(cat "$state_dir/$operation.mapping")"
    ticket=$((mapping + 1))
    printf '%s\n' "$ticket" >"$generation_file"
    printf '%s\n' "$ticket" >"$state_dir/$operation.ticket"
    printf 'finished\n' >"$state_dir/$operation.state"
    status_json "$operation"
    ;;
  relocate-status)
    status_json "$1"
    ;;
  *)
    exit 2
    ;;
esac
SH
chmod +x "$temp/fake-jawaka-platformctl"

cat >"$temp/fake-jawaka-inhibitctl" <<'SH'
#!/usr/bin/env bash
trap 'exit 0' TERM INT HUP
printf 'acquired pid=%d reason=fixture\n' "$$"
while :; do sleep 1; done
SH
chmod +x "$temp/fake-jawaka-inhibitctl"

export PLATFORM=mlp1
export SDCARD_PATH="$primary"
export USERDATA_PATH="$userdata"
export ROMS_PATH="$primary/Roms"
export IMAGES_PATH="$primary/Images"
export SDCARD_PATHS="$primary:$secondary"
export ROMS_PATHS="$primary/Roms:$secondary/Roms"
export IMAGES_PATHS="$primary/Images:$secondary/Images"
export PORTMASTER_SOURCE_TEST_AVAILABLE="primary,secondary_sd"
export PORTMASTER_SOURCE_TEST_FINGERPRINT="move-fixture"
export PORTMASTER_MLP1_PAK_DIR="$repo_dir"
export JAWAKA_PLATFORMCTL="$temp/fake-jawaka-platformctl"
export JAWAKA_INHIBITCTL="$temp/fake-jawaka-inhibitctl"
export FAKE_JAWAKA_DIR="$fake_state"
export UMRK_RUNTIME_PATH="$temp/runtime"

inventory="$("$binary" --ports-text)"
grep -F $'package=alpha\tsource=primary\tmovable=1' <<<"$inventory" >/dev/null
grep -F $'launchers=2' <<<"$inventory" >/dev/null
grep -F $'package=shared-one\tsource=primary\tmovable=0' <<<"$inventory" >/dev/null
grep -F $'package=shared-two\tsource=primary\tmovable=0' <<<"$inventory" >/dev/null
grep -F $'package=unsafe\tsource=primary\tmovable=0' <<<"$inventory" >/dev/null
grep -F 'unmanaged=primary:Manual.sh' <<<"$inventory" >/dev/null

"$binary" --move-port primary alpha secondary_sd
test -f "$secondary_ports/Alpha One.sh"
test -f "$secondary_ports/Alpha Two Renamed.sh"
test -f "$secondary_ports/alpha-data/game.bin"
test ! -e "$primary_ports/Alpha One.sh"
test ! -e "$primary_ports/alpha-data"
test -f "$secondary_images/Alpha One.png"
python3 - "$secondary_images/.portmaster-artwork.json" <<'PY'
import json
import sys
data = json.load(open(sys.argv[1]))
assert data["entries"]["Alpha One.png"]["source_id"] == "secondary_sd"
PY
test -z "$(find "$userdata/portmaster/.leaf/moves" -name '*.json' -print)"

"$binary" --move-port secondary_sd alpha primary
test -f "$primary_ports/Alpha One.sh"
test ! -e "$secondary_ports/Alpha One.sh"

mkdir "$secondary_ports/ALPHA-DATA"
if "$binary" --move-port primary alpha secondary_sd >"$temp/collision.out" 2>&1; then
  echo "casefold collision unexpectedly moved package" >&2
  exit 1
fi
grep -F 'case-insensitive collision' "$temp/collision.out" >/dev/null
rmdir "$secondary_ports/ALPHA-DATA"

export FAKE_JAWAKA_CAPABILITY=false
if "$binary" --move-port primary alpha secondary_sd >"$temp/capability.out" 2>&1; then
  echo "unsupported daemon unexpectedly moved package" >&2
  exit 1
fi
grep -F 'Update Leaf' "$temp/capability.out" >/dev/null
test -z "$(find "$secondary_ports" -name '.leaf-portmaster-move-*' -print)"
export FAKE_JAWAKA_CAPABILITY=true

export PORTMASTER_MOVE_TEST_INTERRUPT_AFTER=db_committed
if "$binary" --move-port primary alpha secondary_sd >"$temp/interrupted.out" 2>&1; then
  echo "fixture interruption unexpectedly completed" >&2
  exit 1
fi
unset PORTMASTER_MOVE_TEST_INTERRUPT_AFTER
test -n "$(find "$userdata/portmaster/.leaf/moves" -name '*.json' -print -quit)"
test -f "$primary_ports/Alpha One.sh"
test ! -e "$secondary_ports/Alpha One.sh"

"$binary" --recover-port-moves
test -f "$secondary_ports/Alpha One.sh"
test ! -e "$primary_ports/Alpha One.sh"
test -z "$(find "$userdata/portmaster/.leaf/moves" -name '*.json' -print)"

echo "move fixtures: PASS"
