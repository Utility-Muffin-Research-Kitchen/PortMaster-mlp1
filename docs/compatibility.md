# Compatibility

MLP1 is an aarch64 RK3566 handheld. The stock kernel supports 32-bit ARM compat,
but the stock rootfs does not ship `/lib/ld-linux-armhf.so.3` or a 32-bit
userspace.

Compatibility tiers:

- Tier 0: static armhf helper binaries.
- Tier 1: dynamic armhf SDL/software-rendered ports using the Leaf armhf pack.
- Tier 2: dynamic armhf GLES ports after a legal 32-bit graphics stack is
  sourced and smoked.

The manager must not claim Tier 2 until it is proven on hardware.

The Spruce binary closure report covers the binaries Spruce ships with its
PortMaster app and runtime. It includes upstream static armhf helper binaries
from the locked `PortMaster.zip`, but it does not close the dynamic 32-bit
PortMaster game-port gap. That requires the separate Leaf armhf userspace pack
and per-port smoke testing.

## Initial armhf pack

`make build-armhf-compat` builds the first compatibility pack from Debian
Bookworm armhf packages. The generated zip is a release-asset candidate, not a
git-vendored payload:

```text
build/armhf-compat/portmaster-mlp1-armhf-compat-bookworm-20260629.zip
build/armhf-compat/portmaster-mlp1-armhf-compat-bookworm-20260629.json
```

The pack installs under `$USERDATA_PATH/portmaster/compat/armhf` and includes:

- `lib/ld-linux-armhf.so.3`
- glibc, libgcc, libstdc++, SDL2, SDL2_image, SDL2_mixer, SDL2_ttf, SDL2_gfx
- common audio/image/font/Wayland/X11 support libraries needed by SDL helpers
- `bin/leaf-armhf-run`, a non-root loader wrapper
- `bin/leaf-armhf-smoke`, a tiny dynamic armhf smoke binary

Device smoke on MLP1 on 2026-06-29:

- `leaf-armhf-smoke` ran through `bin/leaf-armhf-run`.
- upstream `xdelta3.armhf -V` ran through `bin/leaf-armhf-run`.
- upstream `gptokeyb.armhf` and `sdl_resolution.armhf` resolved all dynamic
  libraries with `ld-linux-armhf.so.3 --list`.
- `scripts/scan-and-fix-port-elfs.sh` wrapped a temporary dynamic armhf smoke
  executable and the wrapper ran successfully through `leaf-armhf-run`.

This proves the dynamic loader path and a useful SDL helper closure, but it does
not yet prove a full dynamic armhf game port. Tier 2 GLES remains unclaimed.

## Port normalization

The manager refreshes armhf support every time it launches upstream PortMaster:

- repatch upstream PortMaster
- write `PortMaster/leaf-armhf-env.sh`
- source that hook from upstream `control.txt`
- scan `$ROMS_PATH/PORTS` for 32-bit ARM ELFs using ELF headers, not `file` or
  `readelf`, because those tools are not present on stock MLP1 firmware
- wrap dynamic armhf executables that name `/lib/ld-linux-armhf.so.3`
- leave armhf shared objects untouched and report them

The hook exports `DEVICE_HAS_ARMHF=Y` plus `LEAF_PM_ARMHF_RUN`,
`LEAF_PM_ARMHF_LOADER`, and `LEAF_PM_ARMHF_LIB_PATH`. `DEVICE_ARCH` remains
`aarch64`, so ports that provide native assets still prefer them.

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
