# WiiCompiled + Retro Rewind for Switch 2 (HBLauncher)

An experimental [HBLauncher](https://github.com/ravensclawx/HBLauncher) package of WiiCompiled with
Retro Rewind. It is a static `aarch64-linux-musl` program run by HBLauncher's Linux personality
(ABI 3). It follows the layout of ravensclawx's Twilight Princess and Animal Crossing ports and
uses their Dawn, SDL3 and aurora patches.

**Hardware status:** none. See [VALIDATION.md](VALIDATION.md) for exactly what has been tested
and on what. A successful build does not establish console compatibility or performance.

This integration adds no disc images, extracted game files, Retro Rewind files or saves. You
supply your own clean PAL `RMCP01` disc and your own `RetroRewind6` folder.

## How it differs from a desktop build

- **Guest memory:** `MKW_GUEST_FLAT_CHECKED_ONLY`. HBLauncher can neither place memory at a fixed
  address nor alias it, so every guest access goes through the checked page table.
- **Fibers:** the BTI-safe AArch64 context switch (`runtime/src/platform/aarch64/co_switch_elf.S`).
  Console code pages are BTI-guarded.
- **Graphics:** Dawn's Vulkan backend on HBLauncher's device, presenting to the `nn::vi` window
  (TP's Dawn and aurora patches). SDL3 uses the offscreen video, `hbx` audio and `hbx` joystick
  drivers (TP's SDL3 patch; HBLauncher sets `SDL_VIDEO_DRIVER`/`SDL_AUDIO_DRIVER`).
- **Files:** every file is read and written over TCP through HBLauncher's file server. The app's
  own folder is the file-server root.

## HBLauncher patches

| Patch | Applies to | Why |
|---|---|---|
| `patches/hblauncher-runtime-file-locks.diff` | `sdk/linux/vfs.c` (the Linux runtime linked into the app) | `fcntl` record locks returned `EINVAL`, so SQLite could not open aurora's shader and pipeline caches; every shader then recompiled on every visit, every session. One process owns all files, so the locks are granted. `build.py deps` applies it to its own copy of the SDK. |
| `patches/hblauncher-host-large-writes.diff` | `host/lib/target.cjs` (the PC side of the session) | The RPC refuses more than 64 MiB per write, and the loader writes the whole code image at once. This app's code image is about 127 MiB. **Apply it to the HBLauncher checkout you launch from.** |

Both are small and generic, so they could go upstream.

## Requirements

- An Apple Silicon Mac with Docker Desktop, or an aarch64 Linux machine (builds run on Linux).
- An HBLauncher checkout at ABI 3 (`18b4748` or newer), set as `HBL_ROOT`.
- Your inputs, at the same paths as a desktop build (see `docs/building-macos.md`):
  - `Assets/main.dol`, `Assets/StaticR.rel` and `Assets/DATA` (`files/` + `sys/`) from your PAL disc;
  - `PulsarPacks/completed/RetroRewind/RetroRewind6`: your Retro Rewind folder;
  - for online play, `build/retro-wfc/binary/payload.RMCPD00.bin` (Retro-WFC payload).

## Build (container)

```sh
export HBL_ROOT=/path/to/HBLauncher
hblauncher/docker/run.sh toolchain            # patched musl toolchain (about 7 min, once)
hblauncher/docker/run.sh deps                 # Dawn + SDL3 at their pins, patched; SDK copy
hblauncher/docker/run.sh runtime              # libhbxl.a
hblauncher/docker/run.sh translate --online   # base game + Retro Rewind (omit --online for offline)
hblauncher/docker/run.sh build                # console link (HBLauncher runtime + hbxl.ld)
hblauncher/docker/run.sh pack                 # -> hblauncher/dist/wiicompiled-rr
```

On native aarch64 Linux, run `python3 hblauncher/build.py <action>` directly, with
`HBXL_TOOLCHAIN` pointing at the toolchain.

`build --variant desktop` links stand-ins for HBLauncher's runtime instead
(`desktop/hbx_desktop_stubs.c`). The same code then runs headless on desktop aarch64 Linux,
on Dawn's Null backend, with `SDL_VIDEO_DRIVER=offscreen SDL_AUDIO_DRIVER=dummy`.

Every Retro Rewind update that changes `Code.pul` needs `translate`, `build` and `pack` again.
The `RetroRewind6` folder on the file server must always match the build.

## Install and serve (the PC that runs the session)

```sh
git -C "$HBL_ROOT" apply "$PWD/hblauncher/patches/hblauncher-host-large-writes.diff"
python3 hblauncher/build.py install           # copies dist/wiicompiled-rr into $HBL_ROOT/apps
python3 hblauncher/build.py fsroot --nand /path/to/NAND --rr-save /path/to/rksys.dat
python3 hblauncher/build.py serve             # 10.42.0.1:39503
```

`fsroot` creates `hblauncher/fsroot/`, a private folder that holds your saves; never commit or
share it. It contains:

| Path | Contents |
|---|---|
| `portable.txt`, `UserData/Config.toml` | portable mode: config, NAND, caches and logs stay in `UserData/` |
| `UserData/NAND/` | copied from `--nand` if given: SYSCONF (flag/globe), `title/00000001/00000002/data/setting.txt` (console identity; RWFC links your profile to it), Miis, DWC login data |
| `disc/DATA/` | your extracted disc (copy-on-write clone where the filesystem supports it) |
| `RetroRewind/RetroRewind6/` | your Retro Rewind folder |
| `RetroRewind/riivolution/save/RetroWFC/RMCP/rksys.dat` | Retro Rewind's save, copied from `--rr-save` if given |
| `dsp_coef.bin`, `initial_pipeline_cache.db`, `wii_bootstrap/`, `cacert.pem` | runtime resources from `runtime/assets` |
| `hbxl-env` | environment for the runtime; add `HBXL_TRACE=1` to log unimplemented syscalls |

Keep the file server running, establish a verified HBLauncher session, and pick **Retro Rewind**.

## Sources and credits

WiiCompiled is GPL-3.0 (see `../LICENSE`). The Dawn, SDL3 and aurora patches are from
ravensclawx's Twilight Princess HBLauncher port, MIT-licensed (`patches/LICENSE.tp-integration`).
See [THIRD_PARTY.md](THIRD_PARTY.md).
