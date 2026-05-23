#!/usr/bin/env bash
set -euo pipefail

if [ "${CONFIRM_RESTORE:-}" != "yes" ]; then
  echo "Refusing to write flash. Re-run with CONFIRM_RESTORE=yes if you really want to restore the original dump."
  exit 2
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DUMP_DIR="${DUMP_DIR:-$SCRIPT_DIR/../../tr22-badge/flash-dump}"
PORT="${PORT:-/dev/cu.usbserial-110}"
BAUD="${BAUD:-921600}"

cd "$DUMP_DIR"

python3 -m esptool --chip esp32 --port "$PORT" --baud "$BAUD" \
  --before default-reset --after hard-reset write-flash \
  --flash_mode dio --flash_freq 80m --flash_size 16MB \
  0x1000 bootloader.bin \
  0x9000 nvs.bin \
  0xd000 otadata.bin \
  0xf000 phy_init.bin \
  0x10000 ota_0.bin \
  0x210000 ota_1.bin \
  0x404000 locfd_spiffs.bin
