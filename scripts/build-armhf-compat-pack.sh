#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE="${PM_ARMHF_BUILD_IMAGE:-debian:bookworm-slim}"
VERSION="${PM_ARMHF_COMPAT_VERSION:-bookworm-mali-g24p0-20260630}"
container=0

MALI_REPO="https://github.com/tsukumijima/libmali-rockchip"
MALI_RAW_BASE="https://raw.githubusercontent.com/tsukumijima/libmali-rockchip"
MALI_COMMIT="${PM_ARMHF_MALI_COMMIT:-bd33ee262f47fd936b831afccaa0759b3ecc2482}"
MALI_VARIANT="bifrost-g52-g24p0-wayland-gbm"
MALI_BLOB_PATH="lib/arm-linux-gnueabihf/libmali-$MALI_VARIANT.so"
MALI_BLOB_SHA256="67d7d1d275eb437b6d4470529230c8bc6289bdf0a288fdc3ab5ee2bd648a95f6"
MALI_BLOB_SIZE="38116912"
MALI_COPYRIGHT_PATH="debian/copyright"
MALI_COPYRIGHT_SHA256="a254205ab051a9a6cf952bf73d9f40715eb3bd2963d82ac462762e4dda1e8c77"

if [[ "${1:-}" == "--container" ]]; then
  container=1
  shift
fi

OUT_DIR="${1:-${PM_ARMHF_OUT_DIR:-$ROOT/build/armhf-compat}}"

PACKAGES=(
  libc6:armhf
  libgcc-s1:armhf
  libstdc++6:armhf
  libbz2-1.0:armhf
  zlib1g:armhf
  libsdl2-2.0-0:armhf
  libsdl2-image-2.0-0:armhf
  libsdl2-mixer-2.0-0:armhf
  libsdl2-ttf-2.0-0:armhf
  libsdl2-gfx-1.0-0:armhf
  libopenal1:armhf
  libpng16-16:armhf
  libjpeg62-turbo:armhf
  libfreetype6:armhf
  libharfbuzz0b:armhf
  libglib2.0-0:armhf
  libasound2:armhf
  libpulse0:armhf
  libdbus-1-3:armhf
  libudev1:armhf
  libwayland-client0:armhf
  libwayland-cursor0:armhf
  libwayland-egl1:armhf
  libxkbcommon0:armhf
  libdrm2:armhf
  libgbm1:armhf
  libegl1:armhf
  libgles2:armhf
  libvorbis0a:armhf
  libvorbisfile3:armhf
  libogg0:armhf
  libopus0:armhf
  libopusfile0:armhf
  libflac12:armhf
  libsndfile1:armhf
  libmodplug1:armhf
  libmpg123-0:armhf
)

if [[ "$container" == "0" ]]; then
  mkdir -p "$OUT_DIR"
  OUT_DIR="$(cd "$OUT_DIR" && pwd)"
  case "$OUT_DIR" in
    "$ROOT"/*) OUT_REL="${OUT_DIR#$ROOT/}" ;;
    *)
      echo "output directory must live under $ROOT for the Docker build: $OUT_DIR" >&2
      exit 2
      ;;
  esac

  exec docker run --rm \
    -v "$ROOT":/work \
    -w /work \
    -e PM_ARMHF_OUT_DIR="$OUT_REL" \
    -e PM_ARMHF_COMPAT_VERSION="$VERSION" \
    -e PM_ARMHF_MALI_COMMIT="$MALI_COMMIT" \
    "$IMAGE" \
    bash scripts/build-armhf-compat-pack.sh --container
fi

case "$OUT_DIR" in
  /*) ;;
  *) OUT_DIR="$ROOT/$OUT_DIR" ;;
esac

export DEBIAN_FRONTEND=noninteractive

WORK_DIR="$OUT_DIR/work"
ROOTFS="$WORK_DIR/root"
DEB_DIR="$WORK_DIR/debs"
SOURCES_REPORT="$WORK_DIR/sources.json"
FILE_REPORT="$WORK_DIR/files.json"
MALI_REPORT="$WORK_DIR/mali.json"
ARTIFACT="$OUT_DIR/portmaster-mlp1-armhf-compat-$VERSION.zip"
MANIFEST="$OUT_DIR/portmaster-mlp1-armhf-compat-$VERSION.json"
EMBEDDED_MANIFEST="$ROOTFS/.leaf-armhf-compat-manifest.json"

rm -rf "$WORK_DIR"
mkdir -p "$ROOTFS" "$DEB_DIR/partial" "$OUT_DIR"

echo "=== Installing build tools in $IMAGE ==="
dpkg --add-architecture armhf
apt-get update
apt-get install -y --no-install-recommends \
  binutils-arm-linux-gnueabihf \
  ca-certificates \
  dpkg-dev \
  file \
  gcc-arm-linux-gnueabihf \
  libc6-dev-armhf-cross \
  python3 \
  curl \
  zip

printf '%s\n' "${PACKAGES[@]}" >"$WORK_DIR/package-plan.txt"

echo "=== Downloading armhf Debian packages ==="
apt-get install -y --download-only \
  --no-install-recommends \
  -o Dir::Cache::archives="$DEB_DIR" \
  "${PACKAGES[@]}"

mapfile -t DEBS < <(
  find "$DEB_DIR" -maxdepth 1 -type f -name '*.deb' | sort |
    while IFS= read -r deb; do
      arch="$(dpkg-deb -f "$deb" Architecture)"
      case "$arch" in
        armhf|all) printf '%s\n' "$deb" ;;
      esac
    done
)
if [[ "${#DEBS[@]}" == "0" ]]; then
  echo "no .deb files downloaded" >&2
  exit 1
fi

echo "=== Extracting armhf runtime root ==="
for deb in "${DEBS[@]}"; do
  dpkg-deb -x "$deb" "$ROOTFS"
done

mkdir -p "$ROOTFS/bin" "$ROOTFS/licenses/debian"

echo "=== Capturing Debian package provenance ==="
python3 - "$ROOTFS" "$SOURCES_REPORT" "${DEBS[@]}" <<'PY'
import hashlib
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

root = Path(sys.argv[1])
report_path = Path(sys.argv[2])
debs = [Path(p) for p in sys.argv[3:]]

packages = []
for deb in debs:
    fields = {}
    for key in ("Package", "Version", "Architecture", "Source", "Homepage"):
        try:
            value = subprocess.check_output(
                ["dpkg-deb", "-f", str(deb), key], text=True
            ).strip()
        except subprocess.CalledProcessError:
            value = ""
        fields[key.lower()] = value

    pkg = fields["package"]
    copyright_src = root / "usr" / "share" / "doc" / pkg / "copyright"
    copyright_dst = root / "licenses" / "debian" / f"{pkg}.copyright"
    if copyright_src.exists():
        shutil.copy2(copyright_src, copyright_dst)

    digest = hashlib.sha256(deb.read_bytes()).hexdigest()
    packages.append({
        "package": pkg,
        "version": fields["version"],
        "architecture": fields["architecture"],
        "source": fields["source"] or pkg,
        "homepage": fields["homepage"],
        "deb": deb.name,
        "size": deb.stat().st_size,
        "sha256": digest,
    })

os_release = {}
with open("/etc/os-release", "r", encoding="utf-8") as fp:
    for line in fp:
        line = line.strip()
        if not line or "=" not in line:
            continue
        key, value = line.split("=", 1)
        os_release[key] = value.strip('"')

report_path.write_text(json.dumps({
    "debian": os_release,
    "packages": packages,
}, indent=2, sort_keys=True) + "\n", encoding="utf-8")
PY

rm -rf "$ROOTFS/usr/share/doc" \
       "$ROOTFS/usr/share/lintian" \
       "$ROOTFS/usr/share/man" \
       "$ROOTFS/usr/share/locale" \
       "$ROOTFS/usr/share/bug" \
       "$ROOTFS/usr/share/apport"

echo "=== Installing Rockchip Mali armhf GLES stack ==="
MALI_WORK="$WORK_DIR/mali"
MALI_LIB_DIR="$ROOTFS/usr/lib/arm-linux-gnueabihf"
MALI_VENDOR_DIR="$MALI_LIB_DIR/mali"
mkdir -p "$MALI_WORK" "$MALI_VENDOR_DIR" "$MALI_LIB_DIR/pkgconfig" \
         "$ROOTFS/licenses/mali" \
         "$ROOTFS/etc/OpenCL/vendors" "$ROOTFS/etc/ld.so.conf.d" \
         "$ROOTFS/etc/profile.d"

curl -fsSL "$MALI_RAW_BASE/$MALI_COMMIT/$MALI_BLOB_PATH" \
  -o "$MALI_WORK/libmali.so.1.9.0"
curl -fsSL "$MALI_RAW_BASE/$MALI_COMMIT/$MALI_COPYRIGHT_PATH" \
  -o "$ROOTFS/licenses/mali/libmali-rockchip-debian-copyright"

mali_blob_sha="$(sha256sum "$MALI_WORK/libmali.so.1.9.0" | awk '{print $1}')"
mali_copyright_sha="$(sha256sum "$ROOTFS/licenses/mali/libmali-rockchip-debian-copyright" | awk '{print $1}')"
mali_blob_size="$(wc -c <"$MALI_WORK/libmali.so.1.9.0" | tr -d ' ')"
if [[ "$mali_blob_sha" != "$MALI_BLOB_SHA256" ]]; then
  echo "Mali blob sha256 mismatch: $mali_blob_sha != $MALI_BLOB_SHA256" >&2
  exit 1
fi
if [[ "$mali_copyright_sha" != "$MALI_COPYRIGHT_SHA256" ]]; then
  echo "Mali copyright sha256 mismatch: $mali_copyright_sha != $MALI_COPYRIGHT_SHA256" >&2
  exit 1
fi
if [[ "$mali_blob_size" != "$MALI_BLOB_SIZE" ]]; then
  echo "Mali blob size mismatch: $mali_blob_size != $MALI_BLOB_SIZE" >&2
  exit 1
fi

cp -f "$MALI_WORK/libmali.so.1.9.0" "$MALI_LIB_DIR/libmali.so.1"
ln -sf libmali.so.1 "$MALI_LIB_DIR/libmali.so"

cat >"$MALI_WORK/dummy.c" <<'C'
void leaf_mali_wrapper_anchor(void) {}
C

for spec in EGL:1 GLESv1_CM:1 GLESv2:2 gbm:1 wayland-egl:1 MaliOpenCL:1; do
  name="${spec%%:*}"
  soname_version="${spec##*:}"
  arm-linux-gnueabihf-gcc -shared -fPIC \
    -Wl,-soname,"lib${name}.so.${soname_version}" \
    -Wl,--no-as-needed -L"$MALI_LIB_DIR" -lmali -Wl,--as-needed \
    -o "$MALI_VENDOR_DIR/lib${name}.so.${soname_version}" \
    "$MALI_WORK/dummy.c"
  cp -f "$MALI_VENDOR_DIR/lib${name}.so.${soname_version}" \
        "$MALI_VENDOR_DIR/lib${name}.so"
done

printf '%s\n' 'libMaliOpenCL.so.1' >"$ROOTFS/etc/OpenCL/vendors/mali.icd"
printf '%s\n' '/usr/lib/arm-linux-gnueabihf/mali' >"$ROOTFS/etc/ld.so.conf.d/00-arm-mali.conf"
printf '%s\n' 'export MALI_SCHED_RT_THREAD_PRIORITY=95' >"$ROOTFS/etc/profile.d/mali-priority.sh"
cat >"$MALI_LIB_DIR/pkgconfig/mali.pc" <<'PC'
prefix=/usr
libdir=${prefix}/lib/arm-linux-gnueabihf

Name: mali
Description: Mali GPU User-Space Binary Driver
Version: 1.9.0
Requires: libdrm wayland-client wayland-server
Libs: -L${libdir} -lmali
PC

chmod 755 "$MALI_LIB_DIR"/libmali*.so* "$MALI_VENDOR_DIR"/*.so*
rm -f "$MALI_LIB_DIR/libmali.so"
chmod 644 "$ROOTFS/licenses/mali/libmali-rockchip-debian-copyright" \
          "$ROOTFS/etc/OpenCL/vendors/mali.icd" \
          "$ROOTFS/etc/ld.so.conf.d/00-arm-mali.conf" \
          "$MALI_LIB_DIR/pkgconfig/mali.pc"

python3 - "$MALI_REPORT" "$MALI_REPO" "$MALI_COMMIT" "$MALI_VARIANT" \
  "$MALI_BLOB_PATH" "$MALI_BLOB_SHA256" "$MALI_BLOB_SIZE" \
  "$MALI_COPYRIGHT_PATH" "$MALI_COPYRIGHT_SHA256" <<'PY'
import json
import sys

(
    report_path,
    repo,
    commit,
    variant,
    blob_path,
    blob_sha,
    blob_size,
    copyright_path,
    copyright_sha,
) = sys.argv[1:10]

report = {
    "repo": repo,
    "commit": commit,
    "variant": variant,
    "architecture": "armhf",
    "license": {
        "kind": "ARM Mali userspace driver EULA",
        "upstream_path": copyright_path,
        "installed_path": "licenses/mali/libmali-rockchip-debian-copyright",
        "sha256": copyright_sha,
        "notes": [
            "Binary blobs are closed-source ARM Mali userspace driver components.",
            "Redistribution must retain notices and include this license text.",
        ],
    },
    "blob": {
        "upstream_path": blob_path,
        "installed_paths": [
            "usr/lib/arm-linux-gnueabihf/libmali.so.1",
        ],
        "size": int(blob_size),
        "sha256": blob_sha,
        "driver_release": "g24p0-00eac0",
    },
    "wrappers": [
        "usr/lib/arm-linux-gnueabihf/mali/libEGL.so.1",
        "usr/lib/arm-linux-gnueabihf/mali/libGLESv1_CM.so.1",
        "usr/lib/arm-linux-gnueabihf/mali/libGLESv2.so.2",
        "usr/lib/arm-linux-gnueabihf/mali/libgbm.so.1",
        "usr/lib/arm-linux-gnueabihf/mali/libwayland-egl.so.1",
        "usr/lib/arm-linux-gnueabihf/mali/libMaliOpenCL.so.1",
    ],
}

with open(report_path, "w", encoding="utf-8") as fp:
    json.dump(report, fp, indent=2, sort_keys=True)
    fp.write("\n")
PY

cat >"$WORK_DIR/leaf-armhf-smoke.c" <<'C'
#include <stdio.h>
#include <sys/utsname.h>

int main(void) {
    struct utsname u;
    if (uname(&u) == 0) {
        printf("leaf-armhf-smoke ok machine=%s sys=%s release=%s\n",
               u.machine, u.sysname, u.release);
    } else {
        puts("leaf-armhf-smoke ok");
    }
    return 0;
}
C

arm-linux-gnueabihf-gcc -O2 -Wl,--as-needed \
  -o "$ROOTFS/bin/leaf-armhf-smoke" "$WORK_DIR/leaf-armhf-smoke.c"

cat >"$ROOTFS/bin/leaf-armhf-run" <<'SH'
#!/bin/sh
set -eu

ROOT="${LEAF_PM_ARMHF_ROOT:-}"
if [ -z "$ROOT" ]; then
    ROOT="$(CDPATH= cd "$(dirname "$0")/.." && pwd)"
fi

LOADER="$ROOT/lib/ld-linux-armhf.so.3"
if [ ! -x "$LOADER" ]; then
    echo "leaf-armhf-run: missing loader: $LOADER" >&2
    exit 69
fi

LIB_PATH="$ROOT/usr/lib/arm-linux-gnueabihf/mali:$ROOT/lib/arm-linux-gnueabihf:$ROOT/usr/lib/arm-linux-gnueabihf:$ROOT/usr/lib/arm-linux-gnueabihf/pulseaudio:$ROOT/lib:$ROOT/usr/lib"
if [ -n "${LD_LIBRARY_PATH:-}" ]; then
    LIB_PATH="$LIB_PATH:$LD_LIBRARY_PATH"
fi
export LIBGL_DRIVERS_PATH="${LIBGL_DRIVERS_PATH:-$ROOT/usr/lib/arm-linux-gnueabihf/dri}"
export __EGL_VENDOR_LIBRARY_DIRS="${__EGL_VENDOR_LIBRARY_DIRS:-$ROOT/usr/share/glvnd/egl_vendor.d}"
export PULSE_SERVER="${PULSE_SERVER:-unix:/tmp/pulse-socket}"
export PULSE_CLIENTCONFIG="${PULSE_CLIENTCONFIG:-$ROOT/etc/pulse/client.conf}"
export ALSOFT_DRIVERS="${ALSOFT_DRIVERS:-pulse}"
export ALSOFT_CONF="${ALSOFT_CONF:-$ROOT/etc/openal/alsoft.conf}"

exec "$LOADER" --library-path "$LIB_PATH" "$@"
SH
chmod 755 "$ROOTFS/bin/leaf-armhf-run" "$ROOTFS/bin/leaf-armhf-smoke"

echo "=== Flattening symlinks for FAT32-safe install ==="
python3 - "$ROOTFS" <<'PY'
import os
import shutil
import sys
from pathlib import Path

root = Path(sys.argv[1])

for dirpath, dirnames, filenames in os.walk(root, topdown=False, followlinks=False):
    for name in filenames + dirnames:
        path = Path(dirpath) / name
        if not path.is_symlink():
            continue
        target = path.resolve()
        path.unlink()
        if target.is_file():
            shutil.copy2(target, path)
        elif target.is_dir():
            shutil.copytree(target, path, symlinks=False)
        else:
            # Broken symlink from packaging metadata; it is not useful on FAT32.
            continue
PY

if find "$ROOTFS" -type l | grep -q .; then
  echo "symlink flattening left links behind:" >&2
  find "$ROOTFS" -type l >&2
  exit 1
fi

echo "=== Generating file manifest ==="
python3 - "$ROOTFS" "$FILE_REPORT" <<'PY'
import hashlib
import json
import os
import stat
import sys
from pathlib import Path

root = Path(sys.argv[1])
report = Path(sys.argv[2])
rows = []

for path in sorted(p for p in root.rglob("*") if p.is_file()):
    rel = path.relative_to(root).as_posix()
    data = path.read_bytes()
    mode = path.stat().st_mode
    rows.append({
        "path": rel,
        "size": len(data),
        "mode": oct(stat.S_IMODE(mode)),
        "sha256": hashlib.sha256(data).hexdigest(),
    })

report.write_text(json.dumps(rows, indent=2, sort_keys=True) + "\n", encoding="utf-8")
PY

echo "=== Writing embedded manifest ==="
python3 - "$VERSION" "$SOURCES_REPORT" "$FILE_REPORT" "$MALI_REPORT" "$EMBEDDED_MANIFEST" <<'PY'
import datetime as dt
import json
import sys

version, sources_path, files_path, mali_path, manifest_path = sys.argv[1:6]
with open(sources_path, "r", encoding="utf-8") as fp:
    sources = json.load(fp)
with open(files_path, "r", encoding="utf-8") as fp:
    files = json.load(fp)
with open(mali_path, "r", encoding="utf-8") as fp:
    mali = json.load(fp)

manifest = {
    "version": version,
    "built_at": dt.datetime.now(dt.timezone.utc).isoformat().replace("+00:00", "Z"),
    "minimum_manager_version": "0.1.1",
    "sources": sources,
    "mali": mali,
    "files": files,
    "sha256": {
        "artifact": None,
    },
    "tiers_verified": {
        "tier0_static_helpers": False,
        "tier1_dynamic_loader": False,
        "tier1_sdl_runtime_packaged": True,
        "tier2_gles": True,
    },
}

with open(manifest_path, "w", encoding="utf-8") as fp:
    json.dump(manifest, fp, indent=2, sort_keys=True)
    fp.write("\n")
PY

rm -f "$ARTIFACT" "$MANIFEST"
echo "=== Creating $ARTIFACT ==="
(cd "$ROOTFS" && zip -qr "$ARTIFACT" .)

artifact_sha="$(sha256sum "$ARTIFACT" | awk '{print $1}')"
artifact_size="$(wc -c <"$ARTIFACT" | tr -d ' ')"

echo "=== Writing artifact manifest ==="
python3 - "$EMBEDDED_MANIFEST" "$MANIFEST" "$ARTIFACT" "$artifact_sha" "$artifact_size" <<'PY'
import json
import sys
from pathlib import Path

embedded_path, manifest_path, artifact_path, artifact_sha, artifact_size = sys.argv[1:6]
with open(embedded_path, "r", encoding="utf-8") as fp:
    manifest = json.load(fp)

manifest["artifact"] = {
    "name": Path(artifact_path).name,
    "size": int(artifact_size),
    "sha256": artifact_sha,
}
manifest["sha256"]["artifact"] = artifact_sha

with open(manifest_path, "w", encoding="utf-8") as fp:
    json.dump(manifest, fp, indent=2, sort_keys=True)
    fp.write("\n")
PY

echo "=== Armhf compatibility pack built ==="
echo "$ARTIFACT"
echo "$MANIFEST"
echo "$artifact_sha  $(basename "$ARTIFACT")"
