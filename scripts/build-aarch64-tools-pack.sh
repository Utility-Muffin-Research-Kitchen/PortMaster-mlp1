#!/usr/bin/env bash
set -euo pipefail

ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
LOCK="$ROOT/locks/aarch64-tools.lock.json"
IMAGE="${MLP1_TOOLCHAIN_IMAGE:-ghcr.io/utility-muffin-research-kitchen/mlp1-toolchain:latest}"
BUILD_DIR="$ROOT/build/mlp1"
WORK_DIR="$ROOT/build/aarch64-tools"
OUT_DIR="$BUILD_DIR/compat/tools/aarch64"
LICENSE_DIR="$BUILD_DIR/licenses/rsync"
TOOLCHAIN_ROOT="${MLP1_TOOLCHAIN_ROOT:-/opt/mlp1-toolchain}"
TARGET_PREFIX="$TOOLCHAIN_ROOT/aarch64-buildroot-linux-gnu"
TARGET_SYSROOT="$TARGET_PREFIX/sysroot"
container=0

if [[ "${1:-}" == "--container" ]]; then
  container=1
  shift
fi

if [[ "$container" == "0" ]]; then
  exec docker run --rm \
    -v "$ROOT":/work \
    -w /work \
    "$IMAGE" \
    bash scripts/build-aarch64-tools-pack.sh --container
fi

read_lock() {
  python3 - "$LOCK" "$1" <<'PY'
import json
import sys

path, key = sys.argv[1], sys.argv[2]
with open(path, "r", encoding="utf-8") as f:
    data = json.load(f)

value = data
for part in key.split("."):
    value = value[part]
if isinstance(value, list):
    print("\n".join(value))
else:
    print(value)
PY
}

download_source() {
  local name="$1"
  local url="$2"
  local expected_size="$3"
  local expected_sha256="$4"
  local tarball="$WORK_DIR/downloads/$name"

  if [ ! -f "$tarball" ] || [ "$(wc -c <"$tarball" | tr -d ' ')" != "$expected_size" ]; then
    local tmp="$tarball.tmp.$$"
    curl -fL --retry 3 --connect-timeout 20 -o "$tmp" "$url"
    mv "$tmp" "$tarball"
  fi

  local actual_sha256
  actual_sha256="$(shasum -a 256 "$tarball" | awk '{print $1}')"
  if [ "$actual_sha256" != "$expected_sha256" ]; then
    echo "sha256 mismatch for $name" >&2
    echo "expected: $expected_sha256" >&2
    echo "actual:   $actual_sha256" >&2
    exit 1
  fi

  printf '%s\n' "$tarball"
}

mkdir -p "$WORK_DIR/downloads" "$OUT_DIR/bin" "$OUT_DIR/lib" "$LICENSE_DIR"

container_work="$(mktemp -d /tmp/leaf-aarch64-tools.XXXXXX)"
trap 'rm -rf "$container_work"' EXIT

rsync_version="$(read_lock tools.rsync.version)"
rsync_url="$(read_lock tools.rsync.source.url)"
rsync_expected_size="$(read_lock tools.rsync.source.size)"
rsync_expected_sha256="$(read_lock tools.rsync.source.sha256)"
rsync_tarball="$(download_source "rsync-$rsync_version.tar.gz" "$rsync_url" "$rsync_expected_size" "$rsync_expected_sha256")"
rsync_src_dir="$container_work/rsync-$rsync_version"
mkdir -p "$rsync_src_dir"
tar -xf "$rsync_tarball" -C "$rsync_src_dir" --strip-components=1
(
  cd "$rsync_src_dir"
  ./configure \
    --host=aarch64-buildroot-linux-gnu \
    --prefix=/usr \
    --disable-md2man \
    --disable-ipv6 \
    --disable-openssl \
    --disable-xxhash \
    --disable-zstd \
    --disable-lz4 \
    --disable-acl-support \
    --disable-xattr-support \
    --disable-iconv \
    --with-included-popt \
    --with-included-zlib \
    CC=aarch64-buildroot-linux-gnu-gcc \
    CFLAGS="-Os"
  make -j"$(nproc)" rsync
  aarch64-buildroot-linux-gnu-strip rsync
)

cp -f "$rsync_src_dir/rsync" "$OUT_DIR/bin/rsync"
chmod 755 "$OUT_DIR/bin/rsync"
cp -f "$rsync_src_dir/COPYING" "$LICENSE_DIR/COPYING"

zip_version="$(read_lock tools.zip.version)"
zip_url="$(read_lock tools.zip.source.url)"
zip_expected_size="$(read_lock tools.zip.source.size)"
zip_expected_sha256="$(read_lock tools.zip.source.sha256)"
zip_tarball="$(download_source "zip30.tar.gz" "$zip_url" "$zip_expected_size" "$zip_expected_sha256")"
zip_src_dir="$container_work/zip-$zip_version"
mkdir -p "$zip_src_dir"
tar -xf "$zip_tarball" -C "$zip_src_dir" --strip-components=1
(
  cd "$zip_src_dir"
  make -f unix/Makefile generic CC=aarch64-buildroot-linux-gnu-gcc
  aarch64-buildroot-linux-gnu-strip zip
)

cp -f "$zip_src_dir/zip" "$OUT_DIR/bin/zip"
chmod 755 "$OUT_DIR/bin/zip"
mkdir -p "$BUILD_DIR/licenses/zip"
cp -f "$zip_src_dir/LICENSE" "$BUILD_DIR/licenses/zip/LICENSE"

sed_version="$(read_lock tools.sed.version)"
sed_url="$(read_lock tools.sed.source.url)"
sed_expected_size="$(read_lock tools.sed.source.size)"
sed_expected_sha256="$(read_lock tools.sed.source.sha256)"
sed_tarball="$(download_source "sed-$sed_version.tar.xz" "$sed_url" "$sed_expected_size" "$sed_expected_sha256")"
sed_src_dir="$container_work/sed-$sed_version"
mkdir -p "$sed_src_dir"
tar -xf "$sed_tarball" -C "$sed_src_dir" --strip-components=1
(
  cd "$sed_src_dir"
  ./configure \
    --host=aarch64-buildroot-linux-gnu \
    --prefix=/usr \
    --disable-nls \
    CC=aarch64-buildroot-linux-gnu-gcc \
    CFLAGS="-Os"
  make -j"$(nproc)"
  aarch64-buildroot-linux-gnu-strip sed/sed
)

cp -f "$sed_src_dir/sed/sed" "$OUT_DIR/bin/sed"
chmod 755 "$OUT_DIR/bin/sed"
mkdir -p "$BUILD_DIR/licenses/sed"
cp -f "$sed_src_dir/COPYING" "$BUILD_DIR/licenses/sed/COPYING"

findutils_version="$(read_lock tools.findutils.version)"
findutils_url="$(read_lock tools.findutils.source.url)"
findutils_expected_size="$(read_lock tools.findutils.source.size)"
findutils_expected_sha256="$(read_lock tools.findutils.source.sha256)"
findutils_tarball="$(download_source "findutils-$findutils_version.tar.xz" "$findutils_url" "$findutils_expected_size" "$findutils_expected_sha256")"
findutils_src_dir="$container_work/findutils-$findutils_version"
mkdir -p "$findutils_src_dir"
tar -xf "$findutils_tarball" -C "$findutils_src_dir" --strip-components=1
(
  cd "$findutils_src_dir"
  ./configure \
    --host=aarch64-buildroot-linux-gnu \
    --prefix=/usr \
    --disable-nls \
    CC=aarch64-buildroot-linux-gnu-gcc \
    CFLAGS="-Os"
  make -j"$(nproc)"
  aarch64-buildroot-linux-gnu-strip find/find xargs/xargs
)

cp -f "$findutils_src_dir/find/find" "$OUT_DIR/bin/find"
cp -f "$findutils_src_dir/xargs/xargs" "$OUT_DIR/bin/xargs"
chmod 755 "$OUT_DIR/bin/find" "$OUT_DIR/bin/xargs"
mkdir -p "$BUILD_DIR/licenses/findutils"
cp -f "$findutils_src_dir/COPYING" "$BUILD_DIR/licenses/findutils/COPYING"

grep_version="$(read_lock tools.grep.version)"
grep_url="$(read_lock tools.grep.source.url)"
grep_expected_size="$(read_lock tools.grep.source.size)"
grep_expected_sha256="$(read_lock tools.grep.source.sha256)"
grep_tarball="$(download_source "grep-$grep_version.tar.xz" "$grep_url" "$grep_expected_size" "$grep_expected_sha256")"
grep_src_dir="$container_work/grep-$grep_version"
mkdir -p "$grep_src_dir"
tar -xf "$grep_tarball" -C "$grep_src_dir" --strip-components=1
(
  cd "$grep_src_dir"
  ./configure \
    --host=aarch64-buildroot-linux-gnu \
    --prefix=/usr \
    --disable-nls \
    --disable-perl-regexp \
    CC=aarch64-buildroot-linux-gnu-gcc \
    CFLAGS="-Os"
  make -j"$(nproc)"
  aarch64-buildroot-linux-gnu-strip src/grep
)

cp -f "$grep_src_dir/src/grep" "$OUT_DIR/bin/grep"
chmod 755 "$OUT_DIR/bin/grep"
mkdir -p "$BUILD_DIR/licenses/grep"
cp -f "$grep_src_dir/COPYING" "$BUILD_DIR/licenses/grep/COPYING"

strace_version="$(read_lock tools.strace.version)"
strace_url="$(read_lock tools.strace.source.url)"
strace_expected_size="$(read_lock tools.strace.source.size)"
strace_expected_sha256="$(read_lock tools.strace.source.sha256)"
strace_tarball="$(download_source "strace-$strace_version.tar.xz" "$strace_url" "$strace_expected_size" "$strace_expected_sha256")"
strace_src_dir="$container_work/strace-$strace_version"
mkdir -p "$strace_src_dir"
tar -xf "$strace_tarball" -C "$strace_src_dir" --strip-components=1
(
  cd "$strace_src_dir"
  ./configure \
    --host=aarch64-buildroot-linux-gnu \
    --build=aarch64-buildroot-linux-gnu \
    --prefix=/usr \
    --enable-mpers=no \
    CC=aarch64-buildroot-linux-gnu-gcc \
    CC_FOR_BUILD=aarch64-buildroot-linux-gnu-gcc \
    CFLAGS="-Os"
  make -j"$(nproc)"
  aarch64-buildroot-linux-gnu-strip src/strace
)

cp -f "$strace_src_dir/src/strace" "$OUT_DIR/bin/strace"
chmod 755 "$OUT_DIR/bin/strace"
mkdir -p "$BUILD_DIR/licenses/strace"
cp -f "$strace_src_dir/COPYING" "$BUILD_DIR/licenses/strace/COPYING"

ncurses_version="$(read_lock tools.ncurses.version)"
ncurses_url="$(read_lock tools.ncurses.source.url)"
ncurses_expected_size="$(read_lock tools.ncurses.source.size)"
ncurses_expected_sha256="$(read_lock tools.ncurses.source.sha256)"
ncurses_tarball="$(download_source "ncurses-$ncurses_version.tar.gz" "$ncurses_url" "$ncurses_expected_size" "$ncurses_expected_sha256")"
ncurses_src_dir="$container_work/ncurses-$ncurses_version"
ncurses_prefix="$container_work/ncurses-prefix"
mkdir -p "$ncurses_src_dir"
tar -xf "$ncurses_tarball" -C "$ncurses_src_dir" --strip-components=1
(
  cd "$ncurses_src_dir"
  ./configure \
    --host=aarch64-buildroot-linux-gnu \
    --prefix="$ncurses_prefix" \
    --without-shared \
    --with-normal \
    --without-debug \
    --without-cxx \
    --without-cxx-binding \
    --without-ada \
    --without-manpages \
    --without-progs \
    --without-tests \
    --enable-widec \
    --disable-db-install
  make -j"$(nproc)" libs
  make install.libs install.includes
)

mkdir -p "$BUILD_DIR/licenses/ncurses"
cp -f "$ncurses_src_dir/COPYING" "$BUILD_DIR/licenses/ncurses/COPYING"

dialog_version="$(read_lock tools.dialog.version)"
dialog_url="$(read_lock tools.dialog.source.url)"
dialog_expected_size="$(read_lock tools.dialog.source.size)"
dialog_expected_sha256="$(read_lock tools.dialog.source.sha256)"
dialog_tarball="$(download_source "dialog-$dialog_version.tgz" "$dialog_url" "$dialog_expected_size" "$dialog_expected_sha256")"
dialog_src_dir="$container_work/dialog-$dialog_version"
mkdir -p "$dialog_src_dir"
tar -xf "$dialog_tarball" -C "$dialog_src_dir" --strip-components=1
(
  cd "$dialog_src_dir"
  CPPFLAGS="-I$ncurses_prefix/include -I$ncurses_prefix/include/ncursesw" \
  LDFLAGS="-L$ncurses_prefix/lib" \
  LIBS="-lncursesw" \
    ./configure \
      --host=aarch64-buildroot-linux-gnu \
      --prefix=/usr \
      --with-ncursesw \
      --with-curses-dir="$ncurses_prefix" \
      --disable-nls \
      CC=aarch64-buildroot-linux-gnu-gcc \
      CFLAGS="-Os"
  make -j"$(nproc)" dialog
  aarch64-buildroot-linux-gnu-strip dialog
)

cp -f "$dialog_src_dir/dialog" "$OUT_DIR/bin/dialog"
chmod 755 "$OUT_DIR/bin/dialog"
mkdir -p "$BUILD_DIR/licenses/dialog"
cp -f "$dialog_src_dir/COPYING" "$BUILD_DIR/licenses/dialog/COPYING"

zstd_version="$(read_lock tools.zstd.version)"
zstd_url="$(read_lock tools.zstd.source.url)"
zstd_expected_size="$(read_lock tools.zstd.source.size)"
zstd_expected_sha256="$(read_lock tools.zstd.source.sha256)"
zstd_tarball="$(download_source "zstd-$zstd_version.tar.gz" "$zstd_url" "$zstd_expected_size" "$zstd_expected_sha256")"
zstd_src_dir="$container_work/zstd-$zstd_version"
squashfuse_prefix="$container_work/squashfuse-prefix"
mkdir -p "$zstd_src_dir" "$squashfuse_prefix"
tar -xf "$zstd_tarball" -C "$zstd_src_dir" --strip-components=1
(
  cd "$zstd_src_dir"
  make -C lib -j"$(nproc)" \
    CC=aarch64-buildroot-linux-gnu-gcc \
    AR=aarch64-buildroot-linux-gnu-ar \
    RANLIB=aarch64-buildroot-linux-gnu-ranlib \
    PREFIX="$squashfuse_prefix" \
    BUILD_SHARED=yes \
    BUILD_STATIC=yes \
    install
)
mkdir -p "$BUILD_DIR/licenses/zstd"
cp -f "$zstd_src_dir/LICENSE" "$BUILD_DIR/licenses/zstd/LICENSE"

lz4_version="$(read_lock tools.lz4.version)"
lz4_url="$(read_lock tools.lz4.source.url)"
lz4_expected_size="$(read_lock tools.lz4.source.size)"
lz4_expected_sha256="$(read_lock tools.lz4.source.sha256)"
lz4_tarball="$(download_source "lz4-$lz4_version.tar.gz" "$lz4_url" "$lz4_expected_size" "$lz4_expected_sha256")"
lz4_src_dir="$container_work/lz4-$lz4_version"
mkdir -p "$lz4_src_dir"
tar -xf "$lz4_tarball" -C "$lz4_src_dir" --strip-components=1
(
  cd "$lz4_src_dir"
  make -C lib -j"$(nproc)" \
    CC=aarch64-buildroot-linux-gnu-gcc \
    AR=aarch64-buildroot-linux-gnu-ar \
    PREFIX="$squashfuse_prefix" \
    BUILD_SHARED=yes \
    BUILD_STATIC=yes \
    install
)
mkdir -p "$BUILD_DIR/licenses/lz4"
cp -f "$lz4_src_dir/lib/LICENSE" "$BUILD_DIR/licenses/lz4/LICENSE"

xz_version="$(read_lock tools.xz.version)"
xz_url="$(read_lock tools.xz.source.url)"
xz_expected_size="$(read_lock tools.xz.source.size)"
xz_expected_sha256="$(read_lock tools.xz.source.sha256)"
xz_tarball="$(download_source "xz-$xz_version.tar.gz" "$xz_url" "$xz_expected_size" "$xz_expected_sha256")"
xz_src_dir="$container_work/xz-$xz_version"
mkdir -p "$xz_src_dir"
tar -xf "$xz_tarball" -C "$xz_src_dir" --strip-components=1
(
  cd "$xz_src_dir"
  ./configure \
    --host=aarch64-buildroot-linux-gnu \
    --prefix="$squashfuse_prefix" \
    --disable-nls \
    --disable-xz \
    --disable-xzdec \
    --disable-lzmadec \
    --disable-lzmainfo \
    --disable-doc \
    --disable-scripts \
    --enable-shared \
    --enable-static \
    CC=aarch64-buildroot-linux-gnu-gcc \
    CFLAGS="-Os"
  make -j"$(nproc)"
  make install
)
mkdir -p "$BUILD_DIR/licenses/xz"
cp -f "$xz_src_dir/COPYING" "$BUILD_DIR/licenses/xz/COPYING"

libfuse_version="$(read_lock tools.libfuse.version)"
libfuse_url="$(read_lock tools.libfuse.source.url)"
libfuse_expected_size="$(read_lock tools.libfuse.source.size)"
libfuse_expected_sha256="$(read_lock tools.libfuse.source.sha256)"
libfuse_tarball="$(download_source "fuse-$libfuse_version.tar.gz" "$libfuse_url" "$libfuse_expected_size" "$libfuse_expected_sha256")"
libfuse_src_dir="$container_work/fuse-$libfuse_version"
libfuse_build_dir="$container_work/fuse-build"
libfuse_dest_dir="$container_work/fuse-dest"
meson_cross_file="$container_work/aarch64-meson-cross.txt"
mkdir -p "$libfuse_src_dir"
tar -xf "$libfuse_tarball" -C "$libfuse_src_dir" --strip-components=1
cat >"$meson_cross_file" <<EOF
[binaries]
c = 'aarch64-buildroot-linux-gnu-gcc'
ar = 'aarch64-buildroot-linux-gnu-ar'
strip = 'aarch64-buildroot-linux-gnu-strip'
pkg-config = '$TOOLCHAIN_ROOT/bin/pkg-config'

[host_machine]
system = 'linux'
cpu_family = 'aarch64'
cpu = 'aarch64'
endian = 'little'

[properties]
sys_root = '$TARGET_SYSROOT'
pkg_config_libdir = '$TARGET_SYSROOT/usr/lib/pkgconfig'
EOF
meson setup "$libfuse_build_dir" "$libfuse_src_dir" \
  --cross-file "$meson_cross_file" \
  --prefix="$squashfuse_prefix" \
  --libdir=lib \
  --buildtype=minsize \
  -Dexamples=false \
  -Dtests=false \
  -Dutils=false \
  -Duseroot=false \
  -Dudevrulesdir=/tmp \
  -Dinitscriptdir= \
  -Ddisable-mtab=true
ninja -C "$libfuse_build_dir"
DESTDIR="$libfuse_dest_dir" ninja -C "$libfuse_build_dir" install
cp -a "$libfuse_dest_dir$squashfuse_prefix"/. "$squashfuse_prefix"/
mkdir -p "$BUILD_DIR/licenses/libfuse"
cp -f "$libfuse_src_dir/LICENSE" "$BUILD_DIR/licenses/libfuse/LICENSE"

squashfuse_version="$(read_lock tools.squashfuse.version)"
squashfuse_url="$(read_lock tools.squashfuse.source.url)"
squashfuse_expected_size="$(read_lock tools.squashfuse.source.size)"
squashfuse_expected_sha256="$(read_lock tools.squashfuse.source.sha256)"
squashfuse_tarball="$(download_source "squashfuse-$squashfuse_version.tar.gz" "$squashfuse_url" "$squashfuse_expected_size" "$squashfuse_expected_sha256")"
squashfuse_src_dir="$container_work/squashfuse-$squashfuse_version"
mkdir -p "$squashfuse_src_dir"
tar -xf "$squashfuse_tarball" -C "$squashfuse_src_dir" --strip-components=1
(
  cd "$squashfuse_src_dir"
  ./autogen.sh
  PKG_CONFIG_PATH="$squashfuse_prefix/lib/pkgconfig" \
  CPPFLAGS="-I$squashfuse_prefix/include -I$squashfuse_prefix/include/fuse3" \
  LDFLAGS="-L$squashfuse_prefix/lib" \
    ./configure \
      --host=aarch64-buildroot-linux-gnu \
      --prefix="$squashfuse_prefix" \
      --disable-demo \
      --disable-high-level \
      --enable-multithreading \
      CC=aarch64-buildroot-linux-gnu-gcc \
      CFLAGS="-Os"
  make -j"$(nproc)"
  aarch64-buildroot-linux-gnu-strip squashfuse_ll
)
mkdir -p "$BUILD_DIR/licenses/squashfuse"
cp -f "$squashfuse_src_dir/LICENSE" "$BUILD_DIR/licenses/squashfuse/LICENSE"

cp -f "$squashfuse_src_dir/squashfuse_ll" "$OUT_DIR/bin/squashfuse_ll.bin"
patchelf --set-rpath '$ORIGIN/../lib' "$OUT_DIR/bin/squashfuse_ll.bin"
cp -f "$squashfuse_prefix/lib/libfuse3.so.$libfuse_version" "$OUT_DIR/lib/libfuse3.so.4"
cp -f "$squashfuse_prefix/lib/libzstd.so.$zstd_version" "$OUT_DIR/lib/libzstd.so.1"
cp -f "$squashfuse_prefix/lib/liblz4.so.$lz4_version" "$OUT_DIR/lib/liblz4.so.1"
cp -f "$squashfuse_prefix/lib/liblzma.so.$xz_version" "$OUT_DIR/lib/liblzma.so.5"
aarch64-buildroot-linux-gnu-strip \
  "$OUT_DIR/lib/libfuse3.so.4" \
  "$OUT_DIR/lib/libzstd.so.1" \
  "$OUT_DIR/lib/liblz4.so.1" \
  "$OUT_DIR/lib/liblzma.so.5" 2>/dev/null || true

cat >"$OUT_DIR/bin/squashfuse" <<'EOF'
#!/bin/sh
# Leaf PortMaster app-local squashfuse wrapper. Libraries live beside the
# optional pak's tools, so the stock OS library search path is left untouched.
_leaf_dir="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
_leaf_lib_dir="$(CDPATH= cd -- "$_leaf_dir/../lib" && pwd)"
LD_LIBRARY_PATH="$_leaf_lib_dir${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export LD_LIBRARY_PATH
exec "$_leaf_dir/squashfuse_ll.bin" "$@"
EOF
chmod 755 "$OUT_DIR/bin/squashfuse" "$OUT_DIR/bin/squashfuse_ll.bin" \
  "$OUT_DIR/lib/libfuse3.so.4" "$OUT_DIR/lib/libzstd.so.1" \
  "$OUT_DIR/lib/liblz4.so.1" "$OUT_DIR/lib/liblzma.so.5"

cat >"$OUT_DIR/bin/sudo" <<'EOF'
#!/bin/sh
# Leaf PortMaster app-local sudo shim. The MLP1 launch session already runs as
# root, so this strips common sudo options and executes the requested command.
# It also gives PortMaster runtime squashfs downloads and mounts one SD-local
# compatibility preflight before the kernel can fail with a cryptic mount error.
while [ "$#" -gt 0 ]; do
  case "$1" in
    --)
      shift
      break
      ;;
    --preserve-env|--preserve-env=*|-E|-n|-S|-H)
      shift
      ;;
    -u|-g|-C|-p)
      shift
      [ "$#" -gt 0 ] && shift
      ;;
    -v|-k|-K)
      exit 0
      ;;
    -*)
      shift
      ;;
    *)
      break
      ;;
  esac
done

[ "$#" -gt 0 ] || exit 0
_leaf_sudo_dir="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
_leaf_squashfs_check="$_leaf_sudo_dir/leaf-squashfs-check"
_leaf_squashfuse="$_leaf_sudo_dir/squashfuse"

_leaf_fat32_limit() {
  case "${LEAF_PM_FAT32_FILE_LIMIT_BYTES:-}" in
    ""|*[!0-9]*) printf '%s\n' 4294967296 ;;
    *) printf '%s\n' "$LEAF_PM_FAT32_FILE_LIMIT_BYTES" ;;
  esac
}

_leaf_mount_type_for_path() {
  _leaf_path="$1"
  _leaf_best_mount=""
  _leaf_best_type=""
  [ -r /proc/mounts ] || return 1
  while read -r _leaf_dev _leaf_mount _leaf_type _leaf_rest; do
    case "$_leaf_path" in
      "$_leaf_mount"|"$_leaf_mount"/*)
        if [ "${#_leaf_mount}" -gt "${#_leaf_best_mount}" ]; then
          _leaf_best_mount="$_leaf_mount"
          _leaf_best_type="$_leaf_type"
        fi
        ;;
    esac
  done </proc/mounts
  [ -n "$_leaf_best_type" ] || return 1
  printf '%s\n' "$_leaf_best_type"
}

_leaf_file_size() {
  [ -f "$1" ] || return 1
  wc -c <"$1" 2>/dev/null | tr -d ' '
}

_leaf_download_log() {
  _leaf_msg="$1"
  _leaf_base="${LEAF_PM_DATA_DIR:-${XDG_DATA_HOME:-}}"
  [ -n "$_leaf_base" ] || return 0
  _leaf_log_dir="$_leaf_base/.leaf/logs"
  mkdir -p "$_leaf_log_dir" 2>/dev/null || return 0
  printf '%s %s\n' "$(date +%s 2>/dev/null || printf 0)" "$_leaf_msg" >>"$_leaf_log_dir/download.log" 2>/dev/null || true
}

_leaf_report_runtime_fat32_failure() {
  _leaf_runtime="$1"
  [ -n "$_leaf_runtime" ] || return 0
  [ -f "$_leaf_runtime" ] || return 0
  [ "$(_leaf_mount_type_for_path "$_leaf_runtime" 2>/dev/null || true)" = "vfat" ] || return 0
  _leaf_limit="$(_leaf_fat32_limit)"
  if [ "$_leaf_limit" -gt 1048576 ]; then
    _leaf_near_limit=$((_leaf_limit - 1048576))
  else
    _leaf_near_limit=0
  fi
  _leaf_size="$(_leaf_file_size "$_leaf_runtime" || printf 0)"
  case "$_leaf_size" in
    ""|*[!0-9]*) return 0 ;;
  esac
  [ "$_leaf_size" -ge "$_leaf_near_limit" ] || return 0
  _leaf_msg="Leaf PortMaster: runtime_check failed near the FAT32 single-file limit; '${_leaf_runtime##*/}' is $_leaf_size bytes, and stock MLP1 vfat cannot store single files >= $_leaf_limit bytes"
  echo "$_leaf_msg" >&2
  _leaf_download_log "$_leaf_msg"
}

_leaf_squashfs_status() {
  [ -x "$_leaf_squashfs_check" ] || return 0
  LEAF_PM_SQUASHFS_CHECK_QUIET=1 "$_leaf_squashfs_check" "$1"
}

_leaf_report_squashfs_status() {
  [ -x "$_leaf_squashfs_check" ] || return 0
  "$_leaf_squashfs_check" "$1"
}

_leaf_find_squashfs_mount_args() {
  _leaf_mount_img=""
  _leaf_mount_target=""
  _leaf_skip_next=0
  _leaf_seen_mount=0

  for _leaf_arg in "$@"; do
    if [ "$_leaf_seen_mount" -eq 0 ]; then
      _leaf_seen_mount=1
      continue
    fi
    if [ "$_leaf_skip_next" -eq 1 ]; then
      _leaf_skip_next=0
      continue
    fi
    case "$_leaf_arg" in
      -o|-t|-U|-L|--options|--types)
        _leaf_skip_next=1
        continue
        ;;
      -*)
        continue
        ;;
    esac

    if [ -z "$_leaf_mount_img" ]; then
      case "$_leaf_arg" in
        *.squashfs)
          _leaf_mount_img="$_leaf_arg"
          continue
          ;;
      esac
    elif [ -z "$_leaf_mount_target" ]; then
      _leaf_mount_target="$_leaf_arg"
      break
    fi
  done
}

case "${1##*/}" in
  harbourmaster)
    _leaf_runtime=""
    _leaf_next_is_runtime=0
    for _leaf_arg in "$@"; do
      if [ "$_leaf_next_is_runtime" -eq 1 ]; then
        _leaf_runtime="$_leaf_arg"
        break
      fi
      if [ "$_leaf_arg" = "runtime_check" ]; then
        _leaf_next_is_runtime=1
      fi
    done
    "$@"
    _leaf_rc=$?
    if [ "$_leaf_rc" -ne 0 ] && [ -n "$_leaf_runtime" ]; then
      _leaf_report_runtime_fat32_failure "$_leaf_runtime"
    fi
    if [ "$_leaf_rc" -eq 0 ] && [ -n "$_leaf_runtime" ] && [ -x "$_leaf_squashfs_check" ]; then
      _leaf_squashfs_status "$_leaf_runtime"
      _leaf_check_rc=$?
      case "$_leaf_check_rc" in
        0)
          ;;
        66)
          if [ ! -x "$_leaf_squashfuse" ]; then
            _leaf_report_squashfs_status "$_leaf_runtime" || exit $?
          fi
          ;;
        *)
          _leaf_report_squashfs_status "$_leaf_runtime" || exit $?
          ;;
      esac
    fi
    exit "$_leaf_rc"
    ;;
  mount)
    if [ -x "$_leaf_squashfs_check" ]; then
      _leaf_find_squashfs_mount_args "$@"
      if [ -n "$_leaf_mount_img" ]; then
        _leaf_squashfs_status "$_leaf_mount_img"
        _leaf_check_rc=$?
        case "$_leaf_check_rc" in
          0)
            ;;
          66)
            if [ -x "$_leaf_squashfuse" ] && [ -n "$_leaf_mount_target" ]; then
              echo "Leaf PortMaster: kernel cannot mount '${_leaf_mount_img##*/}'; using app-local squashfuse at $_leaf_mount_target" >&2
              exec "$_leaf_squashfuse" -o ro "$_leaf_mount_img" "$_leaf_mount_target"
            fi
            _leaf_report_squashfs_status "$_leaf_mount_img" || exit $?
            ;;
          *)
            _leaf_report_squashfs_status "$_leaf_mount_img" || exit $?
            ;;
        esac
      fi
    fi
    ;;
esac
exec "$@"
EOF

cat >"$OUT_DIR/bin/leaf-squashfs-check" <<'EOF'
#!/bin/sh
# Leaf PortMaster squashfs runtime preflight. This is intentionally tiny and
# POSIX-shell only: it must run inside arbitrary port scripts before mount.

_leaf_pm_name_for_id() {
  case "$1" in
    1) printf '%s\n' gzip ;;
    2) printf '%s\n' lzma ;;
    3) printf '%s\n' lzo ;;
    4) printf '%s\n' xz ;;
    5) printf '%s\n' lz4 ;;
    6) printf '%s\n' zstd ;;
    *) printf '%s\n' unknown ;;
  esac
}

_leaf_pm_symbol_for_id() {
  case "$1" in
    1) printf '%s\n' CONFIG_SQUASHFS_ZLIB ;;
    2) printf '%s\n' CONFIG_SQUASHFS_LZMA ;;
    3) printf '%s\n' CONFIG_SQUASHFS_LZO ;;
    4) printf '%s\n' CONFIG_SQUASHFS_XZ ;;
    5) printf '%s\n' CONFIG_SQUASHFS_LZ4 ;;
    6) printf '%s\n' CONFIG_SQUASHFS_ZSTD ;;
    *) return 1 ;;
  esac
}

_leaf_pm_resolve_squashfs() {
  _leaf_pm_runtime="$1"
  case "$_leaf_pm_runtime" in
    */*) printf '%s\n' "$_leaf_pm_runtime"; return 0 ;;
  esac

  case "$_leaf_pm_runtime" in
    *.squashfs) ;;
    *) _leaf_pm_runtime="$_leaf_pm_runtime.squashfs" ;;
  esac

  _leaf_pm_control="${PORTMASTER_CONTROLFOLDER:-}"
  if [ -z "$_leaf_pm_control" ] && [ -n "${LEAF_PM_DATA_DIR:-}" ]; then
    _leaf_pm_control="$LEAF_PM_DATA_DIR/PortMaster"
  fi
  if [ -z "$_leaf_pm_control" ] && [ -n "${PORTMASTER_MLP1_DATA_DIR:-}" ]; then
    _leaf_pm_control="$PORTMASTER_MLP1_DATA_DIR/PortMaster"
  fi
  if [ -z "$_leaf_pm_control" ] && [ -n "${SDCARD_PATH:-}" ]; then
    _leaf_pm_control="${SDCARD_PATH%/}/.userdata/${PLATFORM:-mlp1}/portmaster/PortMaster"
  fi

  if [ -n "$_leaf_pm_control" ]; then
    printf '%s\n' "$_leaf_pm_control/libs/$_leaf_pm_runtime"
  else
    printf '%s\n' "$_leaf_pm_runtime"
  fi
}

_leaf_pm_config_has() {
  _leaf_pm_symbol="$1"
  [ -f /proc/config.gz ] || return 2
  zcat /proc/config.gz 2>/dev/null | grep -Eq "^${_leaf_pm_symbol}=(y|m)$"
}

_leaf_pm_log() {
  [ "${LEAF_PM_SQUASHFS_CHECK_QUIET:-0}" = "1" ] && return 0
  echo "$*" >&2
}

if [ "$#" -lt 1 ]; then
  _leaf_pm_log "Leaf PortMaster: leaf-squashfs-check requires a runtime image path"
  exit 64
fi

_leaf_pm_path="$(_leaf_pm_resolve_squashfs "$1")"
if [ ! -f "$_leaf_pm_path" ]; then
  # Missing files are left to harbourmaster/mount so their normal error and
  # download behavior remains unchanged.
  exit 0
fi

set -- $(od -An -tu1 -N22 "$_leaf_pm_path" 2>/dev/null)
if [ "$#" -lt 22 ]; then
  _leaf_pm_log "Leaf PortMaster: cannot read squashfs header: $_leaf_pm_path"
  exit 65
fi
if [ "$1" != "104" ] || [ "$2" != "115" ] || [ "$3" != "113" ] || [ "$4" != "115" ]; then
  _leaf_pm_log "Leaf PortMaster: bad squashfs magic: $_leaf_pm_path"
  exit 65
fi

shift 20
_leaf_pm_id=$(( $1 + ($2 * 256) ))
_leaf_pm_name="$(_leaf_pm_name_for_id "$_leaf_pm_id")"
_leaf_pm_symbol="$(_leaf_pm_symbol_for_id "$_leaf_pm_id" 2>/dev/null || true)"

if [ -z "$_leaf_pm_symbol" ]; then
  _leaf_pm_log "Leaf PortMaster: runtime squashfs '${_leaf_pm_path##*/}' uses unknown compression id $_leaf_pm_id; refusing a likely-broken mount."
  exit 65
fi

if _leaf_pm_config_has "$_leaf_pm_symbol"; then
  exit 0
fi
_leaf_pm_config_rc=$?
if [ "$_leaf_pm_config_rc" -eq 2 ]; then
  # If the config is unreadable, avoid false negatives and let the kernel mount
  # attempt be the source of truth.
  exit 0
fi

_leaf_pm_log "Leaf PortMaster: runtime squashfs '${_leaf_pm_path##*/}' uses $_leaf_pm_name (id $_leaf_pm_id), but this kernel lacks $_leaf_pm_symbol; app-local squashfuse fallback is required."
exit 66
EOF

cat >"$OUT_DIR/bin/doas" <<'EOF'
#!/bin/sh
# Leaf PortMaster app-local doas shim. This mirrors the sudo shim for ports or
# scripts that prefer doas on root-only devices.
while [ "$#" -gt 0 ]; do
  case "$1" in
    --)
      shift
      break
      ;;
    -n|-s)
      shift
      ;;
    -u|-C)
      shift
      [ "$#" -gt 0 ] && shift
      ;;
    -*)
      shift
      ;;
    *)
      break
      ;;
  esac
done

[ "$#" -gt 0 ] || exit 0
exec "$@"
EOF

cat >"$OUT_DIR/bin/systemctl" <<'EOF'
#!/bin/sh
# Leaf PortMaster app-local systemctl shim. MLP1 stock firmware does not run
# systemd; upstream PortMaster only uses these restart calls as CFW hints.
case "${1:-}" in
  --version|-V)
    echo "systemctl leaf-portmaster-shim 0"
    exit 0
    ;;
  restart|start|stop|try-restart|reload|daemon-reload)
    exit 0
    ;;
  is-active|is-enabled|status)
    exit 3
    ;;
  "")
    exit 0
    ;;
  *)
    echo "leaf-systemctl-shim: ignoring unsupported systemctl command: $*" >&2
    exit 0
    ;;
esac
EOF

cat >"$OUT_DIR/bin/dos2unix" <<'EOF'
#!/bin/sh
# Leaf PortMaster app-local dos2unix shim. Covers the common PortMaster patch
# flow: convert CRLF text files in place or with the dos2unix -n SRC DST form.

convert_to() {
  src="$1"
  dst="$2"
  if [ ! -f "$src" ]; then
    echo "dos2unix: $src: No such file" >&2
    return 1
  fi
  tmp="${dst}.leaf-dos2unix.$$"
  trap 'rm -f "$tmp"' EXIT HUP INT TERM
  cp -p "$src" "$tmp" 2>/dev/null || :
  tr -d '\r' <"$src" >"$tmp" || return 1
  mv "$tmp" "$dst"
}

oldfile=1
status=0
while [ "$#" -gt 0 ]; do
  case "$1" in
    --version|-V)
      echo "dos2unix leaf-portmaster-shim 1"
      exit 0
      ;;
    --help|-h)
      echo "usage: dos2unix [options] FILE... | dos2unix -n SRC DST"
      exit 0
      ;;
    --)
      shift
      break
      ;;
    -n|--newfile)
      oldfile=0
      shift
      break
      ;;
    -o|--oldfile|-q|--quiet|-k|--keepdate|-f|--force|-s|--safe)
      shift
      ;;
    -c|--convmode)
      shift
      [ "$#" -gt 0 ] && shift
      ;;
    -*)
      shift
      ;;
    *)
      break
      ;;
  esac
done

if [ "$oldfile" -eq 0 ]; then
  if [ "$#" -lt 2 ]; then
    echo "dos2unix: -n requires SRC and DST" >&2
    exit 64
  fi
  convert_to "$1" "$2"
  exit $?
fi

if [ "$#" -eq 0 ]; then
  echo "dos2unix: no files given" >&2
  exit 64
fi

for path in "$@"; do
  convert_to "$path" "$path" || status=1
done
exit "$status"
EOF

chmod 755 "$OUT_DIR/bin/sudo" "$OUT_DIR/bin/doas" "$OUT_DIR/bin/systemctl" "$OUT_DIR/bin/dos2unix" "$OUT_DIR/bin/leaf-squashfs-check"

cat >"$OUT_DIR/bin/xdelta3" <<'EOF'
#!/bin/sh
# Leaf PortMaster app-local xdelta3 shim. Upstream PortMaster ships the real
# aarch64 xdelta3 binary in its SD-local control folder.
control="${PORTMASTER_CONTROLFOLDER:-}"
if [ -z "$control" ] && [ -n "${LEAF_PM_DATA_DIR:-}" ]; then
  control="$LEAF_PM_DATA_DIR/PortMaster"
fi
if [ -z "$control" ] && [ -n "${PORTMASTER_MLP1_DATA_DIR:-}" ]; then
  control="$PORTMASTER_MLP1_DATA_DIR/PortMaster"
fi
if [ -z "$control" ] && [ -n "${SDCARD_PATH:-}" ]; then
  control="${SDCARD_PATH%/}/.userdata/${PLATFORM:-mlp1}/portmaster/PortMaster"
fi

tool="$control/xdelta3"
if [ -n "$control" ] && [ -x "$tool" ]; then
  exec "$tool" "$@"
fi

echo "leaf-xdelta3-shim: PortMaster xdelta3 binary not found; install PortMaster first" >&2
exit 127
EOF

cat >"$OUT_DIR/bin/7z" <<'EOF'
#!/bin/sh
# Leaf PortMaster app-local 7z shim. Upstream PortMaster ships 7zzs.aarch64 in
# its SD-local control folder; expose it under the common 7z/7za names.
control="${PORTMASTER_CONTROLFOLDER:-}"
if [ -z "$control" ] && [ -n "${LEAF_PM_DATA_DIR:-}" ]; then
  control="$LEAF_PM_DATA_DIR/PortMaster"
fi
if [ -z "$control" ] && [ -n "${PORTMASTER_MLP1_DATA_DIR:-}" ]; then
  control="$PORTMASTER_MLP1_DATA_DIR/PortMaster"
fi
if [ -z "$control" ] && [ -n "${SDCARD_PATH:-}" ]; then
  control="${SDCARD_PATH%/}/.userdata/${PLATFORM:-mlp1}/portmaster/PortMaster"
fi

tool="$control/7zzs.aarch64"
if [ -n "$control" ] && [ -x "$tool" ]; then
  exec "$tool" "$@"
fi

echo "leaf-7z-shim: PortMaster 7zzs.aarch64 binary not found; install PortMaster first" >&2
exit 127
EOF
cp -f "$OUT_DIR/bin/7z" "$OUT_DIR/bin/7za"

cat >"$OUT_DIR/bin/innoextract" <<'EOF'
#!/bin/sh
# Leaf PortMaster app-local innoextract shim. Upstream PortMaster ships the real
# aarch64 innoextract binary in its SD-local control folder.
control="${PORTMASTER_CONTROLFOLDER:-}"
if [ -z "$control" ] && [ -n "${LEAF_PM_DATA_DIR:-}" ]; then
  control="$LEAF_PM_DATA_DIR/PortMaster"
fi
if [ -z "$control" ] && [ -n "${PORTMASTER_MLP1_DATA_DIR:-}" ]; then
  control="$PORTMASTER_MLP1_DATA_DIR/PortMaster"
fi
if [ -z "$control" ] && [ -n "${SDCARD_PATH:-}" ]; then
  control="${SDCARD_PATH%/}/.userdata/${PLATFORM:-mlp1}/portmaster/PortMaster"
fi

tool="$control/innoextract.aarch64"
if [ -n "$control" ] && [ -x "$tool" ]; then
  exec "$tool" "$@"
fi

echo "leaf-innoextract-shim: PortMaster innoextract.aarch64 binary not found; install PortMaster first" >&2
exit 127
EOF

chmod 755 "$OUT_DIR/bin/xdelta3" "$OUT_DIR/bin/7z" "$OUT_DIR/bin/7za" "$OUT_DIR/bin/innoextract"

for tool_path in \
  "$TARGET_SYSROOT/usr/bin/getconf" \
  "$TARGET_SYSROOT/usr/bin/ldd" \
  "$TARGET_PREFIX/bin/readelf"; do
  if [ ! -f "$tool_path" ]; then
    echo "missing target toolchain artifact: $tool_path" >&2
    exit 1
  fi
done

cp -f "$TARGET_SYSROOT/usr/bin/getconf" "$OUT_DIR/bin/getconf"
cp -f "$TARGET_SYSROOT/usr/bin/ldd" "$OUT_DIR/bin/ldd"
cp -f "$TARGET_PREFIX/bin/readelf" "$OUT_DIR/bin/readelf"
aarch64-buildroot-linux-gnu-strip "$OUT_DIR/bin/getconf" "$OUT_DIR/bin/readelf" 2>/dev/null || true

cat >"$OUT_DIR/bin/file" <<'EOF'
#!/bin/sh
# Leaf PortMaster app-local file(1) shim. This intentionally covers the common
# PortMaster/debug cases without carrying a full libmagic database.

show_version() {
  echo "file leaf-portmaster-shim 1"
}

brief=0
paths=""

while [ "$#" -gt 0 ]; do
  case "$1" in
    --version|-v)
      show_version
      exit 0
      ;;
    -b|--brief)
      brief=1
      shift
      ;;
    -L|--dereference|-h|--no-dereference)
      shift
      ;;
    --)
      shift
      break
      ;;
    -*)
      echo "file: unsupported option: $1" >&2
      exit 64
      ;;
    *)
      break
      ;;
  esac
done

describe_elf() {
  path="$1"
  ident="$(dd if="$path" bs=1 skip=4 count=16 2>/dev/null | od -An -tx1 | tr -d ' \n')"
  class="${ident%??????????????????????????????}"
  rest="${ident#??}"
  data="${rest%????????????????????????????}"
  machine="$(dd if="$path" bs=1 skip=18 count=2 2>/dev/null | od -An -tx1 | tr -d ' \n')"

  case "$class" in
    01) bits="32-bit" ;;
    02) bits="64-bit" ;;
    *) bits="unknown-class" ;;
  esac
  case "$data" in
    01) endian="LSB" ;;
    02) endian="MSB" ;;
    *) endian="unknown-endian" ;;
  esac
  case "$machine" in
    b700) arch="ARM aarch64" ;;
    2800) arch="ARM" ;;
    3e00) arch="x86-64" ;;
    0300) arch="Intel 80386" ;;
    *) arch="machine 0x$machine" ;;
  esac

  printf 'ELF %s %s executable, %s\n' "$bits" "$endian" "$arch"
}

describe_one() {
  path="$1"
  if [ ! -e "$path" ]; then
    echo "cannot open '$path' (No such file or directory)"
    return 1
  fi
  if [ -d "$path" ]; then
    echo "directory"
    return 0
  fi
  if [ ! -s "$path" ]; then
    echo "empty"
    return 0
  fi

  magic4="$(dd if="$path" bs=1 count=4 2>/dev/null | od -An -tx1 | tr -d ' \n')"
  magic2="${magic4%????}"
  case "$magic4" in
    7f454c46)
      describe_elf "$path"
      return 0
      ;;
    504b0304)
      echo "Zip archive data"
      return 0
      ;;
    1f8b0800|1f8b0808)
      echo "gzip compressed data"
      return 0
      ;;
  esac
  case "$magic2" in
    2321)
      first="$(sed -n '1{s/[[:cntrl:]]//g;p;q;}' "$path" 2>/dev/null)"
      echo "${first:-script text executable}"
      return 0
      ;;
  esac

  if dd if="$path" bs=512 count=1 2>/dev/null |
     LC_ALL=C grep -q '[^[:print:][:space:]]'; then
    echo "data"
  else
    echo "ASCII text"
  fi
}

status=0
for path in "$@"; do
  if [ "$brief" -eq 0 ]; then
    printf '%s: ' "$path"
  fi
  describe_one "$path" || status=1
done
exit "$status"
EOF

chmod 755 "$OUT_DIR/bin/getconf" "$OUT_DIR/bin/ldd" "$OUT_DIR/bin/readelf" "$OUT_DIR/bin/file"

rsync_binary_sha256="$(shasum -a 256 "$OUT_DIR/bin/rsync" | awk '{print $1}')"
rsync_binary_size="$(wc -c <"$OUT_DIR/bin/rsync" | tr -d ' ')"
rsync_license_sha256="$(shasum -a 256 "$LICENSE_DIR/COPYING" | awk '{print $1}')"
zip_binary_sha256="$(shasum -a 256 "$OUT_DIR/bin/zip" | awk '{print $1}')"
zip_binary_size="$(wc -c <"$OUT_DIR/bin/zip" | tr -d ' ')"
zip_license_sha256="$(shasum -a 256 "$BUILD_DIR/licenses/zip/LICENSE" | awk '{print $1}')"
sed_binary_sha256="$(shasum -a 256 "$OUT_DIR/bin/sed" | awk '{print $1}')"
sed_binary_size="$(wc -c <"$OUT_DIR/bin/sed" | tr -d ' ')"
sed_license_sha256="$(shasum -a 256 "$BUILD_DIR/licenses/sed/COPYING" | awk '{print $1}')"
find_binary_sha256="$(shasum -a 256 "$OUT_DIR/bin/find" | awk '{print $1}')"
find_binary_size="$(wc -c <"$OUT_DIR/bin/find" | tr -d ' ')"
xargs_binary_sha256="$(shasum -a 256 "$OUT_DIR/bin/xargs" | awk '{print $1}')"
xargs_binary_size="$(wc -c <"$OUT_DIR/bin/xargs" | tr -d ' ')"
findutils_license_sha256="$(shasum -a 256 "$BUILD_DIR/licenses/findutils/COPYING" | awk '{print $1}')"
grep_binary_sha256="$(shasum -a 256 "$OUT_DIR/bin/grep" | awk '{print $1}')"
grep_binary_size="$(wc -c <"$OUT_DIR/bin/grep" | tr -d ' ')"
grep_license_sha256="$(shasum -a 256 "$BUILD_DIR/licenses/grep/COPYING" | awk '{print $1}')"
strace_binary_sha256="$(shasum -a 256 "$OUT_DIR/bin/strace" | awk '{print $1}')"
strace_binary_size="$(wc -c <"$OUT_DIR/bin/strace" | tr -d ' ')"
strace_license_sha256="$(shasum -a 256 "$BUILD_DIR/licenses/strace/COPYING" | awk '{print $1}')"
ncurses_license_sha256="$(shasum -a 256 "$BUILD_DIR/licenses/ncurses/COPYING" | awk '{print $1}')"
dialog_binary_sha256="$(shasum -a 256 "$OUT_DIR/bin/dialog" | awk '{print $1}')"
dialog_binary_size="$(wc -c <"$OUT_DIR/bin/dialog" | tr -d ' ')"
dialog_license_sha256="$(shasum -a 256 "$BUILD_DIR/licenses/dialog/COPYING" | awk '{print $1}')"
squashfuse_wrapper_sha256="$(shasum -a 256 "$OUT_DIR/bin/squashfuse" | awk '{print $1}')"
squashfuse_wrapper_size="$(wc -c <"$OUT_DIR/bin/squashfuse" | tr -d ' ')"
squashfuse_binary_sha256="$(shasum -a 256 "$OUT_DIR/bin/squashfuse_ll.bin" | awk '{print $1}')"
squashfuse_binary_size="$(wc -c <"$OUT_DIR/bin/squashfuse_ll.bin" | tr -d ' ')"
libfuse_binary_sha256="$(shasum -a 256 "$OUT_DIR/lib/libfuse3.so.4" | awk '{print $1}')"
libfuse_binary_size="$(wc -c <"$OUT_DIR/lib/libfuse3.so.4" | tr -d ' ')"
zstd_binary_sha256="$(shasum -a 256 "$OUT_DIR/lib/libzstd.so.1" | awk '{print $1}')"
zstd_binary_size="$(wc -c <"$OUT_DIR/lib/libzstd.so.1" | tr -d ' ')"
lz4_binary_sha256="$(shasum -a 256 "$OUT_DIR/lib/liblz4.so.1" | awk '{print $1}')"
lz4_binary_size="$(wc -c <"$OUT_DIR/lib/liblz4.so.1" | tr -d ' ')"
xz_binary_sha256="$(shasum -a 256 "$OUT_DIR/lib/liblzma.so.5" | awk '{print $1}')"
xz_binary_size="$(wc -c <"$OUT_DIR/lib/liblzma.so.5" | tr -d ' ')"
squashfuse_license_sha256="$(shasum -a 256 "$BUILD_DIR/licenses/squashfuse/LICENSE" | awk '{print $1}')"
libfuse_license_sha256="$(shasum -a 256 "$BUILD_DIR/licenses/libfuse/LICENSE" | awk '{print $1}')"
zstd_license_sha256="$(shasum -a 256 "$BUILD_DIR/licenses/zstd/LICENSE" | awk '{print $1}')"
lz4_license_sha256="$(shasum -a 256 "$BUILD_DIR/licenses/lz4/LICENSE" | awk '{print $1}')"
xz_license_sha256="$(shasum -a 256 "$BUILD_DIR/licenses/xz/COPYING" | awk '{print $1}')"
sudo_shim_sha256="$(shasum -a 256 "$OUT_DIR/bin/sudo" | awk '{print $1}')"
sudo_shim_size="$(wc -c <"$OUT_DIR/bin/sudo" | tr -d ' ')"
doas_shim_sha256="$(shasum -a 256 "$OUT_DIR/bin/doas" | awk '{print $1}')"
doas_shim_size="$(wc -c <"$OUT_DIR/bin/doas" | tr -d ' ')"
systemctl_shim_sha256="$(shasum -a 256 "$OUT_DIR/bin/systemctl" | awk '{print $1}')"
systemctl_shim_size="$(wc -c <"$OUT_DIR/bin/systemctl" | tr -d ' ')"
dos2unix_shim_sha256="$(shasum -a 256 "$OUT_DIR/bin/dos2unix" | awk '{print $1}')"
dos2unix_shim_size="$(wc -c <"$OUT_DIR/bin/dos2unix" | tr -d ' ')"
leaf_squashfs_check_sha256="$(shasum -a 256 "$OUT_DIR/bin/leaf-squashfs-check" | awk '{print $1}')"
leaf_squashfs_check_size="$(wc -c <"$OUT_DIR/bin/leaf-squashfs-check" | tr -d ' ')"
xdelta3_shim_sha256="$(shasum -a 256 "$OUT_DIR/bin/xdelta3" | awk '{print $1}')"
xdelta3_shim_size="$(wc -c <"$OUT_DIR/bin/xdelta3" | tr -d ' ')"
seven_zip_shim_sha256="$(shasum -a 256 "$OUT_DIR/bin/7z" | awk '{print $1}')"
seven_zip_shim_size="$(wc -c <"$OUT_DIR/bin/7z" | tr -d ' ')"
innoextract_shim_sha256="$(shasum -a 256 "$OUT_DIR/bin/innoextract" | awk '{print $1}')"
innoextract_shim_size="$(wc -c <"$OUT_DIR/bin/innoextract" | tr -d ' ')"
getconf_binary_sha256="$(shasum -a 256 "$OUT_DIR/bin/getconf" | awk '{print $1}')"
getconf_binary_size="$(wc -c <"$OUT_DIR/bin/getconf" | tr -d ' ')"
ldd_script_sha256="$(shasum -a 256 "$OUT_DIR/bin/ldd" | awk '{print $1}')"
ldd_script_size="$(wc -c <"$OUT_DIR/bin/ldd" | tr -d ' ')"
readelf_binary_sha256="$(shasum -a 256 "$OUT_DIR/bin/readelf" | awk '{print $1}')"
readelf_binary_size="$(wc -c <"$OUT_DIR/bin/readelf" | tr -d ' ')"
file_shim_sha256="$(shasum -a 256 "$OUT_DIR/bin/file" | awk '{print $1}')"
file_shim_size="$(wc -c <"$OUT_DIR/bin/file" | tr -d ' ')"

cat >"$OUT_DIR/manifest.json" <<EOF
{
  "schema": 1,
  "kind": "source-built-native-tools",
  "architecture": "aarch64",
  "tools": [
    {
      "name": "rsync",
      "version": "$rsync_version",
      "path": "bin/rsync",
      "size": $rsync_binary_size,
      "sha256": "$rsync_binary_sha256",
      "source": {
        "url": "$rsync_url",
        "size": $rsync_expected_size,
        "sha256": "$rsync_expected_sha256"
      },
      "license": {
        "spdx": "GPL-3.0-or-later",
        "path": "LICENSES/rsync/COPYING",
        "sha256": "$rsync_license_sha256"
      }
    },
    {
      "name": "zip",
      "version": "$zip_version",
      "path": "bin/zip",
      "size": $zip_binary_size,
      "sha256": "$zip_binary_sha256",
      "source": {
        "url": "$zip_url",
        "size": $zip_expected_size,
        "sha256": "$zip_expected_sha256"
      },
      "license": {
        "spdx": "Info-ZIP",
        "path": "LICENSES/zip/LICENSE",
        "sha256": "$zip_license_sha256"
      }
    },
    {
      "name": "sed",
      "version": "$sed_version",
      "path": "bin/sed",
      "size": $sed_binary_size,
      "sha256": "$sed_binary_sha256",
      "source": {
        "url": "$sed_url",
        "size": $sed_expected_size,
        "sha256": "$sed_expected_sha256"
      },
      "license": {
        "spdx": "GPL-3.0-or-later",
        "path": "LICENSES/sed/COPYING",
        "sha256": "$sed_license_sha256"
      }
    },
    {
      "name": "find",
      "version": "$findutils_version",
      "path": "bin/find",
      "size": $find_binary_size,
      "sha256": "$find_binary_sha256",
      "source": {
        "url": "$findutils_url",
        "size": $findutils_expected_size,
        "sha256": "$findutils_expected_sha256"
      },
      "license": {
        "spdx": "GPL-3.0-or-later",
        "path": "LICENSES/findutils/COPYING",
        "sha256": "$findutils_license_sha256"
      }
    },
    {
      "name": "xargs",
      "version": "$findutils_version",
      "path": "bin/xargs",
      "size": $xargs_binary_size,
      "sha256": "$xargs_binary_sha256",
      "source": {
        "url": "$findutils_url",
        "size": $findutils_expected_size,
        "sha256": "$findutils_expected_sha256"
      },
      "license": {
        "spdx": "GPL-3.0-or-later",
        "path": "LICENSES/findutils/COPYING",
        "sha256": "$findutils_license_sha256"
      }
    },
    {
      "name": "grep",
      "version": "$grep_version",
      "path": "bin/grep",
      "size": $grep_binary_size,
      "sha256": "$grep_binary_sha256",
      "source": {
        "url": "$grep_url",
        "size": $grep_expected_size,
        "sha256": "$grep_expected_sha256"
      },
      "license": {
        "spdx": "GPL-3.0-or-later",
        "path": "LICENSES/grep/COPYING",
        "sha256": "$grep_license_sha256"
      }
    },
    {
      "name": "strace",
      "version": "$strace_version",
      "path": "bin/strace",
      "size": $strace_binary_size,
      "sha256": "$strace_binary_sha256",
      "source": {
        "url": "$strace_url",
        "size": $strace_expected_size,
        "sha256": "$strace_expected_sha256"
      },
      "license": {
        "spdx": "LGPL-2.1-or-later",
        "path": "LICENSES/strace/COPYING",
        "sha256": "$strace_license_sha256"
      }
    },
    {
      "name": "dialog",
      "version": "$dialog_version",
      "path": "bin/dialog",
      "size": $dialog_binary_size,
      "sha256": "$dialog_binary_sha256",
      "source": {
        "url": "$dialog_url",
        "size": $dialog_expected_size,
        "sha256": "$dialog_expected_sha256"
      },
      "license": {
        "spdx": "LGPL-2.1-only",
        "path": "LICENSES/dialog/COPYING",
        "sha256": "$dialog_license_sha256"
      },
      "static_dependencies": [
        {
          "name": "ncurses",
          "version": "$ncurses_version",
          "source": {
            "url": "$ncurses_url",
            "size": $ncurses_expected_size,
            "sha256": "$ncurses_expected_sha256"
          },
          "license": {
            "spdx": "MIT",
            "path": "LICENSES/ncurses/COPYING",
            "sha256": "$ncurses_license_sha256"
          }
        }
      ]
    },
    {
      "name": "squashfuse",
      "version": "$squashfuse_version",
      "path": "bin/squashfuse",
      "size": $squashfuse_wrapper_size,
      "sha256": "$squashfuse_wrapper_sha256",
      "kind": "app-local-squashfs-fuse-wrapper",
      "source": {
        "url": "$squashfuse_url",
        "size": $squashfuse_expected_size,
        "sha256": "$squashfuse_expected_sha256"
      },
      "binary": {
        "path": "bin/squashfuse_ll.bin",
        "size": $squashfuse_binary_size,
        "sha256": "$squashfuse_binary_sha256"
      },
      "runtime_libraries": [
        {
          "name": "libfuse3",
          "version": "$libfuse_version",
          "path": "lib/libfuse3.so.4",
          "size": $libfuse_binary_size,
          "sha256": "$libfuse_binary_sha256",
          "source": {
            "url": "$libfuse_url",
            "size": $libfuse_expected_size,
            "sha256": "$libfuse_expected_sha256"
          },
          "license": {
            "spdx": "LGPL-2.1-only",
            "path": "LICENSES/libfuse/LICENSE",
            "sha256": "$libfuse_license_sha256"
          }
        },
        {
          "name": "libzstd",
          "version": "$zstd_version",
          "path": "lib/libzstd.so.1",
          "size": $zstd_binary_size,
          "sha256": "$zstd_binary_sha256",
          "source": {
            "url": "$zstd_url",
            "size": $zstd_expected_size,
            "sha256": "$zstd_expected_sha256"
          },
          "license": {
            "spdx": "BSD-3-Clause OR GPL-2.0-only",
            "path": "LICENSES/zstd/LICENSE",
            "sha256": "$zstd_license_sha256"
          }
        },
        {
          "name": "liblz4",
          "version": "$lz4_version",
          "path": "lib/liblz4.so.1",
          "size": $lz4_binary_size,
          "sha256": "$lz4_binary_sha256",
          "source": {
            "url": "$lz4_url",
            "size": $lz4_expected_size,
            "sha256": "$lz4_expected_sha256"
          },
          "license": {
            "spdx": "BSD-2-Clause OR GPL-2.0-only",
            "path": "LICENSES/lz4/LICENSE",
            "sha256": "$lz4_license_sha256"
          }
        },
        {
          "name": "liblzma",
          "version": "$xz_version",
          "path": "lib/liblzma.so.5",
          "size": $xz_binary_size,
          "sha256": "$xz_binary_sha256",
          "source": {
            "url": "$xz_url",
            "size": $xz_expected_size,
            "sha256": "$xz_expected_sha256"
          },
          "license": {
            "spdx": "0BSD AND LGPL-2.1-or-later",
            "path": "LICENSES/xz/COPYING",
            "sha256": "$xz_license_sha256"
          }
        }
      ],
      "license": {
        "spdx": "BSD-2-Clause",
        "path": "LICENSES/squashfuse/LICENSE",
        "sha256": "$squashfuse_license_sha256"
      }
    },
    {
      "name": "sudo",
      "version": "leaf-shim-1",
      "path": "bin/sudo",
      "size": $sudo_shim_size,
      "sha256": "$sudo_shim_sha256",
      "kind": "app-local-root-pass-through-shim",
      "license": {
        "spdx": "MIT",
        "path": "LICENSE"
      }
    },
    {
      "name": "doas",
      "version": "leaf-shim-1",
      "path": "bin/doas",
      "size": $doas_shim_size,
      "sha256": "$doas_shim_sha256",
      "kind": "app-local-root-pass-through-shim",
      "license": {
        "spdx": "MIT",
        "path": "LICENSE"
      }
    },
    {
      "name": "systemctl",
      "version": "leaf-shim-1",
      "path": "bin/systemctl",
      "size": $systemctl_shim_size,
      "sha256": "$systemctl_shim_sha256",
      "kind": "app-local-systemd-compat-shim",
      "license": {
        "spdx": "MIT",
        "path": "LICENSE"
      }
    },
    {
      "name": "dos2unix",
      "version": "leaf-shim-1",
      "path": "bin/dos2unix",
      "size": $dos2unix_shim_size,
      "sha256": "$dos2unix_shim_sha256",
      "kind": "app-local-crlf-conversion-shim",
      "license": {
        "spdx": "MIT",
        "path": "LICENSE"
      }
    },
    {
      "name": "leaf-squashfs-check",
      "version": "leaf-shim-1",
      "path": "bin/leaf-squashfs-check",
      "size": $leaf_squashfs_check_size,
      "sha256": "$leaf_squashfs_check_sha256",
      "kind": "app-local-squashfs-runtime-preflight",
      "license": {
        "spdx": "MIT",
        "path": "LICENSE"
      }
    },
    {
      "name": "getconf",
      "version": "glibc-toolchain-sysroot",
      "path": "bin/getconf",
      "size": $getconf_binary_size,
      "sha256": "$getconf_binary_sha256",
      "kind": "target-toolchain-sysroot-binary",
      "source": {
        "path": "$TARGET_SYSROOT/usr/bin/getconf"
      },
      "license": {
        "spdx": "LGPL-2.1-or-later"
      }
    },
    {
      "name": "ldd",
      "version": "glibc-toolchain-sysroot",
      "path": "bin/ldd",
      "size": $ldd_script_size,
      "sha256": "$ldd_script_sha256",
      "kind": "target-toolchain-sysroot-script",
      "source": {
        "path": "$TARGET_SYSROOT/usr/bin/ldd"
      },
      "license": {
        "spdx": "LGPL-2.1-or-later"
      }
    },
    {
      "name": "readelf",
      "version": "binutils-toolchain",
      "path": "bin/readelf",
      "size": $readelf_binary_size,
      "sha256": "$readelf_binary_sha256",
      "kind": "target-toolchain-binary",
      "source": {
        "path": "$TARGET_PREFIX/bin/readelf"
      },
      "license": {
        "spdx": "GPL-3.0-or-later"
      }
    },
    {
      "name": "file",
      "version": "leaf-shim-1",
      "path": "bin/file",
      "size": $file_shim_size,
      "sha256": "$file_shim_sha256",
      "kind": "app-local-basic-file-shim",
      "license": {
        "spdx": "MIT",
        "path": "LICENSE"
      }
    },
    {
      "name": "xdelta3",
      "version": "portmaster-controlfolder-shim-1",
      "path": "bin/xdelta3",
      "size": $xdelta3_shim_size,
      "sha256": "$xdelta3_shim_sha256",
      "kind": "app-local-portmaster-binary-shim",
      "target": "PortMaster/xdelta3",
      "license": {
        "spdx": "MIT",
        "path": "LICENSE"
      }
    },
    {
      "name": "7z",
      "version": "portmaster-controlfolder-shim-1",
      "path": "bin/7z",
      "size": $seven_zip_shim_size,
      "sha256": "$seven_zip_shim_sha256",
      "kind": "app-local-portmaster-binary-shim",
      "target": "PortMaster/7zzs.aarch64",
      "license": {
        "spdx": "MIT",
        "path": "LICENSE"
      }
    },
    {
      "name": "7za",
      "version": "portmaster-controlfolder-shim-1",
      "path": "bin/7za",
      "size": $seven_zip_shim_size,
      "sha256": "$seven_zip_shim_sha256",
      "kind": "app-local-portmaster-binary-shim",
      "target": "PortMaster/7zzs.aarch64",
      "license": {
        "spdx": "MIT",
        "path": "LICENSE"
      }
    },
    {
      "name": "innoextract",
      "version": "portmaster-controlfolder-shim-1",
      "path": "bin/innoextract",
      "size": $innoextract_shim_size,
      "sha256": "$innoextract_shim_sha256",
      "kind": "app-local-portmaster-binary-shim",
      "target": "PortMaster/innoextract.aarch64",
      "license": {
        "spdx": "MIT",
        "path": "LICENSE"
      }
    }
  ]
}
EOF

echo "aarch64 native tools ready: $OUT_DIR"
