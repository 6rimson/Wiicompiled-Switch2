#!/bin/bash
# Runs one part of a console session with this Mac's current address, so a new DHCP lease needs
# no edits. Usage: session.sh server [build.py serve options] | runner | launcher [hbl-start options]
# Prepare the HBLauncher and ns2jb checkouts with tools/setup-session.py first.
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
# Where this machine keeps its checkouts: fsroot is private, so a local paths file can live there.
config="${HBL_SESSION_ENV:-$HERE/fsroot/session.env}"
if [[ -f "$config" ]]; then
  source "$config"
fi
export HBL_ROOT="${HBL_ROOT:?Set HBL_ROOT to your HBLauncher checkout, in fsroot/session.env or the environment}"
NS2JB="${HBL_NS2JB:?Set HBL_NS2JB to your ns2jb checkout, in fsroot/session.env or the environment}"
ADDRESS="$(python3 "$HERE/build.py" address)"
part="${1:-}"
shift || true
case "$part" in
server)
  cd "$HERE/.."
  root="$HERE/fsroot"
  [[ " $* " == *" wiicompiled "* || "$*" == *"--product=wiicompiled"* ]] && root="$HERE/fsroot-wiicompiled"
  python3 hblauncher/build.py serve "$@" 2>&1 | tee "${HBX_SERVER_LOG:-$root/fs-run.log}"
  ;;
runner)
  cd "$NS2JB"
  exec node run.cjs --rpc "$ADDRESS"
  ;;
launcher)
  cd "$HBL_ROOT"
  export HBL_NS2JB="$NS2JB"
  export HBL_SD_SESSION="$HERE/tools/sd-session.cjs"
  export HBL_RPC_TARGET="$ADDRESS"
  export HBL_PAYLOAD="$NS2JB/engine/switch2/ro/payload/main"
  # The patched helper setup-session.py built; the launcher refuses any other.
  export HBL_BULK_SHA256="$(python3 -c 'import json, sys; print(json.load(open(sys.argv[1]))["ns2jb"]["payload"]["sha256"])' "$HERE/dependencies.json")"
  export HBL_ARCHIVE_SDK="$PWD/reference/hblauncher-cpu-sdk/homebrew-launcher"
  export HBL_HID_SYMBOLS="$PWD/reference/hblauncher-cpu-sdk/sdk/native/hid-functions.json"
  # HBLauncher stops after 64 launcher turns by default, about 15 minutes in its menu. Ask for
  # 10,000 (about 40 hours) unless --entries is given.
  [[ " $* " == *" --entries "* ]] || set -- --entries 10000 "$@"
  # A second launcher in the same SmileBASIC session reuses the resident launcher but can't free
  # what the first one's game left mapped, and the next game launch closes SmileBASIC. Stop if
  # this ns2jb session already launched a game. Relaunching from HBLauncher's menu is fine.
  if [[ " $* " == *" --reuse-session "* ]]; then
    args=(); for a in "$@"; do [[ "$a" == --reuse-session ]] || args+=("$a"); done; set -- ${args[@]+"${args[@]}"}
  else
    RUN="$(ls -t "$NS2JB/runs" 2>/dev/null | head -1)"
    if [[ -n "$RUN" && -f build/hbl-session.log ]] && python3 - "$RUN" build/hbl-session.log <<'EOF'
import json, sys
run, launched, current = sys.argv[1], False, False
for line in open(sys.argv[2], errors="replace"):
    try:
        event = json.loads(line)
    except ValueError:
        continue
    if event.get("message") == "open":
        current = str(event.get("data", {}).get("run", "")).endswith("/" + run)
    elif event.get("message") == "launch" and current:
        launched = True
sys.exit(0 if launched else 1)
EOF
    then
      echo "A game already ran in this SmileBASIC session ($RUN). Starting another launcher in it" >&2
      echo "makes the next game launch close SmileBASIC. Close SmileBASIC, open it again, and start" >&2
      echo "a new runner and 4/4 session first, or relaunch from HBLauncher's menu instead." >&2
      echo "(--reuse-session starts it anyway.)" >&2
      exit 1
    fi
  fi
  exec ./hbl-start "$@"
  ;;
*)
  echo "Usage: $0 server | runner | launcher [hbl-start options]" >&2
  exit 2
  ;;
esac
