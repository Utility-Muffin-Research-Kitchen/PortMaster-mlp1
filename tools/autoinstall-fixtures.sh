#!/usr/bin/env bash
set -euo pipefail

repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
binary="${AUTOINSTALL_TEST_BINARY:-$repo_dir/build/bin/portmaster-mlp1}"
fixture_root="$(mktemp -d "${TMPDIR:-/tmp}/portmaster-autoinstall.XXXXXX")"
trap 'rm -rf "$fixture_root"' EXIT

sd_root="$fixture_root/sd"
userdata="$fixture_root/userdata"
control="$fixture_root/control"
dialog_log="$fixture_root/dialog.log"
mkdir -p "$sd_root" "$userdata" "$control/autoinstall"

run_pm() {
  SDCARD_PATH="$sd_root" \
  USERDATA_PATH="$userdata" \
  PORTMASTER_MLP1_PAK_DIR="$repo_dir" \
    "$binary" "$@"
}

run_pm --install-portmaster >/dev/null
run_pm --repatch-portmaster >/dev/null
run_pm --repatch-portmaster >/dev/null

portmaster_tree="$userdata/portmaster/PortMaster"
block="$fixture_root/autoinstall-block.sh"
awk '
  /^## Autoinstallation Code$/ { copy = 1 }
  /^## To help with testing\.$/ { copy = 0 }
  copy { print }
' "$portmaster_tree/PortMaster.sh" > "$block"
test -s "$block"

cat > "$control/PortMasterDialog.txt" <<'EOF'
PortMasterIPCheck() { printf '%s\n' online; }
PortMasterDialogInit() { :; }
PortMasterDialog() { printf 'dialog:%s\n' "$*" >> "$FIXTURE_DIALOG_LOG"; }
PortMasterDialogResult() {
  printf 'install:%s\n' "$2" >> "$FIXTURE_DIALOG_LOG"
  case "$(basename "$2")" in
    failed.zip) printf '%s\n' FAILURE ;;
    *) printf '%s\n' OKAY ;;
  esac
}
PortMasterDialogMessageBox() { printf 'message:%s\n' "$*" >> "$FIXTURE_DIALOG_LOG"; }
PortMasterDialogExit() { :; }
EOF

run_block() {
  CONTROL="$control" \
  BLOCK="$block" \
  FIXTURE_DIALOG_LOG="$dialog_log" \
  HM_PORTS_DIR="$1" \
    bash -euo pipefail -c '
      controlfolder="$CONTROL"
      ESUDO=
      DEVICE_ARCH=aarch64
      source "$BLOCK"
    '
}

ports_dir="$sd_root/Roms/PORTS"
inbox="$ports_dir/autoinstall"
mkdir -p "$inbox"
printf '%s\n' good > "$inbox/good.zip"
printf '%s\n' failed > "$inbox/failed.zip"
printf '%s\n' managed-update > "$inbox/PortMaster.zip"
cp "$inbox/failed.zip" "$fixture_root/failed.before"
cp "$inbox/PortMaster.zip" "$fixture_root/PortMaster.before"

run_block "$ports_dir"

test ! -e "$inbox/good.zip"
cmp "$fixture_root/failed.before" "$inbox/failed.zip"
cmp "$fixture_root/PortMaster.before" "$inbox/PortMaster.zip"
grep -F "install:$inbox/good.zip" "$dialog_log" >/dev/null
grep -F "install:$inbox/failed.zip" "$dialog_log" >/dev/null
if grep -F "install:$inbox/PortMaster.zip" "$dialog_log" >/dev/null; then
  echo "public PortMaster.zip reached the installer" >&2
  exit 1
fi
grep -F "Use Leaf's Update PortMaster action." "$dialog_log" >/dev/null
grep -F "Copy PortMaster-format port ZIP files here" "$inbox/README.txt" >/dev/null

rm -f "$inbox/PortMaster.zip"
mixed_update="$inbox/pOrTmAsTeR.zip"
printf '%s\n' managed-update > "$mixed_update"
cp "$mixed_update" "$fixture_root/mixed-update.before"
: > "$dialog_log"
run_block "$ports_dir"
cmp "$fixture_root/mixed-update.before" "$mixed_update"
if grep -F "install:$mixed_update" "$dialog_log" >/dev/null; then
  echo "mixed-case public PortMaster.zip reached the installer" >&2
  exit 1
fi
grep -F "SKIPPED: pOrTmAsTeR.zip is managed by Leaf" "$dialog_log" >/dev/null

hidden_update="$control/autoinstall/PortMaster.zip"
printf '%s\n' managed-update > "$hidden_update"
: > "$dialog_log"
run_block "$ports_dir"
test ! -e "$hidden_update"
grep -F "install:$hidden_update" "$dialog_log" >/dev/null

printf '%s\n' custom-readme > "$inbox/README.txt"
run_block "$ports_dir"
grep -Fx custom-readme "$inbox/README.txt" >/dev/null

fallback_root="$fixture_root/fallback"
CONTROL="$control" \
BLOCK="$block" \
FIXTURE_DIALOG_LOG="$dialog_log" \
FALLBACK_DIRECTORY="${fallback_root#/}" \
  env -u HM_PORTS_DIR bash -euo pipefail -c '
    controlfolder="$CONTROL"
    directory="$FALLBACK_DIRECTORY"
    ESUDO=
    DEVICE_ARCH=aarch64
    source "$BLOCK"
  '
test -f "$fallback_root/ports/autoinstall/README.txt"

if ! idempotence="$("$repo_dir/scripts/apply-portmaster-patches.sh" \
  --dry-run "$portmaster_tree")"; then
  printf '%s\n' "$idempotence" >&2
  exit 1
fi
printf '%s\n' "$idempotence" | grep -A1 '0011-leaf-public-autoinstall.patch' | \
  grep -F 'already applied' >/dev/null

echo "PortMaster public autoinstall fixtures passed"
