#!/usr/bin/env python3
"""Prepare the ns2jb and HBLauncher checkouts a session runs from.

This port needs both at pinned commits with this repository's patches:
  ns2jb       range mapping commands (0x524f001a-c) in the RPC dispatcher and native helper,
              plus a rebuilt helper, its manifest and the repository's SHA256SUMS;
  HBLauncher  host-side range mapping, failure handling and split large writes, plus the
              inspectors and HID signatures from HBLauncher's own history (commit 9d00834).

Usage:
  hblauncher/tools/setup-session.py --hblauncher DIR --ns2jb DIR --zig /path/to/zig
  hblauncher/tools/setup-session.py --hblauncher DIR --ns2jb DIR --check

Both existing checkouts are inspected before anything is changed. A missing folder is cloned
at the pinned commit; an existing one must already be at it. Nothing here resets, checks out or
discards work in an existing checkout. Running it again on a prepared checkout changes nothing.
--check only reports. HBLauncher's launcher itself is built separately (docker/run.sh launcher).
"""
import argparse
import hashlib
import json
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent.parent
PINS = json.loads((HERE / "dependencies.json").read_text())
PATCHES = HERE / "patches"
HBL_PATCH = PATCHES / PINS["hblauncher"]["host_patch"]
SD_HOST_PATCH = PATCHES / "hblauncher-host-sd-assets.diff"
NS2JB_PATCH = PATCHES / PINS["ns2jb"]["patch"]
REFERENCE = PINS["hblauncher"]["reference"]
PAYLOAD = PINS["ns2jb"]["payload"]
changes = []


class Stop(Exception):
    pass


def git(directory, *args, check=True):
    result = subprocess.run(["git", "-C", str(directory), *args], capture_output=True, text=True)
    if check and result.returncode != 0:
        raise Stop(f"git {' '.join(args)} failed in {directory}:\n{result.stderr.strip()}")
    return result


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def patch_state(directory, patch_file):
    """'applied', 'applicable', or a Stop explaining the overlap."""
    if git(directory, "apply", "--reverse", "--check", str(patch_file), check=False).returncode == 0:
        return "applied"
    forward = git(directory, "apply", "--check", str(patch_file), check=False)
    if forward.returncode == 0:
        return "applicable"
    raise Stop(f"{patch_file.name} neither applies nor is already applied in {directory}.\n"
               f"Local edits overlap it. git said:\n{forward.stderr.strip()}")


def inspect(name, directory, commit):
    """Check an existing checkout without changing it. Returns the work it still needs."""
    if not directory.exists():
        return {"clone": True}
    if not (directory / ".git").exists():
        raise Stop(f"{name}: {directory} exists but is not a git checkout")
    head = git(directory, "rev-parse", "HEAD").stdout.strip()
    if head != commit:
        raise Stop(f"{name}: {directory} is at {head}, not the pinned {commit}. Use a new folder, "
                   f"or check out {commit[:7]} there yourself once any work in it is saved.")
    return {"clone": False}


def inspect_hblauncher(directory):
    work = inspect("HBLauncher", directory, PINS["hblauncher"]["commit"])
    if work["clone"]:
        return work
    work["patch"] = patch_state(directory, HBL_PATCH)
    work["sd_patch"] = patch_state(directory, SD_HOST_PATCH)
    if git(directory, "cat-file", "-e", REFERENCE["commit"] + "^{commit}", check=False).returncode != 0:
        raise Stop(f"HBLauncher: commit {REFERENCE['commit'][:7]} is missing from {directory} "
                   "(a shallow clone?). Run git fetch --unshallow there, then try again.")
    for relative, expected in REFERENCE["files"].items():
        target = directory / REFERENCE["directory"] / relative
        if target.exists() and sha256(target) != expected:
            raise Stop(f"HBLauncher: {target} differs from {REFERENCE['commit'][:7]}:{relative}. "
                       "Move it aside to have it rewritten.")
    return work


def inspect_ns2jb(directory):
    work = inspect("ns2jb", directory, PINS["ns2jb"]["commit"])
    if work["clone"]:
        work["build"] = True
        return work
    work["patch"] = patch_state(directory, NS2JB_PATCH)
    work["build"] = sha256(directory / PAYLOAD["path"]) != PAYLOAD["sha256"]
    return work


def clone(name, directory, url, commit):
    subprocess.run(["git", "clone", "--quiet", url, str(directory)], check=True)
    git(directory, "checkout", "--quiet", "--detach", commit)
    changes.append(f"cloned {name} into {directory}")
    print(f"{name}: cloned {url} at {commit[:7]}")


def apply(name, directory, patch_file):
    if patch_state(directory, patch_file) == "applied":
        print(f"{name}: {patch_file.name} already applied")
        return
    git(directory, "apply", str(patch_file))
    changes.append(f"applied {patch_file.name} to {directory}")
    print(f"{name}: applied {patch_file.name}")


def write_reference_files(directory):
    """Write the archived inspectors and HID signatures from HBLauncher's own history."""
    for relative, expected in REFERENCE["files"].items():
        target = directory / REFERENCE["directory"] / relative
        if target.exists():
            continue
        content = subprocess.run(["git", "-C", str(directory), "show", f"{REFERENCE['commit']}:{relative}"],
                                 capture_output=True, check=True).stdout
        if hashlib.sha256(content).hexdigest() != expected:
            raise Stop(f"HBLauncher: {REFERENCE['commit'][:7]}:{relative} has an unexpected hash")
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(content)
        changes.append(f"wrote {target}")
    print(f"HBLauncher: reference files match {REFERENCE['commit'][:7]}")


def build_helper(directory, zig):
    """Rebuild the native helper from the patched source and check it against the pinned hash."""
    image = directory / PAYLOAD["path"]
    if sha256(image) == PAYLOAD["sha256"]:
        if zig:
            subprocess.run(["node", "engine/build-payload.cjs", "--check", "--zig", str(zig)],
                           cwd=directory, check=True)
            print("ns2jb: the helper rebuilds byte for byte from its source")
        else:
            print("ns2jb: helper matches the pinned hash (pass --zig to also rebuild-check it)")
    else:
        subprocess.run(["node", "engine/build-payload.cjs", "--zig", str(zig)], cwd=directory, check=True)
        changes.append(f"rebuilt {image} and its manifest.json")
        if sha256(image) != PAYLOAD["sha256"]:
            raise Stop(f"ns2jb: the rebuilt helper is {sha256(image)}, not the pinned {PAYLOAD['sha256']}. "
                       f"Check that zig is exactly {PINS['ns2jb']['zig']}. The new main and manifest.json "
                       "were left in place for inspection.")
        print("ns2jb: rebuilt the helper; it matches the pinned hash")
    manifest = json.loads((image.parent / "manifest.json").read_text())
    if manifest.get("payloadSha256") != PAYLOAD["sha256"] or manifest.get("payloadBytes") != PAYLOAD["bytes"]:
        raise Stop("ns2jb: payload/manifest.json does not describe the helper")


def expected_checksums(directory):
    """ns2jb's SHA256SUMS: its tracked files and the files the patch adds."""
    lines = NS2JB_PATCH.read_text().splitlines()
    added = [after[len("+++ b/"):] for before, after in zip(lines, lines[1:])
             if before == "--- /dev/null" and after.startswith("+++ b/")]
    files = sorted(set(git(directory, "ls-files").stdout.splitlines() + added) - {"SHA256SUMS"})
    return "".join(f"{sha256(directory / f)}  {f}\n" for f in files)


def write_checksums(directory):
    sums = directory / "SHA256SUMS"
    text = expected_checksums(directory)
    if sums.read_text() == text:
        print("ns2jb: SHA256SUMS is current")
        return
    sums.write_text(text)
    changes.append(f"updated {sums}")
    print("ns2jb: updated SHA256SUMS")


def check_zig(zig):
    if zig is None:
        raise Stop(f"ns2jb's helper must be rebuilt. Install Zig {PINS['ns2jb']['zig']} and pass "
                   "--zig /absolute/path/to/zig")
    version = subprocess.run([str(zig), "version"], capture_output=True, text=True).stdout.strip()
    if version != PINS["ns2jb"]["zig"]:
        raise Stop(f"{zig} is Zig {version or '(unknown)'}; the helper needs exactly {PINS['ns2jb']['zig']}")


def report(hbl, ns2jb, hbl_work, ns2jb_work):
    """--check: say what a real run would still do, without doing it."""
    todo = []
    if hbl_work["clone"]:
        todo.append(f"clone HBLauncher into {hbl}")
    else:
        if hbl_work["patch"] == "applicable":
            todo.append(f"apply {HBL_PATCH.name}")
        if hbl_work["sd_patch"] == "applicable":
            todo.append(f"apply {SD_HOST_PATCH.name}")
        missing = [r for r in REFERENCE["files"] if not (hbl / REFERENCE["directory"] / r).exists()]
        if missing:
            todo.append(f"write {len(missing)} reference files")
        else:
            print(f"HBLauncher: reference files match {REFERENCE['commit'][:7]}")
    if ns2jb_work["clone"]:
        todo.append(f"clone ns2jb into {ns2jb}")
    else:
        if ns2jb_work["patch"] == "applicable":
            todo.append(f"apply {NS2JB_PATCH.name}")
        if ns2jb_work["build"]:
            todo.append("rebuild the helper")
        else:
            print("ns2jb: helper matches the pinned hash")
        if ns2jb_work["patch"] == "applied" and (ns2jb / "SHA256SUMS").read_text() != expected_checksums(ns2jb):
            todo.append("update SHA256SUMS")
    for name, work in (("HBLauncher", hbl_work), ("ns2jb", ns2jb_work)):
        if work.get("patch") == "applied":
            print(f"{name}: at its pinned commit, patch applied")
    if todo:
        raise Stop("not ready; a real run would " + "; ".join(todo))


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--hblauncher", required=True, type=Path, help="HBLauncher checkout (cloned if missing)")
    parser.add_argument("--ns2jb", required=True, type=Path, help="ns2jb checkout (cloned if missing)")
    parser.add_argument("--zig", type=Path, help=f"Zig {PINS['ns2jb']['zig']}, to rebuild ns2jb's helper")
    parser.add_argument("--check", action="store_true", help="report only; change nothing")
    parser.add_argument("--hblauncher-url", default=PINS["hblauncher"]["repository"])
    parser.add_argument("--ns2jb-url", default=PINS["ns2jb"]["repository"])
    args = parser.parse_args()
    sys.stdout.reconfigure(line_buffering=True)  # keep our lines in order with the builder's
    if shutil.which("node") is None:
        raise SystemExit("Node.js is required (ns2jb asks for 22 or newer)")
    hbl, ns2jb = args.hblauncher.expanduser().resolve(), args.ns2jb.expanduser().resolve()
    zig = args.zig.expanduser().resolve() if args.zig else None
    try:
        # Inspect both before changing either.
        hbl_work, ns2jb_work = inspect_hblauncher(hbl), inspect_ns2jb(ns2jb)
        if args.check:
            report(hbl, ns2jb, hbl_work, ns2jb_work)
        else:
            if ns2jb_work["build"] or zig:
                check_zig(zig)
            if hbl_work["clone"]:
                clone("HBLauncher", hbl, args.hblauncher_url, PINS["hblauncher"]["commit"])
            if ns2jb_work["clone"]:
                clone("ns2jb", ns2jb, args.ns2jb_url, PINS["ns2jb"]["commit"])
            apply("HBLauncher", hbl, HBL_PATCH)
            apply("HBLauncher", hbl, SD_HOST_PATCH)
            write_reference_files(hbl)
            apply("ns2jb", ns2jb, NS2JB_PATCH)
            build_helper(ns2jb, zig)
            write_checksums(ns2jb)
    except (Stop, subprocess.CalledProcessError) as error:
        done = "".join(f"\n  - {change}" for change in changes)
        sys.exit(f"\nStopped: {error}\n" + (f"Changes made before stopping:{done}" if changes
                                            else "Nothing was changed."))
    if args.check:
        print("\nReady; nothing was changed.")
    else:
        print(f"\nReady. Put these in hblauncher/fsroot/session.env:\n"
              f"HBL_ROOT={hbl}\nHBL_NS2JB={ns2jb}\n"
              "After changing the helper, start a fresh SmileBASIC session and runner: "
              "old run folders keep the helper they started with.")


if __name__ == "__main__":
    main()
