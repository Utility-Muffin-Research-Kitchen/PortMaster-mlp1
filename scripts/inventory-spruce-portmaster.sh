#!/usr/bin/env bash
set -euo pipefail
export LC_ALL=C

ROOT="${1:-/Volumes/Storage/GitHub/spruceOS/App/PortMaster}"
OUT="${2:-docs/generated/spruce-portmaster-binary-inventory.tsv}"

archive="$ROOT/portmaster.7z"
wheel="$ROOT/pillow_offline/pillow-11.2.1-cp310-cp310-manylinux_2_28_aarch64.whl"

need() {
  command -v "$1" >/dev/null 2>&1 || {
    echo "missing required tool: $1" >&2
    exit 1
  }
}

need bsdtar
need file
need find
need sort
need unzip

if [ ! -f "$archive" ]; then
  echo "missing Spruce PortMaster archive: $archive" >&2
  exit 1
fi
if [ ! -f "$wheel" ]; then
  echo "missing Spruce Pillow wheel: $wheel" >&2
  exit 1
fi

size_of() {
  if stat -f %z "$1" >/dev/null 2>&1; then
    stat -f %z "$1"
  else
    stat -c %s "$1"
  fi
}

sha256_of() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" | awk '{print $1}'
  else
    shasum -a 256 "$1" | awk '{print $1}'
  fi
}

classify_arch() {
  case "$1" in
    *"ARM aarch64"*) echo "aarch64" ;;
    *"ARM, EABI5"*) echo "armhf" ;;
    *"x86-64"*) echo "x86_64" ;;
    *"Intel 80386"*) echo "x86" ;;
    *"POSIX shell script"*) echo "script" ;;
    *"empty"*) echo "placeholder" ;;
    *) echo "unknown" ;;
  esac
}

classify_kind() {
  case "$1" in
    *"ELF "*"executable"*) echo "elf-executable" ;;
    *"ELF "*"shared object"*) echo "elf-shared" ;;
    *"ELF "*"relocatable"*) echo "elf-object" ;;
    *"POSIX shell script"*) echo "script" ;;
    *"empty"*) echo "placeholder" ;;
    *) echo "other" ;;
  esac
}

classify_bucket() {
  artifact="$1"
  rel="$2"
  if [ "$artifact" = "pillow-wheel" ]; then
    echo "pillow-offline-wheel"
  elif [ "${rel#PortMaster/}" != "$rel" ]; then
    echo "upstream-portmaster-payload"
  elif [ "${rel#bin/}" != "$rel" ] || [ "${rel#lib/}" != "$rel" ] || [ "${rel#site-packages/}" != "$rel" ]; then
    echo "ui-python-runtime"
  else
    echo "unknown"
  fi
}

classify_strategy() {
  artifact="$1"
  rel="$2"
  type="$3"
  if [ "$artifact" = "pillow-wheel" ]; then
    echo "fetch-or-build-pillow-wheel"
  elif [ "${rel#PortMaster/}" != "$rel" ]; then
    echo "already-from-upstream-portmaster"
  elif [ "${rel#bin/python}" != "$rel" ] || [ "${rel#lib/libpython}" != "$rel" ] ||
       [ "${rel#lib/python3.10/}" != "$rel" ]; then
    echo "build-cpython-runtime"
  elif [ "${rel#lib/sdl2dll/}" != "$rel" ] || [ "${rel#site-packages/sdl2dll/}" != "$rel" ] ||
       printf '%s' "$type" | grep -q 'libSDL2\\|libavif\\|libdav1d\\|libgme\\|libogg\\|libopus\\|libtiff\\|libwavpack\\|libwebp\\|libxmp'; then
    echo "fetch-pysdl2-dll-or-build-sdl-stack"
  else
    echo "review"
  fi
}

should_inventory() {
  rel="$1"
  type="$2"
  case "$type" in
    *"ELF "*) return 0 ;;
    *"POSIX shell script"*) return 0 ;;
    *"empty"*) ;;
    *) return 1 ;;
  esac
  case "$rel" in
    bin/*|lib/libpython3.10.so) return 0 ;;
  esac
  return 1
}

tmp="$(mktemp -d "${TMPDIR:-/tmp}/spruce-pm-inventory.XXXXXX")"
cleanup() {
  rm -rf "$tmp"
}
trap cleanup EXIT

mkdir -p "$tmp/archive" "$tmp/wheel" "$(dirname "$OUT")"
bsdtar -xf "$archive" -C "$tmp/archive"
unzip -q "$wheel" -d "$tmp/wheel"

{
  printf 'artifact\tpath\tsize\tsha256\tkind\tarch\tbucket\tstrategy\tfile_type\n'

  find "$tmp/archive/portmaster" -type f | sort | while IFS= read -r f; do
    rel="${f#"$tmp/archive/portmaster/"}"
    type="$(file -b "$f" | tr '\t' ' ' | sed 's/[[:space:]][[:space:]]*/ /g')"
    should_inventory "$rel" "$type" || continue
    bucket="$(classify_bucket "portmaster.7z" "$rel")"
    strategy="$(classify_strategy "portmaster.7z" "$rel" "$type")"
    printf 'portmaster.7z\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
      "$rel" "$(size_of "$f")" "$(sha256_of "$f")" \
      "$(classify_kind "$type")" "$(classify_arch "$type")" \
      "$bucket" "$strategy" "$type"
  done

  find "$tmp/wheel" -type f | sort | while IFS= read -r f; do
    rel="pillow_offline/wheel/${f#"$tmp/wheel/"}"
    type="$(file -b "$f" | tr '\t' ' ' | sed 's/[[:space:]][[:space:]]*/ /g')"
    should_inventory "$rel" "$type" || continue
    bucket="$(classify_bucket "pillow-wheel" "$rel")"
    strategy="$(classify_strategy "pillow-wheel" "$rel" "$type")"
    printf 'pillow-11.2.1-cp310-cp310-manylinux_2_28_aarch64.whl\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
      "$rel" "$(size_of "$f")" "$(sha256_of "$f")" \
      "$(classify_kind "$type")" "$(classify_arch "$type")" \
      "$bucket" "$strategy" "$type"
  done
} > "$OUT"

echo "wrote $OUT"
awk -F '\t' 'NR > 1 { count[$7]++ } END { for (bucket in count) print count[bucket], bucket }' "$OUT" | sort -n
