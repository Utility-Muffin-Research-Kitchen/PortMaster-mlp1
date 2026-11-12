#!/usr/bin/env bash
set -euo pipefail

ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
LOCK="$ROOT/locks/aarch64-compat-libs.lock.json"
IMAGE="${AARCH64_COMPAT_LIBS_IMAGE:-debian:bookworm-slim}"
# Without --platform, Docker runs whichever variant of the image it has stored,
# which need not be the host's own. Default to the Docker host's platform.
PLATFORM="${AARCH64_COMPAT_LIBS_PLATFORM:-}"
BUILD_DIR="$ROOT/build/mlp1"
WORK_DIR="$ROOT/build/aarch64-compat-libs"
DEB_DIR="$WORK_DIR/debs"
ROOTFS="$WORK_DIR/root"
OUT_DIR="$BUILD_DIR/compat/libs/aarch64"
LICENSE_DIR="$BUILD_DIR/licenses/aarch64-compat-libs"
# Build into the work dir and only replace the previous output once the whole
# pack is complete, so a failed download keeps the last good build.
STAGE_OUT_DIR="$WORK_DIR/out/libs"
STAGE_LICENSE_DIR="$WORK_DIR/out/licenses"
container=0

if [[ "${1:-}" == "--container" ]]; then
  container=1
  shift
fi

if [[ "$container" == "0" ]]; then
  if [ -z "$PLATFORM" ]; then
    PLATFORM="$(docker version --format '{{.Server.Os}}/{{.Server.Arch}}')"
  fi
  exec docker run --rm \
    --platform "$PLATFORM" \
    -v "$ROOT":/work \
    -w /work \
    "$IMAGE" \
    bash scripts/build-aarch64-compat-libs-pack.sh --container
fi

rm -rf "$WORK_DIR"
mkdir -p "$DEB_DIR" "$ROOTFS" "$STAGE_OUT_DIR" "$STAGE_LICENSE_DIR/debian"

apt-get update
apt-get install -y --no-install-recommends \
  binutils \
  ca-certificates \
  curl \
  dpkg-dev \
  python3

# Fetch the bullseye arm64 debs with an apt state of their own: a private
# sources list, index lists and cache, an empty dpkg status, and arm64 as the
# only architecture. apt-get download takes each package at the version the
# lock pins and resolves no dependencies, so the build container's architecture
# and installed packages can't change the set. Resolving against them is what
# broke this step when Docker ran the image as i386 instead of arm64.
#
# Debian 11 left LTS, and its packages moved off deb.debian.org and
# security.debian.org. archive.debian.org keeps the final bullseye indexes and
# pools for good, so the pinned versions stay resolvable. Its Release files
# carry no Valid-Until, so apt needs no Check-Valid-Until override.
APT_DIR="$(mktemp -d /tmp/leaf-bullseye-apt.XXXXXX)"
trap 'rm -rf "$APT_DIR"' EXIT
# apt fetches indexes as the _apt user, which needs to reach the lists dir.
chmod 755 "$APT_DIR"
mkdir -p "$APT_DIR/lists/partial" "$APT_DIR/cache/archives/partial"
: >"$APT_DIR/status"
cat >"$APT_DIR/sources.list" <<'EOF'
deb http://archive.debian.org/debian bullseye main
deb http://archive.debian.org/debian bullseye-updates main
deb http://archive.debian.org/debian-security bullseye-security main
EOF

apt_bullseye=(
  -o Dir::Etc::sourcelist="$APT_DIR/sources.list"
  -o Dir::Etc::sourceparts=-
  -o Dir::State::lists="$APT_DIR/lists"
  -o Dir::State::status="$APT_DIR/status"
  -o Dir::Cache="$APT_DIR/cache"
  -o APT::Architecture=arm64
  -o APT::Architectures=arm64
)

python3 - "$LOCK" >"$APT_DIR/pins" <<'PY'
import json
import sys

with open(sys.argv[1], "r", encoding="utf-8") as fp:
    debs = json.load(fp)["debian_packages"]
for entry in debs["requested"] + debs["dependencies"]:
    print(f"{entry['package']}={entry['version']}")
PY
mapfile -t debian_pins <"$APT_DIR/pins"

apt-get "${apt_bullseye[@]}" update
# apt-get download writes to the current directory, so run it from the deb dir.
(
  cd "$DEB_DIR"
  apt-get "${apt_bullseye[@]}" download "${debian_pins[@]}"
)

# The deb dir must now hold exactly the locked debs, byte for byte.
python3 - "$LOCK" "$DEB_DIR" <<'PY'
import hashlib
import json
import subprocess
import sys
from pathlib import Path

with open(sys.argv[1], "r", encoding="utf-8") as fp:
    debs = json.load(fp)["debian_packages"]
expected = {entry["package"]: entry for entry in debs["requested"] + debs["dependencies"]}
seen = set()
errors = []
for deb in sorted(Path(sys.argv[2]).glob("*.deb")):
    fields = {}
    control = subprocess.check_output(
        ["dpkg-deb", "-f", str(deb), "Package", "Version", "Architecture"], text=True
    )
    for line in control.splitlines():
        key, _, value = line.partition(":")
        fields[key.strip()] = value.strip()
    name = fields.get("Package", "")
    entry = expected.get(name)
    if entry is None:
        errors.append(f"{deb.name}: {name or 'unknown package'} is not in the lock")
        continue
    if name in seen:
        errors.append(f"{deb.name}: {name} downloaded twice")
    seen.add(name)
    got = (fields.get("Version"), fields.get("Architecture"))
    if got != (entry["version"], entry["architecture"]):
        errors.append(f"{deb.name}: got {got[0]} {got[1]}, lock pins {entry['version']} {entry['architecture']}")
    digest = hashlib.sha256(deb.read_bytes()).hexdigest()
    if digest != entry["sha256"]:
        errors.append(f"{deb.name}: sha256 {digest}, lock pins {entry['sha256']}")
for name in sorted(set(expected) - seen):
    errors.append(f"{name}: not downloaded")
if errors:
    raise SystemExit("bullseye debs don't match the lock:\n  " + "\n  ".join(errors))
print(f"bullseye debs match the lock: {len(seen)} packages")
PY

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

python3 - "$ROOTFS" "$STAGE_OUT_DIR" "$STAGE_LICENSE_DIR/debian" "$DEB_DIR" <<'PY'
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
        if not copyright_src.exists():
            raise SystemExit(f"{deb.name} has no /usr/share/doc/{pkg}/copyright")
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
                "http://archive.debian.org/debian bullseye main",
                "http://archive.debian.org/debian bullseye-updates main",
                "http://archive.debian.org/debian-security bullseye-security main",
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

python3 -m json.tool "$STAGE_OUT_DIR/manifest.json" >/dev/null

replace_output() {
  local staged="$1" dst="$2" old="$WORK_DIR/replaced-$3"
  mkdir -p "$(dirname "$dst")"
  rm -rf "$old"
  if [ -e "$dst" ]; then
    mv "$dst" "$old"
  fi
  mv "$staged" "$dst"
  rm -rf "$old"
}

replace_output "$STAGE_OUT_DIR" "$OUT_DIR" libs
replace_output "$STAGE_LICENSE_DIR" "$LICENSE_DIR" licenses
echo "aarch64 compatibility libraries ready: $OUT_DIR"
