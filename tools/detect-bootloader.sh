#!/usr/bin/env bash
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

PORT="${PORT:-/dev/cu.usbserial-110}"
BAUD="${BAUD:-115200}"
ATTEMPTS="${ATTEMPTS:-60}"
SLEEP_SECONDS="${SLEEP_SECONDS:-0.5}"
ESPTOOL="${ESPTOOL:-}"

if [ -z "$ESPTOOL" ]; then
  if [ -x /opt/homebrew/bin/esptool.py ]; then
    ESPTOOL=/opt/homebrew/bin/esptool.py
  elif command -v esptool.py >/dev/null 2>&1; then
    ESPTOOL="$(command -v esptool.py)"
  elif [ -x "$REPO_DIR/.deps/esp-idf/components/esptool_py/esptool/esptool.py" ]; then
    ESPTOOL="$REPO_DIR/.deps/esp-idf/components/esptool_py/esptool/esptool.py"
  else
    echo "Could not find esptool.py. Set ESPTOOL=/path/to/esptool.py and retry." >&2
    exit 1
  fi
fi

if [ ! -e "$PORT" ]; then
  echo "Serial port not found: $PORT" >&2
  echo "Connected ports:" >&2
  ls /dev/cu.* 2>/dev/null | sed 's/^/  /' >&2
  exit 1
fi

tmp_output="$(mktemp)"
trap 'rm -f "$tmp_output"' EXIT

echo "Read-only ESP32 bootloader detector"
echo "Port: $PORT"
echo "Baud: $BAUD"
echo "Tool: $ESPTOOL"
echo
echo "Physical sequence:"
echo "  1. Hold BOOT."
echo "  2. Tap RESET."
echo "  3. Keep BOOT held until this script prints SUCCESS."
echo

for attempt in $(seq 1 "$ATTEMPTS"); do
  printf "[%02d/%02d] probing ROM loader... " "$attempt" "$ATTEMPTS"

  if "$ESPTOOL" \
      --chip esp32 \
      --port "$PORT" \
      --baud "$BAUD" \
      --connect-attempts 1 \
      --before no_reset \
      --after no_reset \
      flash_id >"$tmp_output" 2>&1; then
    echo "SUCCESS"
    echo
    grep -E "Chip is|Crystal is|MAC:|Detected flash size|Manufacturer|Device" "$tmp_output" || cat "$tmp_output"
    echo
    echo "Bootloader mode is confirmed. Keep the badge in this state if you want to flash next."
    exit 0
  fi

  if grep -qi "could not open.*port is busy\\|Resource busy\\|Operation not permitted" "$tmp_output"; then
    echo "PORT ERROR"
    cat "$tmp_output"
    exit 2
  fi

  echo "no response"
  sleep "$SLEEP_SECONDS"
done

echo
echo "No ESP32 ROM bootloader response after $ATTEMPTS attempts."
echo "Try again while holding BOOT before tapping RESET, or power-cycle the badge and rerun this script."
exit 3
