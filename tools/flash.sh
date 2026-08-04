#!/usr/bin/env bash
# Build and upload to the attached rover unit. Does not touch the serial monitor.
#
# Kept separate from tools/watch.sh so a failed upload and a failed capture
# report distinct exit codes; chain them with && when you want both.
#
# Usage:
#   tools/flash.sh [env] [board]
#
# Defaults: env featheresp32 (serial logging off), board auto-discovered.
# use featheresp32_debug to turn on serial logging and startup gating
set -uo pipefail

cd "$(dirname "$0")/.."

ENV_NAME="${1:-featheresp32}"

# Export the board choice so the build's gen_caster_secrets.py bakes the same
# unit's credentials that find-board.sh resolves the port for.
if [ -n "${2:-}" ]; then
  export RTK_BOARD="$2"
fi
PORT=$(tools/find-board.sh) || exit 2

echo "flash: $ENV_NAME -> $PORT" >&2
exec pio run -e "$ENV_NAME" -t upload --upload-port "$PORT"
