#!/usr/bin/env bash
set -euo pipefail

ports_dir="$1"
printf '%s\n' "$ports_dir" >>"$LEAF_PM_SCAN_FIXTURE_LOG"
mkdir -p \
  "$(dirname "$LEAF_PM_ARMHF_SCAN_TSV")" \
  "$PORTMASTER_CONTROLFOLDER"
printf 'path\taction\n%s\tfixture\n' "$ports_dir" \
  >"$LEAF_PM_ARMHF_SCAN_TSV"
printf '{"ports_dir":"%s","controlfolder":"%s"}\n' \
  "$ports_dir" "$PORTMASTER_CONTROLFOLDER" \
  >"$LEAF_PM_ARMHF_SCAN_JSON"
printf 'fixture|%s\n' "$ports_dir" >"$LEAF_PM_ARMHF_SCAN_MANIFEST"
printf '# fixture %s\n' "$PORTMASTER_MLP1_DATA_DIR" \
  >"$PORTMASTER_CONTROLFOLDER/leaf-armhf-env.sh"

cp "$LEAF_PM_ARMHF_SCAN_TSV" "$LEAF_PM_ARMHF_SCAN_LATEST_TSV"
cp "$LEAF_PM_ARMHF_SCAN_JSON" "$LEAF_PM_ARMHF_SCAN_LATEST_JSON"
cp "$LEAF_PM_ARMHF_SCAN_MANIFEST" "$LEAF_PM_ARMHF_SCAN_LATEST_MANIFEST"
