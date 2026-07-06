# Compatibility

MLP1 is an aarch64 RK3566 handheld. The stock kernel supports 32-bit ARM compat,
but the stock rootfs does not ship `/lib/ld-linux-armhf.so.3` or a 32-bit
userspace.

Compatibility tiers:

- Tier 0: static armhf helper binaries.
- Tier 1: dynamic armhf SDL/software-rendered ports using the Leaf armhf pack.
- Tier 2: dynamic armhf GLES ports using the bundled 32-bit Rockchip Mali stack.

Tier 2 is claimed only for the pinned Rockchip Mali g13p0 armhf stack after an
MLP1 GLES smoke test. More GLES ports still need per-port smoke testing.

The Spruce binary closure report covers the binaries Spruce ships with its
PortMaster app and runtime. It includes upstream static armhf helper binaries
from the locked `PortMaster.zip`, but it does not close the dynamic 32-bit
PortMaster game-port gap. That requires the separate Leaf armhf userspace pack
and per-port smoke testing.

## Native tool payload

Some PortMaster patch scripts depend on host-style helper tools that stock MLP1
firmware does not provide. The MLP1 pak currently source-builds and ships:

- `rsync` 3.2.7 for aarch64, installed at
  `$USERDATA_PATH/portmaster/compat/tools/aarch64/bin/rsync`
- `zip` 3.0 for aarch64, installed at
  `$USERDATA_PATH/portmaster/compat/tools/aarch64/bin/zip`
- `readelf` from the MLP1 Buildroot/binutils toolchain, installed at
  `$USERDATA_PATH/portmaster/compat/tools/aarch64/bin/readelf`

The generated PortMaster hook prepends that directory to `PATH` for port
launchers that source upstream `control.txt`. The binaries are built with the
MLP1 Buildroot toolchain and depend only on the stock aarch64 libc/loader.
Tool licenses are included in the pak under `LICENSES/rsync/` and
`LICENSES/zip/`.

## Initial armhf pack

`make build-armhf-compat` builds the compatibility pack from Debian Bookworm
armhf packages plus a pinned Rockchip Mali armhf userspace blob. The generated
zip is a release-asset candidate, not a git-vendored payload:

```text
build/armhf-compat/portmaster-mlp1-armhf-compat-bookworm-mali-g13p0-box86-20260630.zip
build/armhf-compat/portmaster-mlp1-armhf-compat-bookworm-mali-g13p0-box86-20260630.json
```

The pack installs under `$USERDATA_PATH/portmaster/compat/armhf` and includes:

- `lib/ld-linux-armhf.so.3`
- glibc, libgcc, libstdc++, SDL2, SDL2_image, SDL2_mixer, SDL2_ttf, SDL2_gfx
- common audio/image/font/Wayland/X11 support libraries needed by SDL helpers
- Rockchip Mali Bifrost G52 `g13p0-01eac0` 32-bit Wayland/GBM userspace from
  the `home:amazingfate:libmali-rockchip` armhf deb
- `licenses/mali/libmali-rockchip-debian-copyright`, which includes the Arm
  Mali userspace driver EULA and redistribution notice requirements
- `bin/box86`, built from pinned upstream `ptitSeb/box86` source under MIT
  license, used in preference to wrapped port-bundled `box86`
- `bin/leaf-armhf-run`, a non-root loader wrapper
- `bin/leaf-armhf-smoke`, a tiny dynamic armhf smoke binary
- `bin/leaf-sdl2-fullscreen.so`, an armhf SDL2 preload shim built from Leaf
  source for SDL2 ports that need MLP1 fullscreen desktop scaling

`bin/leaf-armhf-run` also exports the Leaf PulseAudio socket and OpenAL Soft
defaults used by gmloader-style ports: `PULSE_SERVER=unix:/tmp/pulse-socket`,
`PULSE_CLIENTCONFIG=$LEAF_PM_ARMHF_ROOT/etc/pulse/client.conf`,
`ALSOFT_DRIVERS=pulse`, and
`ALSOFT_CONF=$LEAF_PM_ARMHF_ROOT/etc/openal/alsoft.conf`. The generated Leaf
hook and armhf wrappers also default SDL's GLES loader to
`SDL_VIDEO_GL_DRIVER=libGLESv2.so`, which is required by gmloader ports such as
AM2R on MLP1.

The Mali deb, inner blob, hook library, and license text are pinned by URL,
size, and SHA-256 in the generated manifest. The blob is not built from source
and remains under Arm's closed Mali userspace driver EULA.

Device smoke on MLP1 on 2026-06-29 and 2026-06-30:

- `leaf-armhf-smoke` ran through `bin/leaf-armhf-run`.
- upstream `xdelta3.armhf -V` ran through `bin/leaf-armhf-run`.
- upstream `gptokeyb.armhf` and `sdl_resolution.armhf` resolved all dynamic
  libraries with `ld-linux-armhf.so.3 --list`.
- `scripts/scan-and-fix-port-elfs.sh` wrapped a temporary dynamic armhf smoke
  executable and the wrapper ran successfully through `leaf-armhf-run`.
- Lineoff/gmloader created a hardware OpenGL ES 3.2 context during prototype
  testing, but Lineoff itself is not an audible-audio validation candidate.
- Lineoff/gmloader opened a live PulseAudio sink input through armhf OpenAL
  Soft (`float32le 2ch 44100Hz`) without manual audio environment overrides.
- A dedicated armhf OpenAL smoke binary ran through `bin/leaf-armhf-run` and
  completed against OpenAL Soft.
- Apotris launched as a real PortMaster port and produced audible, correct
  audio on MLP1.
- Shovel Knight launched on MLP1 with managed box86, exported armhf
  `LD_LIBRARY_PATH`, and the `g13p0-01eac0` Mali stack learned from the dArkOS
  RG353V image; the older port-bundled box86 still crashed in comparison.

This proves the dynamic loader path, the SDL helper closure, and one real
dynamic armhf GLES game-port path. It also proves audible audio for at least
one known-audio PortMaster port. It does not prove every PortMaster GLES port.

## Port normalization

The manager refreshes armhf support before upstream PortMaster launches and
again after upstream PortMaster exits:

- repatch upstream PortMaster
- refresh `PortMaster/leaf-armhf-env.sh` through `scripts/write-leaf-runtime-hook.sh`
- source that hook from upstream `control.txt`
- scan `$ROMS_PATH/PORTS` for 32-bit ARM ELFs using ELF headers, not `file` or
  `readelf`, so armhf wrapping does not depend on stock MLP1 firmware tools
- wrap dynamic armhf executables that name `/lib/ld-linux-armhf.so.3`
- leave armhf shared objects untouched and report them
- scan tagged native aarch64 ELFs with the managed SD-local `readelf` when it is
  available, to inventory `DT_NEEDED` libraries for app-local compatibility

The scanner has two layers of speed control. The Leaf wrapper first checks a
cheap top-level `Roms/PORTS` stamp and skips the scanner entirely when no port
tree entry changed. When a scan is needed, `scan-and-fix-port-elfs.sh` uses
`$USERDATA_PATH/portmaster/.leaf/armhf-scan.manifest` to skip unchanged files by
size, mtime, and path. The manifest key includes the script's internal
`RULESET_VERSION`, scan mode, compat availability, SDL fullscreen shim
availability, and `ports_dir`, so changing those inputs naturally forces a cold
scan. Manifest format v3 also records per-ELF SDL2 fullscreen tags, per-ELF
aarch64 compatibility SONAME tags, and the per-script tag used when a launcher
was patched or skipped; this prevents a binary-only port update from leaving an
unchanged `.sh` cached as non-SDL2 or non-compat.
`LEAF_PM_SCAN_NO_CACHE=1` ignores the existing manifest but writes a fresh one;
`LEAF_PM_FULL_PORT_SCAN=1` disables manifest reads and writes for an exhaustive
diagnostic scan.

The hook exports `DEVICE_HAS_ARMHF=Y` plus `LEAF_PM_ARMHF_RUN`,
`LEAF_PM_ARMHF_LOADER`, `LEAF_PM_ARMHF_LIB_PATH`, and `LEAF_PM_BOX86` when the
managed box86 is installed. `DEVICE_ARCH` remains `aarch64`, so ports that
provide native assets still prefer them. The hook does not export armhf GL or
audio variables globally; those stay scoped to `leaf-armhf-run` so native
aarch64 runtimes such as Godot/Weston do not inherit 32-bit compatibility
paths.

The hook also normalizes PortMaster helper state for launched ports. It exports
`HM_TOOLS_DIR`, `HM_PORTS_DIR`, and `HM_SCRIPTS_DIR` to the SD/userdata paths
used by the GUI. When the managed UI Python runtime exists, it creates
`/tmp/leaf-portmaster-python/python3` and prepends that shim to `PATH`. The shim
adds `PYTHONHOME`, `PYTHONPATH`, and the runtime `lib` directory only for the
helper Python process, which lets port scripts run
`harbourmaster runtime_check` without leaking Python runtime settings into the
game executable.

The hook also wraps upstream `bind_directories` and `bind_files` after
`control.txt` is sourced. The wrappers create only the parent path requested by
the port at bind time, and strip a trailing slash from the destination before
symlinking when `PM_CAN_MOUNT=N`. `PM_CAN_MOUNT` defaults to upstream-compatible
`Y`; the only automatic downgrade is a reboot-clean
`/tmp/leaf-pm-mount-probe-failed` marker written by an explicit doctor mount
probe failure. This keeps Pyxel, Ren'Py, AGS, and similar ports on app-local
SD/userdata save/config paths without precreating port-specific directories and
without touching stock eMMC/rootfs.

Launch-env snapshots are diagnostic evidence, not launch-critical state. Normal
port launches write the mode-specific snapshot at most once per boot using a
`/tmp/leaf-pm-env-snapshot-*` marker; `LEAF_PM_ENV_PROBE=1` forces a fresh
snapshot and also refreshes the unqualified `launch-env.*` files for support
bundles and parity checks.

Installed `.sh` port launchers also receive a small `LEAF_PM_PORT_ENV=1`
normalization block before they source upstream `control.txt`. That block
selects the active SD-managed PortMaster tree through `XDG_DATA_HOME` and
`PORTMASTER_CONTROLFOLDER`, rather than letting upstream scripts fall back to
`/roms/ports/PortMaster` on the stock rootfs. The scanner also rewrites common
quoted or unquoted `GAMEDIR=/$directory/ports/<game>` assignments to prefer
`HM_PORTS_DIR`, and the generated hook only overrides `directory` from the active
`ROMS_PATH`/`SDCARD_PATH` when Jawaka's `/roms/ports` bind mount is not present.
This keeps the runtime on SD and avoids writing compatibility state to
eMMC/rootfs.

The generated hook also exposes `LEAF_PM_RETROARCH_BIN` and
`LEAF_PM_RETROARCH_CONFIG` for libretro-style ports. Installed launchers with
upstream `retroarch -L ...` commands are rewritten to call
`leaf_pm_run_retroarch -L ...`, so ports such as 2048 use Leaf's SD-card
RetroArch binary instead of firmware-specific paths like `/usr/bin/retroarch`.

For aarch64 Godot 4.3 ports, the scanner also patches installed Godot shell
launchers with a small `LEAF_PM_GODOT_WAYLAND=1` block. That block calls a hook
function which intercepts `env $weston_dir/westonwrap.sh ...` invocations from
those Godot scripts, including the common `$ESUDO env ...` form used on devices
where PortMaster defines `ESUDO=sudo --preserve-env=...`. MLP1 already has
Leaf's real Weston compositor running, so Godot is launched directly against the
active Wayland display (`XDG_RUNTIME_DIR=/run`, `WAYLAND_DISPLAY=wayland-0` by
default) instead of Westonpack's nested compositor path. When the SD-installed
EGL/GLES compatibility shim is available, that direct Godot launch prepends it
to `LD_LIBRARY_PATH`; the shim is built from source into the Pak and copied to
`$USERDATA_PATH/portmaster/compat/egl/aarch64`. The Godot path keeps the active
stock Mali userspace by default because it matches Leaf's compositor EGL
display, but diagnostic launches can set `LEAF_PM_GODOT_USE_MALI_COMPAT=1` to
force the SD-installed aarch64 Mali bundle. The compatibility path never writes
to the stock rootfs/eMMC. The scanner also wraps direct
`westonwrap.sh cleanup` calls behind `LEAF_PM_SKIP_WESTONPACK_CLEANUP` for
patched Godot launchers; this prevents PortMaster's nested-compositor cleanup
from removing runtime files that belong to Leaf's real compositor. Older
`LEAF_PM_EGL_GLES_SHIM=1` script patches are still recognized through a hook
alias.

The stock MLP1 Mali blob (`libmali.so.1.9.0`, bifrost g13p0, legacy `t6xx`
kernel driver) mis-tracks in-flight GPU jobs against Godot 4's streamed 2D
canvas buffers. Symptom: the boot splash shows, then the screen stays pure
black while the game keeps running; the kernel logs
`mali ... Unhandled Page fault in AS2` / `job status ... TERMINATED` every
frame and the blob raises `GL_OUT_OF_MEMORY`. Slime 3K Demake reproduced this
on every scene after the splash. The generated hook therefore exports
`LEAF_EGL_DRAW_FINISH=1` for Godot runtime launches, which makes the Leaf EGL
shim issue `glFinish` after every draw call, serializing GPU jobs and
eliminating the faults (measured ~40 fps in the Slime 3K menu, ~20 fps in
gameplay scenes, versus a black screen otherwise). Set
`LEAF_PM_GODOT_DRAW_FINISH=0` to disable the workaround, or pass the shim's
other modes (`flush`, `sync`, `upload`) for diagnosis — during bisection none
of the weaker barriers (per-draw `glFlush`, forced `glClientWaitSync`,
finish-before-upload/map/delete) prevented the faults; only per-draw
`glFinish` did. The shim also honors `LEAF_EGL_DEBUG=1` to log the chosen EGL
config, per-frame backbuffer readbacks, and framebuffer-related GL calls to
stderr.

Some installed Godot launchers request the `godot_4.2.2` PortMaster runtime,
whose aarch64 binary only supports X11/headless display drivers. For those
Westonpack-style launchers, the scanner injects
`LEAF_PM_GODOT_WAYLAND_RUNTIME_UPGRADE=1` after the upstream runtime variables
and switches the launch to upstream `godot_4.3`, whose aarch64 runtime supports
Wayland. The script still goes through upstream `runtime_check`, so the 4.3
squashfs is downloaded by PortMaster if it is not already installed. Set
`LEAF_PM_GODOT_422_WAYLAND_UPGRADE=0`, or override
`LEAF_PM_GODOT_WAYLAND_RUNTIME` and `LEAF_PM_GODOT_WAYLAND_EXECUTABLE`, for a
specific diagnostic launch.

Some ports, such as Songo #5, launch a custom Godot/SDL2 runtime directly
instead of using Westonpack. The scanner wraps those direct
`"$GAMEDIR/runtime/$runtime" --main-pack ...` commands with
`leaf_pm_run_godot_sdl2_runtime`, while keeping the original command as a
fallback. That helper applies the SD-managed EGL/Wayland environment and, for
non-FRT runtimes, the bundled Mali compatibility path. When the port did not
provide its own window sizing flags, it adds
`--resolution 960x720`. It intentionally does not add Godot fullscreen (`-f`),
because that path fills the MLP1's native portrait `720x960` KMS framebuffer and
can mangle landscape content.

Godot 3.x FRT ports such as Cats on Mars use an upstream pattern that mounts
`frt_*.squashfs` under `$HOME/godot`, prepends that mountpoint to `PATH`, and
then launches `"$runtime" --main-pack ...`. The scanner recognizes that pattern,
fixes the pre-mount cleanup to unmount the SD/userdata mountpoint rather than
the squashfs file, and wraps the final launch in
`leaf_pm_run_godot_sdl2_runtime`. The mountpoint is under
`$USERDATA_PATH/portmaster` through the generated hook's `HOME`; no stock
rootfs/eMMC paths are modified. FRT launches keep the stock MLP1 Mali library by
default because the bundled Mali compatibility library fails SDL window creation
for these older Godot runtimes with `Could not get EGL display`.

For Gothic/Machismo launchers such as Mina the Hollower, the scanner patches a
small runtime compatibility block that defaults `GOTHIC_BACKEND=gles` on
Leaf/MLP1. The block does not depend on `CFW_NAME`, because direct Jawaka
launches and source-built local installs may not inherit the PortMaster GUI
environment. It also forwards `GOTHIC_BACKEND` through the final `env` launcher
so `sudo`-based setups do not scrub it.

For Ship of Harkinian, the scanner patches the installed launcher with a
launch-time `LEAF_PM_RUNTIME_COMPAT_SOH_DISPLAY=1` helper. The helper keeps all
writes inside the SD-installed port directory, copies an already-generated
`baseroms/oot*.o2r` artifact to the root path that the upstream launcher checks,
and normalizes `shipofharkinian.json` plus `imgui.ini` to MLP1's 960x720
landscape window size. It runs before `otr_check` so the port does not repeat a
completed patch step, and again after upstream `imgui_reset` so the final window
state wins.

For Love 11.5 ports, the scanner detects installed launchers that run a `love`
binary or reference `liblove-11.5.so` and patches them with
`LEAF_PM_RUNTIME_COMPAT_LOVE_11_5_LIBS=1`. The block appends upstream
PortMaster's SD-installed `runtimes/love_11.5/libs.aarch64` directory to
`LD_LIBRARY_PATH`, preserving any port-local `libs` directory first. This covers
ports whose bundled Love launcher omits one of the shared libraries already
provided by the upstream PortMaster runtime, such as `libmodplug.so.1`, without
copying files to eMMC/rootfs or adding a Leaf-specific binary payload.

The scanner also normalizes a generic unsafe shell pattern where launchers
lowercase an entire absolute file path before `mv`. On case-sensitive paths such
as `Roms/PORTS`, that can turn the directory component into a nonexistent
`roms/ports` path. The rewrite keeps the original directory and lowercases only
the basename. On vfat, where case-only renames can fail with `File exists`, it
renames through a temporary sibling path first and restores the source if the
second rename fails.

For SDL2 ports with fixed-size or windowed launch defaults, PortMaster-mlp1
packages a preload shim built from `compat/sdl2/leaf-sdl2-fullscreen.c`. The
aarch64 copy lives at
`$USERDATA_PATH/portmaster/compat/sdl2/aarch64/leaf-sdl2-fullscreen.so`; the
armhf copy lives at
`$USERDATA_PATH/portmaster/compat/armhf/bin/leaf-sdl2-fullscreen.so`. The
scanner now detects SDL2-linked aarch64 and armhf ELF candidates generically by
ELF header plus `libSDL2-2.0.so` string, tags the owning port, and injects a
script-wide `LEAF_PM_SDL2_FULLSCREEN_ENV=1` block into launchers for tagged
ports. The block calls `leaf_pm_enable_sdl2_fullscreen_env "<port>"
"<arch-csv>"` after `get_controls` when possible, otherwise after
`control.txt` is sourced.

The hook exports `LEAF_PM_SDL_FORCE_FULLSCREEN=1` and 960x720 target dimensions
from `DISPLAY_WIDTH`/`DISPLAY_HEIGHT`. Native aarch64 ports receive the shim via
deduped `LD_PRELOAD`. Armhf ports receive it through `LEAF_PM_ARMHF_PRELOAD`,
and `leaf-armhf-run` converts that to `LD_PRELOAD` only for the final armhf
loader exec. This keeps the 32-bit shim away from native helpers such as
`gptokeyb`, `pidof`, or `kill`. Older VVVVVV and `bgdi` launchers that were
already patched through `leaf_pm_run_aarch64_sdl2_fullscreen` or
`leaf_pm_run_armhf_sdl2_fullscreen` remain supported; the old helpers dedupe
their preload path when the new script-wide block is also present.

Opt-outs are available at three levels:

- set `LEAF_PM_SDL_FORCE_FULLSCREEN=0` before launch
- add the port directory name to
  `$USERDATA_PATH/portmaster/sdl2-fullscreen-optout.txt`
- add a repo-side case to `is_sdl2_fullscreen_optout_port`

Godot launchers continue to use the dedicated Godot resolution path rather than
generic SDL fullscreen. Ship of Harkinian is initially kept on its existing
JSON/imgui display normalization path, because it links SDL2 but has more
specific display state to maintain. Fast scan covers common one-level,
`bin/`, `lib/`, and `libs/` binaries; use `LEAF_PM_FULL_PORT_SCAN=1` for a
diagnostic scan when a port's main binary sits deeper in its tree.

Pyxel source launchers are patched through a source-level compatibility rule
instead of the generic SDL2 preload. The scanner adds a helper that computes the
largest integer `display_scale` that fits `DISPLAY_WIDTH`/`DISPLAY_HEIGHT`, then
calls `pyxel.fullscreen(True)`. This keeps the fix in Pyxel's own display API,
because the runtime's Rust extension does not expose `SDL_CreateWindow` through
a normal SDL2 symbol path for preloading.

The scan JSON reports generic SDL2 coverage through
`sdl2_fullscreen_ports_aarch64`, `sdl2_fullscreen_ports_armhf`,
`sdl2_fullscreen_ports_both`, and the
`sdl2_fullscreen_env_scripts_*` counters for patched, already patched,
non-SDL2, no-`GAMEDIR`, missing-shim, opt-out, missing-anchor, and error
outcomes.

Native aarch64 ports can also depend on older Debian SONAMEs that are absent
from the stock MLP1 rootfs, such as `libwebp.so.6`, `libavformat.so.58`, or
`libjpeg.so.8`. The aarch64 compatibility library pack installs those files
under `$USERDATA_PATH/portmaster/compat/libs/aarch64` and the generated hook
exposes `leaf_pm_enable_aarch64_compat_libs`. The scanner does not enable that
path globally. Instead, it reads each tagged native aarch64 ELF's `DT_NEEDED`
entries with the managed `readelf`, resolves them in this order:

- the installed port's own tree, so bundled libraries win
- stock aarch64 library directories such as `/usr/lib` and `/lib`
- the SD-installed aarch64 compatibility library pack

If a needed SONAME resolves only in the app-local compatibility pack, the
scanner injects a per-launcher `LEAF_PM_AARCH64_COMPAT_LIBS=1` block after the
normal PortMaster/control setup anchor. That block calls
`leaf_pm_enable_aarch64_compat_libs`, which prepends only
`$USERDATA_PATH/portmaster/compat/libs/aarch64` to `LD_LIBRARY_PATH`. No stock
rootfs/eMMC path is modified, and the change exists only in SD-installed
launcher scripts and process environment for that port launch. Add the port
directory name, or `*`, to
`$USERDATA_PATH/portmaster/compat-libs-optout.txt` to prevent automatic
injection for diagnostics.

The scan JSON reports this native compatibility pass through
`aarch64_elfs_seen`, `aarch64_compat_lib_ports`,
`aarch64_compat_lib_port_sonames`, `unresolved_sonames`,
`aarch64_compat_lib_scripts_*`, and `readelf_available`. `portmaster --doctor`
also surfaces `lib.unresolved_sonames` as an informational row so missing
SONAMEs can be triaged without treating them as stock-OS drift or an app setup
failure.

The generated hook also installs an SD-managed aarch64 Mali userspace bundle at
`$USERDATA_PATH/portmaster/compat/mali/aarch64/libmali.so.1`. This bundle is
built from the pinned JeffyCN `libmali-next`
`libmali-bifrost-g52-g29p1.so` asset and contains `libmali.so.1` plus the local
`rk_vk_g29.json` ICD used by direct-display Vulkan launchers. Godot direct
launches leave Leaf's stock compositor-matched Mali stack active by default;
set `LEAF_PM_GODOT_USE_MALI_COMPAT=1` for an explicit Godot diagnostic opt-in.
Other ports continue to use the stock rootfs graphics stack unless their own
wrapper opts in.

The same hook also applies the global controller-layout preference for installed
ports. By default it exports the MLP1 Nintendo SDL mapping. If
`$USERDATA_PATH/portmaster/x360` exists, it exports the X360 mapping instead;
`$USERDATA_PATH/portmaster/nintendo` records an explicit Nintendo selection. The
PortMaster GUI launch sets `PORTMASTER_LEAF_PORT_LAYOUT_SCOPE=gui`, which forces
a GUI-only mapping with A/B swapped and X/Y kept in the X360 positions. For
ports, the hook wraps upstream `get_controls`, then writes
`SDL_GAMECONTROLLERCONFIG`, `sdl_controllerconfig`, and a temporary
`SDL_GAMECONTROLLERCONFIG_FILE` under `/tmp` so both direct SDL users and
PortMaster scripts that re-export `$sdl_controllerconfig` see the selected
layout.

On MLP1, `/mnt/sdcard` and `/media/sdcard1` may both exist, but they are not
safe to treat as interchangeable. PortMaster-mlp1 uses explicit
`SDCARD_PATH`/`JAWAKA_SDCARD_ROOT`, the pak's own `Apps/<platform>` location,
or one uniquely Leaf-marked SD root. If those checks cannot identify a single
root, the launcher, scanner, and hook writer fail instead of guessing. The
generated hook still derives `LEAF_PM_ARMHF_ROOT` from the active
`controlfolder` at launch time, which keeps wrapped ports working after reboot
when PortMaster was previously scanned through the other SD alias.

The upstream PortMaster GUI filters available ports through HarbourMaster's
Python device `capabilities`, not only the shell `DEVICE_HAS_ARMHF` flag. Leaf
therefore advertises `armhf` to HarbourMaster only when
`$USERDATA_PATH/portmaster/compat/armhf/lib/ld-linux-armhf.so.3` exists, while
keeping `primary_arch` as `aarch64`.

Reports are written to:

```text
$USERDATA_PATH/portmaster/.leaf/armhf-scan.json
$USERDATA_PATH/portmaster/.leaf/armhf-scan.tsv
$USERDATA_PATH/portmaster/.leaf/armhf-scan.manifest
```

## SD filesystem and large files

On stock MLP1 firmware, Leaf supports PortMaster on the stock-mounted
FAT32/vfat SD card. The connected test device exposes both SD mount candidates
as `vfat`, and the stock kernel does not expose `CONFIG_EXFAT_FS`; exFAT is
therefore not a supported PortMaster card format for stock-firmware boot and
automount. This is a firmware boundary, not a PortMaster install bug.

FAT32 cannot store one file at or above 4 GiB. Leaf does not work around that
by changing the stock OS, writing eMMC filesystem helpers, or installing
system services. Ports with a required single file at that size remain
unsupported on stock MLP1 until their data is split upstream or the device
moves to a different firmware/storage stack.

The manager-owned downloader checks expected download size and target free
space before downloading PortMaster GUI and managed UI-runtime archives. On a
vfat target, a known single download at or above 4 GiB fails before writing a
partial file and logs the specific FAT32 boundary under:

```text
$USERDATA_PATH/portmaster/.leaf/logs/download.log
```

If a write still fails at runtime with `EFBIG`, a short write at the single-file
limit, or `ENOSPC`, the manager reports that classification instead of a
generic curl error. The app-local `sudo` shim also classifies failed
`harbourmaster runtime_check <runtime>.squashfs` calls when the runtime file is
already near the FAT32 boundary. The shim is only on PATH when the optional
`PortMaster.pak` has installed its SD-local tools.

The scanner records an early-warning inventory of installed port files larger
than 3.5 GiB in `armhf-scan.json`. Doctor exposes that cached count as
`storage.large_files` next to `storage.sd_filesystem`, so support bundles can
show whether an installed port is already close to the stock FAT32 limit
without recursively copying user data.

## Runtime squashfs formats

MLP1 stock firmware exposes squashfs with gzip/zlib, lzo, and xz compression
support through the kernel config. It does not expose `CONFIG_SQUASHFS_ZSTD`
or `CONFIG_SQUASHFS_LZ4`, so zstd/lz4 runtime images must not be treated as
kernel-mountable on this device.

The CFW doctor reads each installed runtime image under
`$USERDATA_PATH/portmaster/PortMaster/libs/*.squashfs` directly from the
squashfs superblock and reports the compression id in
`kernel.squashfs_runtime_formats`. Supported ids are `1=gzip`, `2=lzma`,
`3=lzo`, `4=xz`, `5=lz4`, and `6=zstd`. Unsupported installed images produce a
doctor FAIL with the exact image name and format instead of leaving the user to
debug a later mount error.

The smoke matrix also publishes that doctor check as a dedicated
`doctor/kernel.squashfs_runtime_formats` TSV row. On 2026-07-04 the connected
MLP1 had 12 installed runtime images, all gzip/id 1, and the row passed with
`unsupported=0 unreadable=0 unknown=0`. A temporary SD-only synthetic
`leaf-test-zstd.squashfs` header produced the expected FAIL
(`zstd (id 6, kernel missing)`) and was removed immediately afterward.

For port-visible failures, the app-local tools pack installs
`leaf-squashfs-check`, `squashfuse`, and the private shared libraries needed
by `squashfuse` under `$USERDATA_PATH/portmaster/compat/tools/aarch64`. The
fallback is loaded only through the optional `PortMaster.pak` tool path; it
does not install commands, libraries, kernel modules, or mount configuration on
the stock OS/eMMC.

The `sudo` shim routes through the preflight for two cases:

- after `$ESUDO .../harbourmaster ... runtime_check <runtime>.squashfs`
  succeeds, so a newly downloaded unsupported runtime is noticed immediately
  while still allowing launch to continue when the app-local fallback is present
- before `$ESUDO mount .../*.squashfs ...`, so kernel-supported images continue
  to use the normal stock kernel mount path, while zstd/lz4 images are mounted
  through app-local `squashfuse`

The helper is SD/userdata-local and is only on PATH when the optional
`PortMaster.pak` has installed its tools. Unsupported-but-known formats return
exit 66 from `leaf-squashfs-check`, which lets `sudo mount` distinguish "use
fallback" from corrupt/unknown images. The fallback creates only the requested
runtime mount in the current launch session; it is cleared by unmounting or by
reboot and leaves the stock OS untouched.

## Support bundle

For handoff to a Leaf tester or PortMaster maintainer, the optional pak can
aggregate the existing diagnostics into one zip:

```sh
./launch.sh --support-bundle /tmp/leaf-pm-support.zip
```

If no path is supplied, the bundle is written under
`$USERDATA_PATH/portmaster/.leaf/support/`. The command stages temporary files
under that same SD/userdata support directory, then writes only the requested
zip path. It does not recurse through broad userdata, save data, PulseAudio
cookies, backups, staging downloads, or full runtime trees.

The bundle includes doctor CFW JSON/text, UI state text, launch-env snapshots,
Leaf manifests/update state, armhf scan reports, smoke matrix reports and logs,
PortMaster manager/upstream log tails, recent port log tails, kernel/os/storage
summaries, installed pak listings, and lock/manifest versions for the installed
packs. It also includes a README with the standard SSH debug flow:

- install or launch `SSHServer.pak` from `Apps/mlp1` when network shell access
  is needed
- prepend the app-local tools path:
  `export PATH="$USERDATA_PATH/portmaster/compat/tools/aarch64/bin:$PATH"`
- rerun probes from `PortMaster.pak`, such as `./launch.sh --doctor-cfw-text`
  or `LEAF_PM_ENV_PROBE=1` on a port script

## Active smoke evidence

`scripts/adb-portmaster-smoke-matrix.sh` keeps passive readiness rows by
default. With `LEAF_PM_SMOKE_INTERACTIVE=1`, rows marked for active smoke launch
their port script in an isolated process group from ADB, poll for the expected
runtime/game process, capture stdout plus port log tails, and terminate the
process group. Reports are written under
`$USERDATA_PATH/portmaster/.leaf/smoke/`.

MLP1 interactive smoke on 2026-07-04 with
`LEAF_PM_SMOKE_PORTS='6-feet-under,cats-on-mars,mr-rescue'` produced
`pass=24 ready=5 skipped=1`. Active launch rows:

| Runtime family | Port | Evidence |
| --- | --- | --- |
| GameMaker gmtoolkit + dotnet | 6 Feet Under | `interactive-launch pass`; first launch mounted `gmtoolkit.squashfs` and `dotnet-8.0.12.squashfs`, patched the game data to 960x720, and reached `gmloadernext.aarch64` main loop. |
| Godot 3.x FRT | Cats on Mars | `interactive-launch pass`; `frt_3.2.3.squashfs` mount path was patched and the `frt_3.2.3` process was observed. |
| Love2D | Mr. Rescue | `interactive-launch pass`; the Love 11.5 lib compatibility block was present and the Love runtime process was observed. |

MLP1 interactive smoke on 2026-07-04 with
`SDCARD_PATH=/media/sdcard1 LEAF_PM_SMOKE_PORTS='shattered-pixel-dungeon unciv megaball ticoban cute-fame-halloween-bash shards-of-god'`
produced `pass=36 ready=8 skipped=1`. Active launch rows:

| Runtime family | Port | Evidence |
| --- | --- | --- |
| Java JRE17 + Weston | Shattered Pixel Dungeon | `interactive-launch pass`; Java 17 runtime and Westonpack path mounted from app-local PortMaster libs and the `ShatteredPD.jar` process was observed. |
| Java JDK8 + Weston | Unciv | `interactive-launch pass`; Java 8 JDK runtime and Westonpack path mounted from app-local PortMaster libs and the `Unciv.jar` process was observed on the 1 GB RAM profile. |
| Pyxel | Megaball | `interactive-launch pass`; Pyxel 2.2.8 runtime process was observed running the bundled `main.py`. |
| Pyxel | Ticoban | `interactive-launch pass`; Pyxel 2.2.8 runtime process was observed running `ticoban.pyxapp`. |
| Ren'Py | Cute Fame: Halloween Bash | `interactive-launch pass`; the launcher uses `renpy_8.1.3.squashfs` and reached the Ren'Py startup process. HarbourMaster metadata also downloaded `renpy_8.3.4.squashfs`, so both runtimes are present. |
| AGS | Shards of God | `interactive-launch pass`; AGS 3.6 interpreter reached the game loop with the SDL2 fullscreen shim active. |

MLP1 interactive smoke on 2026-07-05 with
`SDCARD_PATH=/media/sdcard1 LEAF_PM_SMOKE_PORTS='vvvvvv 2048 apotris songo5 celeste shovel-knight'`
produced `pass=36 ready=8 skipped=1`. Active launch rows:

| Runtime family | Port | Evidence |
| --- | --- | --- |
| Native SDL2 | VVVVVV | `interactive-launch pass`; the `VVVVVV` process was observed with the aarch64 SDL2 fullscreen shim active. |
| Libretro/RetroArch | 2048 | `interactive-launch pass`; the RetroArch/libretro launch path was observed through the normalized `leaf_pm_run_retroarch` wrapper. |
| GameMaker SDL2 | Apotris | `interactive-launch pass`; the aarch64 `Apotris.aarch64` process was observed. |
| Godot 4.3 direct SDL2 | Songo5 | `interactive-launch pass`; the scanner-patched direct Godot path ran through `leaf_pm_run_godot_sdl2_runtime` and the `sbc_4_3_rcv12` process was observed at 960x720. |
| Mono/FNA | Celeste | `interactive-launch pass`; the scanner now repairs lowercase `gamedir=/$directory/ports/...` assignments to `HM_PORTS_DIR`, then the Mono runtime launched `Celeste.exe`. |
| box86 | Shovel Knight | `interactive-launch pass`; the bundled `box86` path launched `ShovelKnight` with the managed armhf compatibility environment. |

MLP1 manual launch probe on 2026-07-06 for Slime 3K Demake:

| Runtime family | Port | Evidence |
| --- | --- | --- |
| Godot 4.3 Westonpack | Slime 3K Demake | Direct ADB launch reached Godot's Wayland display path and remained running until the 12 second timeout after the hook intercepted `$ESUDO env $weston_dir/westonwrap.sh ...` and left the stock Mali EGL stack active. |

After the post-exit scan, the manager asks Jawaka to run `scan-library` through
`jawaka-platformctl`. The request is best-effort so PortMaster remains usable
when Jawaka is not running, but normal Leaf launches refresh the Ports list
after installing or removing ports.
