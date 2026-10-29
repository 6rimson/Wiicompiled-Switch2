#!/usr/bin/env python3
"""Build, package, install or serve WiiCompiled + Retro Rewind as an HBLauncher app (Switch 2).

Build actions (deps, runtime, translate, build, pack) run on aarch64 Linux - the provided
container (hblauncher/docker/run.sh) or a native machine. fsroot, install and serve run on the PC
that drives the HBLauncher session (macOS or Linux).

  toolchain   build HBLauncher's patched aarch64-linux-musl toolchain into $HBXL_TOOLCHAIN
  deps        fetch Dawn and SDL3 at their pins and apply the HBLauncher patches; copy the
              HBLauncher SDK and apply this port's runtime patch
  runtime     build libhbxl.a (HBLauncher's Linux runtime) from the patched SDK copy
  translate   translate the base game and Retro Rewind (add --online for the Retro-WFC payload)
  build       configure and build (--variant console: HBLauncher link; desktop: headless test)
  pack        split the console ELF into an HBLauncher package under hblauncher/dist/<id>
  install     copy the package into $HBL_ROOT/apps (never overwrites)
  fsroot      lay out a private file-server root from your own disc, RR folder and saves
  serve       run HBLauncher's file server on this app's port

Environment: HBL_ROOT (HBLauncher checkout), HBXL_TOOLCHAIN (patched musl toolchain),
HBX_DEPS (default hblauncher/deps), HBX_BUILD (default hblauncher/build).
"""
import argparse
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
MANIFEST = json.loads((HERE / "app/manifest.json").read_text())
PINS = json.loads((HERE / "dependencies.json").read_text())
APP_ID = MANIFEST["id"]
FS_PORT = MANIFEST["linux"]["fs_port"]
DEPS = Path(os.environ.get("HBX_DEPS", HERE / "deps"))
BUILD = Path(os.environ.get("HBX_BUILD", HERE / "build"))
PATCHES = HERE / "patches"
PRODUCT = "RetroRewind"
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
    apply_patch(sdk, PATCHES / "hblauncher-runtime-file-locks.diff")


def runtime():
    require_linux_aarch64()
    required("HBXL_TOOLCHAIN", "bin/aarch64-linux-musl-gcc")
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
    for path in (REPO / "Assets/main.dol", REPO / "Assets/StaticR.rel", RR_PACK / "Binaries/Code.pul"):
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
    (REPO / "generated/build_shards").mkdir(parents=True, exist_ok=True)
    translator("emit-build-shards", *project, "--base-metadata", "generated/base_translation_output.json",
               "--base-functions-dir", "generated/functions", "--native-source-dir", "runtime/src",
               "--out", "generated/build_shards",
               "--resolved-profile", "build/mods/retro_rewind_full_cpp/resolved_dispatch_profile.json",
               "--retro-cpp-dir", "build/mods/retro_rewind_full_cpp/cpp")


def apply_aurora_patch():
    aurora = REPO / "aurora-main"
    if not (aurora / "CMakeLists.txt").exists():
        raise SystemExit("Run git submodule update --init aurora-main first")
    apply_patch(aurora, PATCHES / PINS["aurora"]["patch"])


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
    ld = hbl_sdk() / "sdk/linux/hbxl.ld"
    linux = MANIFEST["linux"]
    if any(c.isspace() for c in f"{ld}{lib}"):
        raise SystemExit("Use build and SDK paths without whitespace")
    return " ".join([
        "-nostartfiles", f"-Wl,-T,{ld}",
        f"-Wl,--defsym=HBXL_CODE_BASE={linux['code_base']}", f"-Wl,--defsym=HBXL_DATA_BASE={linux['data_base']}",
        f"-Wl,--defsym=HBXL_FS_PORT={FS_PORT}",
        "-Wl,--eh-frame-hdr", "-Wl,-z,noexecstack", "-Wl,--build-id=none",
        f"-Wl,--whole-archive {lib} -Wl,--no-whole-archive"])


def build(variant, jobs, extra):
    require_linux_aarch64()
    required("HBXL_TOOLCHAIN", "bin/aarch64-linux-musl-gcc")
    if not (REPO / "generated/build_shards/shards.cmake").exists():
        raise SystemExit("Translate first: build.py translate [--online]")
    apply_aurora_patch()
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
         f"-DHBX_EXTRA_LINK={link_flags(variant, out)}",
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
    run(["python3", sdk / "sdk/tools/hbxl-build.py", HERE / "app", "--pack", elf])
    # Distribute only the manifest and the images, never build caches, discs or saves.
    dest = HERE / "dist" / APP_ID
    (dest / "build").mkdir(parents=True, exist_ok=True)
    shutil.copy2(HERE / "app/manifest.json", dest / "manifest.json")
    for name in ("code.bin", "data.bin", "report.json"):
        shutil.copy2(HERE / "app/build" / name, dest / "build" / name)
    print(f"Package: {dest}")


def install():
    package = HERE / "dist" / APP_ID
    if not (package / "build/code.bin").exists():
        raise SystemExit("Pack first: build.py pack")
    destination = required("HBL_ROOT", "host") / "apps" / APP_ID
    if destination.exists():
        raise SystemExit(f"Install destination already exists: {destination}")
    shutil.copytree(package, destination)
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
    # The same runtime resources the desktop builds copy next to the executable.
    assets = REPO / "runtime/assets"
    for source, name in ((assets / "dsp/dsp_coef.bin", "dsp_coef.bin"),
                         (assets / "pipeline/initial_pipeline_cache.db", "initial_pipeline_cache.db"),
                         (assets / "certs/cacert.pem", "cacert.pem")):
        shutil.copy2(source, root / name)
    clone_tree(assets / "wii", root / "wii_bootstrap")
    if not (disc / "sys/fst.bin").exists():
        raise SystemExit(f"{disc} is not an extracted disc (needs files/ and sys/fst.bin)")
    clone_tree(disc, root / "disc/DATA")
    if not (rr / "Binaries/Code.pul").exists():
        raise SystemExit(f"{rr} is not a RetroRewind6 folder")
    clone_tree(rr, root / "RetroRewind/RetroRewind6")
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
        config.write_text('[video]\nwidescreen = true\nresolution_multiplier = 1.0\ngraphics_api = "auto"\n\n'
                          '[paths]\ndvd_root = "/disc/DATA"\nretro_rewind_root = "/RetroRewind/RetroRewind6"\n')
    env = root / "hbxl-env"
    if not env.exists():
        env.write_text("# HBXL_TRACE=1 logs every syscall the runtime does not implement\n")
    print(f"File-server root: {root}")


def serve(root):
    if not (root / "portable.txt").exists():
        raise SystemExit(f"{root} is not a file-server root; run build.py fsroot first")
    hbl = required("HBL_ROOT", "tools/hbxl-fileserver.py")
    run(["python3", hbl / "tools/hbxl-fileserver.py", root.resolve(), "--port", str(FS_PORT)])


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("action", choices=["toolchain", "deps", "runtime", "translate", "build", "pack", "install",
                                           "fsroot", "serve"])
    parser.add_argument("--online", action="store_true", help="translate: include the Retro-WFC payload")
    parser.add_argument("--variant", choices=["console", "desktop"], default="console")
    parser.add_argument("--jobs", type=int, default=min(6, os.cpu_count() or 1))
    parser.add_argument("--elf", type=Path, help="pack: an already linked console ELF")
    parser.add_argument("--root", type=Path, default=HERE / "fsroot", help="fsroot/serve: the file-server root")
    parser.add_argument("--disc", type=Path, default=REPO / "Assets/DATA", help="fsroot: extracted RMCP01 disc")
    parser.add_argument("--rr", type=Path, default=RR_PACK, help="fsroot: your RetroRewind6 folder")
    parser.add_argument("--nand", type=Path, help="fsroot: a NAND folder to start from (copied)")
    parser.add_argument("--rr-save", type=Path, help="fsroot: a Retro Rewind rksys.dat to start from (copied)")
    args, extra = parser.parse_known_args()
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


if __name__ == "__main__":
    main()
