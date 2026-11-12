# WiiCompiled + Retro Rewind for Switch 2 (HBLauncher)

An experimental port of [WiiCompiled](https://github.com/patchzyy/Wiicompiled) with Retro Rewind for
the Nintendo Switch 2, run by RavensClawX's [HBLauncher](https://github.com/ravensclawx/HBLauncher).
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

Built on HBLauncher `18b4748` and ns2jb `81d5bb1` (November 2026). The main build is Retro Rewind;
plain Mario Kart Wii also works. PAL disc only.

## What works

Tested on one Switch 2, with the defaults below, a PC on the same home network, and Retro Rewind
6.13.5:

- Retro Rewind boots; menus, offline races, online races on Retro WFC and saves work.
- Races run at or near 60 FPS, thanks to PGO. Busy moments with lots of karts on screen can
  dip to the mid 50s, briefly lower in the busiest ones. The numbers are in
  [VALIDATION.md](VALIDATION.md).
- Returning to HBLauncher and launching again.
- Sleep mode, including going online after waking.
- Experimental: playing from the microSD, so the PC is only needed to start the game.
- Plain Mario Kart Wii boots and runs races near 60 FPS.

## Known issues

- **A PC has to stay on**, on Ethernet with the console, unless the game is on the microSD ([experimental](#optional-microsd-card-experimental)).
- **Unplugging ends the HBLauncher session.** With the game on the microSD you can unplug the Ethernet once the game is running, but you can't go back to HBLauncher's menu afterwards. To play again, close SmileBASIC 4, open it again, and start a new runner and 4/4 session.
- **Don't press Plus.** SmileBASIC sees the controller behind the game, and Plus stops the SmileBASIC program that keeps the session alive. The game keeps running for about two minutes, then the session dies. Pause is on **Minus**, and the port's menu is **L + R + right stick click**.
- **Not a locked 60 FPS.** Busy moments with lots of karts on screen dip to the mid 50s, briefly lower in the busiest ones, and there's a short dip right before the countdown. Offline, the game also runs slightly slower than normal speed in those moments.
- **Change the resolution outside races.** Changing it during a race breaks the graphics until you restart the game; changing it in the game's menus works.
- **Online can dip briefly.** Online keeps Retro Rewind's frame skip, so a slow moment skips frames instead of slowing the game down. With PGO it's rare.
- **Restart SmileBASIC after a crash or a new install.** The game shares SmileBASIC's process with HBLauncher, so memory it didn't free stays taken, and the next launch can close SmileBASIC. Close SmileBASIC, open it again, and start a new runner and 4/4 session. To play again normally, relaunch from HBLauncher's menu; `session.sh launcher` refuses to start a second time in a session where the game already ran.
- **Plain Mario Kart Wii can drop to 30 FPS.** Like a real Wii, it waits for the screen refresh, so a frame that's slightly late costs a whole extra frame. PGO keeps frames fast enough that this rarely happens.
- **New places stutter the first time** while their shaders build; they're saved after that. A new install stutters a lot in its first races. Each launch also spends about half a minute rebuilding the saved shaders, so wait on the title screen before racing.
- **Retro Rewind updates need a rebuild.** Every update that changes `Code.pul` needs a new `translate`, `build` and `pack`.
- **Black textures after sleep (seen once).** If it happens, please share `UserData/Logs/console.log` before restarting the game twice.
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
  get **0.15.2 → macOS aarch64** (`zig-aarch64-macos-0.15.2.tar.xz`), then unpack it, clear the
  mark that makes macOS block downloaded programs (`xattr`), and check it:

  ```sh
  cd ~/Switch2
  tar -xf ~/Downloads/zig-aarch64-macos-0.15.2.tar.xz
  mv zig-aarch64-macos-0.15.2 zig
  xattr -dr com.apple.quarantine zig
  zig/zig version
  cd WiiCompiled-Switch2
  ```

  **You should see** `0.15.2`.

- **Your own Mario Kart Wii PAL disc (`RMCP01`)** and the latest Retro Rewind.
- **Optional:** Clang from the Xcode Command Line Tools, for ns2jb's tests in step 2.

HBLauncher (`18b4748`) and ns2jb (`81d5bb1`) are cloned at those exact versions in step 2. Newer
versions haven't been tested with this port's patches.

### 2. Prepare HBLauncher and ns2jb

HBLauncher and ns2jb both need a patch before they can load this game (without them a launch fails
with `Unknown process probe`). One script prepares both:

```sh
cd ~/Switch2/WiiCompiled-Switch2
hblauncher/tools/setup-session.py --hblauncher ~/Switch2/HBLauncher --ns2jb ~/Switch2/ns2jb --zig ~/Switch2/zig/zig
```

It clones any folder that doesn't exist yet and checks out the pinned commit, then:

- applies the range-mapping and SD asset session patches to HBLauncher;
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
command does it in Docker, using HBLauncher's own build script (`tools/build-all.sh`).

Docker Desktop has to be running first. Open it (or run `open -a Docker`) and wait until this
prints a version number instead of `Cannot connect to the Docker daemon`:

```sh
docker info --format '{{.ServerVersion}}'
```

Then build the launcher:

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
/tmp/hbl-range-native
node --test tests/hbl-wire.test.cjs $(git ls-files '*.test.cjs')
cd ~/Switch2/HBLauncher && node --test tests/host/range-safety.test.cjs
cd ~/Switch2/WiiCompiled-Switch2
```

The first test program runs 50 mapping scenarios and should end with `50 scenarios passed`; the
`node --test` runs should report no failures.

HBLauncher's own tests check its PC side: its app format, its package loading, and a full
launch-and-return cycle against a simulated console. They need the launcher you just built and the
Vulkan headers, which the Docker image has, so they run in Docker:

```sh
IMAGE=$(docker images wiicompiled-hbx-build --format '{{.Repository}}:{{.Tag}}' | grep -v ':latest' | head -1)
docker run --rm -v ~/Switch2/HBLauncher:/hbl -w /hbl "$IMAGE" node tests/host/lifecycle.test.cjs
```

The first line finds the Docker image the launcher build made. It should end with
`12/12 host tests passed`. Run it before installing the game in step 5, since the test expects only
HBLauncher's own apps.

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
cp /tmp/mkw-extract/sys/main.dol Assets/main.dol
cp /tmp/mkw-extract/files/rel/StaticR.rel Assets/StaticR.rel
cp -R /tmp/mkw-extract/files Assets/DATA/files
cp -R /tmp/mkw-extract/sys Assets/DATA/sys
shasum -a 256 Assets/main.dol Assets/StaticR.rel
```

nodtool puts the disc's `sys/` and `files/` folders straight into `/tmp/mkw-extract`. **You should
see** these two hashes, which mean a clean PAL disc:

- `Assets/main.dol`: `80d18895b39c63bd80f457398bfcbb91b7d16ac116a41a88967e954080155b05`
- `Assets/StaticR.rel`: `16d9d146112541fefea701ecb5bc1a496f9d50e4a752fbb5b6778e7c6399f67d`

Only when both match, delete the temporary copy:

```sh
rm -rf /tmp/mkw-extract
```

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

Make sure Docker Desktop is running (see step 2). Each step runs in a Docker container, and build
state stays in the Docker volume `wiicompiled-hbx` (set `HBX_VOLUME` to use another). Run these one
at a time, waiting for each to finish:

```sh
cd ~/Switch2/WiiCompiled-Switch2
export HBL_ROOT=~/Switch2/HBLauncher
hblauncher/docker/run.sh toolchain
hblauncher/docker/run.sh deps
hblauncher/docker/run.sh runtime
hblauncher/docker/run.sh translate --online
hblauncher/docker/run.sh build
hblauncher/docker/run.sh pack
```

What each does:

- `toolchain`: builds the patched musl compiler toolchain, once (a few minutes).
- `deps`: fetches Dawn and SDL3 at their pinned versions and patches them, and copies HBLauncher's
  SDK.
- `runtime`: builds HBLauncher's Linux runtime library (`libhbxl.a`).
- `translate --online`: translates the base game and Retro Rewind. Leave out `--online` for
  offline only (then the payload isn't needed).
- `build`: compiles and links the game for the console (up to 30 minutes), optimized with the
  recorded profile in `hblauncher/pgo/`.
- `pack`: makes the HBLauncher package in `hblauncher/dist/wiicompiled-rr`.

**You should see** `pack` end with `Package: …/hblauncher/dist/wiicompiled-rr`. Running a failed step
again is safe. In a new terminal, run the `export` line again before continuing.

#### Plain Mario Kart Wii

`--product wiicompiled` builds it as a separate app, **Mario Kart Wii**, with its own file-server
folder (`hblauncher/fsroot-wiicompiled/`). Steps 1–3 are the same, without the Retro Rewind folder
or payload:

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

You don't need to set your Mac's address: the file server (step 6) writes it into the installed game
each time it starts.

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
| `etc/resolv.conf` | the DNS servers the game uses to find Retro WFC (`1.1.1.1`, `8.8.8.8`); edit it to use others |
| `hbxl-env` | options read at launch (see below) |
| `icon.png` | optional launcher icon; add your own |

#### Optional: microSD card (experimental)

With the console off, copy everything in `hblauncher/fsroot/` to a `wiicompiled` folder at the root
of the card. The game then runs from the card, and the PC is only needed to start it (step 6, the
runner and the launcher).

Your saves are then on the card, and the PC's copy stops changing. To back them up, copy
`wiicompiled/UserData/` and `wiicompiled/RetroRewind/riivolution/` back to `hblauncher/fsroot/`.

### 6. Play

Create `hblauncher/fsroot/session.env` with the two lines `setup-session.py` printed, for example:

```sh
HBL_ROOT=$HOME/Switch2/HBLauncher
HBL_NS2JB=$HOME/Switch2/ns2jb
```

Each session goes like this; open three terminal windows in `~/Switch2/WiiCompiled-Switch2` (two
with the game on the microSD):

1. **On the console:** restart it, open SmileBASIC and go to Direct mode.
2. **Terminal 1, the file server** (skip it if the game is on the
   [microSD](#optional-microsd-card-experimental)):

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

Keep the server (if you use it) and the runner running while you play. To play again, relaunch from HBLauncher's menu. Pause is on **Minus**; never press **Plus** (see known
issues). HBLauncher writes its own log to `~/Switch2/HBLauncher/build/hbl-session.log`.

## Options

Press **L + R + right stick click** in the game for the port's menu (D-pad to move and change, A to
rebind a button, B to close):

| Setting | Default | Notes |
|---|---|---|
| Resolution | 1.0× | 1.5× (about 720p) and 2× run about the same. Change it in menus, not during a race. Saved. |
| Show FPS | off | Saved. |
| Volume | 100% | Saved. |
| CPU boost | on | Raises the CPU from about 1107 to 1534 MHz. Every FPS figure here was measured with it on. |
| Race GX worker | on | Moves graphics work off the game's main thread in offline races. |
| Frame skip (offline) | off | Retro Rewind's frame skip in offline races. Online, it's always on. |
| Buttons | A, B, X, Y as labelled | Rebind any button. Saved. |

CPU boost, Race GX worker and Frame skip reset at each launch. To change what they start as, add a
line to `hblauncher/fsroot/hbxl-env`: `MKW_CPU_BOOST=0`, `MKW_RACE_WORKER=0` or `MKW_FRAME_SKIP=1`.

**For testing and debugging**, also in `hbxl-env`: `MKW_PERF_STATS=1` logs FPS and frame timing,
`MKW_IRQ_FOLLOW_THREAD=0` turns off the interrupt fix for comparisons, `AURORA_PIPELINE_THREADS=N`
sets the shader build threads (default 2), and `HBXL_TRACE=1` logs unimplemented system calls (very
slow).

## Patches

The patches this port applies to HBLauncher, ns2jb, aurora, Dawn and SDL, and why each was added,
are listed in [PATCHES.md](PATCHES.md).

## Other build options

`build` uses the recorded speed profile (PGO) in `hblauncher/pgo/` on its own; it holds no game code. After
larger runtime or translator changes, record a new one with `hblauncher/tools/pgo.sh` (15 minutes,
headless in Docker; `--product wiicompiled` for plain Mario Kart Wii), then `build` and `pack` again.
Add `-DMKW_PGO=` to `build` to build without it.

On native aarch64 Linux, run `python3 hblauncher/build.py <action>` directly, with
`HBXL_TOOLCHAIN` pointing at the toolchain. Linux has no automatic address lookup, so before
`build`, set `HBX_FS_HOST=<the PC's IPv4 address>` (or leave it unset if you use HBLauncher's wired
link at `10.42.0.1`).

`build --variant desktop` links stand-ins for HBLauncher's runtime instead
(`desktop/hbx_desktop_stubs.c`). The same code then runs headless on desktop aarch64 Linux,
on Dawn's Null backend, with `SDL_VIDEO_DRIVER=offscreen SDL_AUDIO_DRIVER=dummy`.

## Credits

- **[WiiCompiled](https://github.com/patchzyy/Wiicompiled)** by patchzyy and contributors: the
  recompilation this is built on.
- **[HBLauncher](https://github.com/ravensclawx/HBLauncher)** and the
  **[Twilight Princess](https://github.com/ravensclawx/TwilightPrincess-HBLauncher)** and
  **[Animal Crossing](https://github.com/ravensclawx/AnimalCrossing-HBLauncher)** ports by
  RavensClawX: the launcher, Linux runtime and the Dawn, SDL3 and aurora patches this port started
  from.
- **[ns2jb](https://gitlab.com/pepsipu/ns2jb)**, the Switch 2 exploit by Sammy Hajhamid (pepsipu),
  used here through [kurumuz's fork](https://gitlab.com/kurumuz/ns2jb). It opens the console
  session; this port adds the memory-loading commands HBLauncher needs to start apps.
- **[DriftDroid](https://github.com/dorPXP/driftdroid)** by dorPXP: the Android and Switch 1 port of
  WiiCompiled. Its native THP video decoder, lighting helper and thread-switch interrupt fix are
  used here, adapted, and its notes and fixes helped throughout.
- **[KartPadNX](https://github.com/KawaiiBunga/KartPadNX)** by KawaiiBunga, another Switch 1 port
  of WiiCompiled. The idea to use PGO came from KartPadNX and DriftDroid; the setup here is this
  port's own, with [LLVM](https://llvm.org)'s profile tools.
- **[Retro Rewind](https://wiki.tockdom.com/wiki/Retro_Rewind)** by ZPL and team, and
  **[Retro WFC](https://rwfc.net)**, the online service it plays on; you use their files and
  servers with your own copy.
- **[aurora](https://github.com/encounter/aurora)** by encounter: the GameCube and Wii graphics
  layer that turns the game's drawing into modern GPU shaders.
- **[Dawn](https://dawn.googlesource.com/dawn)** by Google: the WebGPU library aurora draws
  through, which uses the console's Vulkan driver. It's built from
  [encounter's fork](https://github.com/encounter/dawn), as aurora uses.
- **[SDL](https://github.com/libsdl-org/SDL)**: audio and controller input, through HBLauncher
  drivers from the Twilight Princess port.

WiiCompiled is GPL-3.0 (see `../LICENSE`), and so is this port. The Twilight Princess patches are
MIT (`patches/LICENSE.tp-integration`). See [THIRD_PARTY.md](THIRD_PARTY.md).
"Nintendo Switch 2" and "Mario Kart Wii" are trademarks of Nintendo; this project is not affiliated with
or endorsed by Nintendo.
