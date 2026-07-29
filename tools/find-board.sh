#!/usr/bin/env bash
# Resolve which known rover unit is attached; print its device path.
#
# Units are listed by CP2104 serial number in tools/known-boards.txt. Discovering
# the board this way (rather than trusting a hardcoded upload_port) means an
# unknown or unplugged board is a clear error instead of a confusing upload
# failure, and swapping units needs no edit to platformio.ini.
#
# Usage:
#   tools/find-board.sh            # exactly one known unit must be attached
#   tools/find-board.sh 01562BEE   # require this specific unit
#   RTK_BOARD=rover-01 tools/find-board.sh   # or select by label
#
# Exit: 0 + device path on stdout. 2 if none / unknown / ambiguous.
set -uo pipefail

cd "$(dirname "$0")/.."
LIST=tools/known-boards.txt

if [ ! -f "$LIST" ]; then
  echo "find-board: missing $LIST" >&2
  exit 2
fi

want="${1:-${RTK_BOARD:-}}"

found_paths=()
found_labels=()
known_match=0
while read -r serial label _rest; do
  [ -z "${serial:-}" ] && continue
  case "$serial" in \#*) continue ;; esac
  if [ -n "$want" ]; then
    if [ "$want" != "$serial" ] && [ "$want" != "$label" ]; then
      continue
    fi
    known_match=1
  fi
  path="/dev/cu.usbserial-$serial"
  [ -e "$path" ] || continue
  found_paths+=("$path")
  found_labels+=("$label ($serial)")
done < <(sed 's/#.*//' "$LIST")

if [ -n "$want" ] && [ "$known_match" -eq 0 ]; then
  echo "find-board: '$want' is not a known unit in $LIST." >&2
  echo "Known units:" >&2
  sed 's/#.*//; s/[[:space:]]*$//; /^$/d' "$LIST" >&2
  echo "If this is a new unit, add its SER= from 'pio device list' to $LIST." >&2
  exit 2
fi

case "${#found_paths[@]}" in
  1)
    echo "${found_paths[0]}"
    ;;
  0)
    if [ -n "$want" ]; then
      echo "find-board: requested unit '$want' is not attached." >&2
    else
      echo "find-board: no known rover unit attached." >&2
    fi
    echo "Known units ($LIST):" >&2
    sed 's/#.*//; s/[[:space:]]*$//; /^$/d' "$LIST" >&2
    echo "Attached USB-serial devices:" >&2
    # stdout must be pointed at stderr BEFORE stderr is silenced, or the
    # listing is written to /dev/null along with the errors.
    ls /dev/cu.usbserial-* >&2 2>/dev/null || echo "  (none)" >&2
    echo "If this is a new unit, add its SER= from 'pio device list' to $LIST." >&2
    exit 2
    ;;
  *)
    echo "find-board: more than one known unit attached; pick one." >&2
    printf '  %s\n' "${found_labels[@]}" >&2
    echo "Re-run as: tools/find-board.sh <SERIAL>   (or RTK_BOARD=<label>)" >&2
    exit 2
    ;;
esac
