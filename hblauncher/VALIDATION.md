# Validation

What has been tested, on what, and when. The commit history was reordered into build order for
release, so commit dates follow that order; the dates here are when each test actually ran.

## First build (2026-10-29)

The first build, before anything ran on a Switch 2, against HBLauncher `18b4748` (ABI 3). Later
builds differ in the details (more runtime patches, a smaller code image); the fresh-folder build
below is the current one.

| What | On | Result |
|---|---|---|
| Patched musl toolchain (`build.py toolchain`) | arm64 Ubuntu 24.04 container on Apple Silicon | builds (GCC 15.1.0, musl 1.2.6 with HBLauncher's syscall hooks, BTI target libraries) |
| `deps`: Dawn, SDL3 and HBLauncher SDK patches | same | all apply cleanly at their pins |
| `runtime`: `libhbxl.a` with the HBLauncher runtime patches of the time | same | builds |
| `translate --online`: base 29,637 + Retro Rewind 6.13.4 4,005 functions, Retro-WFC payload signature | same | ok |
| `build` (console variant): static aarch64-linux-musl, BTI, `hbxl.ld`, `libhbxl.a` | same | links; code 127,315,552 B at `0x40000000`, data region 3,109,200 B at `0x50000000` |
| `pack` | same | `code.bin`, `data.bin`, `report.json` |
| `tools/check-package.cjs`: HBLauncher's `loadPackage` (manifest, ABI, sizes, hashes) | macOS host, Node | ok |
| `tools/check-package.cjs`: launcher lifecycle on HBLauncher's mock backend (map at bases, overlap checks against the resident launcher, enter once, release) | same | ok |
| The same code (HBX_SWITCH2, checked-only memory, BTI fibers) linked with `desktop/hbx_desktop_stubs.c` | same container, BTI enforced by the kernel | boots Retro Rewind headless on Dawn's Null backend to the title-screen modules: SYSCONF, disc index, `StaticR.rel`, WFC payload, Pulsar init. No crash and no BTI fault in 90 s |
| Upstream host tests (incl. the new fiber switch), static musl/BTI | same | 9/9 pass |

## On a Switch 2

Tested from 2026-10-30 to 2026-11-05 on one console, with HBLauncher `18b4748`, ns2jb `81d5bb1`,
Retro Rewind 6.13.4 and the file server on an Apple Silicon Mac on the same home network.

| What | Result |
|---|---|
| Launch from HBLauncher, boot to Retro Rewind's menus | works |
| Rendering, audio, controller input | works |
| Saves, through the file server | works |
| Offline races, defaults (CPU boost, race GX worker, frame skip off) at 1.5× | near 60 FPS in lighter stretches; full 12-kart races in the mid 50s in the pack and near 60 in first place; low 50s in busy parts of some custom tracks |
| Retro WFC online, including races (frame skip on) | works |
| Return to HBLauncher and launch again | works |

## HBLauncher and ns2jb setup

Checked on 2026-11-03 on an Apple Silicon Mac (Docker Engine 29.8), from fresh clones of
HBLauncher `18b4748` and ns2jb `81d5bb1` from GitHub and GitLab, with `tools/setup-session.py`,
Zig 0.15.2 and `docker/run.sh launcher`:

| What | Result |
|---|---|
| Both patches apply to the clean checkouts | ok |
| Rebuilt ns2jb helper | 5,801 bytes, SHA-256 `8a2c670b…`, identical to the helper used on the console; a second rebuild matches it byte for byte |
| ns2jb `manifest.json` and `SHA256SUMS` | identical to the working setup's |
| Inspectors and HID signatures from HBLauncher `9d00834` | identical to the working setup's |
| Every file of both prepared checkouts | identical to the working setup used on the console (build output aside) |
| Running the script again; `--check` on the working setup | no changes |
| A checkout at another commit, an overlapping local edit, or no Zig | stops before changing either checkout |
| A failure after one checkout was changed | the message lists what was already changed |
| `docker/run.sh launcher` (HBLauncher's `tools/build-all.sh`) | builds; launcher `code.bin` `222ec59b…`, `data.bin`, `report.json`, demo apps, probe and SDK example identical to the ones used on the console |
| HBLauncher host tests in the Docker image, with that build | 12/12 |
| ns2jb native range tests (Clang, AddressSanitizer + UBSan) | 50 scenarios pass |
| ns2jb protocol and existing tests | 19 pass |
| HBLauncher `range-safety` host tests | 14/14 |

The console tests with this mapping (small RX/RW maps, native execution, unmap and restore, remapping
the same address, 64 MiB + 12 KiB code across a request boundary, 3 MiB RW, the launcher display)
passed on the same console before the game ran.

## The README from fresh folders

Checked on 2026-11-03 on the same Mac: a fresh clone of this branch with its aurora submodule and
this release's changes, the HBLauncher and ns2jb prepared above, and a new Docker volume. The
disc and Retro Rewind files were copies of an existing extraction, not extracted again.

| Step | Result |
|---|---|
| `docker/run.sh toolchain`, `deps`, `runtime`, `translate --online`, `build`, `pack` | all succeed (about 13 minutes on 10 cores, after the image build) |
| A second `translate --online`, `build`, `pack` | byte-identical package (code `b26ee89b…`, 122,782,656 B) |
| `build` and `pack` again with the new-NAND fix below | code `4a48dcd0…`, 122,782,648 B; the package used in the console test |
| `build.py install`, `build.py fsroot`, `tools/check-package.cjs` | ok; HBLauncher's `loadPackage` and mock lifecycle pass |
| `tools/session.sh launcher --preflight` | every required check passes; the console, 4/4 session, Pico and capture checks fail as expected with no console |

The fresh packages aren't byte-identical to the one played on the console before this test (code
`a954d816…`), which was built from the same game source in an older Docker image and volume. The
cause wasn't tracked down.

On the console (2026-11-03 to 2026-11-05), the fresh setup with a brand-new NAND and no save: Retro Rewind boots,
creates its own NAND and settings, reaches the menus and runs offline races at about 60 FPS
(mid 50s in a full 12-kart pack), and returns to HBLauncher cleanly. This run found and fixed two
problems a new player would hit: the new NAND's `setting.txt` couldn't be created (no hard links
over the file server), and building the starter pipeline cache at start-up ran out of memory.
Changing the resolution to 1.5× during a race broke the graphics; changing it in the menus didn't.
The upload probe found no bad texture copies in either case.

## Not tested

- Every track. Busy scenes on some custom tracks dip below 60 FPS.
- Pipeline thread counts other than 2.
- A build without `patches/aurora-hbx-lighting-safe.diff` or `patches/dawn-hbx-no-dedicated.diff`.
- Building on native aarch64 Linux.
- Online play from the fresh folders (it would create a new Retro WFC profile).
- `setup-session.py` on Linux, or with Zig on anything but an Apple Silicon Mac.
- `build --variant desktop` through `build.py`. The desktop result above came from the same
  configuration built by the Phase 2 scripts.
