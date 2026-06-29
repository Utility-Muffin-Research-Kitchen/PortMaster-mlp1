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
repair. Launch also runs repair before handoff.

Runtime note: upstream PortMaster currently requires `python3`; stock MLP1 does
not provide it. `--install-ui-runtime` downloads the lock-pinned generated
runtime release asset and installs it into user data. `--install-runtime-archive
<archive>` remains available for smoke testing a locally staged
`portmaster.7z`-style archive.

Spruce runtime inventory:

```sh
scripts/inventory-spruce-portmaster.sh
```

The generated list lives at:

```text
docs/generated/spruce-portmaster-binary-inventory.tsv
```

The UI runtime lock stub is separate from PortMaster game runtimes:

```text
locks/ui-runtime.lock.json
```
