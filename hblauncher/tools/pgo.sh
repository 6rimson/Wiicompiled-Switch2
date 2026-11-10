#!/bin/bash
# Records a profile for profile-guided optimization (PGO) of the console build.
# Usage: pgo.sh [seconds] [--product retro-rewind|wiicompiled]
#
# 1. Builds the desktop variant with MKW_PGO=generate. It compiles the same code as the console
#    build; only the final link differs.
# 2. Plays it headless and offline in Docker for <seconds> (default 900): MKW_AUTOPAD=1 presses A
#    through the menus into a Grand Prix. Your save and NAND are copied into the throwaway
#    container, so nothing in fsroot changes.
# 3. Merges the recording into hblauncher/pgo/<product>.profdata.xz, the profile `build` uses.
#
# The profile covers the base game and the runtime, so a new Retro Rewind doesn't need a new one;
# re-record after larger changes to the runtime or the translator. Then build and pack again.
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
if [[ -f "$HERE/fsroot/session.env" ]]; then
  source "$HERE/fsroot/session.env"
fi
export HBL_ROOT="${HBL_ROOT:?Set HBL_ROOT to your HBLauncher checkout, in fsroot/session.env or the environment}"

SECONDS_TO_PLAY=900
PRODUCT=retro-rewind
while [[ $# -gt 0 ]]; do
  case "$1" in
    --product) PRODUCT="${2:?--product needs a name}"; shift 2 ;;
    --product=*) PRODUCT="${1#--product=}"; shift ;;
    [0-9]*) SECONDS_TO_PLAY="$1"; shift ;;
    *) echo "usage: pgo.sh [seconds] [--product retro-rewind|wiicompiled]" >&2; exit 2 ;;
  esac
done
case "$PRODUCT" in
  retro-rewind) TARGET=RetroRewind; ROOT="$HERE/fsroot" ;;
  wiicompiled) TARGET=WiiCompiled; ROOT="$HERE/fsroot-wiicompiled" ;;
  *) echo "Unknown product: $PRODUCT" >&2; exit 2 ;;
esac
[[ -d "$ROOT/UserData" && -d "$ROOT/disc" ]] || { echo "Lay out $ROOT first: build.py fsroot" >&2; exit 1; }

"$HERE/docker/run.sh" build --variant desktop --product "$PRODUCT" -DMKW_PGO=generate

# The same image and volume as docker/run.sh.
IMAGE="${HBX_IMAGE:-wiicompiled-hbx-build:$(shasum -a 256 "$HERE/docker/Dockerfile" | cut -c1-12)}"
mkdir -p "$HERE/pgo"
MOUNTS=(-v "${HBX_VOLUME:-wiicompiled-hbx}:/opt/work" -v "$ROOT:/host-fsroot:ro" -v "$ROOT/disc:/disc:ro"
        -v "$HERE/pgo:/out")
if [[ -d "$ROOT/RetroRewind/RetroRewind6" ]]; then
  MOUNTS+=(-v "$ROOT/RetroRewind/RetroRewind6:/RetroRewind/RetroRewind6:ro")
fi
echo "Recording $SECONDS_TO_PLAY s of play; this runs on its own."
docker run --rm --network none "${MOUNTS[@]}" \
  -e TARGET="$TARGET" -e PRODUCT="$PRODUCT" -e PLAY="$SECONDS_TO_PLAY" "$IMAGE" bash -c '
set -euo pipefail
# The console layout: portable.txt and UserData at /, the game files where Config.toml points.
cp -a /host-fsroot/UserData /UserData
cp -a /host-fsroot/portable.txt /
if [[ -d /host-fsroot/RetroRewind/riivolution ]]; then
  cp -a /host-fsroot/RetroRewind/riivolution /RetroRewind/riivolution
fi
cp -a /host-fsroot/etc/. /etc/ 2>/dev/null || true
mkdir -p /app
cp /opt/work/build/desktop/$TARGET /host-fsroot/dsp_coef.bin /app/
cp -a /host-fsroot/wii_bootstrap /host-fsroot/cacert.pem /app/
RAW=/opt/work/pgo/raw-$PRODUCT
rm -rf $RAW && mkdir -p $RAW
cd /
# pgo_profile_dump.cpp writes the recording when timeout stops the game.
MKW_AUTOPAD=1 MKW_PERF_STATS=1 SDL_VIDEO_DRIVER=offscreen SDL_AUDIO_DRIVER=dummy \
  LLVM_PROFILE_FILE=$RAW/%p.profraw timeout -s TERM "$PLAY" /app/$TARGET \
  > /opt/work/pgo/$PRODUCT-record.log 2>&1 || true
ls $RAW/*.profraw >/dev/null 2>&1 || { echo "No recording was written; see /opt/work/pgo/$PRODUCT-record.log"; exit 1; }
echo "seconds spent racing: $(grep -a -c "race ticks [1-9]" /opt/work/pgo/$PRODUCT-record.log || true)"
llvm-profdata-22 merge -o /tmp/$PRODUCT.profdata $RAW/*.profraw
xz -9 -c /tmp/$PRODUCT.profdata > /out/$PRODUCT.profdata.xz
echo "Profile: hblauncher/pgo/$PRODUCT.profdata.xz"
'
