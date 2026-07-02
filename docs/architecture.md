# Architecture

`PortMaster-mlp1` is a small Leaf app that manages an upstream PortMaster
install under user data. It does not replace PortMaster's own store UI.

The manager owns:

- downloading and verifying stable upstream PortMaster assets
- applying Leaf/MLP1 patches and generated overlays
- launching upstream PortMaster with Leaf paths and a managed Python runtime
- disabling upstream GUI self-update prompts during managed launches
- polling and applying stable PortMaster GUI updates through the manager
- recording GUI update state and attempts under `.leaf/`
- verifying/repatching after launch and after upstream exits
- runtime and armhf compatibility pack status
- doctor/log screens

The app is distributed through Pak Rat as a first-party optional app.

Stock MLP1 firmware does not ship `python3`, while upstream `PortMaster.sh`
executes Python scripts such as `pugwash` and `harbourmaster`. The manager
therefore treats the PortMaster UI Python runtime as a separate
generated/downloaded artifact installed at
`$USERDATA_PATH/portmaster/runtime`.

The managed runtime supplies CPython and libraries that upstream `pugwash`
needs, including liblzma for `NotoSans.tar.xz` extraction. The PortMaster UI
intentionally uses stock MLP1 SDL libraries from `/usr/lib`; the bundled SDL
libraries from `pysdl2-dll` initialized but segfaulted on the device during UI
startup.
