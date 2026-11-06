# WiiCompiled + Retro Rewind for Switch 2 (HBLauncher)

An experimental port of [WiiCompiled](https://github.com/patchzyy/Wiicompiled) with Retro Rewind to
the Nintendo Switch 2, run by ravensclawx's [HBLauncher](https://github.com/ravensclawx/HBLauncher).
The game is a static `aarch64-linux-musl` program that HBLauncher's Linux personality (ABI 3)
loads into the SmileBASIC process that ns2jb opens.

> [!IMPORTANT]
> **This is a proof of concept, shared as is.** I made it for fun and may not be able to help with
> problems, but anyone is welcome to fork it and carry it further; if someone does, I'll link their
> fork here.

> [!NOTE]
> **Made with AI assistance.** Most of this port's code was written with Claude Code. I tested
> every build on my own console and decided what went in.

This repository contains no game files, disc images, Retro Rewind files, saves or built packages.
You build everything yourself from your own PAL (`RMCP01`) disc and your own Retro Rewind folder.
A built package contains translated game code, so please don't share one either.

## Status

This is an experimental proof of concept for the homebrew tools available in November 2026
(HBLauncher `18b4748`, ns2jb `81d5bb1`). Newer homebrew launchers will likely make this way of
running it obsolete; if that happens, I'll point to them here and archive this repository. The
WiiCompiled fixes and the Switch 2 notes in this repository should still be useful to whoever
ports it next.

Only the Retro Rewind version is packaged; plain Mario Kart Wii isn't, and only the PAL disc is
supported.

## What works

Tested on one Switch 2, with the defaults below, a PC on the same home network, and Retro Rewind
6.13.4:

- Retro Rewind boots, and menus, offline races and saves work.
- Offline races run near 60 FPS in lighter stretches (measured at 1.5× resolution); Toad's
  Factory measured 59.4 FPS at 97.7% game speed. It isn't a steady 60 (see known issues).
- Online play on Retro WFC works, including online races.
- Returning to HBLauncher, then launching the game again.

## Known issues

- **A PC has to stay on.** Every file is read from and written to your PC over the network
  through HBLauncher's file server. There's no SD card support and no play away from home, and
  ns2jb needs both the PC and the console on Ethernet.
- **Don't press Plus.** SmileBASIC sees the controller behind the game, and Plus stops the
  SmileBASIC program that keeps the session alive. The game keeps running for about two minutes,
  then the session dies. Pause is on **Minus**, and the port's menu is **L + R + right stick
  click**.
- **Offline isn't a steady 60 FPS.** Full 12-kart races run in the mid 50s in the pack and near 60
  in first place, and busy parts of some detailed custom tracks drop into the low 50s. Offline the
  game draws every frame, so in those moments it also runs slightly slower than normal speed.
- **Change the resolution outside races.** Changing it during a race breaks the graphics until
  you restart the game; changing it in the game's menus works.
- **Online can look choppy.** Online keeps Retro Rewind's frame skip so the game always runs at
  full speed, which means busy moments can drop to about 30 FPS.
- **After a crash, restart SmileBASIC.** The game shares SmileBASIC's process with HBLauncher, so
  memory a crashed game didn't free is still taken. Launching again in the same session can fail
  at start-up or close SmileBASIC with a system error. Close SmileBASIC, open it again, and start
  a new runner and 4/4 session.
- **The first visit to a place stutters** while shaders build; they're cached after that. The
  first boot can wait a few minutes for this.
- Every Retro Rewind update that changes `Code.pul` needs a new `translate`, `build` and `pack`.
- Only built on an Apple Silicon Mac with Docker. The scripts should also work on aarch64 Linux,
  but I haven't tried.
- It may stop working after an HBLauncher, ns2jb, firmware or Retro Rewind update. It was tested
  with HBLauncher `18b4748` and ns2jb `81d5bb1`, patched as in step 2.

## Setup

The first build takes a long time, mostly unattended; the console link alone is about 30 minutes.
This path is for an Apple Silicon Mac with Docker Desktop.

### 1. What you need

- A Mac or Linux PC on Ethernet: ns2jb doesn't support Windows. Check
  [ns2jb's README](https://gitlab.com/kurumuz/ns2jb) for the system software and SmileBASIC
  versions it supports, and do its one-time SmileBASIC setup (saving `bootstrap.bas` as `NS2`).
- [HBLauncher](https://github.com/ravensclawx/HBLauncher) at `18b4748` and
  [ns2jb](https://gitlab.com/kurumuz/ns2jb) at `81d5bb1`, exactly. Step 2 clones and patches them.
  Newer versions haven't been tested with these patches.
- On the Mac: [Docker Desktop](https://www.docker.com/products/docker-desktop/), Git, Python 3,
  [Node.js](https://nodejs.org/) 22 or newer, and [Zig 0.15.2](https://ziglang.org/download/),
  exactly, to rebuild ns2jb's helper. Clang from the Xcode Command Line Tools runs the optional
  ns2jb tests. Everything else (Clang 22, .NET 8, the musl toolchain, HBLauncher's build tools)
  comes in the Docker image, which `docker/run.sh` builds the first time.
- Your own Mario Kart Wii PAL disc (`RMCP01`) and the latest Retro Rewind.
- This repository with its aurora submodule:

  ```sh
  git clone --recursive --branch switch2-hblauncher https://github.com/6rimson/WiiCompiled-Switch2.git
  cd WiiCompiled-Switch2
  ```

  Run every command below from this folder.

### 2. Prepare HBLauncher and ns2jb

Upstream HBLauncher asks ns2jb to map memory ranges (commands `0x524f001a`–`0x524f001c`) that ns2jb
`81d5bb1` doesn't implement, so a launch fails with `Unknown process probe`. This port patches
both sides, and one script prepares them:

```sh
hblauncher/tools/setup-session.py --hblauncher ~/hbl/HBLauncher --ns2jb ~/hbl/ns2jb --zig /path/to/zig
```

It clones any folder that doesn't exist yet and checks out the pinned commit, then:

- applies `patches/hblauncher-host-range-mapping.diff` to HBLauncher;
- writes the inspectors and HID signatures the launcher needs into
  `reference/hblauncher-cpu-sdk/`, taken from HBLauncher's own history (commit `9d00834`, MIT);
- applies `patches/ns2jb-range-mapping.diff` to ns2jb, rebuilds its native helper with Zig,
  checks it against the pinned SHA-256, and updates ns2jb's `SHA256SUMS`.

Running it again changes nothing. A folder at another commit, or with local edits that overlap
a patch, stops the script with an explanation; it never resets or discards anything. `--check`
only reports. After the helper changes, start a fresh SmileBASIC session and runner: an old run
folder keeps the helper it started with.

Then build HBLauncher's launcher, demo apps and hardware probe in Docker (HBLauncher's own
`tools/build-all.sh`; about two minutes). It writes only build output inside the HBLauncher folder:

```sh
HBL_ROOT=~/hbl/HBLauncher hblauncher/docker/run.sh launcher
```

The launcher built this way is byte-identical to the one used on the console
(`launcher/build/code.bin` SHA-256 `222ec59b…`). A different hash doesn't mean it's broken
(a newer compiler can change it), but it is then a launcher nobody has tested with this port.

Optional: the tests below check the patched PC-side code on your Mac, without a console. A session
doesn't need them; they're for confirming a setup or checking a change. To run the ones that
come with the patches:

```sh
cd /path/to/ns2jb
clang -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined tests/hbl-range-native.c -o /tmp/hbl-range-native
/tmp/hbl-range-native                                    # 50 mapping scenarios
node --test tests/hbl-wire.test.cjs $(git ls-files '*.test.cjs')
cd /path/to/HBLauncher && node --test tests/host/range-safety.test.cjs
```

HBLauncher's own host tests (ABI, packages, launcher lifecycle) need its built launcher and the
Vulkan headers, which the Docker image has:

```sh
docker run --rm -v ~/hbl/HBLauncher:/hbl -w /hbl wiicompiled-hbx-build:<tag> node tests/host/lifecycle.test.cjs
```

`docker images wiicompiled-hbx-build` shows the tag. Run it before installing the game, since
the test expects only HBLauncher's own apps.

### 3. Put your files in place

Use the same paths as a desktop WiiCompiled build; [`docs/building-macos.md`](../docs/building-macos.md)
shows how to extract the disc and get Retro Rewind and the online payload:

- `Assets/main.dol`, `Assets/StaticR.rel` and `Assets/DATA` (`files/` + `sys/`) from your disc;
- `PulsarPacks/completed/RetroRewind/RetroRewind6`: your Retro Rewind folder;
- for online play, `build/retro-wfc/binary/payload.RMCPD00.bin` (the Retro-WFC payload).

### 4. Build

Each step runs in a Docker container. Build state stays in the Docker volume `wiicompiled-hbx`
(set `HBX_VOLUME` to use another):

```sh
export HBL_ROOT=/path/to/HBLauncher
hblauncher/docker/run.sh toolchain            # patched musl toolchain (about 7 min, once)
hblauncher/docker/run.sh deps                 # Dawn + SDL3 at their pins, patched; SDK copy
hblauncher/docker/run.sh runtime              # libhbxl.a
hblauncher/docker/run.sh translate --online   # base game + Retro Rewind (leave out --online for offline only)
hblauncher/docker/run.sh build                # console link (about 30 min)
hblauncher/docker/run.sh pack                 # -> hblauncher/dist/wiicompiled-rr
```

The console needs your PC's address. On macOS, `serve` finds it and writes it into the installed
app, so nothing else is needed. On Linux, set `HBX_FS_HOST=<the PC's IPv4>` for `build`, or leave
it unset if you use HBLauncher's wired link at `10.42.0.1`.

### 5. Install

```sh
python3 hblauncher/build.py install
python3 hblauncher/build.py fsroot --nand /path/to/NAND --rr-save /path/to/rksys.dat
node hblauncher/tools/check-package.cjs "$HBL_ROOT/apps/wiicompiled-rr"
```

`--nand` and `--rr-save` are optional: a NAND folder from a desktop
WiiCompiled build brings your Miis, region and online profile, and `rksys.dat` brings your
Retro Rewind save.

`fsroot` creates `hblauncher/fsroot/`, the folder the console sees. It holds your saves and
console data, is ignored by Git, and should never be shared:

| Path | Contents |
|---|---|
| `portable.txt`, `UserData/Config.toml` | settings; NAND, caches and logs also stay in `UserData/` |
| `UserData/NAND/` | from `--nand`: SYSCONF, `setting.txt` (console identity; Retro WFC links your profile to it), Miis, WFC login data |
| `disc/DATA/` | your extracted disc |
| `RetroRewind/RetroRewind6/` | your Retro Rewind folder |
| `RetroRewind/riivolution/save/RetroWFC/RMCP/rksys.dat` | Retro Rewind's save, from `--rr-save` |
| `dsp_coef.bin`, `wii_bootstrap/`, `cacert.pem` | runtime resources from `runtime/assets` (not the starter pipeline cache: building it all at start-up runs out of memory on the console) |
| `hbxl-env` | options read at launch (see below) |
| `icon.png` | optional launcher icon; add your own |

### 6. Play

Create `hblauncher/fsroot/session.env` with the two checkouts from step 2:

```sh
HBL_ROOT=/path/to/HBLauncher
HBL_NS2JB=/path/to/ns2jb
```

Then run each part in its own terminal, in this order:

```sh
hblauncher/tools/session.sh server     # file server; its log goes to hblauncher/fsroot/fs-run.log
hblauncher/tools/session.sh runner     # ns2jb's runner: start SmileBASIC and bring the session to 4/4
hblauncher/tools/session.sh launcher   # HBLauncher; pick Retro Rewind
```

Keep the server and runner running while you play. While the runner is alive, `launcher` reuses
the open session without a new 4/4. HBLauncher writes its own log to
`$HBL_ROOT/build/hbl-session.log`.

## Options

The port's menu (**L + R + right stick click**) has the main switches. `hblauncher/fsroot/hbxl-env`
sets their starting values, one `NAME=value` per line:

| Option | Default | What it does |
|---|---|---|
| `MKW_CPU_BOOST=N` | `1` | CPU boost mode at launch (about 1534 MHz instead of 1107). `0` turns it off. |
| `MKW_RACE_WORKER=0` | on | Starts with the race GX worker off. The worker records graphics commands on the game thread and replays them on another core during offline races. |
| `MKW_FRAME_SKIP=1` | off | Starts with Retro Rewind's frame skip on in offline races. Online and outside races it's always on. |
| `MKW_PERF_STATS=1` | off | Logs FPS and frame timing. |
| `AURORA_PIPELINE_THREADS=N` | `2` | Threads that build shaders (0–4; `0` builds on the frame thread). Only 2 has been measured. |
| `HBXL_TRACE=1` | off | Logs every system call the runtime doesn't implement. Very slow; debugging only. |

`UserData/Config.toml` holds the game's settings. Resolution starts at
`resolution_multiplier = 1.0` (854×480); you can raise it, and 1.5 (about 720p) is what the FPS
figures above were measured at. `gx_worker = true` (the default) is needed for the race worker.

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
- **Speed:** in offline races the game draws every tick (Retro Rewind's frame skip is off) and
  the race GX worker replays graphics on its own core. The CPU boost is on at launch.

## Patches

`patches/dawn-hbx-switch2.diff`, `patches/sdl3-hbx-switch2.diff` and
`patches/aurora-hbx-switch2.diff` are ravensclawx's, from the Twilight Princess HBLauncher port,
unchanged. The rest are this port's. Several were added while chasing a problem and were never
tested on their own, so "why" says what was seen at the time, not that the patch is proven needed.

| Patch | Applies to | Why |
|---|---|---|
| `patches/hblauncher-runtime-file-locks.diff` | `sdk/linux/vfs.c` (the Linux runtime linked into the app) | `fcntl` record locks returned `EINVAL`, so SQLite could not open aurora's shader and pipeline caches, and shaders were rebuilt every session. One process owns all files, so the locks are granted. `build.py deps` applies it to its own copy of the SDK. |
| `patches/hblauncher-runtime-fs-host.diff` | `sdk/linux/vfs.c` | The runtime had the file server's address fixed at `10.42.0.1` (HBLauncher's wired link). The link can now set it (`--defsym=HBXL_FS_HOST`), so the console can reach a PC on the home network. `build.py` takes it from `HBX_FS_HOST`. |
| `patches/hblauncher-runtime-getcwd.diff` | `sdk/linux/vfs.c` | `getcwd` checked the wrong argument for the buffer size, so it always failed with `ERANGE` and the game aborted at start-up. |
| `patches/hblauncher-runtime-main-stack.diff` | `sdk/linux/runtime.c` | System services can't read or write the Vulkan-backed pool, and SDK calls keep their request buffers on the caller's stack. It makes sure the main thread's 8 MiB stack comes from the SDK heap, which `OpenDefaultAudioOut` and other services need, whatever the page allocator does. |
| `patches/hblauncher-runtime-file-modes.diff` | `sdk/linux/vfs.c` | `fchmod`, `fchmodat`, `fchownat` and `fchown` returned `ENOSYS`, so `std::filesystem::copy_file` stopped after creating its write-only target. Retro Rewind's save clone then left a 0-byte `banner.bin`, and the game showed a save-memory error when a race started. They now succeed as no-ops, and the runtime no longer passes a file's creation mode to the file server. |
| `patches/hblauncher-runtime-fast-clock.diff` | `sdk/linux/runtime.c` | Every `clock_gettime` was a kernel call (`svc 0x1e`), and races made up to 30,000 per frame. The runtime now uses a direct counter read when `nn::os::GetSystemTick` itself does one, and answers `clock_gettime` first. |
| `patches/hblauncher-runtime-thread-pin.diff` | `sdk/linux/thread.c` | Adds `hbxl_pin_next_thread`, so the GX, render and audio workers get their own cores. |
| `patches/hblauncher-runtime-sockets.diff` | `sdk/linux/net.c` (new), `runtime.c`, `hbxl.h`, `sdk/tools/hbxl-build.py` | Linux socket syscalls on `nnsocket*`, through SDK-heap buffers, with console DNS. Needed for Retro WFC. |
| `patches/hblauncher-runtime-net-fixes.diff` | `sdk/linux/net.c`, `thread.c` | The DNS resolver runs on an SDK-heap stack, because the system service can't read Vulkan-backed memory; skips musl's unused resolver slots; keeps one core free so a thread can always be placed. |
| `patches/hblauncher-runtime-net-nonblock.diff` | `sdk/linux/net.c` | The console's socket API uses Linux's `O_NONBLOCK` (0x800), not BSD's 4, with BSD values for the rest. Non-blocking sockets are also tracked here, so one stuck read can't stop the game. |
| `patches/hblauncher-runtime-net-trace.diff` | `sdk/linux/net.c` | Records the socket call in progress, so the freeze watchdog can show a call that never returned. |
| `patches/aurora-hbx-pipeline-threads.diff` | `aurora-main/lib/gfx/pipeline_cache.cpp` (console build only) | `vkCreateGraphicsPipelines` failed with `VK_ERROR_UNKNOWN` on aurora's pipeline workers, whose stacks were in the runtime's Vulkan-backed pool. aurora now starts two workers with SDK-heap stacks. `AURORA_PIPELINE_THREADS` (0–4; 0 builds on the frame thread) and `AURORA_PIPELINE_STACK=default` in `hbxl-env` change that without a rebuild. `build.py build` applies it after TP's aurora patch. |
| `patches/aurora-hbx-pipeline-retry.diff` | `aurora-main/lib/gx/gx.cpp`, `lib/gfx/pipeline_cache.cpp` (console build only) | The console's driver sometimes refused a GX pipeline (`VK_ERROR_UNKNOWN`, a different one each run), and aurora treated that as fatal. The failure is now caught with a Dawn error scope and logged with the largest free SDK-heap block; that pipeline's draws are skipped, and it's retried up to 5 times with a growing delay. |
| `patches/aurora-hbx-no-boot-prune.diff` | `aurora-main/lib/gfx/pipeline_cache.cpp` | Cache pruning stopped the game for about 1.4 s over the file server, and the cache grows slowly, so it only runs on request. |
| `patches/aurora-hbx-dawn-toggles.diff` | `aurora-main/lib/webgpu/gpu.cpp` (console build only) | Adds Dawn device toggles from `hbxl-env` (`AURORA_DAWN_ENABLE=a,b`, `AURORA_DAWN_DISABLE=a,b`), so driver experiments need no rebuild. Added to try `vulkan_monolithic_pipeline_cache` against early pipeline failures; that toggle made no difference, and none is set by default. |
| `patches/aurora-hbx-device-lock.diff` | `aurora-main/lib/webgpu/gpu.cpp` (console build only) | Requests Dawn's `ImplicitDeviceSynchronization`. Without it Dawn's device has no lock, and aurora calls it from several threads. |
| `patches/aurora-hbx-heap-block.diff`, `patches/dawn-hbx-image-info.diff` | aurora `lib/webgpu/gpu.cpp`, `lib/gfx`; Dawn `TextureVk.cpp` | Sets Dawn's heap block to 512 MiB (`AURORA_HBX_HEAP_BLOCK_MIB`). GPU copies into textures came out empty, apparently in Dawn's later 8 MiB blocks, which left much of the game black; with one large block everything rendered. Why the later blocks failed is still unknown. Also reports probe results per block. |
| `patches/dawn-hbx-no-dedicated.diff` | Dawn `src/dawn/native/vulkan/TextureVk.cpp` (console build only) | Keeps images the driver only prefers to dedicate in Dawn's shared heaps. Added while large textures came out black, before the heap-block cause above was found; not tested without it since. |
| `patches/aurora-hbx-lighting-safe.diff` | `aurora-main/lib/gx/shader.cpp` (console build only) | Makes aurora's integer lighting ignore NaN, Inf and zero-length normals instead of converting them, and takes DriftDroid's mask-and-compare version of the 24-bit indirect texture coordinate helper. Added while chasing black surfaces, before their real cause (the heap blocks above) was found. It's unproven whether the driver needs it; a build without it hasn't been tested. |
| `patches/aurora-hbx-console-input.diff` | aurora `pad.cpp`, `fifo.cpp`, `THPDec.cpp`, `thp.h`, `aurora_thp.cmake` | Physical A/B/X/Y layout, FIFO thread on its own core, and `THPVideoFrameSize` for the native video decoder (both decoder pieces adapted from DriftDroid). |
| `patches/aurora-hbx-fifo-spin.diff`, `patches/aurora-hbx-drain-spin.diff` | aurora `lib/gx/fifo.cpp` | The GX threads have cores of their own, so they poll briefly instead of sleeping, and wake the worker once per batch instead of once per command. This saved hundreds of kernel wake-ups per frame. |
| `patches/aurora-hbx-release-gpu.diff`, `patches/aurora-hbx-device-destroy.diff` | aurora `lib/aurora.cpp`, `lib/webgpu/gpu.cpp` | The game shares its process with HBLauncher. Before returning to the launcher it frees everything on the GPU (leaving the window, input and audio alone), otherwise the next launch ran out of GPU memory. |
| `patches/aurora-hbx-diagnostics.diff`, `patches/aurora-hbx-frame-timing.diff`, `patches/hblauncher-runtime-syscall-counts.diff` | aurora and `sdk/linux/runtime.c` | Counters for the `MKW_PERF_STATS=1` report: frame timing, skipped draws, texture uploads, all-zero uploads and syscalls. |
| `patches/aurora-hbx-texture-debug.diff` | `aurora-main/lib/gx/shader.cpp` (console build only) | Debugging: `AURORA_HBX_DEBUG_TEXTURE=1`, `2` or `3` draws each surface's first, second or third texture directly, bypassing TEV; `=rast` draws its lit vertex colour. |
| `patches/aurora-hbx-bind-diag.diff` | `aurora-main/lib/gx/gx.cpp`, `lib/gx/texture.cpp` (console build only) | Debugging: logs sampled texture slots that are empty, every upload 512 wide or larger, and large textures drawn from an EFB copy. |
| `patches/aurora-hbx-texture-selftest.diff` | `aurora-main/lib/webgpu/gpu.cpp` (console build only) | Debugging: `AURORA_HBX_SELFTEST=1` uploads six texture sizes at start-up, with and without dedicated memory, and logs the readback and sampled results. |
| `patches/aurora-hbx-frame-check.diff` | aurora (console build only) | Debugging: `AURORA_HBX_VERIFY=1` compares uploaded textures with the GPU's copy, and `AURORA_HBX_CAPTURE=1` saves screens and draw lists to `UserData/Captures`. Also pins the render worker. |
| `patches/aurora-hbx-upload-probe.diff`, `patches/dawn-hbx-buffer-log.diff`, `patches/hblauncher-runtime-memory-log.diff` | aurora, Dawn, `sdk/linux/memory.c` | Debugging: `AURORA_HBX_PROBE=1` follows uploaded texture rows through staging, the copy and later use. The logs list memory types and what the staging buffer uses. |
| `patches/hblauncher-host-range-mapping.diff` | HBLauncher `host/lib/target.cjs`, `preflight.cjs`, `host/start.cjs`, `tests/host/range-safety.test.cjs` (the PC side) | Maps the app with ns2jb's range commands, up to 64 MiB per request. Records a source as retained before mapping, tracks partial progress, checks every reply, and after an uncertain failure blocks further use of that session and writes a failure marker instead of retrying. Splits writes into 32 MiB pieces, since the RPC refuses more than 64 MiB per write and this app's code image is about 117 MiB. Prebuilt launches no longer need cross compilers. Applied by `tools/setup-session.py`. |
| `patches/ns2jb-range-mapping.diff` | ns2jb `engine/switch2/ro/worker-control.cjs`, `payload/main.c`, `process.c`, new `hbl-range.h`, `tests/` | Adds map RX, map RW and unmap for page ranges (`0x524f001a`–`0x524f001c`) to the RPC dispatcher and the native helper. The helper checks the caller's identity, bounds, alignment, memory state and permissions, never asks for writable and executable memory together, and rolls back once if setting permissions fails. The exploit and bootstrap are unchanged. Includes 50 native scenarios and protocol tests. Applied, rebuilt and checked by `tools/setup-session.py`. |

The runtime patches are applied by `build.py deps` and `build.py runtime`.

## Other build options

On native aarch64 Linux, run `python3 hblauncher/build.py <action>` directly, with
`HBXL_TOOLCHAIN` pointing at the toolchain.

`build --variant desktop` links stand-ins for HBLauncher's runtime instead
(`desktop/hbx_desktop_stubs.c`). The same code then runs headless on desktop aarch64 Linux,
on Dawn's Null backend, with `SDL_VIDEO_DRIVER=offscreen SDL_AUDIO_DRIVER=dummy`.

## Credits

- **WiiCompiled** by patchzyy and contributors: the recompilation this is built on.
- **HBLauncher** and the **Twilight Princess** and **Animal Crossing** ports by ravensclawx: the
  launcher, Linux runtime and the Dawn, SDL3 and aurora patches this port started from.
- **ns2jb** by kurumuz, which opens the console session; this port adds range mapping to it.
- **DriftDroid** by dorPXP: the Android and Switch 1 port of WiiCompiled. Its native THP video
  decoder and lighting helper are used here, adapted, and its notes and fixes helped throughout.
- **Retro Rewind** and **Retro WFC**, whose files and servers you use with your own copy.
- **aurora**, **Dawn** and **SDL**.

WiiCompiled is GPL-3.0 (see `../LICENSE`), and so is this port. The Twilight Princess patches are
MIT (`patches/LICENSE.tp-integration`). See [THIRD_PARTY.md](THIRD_PARTY.md).
"Nintendo Switch" and "Mario Kart" are trademarks of Nintendo; this project is not affiliated with
or endorsed by Nintendo.
