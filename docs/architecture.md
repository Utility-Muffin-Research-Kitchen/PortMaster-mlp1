# Architecture

`PortMaster-mlp1` is a small Leaf app that manages an upstream PortMaster
install under user data. It does not replace PortMaster's own store UI.

The manager owns:

- downloading and verifying stable upstream PortMaster assets
- applying Leaf/MLP1 patches and generated overlays
- launching upstream PortMaster with Leaf paths and a managed Python runtime
- verifying/repatching after upstream self-updates
- runtime and armhf compatibility pack status
- doctor/log screens

The app is distributed through Pak Rat as a first-party optional app.

Stock MLP1 firmware does not ship `python3`, while upstream `PortMaster.sh`
executes Python scripts such as `pugwash` and `harbourmaster`. The manager
therefore treats the PortMaster UI runtime as a separate generated/downloaded
artifact installed at `$USERDATA_PATH/portmaster/runtime`. Spruce's runtime is
useful proof that this shape works on MLP1, but Leaf needs a provenance-clean,
hash-pinned runtime artifact before it becomes a default user flow.
