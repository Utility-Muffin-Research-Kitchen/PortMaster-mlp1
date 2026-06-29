#!/usr/bin/env bash
set -euo pipefail

ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
LOCK="${UI_RUNTIME_LOCK:-$ROOT/locks/ui-runtime.lock.json}"
OUT_DIR="${1:-$ROOT/build/ui-runtime/cpython}"
SOURCES_DIR="${UI_RUNTIME_SOURCES_DIR:-$ROOT/build/ui-runtime/sources}"
PYTHON="${PYTHON:-python3}"
IMAGE="${MLP1_TOOLCHAIN_IMAGE:-ghcr.io/utility-muffin-research-kitchen/mlp1-toolchain:latest}"
INCLUDE_PILLOW="${INCLUDE_PILLOW:-0}"

need() {
  command -v "$1" >/dev/null 2>&1 || {
    echo "missing required tool: $1" >&2
    exit 1
  }
}

sha256_of() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" | awk '{print $1}'
  else
    shasum -a 256 "$1" | awk '{print $1}'
  fi
}

size_of() {
  if stat -f %z "$1" >/dev/null 2>&1; then
    stat -f %z "$1"
  else
    stat -c %s "$1"
  fi
}

bool_is_true() {
  case "${1:-}" in
    1|true|TRUE|yes|YES|on|ON) return 0 ;;
    *) return 1 ;;
  esac
}

default_jobs() {
  if [ -n "${BUILD_JOBS:-}" ]; then
    printf '%s\n' "$BUILD_JOBS"
  elif command -v sysctl >/dev/null 2>&1; then
    sysctl -n hw.ncpu
  elif command -v nproc >/dev/null 2>&1; then
    nproc
  else
    printf '4\n'
  fi
}

container_main() {
  need python3
  need tar
  need make

  local out_dir="${OUT_DIR_IN_CONTAINER:?missing OUT_DIR_IN_CONTAINER}"
  local sources_dir="${SOURCES_DIR_IN_CONTAINER:?missing SOURCES_DIR_IN_CONTAINER}"
  local cpython_filename="${CPYTHON_FILENAME:?missing CPYTHON_FILENAME}"
  local expected_sha="${CPYTHON_SHA256:?missing CPYTHON_SHA256}"
  local xz_filename="${XZ_FILENAME:?missing XZ_FILENAME}"
  local xz_expected_sha="${XZ_SHA256:?missing XZ_SHA256}"
  local jobs="${BUILD_JOBS:-$(default_jobs)}"
  local archive="$sources_dir/$cpython_filename"
  local xz_archive="$sources_dir/$xz_filename"

  if [ ! -f "$archive" ]; then
    echo "missing CPython source archive in container: $archive" >&2
    exit 1
  fi
  local actual_sha
  actual_sha="$(sha256_of "$archive")"
  if [ "$actual_sha" != "$expected_sha" ]; then
    echo "CPython source hash mismatch in container" >&2
    echo "expected $expected_sha" >&2
    echo "actual   $actual_sha" >&2
    exit 1
  fi
  if [ ! -f "$xz_archive" ]; then
    echo "missing XZ source archive in container: $xz_archive" >&2
    exit 1
  fi
  actual_sha="$(sha256_of "$xz_archive")"
  if [ "$actual_sha" != "$xz_expected_sha" ]; then
    echo "XZ source hash mismatch in container" >&2
    echo "expected $xz_expected_sha" >&2
    echo "actual   $actual_sha" >&2
    exit 1
  fi

  rm -rf "$out_dir/deps" "$out_dir/work" "$out_dir/root"
  mkdir -p "$out_dir/deps/work" "$out_dir/deps/root" "$out_dir/work" "$out_dir/root"
  tar -xf "$archive" -C "$out_dir/work"

  local src
  src="$(find "$out_dir/work" -maxdepth 1 -type d -name 'Python-*' | head -n 1)"
  if [ -z "$src" ] || [ ! -f "$src/configure" ]; then
    echo "CPython archive did not extract to Python-*/configure" >&2
    exit 1
  fi

  local build="$out_dir/work/build"
  mkdir -p "$build"

  local tool_cc="${CC:-aarch64-buildroot-linux-gnu-gcc}"
  local tool_cc_bin="${tool_cc%% *}"
  local sysroot="${SYSROOT:-$("$tool_cc_bin" -print-sysroot)}"
  export SYSROOT="$sysroot"
  export CC="$tool_cc --sysroot=$sysroot"
  export CXX="${CXX:-aarch64-buildroot-linux-gnu-g++} --sysroot=$sysroot"
  export AR="${AR:-aarch64-buildroot-linux-gnu-ar}"
  export RANLIB="${RANLIB:-aarch64-buildroot-linux-gnu-ranlib}"
  export READELF="${READELF:-aarch64-buildroot-linux-gnu-readelf}"
  export PKG_CONFIG_SYSROOT_DIR="$sysroot"
  export PKG_CONFIG_PATH="$sysroot/usr/lib/pkgconfig:$sysroot/usr/share/pkgconfig"
  export PKG_CONFIG_ALLOW_CROSS=1

  tar -xf "$xz_archive" -C "$out_dir/deps/work"
  local xz_src
  xz_src="$(find "$out_dir/deps/work" -maxdepth 1 -type d -name 'xz-*' | head -n 1)"
  if [ -z "$xz_src" ] || [ ! -f "$xz_src/configure" ]; then
    echo "XZ archive did not extract to xz-*/configure" >&2
    exit 1
  fi

  local deps_prefix="$out_dir/deps/root/portmaster"
  (
    cd "$xz_src"
    CFLAGS="-fPIC ${CFLAGS:-}" ./configure \
      --host=aarch64-buildroot-linux-gnu \
      --prefix=/portmaster \
      --enable-shared \
      --disable-static \
      --disable-xz \
      --disable-xzdec \
      --disable-lzmadec \
      --disable-lzmainfo \
      --disable-scripts \
      --disable-doc \
      --disable-nls
    make -j "$jobs"
    make install DESTDIR="$out_dir/deps/root"
  ) 2>&1 | tee "$out_dir/liblzma-build.log"

  export CPPFLAGS="-I$deps_prefix/include -I$sysroot/usr/include ${CPPFLAGS:-}"
  export LDFLAGS="-L$deps_prefix/lib -L$sysroot/usr/lib -Wl,-rpath-link,$deps_prefix/lib -Wl,-rpath-link,$sysroot/usr/lib ${LDFLAGS:-}"
  export PKG_CONFIG_PATH="$deps_prefix/lib/pkgconfig:$PKG_CONFIG_PATH"

  (
    cd "$build"
    "$src/configure" \
      --prefix=/portmaster \
      --enable-shared \
      --with-openssl="$sysroot/usr" \
      --with-openssl-rpath=auto \
      --with-system-expat \
      --with-system-ffi \
      --without-ensurepip

    make -j "$jobs" all _PYTHON_HOST_PLATFORM=linux-aarch64

    export LD_LIBRARY_PATH="$build:$deps_prefix/lib"
    export PYTHONPATH="$build/build/lib.linux-aarch64-3.10"
    export PYTHONDONTWRITEBYTECODE=1
    ./python.exe - <<'PY'
import _lzma
import _posixsubprocess
import ctypes
import hashlib
import json
import lzma
import sqlite3
import ssl
import subprocess
import sys
import zlib

print(sys.version)
print(ssl.OPENSSL_VERSION)
print(sqlite3.sqlite_version)
print("build-imports-ok")
PY

    make install DESTDIR="$out_dir/root" ENSUREPIP=no _PYTHON_HOST_PLATFORM=linux-aarch64
  ) 2>&1 | tee "$out_dir/cpython-build.log"

  local runtime="$out_dir/root/portmaster"
  if [ ! -x "$runtime/bin/python3.10" ]; then
    echo "install did not produce $runtime/bin/python3.10" >&2
    exit 1
  fi
  if [ ! -e "$runtime/bin/python3" ]; then
    cp -f "$runtime/bin/python3.10" "$runtime/bin/python3"
  fi
  if [ ! -e "$runtime/bin/python" ]; then
    cp -f "$runtime/bin/python3.10" "$runtime/bin/python"
  fi

  find "$runtime" -type l -print | while IFS= read -r link; do
    tmp="$link.leaf-copy"
    rm -rf "$tmp"
    if [ -f "$link" ]; then
      cp -pL "$link" "$tmp"
    elif [ -d "$link" ]; then
      mkdir -p "$tmp"
      cp -RpL "$link"/. "$tmp"/
    else
      rm -f "$link"
      continue
    fi
    rm -f "$link"
    mv "$tmp" "$link"
  done

  rm -rf "$runtime/lib/python3.10/test" \
         "$runtime/lib/python3.10/idlelib" \
         "$runtime/lib/python3.10/tkinter" \
         "$runtime/lib/python3.10/ensurepip" \
         "$runtime/lib/python3.10/turtledemo" \
         "$runtime/share"
  find "$runtime/lib/python3.10" -type d \
    \( -name '__pycache__' -o -name test -o -name tests \) -print |
    while IFS= read -r dir; do
      rm -rf "$dir"
    done
  find "$runtime/lib/python3.10" -type f \( -name '*.pyc' -o -name '*.pyo' \) -delete
  rm -f "$runtime/bin/2to3" "$runtime/bin/2to3-3.10" \
        "$runtime/bin/idle3" "$runtime/bin/idle3.10" \
        "$runtime/bin/pydoc3" "$runtime/bin/pydoc3.10" \
        "$runtime/bin/python-config" "$runtime/bin/python3-config" \
        "$runtime/bin/python3.10-config"
  rm -rf "$runtime/include" "$runtime/lib/pkgconfig" "$runtime/lib/python3.10/config-"*
  rm -f "$runtime/lib/libpython3.10.a"
  for lib in "$deps_prefix"/lib/liblzma.so*; do
    [ -e "$lib" ] || continue
    cp -pL "$lib" "$runtime/lib/$(basename "$lib")"
  done
  find "$runtime/lib/python3.10/lib-dynload" -maxdepth 1 -type f \
    \( -name '_test*.so' -o -name '_xx*.so' -o -name 'xx*.so' \) -delete

  local readelf_tool="${READELF:-aarch64-buildroot-linux-gnu-readelf}"
  local strip_tool="${STRIP:-aarch64-buildroot-linux-gnu-strip}"
  if command -v "$readelf_tool" >/dev/null 2>&1 && command -v "$strip_tool" >/dev/null 2>&1; then
    find "$runtime" -type f -print | while IFS= read -r file; do
      if "$readelf_tool" -h "$file" >/dev/null 2>&1; then
        "$strip_tool" --strip-unneeded "$file" 2>/dev/null || true
      fi
    done
  fi

  for module in _ssl _sqlite3 _ctypes _lzma _posixsubprocess zlib; do
    if ! ls "$runtime/lib/python3.10/lib-dynload/$module"*.so >/dev/null 2>&1; then
      echo "required CPython extension missing after install: $module" >&2
      exit 1
    fi
  done

  export LD_LIBRARY_PATH="$runtime/lib"
  export PYTHONHOME="$runtime"
  export PYTHONPATH="$runtime/lib/python3.10:$runtime/lib/python3.10/site-packages:$runtime/lib"
  export PYTHONDONTWRITEBYTECODE=1
  "$runtime/bin/python3" - <<'PY'
import _lzma
import _posixsubprocess
import ctypes
import hashlib
import json
import lzma
import sqlite3
import ssl
import subprocess
import sys
import zlib

print(sys.version)
print(ssl.OPENSSL_VERSION)
print(sqlite3.sqlite_version)
print("installed-imports-ok")
PY

  find "$runtime/lib/python3.10" -type d -name '__pycache__' -print |
    while IFS= read -r dir; do
      rm -rf "$dir"
    done
  find "$runtime/lib/python3.10" -type f \( -name '*.pyc' -o -name '*.pyo' \) -delete
}

if [ "${1:-}" = "--container" ]; then
  container_main
  exit 0
fi

need "$PYTHON"
need curl
need docker
need zip

mkdir -p "$OUT_DIR" "$SOURCES_DIR"

if bool_is_true "$INCLUDE_PILLOW"; then
  INCLUDE_OPTIONAL=1 "$ROOT/scripts/fetch-ui-runtime-sources.sh" "$SOURCES_DIR"
else
  "$ROOT/scripts/fetch-ui-runtime-sources.sh" "$SOURCES_DIR"
fi

cpython_plan="$OUT_DIR/.cpython-source.tsv"
"$PYTHON" - "$LOCK" >"$cpython_plan" <<'PY'
import json
import sys

with open(sys.argv[1], "r", encoding="utf-8") as fp:
    lock = json.load(fp)

for item in lock.get("source_inputs", []):
    if item.get("bucket") == "cpython" and item.get("type") == "source-tarball":
        print("\t".join([
            item["version"],
            item["filename"],
            str(item["size"]),
            item["sha256"],
            item["url"],
            item.get("license", ""),
        ]))
        break
else:
    raise SystemExit("ui-runtime lock has no cpython source-tarball input")
PY

IFS=$'\t' read -r cpython_version cpython_filename cpython_size cpython_sha cpython_url cpython_license <"$cpython_plan"
cpython_archive="$SOURCES_DIR/$cpython_filename"
if [ -f "$cpython_archive" ] &&
   [ "$(size_of "$cpython_archive")" = "$cpython_size" ] &&
   [ "$(sha256_of "$cpython_archive")" = "$cpython_sha" ]; then
  echo "OK $cpython_filename"
else
  tmp="$SOURCES_DIR/.download-cpython"
  rm -f "$tmp"
  curl -fL --retry 3 --output "$tmp" "$cpython_url"
  actual_size="$(size_of "$tmp")"
  actual_sha="$(sha256_of "$tmp")"
  if [ "$actual_size" != "$cpython_size" ]; then
    echo "size mismatch for $cpython_filename: expected $cpython_size got $actual_size" >&2
    exit 1
  fi
  if [ "$actual_sha" != "$cpython_sha" ]; then
    echo "sha256 mismatch for $cpython_filename: expected $cpython_sha got $actual_sha" >&2
    exit 1
  fi
  mv -f "$tmp" "$cpython_archive"
  echo "OK $cpython_filename"
fi

xz_plan="$OUT_DIR/.xz-source.tsv"
"$PYTHON" - "$LOCK" >"$xz_plan" <<'PY'
import json
import sys

with open(sys.argv[1], "r", encoding="utf-8") as fp:
    lock = json.load(fp)

for item in lock.get("source_inputs", []):
    if item.get("bucket") == "xz" and item.get("type") == "source-tarball":
        print("\t".join([
            item["version"],
            item["filename"],
            str(item["size"]),
            item["sha256"],
            item["url"],
            item.get("license", ""),
        ]))
        break
else:
    raise SystemExit("ui-runtime lock has no xz source-tarball input")
PY

IFS=$'\t' read -r xz_version xz_filename xz_size xz_sha xz_url xz_license <"$xz_plan"
xz_archive="$SOURCES_DIR/$xz_filename"
if [ -f "$xz_archive" ] &&
   [ "$(size_of "$xz_archive")" = "$xz_size" ] &&
   [ "$(sha256_of "$xz_archive")" = "$xz_sha" ]; then
  echo "OK $xz_filename"
else
  tmp="$SOURCES_DIR/.download-xz"
  rm -f "$tmp"
  curl -fL --retry 3 --output "$tmp" "$xz_url"
  actual_size="$(size_of "$tmp")"
  actual_sha="$(sha256_of "$tmp")"
  if [ "$actual_size" != "$xz_size" ]; then
    echo "size mismatch for $xz_filename: expected $xz_size got $actual_size" >&2
    exit 1
  fi
  if [ "$actual_sha" != "$xz_sha" ]; then
    echo "sha256 mismatch for $xz_filename: expected $xz_sha got $actual_sha" >&2
    exit 1
  fi
  mv -f "$tmp" "$xz_archive"
  echo "OK $xz_filename"
fi

workspace_root="$(cd "$ROOT/.." && pwd)"
case "$OUT_DIR" in
  "$workspace_root"/*) out_dir_container="/workspace/${OUT_DIR#"$workspace_root"/}" ;;
  *) echo "OUT_DIR must be under workspace root: $workspace_root" >&2; exit 1 ;;
esac
case "$SOURCES_DIR" in
  "$workspace_root"/*) sources_dir_container="/workspace/${SOURCES_DIR#"$workspace_root"/}" ;;
  *) echo "SOURCES_DIR must be under workspace root: $workspace_root" >&2; exit 1 ;;
esac

jobs="$(default_jobs)"
docker run --rm \
  -e OUT_DIR_IN_CONTAINER="$out_dir_container" \
  -e SOURCES_DIR_IN_CONTAINER="$sources_dir_container" \
  -e CPYTHON_FILENAME="$cpython_filename" \
  -e CPYTHON_SHA256="$cpython_sha" \
  -e XZ_FILENAME="$xz_filename" \
  -e XZ_SHA256="$xz_sha" \
  -e BUILD_JOBS="$jobs" \
  -v "$workspace_root":/workspace \
  -w /workspace/PortMaster-mlp1 \
  "$IMAGE" \
  bash scripts/build-ui-runtime-cpython.sh --container

runtime="$OUT_DIR/root/portmaster"
site="$runtime/lib/python3.10/site-packages"
mkdir -p "$site"

wheel_plan="$OUT_DIR/.wheel-plan.tsv"
"$PYTHON" - "$LOCK" "$INCLUDE_PILLOW" >"$wheel_plan" <<'PY'
import json
import sys

lock_path, include_pillow_raw = sys.argv[1:3]
include_pillow = include_pillow_raw.lower() in {"1", "true", "yes", "on"}
with open(lock_path, "r", encoding="utf-8") as fp:
    lock = json.load(fp)
for item in lock.get("source_inputs", []):
    if item.get("type") != "pypi-wheel":
        continue
    if item["bucket"] == "pillow" and not include_pillow:
        continue
    if item["bucket"] not in {"pysdl2-dll", "pillow"}:
        continue
    print("\t".join([item["bucket"], item["filename"], item["sha256"]]))
PY

while IFS=$'\t' read -r bucket filename expected_sha; do
  wheel="$SOURCES_DIR/$filename"
  if [ ! -f "$wheel" ]; then
    echo "missing fetched wheel: $wheel" >&2
    exit 1
  fi
  actual_sha="$(sha256_of "$wheel")"
  if [ "$actual_sha" != "$expected_sha" ]; then
    echo "wheel hash mismatch for $filename" >&2
    echo "expected $expected_sha" >&2
    echo "actual   $actual_sha" >&2
    exit 1
  fi
  "$PYTHON" -m zipfile -e "$wheel" "$site"
done <"$wheel_plan"

if [ -d "$site/sdl2dll" ]; then
  rm -rf "$runtime/lib/sdl2dll"
  cp -R "$site/sdl2dll" "$runtime/lib/sdl2dll"
fi
if [ -d "$site/pysdl2_dll-2.32.0.dist-info" ]; then
  rm -rf "$runtime/lib/pysdl2_dll-2.32.0.dist-info"
  cp -R "$site/pysdl2_dll-2.32.0.dist-info" "$runtime/lib/"
fi

chmod 755 "$runtime/bin/python" "$runtime/bin/python3" "$runtime/bin/python3.10" 2>/dev/null || true

image_id="$(docker image inspect --format '{{.Id}}' "$IMAGE" 2>/dev/null || true)"
image_digest="$(docker image inspect --format '{{range .RepoDigests}}{{println .}}{{end}}' "$IMAGE" 2>/dev/null | head -n 1 || true)"
runtime_manifest="$runtime/.leaf-runtime-manifest.json"
"$PYTHON" - "$LOCK" "$SOURCES_DIR" "$runtime_manifest" "$cpython_archive" \
  "$INCLUDE_PILLOW" "$IMAGE" "$image_digest" "$image_id" <<'PY'
import hashlib
import json
import os
import sys
from datetime import datetime, timezone

(
    lock_path,
    sources_dir,
    manifest_path,
    cpython_archive,
    include_pillow_raw,
    image,
    image_digest,
    image_id,
) = sys.argv[1:9]
include_pillow = include_pillow_raw.lower() in {"1", "true", "yes", "on"}

def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as fp:
        for chunk in iter(lambda: fp.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()

with open(lock_path, "r", encoding="utf-8") as fp:
    lock = json.load(fp)

sources = []
for item in lock.get("source_inputs", []):
    if item.get("bucket") == "cpython" and item.get("type") == "source-tarball":
        sources.append({
            "bucket": item["bucket"],
            "type": item["type"],
            "project": item.get("project", "CPython"),
            "version": item["version"],
            "filename": item["filename"],
            "size": os.path.getsize(cpython_archive),
            "sha256": sha256(cpython_archive),
            "license": item.get("license", ""),
            "url": item.get("url", ""),
            "production": True,
        })
        break

for item in lock.get("source_inputs", []):
    if item.get("bucket") == "xz" and item.get("type") == "source-tarball":
        path = os.path.join(sources_dir, item["filename"])
        sources.append({
            "bucket": item["bucket"],
            "type": item["type"],
            "project": item.get("project", "XZ Utils / liblzma"),
            "version": item["version"],
            "filename": item["filename"],
            "size": os.path.getsize(path),
            "sha256": sha256(path),
            "license": item.get("license", ""),
            "url": item.get("url", ""),
            "production": True,
        })
        break

for item in lock.get("source_inputs", []):
    if item.get("type") != "pypi-wheel":
        continue
    if item["bucket"] == "pillow" and not include_pillow:
        continue
    if item["bucket"] not in {"pysdl2-dll", "pillow"}:
        continue
    path = os.path.join(sources_dir, item["filename"])
    sources.append({
        "bucket": item["bucket"],
        "type": item["type"],
        "project": item["project"],
        "version": item["version"],
        "filename": item["filename"],
        "size": os.path.getsize(path),
        "sha256": sha256(path),
        "license": item.get("license", ""),
        "url": item.get("url", ""),
        "production": item["bucket"] != "pillow" or include_pillow,
    })

manifest = {
    "schema": 1,
    "product": lock.get("product"),
    "kind": "cpython-runtime",
    "production": True,
    "generated_at": datetime.now(timezone.utc).isoformat(),
    "target": lock.get("target", {}),
    "build": {
        "toolchain_image": image,
        "toolchain_image_digest": image_digest,
        "toolchain_image_id": image_id,
        "configure": [
            "--prefix=/portmaster",
            "--enable-shared",
            "--with-openssl=$SYSROOT/usr",
            "--with-openssl-rpath=auto",
            "--with-system-expat",
            "--with-system-ffi",
            "--without-ensurepip",
        ],
        "make_platform": "linux-aarch64",
        "runtime_pruned": True,
        "dependency_builds": [
            {
                "project": "XZ Utils / liblzma",
                "configure": [
                    "--host=aarch64-buildroot-linux-gnu",
                    "--prefix=/portmaster",
                    "--enable-shared",
                    "--disable-static",
                    "--disable-xz",
                    "--disable-xzdec",
                    "--disable-lzmadec",
                    "--disable-lzmainfo",
                    "--disable-scripts",
                    "--disable-doc",
                    "--disable-nls",
                ],
                "runtime_files": ["lib/liblzma.so*"],
            }
        ],
    },
    "sources": sources,
}
with open(manifest_path, "w", encoding="utf-8") as fp:
    json.dump(manifest, fp, indent=2)
    fp.write("\n")
PY

artifact="$OUT_DIR/portmaster-mlp1-ui-runtime-python310-aarch64-cpython-$cpython_version.zip"
manifest="$OUT_DIR/portmaster-mlp1-ui-runtime-python310-aarch64-cpython-$cpython_version.json"
rm -f "$artifact" "$manifest"
(cd "$OUT_DIR/root" && zip -X -qr "$artifact" portmaster)

"$PYTHON" - "$runtime_manifest" "$artifact" "$manifest" "$runtime" <<'PY'
import hashlib
import json
import os
import sys
from datetime import datetime, timezone

runtime_manifest, artifact_path, manifest_path, runtime_root = sys.argv[1:5]

def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as fp:
        for chunk in iter(lambda: fp.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()

with open(runtime_manifest, "r", encoding="utf-8") as fp:
    manifest = json.load(fp)

file_count = 0
installed_size = 0
for dirpath, _, filenames in os.walk(runtime_root):
    for name in filenames:
        path = os.path.join(dirpath, name)
        file_count += 1
        installed_size += os.path.getsize(path)

manifest["artifact"] = {
    "path": artifact_path,
    "filename": os.path.basename(artifact_path),
    "size": os.path.getsize(artifact_path),
    "sha256": sha256(artifact_path),
    "installed_file_count": file_count,
    "installed_size": installed_size,
}
manifest["generated_at"] = datetime.now(timezone.utc).isoformat()

with open(manifest_path, "w", encoding="utf-8") as fp:
    json.dump(manifest, fp, indent=2)
    fp.write("\n")
PY

printf '%s  %s\n' "$(sha256_of "$artifact")" "$artifact"
echo "wrote $manifest"
