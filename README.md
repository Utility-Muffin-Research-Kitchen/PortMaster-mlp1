# PortMaster-mlp1

Leaf first-party optional app for managing PortMaster on the Miniloong Pocket 1.

This repo builds a small manager `.pak`. Users should install it through Pak Rat;
it is not intended to be part of the base Leaf SD release.

Current scope:

- MLP1 only.
- Stable upstream PortMaster only.
- Small online manager package.
- Upstream PortMaster, runtime bundles, armhf compatibility packs, and native
  helper tools are downloaded, generated, or published as release assets, not
  committed to git.

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
make update-failure-fixtures
```

`make local-pakrat-feed` writes a local-only catalog and artifacts under:

```text
build/local-pakrat-feed
```

`make pakrat-local-smoke` serves that feed on a temporary localhost port, asks
Jawaka's Pak Rat helper to install `org.umrk.portmaster` into a temp SD root,
and verifies the installed pak. When the generated UI runtime artifact exists,
the staged test package's runtime lock is rewritten to the local feed too.
`make update-failure-fixtures` exercises the manager-owned GUI update failure
paths against a temporary SD/userdata root.

PortMaster is Pak Rat-owned. Do not add it to Leaf's direct `stage-app`
release-managed app policy; device installs and smoke tests should go through a
Pak Rat catalog.

## Manager UI

The on-device manager is state-driven:

- `Launch PortMaster` is always the first row.
- If PortMaster is not launchable, the launch row stays visible but disabled
  with a short reason such as `Not installed` or `Setup needed`.
- `Install PortMaster` is shown only before the managed upstream GUI exists.
  That action performs the full user setup: upstream PortMaster install,
  managed UI runtime install when needed, Leaf patches, compatibility assets,
  artwork sync, and Jawaka rescan.
- `Repair PortMaster` is shown once PortMaster is installed. It is the recovery
  path for missing runtime, stale/missing manifest, missing Leaf patches, or
  compatibility asset drift.
- `Update PortMaster` appears only when the manager has found a newer compatible
  stable GUI release.
- `Controller Layout` remains a normal user setting.
- `Troubleshooting` contains the detailed health check, manual update check,
  logs, and paths.

Game runtimes and armhf compatibility are not main-menu choices. Upstream
PortMaster manages game runtimes from inside its GUI; Leaf's armhf, SDL,
EGL/GLES, helper-tool, and wrapper compatibility is refreshed automatically
during install, repair, launch, and post-exit repair.

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
$USERDATA_PATH/portmaster/compat
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

The same hook prepends `$USERDATA_PATH/portmaster/compat/tools/aarch64/bin` to
`PATH`. The MLP1 pak source-builds and bundles pinned aarch64 helpers there so
port patch scripts can use common tools without writing to the stock rootfs.

Stock MLP1 mounts the supported SD card as FAT32/vfat for this PortMaster
path. Single files at or above 4 GiB are not supported on that filesystem. The
manager preflights known-size GUI/runtime downloads, classifies FAT32-size and
free-space write failures, and appends those details under
`$USERDATA_PATH/portmaster/.leaf/logs/`; it does not install filesystem support
or persistent helpers into the stock OS/eMMC. The scanner also records
installed port files larger than 3.5 GiB for the doctor report.

On launch it sources:

```text
$SDCARD_PATH/.system/leaf/platforms/$PLATFORM/launcher/env.sh
```

when present. `SDCARD_PATH` may be supplied by Leaf/Jawaka, inferred from the
pak's own `Apps/<platform>/PortMaster.pak` location, or resolved from a single
Leaf-marked SD mount. If the SD root cannot be resolved unambiguously, launch
fails instead of guessing a mount path.

Useful smoke commands from a staged pak:

```sh
./launch.sh --ui-state-text
./launch.sh --doctor-text
./launch.sh --doctor-cfw-text
LEAF_PM_DOCTOR_MOUNT_TEST=1 ./launch.sh --doctor-cfw-text
LEAF_PM_DOCTOR_LOOP_STRESS=1 ./launch.sh --doctor-cfw-text
./launch.sh --install-portmaster
./launch.sh --repatch-portmaster
./launch.sh --check-portmaster-update
./launch.sh --check-portmaster-update-cached
./launch.sh --update-portmaster
./launch.sh --install-ui-runtime
./launch.sh --install-runtime-archive /path/to/portmaster-runtime.7z
./launch.sh --launch-portmaster
```

`--ui-state-text` prints the same top-level row model used by the GUI. It is
useful for ADB checks that need to prove menu state without launching SDL.

The host-side compatibility matrix writes report artifacts to SD userdata only:

```sh
make smoke-matrix
LEAF_PM_SMOKE_LOOP_STRESS=1 make smoke-matrix
LEAF_PM_SMOKE_PORTS=2048,SDLPoP make smoke-matrix
LEAF_PM_SMOKE_INTERACTIVE=1 LEAF_PM_SMOKE_PORTS=Duck-Dodge make smoke-matrix
```

Reports are published on the device at:

```text
$USERDATA_PATH/portmaster/.leaf/smoke/latest.tsv
$USERDATA_PATH/portmaster/.leaf/smoke/latest.json
$USERDATA_PATH/portmaster/.leaf/smoke/logs/
```

The matrix is passive by default: it runs the CFW doctor, checks runtime/tool
fixtures, validates representative installed port scripts and required files,
and records existing port log tails. It does not launch games automatically.
If `PortMaster.pak` is not installed, the harness exits as skipped before
loading or writing any PortMaster-specific device path.

## PortMaster GUI Updates

Leaf disables upstream PortMaster GUI self-update prompts during managed
launches with `LEAF_PM_DISABLE_SELF_UPDATE=1`. This does not use upstream
`--no-check`, so normal HarbourMaster source/catalog refreshes still run inside
the GUI.

When the wrapper starts and PortMaster setup is ready, the manager performs a
cached/due stable metadata check before showing the menu. Fresh cached results
are used immediately; due network checks use a short startup timeout so a slow
or captive network does not block the wrapper. If a newer compatible stable GUI
release is available, the manager alerts the user. Choosing `Later` suppresses
the modal for that same upstream version, while the `Update PortMaster` row
remains available as an explicit action.

The troubleshooting screen also exposes a manual `Check For Updates` action,
and CLI users can run `--check-portmaster-update`. Explicit checks bypass the
declined-version prompt suppression because the user asked for them. If an
update is accepted, the manager downloads `PortMaster.zip`, verifies the
upstream MD5, records a SHA-256 in the Leaf manifest, applies the Leaf patch set
in staging, backs up the current install, and promotes the patched tree. If
promotion succeeds but a post-promote step fails, the manager restores the
previous install before returning an error. Set
`LEAF_PM_ALLOW_UPSTREAM_SELF_UPDATE=1` only for developer debugging of the raw
upstream path.

Before promotion, the staged tree is structurally validated for the expected
Leaf markers in `PortMaster.sh`, `pugwash`, `control.txt`, `device_info.txt`,
and HarbourMaster `hardware.py`.

Manager-owned update state lives at:

```text
$USERDATA_PATH/portmaster/.leaf/gui-update-state.json
```

Update attempts are appended to:

```text
$USERDATA_PATH/portmaster/.leaf/logs/update.log
```

Successful checks are cached for 24 hours. Use
`LEAF_PM_FORCE_UPDATE_CHECK=1` to force a fresh metadata poll, or
`LEAF_PM_SKIP_UPDATE_CHECK=1` to skip the manager-owned startup check.
Failed candidates are suppressed for the same upstream version only while the
manager version and Leaf patch-set fingerprint are unchanged.

## UI Runtime Work

The PortMaster UI runtime is separate from PortMaster game runtimes and is
installed automatically by the manager's install/repair flows when needed. The
repo can fetch locked PyPI inputs used while building or experimenting with the
runtime:

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

Armhf support is automatic in the manager UI. Users do not install it from a
separate menu row; install/repair/launch flows refresh the compatibility files
and wrappers as needed.

The armhf compatibility pack is generated from Debian armhf packages in Docker,
plus a pinned Rockchip Mali 32-bit userspace blob for GLES ports. Package/file
provenance and Mali license metadata are written to the generated manifest:

```sh
make build-armhf-compat
```

Output:

```text
build/armhf-compat/portmaster-mlp1-armhf-compat-bookworm-mali-g13p0-box86-20260630.zip
build/armhf-compat/portmaster-mlp1-armhf-compat-bookworm-mali-g13p0-box86-20260630.json
```

The zip installs under:

```text
$USERDATA_PATH/portmaster/compat/armhf
```

It includes `bin/leaf-armhf-run`, which runs dynamic armhf programs through the
packaged loader and library path without writing to the stock rootfs. It also
includes a managed current `bin/box86`; wrapped port-bundled `box86` binaries
prefer this managed copy when present.

The generated Leaf hook and armhf executable wrappers default SDL's GLES loader
to `SDL_VIDEO_GL_DRIVER=libGLESv2.so`. This lets gmloader ports locate the
Rockchip Mali GLES library on MLP1 without per-port launch-script edits.

The Mali stack is the armhf `libmali-bifrost-g52-g13p0-wayland-gbm` deb from
the `home:amazingfate:libmali-rockchip` OBS repository, pinned by package
URL, size, and SHA-256 in `scripts/build-armhf-compat-pack.sh`. The generated
pack includes the upstream Debian copyright file under `licenses/mali/`; that
file contains the Arm Mali userspace driver EULA and redistribution notice
requirements.

On PortMaster launch, and again after the upstream GUI exits, the manager runs:

```text
scripts/scan-and-fix-port-elfs.sh
```

That scanner calls `scripts/write-leaf-runtime-hook.sh` to refresh
`$USERDATA_PATH/portmaster/PortMaster/leaf-armhf-env.sh`, ensures upstream
`control.txt` sources it, and records installed armhf port ELFs in:

```text
$USERDATA_PATH/portmaster/.leaf/armhf-scan.json
$USERDATA_PATH/portmaster/.leaf/armhf-scan.tsv
$USERDATA_PATH/portmaster/.leaf/armhf-scan.manifest
```

Normal launch uses a fast scan: installed `.sh` launchers plus likely binary
and library locations are checked, while large asset trees are skipped. The
scanner keeps an incremental manifest keyed by its internal `RULESET_VERSION`,
scan mode, compatibility-pack availability, SDL shim availability, and
`Roms/PORTS` path; unchanged files are skipped on warm runs. Manifest v2 also
records generic SDL2 fullscreen tags, so replacing only a port binary can still
invalidate the related script decision. Set
`LEAF_PM_SCAN_NO_CACHE=1` to ignore the old manifest and write a fresh one, or
set `LEAF_PM_FULL_PORT_SCAN=1` when running
`scripts/scan-and-fix-port-elfs.sh` manually to force the older exhaustive walk
of every file under `Roms/PORTS` without reading or writing the manifest. The
wrapper also stores a cheap top-level `Roms/PORTS` stamp so repeated PortMaster
open/close cycles skip port repair, artwork sync, and Jawaka rescan when no
ports changed.

## Native Compatibility Tools

The MLP1 package builds a small aarch64 native tool payload with the MLP1
Buildroot toolchain:

```sh
make build-aarch64-tools
```

Output:

```text
build/mlp1/compat/tools/aarch64/bin/rsync
build/mlp1/compat/tools/aarch64/bin/zip
build/mlp1/compat/tools/aarch64/manifest.json
```

The payload currently includes:

- `rsync` 3.2.7, built from locked upstream source with bundled zlib and popt,
  ACL/xattr/iconv/compression extras disabled, and only the device libc as a
  runtime dependency.
- `zip` 3.0, built from locked Info-ZIP source with only the device libc as a
  runtime dependency.

Install and repair copy these tools to:

```text
$USERDATA_PATH/portmaster/compat/tools/aarch64/bin
```

The pak carries tool licenses under `LICENSES/rsync/` and `LICENSES/zip/`.

Dynamic armhf executables that require `/lib/ld-linux-armhf.so.3` are moved to
`.leaf-armhf/` beside the original file and replaced with a shell wrapper that
executes them through `bin/leaf-armhf-run`. Armhf shared objects, such as
libretro cores, are reported but not rewritten.

Installed `.sh` launchers are also patched with a small SD environment block so
upstream PortMaster scripts resolve `XDG_DATA_HOME`, `PORTMASTER_CONTROLFOLDER`,
and common quoted or unquoted `GAMEDIR=/$directory/ports/...` paths to the
active SD-managed PortMaster tree instead of `/roms/ports` on the stock rootfs.

Libretro-style PortMaster launchers, such as 2048, are normalized in the same
pass. Upstream scripts usually hardcode firmware-specific RetroArch locations
like `/usr/bin/retroarch`; the scanner rewrites those `retroarch -L ...` launch
lines to `leaf_pm_run_retroarch -L ...`, which uses Leaf's SD-managed
`$LEAF_PM_RETROARCH_BIN` and config path.

The same scan patches installed aarch64 Godot 4.3 shell launchers with a
Leaf-only Wayland block. MLP1 already has Leaf's Weston compositor running, so
patched Godot scripts bypass PortMaster's nested Westonpack launch path and run
directly against the active Wayland display. When present, the EGL/GLES
compatibility shim is prepended only for that direct Godot command so armhf and
non-Godot ports do not inherit the aarch64 graphics shim. The scanner also
guards Westonpack cleanup calls for those Godot launchers, because cleaning up a
nested compositor that was never started can disturb Leaf's real Wayland
runtime.

Godot 4.2.2 PortMaster launchers are upgraded to the upstream `godot_4.3`
runtime before that direct Wayland path runs, because the 4.2.2 aarch64 runtime
only advertises X11/headless display drivers. Set
`LEAF_PM_GODOT_422_WAYLAND_UPGRADE=0` to keep the original runtime for a
specific diagnostic launch.

The scanner also handles direct Godot/SDL2 launchers such as Songo #5. Those
ports do not call Westonpack, so the scanner wraps their direct
`"$GAMEDIR/runtime/$runtime" --main-pack ...` command with the same SD-managed
Mali/EGL/Wayland environment. If the port did not already specify a window mode
or resolution, the helper adds `--resolution 960x720`; it deliberately avoids
`-f`, which uses the MLP1's native portrait KMS framebuffer and mangles
landscape content.

Generic SDL2 fullscreen is handled separately from Godot. The scanner tags
installed aarch64 and armhf port ELFs that link `libSDL2-2.0.so`, then injects a
script-wide `LEAF_PM_SDL2_FULLSCREEN_ENV=1` block into the owning launcher. The
generated hook preloads `compat/sdl2/aarch64/leaf-sdl2-fullscreen.so` for native
aarch64 games and passes the armhf shim through `LEAF_PM_ARMHF_PRELOAD` for
`leaf-armhf-run`. This covers ports such as SDLPoP whose final game command is
inside a pipeline rather than a simple regex-friendly launch line. Opt out with
`LEAF_PM_SDL_FORCE_FULLSCREEN=0`, with
`$USERDATA_PATH/portmaster/sdl2-fullscreen-optout.txt`, or with the repo-side
opt-out list for known special cases such as Ship of Harkinian.

The scanner also owns narrow runtime compatibility rules for installed launch
scripts. The current non-Godot rule targets Gothic/Machismo launchers on Leaf
and defaults `GOTHIC_BACKEND=gles`. Leaf stock Vulkan currently exposes only
the direct-display path for that runtime, which sees the MLP1 panel as
`720x960` and can rotate games such as Mina the Hollower. The GLES path uses
the active Wayland surface and keeps the game at `960x720`. The generated block
does not depend on `CFW_NAME`, because direct Jawaka port launches and
source-built local installs may not inherit the PortMaster GUI environment. It
also forwards `GOTHIC_BACKEND` through the final `env` launcher so `sudo`-based
setups do not scrub it.

Love 11.5 launchers receive a generic runtime-library normalization block. If a
port runs a bundled `love` binary but ships only part of the Love library set,
the scanner appends upstream PortMaster's SD-installed
`runtimes/love_11.5/libs.aarch64` directory to `LD_LIBRARY_PATH`, after any
port-local library path. This lets ports keep their own bundled libraries while
falling back to the hash-pinned upstream Love runtime for libraries such as
`libmodplug.so.1`.

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
