#!/bin/bash
# Runs one part of a console session with this Mac's current address, so a new DHCP lease needs
# no edits. Usage: session.sh server | runner | launcher [hbl-start options]
# Prepare the HBLauncher and ns2jb checkouts with tools/setup-session.py first.
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
# Where this machine keeps its checkouts: fsroot is private, so a local paths file can live there.
if [[ -f "$HERE/fsroot/session.env" ]]; then
  source "$HERE/fsroot/session.env"
fi
export HBL_ROOT="${HBL_ROOT:?Set HBL_ROOT to your HBLauncher checkout, in fsroot/session.env or the environment}"
NS2JB="${HBL_NS2JB:?Set HBL_NS2JB to your ns2jb checkout, in fsroot/session.env or the environment}"
ADDRESS="$(python3 "$HERE/build.py" address)"
part="${1:-}"
shift || true
case "$part" in
server)
  cd "$HERE/.."
  python3 hblauncher/build.py serve 2>&1 | tee ~/Desktop/fs-run.log
  ;;
runner)
  cd "$NS2JB"
  exec node run.cjs --rpc "$ADDRESS"
  ;;
launcher)
  cd "$HBL_ROOT"
  export HBL_NS2JB="$NS2JB"
  export HBL_RPC_TARGET="$ADDRESS"
  export HBL_PAYLOAD="$NS2JB/engine/switch2/ro/payload/main"
  # The patched helper setup-session.py built; the launcher refuses any other.
  export HBL_BULK_SHA256="$(python3 -c 'import json, sys; print(json.load(open(sys.argv[1]))["ns2jb"]["payload"]["sha256"])' "$HERE/dependencies.json")"
  export HBL_ARCHIVE_SDK="$PWD/reference/hblauncher-cpu-sdk/homebrew-launcher"
  export HBL_HID_SYMBOLS="$PWD/reference/hblauncher-cpu-sdk/sdk/native/hid-functions.json"
  exec ./hbl-start "$@"
  ;;
*)
  echo "Usage: $0 server | runner | launcher [hbl-start options]" >&2
  exit 2
  ;;
esac
