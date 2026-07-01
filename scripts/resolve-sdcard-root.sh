#!/bin/sh
set -eu

platform="${PLATFORM:-mlp1}"
pak_dir="${1:-${PORTMASTER_MLP1_PAK_DIR:-}}"

canonical_dir() {
  [ -n "$1" ] || return 1
  [ -d "$1" ] || return 1
  (CDPATH= cd -- "$1" && pwd)
}

has_leaf_marker() {
  root="${1%/}"
  [ -f "$root/.system/leaf/platforms/$platform/enabled" ] ||
    [ -f "$root/.system/leaf/platforms/$platform/launcher/bin/loong_pangu" ]
}

emit_explicit() {
  label="$1"
  value="$2"
  resolved="$(canonical_dir "$value")" || {
    echo "$label does not exist or is not a directory: $value" >&2
    return 1
  }
  printf '%s\n' "$resolved"
}

if [ -n "${SDCARD_PATH:-}" ]; then
  emit_explicit SDCARD_PATH "$SDCARD_PATH"
  exit $?
fi

if [ -n "${JAWAKA_SDCARD_ROOT:-}" ]; then
  emit_explicit JAWAKA_SDCARD_ROOT "$JAWAKA_SDCARD_ROOT"
  exit $?
fi

if [ -n "$pak_dir" ] && [ -d "$pak_dir" ]; then
  resolved_pak_dir="$(canonical_dir "$pak_dir")"
  case "$resolved_pak_dir" in
    */Apps/*/*.pak)
      canonical_dir "$resolved_pak_dir/../../.."
      exit $?
      ;;
  esac
fi

first_match=""
match_count=0
candidate_roots="${LEAF_PM_SDCARD_CANDIDATES:-/mnt/sdcard /media/sdcard1}"
for candidate in $candidate_roots; do
  if has_leaf_marker "$candidate"; then
    resolved="$(canonical_dir "$candidate")" || continue
    if [ -z "$first_match" ]; then
      first_match="$resolved"
      match_count=1
    elif [ "$resolved" != "$first_match" ]; then
      match_count=$((match_count + 1))
    fi
  fi
done

case "$match_count" in
  1)
    printf '%s\n' "$first_match"
    ;;
  0)
    echo "cannot resolve Leaf SD root; set SDCARD_PATH" >&2
    exit 1
    ;;
  *)
    echo "ambiguous Leaf SD root; set SDCARD_PATH explicitly" >&2
    exit 1
    ;;
esac
