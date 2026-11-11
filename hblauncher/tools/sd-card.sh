#!/bin/bash
# Prepares a microSD card for MKW_SD=all (experimental) and copies progress back.
#
#   sd-card.sh push [--root DIR] [--card /Volumes/NAME] [--force]
#       Copies the file-server root (game data, NAND, saves, settings, caches) to <card>/wiicompiled
#       and marks the card prepared (/wiicompiled/.fsroot). With MKW_SD=all the game then uses the
#       card as the only copy of its saves, settings and caches. Refuses a card that is already
#       prepared, since that card may hold newer progress than the PC: pull first, or --force.
#       The root's hbxl-env goes to the card too, with MKW_SD=all, for starting without the server.
#   sd-card.sh pull [--root DIR] [--card /Volumes/NAME]
#       Copies the NAND, settings, caches and Retro Rewind's save from the card back to the root,
#       after backing up the root's copies to UserData/save-backup-<time>-before-pull.
#
# DIR defaults to hblauncher/fsroot; NAME to SWITCH2. Insert the card in the Mac first.
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
action="${1:-}"
shift || true
root="$HERE/fsroot"
card="/Volumes/SWITCH2"
force=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --root) root="${2:?}"; shift 2 ;;
    --card) card="${2:?}"; shift 2 ;;
    --force) force=1; shift ;;
    *) echo "Unknown option: $1" >&2; exit 2 ;;
  esac
done
[[ -d "$root/UserData/NAND" ]] || { echo "No file-server root with a NAND at $root" >&2; exit 1; }
[[ -d "$card" ]] || { echo "No card at $card. Insert it, or pass --card /Volumes/NAME." >&2; exit 1; }
dest="$card/wiicompiled"
# FAT keeps timestamps to 2 s and no permissions or owners.
RSYNC=(rsync -rt --modify-window=2 --exclude .DS_Store --exclude '._*')

case "$action" in
push)
  if [[ -f "$dest/.fsroot" && $force -eq 0 ]]; then
    echo "$card is already prepared and may hold newer progress than $root." >&2
    echo "Run 'sd-card.sh pull' first, or add --force to replace it." >&2
    exit 1
  fi
  mkdir -p "$dest"
  rm -f "$dest/.fsroot"  # an interrupted copy leaves the card unprepared
  # The writable folders become exact copies (--delete), so nothing stale is left behind, such as
  # an old SQLite journal. Game data only gets new and changed files.
  "${RSYNC[@]}" --delete --exclude 'save-backup-*' --exclude 'Cache.run3-backup' --exclude Captures \
    "$root/UserData/" "$dest/UserData/"
  if [[ -d "$root/RetroRewind/riivolution" ]]; then
    mkdir -p "$dest/RetroRewind"
    "${RSYNC[@]}" --delete "$root/RetroRewind/riivolution/" "$dest/RetroRewind/riivolution/"
  fi
  "${RSYNC[@]}" --exclude UserData --exclude 'RetroRewind/riivolution' --exclude 'RetroRewind/*-backup' \
    --exclude fs-run.log --exclude session.env --exclude 'hbxl-env*' "$root/" "$dest/"
  # The settings the game reads when it starts without the file server: the root's, with the card
  # as the only copy (MKW_SD=all; earlier lines win).
  { echo "MKW_SD=all"; [[ -f "$root/hbxl-env" ]] && grep -v '^MKW_SD=' "$root/hbxl-env"; true; } > "$dest/hbxl-env"
  printf 'prepared from %s on %s\n' "$root" "$(date '+%Y-%m-%d %H:%M')" > "$dest/.fsroot"
  # macOS keeps file metadata on FAT in ._ files, which dot_clean can't always merge there.
  find "$dest" -name '._*' -delete
  sync
  echo "Prepared $card from $root. Eject it before taking it out."
  ;;
pull)
  [[ -f "$dest/.fsroot" ]] || { echo "$card isn't prepared; nothing to pull." >&2; exit 1; }
  backup="$root/UserData/save-backup-$(date +%Y-%m-%d-%H%M)-before-pull"
  mkdir -p "$backup"
  cp -Rp "$root/UserData/NAND" "$backup/NAND"
  [[ -f "$root/UserData/Config.toml" ]] && cp -p "$root/UserData/Config.toml" "$backup/"
  if [[ -d "$root/RetroRewind/riivolution/save" ]]; then
    cp -Rp "$root/RetroRewind/riivolution/save" "$backup/riivolution-save"
  fi
  # Exact copies (--delete): a cache database with another copy's leftover journal is damaged.
  "${RSYNC[@]}" --delete "$dest/UserData/NAND/" "$root/UserData/NAND/"
  "${RSYNC[@]}" --delete "$dest/UserData/Cache/" "$root/UserData/Cache/"
  [[ -f "$dest/UserData/Config.toml" ]] && "${RSYNC[@]}" "$dest/UserData/Config.toml" "$root/UserData/"
  if [[ -d "$dest/RetroRewind/riivolution/save" ]]; then
    "${RSYNC[@]}" --delete "$dest/RetroRewind/riivolution/save/" "$root/RetroRewind/riivolution/save/"
  fi
  echo "Copied the card's progress to $root. The previous copies are in $backup."
  ;;
*)
  echo "Usage: sd-card.sh push|pull [--root DIR] [--card /Volumes/NAME] [--force]" >&2
  exit 2
  ;;
esac
