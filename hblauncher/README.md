# WiiCompiled + Retro Rewind for Switch 2 (HBLauncher)

An experimental port of [WiiCompiled](https://github.com/patchzyy/Wiicompiled) with Retro Rewind for
the Nintendo Switch 2, run by ravensclawx's [HBLauncher](https://github.com/ravensclawx/HBLauncher).
The game is a static `aarch64-linux-musl` program that HBLauncher's Linux personality (ABI 3)
loads into the SmileBASIC process that ns2jb opens.

> [!IMPORTANT]
> **This is an experimental project, shared as is.** I'd like to keep improving its
> performance and fixing its issues, but nothing is guaranteed. Newer, more polished homebrew menus
> and tools will likely make this way of running the game outdated. Anyone is welcome to fork it and
> carry it further; if someone does, I'll link their fork here.

> [!NOTE]
> **Made with AI assistance.** Most of this port's code was written with Claude Code. I tested
> every build on my own console and decided what went in.

This repository contains no game files, disc images, Retro Rewind files, saves or built packages.
You build everything yourself from your own PAL (`RMCP01`) disc and your own Retro Rewind folder.
A built package contains translated game code, so please don't share one either.

## Status

This is an experimental project built on the homebrew tools available in November 2026
(HBLauncher `18b4748`, ns2jb `81d5bb1`). Newer homebrew launchers will likely make this way of
running it obsolete; if that happens, I'll point to them here and archive this repository. The
WiiCompiled fixes and the Switch 2 notes in this repository should still be useful to whoever
ports it next.

The main build is Retro Rewind. Plain Mario Kart Wii (WiiCompiled without Retro Rewind) also
builds and runs, but it's **experimental**: it's slower in races, for the reason under known issues.
Only the PAL disc is supported.

## What works

Tested on one Switch 2, with the defaults below, a PC on the same home network, and Retro Rewind
6.13.4:

- Retro Rewind boots, and menus, offline races and saves work.
- Offline races run near 60 FPS in lighter stretches (measured at 1.5× resolution); Toad's
  Factory measured 59.4 FPS at 97.7% game speed. It isn't a steady 60 (see known issues).
- Online play on Retro WFC works, including online races.
- Returning to HBLauncher, then launching the game again.
- Plain Mario Kart Wii (experimental) boots from a new NAND and runs offline races, more slowly.

## Known issues

- **A PC has to stay on.** Every file is read from and written to your PC over the network through HBLauncher's file server. There's no SD card support and no play away from home, and ns2jb needs both the PC and the console on Ethernet.
- **Don't press Plus.** SmileBASIC sees the controller behind the game, and Plus stops the SmileBASIC program that keeps the session alive. The game keeps running for about two minutes, then the session dies. Pause is on **Minus**, and the port's menu is **L + R + right stick click**.
- **Offline isn't a steady 60 FPS.** Full 12-kart races run in the mid 50s in the pack and near 60 in first place, and busy parts of some detailed custom tracks drop into the low 50s. Offline the game draws every frame, so in those moments it also runs slightly slower than normal speed.
- **Change the resolution outside races.** Changing it during a race breaks the graphics until you restart the game; changing it in the game's menus works.
- **Online can look choppy.** Online keeps Retro Rewind's frame skip so the game always runs at full speed, which means busy moments can drop to about 30 FPS.
- **After a crash, restart SmileBASIC.** The game shares SmileBASIC's process with HBLauncher, so memory a crashed game didn't free is still taken. Launching again in the same session can fail at start-up or close SmileBASIC with a system error. Close SmileBASIC, open it again, and start a new runner and 4/4 session.
- **Plain Mario Kart Wii is slower in races.** Busy stretches, such as Mario Circuit's countdown, lock to 30 FPS and the game slows down. Both games need about the same work per frame there (16–18 ms against a 16.7 ms budget). Retro Rewind runs its own frame loop, where this port draws every tick, so a frame that is slightly late just shows late: Mario Circuit averaged 57 FPS. Plain Mario Kart Wii waits for the next screen refresh after each frame, as on a Wii, so a frame 1 ms late waits a whole extra refresh (33 ms): it averaged 50 FPS, with long stretches at 30. Fixing it means changing how the runtime paces screen refreshes for plain Mario Kart Wii.
- **The first visit to a place stutters** while shaders build; they're cached after that. The first boot can wait a few minutes for this.
- **Retro Rewind updates need a rebuild.** Every update that changes `Code.pul` needs a new `translate`, `build` and `pack`.
- **Only built on an Apple Silicon Mac.** The scripts should also work on aarch64 Linux, but I haven't tried.
- **Updates elsewhere can break it.** It may stop working after an HBLauncher, ns2jb, firmware or Retro Rewind update. It was tested with HBLauncher `18b4748` and ns2jb `81d5bb1`, patched as in step 2.

## Setup

This guide is for people comfortable typing commands in a terminal. If you've never done that, this
port will be hard going; an easier homebrew launcher may come along later. Expect an hour or two for
the first build and setup: most of it is the build running on its own, and getting the console
session working can take a few tries. Everything here is for an Apple Silicon Mac with Docker
Desktop.

The examples keep everything in one folder, `~/Switch2`:

| Path | What it is |
|---|---|
| `~/Switch2/WiiCompiled-Switch2` | this repository |
| `~/Switch2/HBLauncher` | HBLauncher, created in step 2 |
| `~/Switch2/ns2jb` | ns2jb, created in step 2 |
| `~/Switch2/zig` | Zig 0.15.2, from step 1 |
| `~/Switch2/nodtool` | nodtool, the disc extractor (a single program file), from step 3 |

You can use other locations; just use the same ones in every step.

### 0. Get this repository

```sh
mkdir -p ~/Switch2 && cd ~/Switch2
git clone --recursive --branch switch2-hblauncher https://github.com/6rimson/WiiCompiled-Switch2.git
cd WiiCompiled-Switch2
```

`--recursive` also fetches aurora, the graphics library WiiCompiled includes. Run every command
below from this folder.

### 1. What you need

- **A Mac or Linux PC on Ethernet**, on the same network as the console; ns2jb doesn't support
  Windows, and the console must be on Ethernet too. Check
  [ns2jb's README](https://gitlab.com/kurumuz/ns2jb) for the system software and SmileBASIC versions
  it supports, and do its one-time SmileBASIC setup: typing `bootstrap.bas` into SmileBASIC and
  saving it as `NS2` (a USB keyboard helps).
- **On the Mac:** [Docker Desktop](https://www.docker.com/products/docker-desktop/), Git, **Python 3**
  and [Node.js](https://nodejs.org/) 22 or newer. Git and Python 3 come with Apple's Command Line
  Tools (`xcode-select --install`). Check them with `git --version`, `python3 --version` and
  `node --version`. Everything else (Clang 22, .NET 8, the musl toolchain, HBLauncher's build
  tools) comes in a Docker image that `docker/run.sh` builds the first time. The build was tested with Docker Desktop's memory set to 8 GB (Settings →
  Resources); less may fail partway.
- **Zig 0.15.2, exactly (required).** ns2jb runs a small helper program on the console; this port's
  ns2jb patch changes that helper, so it has to be rebuilt, and Zig is the compiler ns2jb uses for
  it. The setup in step 2 stops without it. From [ziglang.org/download](https://ziglang.org/download/),
  get **0.15.2 → macOS aarch64** (`zig-aarch64-macos-0.15.2.tar.xz`), then:

  ```sh
  cd ~/Switch2
  tar -xf ~/Downloads/zig-aarch64-macos-0.15.2.tar.xz
  mv zig-aarch64-macos-0.15.2 zig
  xattr -dr com.apple.quarantine zig   # stops macOS from blocking a downloaded program
  zig/zig version                      # should print 0.15.2
  cd WiiCompiled-Switch2
  ```

- **Your own Mario Kart Wii PAL disc (`RMCP01`)** and the latest Retro Rewind.
- **Optional:** Clang from the Xcode Command Line Tools, for ns2jb's tests in step 2.

HBLauncher (`18b4748`) and ns2jb (`81d5bb1`) are cloned at those exact versions in step 2. Newer
versions haven't been tested with this port's patches.

### 2. Prepare HBLauncher and ns2jb

These patches let HBLauncher load executable code and writable app data into the running
SmileBASIC process. They provide the missing loader compatibility; the game-specific runtime fixes
are documented separately below.

HBLauncher at our pinned commit `18b4748` asks ns2jb to map memory ranges (commands
`0x524f001a`–`0x524f001c`) that ns2jb `81d5bb1` doesn't implement, so a launch fails with
`Unknown process probe`. This port patches both sides, and one script prepares them:

```sh
cd ~/Switch2/WiiCompiled-Switch2
hblauncher/tools/setup-session.py --hblauncher ~/Switch2/HBLauncher --ns2jb ~/Switch2/ns2jb --zig ~/Switch2/zig/zig
```

It clones any folder that doesn't exist yet and checks out the pinned commit, then:

- applies `patches/hblauncher-host-range-mapping.diff` to HBLauncher;
- writes the inspectors and HID signatures the launcher needs into
  `reference/hblauncher-cpu-sdk/`, taken from HBLauncher's own history (commit `9d00834`, MIT);
- applies `patches/ns2jb-range-mapping.diff` to ns2jb, rebuilds its native helper with Zig,
  checks it against the pinned SHA-256, and updates ns2jb's `SHA256SUMS`.

**You should see** it end with `Ready.` and two lines to put in `session.env` (step 6). Running it
again changes nothing. A folder at another commit, or with local edits that overlap a patch, stops
the script with an explanation; it never resets or discards anything. `--check` only reports.
After the helper changes, start a fresh SmileBASIC session and runner: an old run folder keeps the
helper it started with.

Next, **build HBLauncher's launcher (required).** The launcher is the menu HBLauncher shows on the
console. HBLauncher's repository has only its source code, so it has to be built once. This
command does it in Docker, using HBLauncher's own build script (`tools/build-all.sh`):

```sh
HBL_ROOT=~/Switch2/HBLauncher hblauncher/docker/run.sh launcher
```

`HBL_ROOT=…` tells the script which HBLauncher folder to build. It takes about two minutes, plus a
few more the first time while the Docker image builds, and it only adds build output inside
`~/Switch2/HBLauncher`. **You should see** it end with `build complete`. The launcher built this way
is byte-identical to the one used on the console (`launcher/build/code.bin` SHA-256 `222ec59b…`). A
different hash doesn't mean it's broken (a newer compiler can change it), but it is then a launcher
nobody has tested with this port.

#### Optional: check the patches

**Skip this if you just want to play.** These tests run on your Mac, without a console, and check
that the patched PC-side code behaves as expected. They're useful if something seems wrong, or if
you change the patches. The tests that come with this port's ns2jb and HBLauncher patches need
Clang:

```sh
cd ~/Switch2/ns2jb
clang -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined tests/hbl-range-native.c -o /tmp/hbl-range-native
/tmp/hbl-range-native                                    # 50 mapping scenarios
node --test tests/hbl-wire.test.cjs $(git ls-files '*.test.cjs')
cd ~/Switch2/HBLauncher && node --test tests/host/range-safety.test.cjs
cd ~/Switch2/WiiCompiled-Switch2
```

HBLauncher's own tests check its PC side: its app format, its package loading, and a full
launch-and-return cycle against a simulated console. They need the launcher you just built and the
Vulkan headers, which the Docker image has, so they run in Docker:

```sh
docker run --rm -v ~/Switch2/HBLauncher:/hbl -w /hbl wiicompiled-hbx-build:<tag> node tests/host/lifecycle.test.cjs
```

Replace `<tag>` with the tag `docker images wiicompiled-hbx-build` shows. Run it before installing
the game in step 5, since the test expects only HBLauncher's own apps.

### 3. Put your files in place

These are the same files a desktop WiiCompiled build uses, in the same places. The commands come
from WiiCompiled's [macOS guide](../docs/building-macos.md), which has more detail. They use paths
inside this repository (`Assets/`, `PulsarPacks/`, `build/`), so run them from
`~/Switch2/WiiCompiled-Switch2`; in a new terminal, start with:

```sh
cd ~/Switch2/WiiCompiled-Switch2
```

**Your disc.** Extract it with [nodtool](https://github.com/encounter/nod/releases) (any of `.iso`,
`.wbfs`, `.rvz` and similar works):

```sh
curl -fsSL "https://github.com/encounter/nod/releases/download/v2.0.0-alpha.10/nodtool-macos-arm64" -o ~/Switch2/nodtool
chmod +x ~/Switch2/nodtool
xattr -d com.apple.quarantine ~/Switch2/nodtool 2>/dev/null
mkdir -p /tmp/mkw-extract Assets/DATA
~/Switch2/nodtool extract /path/to/RMCP01.iso /tmp/mkw-extract
cp /tmp/mkw-extract/*/sys/main.dol Assets/main.dol
cp /tmp/mkw-extract/*/files/rel/StaticR.rel Assets/StaticR.rel
cp -R /tmp/mkw-extract/*/files Assets/DATA/files
cp -R /tmp/mkw-extract/*/sys Assets/DATA/sys
rm -rf /tmp/mkw-extract
shasum -a 256 Assets/main.dol Assets/StaticR.rel
```

**You should see** these two hashes, which mean a clean PAL disc:

- `Assets/main.dol`: `80d18895b39c63bd80f457398bfcbb91b7d16ac116a41a88967e954080155b05`
- `Assets/StaticR.rel`: `16d9d146112541fefea701ecb5bc1a496f9d50e4a752fbb5b6778e7c6399f67d`

**Retro Rewind.** Get the latest [Retro Rewind](https://wiki.tockdom.com/wiki/Retro_Rewind) release
(for example with [Wheel Wizard](https://github.com/TeamWheelWizard/WheelWizard)), and copy its
`RetroRewind6` folder, the one that contains `Binaries/Code.pul`:

```sh
mkdir -p PulsarPacks/completed/RetroRewind
cp -R /path/to/RetroRewind6 PulsarPacks/completed/RetroRewind/
```

**Online play (optional).** Download the Retro-WFC payload; the build checks its signature:

```sh
mkdir -p build/retro-wfc/binary
curl -fsSL --retry 3 "https://rwfc.net/api/wfc/payload?g=RMCPD00" -o build/retro-wfc/binary/payload.RMCPD00.bin
```

Plain Mario Kart Wii needs only the disc.

### 4. Build

Make sure Docker Desktop is running. Each step runs in a Docker container, and build state stays
in the Docker volume `wiicompiled-hbx` (set `HBX_VOLUME` to use another):

```sh
cd ~/Switch2/WiiCompiled-Switch2
export HBL_ROOT=~/Switch2/HBLauncher
hblauncher/docker/run.sh toolchain            # patched musl toolchain (about 7 min, once)
hblauncher/docker/run.sh deps                 # Dawn + SDL3 at their pins, patched; SDK copy
hblauncher/docker/run.sh runtime              # libhbxl.a
hblauncher/docker/run.sh translate --online   # base game + Retro Rewind (leave out --online for offline only)
hblauncher/docker/run.sh build                # console link (up to 30 min)
hblauncher/docker/run.sh pack                 # -> hblauncher/dist/wiicompiled-rr
```

**You should see** `pack` end with `Package: …/hblauncher/dist/wiicompiled-rr`. Running a failed step
again is safe. In a new terminal, run the `export` line again before continuing.

That builds Retro Rewind. To build plain Mario Kart Wii (experimental) instead, use the same steps
with `--product wiicompiled` and without `--online`; see
[Plain Mario Kart Wii](#plain-mario-kart-wii-experimental) below.

You don't need to set your Mac's network address anywhere. The game reads its files from your Mac,
so it needs that address, but each time you start the file server (step 6) it looks up your Mac's
current address and writes it into the installed game. That keeps working even when your router
gives the Mac a new address.

### 5. Install

In the same terminal as step 4 (it needs `HBL_ROOT`):

```sh
cd ~/Switch2/WiiCompiled-Switch2
python3 hblauncher/build.py install
python3 hblauncher/build.py fsroot --nand /path/to/NAND --rr-save /path/to/rksys.dat
node hblauncher/tools/check-package.cjs "$HBL_ROOT/apps/wiicompiled-rr"
```

**You should see** the last command print `loadPackage: ok` and `mock lifecycle: ok`.

`--nand` and `--rr-save` are optional. Without them the game starts like a new Wii: a new console
identity, no Miis and no save.

**Bringing your saves:**

- `--nand` copies a Wii NAND folder: Miis, region and flag (SYSCONF), the console identity in
  `title/00000001/00000002/data/setting.txt` (Retro WFC links your online profile to it) and
  WFC login data. Plain Mario Kart Wii keeps its save in the NAND too, at
  `title/00010004/524d4350/data/rksys.dat`. Tested with the `UserData/NAND` folder of a desktop
  WiiCompiled build; a Dolphin NAND should have the same layout but hasn't been tried.
- `--rr-save` copies Retro Rewind's own save, `rksys.dat`, from
  `riivolution/save/RetroWFC/RMCP/` next to your Retro Rewind folder (on a desktop build, an SD card
  or in Dolphin).
- Both are copies: the console writes to the copies in `hblauncher/fsroot/`, never to the
  originals. Copy them back yourself if you want progress made on the Switch 2 elsewhere.

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

Create `hblauncher/fsroot/session.env` with the two lines `setup-session.py` printed, for example:

```sh
HBL_ROOT=$HOME/Switch2/HBLauncher
HBL_NS2JB=$HOME/Switch2/ns2jb
```

Each session goes like this; open three terminal windows in `~/Switch2/WiiCompiled-Switch2`:

1. **On the console:** restart it, open SmileBASIC and go to Direct mode.
2. **Terminal 1, the file server:**

   ```sh
   hblauncher/tools/session.sh server
   ```

   The first time, macOS may ask whether Python may accept incoming network connections: choose
   **Allow**, or the console can't reach your files. Its log goes to `hblauncher/fsroot/fs-run.log`.
3. **Terminal 2, ns2jb's runner:**

   ```sh
   hblauncher/tools/session.sh runner
   ```

   Allow Node's incoming connections too if macOS asks. The runner prints a `V=START(...)` line.
4. **In SmileBASIC,** enter these one at a time, confirming the load when asked:

   ```basic
   LOAD "TXT:NS2"
   RUN
   REMOVE NSR,&H60000000,&H3FFFFFFF
   ```

   An **Out of memory** message after `REMOVE` is expected. Then type the `V=START(...)` line from
   the runner exactly. Wait until the runner reports **4/4 verified**. This is the exploit stage
   and doesn't always succeed; if it fails, restart the console and try again. ns2jb's README has
   the details.
5. **Terminal 3, HBLauncher:**

   ```sh
   hblauncher/tools/session.sh launcher
   ```

   HBLauncher appears on the console; pick **Retro Rewind**. The first boot waits a few minutes on
   a shader screen (L + R + right stick opens the settings there), then the game starts.

Keep the server and runner running while you play. While the runner is alive, `launcher` reuses
the open session without a new 4/4. HBLauncher on its own stops after about 15 minutes in its menu
(64 launcher turns); `session.sh launcher` asks for 10,000 turns, about 40 hours, instead. Add
`--entries N` to choose another limit. Pause is on **Minus**; never press **Plus** (see known
issues). HBLauncher writes its own log to `~/Switch2/HBLauncher/build/hbl-session.log`.

### Plain Mario Kart Wii (experimental)

`--product wiicompiled` builds the game without Retro Rewind as a separate app, **Mario Kart Wii**,
with its own file-server folder (`hblauncher/fsroot-wiicompiled/`). It's offline only (Retro WFC
is part of Retro Rewind) and slower in races; see known issues. Steps 1–3 are the same, without the
Retro Rewind folder or payload:

```sh
hblauncher/docker/run.sh translate --product wiicompiled
hblauncher/docker/run.sh build --product wiicompiled
hblauncher/docker/run.sh pack --product wiicompiled
python3 hblauncher/build.py install --product wiicompiled
python3 hblauncher/build.py fsroot --product wiicompiled --nand /path/to/NAND
hblauncher/tools/session.sh server --product wiicompiled
```

`translate --product wiicompiled` replaces the Retro Rewind translation in the Docker volume, so
building Retro Rewind afterwards needs `translate --online` again (or use another `HBX_VOLUME`).
The file server serves one game at a time; start it with the game you'll pick in HBLauncher.

## Options

The port's menu (**L + R + right stick click**) has the main switches. `hblauncher/fsroot/hbxl-env`
sets their starting values, one `NAME=value` per line:

| Option | Default | What it does |
|---|---|---|
| `MKW_CPU_BOOST=N` | `1` | The console's CPU boost mode at launch: it raises the measured CPU clock from about 1107 MHz to about 1534 MHz, and every FPS figure in this README was measured with it on. `0` turns it off. |
| `MKW_RACE_WORKER=0` | on | Starts with the race GX worker off. The worker records graphics commands on the game thread and replays them on another core during offline races. It may improve race performance, but its effect hasn't been fully verified. |
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
- **Speed:** in Retro Rewind's offline races the game draws every tick (Retro Rewind's frame skip
  is off), and in every offline race the race GX worker replays graphics on its own core. The CPU
  boost mode is on at launch (about 1534 MHz instead of 1107).

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
`HBXL_TOOLCHAIN` pointing at the toolchain. Linux has no automatic address lookup, so before
`build`, set `HBX_FS_HOST=<the PC's IPv4 address>` (or leave it unset if you use HBLauncher's wired
link at `10.42.0.1`).

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
