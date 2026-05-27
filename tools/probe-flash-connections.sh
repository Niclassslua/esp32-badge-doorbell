#!/usr/bin/env bash
# probe-flash-connections.sh — read-only ESP32 flash wiring probe.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

PORT="${PORT:-/dev/cu.usbserial-110}"
BAUD="${BAUD:-115200}"
PYTHON="${PYTHON:-$HOME/.espressif/python_env/idf5.5_py3.12_env/bin/python}"
ESPTOOL_SCRIPT="${ESPTOOL_SCRIPT:-/opt/homebrew/bin/esptool.py}"

if [ ! -x "$PYTHON" ]; then
  echo "ESP-IDF python venv not found at $PYTHON" >&2
  exit 1
fi
if [ ! -f "$ESPTOOL_SCRIPT" ]; then
  echo "esptool.py not found at $ESPTOOL_SCRIPT" >&2
  exit 1
fi

tmp_output="$(mktemp)"
trap 'rm -f "$tmp_output"' EXIT

esptool_base() {
  "$PYTHON" "$ESPTOOL_SCRIPT" \
    --chip esp32 --port "$PORT" --baud "$BAUD" \
    --before no_reset --after no_reset --connect-attempts 1 \
    "$@"
}

echo "TR22 flash connection probe"
echo "  Port: $PORT  Baud: $BAUD"
echo
echo "Put the badge in ROM bootloader first:"
echo "  hold BOOT, tap RESET, release BOOT after ROM loader is detected."
echo
echo "Checking ROM loader..."
if ! esptool_base --no-stub read_mac >"$tmp_output" 2>&1; then
  cat "$tmp_output"
  exit 1
fi
grep -E "Chip is|Crystal is|MAC:" "$tmp_output" || cat "$tmp_output"
echo

for connection in SPI HSPI; do
  echo "Probing flash via --spi-connection $connection"
  if esptool_base flash_id --spi-connection "$connection" >"$tmp_output" 2>&1; then
    grep -E "Manufacturer:|Device:|Detected flash size|Flash voltage" "$tmp_output" || cat "$tmp_output"
    if grep -q "Detected flash size:" "$tmp_output" &&
       ! grep -q "Detected flash size: Unknown" "$tmp_output"; then
      echo
      echo "SUCCESS: use FLASH_SPI_CONNECTION=$connection for flashing."
      exit 0
    fi
  else
    grep -E "WARNING: Failed to communicate|Manufacturer:|Device:|Detected flash size|Flash voltage|fatal error" "$tmp_output" || cat "$tmp_output"
  fi
  echo
done

echo "No readable flash found via built-in SPI/HSPI connections." >&2
exit 1
