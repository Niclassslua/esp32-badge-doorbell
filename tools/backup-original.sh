#!/usr/bin/env bash
set -euo pipefail

PORT="${PORT:-/dev/cu.usbserial-110}"
BAUD="${BAUD:-921600}"
OUT="${OUT:-tr22-full-backup.bin}"
ESPTOOL="${ESPTOOL:-/opt/homebrew/bin/esptool.py}"

"$ESPTOOL" --chip esp32 --port "$PORT" --baud "$BAUD" \
  read-flash 0x0 0x1000000 "$OUT"
