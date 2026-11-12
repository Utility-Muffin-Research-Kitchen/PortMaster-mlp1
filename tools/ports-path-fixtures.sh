#!/usr/bin/env bash
# Checks the scanner's /$directory/ports rewrite (normalize_port_paths_script).
#
# Golden cases: tools/fixtures/ports-path/input/*.sh must turn into
# tools/fixtures/ports-path/expected/*.sh.
#
# With PORTS_CATALOG_DIR pointing at a PortMaster-New checkout's ports/
# folder, every *.sh under it also goes through the rewrite. For each script:
#   - a changed script still passes bash -n,
#   - only HM_PORTS_DIR wraps and double quotes changed,
#   - a second pass reports port-paths-already and changes nothing.
# PORTS_PATH_SHOW_DIFF=1 prints every changed line.
#
# PORTS_PATH_UPDATE_EXPECTED=1 rewrites the expected files from the current
# scanner (review the diff before committing).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SCANNER="$ROOT/scripts/scan-and-fix-port-elfs.sh"
FIXTURES="$ROOT/tools/fixtures/ports-path"
TMP_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/portmaster-ports-path.XXXXXX")"
trap 'rm -rf "$TMP_ROOT"' EXIT

for fn in port_paths_has_unwrapped rewrite_port_paths_generic normalize_port_paths_script; do
  body="$(awk -v fn="$fn" '$0 ~ "^" fn "\\(\\) \\{" { p = 1 } p { print } p && /^\}/ { exit }' "$SCANNER")"
  if [ -z "$body" ]; then
    echo "ports-path fixtures: $fn not found in $SCANNER" >&2
    exit 1
  fi
  eval "$body"
done

failures=0
fail() {
  echo "FAIL: $*" >&2
  failures=$((failures + 1))
}

# Strip what the rewrite may add: HM_PORTS_DIR wraps, the braced spelling and
# double quotes. Everything else must match the input byte for byte.
skeleton() {
  awk '{ print }' "$1" | sed \
    -e 's/\${HM_PORTS_DIR:-\/\$directory\/ports}/\/$directory\/ports/g' \
    -e 's/\/\${directory}\/ports/\/$directory\/ports/g' \
    -e 's/"//g'
}

# check_one <input> <work-file> <label>; sets CHECK_ACTION to the first-pass action.
CHECK_ACTION=""
check_one() {
  local input="$1" work="$2" label="$3" action second before
  cp "$input" "$work"
  action="$(normalize_port_paths_script "$work")"
  case "$action" in
    port-paths-patched|port-paths-already) ;;
    *) fail "$label: unexpected action '$action'" ;;
  esac
  if [ "$action" = port-paths-patched ]; then
    if cmp -s "$input" "$work"; then
      fail "$label: reported patched but nothing changed"
    fi
    if bash -n "$input" >/dev/null 2>&1 && ! bash -n "$work" >/dev/null 2>&1; then
      fail "$label: rewrite broke bash -n"
    fi
  elif ! cmp -s "$input" "$work"; then
    fail "$label: reported already but the file changed"
  fi
  if ! diff -q <(skeleton "$input") <(skeleton "$work") >/dev/null; then
    fail "$label: changed more than HM_PORTS_DIR wraps and quotes"
    diff <(skeleton "$input") <(skeleton "$work") >&2 || true
  fi
  before="$TMP_ROOT/second-pass.before"
  cp "$work" "$before"
  second="$(normalize_port_paths_script "$work")"
  if [ "$second" != port-paths-already ]; then
    fail "$label: second pass reported '$second'"
  fi
  if ! cmp -s "$before" "$work"; then
    fail "$label: second pass changed the file"
    diff "$before" "$work" >&2 || true
  fi
  CHECK_ACTION="$action"
}

golden=0
mkdir -p "$TMP_ROOT/golden"
for input in "$FIXTURES"/input/*.sh; do
  name="$(basename "$input")"
  work="$TMP_ROOT/golden/$name"
  check_one "$input" "$work" "golden $name"
  expected="$FIXTURES/expected/$name"
  if [ "${PORTS_PATH_UPDATE_EXPECTED:-0}" = 1 ]; then
    cp "$work" "$expected"
  elif [ ! -f "$expected" ]; then
    fail "golden $name: missing $expected"
  elif ! cmp -s "$expected" "$work"; then
    fail "golden $name: output differs from expected"
    diff -u "$expected" "$work" >&2 || true
  fi
  if ! bash -n "$work" >/dev/null 2>&1; then
    fail "golden $name: output fails bash -n"
  fi
  golden=$((golden + 1))
done
echo "ports-path fixtures: $golden golden cases checked"

# values.sh only assigns variables, so evaluate it before and after the rewrite.
# 1. Without HM_PORTS_DIR every value matches the original.
# 2. With HM_PORTS_DIR=/alt/ports and directory=roms the rewritten script gives
#    what the original gives with directory=alt (heredoc bodies stay literal,
#    so v13 is checked on its own).
values_of() {
  env -i PATH="$PATH" bash --norc --noprofile -c '
    directory="$1"
    if [ -n "$2" ]; then export HM_PORTS_DIR="$2"; fi
    # shellcheck disable=SC1090
    source "$3"
    printf "%s|" "$v1" "$v2" "$v3" "$v4" "$v5" "$v6" "$v7" "${v8[@]}" "$v9" "$v10" "$v11" "$v12"
    if [ "$4" = with-v13 ]; then printf "%s|" "$v13"; fi
  ' _ "$@"
}
values_in="$FIXTURES/input/values.sh"
values_out="$TMP_ROOT/golden/values.sh"
if [ "$(values_of roms "" "$values_in" with-v13)" != "$(values_of roms "" "$values_out" with-v13)" ]; then
  fail "values.sh: rewritten values differ without HM_PORTS_DIR"
  values_of roms "" "$values_in" with-v13 >&2; echo >&2
  values_of roms "" "$values_out" with-v13 >&2; echo >&2
fi
if [ "$(values_of alt "" "$values_in")" != "$(values_of roms /alt/ports "$values_out")" ]; then
  fail "values.sh: HM_PORTS_DIR did not replace exactly the ports prefix"
  values_of alt "" "$values_in" >&2; echo >&2
  values_of roms /alt/ports "$values_out" >&2; echo >&2
fi
if [ "$(env -i PATH="$PATH" bash --norc --noprofile -c 'directory=roms; HM_PORTS_DIR=/alt/ports; source "$1"; printf %s "$v13"' _ "$values_out")" != /roms/ports/k ]; then
  fail "values.sh: heredoc body was rewritten"
fi

if [ -n "${PORTS_CATALOG_DIR:-}" ]; then
  if [ ! -d "$PORTS_CATALOG_DIR" ]; then
    echo "ports-path fixtures: PORTS_CATALOG_DIR is not a directory: $PORTS_CATALOG_DIR" >&2
    exit 1
  fi
  total=0
  patched=0
  mkdir -p "$TMP_ROOT/catalog"
  while IFS= read -r -d '' input; do
    total=$((total + 1))
    work="$TMP_ROOT/catalog/$total.sh"
    label="${input#"$PORTS_CATALOG_DIR"/}"
    check_one "$input" "$work" "catalog $label"
    if [ "$CHECK_ACTION" = port-paths-patched ]; then
      patched=$((patched + 1))
      if [ "${PORTS_PATH_SHOW_DIFF:-0}" = 1 ]; then
        echo "== $label"
        diff "$input" "$work" | grep '^[<>]' || true
      fi
    fi
  done < <(find "$PORTS_CATALOG_DIR" -type f -name '*.sh' -print0 | sort -z)
  echo "ports-path fixtures: $total catalog scripts checked, $patched rewritten"
fi

if [ "$failures" -ne 0 ]; then
  echo "ports-path fixtures: $failures failure(s)" >&2
  exit 1
fi
echo "PortMaster ports-path fixtures passed"
