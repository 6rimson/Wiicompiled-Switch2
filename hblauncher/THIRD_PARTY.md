# Third-party notices (HBLauncher package)

- **HBLauncher** by ravensclawx and contributors (MIT): the Linux runtime (`sdk/linux`) linked into
  the app, its linker script and packer, and the host tools. They are used from your HBLauncher
  checkout, not bundled. `patches/hblauncher-*.diff` are changes to it made by this port.
- **Twilight Princess HBLauncher integration** by RavensClawX and contributors (MIT; see
  `patches/LICENSE.tp-integration`): `patches/dawn-hbx-switch2.diff`,
  `patches/sdl3-hbx-switch2.diff`, `patches/aurora-hbx-switch2.diff` and
  `cmake/config.mak.example`, unchanged. The Clang toolchain file, `build.py` and `docker/` follow
  that port's layout.
- **Dawn**, **SDL3**, **musl**, **GCC**, **binutils**, **musl-cross-make**, **LLVM/Clang** and the
  **.NET SDK** are external dependencies with their own licenses. They are fetched at build time
  and their binaries are not included in this repository.

This package contains no game disc images, extracted game files, Retro Rewind files or saves.
"Nintendo Switch" and "Mario Kart" are trademarks of Nintendo; this project is not affiliated
with or endorsed by Nintendo.
