# Third-party notices (HBLauncher package)

- **[WiiCompiled](https://github.com/patchzyy/Wiicompiled)** by patchzyy and contributors (GPLv3):
  this port is a modified version of it. The port's changes date from October 2026 onward and are
  also under GPLv3.
- **[HBLauncher](https://github.com/ravensclawx/HBLauncher)** by RavensClawX and contributors (MIT;
  the same notice is in `patches/LICENSE.tp-integration`): the Linux runtime (`sdk/linux`) linked
  into the app, its linker script and packer, and the host tools. They are used from your
  HBLauncher checkout, not bundled. `patches/hblauncher-*.diff` are changes to it made by this
  port. `tools/setup-session.py` also writes the launcher's inspectors and HID signatures into
  your checkout from HBLauncher's own history (commit `9d00834`); they are not copied into this
  repository.
- **[DriftDroid](https://github.com/dorPXP/driftdroid)** by dorPXP and contributors (GPLv3): the
  native THP video decoder in `runtime/src/hle/thp.cpp` and the `THPVideoFrameSize` helper in
  `patches/aurora-hbx-console-input.diff`, both adapted from DriftDroid's Switch build, and the
  mask-and-compare `tev_s24_fixed_vec2i` helper in `patches/aurora-hbx-lighting-safe.diff`.
- **[Twilight Princess HBLauncher integration](https://github.com/ravensclawx/TwilightPrincess-HBLauncher)**
  by RavensClawX and contributors (MIT; see `patches/LICENSE.tp-integration`):
  `patches/dawn-hbx-switch2.diff`, `patches/sdl3-hbx-switch2.diff`,
  `patches/aurora-hbx-switch2.diff` and `cmake/config.mak.example`, unchanged. The Clang toolchain
  file, `build.py` and `docker/` follow that port's layout.
- **Dawn**, **SDL3**, **musl**, **GCC**, **binutils**, **musl-cross-make**, **LLVM/Clang** and the
  **.NET SDK** are external dependencies with their own licenses. They are fetched at build time
  and their binaries are not included in this repository.
- **[ns2jb](https://gitlab.com/pepsipu/ns2jb)**, originally by Sammy Hajhamid (pepsipu), in
  [kurumuz's fork](https://gitlab.com/kurumuz/ns2jb), starts the console session. It is used from
  your own checkout. `patches/ns2jb-range-mapping.diff` is this port's change to it (range mapping
  in the RPC dispatcher and native helper, with tests); like any patch it quotes a few surrounding
  lines of ns2jb. ns2jb's repository has no license file.
- **[Retro Rewind](https://wiki.tockdom.com/wiki/Retro_Rewind)** by ZPL and team is read from your
  own folder, and the **[Retro WFC](https://rwfc.net)** payload is downloaded when you set up the
  build. Neither is included in this repository.

This package contains no game disc images, extracted game files, Retro Rewind files or saves.
"Nintendo Switch 2" and "Mario Kart Wii" are trademarks of Nintendo; this project is not affiliated
with or endorsed by Nintendo.
