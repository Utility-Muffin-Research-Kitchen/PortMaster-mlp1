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
