#!/usr/bin/env python3
"""Focused fixtures for the Leaf HarbourMaster multi-source overlay."""

import json
import os
import sys
import tempfile
import types
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PATCH = ROOT / "patches/portmaster-gui/mlp1/0009-leaf-multi-source-inventory.patch"


def load_overlay():
    lines = PATCH.read_text().splitlines()
    start = lines.index("+++ pylibs/harbourmaster/leaf_sources.py") + 1
    source = []
    for line in lines[start:]:
        if line.startswith("--- "):
            break
        if line.startswith("@@"):
            continue
        if line.startswith("+"):
            source.append(line[1:])
    package = types.ModuleType("leaf_fixture")
    package.__path__ = []
    util = types.ModuleType("leaf_fixture.util")
    util.name_cleaner = lambda value: Path(value).name.casefold().removesuffix(".zip")
    loguru = types.ModuleType("loguru")
    loguru.logger = types.SimpleNamespace(error=lambda *args, **kwargs: None)
    sys.modules["leaf_fixture"] = package
    sys.modules["leaf_fixture.util"] = util
    sys.modules["loguru"] = loguru
    module = types.ModuleType("leaf_fixture.leaf_sources")
    module.__package__ = "leaf_fixture"
    exec(compile("\n".join(source), str(PATCH), "exec"), module.__dict__)
    return module


class Callback:
    def __init__(self):
        self.messages = []

    def message_box(self, text):
        self.messages.append(text)


class FakeHarbour:
    def __init__(self, tools, ports, records):
        self.tools_dir = tools
        self.ports_dir = ports
        self.scripts_dir = ports
        self.callback = Callback()
        self.records = records
        self.installed_ports = {}
        self.broken_ports = {}
        self.unknown_ports = []

    def _leaf_load_ports_one_source(self):
        source = self.records[str(self.ports_dir)]
        self.installed_ports = {
            name: {"name": name, "files": {}}
            for name in source.get("installed", [])}
        self.broken_ports = {
            name: {"name": name, "files": {}}
            for name in source.get("broken", [])}
        self.unknown_ports = list(source.get("unknown", []))


def mount_line(mount_id, device, root):
    return (
        f"{mount_id} 1 {device} / {root} rw,relatime - vfat "
        f"/dev/mmcblk{mount_id} rw\n")


def main():
    overlay = load_overlay()
    with tempfile.TemporaryDirectory(prefix="leaf-pm-sources-") as temp:
        temp = Path(temp)
        primary = temp / "primary"
        secondary = temp / "secondary"
        primary_ports = primary / "Roms" / "PORTS"
        secondary_ports = secondary / "Roms" / "PORTS"
        primary_ports.mkdir(parents=True)
        secondary_ports.mkdir(parents=True)
        mountinfo = temp / "mountinfo"
        mountinfo.write_text(
            mount_line(41, "179:1", primary) +
            mount_line(42, "179:2", secondary))

        os.environ.update({
            "HM_PORTS_READ_SOURCE_IDS": "primary:secondary_sd",
            "HM_PORTS_READ_DIRS": f"{primary_ports}:{secondary_ports}",
            "HM_SCRIPTS_READ_DIRS": f"{primary_ports}:{secondary_ports}",
            "LEAF_PM_SOURCE_ROOTS": f"{primary}:{secondary}",
            "LEAF_PM_SOURCE_MOUNT_IDS": "41:42",
            "LEAF_PM_SOURCE_DEVICES": "179,1:179,2",
            "LEAF_PM_SOURCE_ST_DEVS":
                f"{primary.stat().st_dev}:{secondary.stat().st_dev}",
            "LEAF_PM_SOURCE_PORTS_ST_DEVS":
                f"{primary_ports.stat().st_dev}:{secondary_ports.stat().st_dev}",
            "LEAF_PM_SOURCE_FINGERPRINTS": "-:-",
            "LEAF_PM_SELECTED_SOURCE_ID": "secondary_sd",
            "LEAF_PM_LAST_KNOWN_INVENTORY": str(temp / "inventory.json"),
            "LEAF_PM_TEST_MOUNTINFO": str(mountinfo),
        })

        harbour = FakeHarbour(
            temp, secondary_ports,
            {
                str(primary_ports): {
                    "installed": ["alpha", "duplicate"],
                    "broken": ["broken-primary"],
                },
                str(secondary_ports): {
                    "installed": ["beta", "duplicate"],
                },
            })
        overlay.configure(harbour)
        overlay.load_ports(harbour)

        assert set(harbour.installed_ports) == {
            "alpha", "beta", "duplicate"}
        assert harbour.installed_ports["alpha"]["leaf_source_id"] == "primary"
        assert harbour.installed_ports["beta"]["leaf_source_id"] == "secondary_sd"
        assert harbour.installed_ports["duplicate"]["leaf_conflict"] is True
        assert set(harbour.leaf_duplicate_ports["duplicate"]) == {
            "primary", "secondary_sd"}
        inventory = json.loads((temp / "inventory.json").read_text())
        assert inventory["sources"]["primary"]["ports"] == [
            "alpha", "broken-primary", "duplicate"]
        assert inventory["sources"]["secondary_sd"]["ports"] == [
            "beta", "duplicate"]

        assert overlay.prepare_install(harbour, "alpha.zip") is True
        assert harbour.ports_dir == primary_ports
        assert overlay.prepare_install(harbour, "new-port.zip") is True
        assert harbour.ports_dir == secondary_ports
        assert overlay.prepare_install(harbour, "duplicate.zip") is False
        assert overlay.prepare_uninstall(
            harbour, "duplicate", "secondary_sd") is True
        assert harbour.ports_dir == secondary_ports

        harbour.leaf_last_known["sources"]["removed_card"] = {
            "ports": ["remembered"]}
        assert overlay.prepare_install(harbour, "remembered.zip") is False

        mountinfo.write_text(mount_line(41, "179:1", primary))
        assert overlay.prepare_install(harbour, "new-after-removal.zip") is False
        assert not (secondary_ports / "new-after-removal").exists()

    print("multi-source fixtures: PASS")


if __name__ == "__main__":
    main()
