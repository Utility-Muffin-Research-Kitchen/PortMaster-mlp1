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

The current patch set is recorded in `.leaf/manifest.json` after install,
managed update, or repair. Launch also runs repair before handoff and again
after the upstream GUI exits. If upstream ships a fresh `pylibs.zip`, the
manager extracts it, removes the zip/md5 marker, and reapplies the Leaf
`hardware.py` patch before launch so changed upstream trees do not silently undo
MLP1 compatibility.

During normal managed launch, Leaf sets:

```text
LEAF_PM_DISABLE_SELF_UPDATE=1
```

The Leaf patch set teaches `pugwash` to skip only its PortMaster GUI self-update
check when that variable is truthy. The manager deliberately does not pass
`--no-check`, because that would also suppress HarbourMaster source/catalog
refreshes inside the GUI.

Stable GUI update checks are manager-owned:

```sh
./launch.sh --check-portmaster-update
./launch.sh --update-portmaster
```

The update path fetches upstream stable `version.json`, downloads the candidate
`PortMaster.zip`, verifies the upstream MD5, records a SHA-256 in the Leaf
manifest, applies the Leaf patch set in staging, validates the patched tree,
and promotes it only after validation passes.
The existing install is backed up before promotion; if a post-promote step
fails, the manager restores that backup or removes the incomplete initial
install before returning the failure.
The UI also runs a manager-owned cached/due check before `Launch PortMaster`.
Successful checks are cached for 24 hours in:

```text
$USERDATA_PATH/portmaster/.leaf/gui-update-state.json
```

If the user chooses Later, that exact upstream version is not prompted again
until a newer version appears or the state file is removed. Failed update
versions are also recorded there so a broken candidate does not block every
launch with the same prompt. Failed-version suppression is tied to the manager
version, patch-set ID, and patch-set fingerprint; changing the manager build or
Leaf patch set allows the same upstream version to be offered again.

Update attempts are appended to:

```text
$USERDATA_PATH/portmaster/.leaf/logs/update.log
```

The staged candidate validator checks that `PortMaster.sh`, `pugwash`,
`control.txt`, `device_info.txt`, and HarbourMaster `hardware.py` contain the
expected Leaf markers before the live tree is touched.

Use `LEAF_PM_FORCE_UPDATE_CHECK=1` to force a fresh metadata poll,
`LEAF_PM_SKIP_UPDATE_CHECK=1` to skip the manager-owned prelaunch check,
`LEAF_PM_UPDATE_VERSION_URL` to point at test metadata, and
`LEAF_PM_ALLOW_HTTP_UPDATE_METADATA=1` only for local HTTP metadata tests. Use
`LEAF_PM_ALLOW_UPSTREAM_SELF_UPDATE=1` only as a developer escape hatch for
testing the raw upstream prompt path.

Native failure fixtures are available with:

```sh
make update-failure-fixtures
```

Known deferred bypass: upstream `PortMaster.sh` can still process a manually
dropped `PortMaster.zip` from an autoinstall directory before `pugwash` starts.
This phase does not intercept that path. The post-exit repair pass is kept so
accidental tree changes are repaired when the current patch set still applies.

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
