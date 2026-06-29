# Upstream Sync

V1 tracks stable `PortsMaster/PortMaster-GUI` only.

Current lock:

```text
locks/portmaster-gui-stable.lock.json
```

Refresh flow:

```sh
scripts/fetch-portmaster.sh --metadata-only
scripts/fetch-portmaster.sh --download /tmp/PortMaster.zip
```

Patch refresh:

```sh
scripts/apply-portmaster-patches.sh --dry-run <extracted-portmaster-tree>
```

The current patch set is recorded in `.leaf/manifest.json` after install or
repair. Launch also runs repair before handoff. If upstream ships a fresh
`pylibs.zip`, the manager extracts it, removes the zip/md5 marker, and reapplies
the Leaf `hardware.py` patch before launch so user updates do not silently undo
MLP1 compatibility.

Runtime note: upstream PortMaster currently requires `python3`; stock MLP1 does
not provide it. `--install-ui-runtime` downloads the lock-pinned generated
runtime release asset and installs it into user data. `--install-runtime-archive
<archive>` remains available for smoke testing a locally staged
`portmaster.7z`-style archive. The runtime supplies CPython and liblzma; the UI
uses stock MLP1 SDL from `/usr/lib` by default because the bundled `pysdl2-dll`
SDL libraries segfaulted during device smoke testing.

Spruce runtime inventory:

```sh
scripts/inventory-spruce-portmaster.sh
make spruce-bin-closure
```

The generated inventory and closure report live at:

```text
docs/generated/spruce-portmaster-binary-inventory.tsv
docs/generated/spruce-portmaster-bin-closure.tsv
```

The closure generator fails on any unresolved Spruce row. A closed row either
maps to the locked upstream PortMaster payload, the generated CPython runtime,
the locked optional Pillow wheel, or an intentional exclusion for dev/demo
artifacts.

The UI runtime lock stub is separate from PortMaster game runtimes:

```text
locks/ui-runtime.lock.json
```
