# PortMaster-mlp1

Leaf first-party optional app for managing PortMaster on the Miniloong Pocket 1.

This repo builds a small manager `.pak`. Users should install it through Pak Rat;
it is not intended to be part of the base Leaf SD release.

Current scope:

- MLP1 only.
- Stable upstream PortMaster only.
- Small online manager package.
- Upstream PortMaster, runtime bundles, and armhf compatibility packs are
  downloaded or published as generated release assets, not committed to git.

## Build

```sh
make native
make package-platform PLATFORM=mlp1
make dist-pakrat
```

The MLP1 package is assembled at:

```text
build/mlp1/package/PortMaster.pak
```

The Pak Rat-ready archive is assembled at:

```text
build/mlp1/PortMaster.mlp1.pak.zip
```

## Local Pak Rat Test

PortMaster should be exercised through a local Pak Rat feed before it is added
to the production Leaf store catalog:

```sh
make local-pakrat-feed
make pakrat-local-smoke
```

`make local-pakrat-feed` writes a local-only catalog and artifacts under:

```text
build/local-pakrat-feed
```

`make pakrat-local-smoke` serves that feed on a temporary localhost port, asks
Jawaka's Pak Rat helper to install `org.umrk.portmaster` into a temp SD root,
and verifies the installed pak. When the generated UI runtime artifact exists,
the staged test package's runtime lock is rewritten to the local feed too.

## Runtime Layout

The manager uses Leaf's runtime env contract:

```text
$USERDATA_PATH/portmaster
$ROMS_PATH/PORTS
$IMAGES_PATH/PORTS
```

Managed PortMaster state lives under:

```text
$USERDATA_PATH/portmaster/PortMaster
$USERDATA_PATH/portmaster/runtime
$USERDATA_PATH/portmaster/.leaf
```

The manager also stores the global controller-layout preference there. X360 is
the default. Creating `$USERDATA_PATH/portmaster/nintendo` selects the Nintendo
face-button layout; deleting that file returns to X360. The app's Controller
Layout screen manages the same sentinel and applies the selected SDL mapping to
installed port launch scripts. The PortMaster GUI itself uses a fixed GUI-only
mapping with A/B swapped for the MLP1 face-button labels; the global layout
setting does not affect the GUI.

The `runtime` directory is for the Python runtime needed by upstream
PortMaster's Python UI on stock MLP1 firmware. It is installed from the
lock-pinned generated release asset or from an externally staged archive; the
repo does not vendor that binary payload. The UI currently uses stock MLP1 SDL
libraries from `/usr/lib` because the bundled `pysdl2-dll` SDL stack segfaulted
during device smoke testing.

Installed port scripts also source the Leaf PortMaster hook from upstream
`control.txt`. When the managed Python runtime exists, that hook creates a
temporary `/tmp/leaf-portmaster-python/python3` shim so PortMaster helper calls
such as `harbourmaster runtime_check` can download and verify game runtimes
without exporting Python runtime variables into every launched game.

On launch it sources:

```text
$SDCARD_PATH/.system/leaf/platforms/$PLATFORM/launcher/env.sh
```

when present.

Useful smoke commands from a staged pak:

```sh
./launch.sh --doctor-text
./launch.sh --install-portmaster
./launch.sh --repatch-portmaster
./launch.sh --install-ui-runtime
./launch.sh --install-runtime-archive /path/to/portmaster-runtime.7z
./launch.sh --launch-portmaster
```

## UI Runtime Work

The PortMaster UI runtime is separate from PortMaster game runtimes. The repo can
fetch locked PyPI inputs used while building or experimenting with the runtime:

```sh
make fetch-ui-runtime-sources
INCLUDE_OPTIONAL=1 make fetch-ui-runtime-sources
```

The production CPython runtime is built from locked source inputs:

```sh
make build-ui-runtime-cpython
```

Output:

```text
build/ui-runtime/cpython/portmaster-mlp1-ui-runtime-python310-aarch64-cpython-3.10.16.zip
build/ui-runtime/cpython/portmaster-mlp1-ui-runtime-python310-aarch64-cpython-3.10.16.json
```

For comparison smoke testing, a reference runtime zip can be built from the
known working Spruce CPython runtime plus locked PyPI wheels:

```sh
make build-ui-runtime-reference
```

Output:

```text
build/ui-runtime/reference/portmaster-mlp1-ui-runtime-python310-aarch64-reference.zip
build/ui-runtime/reference/portmaster-mlp1-ui-runtime-python310-aarch64-reference.json
```

That reference artifact is not a production Leaf supply-chain artifact. It is a
bridge for device testing while the CPython source/toolchain build is brought
up.

The intended production runtime is built from the locked CPython source tarball
inside the MLP1 toolchain container, then overlaid with the locked PyPI SDL
wheel:

```sh
make build-ui-runtime-cpython
```

Output:

```text
build/ui-runtime/cpython/portmaster-mlp1-ui-runtime-python310-aarch64-cpython-3.10.16.zip
build/ui-runtime/cpython/portmaster-mlp1-ui-runtime-python310-aarch64-cpython-3.10.16.json
```

## Armhf Compatibility Pack

The armhf compatibility pack is generated from Debian armhf packages in Docker,
plus a pinned Rockchip Mali 32-bit userspace blob for GLES ports. Package/file
provenance and Mali license metadata are written to the generated manifest:

```sh
make build-armhf-compat
```

Output:

```text
build/armhf-compat/portmaster-mlp1-armhf-compat-bookworm-mali-g24p0-20260630.zip
build/armhf-compat/portmaster-mlp1-armhf-compat-bookworm-mali-g24p0-20260630.json
```

The zip installs under:

```text
$USERDATA_PATH/portmaster/compat/armhf
```

It includes `bin/leaf-armhf-run`, which runs dynamic armhf programs through the
packaged loader and library path without writing to the stock rootfs.

The Mali blob is `libmali-bifrost-g52-g24p0-wayland-gbm.so` from
`tsukumijima/libmali-rockchip`, pinned by commit and SHA-256 in
`scripts/build-armhf-compat-pack.sh`. The generated pack includes the upstream
Debian copyright file under `licenses/mali/`; that file contains the Arm Mali
userspace driver EULA and redistribution notice requirements.

On PortMaster launch, and again after the upstream GUI exits, the manager runs:

```text
scripts/scan-and-fix-port-elfs.sh
```

That scanner writes `$USERDATA_PATH/portmaster/PortMaster/leaf-armhf-env.sh`,
ensures upstream `control.txt` sources it, and records installed armhf port
ELFs in:

```text
$USERDATA_PATH/portmaster/.leaf/armhf-scan.json
$USERDATA_PATH/portmaster/.leaf/armhf-scan.tsv
```

Dynamic armhf executables that require `/lib/ld-linux-armhf.so.3` are moved to
`.leaf-armhf/` beside the original file and replaced with a shell wrapper that
executes them through `bin/leaf-armhf-run`. Armhf shared objects, such as
libretro cores, are reported but not rewritten.

Installed `.sh` launchers are also patched with a small SD environment block so
upstream PortMaster scripts resolve `XDG_DATA_HOME`, `PORTMASTER_CONTROLFOLDER`,
and common `GAMEDIR=/$directory/ports/...` paths to the active SD-managed
PortMaster tree instead of `/roms/ports` on the stock rootfs.

The same scan patches installed aarch64 Godot 4.3 shell launchers with a
Leaf-only Wayland block. MLP1 already has Leaf's Weston compositor running, so
patched Godot scripts bypass PortMaster's nested Westonpack launch path and run
directly against the active Wayland display. When present, the EGL/GLES
compatibility shim is prepended only for that direct Godot command so armhf and
non-Godot ports do not inherit the aarch64 graphics shim. The scanner also
guards Westonpack cleanup calls for those Godot launchers, because cleaning up a
nested compositor that was never started can disturb Leaf's real Wayland
runtime.

The scanner also patches Mina the Hollower's `machismo` launcher on Leaf to
default `GOTHIC_BACKEND=gles`. Leaf stock Vulkan currently exposes only the
direct-display path for this port, which sees the MLP1 panel as `720x960` and
rotates the game. The GLES path uses the active Wayland surface and keeps the
game at `960x720`.

MLP1 stock ships a 64-bit `g13p0` Mali userspace blob that can fault under some
Godot 4 content. `make package-mlp1` therefore builds a small aarch64 Mali
compat bundle from the pinned `tsukumijima/libmali-rockchip` `g24p0`
Wayland/GBM deb. Install/repair copies only `libmali.so.1` and
`libmali-hook.so.1` to SD user data, and the Godot hook puts that directory
ahead of stock only for direct Godot launches.

After the post-exit scan, the manager sends Jawaka a non-fatal
`scan-library` IPC request through `jawaka-platformctl` so newly installed
PortMaster `.sh` launchers appear in the Ports list without restarting the
launcher stack.

## Spruce Binary Closure

Spruce's PortMaster package is tracked as an inventory plus an explicit closure
report:

```sh
scripts/inventory-spruce-portmaster.sh
make spruce-bin-closure
```

The closure report lives at:

```text
docs/generated/spruce-portmaster-bin-closure.tsv
```

The generator fails if any Spruce-provided row is unresolved. Dynamic 32-bit
PortMaster game-port compatibility is tracked separately from this report.
