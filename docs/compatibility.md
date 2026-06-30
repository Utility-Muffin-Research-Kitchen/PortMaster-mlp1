# Compatibility

MLP1 is an aarch64 RK3566 handheld. The stock kernel supports 32-bit ARM compat,
but the stock rootfs does not ship `/lib/ld-linux-armhf.so.3` or a 32-bit
userspace.

Compatibility tiers:

- Tier 0: static armhf helper binaries.
- Tier 1: dynamic armhf SDL/software-rendered ports using the Leaf armhf pack.
- Tier 2: dynamic armhf GLES ports using the bundled 32-bit Rockchip Mali stack.

Tier 2 is claimed only for the pinned Rockchip Mali g24p0 stack after an MLP1
Lineoff GLES smoke test. More GLES ports still need per-port smoke testing.

The Spruce binary closure report covers the binaries Spruce ships with its
PortMaster app and runtime. It includes upstream static armhf helper binaries
from the locked `PortMaster.zip`, but it does not close the dynamic 32-bit
PortMaster game-port gap. That requires the separate Leaf armhf userspace pack
and per-port smoke testing.

## Initial armhf pack

`make build-armhf-compat` builds the compatibility pack from Debian Bookworm
armhf packages plus a pinned Rockchip Mali armhf userspace blob. The generated
zip is a release-asset candidate, not a git-vendored payload:

```text
build/armhf-compat/portmaster-mlp1-armhf-compat-bookworm-mali-g24p0-20260630.zip
build/armhf-compat/portmaster-mlp1-armhf-compat-bookworm-mali-g24p0-20260630.json
```

The pack installs under `$USERDATA_PATH/portmaster/compat/armhf` and includes:

- `lib/ld-linux-armhf.so.3`
- glibc, libgcc, libstdc++, SDL2, SDL2_image, SDL2_mixer, SDL2_ttf, SDL2_gfx
- common audio/image/font/Wayland/X11 support libraries needed by SDL helpers
- Rockchip Mali Bifrost G52 `g24p0-00eac0` 32-bit Wayland/GBM userspace from
  `tsukumijima/libmali-rockchip`
- `licenses/mali/libmali-rockchip-debian-copyright`, which includes the Arm
  Mali userspace driver EULA and redistribution notice requirements
- `bin/leaf-armhf-run`, a non-root loader wrapper
- `bin/leaf-armhf-smoke`, a tiny dynamic armhf smoke binary

`bin/leaf-armhf-run` also exports the Leaf PulseAudio socket and OpenAL Soft
defaults used by gmloader-style ports: `PULSE_SERVER=unix:/tmp/pulse-socket`,
`PULSE_CLIENTCONFIG=$LEAF_PM_ARMHF_ROOT/etc/pulse/client.conf`,
`ALSOFT_DRIVERS=pulse`, and
`ALSOFT_CONF=$LEAF_PM_ARMHF_ROOT/etc/openal/alsoft.conf`.

The Mali blob is pinned by commit, path, size, and SHA-256 in the generated
manifest. The blob is not built from source and remains under Arm's closed Mali
userspace driver EULA.

Device smoke on MLP1 on 2026-06-29 and 2026-06-30:

- `leaf-armhf-smoke` ran through `bin/leaf-armhf-run`.
- upstream `xdelta3.armhf -V` ran through `bin/leaf-armhf-run`.
- upstream `gptokeyb.armhf` and `sdl_resolution.armhf` resolved all dynamic
  libraries with `ld-linux-armhf.so.3 --list`.
- `scripts/scan-and-fix-port-elfs.sh` wrapped a temporary dynamic armhf smoke
  executable and the wrapper ran successfully through `leaf-armhf-run`.
- Lineoff/gmloader created a hardware OpenGL ES 3.2 context on MLP1 with ARM
  vendor string and the `g24p0-00eac0` driver string.
- Lineoff/gmloader opened a live PulseAudio sink input through armhf OpenAL
  Soft (`float32le 2ch 44100Hz`) without manual audio environment overrides.
  Lineoff itself is not an audible-audio validation candidate.
- A dedicated armhf OpenAL smoke binary ran through `bin/leaf-armhf-run` and
  completed against OpenAL Soft.
- Apotris launched as a real PortMaster port and produced audible, correct
  audio on MLP1.

This proves the dynamic loader path, the SDL helper closure, and one real
dynamic armhf GLES game-port path. It also proves audible audio for at least
one known-audio PortMaster port. It does not prove every PortMaster GLES port.

## Port normalization

The manager refreshes armhf support before upstream PortMaster launches and
again after upstream PortMaster exits:

- repatch upstream PortMaster
- write `PortMaster/leaf-armhf-env.sh`
- source that hook from upstream `control.txt`
- scan `$ROMS_PATH/PORTS` for 32-bit ARM ELFs using ELF headers, not `file` or
  `readelf`, because those tools are not present on stock MLP1 firmware
- wrap dynamic armhf executables that name `/lib/ld-linux-armhf.so.3`
- leave armhf shared objects untouched and report them

The hook exports `DEVICE_HAS_ARMHF=Y` plus `LEAF_PM_ARMHF_RUN`,
`LEAF_PM_ARMHF_LOADER`, and `LEAF_PM_ARMHF_LIB_PATH`. `DEVICE_ARCH` remains
`aarch64`, so ports that provide native assets still prefer them. The hook does
not export armhf GL or audio variables globally; those stay scoped to
`leaf-armhf-run` so native aarch64 runtimes such as Godot/Weston do not inherit
32-bit compatibility paths.

The hook also normalizes PortMaster helper state for launched ports. It exports
`HM_TOOLS_DIR`, `HM_PORTS_DIR`, and `HM_SCRIPTS_DIR` to the SD/userdata paths
used by the GUI. When the managed UI Python runtime exists, it creates
`/tmp/leaf-portmaster-python/python3` and prepends that shim to `PATH`. The shim
adds `PYTHONHOME`, `PYTHONPATH`, and the runtime `lib` directory only for the
helper Python process, which lets port scripts run
`harbourmaster runtime_check` without leaking Python runtime settings into the
game executable.

Installed `.sh` port launchers also receive a small `LEAF_PM_PORT_ENV=1`
normalization block before they source upstream `control.txt`. That block
selects the active SD-managed PortMaster tree through `XDG_DATA_HOME` and
`PORTMASTER_CONTROLFOLDER`, rather than letting upstream scripts fall back to
`/roms/ports/PortMaster` on the stock rootfs. The scanner also rewrites common
`GAMEDIR=/$directory/ports/<game>` assignments to prefer `HM_PORTS_DIR`, and
the generated hook only overrides `directory` from the active
`ROMS_PATH`/`SDCARD_PATH` when Jawaka's `/roms/ports` bind mount is not present.
This keeps the runtime on SD and avoids writing compatibility state to
eMMC/rootfs.

For aarch64 Godot 4.3 ports, the scanner also patches installed Godot shell
launchers with a small `LEAF_PM_GODOT_WAYLAND=1` block. That block calls a hook
function which intercepts only `env $weston_dir/westonwrap.sh ...` invocations
from those Godot scripts. MLP1 already has Leaf's real Weston compositor
running, so Godot is launched directly against the active Wayland display
(`XDG_RUNTIME_DIR=/run`, `WAYLAND_DISPLAY=wayland-0` by default) instead of
Westonpack's nested compositor path. When the SD-installed EGL/GLES
compatibility shim is available, that direct Godot launch prepends it to
`LD_LIBRARY_PATH`; the shim is built from source into the Pak and copied to
`$USERDATA_PATH/portmaster/compat/egl/aarch64`. The compatibility path never
writes to the stock rootfs/eMMC. The scanner also wraps direct
`westonwrap.sh cleanup` calls behind `LEAF_PM_SKIP_WESTONPACK_CLEANUP` for
patched Godot launchers; this prevents PortMaster's nested-compositor cleanup
from removing runtime files that belong to Leaf's real compositor. Older
`LEAF_PM_EGL_GLES_SHIM=1` script patches are still recognized through a hook
alias.

The Godot hook also prefers an SD-installed aarch64 Mali userspace bundle when
`$USERDATA_PATH/portmaster/compat/mali/aarch64/libmali.so.1` is present. This
bundle is built from the pinned `tsukumijima/libmali-rockchip`
`libmali-bifrost-g52-g24p0-wayland-gbm_1.9-1_arm64.deb` asset and contains only
`libmali.so.1` and `libmali-hook.so.1`. It is intentionally scoped to Godot
direct-Wayland launches; other ports continue to use the stock rootfs graphics
stack unless their own wrapper opts in.

The same hook also applies the global controller-layout preference for installed
ports. By default it exports the MLP1 X360 SDL mapping. If
`$USERDATA_PATH/portmaster/nintendo` exists, it exports the Nintendo mapping
instead. The PortMaster GUI launch sets `PORTMASTER_LEAF_PORT_LAYOUT_SCOPE=gui`,
which forces a GUI-only mapping with A/B swapped and X/Y kept in the X360
positions. For ports, the hook wraps upstream `get_controls`, then writes
`SDL_GAMECONTROLLERCONFIG`, `sdl_controllerconfig`, and a temporary
`SDL_GAMECONTROLLERCONFIG_FILE` under `/tmp` so both direct SDL users and
PortMaster scripts that re-export `$sdl_controllerconfig` see the selected
layout.

On MLP1, `/mnt/sdcard` and `/media/sdcard1` may both exist, but they are not
safe to treat as interchangeable. The scanner prefers the Leaf-marked SD path
and the generated hook derives `LEAF_PM_ARMHF_ROOT` from the active
`controlfolder` at launch time. This keeps wrapped ports working after reboot
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
```

After the post-exit scan, the manager asks Jawaka to run `scan-library` through
`jawaka-platformctl`. The request is best-effort so PortMaster remains usable
when Jawaka is not running, but normal Leaf launches refresh the Ports list
after installing or removing ports.
