#!/usr/bin/env bash
# Capture a bounded window of serial output from the attached board, then exit.
#
# This is the half that genuinely needs wrapping: `pio device monitor` cannot run
# without a TTY on stdin (pyserial miniterm calls termios.tcgetattr and aborts),
# so tools/serial-capture.py reads the port directly instead.
#
# Usage:
#   tools/watch.sh [seconds] [board]
#
# Defaults: 30 seconds, board auto-discovered.
set -uo pipefail

cd "$(dirname "$0")/.."

WATCH_SECONDS="${1:-30}"
PORT=$(tools/find-board.sh "${2:-}") || exit 2

PYTHON="$HOME/.platformio/penv/bin/python"   # the only interpreter here with pyserial
if [ ! -x "$PYTHON" ]; then
  echo "watch: $PYTHON not found (PlatformIO venv missing?)" >&2
  exit 2
fi

echo "watch: ${WATCH_SECONDS}s on $PORT" >&2
exec "$PYTHON" tools/serial-capture.py "$PORT" "$WATCH_SECONDS"
