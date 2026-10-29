# Validation

Validated on 2026-10-29 against HBLauncher `18b4748` (ABI 3).

## Tested

| What | On | Result |
|---|---|---|
| Patched musl toolchain (`build.py toolchain`) | arm64 Ubuntu 24.04 container on Apple Silicon | builds (GCC 15.1.0, musl 1.2.6 with HBLauncher's syscall hooks, BTI target libraries) |
| `deps`: Dawn, SDL3 and HBLauncher SDK patches | same | all apply cleanly at their pins |
| `runtime`: `libhbxl.a` with `hblauncher-runtime-file-locks.diff` | same | builds |
| `translate --online`: base 29,637 + Retro Rewind 6.13.4 4,005 functions, Retro-WFC payload signature | same | ok |
| `build` (console variant): static aarch64-linux-musl, BTI, `hbxl.ld`, `libhbxl.a` | same | links; code 127,315,552 B at `0x40000000`, data region 3,109,200 B at `0x50000000` |
| `pack` | same | `code.bin`, `data.bin`, `report.json` |
| `tools/check-package.cjs`: HBLauncher's `loadPackage` (manifest, ABI, sizes, hashes) | macOS host, Node | ok |
| `tools/check-package.cjs`: launcher lifecycle on HBLauncher's mock backend (map at bases, overlap checks against the resident launcher, enter once, release) | same | ok |
| The same code (HBX_SWITCH2, checked-only memory, BTI fibers) linked with `desktop/hbx_desktop_stubs.c` | same container, BTI enforced by the kernel | boots Retro Rewind headless on Dawn's Null backend to the title-screen modules: SYSCONF, disc index, `StaticR.rel`, WFC payload, Pulsar init. No crash and no BTI fault in 90 s |
| Upstream host tests (incl. the new fiber switch), static musl/BTI | same | 9/9 pass |

## Not tested

- **Anything on a Switch 2.** This includes launch, mapping a 121 MiB code image, Vulkan/Dawn on
  HBLauncher's device, rendering, audio, input, file-server I/O, speed, saves and stability.
- The host-side upload of the code image through the real RPC. It is capped at 64 MiB per write;
  `patches/hblauncher-host-large-writes.diff` splits it, but has only been checked to load and apply.
- Whether the session's SDK allocator can hold a ~122 MiB code source block plus the runtime's
  own heap.
- Unimplemented syscalls the game reaches on the console (the file-server root's `hbxl-env`
  enables `HBXL_TRACE=1` for this).
- **Online play.** The Linux runtime has no socket syscalls yet (Phase 4), so connecting to Wi-Fi
  in-game will fail.
- `build --variant desktop` through `build.py`. The desktop result above came from the same
  configuration built by the Phase 2 scripts.
