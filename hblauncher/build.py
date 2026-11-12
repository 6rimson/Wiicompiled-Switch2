#!/usr/bin/env python3
"""Build, package, install or serve WiiCompiled + Retro Rewind as an HBLauncher app (Switch 2).
--product wiicompiled does the same for plain Mario Kart Wii, as a separate app and fsroot.

Build actions (deps, runtime, translate, build, pack) run on aarch64 Linux - the provided
container (hblauncher/docker/run.sh) or a native machine. fsroot, install and serve run on the PC
that drives the HBLauncher session (macOS or Linux).

  toolchain   build HBLauncher's patched aarch64-linux-musl toolchain into $HBXL_TOOLCHAIN
  deps        fetch Dawn and SDL3 at their pins and apply the HBLauncher patches; copy the
              HBLauncher SDK and apply this port's runtime patches
  runtime     apply any new runtime patches, then build libhbxl.a (HBLauncher's Linux runtime)
  address     print this machine's current address on the home network
  translate   translate the base game and Retro Rewind (add --online for the Retro-WFC payload)
  build       configure and build (--variant console: HBLauncher link; desktop: headless test)
  pack        split the console ELF into an HBLauncher package under hblauncher/dist/<id>
  install     copy the package into $HBL_ROOT/apps (never overwrites)
  fsroot      lay out a private file-server root from your own disc, RR folder and saves
  serve       run HBLauncher's file server on this app's port

Environment: HBL_ROOT (HBLauncher checkout), HBXL_TOOLCHAIN (patched musl toolchain),
HBX_DEPS (default hblauncher/deps), HBX_BUILD (default hblauncher/build), HBX_FS_HOST (the PC's
IPv4 address the console reaches, compiled into the app and used by serve; default 10.42.0.1,
HBLauncher's wired link).
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import socket
import subprocess
import sys

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
PINS = json.loads((HERE / "dependencies.json").read_text())
# Each product is its own CMake target, HBLauncher app and file-server root. main() picks one.
PRODUCTS = {
    "retro-rewind": {"target": "RetroRewind", "app": HERE / "app", "root": HERE / "fsroot"},
    "wiicompiled": {"target": "WiiCompiled", "app": HERE / "app-wiicompiled", "root": HERE / "fsroot-wiicompiled"},
}


def select_product(name):
    global PRODUCT_NAME, PRODUCT, APP_DIR, MANIFEST, APP_ID, FS_PORT
    PRODUCT_NAME = name
    PRODUCT = PRODUCTS[name]["target"]
    APP_DIR = PRODUCTS[name]["app"]
    MANIFEST = json.loads((APP_DIR / "manifest.json").read_text())
    APP_ID = MANIFEST["id"]
    FS_PORT = MANIFEST["linux"]["fs_port"]


def retro_rewind():
    return PRODUCT_NAME == "retro-rewind"


DEPS = Path(os.environ.get("HBX_DEPS", HERE / "deps"))
BUILD = Path(os.environ.get("HBX_BUILD", HERE / "build"))
PATCHES = HERE / "patches"
# Shipped PGO profiles, <product>.profdata.xz, recorded with tools/pgo.sh.
PGO_DIR = HERE / "pgo"
FS_HOST = os.environ.get("HBX_FS_HOST", "10.42.0.1")
TRANSLATOR = REPO / "translator/src/Translator.Cli/bin/Release/net8.0/Translator.Cli.dll"
RR_PACK = REPO / "PulsarPacks/completed/RetroRewind/RetroRewind6"
WFC_PAYLOAD = REPO / "build/retro-wfc/binary/payload.RMCPD00.bin"


def run(args, **kwargs):
    print("+", " ".join(str(a) for a in args), flush=True)
    subprocess.run([str(a) for a in args], check=True, **kwargs)


def required(name, relative=None):
    value = os.environ.get(name)
    if not value:
        raise SystemExit(f"Set {name}; see hblauncher/README.md")
    path = Path(value).expanduser().resolve()
    if not (path / relative if relative else path).exists():
        raise SystemExit(f"Invalid {name}: {path}")
    return path


RUNTIME_PATCHES = (
    "hblauncher-runtime-file-locks.diff",
    "hblauncher-runtime-fs-host.diff",
    "hblauncher-runtime-getcwd.diff",
    "hblauncher-runtime-main-stack.diff",
    "hblauncher-runtime-file-modes.diff",
    "hblauncher-runtime-syscall-counts.diff",
    "hblauncher-runtime-fast-clock.diff", "hblauncher-runtime-thread-pin.diff", "hblauncher-runtime-memory-log.diff", "hblauncher-runtime-sockets.diff", "hblauncher-runtime-net-fixes.diff", "hblauncher-runtime-net-trace.diff", "hblauncher-runtime-net-nonblock.diff", "hblauncher-runtime-sd-path.diff",
)


def require_linux_aarch64():
    if platform.system() != "Linux" or platform.machine() not in ("aarch64", "arm64"):
        raise SystemExit("Build actions run on aarch64 Linux (hblauncher/docker/run.sh)")


def apply_patch(directory, patch):
    """Apply a patch once; accept it when it is already applied."""
    git = (directory / ".git").exists()
    forward = ["git", "-C", directory, "apply"] if git else ["patch", "-d", directory, "-p1", "--forward", "-s"]
    check = ["git", "-C", directory, "apply", "--check"] if git else forward + ["--dry-run"]
    reverse = (["git", "-C", directory, "apply", "--reverse", "--check"] if git
               else ["patch", "-d", directory, "-p1", "-R", "-s", "--dry-run"])
    with open(patch, "rb") as p:
        if subprocess.run([str(a) for a in check], stdin=p, capture_output=True).returncode == 0:
            p.seek(0)
            subprocess.run([str(a) for a in forward] + ([str(patch)] if git else []),
                           stdin=None if git else p, check=True)
            print(f"patched {directory.name} with {patch.name}")
            return
        p.seek(0)
        if subprocess.run([str(a) for a in reverse] + ([str(patch)] if git else []),
                          stdin=None if git else p, capture_output=True).returncode == 0:
            return
    raise SystemExit(f"{patch.name} does not apply to {directory}")


def checkout(name, url, revision, patch):
    path = DEPS / name
    if not path.exists():
        run(["git", "clone", "-q", "--no-checkout", "--filter=blob:none", url, path])
        run(["git", "-C", path, "checkout", "-q", "--detach", revision])
    actual = subprocess.check_output(["git", "-C", str(path), "rev-parse", "HEAD"], text=True).strip()
    if actual != revision:
        raise SystemExit(f"{path} is at {actual}, expected {revision}; use a fresh HBX_DEPS")
    apply_patch(path, PATCHES / patch)
    return path


def toolchain():
    """musl-cross-make at its pin with the Twilight Princess port's config (BTI target libraries)
    and HBLauncher's musl syscall hooks."""
    require_linux_aarch64()
    out = Path(os.environ.get("HBXL_TOOLCHAIN", DEPS / "toolchain")).resolve()
    if (out / "bin/aarch64-linux-musl-gcc").exists():
        print(f"toolchain already built: {out}")
        return
    hbl = required("HBL_ROOT", "sdk/linux/musl-hbx-syscall-hooks.diff")
    pin = PINS["musl-cross-make"]
    mcm = DEPS / "musl-cross-make"
    if not mcm.exists():
        run(["git", "clone", "-q", pin["repository"] + ".git", mcm])
    run(["git", "-C", mcm, "checkout", "-q", pin["commit"]])
    config = (HERE / "cmake/config.mak.example").read_text()
    config = "\n".join(f"OUTPUT = {out}" if line.startswith("OUTPUT =") else line for line in config.splitlines())
    (mcm / "config.mak").write_text(config + "\n")
    hooks = mcm / "patches/musl-1.2.6"
    hooks.mkdir(parents=True, exist_ok=True)
    shutil.copy2(hbl / "sdk/linux/musl-hbx-syscall-hooks.diff", hooks / "9001-hbx-syscall-hooks.diff")
    run(["make", "-C", mcm, f"-j{os.cpu_count() or 4}"])
    run(["make", "-C", mcm, "install"])


def hbl_sdk():
    """The HBLauncher SDK copy this port links against, with its runtime patch applied."""
    return DEPS / "hblauncher"


def deps():
    DEPS.mkdir(parents=True, exist_ok=True)
    checkout("dawn", PINS["dawn"]["repository"] + ".git", PINS["dawn"]["commit"], PINS["dawn"]["patch"])
    checkout("sdl3", PINS["sdl3"]["repository"] + ".git", PINS["sdl3"]["commit"], PINS["sdl3"]["patch"])
    hbl = required("HBL_ROOT", "sdk/linux/runtime.c")
    sdk = hbl_sdk()
    if sdk.exists():
        shutil.rmtree(sdk)
    shutil.copytree(hbl / "sdk", sdk / "sdk")
    shutil.copytree(hbl / "tools", sdk / "tools")
    apply_runtime_patches(sdk)


def apply_patch_series(directory, patches):
    """Apply patches in order. Later patches change lines near earlier ones, which can no longer
    be reversed, so the newest applied patch stands for all those before it."""
    pending = []
    for patch in reversed(patches):
        reverse = ["patch", "-d", str(directory), "-p1", "-R", "-s", "--dry-run"]
        with open(PATCHES / patch, "rb") as p:
            if subprocess.run(reverse, stdin=p, capture_output=True).returncode == 0:
                break
        pending.insert(0, patch)
    for patch in pending:
        apply_patch(directory, PATCHES / patch)


def apply_runtime_patches(sdk):
    """Every patch to HBLauncher's Linux runtime; already-applied ones are skipped, so an existing
    SDK copy picks up new patches without running deps again. Later patches change lines near
    earlier ones, which can no longer be reversed, so the newest applied patch stands for all
    those before it."""
    apply_patch_series(sdk, RUNTIME_PATCHES)


def runtime():
    require_linux_aarch64()
    required("HBXL_TOOLCHAIN", "bin/aarch64-linux-musl-gcc")
    apply_runtime_patches(hbl_sdk())
    BUILD.mkdir(parents=True, exist_ok=True)
    run(["python3", hbl_sdk() / "sdk/tools/hbxl-build.py", "--runtime-lib", BUILD / "libhbxl.a"])


def translator(*args):
    if not TRANSLATOR.exists():
        run(["dotnet", "build", REPO / "translator/src/Translator.Cli/Translator.Cli.csproj", "-c", "Release"])
    run(["dotnet", TRANSLATOR, *args], cwd=REPO)


def translate(online):
    """docs/building-macos.md steps 2-3. The translator writes host-format assembly and absolute
    paths, so translate on the machine that builds."""
    require_linux_aarch64()
    if online and not retro_rewind():
        raise SystemExit("--online is for Retro Rewind (Retro WFC); plain WiiCompiled builds offline")
    inputs = [REPO / "Assets/main.dol", REPO / "Assets/StaticR.rel"]
    if retro_rewind():
        inputs.append(RR_PACK / "Binaries/Code.pul")
    for path in inputs:
        if not path.exists():
            raise SystemExit(f"Missing {path}; see hblauncher/README.md (inputs)")
    jobs = str(os.cpu_count() or 4)
    project = ["--project", "projects/mkwii/recomp.yml"]
    (REPO / "generated/functions").mkdir(parents=True, exist_ok=True)
    (REPO / "build/base").mkdir(parents=True, exist_ok=True)
    translator("translate-recursive", "0x800060A4", *project, "--outdir", "generated/functions",
               "--output-metadata", "generated/base_translation_output.json",
               "--production-source-bundle", "generated/base_translation_sources.bin",
               "--no-function-files", "--prune-stale", "--threads", jobs)
    translator("emit-base-manifest", *project, "--out", "build/base", "--functions-dir", "generated/functions",
               "--translation-output-metadata", "generated/base_translation_output.json", "--region", "P")
    (REPO / "generated/build_shards").mkdir(parents=True, exist_ok=True)
    shards = ["emit-build-shards", *project, "--base-metadata", "generated/base_translation_output.json",
              "--base-functions-dir", "generated/functions", "--native-source-dir", "runtime/src",
              "--out", "generated/build_shards"]
    if not retro_rewind():
        translator("generate-data-init", *project)
        translator(*shards)
        return
    wfc = ["--skip-retro-wfc"]
    if online:
        if not WFC_PAYLOAD.exists():
            raise SystemExit(f"Missing {WFC_PAYLOAD}; download it as in docs/building-macos.md")
        translator("validate-retro-wfc-payload", "--directory", "build/retro-wfc", "--game-id", "RMCP01")
        wfc = ["--retro-wfc-payload", str(WFC_PAYLOAD)]
    (REPO / "build/mods/retro_rewind_full_cpp").mkdir(parents=True, exist_ok=True)
    translator("translate-mod", *project, "--profile", "retro-rewind",
               "--base-manifest", "build/base/mkwii_base_manifest.json",
               "--base-translation-output-metadata", "generated/base_translation_output.json",
               "--code-pul", RR_PACK / "Binaries/Code.pul", "--mod-root", RR_PACK, "--mod-name", "Retro Rewind",
               "--region", "P", "--out", "build/mods/retro_rewind_full_cpp", "--prefer-cached-inputs",
               "--emit-cpp", "--threads", jobs, *wfc)
    translator("generate-data-init", *project)
    translator(*shards, "--resolved-profile", "build/mods/retro_rewind_full_cpp/resolved_dispatch_profile.json",
               "--retro-cpp-dir", "build/mods/retro_rewind_full_cpp/cpp")


def apply_aurora_patch():
    aurora = REPO / "aurora-main"
    if not (aurora / "CMakeLists.txt").exists():
        raise SystemExit("Run git submodule update --init aurora-main first")
    apply_patch(aurora, PATCHES / PINS["aurora"]["patch"])
    for patch in ("aurora-hbx-pipeline-threads.diff", "aurora-hbx-pipeline-retry.diff",
                  "aurora-hbx-dawn-toggles.diff", "aurora-hbx-diagnostics.diff",
                  "aurora-hbx-texture-debug.diff", "aurora-hbx-frame-timing.diff",
                  "aurora-hbx-lighting-safe.diff", "aurora-hbx-bind-diag.diff",
                  "aurora-hbx-texture-selftest.diff", "aurora-hbx-frame-check.diff", "aurora-hbx-device-lock.diff", "aurora-hbx-upload-probe.diff", "aurora-hbx-heap-block.diff", "aurora-hbx-console-input.diff", "aurora-hbx-fifo-spin.diff", "aurora-hbx-drain-spin.diff", "aurora-hbx-device-destroy.diff", "aurora-hbx-release-gpu.diff", "aurora-hbx-no-boot-prune.diff"):
        apply_patch(aurora, PATCHES / patch)


def link_flags(variant, build):
    if variant == "desktop":
        stubs = build / "hbx_desktop_stubs.o"
        toolchain = required("HBXL_TOOLCHAIN", "bin/aarch64-linux-musl-gcc")
        run(["clang", "--target=aarch64-linux-musl", f"--sysroot={toolchain / 'aarch64-linux-musl'}",
             "-mbranch-protection=bti", "-O2", f"-I{hbl_sdk() / 'sdk/include'}",
             "-c", HERE / "desktop/hbx_desktop_stubs.c", "-o", stubs])
        return str(stubs)
    lib = BUILD / "libhbxl.a"
    if not lib.exists():
        raise SystemExit("Build the runtime first: build.py runtime")
    toolchain = required("HBXL_TOOLCHAIN", "bin/aarch64-linux-musl-gcc")
    sd_objects = []
    for source in ("storage", "directory", "vfs", "benchmark", "offline", "network"):
        obj = build / f"sd-{source}.o"
        run([toolchain / "bin/aarch64-linux-musl-gcc", "-O2", "-fno-pie", "-mbranch-protection=bti",
             "-Wall", "-Wextra", "-Werror", f"-I{hbl_sdk() / 'sdk/include'}", f"-I{hbl_sdk() / 'sdk/linux'}",
             "-c", HERE / "sd" / (source + ".c"), "-o", obj])
        sd_objects.append(str(obj))
    ld = hbl_sdk() / "sdk/linux/hbxl.ld"
    linux = MANIFEST["linux"]
    if any(c.isspace() for c in f"{ld}{lib}"):
        raise SystemExit("Use build and SDK paths without whitespace")
    octets = [int(part) for part in FS_HOST.split(".")]
    if len(octets) != 4 or any(not 0 <= o <= 255 for o in octets):
        raise SystemExit(f"HBX_FS_HOST must be an IPv4 address, not {FS_HOST!r}")
    fs_host = octets[0] | octets[1] << 8 | octets[2] << 16 | octets[3] << 24   # sockaddr byte order
    print(f"file server address compiled in: {FS_HOST}")
    return " ".join([
        "-nostartfiles", "-Wl,--wrap=hbxl_vfs_syscall", "-Wl,--wrap=hbxl_vfs_shutdown", "-Wl,--wrap=hbxl_vfs_write_console",
        "-Wl,--wrap=hbxl_vfs_init", "-Wl,--wrap=hbxl_vfs_time_offset_ns", "-Wl,--wrap=hbxl_net_syscall", *sd_objects, f"-Wl,-T,{ld}",
        f"-Wl,--defsym=HBXL_CODE_BASE={linux['code_base']}", f"-Wl,--defsym=HBXL_DATA_BASE={linux['data_base']}",
        f"-Wl,--defsym=HBXL_FS_PORT={FS_PORT}", f"-Wl,--defsym=HBXL_FS_HOST=0x{fs_host:08x}",
        # Hot functions first: the game's code is 120 MB, more than the TLBs and L2 can hold.
        f"-Wl,--section-ordering-file={HERE / 'section-order.ld'}",
        "-Wl,--eh-frame-hdr", "-Wl,-z,noexecstack", "-Wl,--build-id=none",
        f"-Wl,--whole-archive {lib} -Wl,--no-whole-archive"])


def pgo_args(variant, extra):
    """Build the console app with the shipped PGO profile, unless the command line sets MKW_PGO."""
    if any(arg.startswith("-DMKW_PGO") for arg in extra):
        return []
    shipped = PGO_DIR / f"{PRODUCT_NAME}.profdata.xz"
    if variant != "console" or not shipped.exists():
        return ["-DMKW_PGO="]
    import lzma
    data = lzma.decompress(shipped.read_bytes())
    # The content hash is in the name: clang doesn't track the profile as a dependency, so a new
    # profile has to change the compile command to rebuild what it covers.
    profile = BUILD / "pgo" / f"{PRODUCT_NAME}-{hashlib.sha256(data).hexdigest()[:12]}.profdata"
    if not profile.exists():
        profile.parent.mkdir(parents=True, exist_ok=True)
        profile.write_bytes(data)
    print(f"PGO profile: {shipped.name}")
    return ["-DMKW_PGO=use", f"-DMKW_PGO_PROFILE={profile}"]


def build(variant, jobs, extra):
    require_linux_aarch64()
    extra = [*pgo_args(variant, extra), *extra]
    required("HBXL_TOOLCHAIN", "bin/aarch64-linux-musl-gcc")
    if not (REPO / "generated/build_shards/shards.cmake").exists():
        raise SystemExit("Translate first: build.py translate [--online]")
    apply_aurora_patch()
    apply_patch_series(DEPS / "dawn", ("dawn-hbx-no-dedicated.diff", "dawn-hbx-buffer-log.diff",
                                       "dawn-hbx-image-info.diff"))
    out = BUILD / variant
    out.mkdir(parents=True, exist_ok=True)
    off = ["DAWN_BUILD_PROTOBUF", "TINT_BUILD_IR_BINARY", "DAWN_USE_X11", "DAWN_USE_WAYLAND",
           "DAWN_ENABLE_OPENGLES", "DAWN_ENABLE_DESKTOP_GL", "TINT_BUILD_GLSL_WRITER", "TINT_BUILD_HLSL_WRITER",
           "TINT_BUILD_MSL_WRITER", "TINT_BUILD_GLSL_VALIDATOR", "DAWN_USE_GLFW",
           "DAWN_SUPPORTS_GLFW_FOR_WINDOWING", "GLFW_BUILD_WAYLAND", "GLFW_BUILD_X11", "SDL_X11", "SDL_WAYLAND",
           "SDL_KMSDRM", "SDL_OPENGL", "SDL_OPENGLES", "SDL_ALSA", "SDL_PULSEAUDIO", "SDL_PIPEWIRE", "SDL_JACK",
           "SDL_SNDIO", "SDL_DBUS", "SDL_IBUS", "SDL_LIBUDEV", "SDL_HIDAPI", "SDL_CAMERA", "SDL_TESTS",
           "SDL_EXAMPLES"]
    run(["cmake", "-S", REPO / "runtime", "-B", out, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
         f"-DCMAKE_TOOLCHAIN_FILE={HERE / 'cmake/switch2-musl-clang.cmake'}",
         # The toolchain file builds the link flags from HBX_EXTRA_LINK, but CMake only applies
         # *_INIT values when the cache entry is new: drop it so a changed address, port or
         # runtime always reaches the link.
         "-UCMAKE_EXE_LINKER_FLAGS", f"-DHBX_EXTRA_LINK={link_flags(variant, out)}",
         # The same goes for the compile flags: a changed HBX_COMMON has to reach an existing tree.
         "-UCMAKE_C_FLAGS", "-UCMAKE_CXX_FLAGS", "-UCMAKE_ASM_FLAGS",
         "-DHBX_SWITCH2=ON", f"-DHBX_SDK_INCLUDE={hbl_sdk() / 'sdk/include'}",
         "-DMKW_GUEST_FLAT_CHECKED_ONLY=ON", "-DMKW_AARCH64_CPU=cortex-a78c",
         "-DAURORA_DAWN_PROVIDER=vendor", "-DAURORA_DAWN_LINKAGE=static",
         f"-DFETCHCONTENT_SOURCE_DIR_DAWN={DEPS / 'dawn'}",
         "-DAURORA_SDL3_PROVIDER=vendor", "-DAURORA_SDL3_LINKAGE=static",
         f"-DFETCHCONTENT_SOURCE_DIR_SDL={DEPS / 'sdl3'}",
         "-DSDL_UNIX_CONSOLE_BUILD=ON", "-DSDL_OFFSCREEN=ON", *[f"-D{o}=OFF" for o in off], *extra])
    elf = out / PRODUCT
    elf.unlink(missing_ok=True)   # the runtime archive is a link option: always relink
    run(["cmake", "--build", out, "--target", PRODUCT, "--parallel", str(jobs)])
    print(f"built {elf}")


def pack(elf):
    sdk = hbl_sdk()
    required("HBXL_TOOLCHAIN", "bin/aarch64-linux-musl-nm")
    elf = (elf or BUILD / "console" / PRODUCT).resolve()
    if not elf.is_file():
        raise SystemExit(f"Build the console variant first; missing {elf}")
    run(["python3", sdk / "sdk/tools/hbxl-build.py", APP_DIR, "--pack", elf])
    # Distribute only the manifest and the images, never build caches, discs or saves.
    dest = HERE / "dist" / APP_ID
    (dest / "build").mkdir(parents=True, exist_ok=True)
    nm = required("HBXL_TOOLCHAIN") / "bin/aarch64-linux-musl-nm"
    symbols = subprocess.check_output([nm, "--defined-only", elf], text=True)
    manifest = dict(MANIFEST)
    if any(line.endswith(" T hbxl_storage_perf") for line in symbols.splitlines()):
        manifest["sd_assets"] = True
    (dest / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    for name in ("code.bin", "data.bin", "report.json"):
        shutil.copy2(APP_DIR / "build" / name, dest / "build" / name)
    write_fs_host_record(dest, FS_HOST)
    print(f"Package: {dest}")


def install():
    package = HERE / "dist" / APP_ID
    if not (package / "build/code.bin").exists():
        raise SystemExit("Pack first: build.py pack")
    destination = required("HBL_ROOT", "host") / "apps" / APP_ID
    if destination.exists():
        raise SystemExit(f"Install destination already exists: {destination}")
    shutil.copytree(package, destination)
    # The launcher's icon is made from the user's own game files, so it lives in the private root.
    icon = PRODUCTS[PRODUCT_NAME]["root"] / "icon.png"
    if icon.exists():
        shutil.copy2(icon, destination / "icon.png")
    print(f"Installed: {destination}")


def clone_tree(source, destination):
    """Copy-on-write where the filesystem supports it (APFS clones, Linux reflinks)."""
    if destination.exists():
        return
    destination.parent.mkdir(parents=True, exist_ok=True)
    if platform.system() == "Darwin":
        run(["cp", "-Rc", source, destination])
    else:
        run(["cp", "-a", "--reflink=auto", source, destination])


def fsroot(root, disc, rr, nand, rr_save):
    """The app sees the file-server root as its own folder (/proc/self/exe is /hbxl-app), so
    portable mode keeps Config.toml, NAND, caches and logs in <root>/UserData."""
    root = root.resolve()
    root.mkdir(parents=True, exist_ok=True)
    (root / "portable.txt").touch()
    # The runtime resources the desktop builds copy next to the executable, except the starter
    # pipeline cache: on the console, building its 1,101 pipelines at once at start-up ran out of
    # memory, so pipelines are built as the game first draws with them.
    assets = REPO / "runtime/assets"
    for source, name in ((assets / "dsp/dsp_coef.bin", "dsp_coef.bin"),
                         (assets / "certs/cacert.pem", "cacert.pem")):
        shutil.copy2(source, root / name)
    clone_tree(assets / "wii", root / "wii_bootstrap")
    # musl reads its DNS servers from <root>/etc/resolv.conf; without one every lookup fails and
    # Retro WFC can't be reached.
    resolv = root / "etc/resolv.conf"
    if not resolv.exists():
        resolv.parent.mkdir(parents=True, exist_ok=True)
        resolv.write_text("nameserver 1.1.1.1\nnameserver 8.8.8.8\n")
    if not (disc / "sys/fst.bin").exists():
        raise SystemExit(f"{disc} is not an extracted disc (needs files/ and sys/fst.bin)")
    clone_tree(disc, root / "disc/DATA")
    if retro_rewind():
        if not (rr / "Binaries/Code.pul").exists():
            raise SystemExit(f"{rr} is not a RetroRewind6 folder")
        clone_tree(rr, root / "RetroRewind/RetroRewind6")
    elif rr_save:
        raise SystemExit("--rr-save is a Retro Rewind save; plain WiiCompiled keeps its save in the NAND")
    if nand:
        clone_tree(nand, root / "UserData/NAND")
    if rr_save:
        save = root / "RetroRewind/riivolution/save/RetroWFC/RMCP"
        save.mkdir(parents=True, exist_ok=True)
        if not (save / "rksys.dat").exists():
            shutil.copy2(rr_save, save / "rksys.dat")
    config = root / "UserData/Config.toml"
    if not config.exists():
        config.parent.mkdir(parents=True, exist_ok=True)
        paths = '[paths]\ndvd_root = "/disc/DATA"\n'
        if retro_rewind():
            paths += 'retro_rewind_root = "/RetroRewind/RetroRewind6"\n'
        config.write_text('[video]\nwidescreen = true\nresolution_multiplier = 1.0\ngraphics_api = "auto"\n'
                          'gx_worker = true\n\n' + paths)
    env = root / "hbxl-env"
    if not env.exists():
        env.write_text("# HBXL_TRACE=1 logs every syscall the runtime does not implement\n"
                       "# MKW_PERF_STATS=1 logs FPS and frame timing\n"
                       "# MKW_CPU_BOOST=N boost mode at launch (default 1; 0 = off)\n"
                       "# MKW_RACE_WORKER=0 starts with the race GX worker off\n"
                       "# MKW_FRAME_SKIP=1 starts with offline frame skip on\n"
                       "# AURORA_PIPELINE_THREADS=N pipeline workers (default 2; 0 = on the frame thread)\n"
                       "# AURORA_PIPELINE_STACK=default puts their stacks in the Vulkan pool (default: SDK heap)\n"
                       "# AURORA_DAWN_ENABLE=a,b / AURORA_DAWN_DISABLE=a,b add Dawn device toggles\n")
    print(f"File-server root: {root}")


def current_address():
    """This machine's address on its default route, or None where that can't be found."""
    if platform.system() != "Darwin":
        return None
    route = subprocess.run(["route", "-n", "get", "default"], capture_output=True, text=True).stdout
    interface = re.search(r"interface:\s*(\S+)", route)
    if not interface:
        return None
    return subprocess.run(["ipconfig", "getifaddr", interface[1]], capture_output=True, text=True).stdout.strip() or None


def fs_host_bytes(address):
    return socket.inet_aton(address) + bytes(4)


def fs_host_slot(data, address):
    """The 8-byte slot in data.bin that holds the file server's address."""
    needle = fs_host_bytes(address)
    slots = [m.start() for m in re.finditer(re.escape(needle), data) if m.start() % 8 == 0]
    if len(slots) != 1:
        raise SystemExit(f"Found {address} {len(slots)} times in data.bin, not once; pack again with HBX_FS_HOST")
    return slots[0]


def write_fs_host_record(package, address):
    data = (package / "build/data.bin").read_bytes()
    record = {"offset": fs_host_slot(data, address)}
    (package / "build/fs-host.json").write_text(json.dumps(record) + "\n")


def retarget(package, address):
    """Point a packed app at this file-server address. The address is one word in data.bin, so a
    new DHCP lease needs no rebuild. Returns the address the app used before."""
    data_file = package / "build/data.bin"
    report_file = package / "build/report.json"
    record_file = package / "build/fs-host.json"
    data = bytearray(data_file.read_bytes())
    if record_file.exists():
        offset = json.loads(record_file.read_text())["offset"]
    else:
        offset = fs_host_slot(data, FS_HOST)
    if data[offset + 4:offset + 8] != bytes(4):
        raise SystemExit(f"{data_file} has no address at offset {offset}; pack and install again")
    before = socket.inet_ntoa(bytes(data[offset:offset + 4]))
    record_file.write_text(json.dumps({"offset": offset}) + "\n")
    if before == address:
        return before
    data[offset:offset + 8] = fs_host_bytes(address)
    report = json.loads(report_file.read_text())
    report["dataSha256"] = hashlib.sha256(data).hexdigest()
    data_file.write_bytes(data)
    report_file.write_text(json.dumps(report, indent=2) + "\n")
    return before


def serve(root):
    if not (root / "portable.txt").exists():
        raise SystemExit(f"{root} is not a file-server root; run build.py fsroot first")
    hbl = required("HBL_ROOT", "tools/hbxl-fileserver.py")
    # Routers hand out new addresses, so the installed app follows whatever this machine has now.
    address = current_address() or FS_HOST
    for package in (hbl / "apps" / APP_ID, HERE / "dist" / APP_ID):
        if (package / "build/data.bin").exists():
            before = retarget(package, address)
            if before != address:
                print(f"{package}: file server address {before} -> {address}")
    print(f"file server address: {address}")
    run(["python3", hbl / "tools/hbxl-fileserver.py", root.resolve(), "--host", address, "--port", str(FS_PORT)])


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("action", choices=["toolchain", "deps", "runtime", "translate", "build", "pack", "install",
                                           "fsroot", "serve", "address"])
    parser.add_argument("--product", choices=sorted(PRODUCTS), default="retro-rewind",
                        help="which game to build, pack, install, lay out or serve")
    parser.add_argument("--online", action="store_true", help="translate: include the Retro-WFC payload")
    parser.add_argument("--variant", choices=["console", "desktop"], default="console")
    parser.add_argument("--jobs", type=int, default=min(6, os.cpu_count() or 1))
    parser.add_argument("--elf", type=Path, help="pack: an already linked console ELF")
    parser.add_argument("--root", type=Path, help="fsroot/serve: the file-server root (default: the product's)")
    parser.add_argument("--disc", type=Path, default=REPO / "Assets/DATA", help="fsroot: extracted RMCP01 disc")
    parser.add_argument("--rr", type=Path, default=RR_PACK, help="fsroot: your RetroRewind6 folder")
    parser.add_argument("--nand", type=Path, help="fsroot: a NAND folder to start from (copied)")
    parser.add_argument("--rr-save", type=Path, help="fsroot: a Retro Rewind rksys.dat to start from (copied)")
    args, extra = parser.parse_known_args()
    select_product(args.product)
    args.root = args.root or PRODUCTS[args.product]["root"]
    if extra and args.action != "build":
        parser.error("Extra CMake arguments require build")
    if args.action == "toolchain":
        toolchain()
    elif args.action == "deps":
        deps()
    elif args.action == "runtime":
        runtime()
    elif args.action == "translate":
        translate(args.online)
    elif args.action == "build":
        build(args.variant, args.jobs, extra)
    elif args.action == "pack":
        pack(args.elf)
    elif args.action == "install":
        install()
    elif args.action == "fsroot":
        fsroot(args.root, args.disc, args.rr, args.nand, args.rr_save)
    elif args.action == "serve":
        serve(args.root)
    elif args.action == "address":
        address = current_address()
        if not address:
            raise SystemExit("No network address found. Is the Mac's network cable connected?")
        print(address)


if __name__ == "__main__":
    main()
