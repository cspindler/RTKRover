#!/usr/bin/env bash
# Build and upload to the attached rover unit. Does not touch the serial monitor.
#
# Kept separate from tools/watch.sh so a failed upload and a failed capture
# report distinct exit codes; chain them with && when you want both.
#
# Usage:
#   tools/flash.sh [env] [board]
#
# Defaults: env featheresp32_debug (serial logging on), board auto-discovered.
set -uo pipefail

cd "$(dirname "$0")/.."

ENV_NAME="${1:-featheresp32_debug}"
PORT=$(tools/find-board.sh "${2:-}") || exit 2

echo "flash: $ENV_NAME -> $PORT" >&2
exec pio run -e "$ENV_NAME" -t upload --upload-port "$PORT"
