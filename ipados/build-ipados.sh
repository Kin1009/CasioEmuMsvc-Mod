#!/bin/bash
# Backwards-compatible entry point kept for docs/IPADOS_PORT.md and older notes.
# The real, full-featured driver lives at the repository root.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"

# Legacy interface: ipados/build-ipados.sh <simulator-UDID> used to build,
# install, launch and screenshot.  Map that to the new driver's flags.
if [ "$#" -gt 0 ] && [ "${1#-}" = "$1" ]; then
  DEVICE="$1"
  shift
  exec "$ROOT/build-ipados.sh" --install "$DEVICE" --launch \
    --screenshot "$ROOT/ipados-simulator.png" "$@"
fi

exec "$ROOT/build-ipados.sh" "$@"
