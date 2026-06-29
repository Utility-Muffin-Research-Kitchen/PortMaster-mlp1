#!/usr/bin/env python3
"""Map Spruce PortMaster binary inventory rows to Leaf source/delivery decisions."""

import argparse
from collections import Counter
import csv
import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_INVENTORY = ROOT / "docs/generated/spruce-portmaster-binary-inventory.tsv"
DEFAULT_OUTPUT = ROOT / "docs/generated/spruce-portmaster-bin-closure.tsv"
UI_LOCK = ROOT / "locks/ui-runtime.lock.json"
PM_LOCK = ROOT / "locks/portmaster-gui-stable.lock.json"


def load_json(path):
    with path.open("r", encoding="utf-8") as fp:
        return json.load(fp)


def lock_source(lock, bucket):
    for item in lock.get("source_inputs", []):
        if item.get("bucket") == bucket:
            return item
    return {}


def artifact_by_kind(lock, kind):
    for item in lock.get("artifacts", []):
        if item.get("kind") == kind:
            return item
    return {}


def basename(path):
    return path.rsplit("/", 1)[-1]


def close_row(row, ui_lock, pm_lock):
    path = row["path"]
    bucket = row["bucket"]
    strategy = row["strategy"]
    kind = row["kind"]

    if bucket == "upstream-portmaster-payload":
        return {
            "closure_status": "covered-upstream-portmaster",
            "leaf_source": f"PortsMaster/PortMaster-GUI {pm_lock.get('tag', '')}",
            "leaf_delivery": "$USERDATA_PATH/portmaster/PortMaster from locked PortMaster.zip",
            "default_install": "yes",
            "reason": "Installed from the hash-pinned upstream PortMaster release; Leaf only patches around it.",
        }

    if bucket == "pillow-offline-wheel":
        item = lock_source(ui_lock, "pillow")
        return {
            "closure_status": "covered-optional-pypi-wheel",
            "leaf_source": f"{item.get('project', 'Pillow')} {item.get('version', '')}",
            "leaf_delivery": "Optional locked PyPI wheel source; not installed in required UI runtime",
            "default_install": "no",
            "reason": "Spruce uses this for offline Pillow/image tooling; Leaf UI launch has not required Pillow.",
        }

    if bucket != "ui-python-runtime":
        return {
            "closure_status": "unknown",
            "leaf_source": "",
            "leaf_delivery": "",
            "default_install": "no",
            "reason": "Unknown inventory bucket.",
        }

    if strategy == "fetch-pysdl2-dll-or-build-sdl-stack":
        item = lock_source(ui_lock, "pysdl2-dll")
        leaf_path = path
        if path.startswith("site-packages/"):
            leaf_path = "lib/python3.10/" + path
        return {
            "closure_status": "covered-runtime-pysdl2-dll-disabled",
            "leaf_source": f"{item.get('project', 'pysdl2-dll')} {item.get('version', '')}",
            "leaf_delivery": f"$USERDATA_PATH/portmaster/runtime/{leaf_path}",
            "default_install": "yes",
            "reason": "Present in the runtime artifact for parity/provenance, but not used by launch because the bundled SDL stack segfaulted on MLP1.",
        }

    if strategy == "build-cpython-runtime":
        item = lock_source(ui_lock, "cpython")
        artifact = artifact_by_kind(ui_lock, "cpython-runtime")
        if kind == "placeholder":
            return {
                "closure_status": "covered-runtime-fat32-copy",
                "leaf_source": f"{item.get('project', 'CPython')} {item.get('version', '')}",
                "leaf_delivery": f"{artifact.get('filename', 'UI runtime artifact')} flattens symlinks/placeholders for FAT32",
                "default_install": "yes",
                "reason": "Spruce placeholder/symlink shape is intentionally replaced by real files for FAT32-safe SD installs.",
            }
        if (
            path.startswith("bin/python")
            or path.startswith("lib/libpython")
            or path == "lib/python3.10/cgi.py"
            or path == "lib/python3.10/ctypes/macholib/fetch_macholib"
            or "/lib-dynload/_crypt." in path
        ):
            return {
                "closure_status": "covered-runtime-cpython-source",
                "leaf_source": f"{item.get('project', 'CPython')} {item.get('version', '')}",
                "leaf_delivery": f"$USERDATA_PATH/portmaster/runtime/{path}",
                "default_install": "yes",
                "reason": "Built from locked CPython source inside the MLP1 toolchain runtime artifact.",
            }
        if "config-3.10" in path or path.endswith("python.o") or "/lib-dynload/_testclinic." in path:
            return {
                "closure_status": "excluded-dev-only",
                "leaf_source": f"{item.get('project', 'CPython')} {item.get('version', '')}",
                "leaf_delivery": "Not shipped",
                "default_install": "no",
                "reason": "Build/config/test artifacts are not required for PortMaster UI execution and were pruned from the runtime.",
            }

    if strategy == "review" and path.startswith("lib/lib") and kind == "elf-shared":
        item = lock_source(ui_lock, "pysdl2-dll")
        return {
            "closure_status": "covered-duplicate-runtime-pysdl2-dll-disabled",
            "leaf_source": f"{item.get('project', 'pysdl2-dll')} {item.get('version', '')}",
            "leaf_delivery": f"$USERDATA_PATH/portmaster/runtime/lib/sdl2dll/dll/{basename(path)}",
            "default_install": "yes",
            "reason": "Spruce duplicated the same SDL stack at top-level lib/; Leaf keeps one canonical disabled copy plus stock /usr/lib SDL for launch.",
        }

    if strategy == "review" and path.startswith("bin/"):
        return {
            "closure_status": "excluded-dev-tool",
            "leaf_source": "CPython source runtime",
            "leaf_delivery": "Not shipped",
            "default_install": "no",
            "reason": "pip/idle/pydoc/2to3 wrappers are development tools, not PortMaster runtime ABI.",
        }

    if strategy == "review" and path.startswith("lib/tk8.6/demos/"):
        return {
            "closure_status": "excluded-tk-demo",
            "leaf_source": "CPython/Tk demo files",
            "leaf_delivery": "Not shipped",
            "default_install": "no",
            "reason": "Tk demos are unrelated to PortMaster and were pruned from the runtime.",
        }

    return {
        "closure_status": "unknown",
        "leaf_source": "",
        "leaf_delivery": "",
        "default_install": "no",
        "reason": "No closure rule matched.",
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--inventory", type=Path, default=DEFAULT_INVENTORY)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    args = parser.parse_args()

    ui_lock = load_json(UI_LOCK)
    pm_lock = load_json(PM_LOCK)
    with args.inventory.open("r", encoding="utf-8", newline="") as fp:
        rows = list(csv.DictReader(fp, delimiter="\t"))

    fields = [
        "artifact",
        "path",
        "kind",
        "arch",
        "bucket",
        "strategy",
        "sha256",
        "closure_status",
        "leaf_source",
        "leaf_delivery",
        "default_install",
        "reason",
    ]

    args.output.parent.mkdir(parents=True, exist_ok=True)
    closed_rows = []
    with args.output.open("w", encoding="utf-8", newline="") as fp:
        writer = csv.DictWriter(fp, fieldnames=fields, delimiter="\t", lineterminator="\n")
        writer.writeheader()
        for row in rows:
            closed = close_row(row, ui_lock, pm_lock)
            out_row = {field: row.get(field, "") for field in fields}
            out_row.update(closed)
            writer.writerow(out_row)
            closed_rows.append(out_row)

    unknown = [row for row in closed_rows if row["closure_status"] == "unknown"]
    if unknown:
        for row in unknown:
            print(f"unknown closure: {row['path']}", flush=True)
        raise SystemExit(1)
    print(f"wrote {args.output}")
    for status, count in Counter(row["closure_status"] for row in closed_rows).most_common():
        print(f"{count} {status}")


if __name__ == "__main__":
    main()
