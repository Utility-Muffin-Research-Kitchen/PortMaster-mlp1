#!/usr/bin/env bash
set -euo pipefail

ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
LOCK="$ROOT/locks/aarch64-tools.lock.json"
IMAGE="${MLP1_TOOLCHAIN_IMAGE:-ghcr.io/utility-muffin-research-kitchen/mlp1-toolchain:latest}"
BUILD_DIR="$ROOT/build/mlp1"
WORK_DIR="$ROOT/build/aarch64-tools"
OUT_DIR="$BUILD_DIR/compat/tools/aarch64"
LICENSE_DIR="$BUILD_DIR/licenses/rsync"
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

mkdir -p "$WORK_DIR/downloads" "$OUT_DIR/bin" "$LICENSE_DIR"

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

cat >"$OUT_DIR/bin/sudo" <<'EOF'
#!/bin/sh
# Leaf PortMaster app-local sudo shim. The MLP1 launch session already runs as
# root, so this strips common sudo options and executes the requested command.
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
exec "$@"
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

chmod 755 "$OUT_DIR/bin/sudo" "$OUT_DIR/bin/doas" "$OUT_DIR/bin/systemctl"

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

chmod 755 "$OUT_DIR/bin/xdelta3" "$OUT_DIR/bin/7z" "$OUT_DIR/bin/7za"

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
ncurses_license_sha256="$(shasum -a 256 "$BUILD_DIR/licenses/ncurses/COPYING" | awk '{print $1}')"
dialog_binary_sha256="$(shasum -a 256 "$OUT_DIR/bin/dialog" | awk '{print $1}')"
dialog_binary_size="$(wc -c <"$OUT_DIR/bin/dialog" | tr -d ' ')"
dialog_license_sha256="$(shasum -a 256 "$BUILD_DIR/licenses/dialog/COPYING" | awk '{print $1}')"
sudo_shim_sha256="$(shasum -a 256 "$OUT_DIR/bin/sudo" | awk '{print $1}')"
sudo_shim_size="$(wc -c <"$OUT_DIR/bin/sudo" | tr -d ' ')"
doas_shim_sha256="$(shasum -a 256 "$OUT_DIR/bin/doas" | awk '{print $1}')"
doas_shim_size="$(wc -c <"$OUT_DIR/bin/doas" | tr -d ' ')"
systemctl_shim_sha256="$(shasum -a 256 "$OUT_DIR/bin/systemctl" | awk '{print $1}')"
systemctl_shim_size="$(wc -c <"$OUT_DIR/bin/systemctl" | tr -d ' ')"
xdelta3_shim_sha256="$(shasum -a 256 "$OUT_DIR/bin/xdelta3" | awk '{print $1}')"
xdelta3_shim_size="$(wc -c <"$OUT_DIR/bin/xdelta3" | tr -d ' ')"
seven_zip_shim_sha256="$(shasum -a 256 "$OUT_DIR/bin/7z" | awk '{print $1}')"
seven_zip_shim_size="$(wc -c <"$OUT_DIR/bin/7z" | tr -d ' ')"

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
    }
  ]
}
EOF

echo "aarch64 native tools ready: $OUT_DIR"
