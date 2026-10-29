#!/bin/bash
# Run hblauncher/build.py's Linux actions in an arm64 container (e.g. Docker Desktop on Apple
# Silicon). The repository is synced into the docker volume "wiicompiled-hbx" (the translator
# writes host-format assembly and absolute paths, so Linux builds translate in their own tree);
# the game inputs (Assets/, PulsarPacks/, build/retro-wfc) are linked, never copied, and the
# finished package is copied back to hblauncher/dist.
#   HBL_ROOT=/path/to/HBLauncher hblauncher/docker/run.sh toolchain
#   HBL_ROOT=/path/to/HBLauncher hblauncher/docker/run.sh deps
#   ... runtime | translate --online | build | pack
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
REPO="$(cd "$HERE/.." && pwd)"
HBL_ROOT="$(cd "${HBL_ROOT:?Set HBL_ROOT to your HBLauncher checkout}" && pwd)"
IMAGE=wiicompiled-hbx-build
docker image inspect "$IMAGE" >/dev/null 2>&1 || docker build -t "$IMAGE" "$HERE/docker"
TTY=(); [[ -t 0 ]] && TTY=(-it)
exec docker run --rm ${TTY[@]+"${TTY[@]}"} \
  -v "$REPO:/host" -v "$HBL_ROOT:/hbl:ro" -v wiicompiled-hbx:/opt/work \
  -e HBL_ROOT=/hbl -e HBXL_TOOLCHAIN=/opt/work/toolchain \
  -e HBX_DEPS=/opt/work/deps -e HBX_BUILD=/opt/work/build \
  "$IMAGE" bash -c '
set -euo pipefail
SRC=/opt/work/src
mkdir -p $SRC/build
rsync -a --delete --exclude .git --exclude generated --exclude build --exclude "build-*" \
  --exclude Assets --exclude PulsarPacks --exclude hblauncher/deps --exclude hblauncher/build \
  --exclude hblauncher/dist --exclude hblauncher/fsroot --exclude hblauncher/app/build \
  --exclude "translator/*/*/bin" --exclude "translator/*/*/obj" /host/ $SRC/
ln -sfn /host/Assets $SRC/Assets
ln -sfn /host/PulsarPacks $SRC/PulsarPacks
ln -sfn /host/build/retro-wfc $SRC/build/retro-wfc
python3 $SRC/hblauncher/build.py "$@"
if [[ -d $SRC/hblauncher/dist ]]; then
  mkdir -p /host/hblauncher/dist && rsync -a $SRC/hblauncher/dist/ /host/hblauncher/dist/
fi
' run.sh "$@"
