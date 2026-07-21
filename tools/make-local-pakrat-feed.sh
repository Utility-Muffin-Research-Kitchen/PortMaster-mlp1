#!/usr/bin/env bash
set -euo pipefail

APP_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FEED_ROOT="$APP_DIR/build/local-pakrat-feed"
BASE_URL="${PAKRAT_LOCAL_BASE_URL:-http://127.0.0.1:${PAKRAT_LOCAL_PORT:-8765}/pakrat/v1/}"
HISTORY_PATH="$APP_DIR/tools/pakrat-history-v0.1.2.json"
BUILD_FIRST=1
INCLUDE_RUNTIME=auto
INCLUDE_ARMHF=auto

usage() {
    cat <<USAGE
usage: tools/make-local-pakrat-feed.sh [options]

Options:
  --feed-root PATH       Output root served by python -m http.server.
  --base-url URL         Pak Rat v1 base URL. Default: $BASE_URL
  --history PATH         Immutable catalog history. Default: $HISTORY_PATH
  --skip-build           Use the existing build/mlp1 package.
  --no-runtime           Do not copy/rewrite the UI runtime lock for local use.
  --no-armhf             Do not copy/rewrite the armhf compatibility lock.

The generated catalog is local-only and may use http://127.0.0.1 URLs.
USAGE
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --feed-root)
            FEED_ROOT="${2:?missing --feed-root value}"
            shift 2
            ;;
        --base-url)
            BASE_URL="${2:?missing --base-url value}"
            shift 2
            ;;
        --history)
            HISTORY_PATH="${2:?missing --history value}"
            shift 2
            ;;
        --skip-build)
            BUILD_FIRST=0
            shift
            ;;
        --no-runtime)
            INCLUDE_RUNTIME=0
            shift
            ;;
        --no-armhf)
            INCLUDE_ARMHF=0
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "unknown option: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

case "$BASE_URL" in
    */) ;;
    *) BASE_URL="$BASE_URL/" ;;
esac

if [ "$BUILD_FIRST" -eq 1 ]; then
    make -C "$APP_DIR" dist-pakrat
fi

APP_DIR="$APP_DIR" FEED_ROOT="$FEED_ROOT" BASE_URL="$BASE_URL" \
HISTORY_PATH="$HISTORY_PATH" \
INCLUDE_RUNTIME="$INCLUDE_RUNTIME" INCLUDE_ARMHF="$INCLUDE_ARMHF" python3 - <<'PY'
import hashlib
import json
import os
import shutil
import stat
import time
import zipfile
from pathlib import Path

app_dir = Path(os.environ["APP_DIR"])
feed_root = Path(os.environ["FEED_ROOT"])
base_url = os.environ["BASE_URL"]
history_path = Path(os.environ["HISTORY_PATH"])
include_runtime = os.environ["INCLUDE_RUNTIME"] != "0"
include_armhf = os.environ["INCLUDE_ARMHF"] != "0"

package_src = app_dir / "build/mlp1/package/PortMaster.pak"
pak_json_path = package_src / "pak.json"
pakrat_json_path = app_dir / "pakrat.json"
runtime_build_dir = app_dir / "build/ui-runtime/cpython"
armhf_build_dir = app_dir / "build/armhf-compat"

if not package_src.is_dir():
    raise SystemExit(f"missing package directory: {package_src}")
if not pak_json_path.is_file():
    raise SystemExit(f"missing pak.json: {pak_json_path}")
if not pakrat_json_path.is_file():
    raise SystemExit(f"missing pakrat.json: {pakrat_json_path}")
if not history_path.is_file():
    raise SystemExit(f"missing Pak Rat history: {history_path}")

pak = json.loads(pak_json_path.read_text())
pakrat = json.loads(pakrat_json_path.read_text())
history = json.loads(history_path.read_text())
package_meta = pakrat["leaf"]["packages"][0]
version = pak["pak_version"]
minimum = package_meta.get("min_leaf_version")
install_name = package_meta["install_name"]
artifact_name = package_meta["artifact_name"]
if version != package_meta["version"]:
    raise SystemExit(
        f"pak.json version {version!r} does not match pakrat.json "
        f"version {package_meta['version']!r}"
    )
if pak.get("min_leaf_version") != minimum:
    raise SystemExit(
        f"pak.json min_leaf_version {pak.get('min_leaf_version')!r} does not "
        f"match pakrat.json min_leaf_version {minimum!r}"
    )

history_apps = [
    app for app in history.get("apps", []) if app.get("id") == pakrat["id"]
]
if len(history_apps) != 1:
    raise SystemExit(f"{history_path}: expected one history entry for {pakrat['id']}")
history_packages = [
    package
    for package in history_apps[0].get("packages", [])
    if package.get("platform") == package_meta["platform"]
    and package.get("install_name") == install_name
]
if len(history_packages) != 1:
    raise SystemExit(f"{history_path}: expected one matching package history")
history_package = history_packages[0]
if history_package.get("runtime_manifest_path") != package_meta["runtime_manifest_path"]:
    raise SystemExit(f"{history_path}: runtime manifest path changed")
history_versions = history_package.get("versions", [])
safe_floors = [
    entry for entry in history_versions if "min_leaf_version" not in entry
]
if not history_versions or not safe_floors:
    raise SystemExit(f"{history_path}: expected an ungated safe-floor history")
safe_floor = max(
    safe_floors,
    key=lambda entry: tuple(int(part) for part in entry["version"].split(".")),
)
if (
    history_apps[0].get("version") != safe_floor.get("version")
    or history_package.get("version") != safe_floor.get("version")
    or history_package.get("artifact") != safe_floor.get("artifact")
):
    raise SystemExit(f"{history_path}: legacy fields do not match the safe floor")

shutil.rmtree(feed_root, ignore_errors=True)
catalog_dir = feed_root / "pakrat/v1"
artifact_dir = catalog_dir / "artifacts"
package_artifact_dir = artifact_dir / pakrat["id"] / version
work_dir = feed_root / "_work"
staged_package = work_dir / install_name
package_artifact_dir.mkdir(parents=True, exist_ok=True)
work_dir.mkdir(parents=True, exist_ok=True)
shutil.copytree(package_src, staged_package)

runtime_included = False
lock_path = staged_package / "locks/ui-runtime.lock.json"
if include_runtime and lock_path.is_file():
    lock = json.loads(lock_path.read_text())
    for artifact in lock.get("artifacts", []):
        if artifact.get("kind") != "cpython-runtime":
            continue
        runtime_zip = runtime_build_dir / artifact["filename"]
        manifest = artifact.get("manifest", {})
        runtime_manifest = runtime_build_dir / manifest.get("filename", "")
        if not runtime_zip.is_file() or not runtime_manifest.is_file():
            break
        for source in (runtime_zip, runtime_manifest):
            shutil.copy2(source, artifact_dir / source.name)
        artifact["url"] = base_url + "artifacts/" + runtime_zip.name
        artifact["size"] = runtime_zip.stat().st_size
        artifact["sha256"] = hashlib.sha256(runtime_zip.read_bytes()).hexdigest()
        manifest["url"] = base_url + "artifacts/" + runtime_manifest.name
        manifest["size"] = runtime_manifest.stat().st_size
        manifest["sha256"] = hashlib.sha256(runtime_manifest.read_bytes()).hexdigest()
        runtime_included = True
        break
    lock_path.write_text(json.dumps(lock, indent=2) + "\n")

armhf_included = False
armhf_lock_path = staged_package / "locks/armhf-compat.lock.json"
if include_armhf and armhf_lock_path.is_file():
    lock = json.loads(armhf_lock_path.read_text())
    artifacts = lock.get("artifacts", [])
    if len(artifacts) == 1:
        artifact = artifacts[0]
        archive = armhf_build_dir / artifact["name"]
        manifest = artifact.get("manifest", {})
        manifest_path = armhf_build_dir / manifest.get("filename", "")
        if archive.is_file() and manifest_path.is_file():
            for source in (archive, manifest_path):
                shutil.copy2(source, artifact_dir / source.name)
            artifact["url"] = base_url + "artifacts/" + archive.name
            artifact["size"] = archive.stat().st_size
            artifact["sha256"] = hashlib.sha256(archive.read_bytes()).hexdigest()
            manifest["url"] = base_url + "artifacts/" + manifest_path.name
            manifest["size"] = manifest_path.stat().st_size
            manifest["sha256"] = hashlib.sha256(manifest_path.read_bytes()).hexdigest()
            armhf_included = True
    armhf_lock_path.write_text(json.dumps(lock, indent=2) + "\n")

installed_size = 0
for path in staged_package.rglob("*"):
    if path.is_file():
        installed_size += path.stat().st_size

app_zip = package_artifact_dir / artifact_name
with zipfile.ZipFile(app_zip, "w", compression=zipfile.ZIP_DEFLATED) as zf:
    for path in sorted(staged_package.rglob("*")):
        rel = path.relative_to(work_dir)
        info = zipfile.ZipInfo(str(rel))
        st = path.stat()
        info.date_time = time.localtime(st.st_mtime)[:6]
        mode = stat.S_IMODE(st.st_mode)
        if path.is_dir():
            info.filename += "/"
            info.external_attr = ((mode or 0o755) | stat.S_IFDIR) << 16
            zf.writestr(info, b"")
        else:
            info.external_attr = ((mode or 0o644) | stat.S_IFREG) << 16
            zf.writestr(info, path.read_bytes())

app_sha = hashlib.sha256(app_zip.read_bytes()).hexdigest()
generated_at = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
revision = time.strftime("local-%Y%m%dT%H%M%SZ", time.gmtime())
version_key = lambda value: tuple(int(part) for part in value.split("."))
history_version_names = [entry["version"] for entry in history_versions]
if (
    len(set(history_version_names)) != len(history_version_names)
    or version in history_version_names
    or version_key(version) <= max(version_key(value) for value in history_version_names)
):
    raise SystemExit(f"{history_path}: current version must be newer than history")
current_entry = {
    "version": version,
    "artifact": {
        "url": (
            base_url
            + "artifacts/"
            + pakrat["id"]
            + "/"
            + version
            + "/"
            + artifact_name
        ),
        "name": artifact_name,
        "archive": "zip",
        "size": app_zip.stat().st_size,
        "installed_size": installed_size,
        "sha256": app_sha,
    },
}
if minimum is not None:
    current_entry["min_leaf_version"] = minimum
all_versions = sorted(
    [current_entry, *history_versions],
    key=lambda entry: version_key(entry["version"]),
    reverse=True,
)

catalog = {
    "schema": 1,
    "product": "pak-rat",
    "catalog_revision": revision,
    "generated_at": generated_at,
    "apps": [
        {
            "id": pakrat["id"],
            "name": pakrat["name"],
            "summary": pakrat["summary"],
            "description": pakrat["description"],
            "author": pakrat["author"],
            "repo_url": pakrat["repo_url"],
            "categories": pakrat["categories"],
            "version": safe_floor["version"],
            "packages": [
                {
                    "platform": package_meta["platform"],
                    "runtime": "leaf",
                    "version": safe_floor["version"],
                    "install_name": install_name,
                    "runtime_manifest_path": package_meta["runtime_manifest_path"],
                    "artifact": safe_floor["artifact"],
                    "versions": all_versions,
                }
            ],
        }
    ],
}

(catalog_dir / "storefront.json").write_text(json.dumps(catalog, indent=2) + "\n")
(feed_root / "README.txt").write_text(
    "Local PortMaster Pak Rat feed.\n"
    f"Base URL: {base_url}\n"
    f"Runtime included: {'yes' if runtime_included else 'no'}\n"
    f"armhf compatibility included: {'yes' if armhf_included else 'no'}\n"
)

print(f"Local Pak Rat feed: {feed_root}")
print(f"Catalog URL: {base_url}storefront.json")
print(f"PortMaster zip: {app_zip} ({app_zip.stat().st_size} bytes, sha256 {app_sha})")
if runtime_included:
    print("UI runtime lock rewritten to local feed artifacts.")
else:
    print("UI runtime artifact not included; runtime install will use the lock's existing URL.")
if armhf_included:
    print("armhf compatibility lock rewritten to local feed artifacts.")
else:
    print("armhf compatibility artifact not included; install will use the lock's existing URL.")
PY
