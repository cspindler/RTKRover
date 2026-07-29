#!/usr/bin/env bash
# Build, flash, and capture a bounded window of serial output, then exit.
#
# `pio device monitor` never returns on its own, which stalls any automated
# iterate -> build -> flash -> observe loop. This wrapper always terminates.
#
# Usage:
#   tools/flash-and-watch.sh [seconds] [env]
#   tools/flash-and-watch.sh watch [seconds] [env]   # monitor only, no upload
#
# Defaults: 45 seconds, env featheresp32_debug (serial logging on).
set -uo pipefail

cd "$(dirname "$0")/.."

MODE=flash
if [ "${1:-}" = "watch" ]; then
  MODE=watch
  shift
fi

SECONDS_TO_WATCH="${1:-45}"
ENV_NAME="${2:-featheresp32_debug}"

if [ "$MODE" = watch ]; then
  set -- device monitor -e "$ENV_NAME"
else
  set -- run -e "$ENV_NAME" -t upload -t monitor
fi

# 137/143 = killed by the timeout, which is the normal end of a capture window.
timeout --signal=TERM --kill-after=5 "$SECONDS_TO_WATCH" pio "$@"
status=$?
case "$status" in
  0 | 124 | 143 | 137) exit 0 ;;
  *) exit "$status" ;;
esac
