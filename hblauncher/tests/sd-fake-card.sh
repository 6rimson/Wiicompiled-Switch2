#!/bin/bash
# Builds tests/sd-fake-card.c with the port's musl toolchain and runs it in the build image.
# Needs the toolchain and deps in the Docker volume (docker/run.sh toolchain, deps).
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
IMAGE="${HBX_IMAGE:-wiicompiled-hbx-build:$(shasum -a 256 "$HERE/docker/Dockerfile" | cut -c1-12)}"
docker run --rm -v "${HBX_VOLUME:-wiicompiled-hbx}:/opt/work:ro" -v "$HERE:/h:ro" "$IMAGE" bash -c '
set -e
SDK=/opt/work/deps/hblauncher/sdk
/opt/work/toolchain/bin/aarch64-linux-musl-gcc -static -O1 -g -Wall -Wextra -Wno-unused-result \
  -I$SDK/include -I$SDK/linux /h/sd/storage.c /h/sd/directory.c /h/sd/vfs.c /h/tests/sd-fake-card.c \
  -o /tmp/sd-fake-card
/tmp/sd-fake-card'
