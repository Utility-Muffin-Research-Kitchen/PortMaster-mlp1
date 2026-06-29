# Spruce PortMaster Binary Inventory

Generated inventory:

```text
docs/generated/spruce-portmaster-binary-inventory.tsv
```

Regenerate with:

```sh
scripts/inventory-spruce-portmaster.sh
```

Source artifacts inspected:

```text
/Volumes/Storage/GitHub/spruceOS/App/PortMaster/portmaster.7z
sha256 036257ee7891bac07aa70017d0646f10fed5e9f27deed97913fe99ccbe481d98

/Volumes/Storage/GitHub/spruceOS/App/PortMaster/pillow_offline/pillow-11.2.1-cp310-cp310-manylinux_2_28_aarch64.whl
sha256 562d11134c97a62fe3af29581f083033179f7ff435f78392565a1ad2d1c2c45c
```

## Docker and Porty McPortface Reference

Freely available Docker images are acceptable as controlled build or extraction
environments, but the image itself is not enough provenance for a shipped Leaf
binary. Any file copied from an image must still have a recorded source package
or upstream project, license, version, URL when available, image digest, file
size, and SHA-256.

Preferred source order:

1. Build from source in the MLP1 toolchain container.
2. Use a pinned upstream release artifact or PyPI wheel with URL, version,
   license, size, and SHA-256.
3. Extract from a pinned Docker image only when the underlying distro package or
   upstream artifact is identifiable and license-compatible.
4. Treat opaque vendored blobs as last-resort reference material, not a default
   Leaf supply chain.

`/Volumes/Storage/GitHub/nextui-PortyMcPortface-my355-pak` uses a pragmatic
mixed model:

- It builds the app, helper tools, and `box64` inside
  `ghcr.io/loveretro/my355-toolchain:latest`.
- It downloads `7zzs` from the official 7-Zip arm64 release because the p7zip
  source build is not usable in that toolchain path.
- It downloads Box64 x86 runtime library bundles from `ptitSeb/box64` releases
  and extracts core glibc files from an AlmaLinux RPM inside that bundle flow.
- It copies vendored `third_party` binaries for `rsync`, SDL2, and Pulse, plus
  payload archives such as `bin.tar.gz` and `lib.tar.gz`.
- Its package-layout test asserts that the resulting `.pakz` contains expected
  binaries and rejects unwanted symlink/hardlink entries in the library tar.

Leaf should borrow that shape, not those exact binaries: source-build what we
own, download exact upstream release artifacts where source-building is not yet
practical, and improve the process with lock manifests and hashes for every
copied binary/release asset.

## Counts

The inventory currently has 137 payload rows, excluding the header:

```text
81 ui-python-runtime
33 upstream-portmaster-payload
23 pillow-offline-wheel
```

By file kind:

```text
85 elf-shared
21 elf-executable
24 script
6 placeholder
1 elf-object
```

## Production Buckets

### UI Python Runtime

These are the Spruce-provided files outside `PortMaster/` that make upstream
`pugwash`/`harbourmaster` run on stock MLP1, which lacks system `python3`.

Production approach:

- Build or fetch a provenance-clean CPython runtime for aarch64 Linux.
- Prefer CPython source builds using the MLP1 toolchain once reproducible.
- Short-term acceptable path: exact-version binary artifact with upstream
  source, license, URL, size, and SHA-256 pinned in `locks/runtimes.lock.json`.
- Install Python packages into one canonical site-packages path; avoid Spruce's
  duplicate SDL library copies unless smoke testing proves they are required.

Important rows from the generated inventory:

```text
bin/python3
bin/python3.10
lib/libpython3.10.so.1.0
lib/libpython3.so
lib/python3.10/lib-dynload/_crypt.cpython-310-aarch64-linux-gnu.so
lib/python3.10/lib-dynload/_testclinic.cpython-310-aarch64-linux-gnu.so
```

The generated inventory also captures script entrypoints and placeholder files
from `bin/`, such as `pip3.10`, `python`, and `python3-config`. For Leaf, those
should be treated as packaging conveniences, not required runtime ABI.

### PySDL2 DLL / SDL Stack

Spruce carries the same PySDL2 DLL native libraries in multiple locations:

```text
lib/*.so
lib/sdl2dll/dll/*.so
site-packages/sdl2dll/dll/*.so
```

The repeated native library set is:

```text
libSDL2-2.0.so
libSDL2_gfx-1.0.so
libSDL2_image-2.0.so
libSDL2_mixer-2.0.so
libSDL2_ttf-2.0.so
libavif.so.16
libdav1d.so.6
libgme.so.0
libogg.so.0
libopus.so.0
libopusfile.so.0
libtiff.so.5
libwavpack.so.1
libwebp.so.1.0.3
libwebpdemux.so.2.6.0
libxmp.so.4
```

Production approach:

- First try sourcing `pysdl2-dll==2.32.0` from PyPI for
  `manylinux_2_28_aarch64`, pinned by URL and SHA-256.
- Install it into the managed Python runtime and set `PYSDL2_DLL_PATH` to its
  real `sdl2dll/dll` directory.
- If PyPI wheels become insufficient, build SDL2 and companion libs in the MLP1
  toolchain and publish them as part of the UI runtime artifact.

Current locked PyPI source:

```text
pysdl2_dll-2.32.0-py2.py3-none-manylinux_2_28_aarch64.whl
sha256 c9a5c97b0647f9ddf9ad52f5f95b90d1dbf362ab52fcad0ff091e3f32663b257
```

Fetch/verify with:

```sh
make fetch-ui-runtime-sources
```

### Pillow Offline Wheel

Spruce also provides an offline Pillow wheel with 23 ELF files. This is separate
from the UI runtime and is only needed for Spruce's image conversion path, not
for basic PortMaster launch.

Production approach:

- Do not bundle Pillow in the first required UI runtime unless a Leaf smoke test
  proves upstream PortMaster needs it.
- If needed, source `Pillow==11.2.1` from PyPI with exact URL/SHA-256 first.
- Rebuild with `cibuildwheel` or a repo-owned cross build later if we need
  fully self-produced binaries.

Current optional PyPI source:

```text
pillow-11.2.1-cp310-cp310-manylinux_2_28_aarch64.whl
sha256 562d11134c97a62fe3af29581f083033179f7ff435f78392565a1ad2d1c2c45c
```

This hash matches Spruce's offline Pillow wheel exactly. Fetch/verify with:

```sh
INCLUDE_OPTIONAL=1 make fetch-ui-runtime-sources
```

The compiled Pillow extension rows are:

```text
PIL/_imaging.cpython-310-aarch64-linux-gnu.so
PIL/_imagingcms.cpython-310-aarch64-linux-gnu.so
PIL/_imagingft.cpython-310-aarch64-linux-gnu.so
PIL/_imagingmath.cpython-310-aarch64-linux-gnu.so
PIL/_imagingmorph.cpython-310-aarch64-linux-gnu.so
PIL/_imagingtk.cpython-310-aarch64-linux-gnu.so
PIL/_webp.cpython-310-aarch64-linux-gnu.so
```

The bundled Pillow native dependency rows are listed in the generated TSV under
`pillow_offline/wheel/pillow.libs/`.

### Upstream PortMaster Payload

The `PortMaster/` subtree in Spruce contains binaries that are also provided by
the upstream `PortsMaster/PortMaster-GUI` release we already download and patch.
Leaf should not reproduce these in the UI runtime artifact.

Production approach:

- Continue installing these from the hash-pinned upstream `PortMaster.zip`.
- Keep Leaf patches separate and recorded in `.leaf/manifest.json`.
- Do not add x86_64 helpers to a Leaf-produced runtime.
- Armhf helpers stay part of upstream PortMaster until the separate armhf
  compatibility pack can actually run dynamic armhf ports.

Representative upstream rows:

```text
PortMaster/gptokeyb
PortMaster/gptokeyb.armhf
PortMaster/gptokeyb2
PortMaster/gptokeyb2.armhf
PortMaster/libinterpose.aarch64.so
PortMaster/libinterpose.armhf.so
PortMaster/oga_controls
PortMaster/sdl2imgshow.aarch64
PortMaster/sdl_resolution.aarch64
PortMaster/xdelta3
PortMaster/xdelta3.armhf
PortMaster/runtimes/love_11.5/love.aarch64
```

## Initial Runtime Artifact Shape

The first Leaf UI runtime release asset should target only the launch blocker:

```text
portmaster-mlp1-ui-runtime-python310-aarch64-<version>.tar.zst
```

Suggested contents:

```text
runtime/
  bin/python3
  bin/python3.10
  lib/libpython3.10.so.1.0
  lib/python3.10/
  lib/python3.10/site-packages/
```

Then add exactly one SDL DLL location, either:

```text
runtime/lib/python3.10/site-packages/sdl2dll/dll/
```

or:

```text
runtime/lib/sdl2dll/dll/
```

The manager launch code should match whichever path the produced artifact uses.

## Next Build Questions

- Can the MLP1 Buildroot toolchain build CPython 3.10/3.11 with usable `_ssl`,
  `_sqlite3`, `_ctypes`, `zlib`, `hashlib`, and `readline` support?
- Can we use PyPI `pysdl2-dll==2.32.0` as a pinned binary dependency, or do we
  want to build SDL2 and companion libs ourselves from source immediately?
- Does upstream PortMaster need Pillow on Leaf, or is Spruce's Pillow wheel only
  for Spruce image-cache maintenance?
- Can we trim development-only entries such as `idle`, `2to3`, `pip`, `tk`
  demos, and `python.o` from the required runtime without breaking PortMaster?
