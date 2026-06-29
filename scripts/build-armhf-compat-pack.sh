#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE="${PM_ARMHF_BUILD_IMAGE:-debian:bookworm-slim}"
VERSION="${PM_ARMHF_COMPAT_VERSION:-bookworm-20260629}"
container=0

if [[ "${1:-}" == "--container" ]]; then
  container=1
  shift
fi

OUT_DIR="${1:-${PM_ARMHF_OUT_DIR:-$ROOT/build/armhf-compat}}"

PACKAGES=(
  libc6:armhf
  libgcc-s1:armhf
  libstdc++6:armhf
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

LIB_PATH="$ROOT/lib/arm-linux-gnueabihf:$ROOT/usr/lib/arm-linux-gnueabihf:$ROOT/usr/lib/arm-linux-gnueabihf/pulseaudio:$ROOT/lib:$ROOT/usr/lib"
if [ -n "${LD_LIBRARY_PATH:-}" ]; then
    LIB_PATH="$LIB_PATH:$LD_LIBRARY_PATH"
fi

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
python3 - "$VERSION" "$SOURCES_REPORT" "$FILE_REPORT" "$EMBEDDED_MANIFEST" <<'PY'
import datetime as dt
import json
import sys

version, sources_path, files_path, manifest_path = sys.argv[1:5]
with open(sources_path, "r", encoding="utf-8") as fp:
    sources = json.load(fp)
with open(files_path, "r", encoding="utf-8") as fp:
    files = json.load(fp)

manifest = {
    "version": version,
    "built_at": dt.datetime.now(dt.timezone.utc).isoformat().replace("+00:00", "Z"),
    "minimum_manager_version": "0.1.1",
    "sources": sources,
    "files": files,
    "sha256": {
        "artifact": None,
    },
    "tiers_verified": {
        "tier0_static_helpers": False,
        "tier1_dynamic_loader": False,
        "tier1_sdl_runtime_packaged": True,
        "tier2_gles": False,
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
