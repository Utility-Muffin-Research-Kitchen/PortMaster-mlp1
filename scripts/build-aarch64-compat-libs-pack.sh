#!/usr/bin/env bash
set -euo pipefail

ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
IMAGE="${AARCH64_COMPAT_LIBS_IMAGE:-debian:bookworm-slim}"
BUILD_DIR="$ROOT/build/mlp1"
WORK_DIR="$ROOT/build/aarch64-compat-libs"
DEB_DIR="$WORK_DIR/debs"
ROOTFS="$WORK_DIR/root"
OUT_DIR="$BUILD_DIR/compat/libs/aarch64"
LICENSE_DIR="$BUILD_DIR/licenses/aarch64-compat-libs"
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
    bash scripts/build-aarch64-compat-libs-pack.sh --container
fi

rm -rf "$WORK_DIR" "$OUT_DIR" "$LICENSE_DIR"
mkdir -p "$DEB_DIR/partial" "$ROOTFS" "$OUT_DIR" "$LICENSE_DIR/debian"

apt-get update
apt-get install -y --no-install-recommends \
  binutils \
  ca-certificates \
  curl \
  dpkg-dev \
  python3

cat >/tmp/leaf-bullseye-arm64.list <<'EOF'
deb [arch=arm64] http://deb.debian.org/debian bullseye main
deb [arch=arm64] http://deb.debian.org/debian bullseye-updates main
deb [arch=arm64] http://security.debian.org/debian-security bullseye-security main
EOF

dpkg --add-architecture arm64
apt-get update \
  -o Dir::Etc::sourcelist=/tmp/leaf-bullseye-arm64.list \
  -o Dir::Etc::sourceparts=- \
  -o APT::Get::List-Cleanup=0

declare -a bullseye_packages=(
  "libflac8:arm64=1.3.3-2+deb11u2"
  "libavcodec58:arm64=7:4.3.7-0+deb11u1"
  "libavformat58:arm64=7:4.3.7-0+deb11u1"
  "libavutil56:arm64=7:4.3.7-0+deb11u1"
  "libswresample3:arm64=7:4.3.7-0+deb11u1"
  "libswscale5:arm64=7:4.3.7-0+deb11u1"
  "libvpx6:arm64=1.9.0-1+deb11u3"
  "libwebp6:arm64=0.6.1-2.1+deb11u2"
  "libaom0:arm64=1.0.0.errata1-3+deb11u1"
  "libdav1d4:arm64=0.7.1-3+deb11u1"
  "libcodec2-0.9:arm64=0.9.2-4"
  "libx264-160:arm64=2:0.160.3011+gitcde9a93-2.1"
  "libx265-192:arm64=3.4-2"
  "libwavpack1:arm64=5.4.0-1"
  "libwebpmux3:arm64"
  "librsvg2-2:arm64"
  "libzvbi0:arm64"
  "libsnappy1v5:arm64"
  "libgsm1:arm64"
  "libmp3lame0:arm64"
  "libopenjp2-7:arm64"
  "libshine3:arm64"
  "libtwolame0:arm64"
  "libxvidcore4:arm64"
  "libva2:arm64"
  "libgme0:arm64"
  "libopenmpt0:arm64"
  "libchromaprint1:arm64"
  "libbluray2:arm64"
  "librabbitmq4:arm64"
  "libsrt1.4-gnutls:arm64"
  "libssh-gcrypt-4:arm64"
  "libzmq5:arm64"
  "libva-drm2:arm64"
  "libva-x11-2:arm64"
  "libvdpau1:arm64"
  "libsoxr0:arm64"
  "libnorm1:arm64"
  "libpgm-5.3-0:arm64"
  "libsodium23:arm64"
  "libudfread0:arm64"
  "libxfixes3:arm64"
)

apt-get install -y --download-only --no-install-recommends \
  -o Dir::Etc::sourcelist=/tmp/leaf-bullseye-arm64.list \
  -o Dir::Etc::sourceparts=- \
  -o Dir::Cache::archives="$DEB_DIR" \
  "${bullseye_packages[@]}"

# These packages are already installed for the container's native architecture,
# so ask apt for the exact arm64 artifacts explicitly.
apt-get download \
  -o Dir::Etc::sourcelist=/tmp/leaf-bullseye-arm64.list \
  -o Dir::Etc::sourceparts=- \
  -o Dir::Cache::archives="$DEB_DIR" \
  "liblzma5:arm64=5.2.5-2.1~deb11u1" \
  "libgcrypt20:arm64=1.8.7-6" \
  "libgpg-error0:arm64=1.38-2" \
  "libgssapi-krb5-2:arm64=1.18.3-6+deb11u8" \
  "libkrb5-3:arm64=1.18.3-6+deb11u8" \
  "libk5crypto3:arm64=1.18.3-6+deb11u8" \
  "libkrb5support0:arm64=1.18.3-6+deb11u8" \
  "libkeyutils1:arm64=1.6.1-2"
mv ./*.deb "$DEB_DIR/" 2>/dev/null || true

ubuntu_jpeg_deb="$DEB_DIR/libjpeg-turbo8_2.0.3-0ubuntu1.20.04.3_arm64.deb"
ubuntu_jpeg_sha="9cd60a19f70399f4a828c7849bc7f18e53c059112100264e568486774eefcc17"
if [ ! -f "$ubuntu_jpeg_deb" ] || [ "$(sha256sum "$ubuntu_jpeg_deb" | awk '{print $1}')" != "$ubuntu_jpeg_sha" ]; then
  tmp="$ubuntu_jpeg_deb.tmp.$$"
  curl -fL --retry 3 --connect-timeout 20 \
    -o "$tmp" \
    "http://ports.ubuntu.com/ubuntu-ports/pool/main/libj/libjpeg-turbo/libjpeg-turbo8_2.0.3-0ubuntu1.20.04.3_arm64.deb"
  echo "$ubuntu_jpeg_sha  $tmp" | sha256sum -c -
  mv "$tmp" "$ubuntu_jpeg_deb"
fi

for deb in "$DEB_DIR"/*.deb; do
  dpkg-deb -x "$deb" "$ROOTFS"
done

python3 - "$ROOTFS" "$OUT_DIR" "$LICENSE_DIR/debian" "$DEB_DIR" <<'PY'
import hashlib
import json
import os
import shutil
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

root = Path(sys.argv[1])
out_dir = Path(sys.argv[2])
license_dir = Path(sys.argv[3])
deb_dir = Path(sys.argv[4])

required_sonames = [
    "libFLAC.so.8",
    "libjpeg.so.8",
    "libavcodec.so.58",
    "libavformat.so.58",
    "libavutil.so.56",
    "libswresample.so.3",
    "libswscale.so.5",
    "libvpx.so.6",
    "libwebp.so.6",
    "libaom.so.0",
    "libdav1d.so.4",
    "libcodec2.so.0.9",
    "libx264.so.160",
    "libx265.so.192",
    "libwavpack.so.1",
]

dependency_sonames = [
    "libwebpmux.so.3",
    "librsvg-2.so.2",
    "libzvbi.so.0",
    "libsnappy.so.1",
    "libgsm.so.1",
    "libmp3lame.so.0",
    "libopenjp2.so.7",
    "libshine.so.3",
    "libtwolame.so.0",
    "libxvidcore.so.4",
    "libva.so.2",
    "libgme.so.0",
    "libopenmpt.so.0",
    "libchromaprint.so.1",
    "libbluray.so.2",
    "librabbitmq.so.4",
    "libsrt-gnutls.so.1.4",
    "libssh-gcrypt.so.4",
    "libzmq.so.5",
    "libva-drm.so.2",
    "libva-x11.so.2",
    "libvdpau.so.1",
    "libsoxr.so.0",
    "libnorm.so.1",
    "libpgm-5.3.so.0",
    "libsodium.so.23",
    "libudfread.so.0",
    "libXfixes.so.3",
    "liblzma.so.5",
    "libgcrypt.so.20",
    "libgpg-error.so.0",
    "libgssapi_krb5.so.2",
    "libkrb5.so.3",
    "libk5crypto.so.3",
    "libkrb5support.so.0",
    "libkeyutils.so.1",
]

def sha256(path):
    h = hashlib.sha256()
    with path.open("rb") as fp:
        for chunk in iter(lambda: fp.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()

def readelf_dynamic(path):
    proc = subprocess.run(["readelf", "-d", str(path)], text=True, capture_output=True, check=True)
    soname = ""
    needed = []
    for line in proc.stdout.splitlines():
        if "Library soname:" in line:
            soname = line.split("[", 1)[1].split("]", 1)[0]
        elif "Shared library:" in line:
            needed.append(line.split("[", 1)[1].split("]", 1)[0])
    return soname, needed

def find_by_soname(name):
    exact = list(root.rglob(name))
    if exact:
        return exact[0]
    for path in root.rglob("*.so*"):
        if not path.is_file():
            continue
        try:
            soname, _ = readelf_dynamic(path)
        except subprocess.CalledProcessError:
            continue
        if soname == name:
            return path
    raise SystemExit(f"missing selected SONAME {name}")

packages = []
for deb in sorted(deb_dir.glob("*.deb")):
    fields = {}
    for key in ("Package", "Version", "Architecture", "Source", "Homepage"):
        try:
            value = subprocess.check_output(
                ["dpkg-deb", "-f", str(deb), key], text=True
            ).strip()
        except subprocess.CalledProcessError:
            value = ""
        fields[key.lower()] = value
    pkg = fields.get("package", "")
    if pkg:
        copyright_src = root / "usr" / "share" / "doc" / pkg / "copyright"
        if copyright_src.exists():
            shutil.copy2(copyright_src, license_dir / f"{pkg}.copyright")
    packages.append({
        "package": pkg,
        "version": fields.get("version", ""),
        "architecture": fields.get("architecture", ""),
        "source": fields.get("source", "") or pkg,
        "homepage": fields.get("homepage", ""),
        "deb": deb.name,
        "size": deb.stat().st_size,
        "sha256": sha256(deb),
    })

libraries = []
for name in required_sonames + dependency_sonames:
    src = find_by_soname(name)
    real_src = src.resolve()
    dst = out_dir / name
    shutil.copy2(real_src, dst)
    os.chmod(dst, 0o755)
    soname, needed = readelf_dynamic(dst)
    if soname and soname != name:
        raise SystemExit(f"{dst.name} has unexpected SONAME {soname}")
    libraries.append({
        "name": name,
        "kind": "required" if name in required_sonames else "dependency",
        "size": dst.stat().st_size,
        "sha256": sha256(dst),
        "needed": needed,
        "source_path": str(real_src.relative_to(root)),
    })

manifest = {
    "schema": 1,
    "product": "portmaster-mlp1-aarch64-compat-libs",
    "architecture": "aarch64",
    "install_dir": "$USERDATA_PATH/portmaster/compat/libs/aarch64",
    "policy": "app-local only; not written to stock OS/eMMC; opt-in through LEAF_PM_ENABLE_AARCH64_COMPAT_LIBS or leaf_pm_enable_aarch64_compat_libs",
    "sources": [
        {
            "distro": "Debian 11 bullseye",
            "architectures": ["arm64", "all"],
            "repos": [
                "http://deb.debian.org/debian bullseye main",
                "http://deb.debian.org/debian bullseye-updates main",
                "http://security.debian.org/debian-security bullseye-security main",
            ],
        },
        {
            "distro": "Ubuntu 20.04 focal",
            "package": "libjpeg-turbo8",
            "url": "http://ports.ubuntu.com/ubuntu-ports/pool/main/libj/libjpeg-turbo/libjpeg-turbo8_2.0.3-0ubuntu1.20.04.3_arm64.deb",
            "reason": "Debian bullseye provides libjpeg.so.62; the PortMaster CFW spec older set asks for libjpeg.so.8.",
        },
    ],
    "required_sonames": required_sonames,
    "dependency_sonames": dependency_sonames,
    "libraries": libraries,
    "packages": packages,
    "generated_at": datetime.now(timezone.utc).isoformat(),
}

(out_dir / "manifest.json").write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
PY

python3 -m json.tool "$OUT_DIR/manifest.json" >/dev/null
echo "aarch64 compatibility libraries ready: $OUT_DIR"
