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
| `patches/hblauncher-runtime-fs-host.diff` | `sdk/linux/vfs.c` | The runtime had the file server's address fixed at `10.42.0.1` (HBLauncher's wired link). The link can now set it (`--defsym=HBXL_FS_HOST`), so the console can reach a PC on the home LAN or Wi-Fi. `build.py` takes it from `HBX_FS_HOST`. |
| `patches/hblauncher-runtime-getcwd.diff` | `sdk/linux/vfs.c` | `getcwd` checked the wrong argument for the buffer size, so it always failed with `ERANGE` and the game aborted at start-up. |
| `patches/hblauncher-runtime-main-stack.diff` | `sdk/linux/runtime.c` | System services can't read or write the Vulkan-backed pool, and SDK calls keep their request buffers on the caller's stack. It makes sure the main thread's 8 MiB stack comes from the SDK heap, which `OpenDefaultAudioOut` and other services need, whatever the page allocator does. |
| `patches/hblauncher-runtime-file-modes.diff` | `sdk/linux/vfs.c` | `fchmod`, `fchmodat`, `fchownat` and `fchown` returned `ENOSYS`, so `std::filesystem::copy_file` stopped after creating its write-only target. Retro Rewind's save clone then left a 0-byte `banner.bin`, and the game showed a save-memory error when a race started. They now succeed as no-ops, and the runtime no longer passes a file's creation mode to the file server. |
| `patches/aurora-hbx-pipeline-threads.diff` | `aurora-main/lib/gfx/pipeline_cache.cpp` (console build only) | `vkCreateGraphicsPipelines` failed with `VK_ERROR_UNKNOWN` on aurora's pipeline workers, whose stacks were in the runtime's Vulkan-backed pool. aurora now starts one worker with an SDK-heap stack. `AURORA_PIPELINE_THREADS` (0–4; 0 builds on the frame thread) and `AURORA_PIPELINE_STACK=default` in `hbxl-env` change that without a rebuild. `build.py build` applies it after TP's aurora patch. |
| `patches/aurora-hbx-pipeline-retry.diff` | `aurora-main/lib/gx/gx.cpp`, `lib/gfx/pipeline_cache.cpp` (console build only) | The console's driver sometimes refuses a GX pipeline (`VK_ERROR_UNKNOWN`, a different one each run), and aurora treated that as fatal. The failure is now caught with a Dawn error scope and logged with the largest free SDK-heap block; that pipeline's draws are skipped, and it's retried up to 5 times with a growing delay. Applied after the pipeline-threads patch. |
| `patches/aurora-hbx-dawn-toggles.diff` | `aurora-main/lib/webgpu/gpu.cpp` (console build only) | Adds Dawn device toggles from `hbxl-env` (`AURORA_DAWN_ENABLE=a,b`, `AURORA_DAWN_DISABLE=a,b`), so driver experiments need no rebuild. Used for `vulkan_monolithic_pipeline_cache`: the console's driver refused every pipeline after about two dozen, with plenty of SDK heap free, and Dawn otherwise keeps one `VkPipelineCache` per pipeline. |
| `patches/aurora-hbx-diagnostics.diff`, `patches/hblauncher-runtime-syscall-counts.diff` | aurora and `sdk/linux/runtime.c` | Counters for the `MKW_PERF_STATS=1` report: skipped draws, texture uploads, all-zero uploads and syscalls. |
| `patches/hblauncher-runtime-fast-clock.diff` | `sdk/linux/runtime.c` | Every `clock_gettime` was a kernel call (`svc 0x1e`), and races made up to 30,000 per frame. The runtime now uses a direct counter read when `nn::os::GetSystemTick` itself does one, and answers `clock_gettime` first. |
| `patches/aurora-hbx-texture-debug.diff` | `aurora-main/lib/gx/shader.cpp` (console build only) | `AURORA_HBX_DEBUG_TEXTURE=1`, `2` or `3` draws each surface's first, second or third texture directly, bypassing TEV; `=rast` draws its lit vertex colour. These find where black surfaces come from. |
| `patches/aurora-hbx-lighting-safe.diff` | `aurora-main/lib/gx/shader.cpp` (console build only) | aurora's integer lighting converted NaN and Inf straight to integers, and `normalize` got zero vectors, neither of which this driver tolerates. Bad terms now contribute nothing. It also takes DriftDroid's 24-bit indirect texture coordinate fix. |
| `patches/aurora-hbx-bind-diag.diff` | `aurora-main/lib/gx/gx.cpp`, `lib/gx/texture.cpp` (console build only) | Logs sampled texture slots that are empty, every upload 512 wide or larger, and large textures drawn from an EFB copy. |
| `patches/dawn-hbx-no-dedicated.diff` | Dawn `src/dawn/native/vulkan/TextureVk.cpp` (console build only) | Keeps images the driver only prefers to dedicate in Dawn's shared heaps. Large textures came out black on the console. `build.py build` applies it to the existing Dawn copy. |
| `patches/aurora-hbx-texture-selftest.diff` | `aurora-main/lib/webgpu/gpu.cpp` (console build only) | `AURORA_HBX_SELFTEST=1` uploads six texture sizes at start-up, with and without dedicated memory, and logs the readback and sampled results. |
| `patches/hblauncher-runtime-thread-pin.diff` | `sdk/linux/thread.c` | Adds `hbxl_pin_next_thread`, so the GX, render and audio workers get their own cores. |
| `patches/aurora-hbx-frame-check.diff` | aurora (console build only) | `AURORA_HBX_VERIFY=1` compares uploaded textures with the GPU's copy. `AURORA_HBX_CAPTURE=1` saves screens and draw lists to `UserData/Captures`. Also pins the render worker. |
| `patches/aurora-hbx-device-lock.diff` | `aurora-main/lib/webgpu/gpu.cpp` (console build only) | Requests Dawn's `ImplicitDeviceSynchronization`. Without it Dawn's device has no lock, and aurora calls it from several threads. |
| `patches/aurora-hbx-upload-probe.diff`, `patches/dawn-hbx-buffer-log.diff`, `patches/hblauncher-runtime-memory-log.diff` | aurora, Dawn, `sdk/linux/memory.c` | `AURORA_HBX_PROBE=1` follows uploaded texture rows through staging, the copy and later use. The logs list memory types and what the staging buffer uses. |
| `patches/aurora-hbx-heap-block.diff`, `patches/dawn-hbx-image-info.diff` | aurora `lib/webgpu/gpu.cpp`, `lib/gfx`; Dawn `TextureVk.cpp` | Sets Dawn's heap block to 512 MiB (`AURORA_HBX_HEAP_BLOCK_MIB`), because GPU copies into textures came out empty, apparently depending on the memory block, and reports probe results per block. |
| `patches/hblauncher-runtime-sockets.diff` | `sdk/linux/net.c` (new), `runtime.c`, `hbxl.h`, `sdk/tools/hbxl-build.py` | Linux socket syscalls on `nnsocket*`, through SDK-heap buffers, with console DNS. Needed for Retro WFC. |
| `patches/aurora-hbx-console-input.diff` | aurora `pad.cpp`, `fifo.cpp`, `THPDec.cpp`, `thp.h`, `aurora_thp.cmake` | Physical A/B/X/Y layout, FIFO thread on its own core, and `THPVideoFrameSize` for the native video decoder. |
| `patches/hblauncher-host-large-writes.diff` | `host/lib/target.cjs` (the PC side of the session) | The RPC refuses more than 64 MiB per write, and the loader writes the whole code image at once. This app's code image is about 127 MiB. **Apply it to the HBLauncher checkout you launch from.** |

The runtime patches are applied by `build.py deps` and `build.py runtime`. All are small and generic, so they could go upstream.

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

If the console reaches the PC at another address than HBLauncher's wired `10.42.0.1` (both on
the home router, for example), set `HBX_FS_HOST=<the PC's IPv4>` for `build`. On macOS, `serve`
then finds the Mac's current address and writes it into the installed app, so a new DHCP lease
needs no rebuild. `tools/session.sh server|runner|launcher` runs each part of a session with that
address.

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
