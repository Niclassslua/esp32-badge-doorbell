#!/usr/bin/env bash
# flash-bootstrap.sh — Cold-flash the badge with recovery in ota_0 + app in ota_1.
#
# Use this for a fresh badge (no prior firmware) or when you want both slots
# rewritten in one go. After this flash:
#   - bootloader picks ota_0 (recovery) on first boot
#   - recovery starts, attempts WiFi, calls OTA against OTA_FIRMWARE_URL
#   - if the OTA server is reachable AND serves the same app already in ota_1,
#     recovery's ota_update_check_and_apply_to_partition() short-circuits to
#     esp_ota_set_boot_partition(ota_1) + reboot
#   - normal app boots; from then on the bootloader prefers ota_1
#
# Pre-req: tools/ota-server.py must be running on the host in OTA_FIRMWARE_URL
# before the badge powers on, otherwise recovery loops forever.
#
# Usage:
#   ./flash-bootstrap.sh [--port /dev/cu.xxx] [--baud 115200] [--skip-flash-probe]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
FIRMWARE_DIR="${FIRMWARE_DIR:-$REPO_DIR/firmware/native-esp32}"
BUILD_APP_DIR="${BUILD_APP_DIR:-$FIRMWARE_DIR/build}"
BUILD_RECOVERY_DIR="${BUILD_RECOVERY_DIR:-$FIRMWARE_DIR/build-recovery}"

PORT="${PORT:-/dev/cu.usbserial-110}"
BAUD="${BAUD:-115200}"
FLASH_MODE="${FLASH_MODE:-dio}"
FLASH_FREQ="${FLASH_FREQ:-80m}"
FLASH_SIZE="${FLASH_SIZE:-16MB}"
FLASH_SPI_CONNECTION="${FLASH_SPI_CONNECTION:-}"
NO_STUB_ARGS=()
SPI_CONNECTION_ARGS=()
SKIP_FLASH_PROBE=0

if [ -n "$FLASH_SPI_CONNECTION" ]; then
  SPI_CONNECTION_ARGS=(--spi-connection "$FLASH_SPI_CONNECTION")
fi

while [[ $# -gt 0 ]]; do
  case "$1" in
    --port)    PORT="$2"; shift 2 ;;
    --baud)    BAUD="$2"; shift 2 ;;
    --no-stub) NO_STUB_ARGS=(--no-stub); shift ;;
    --skip-flash-probe) SKIP_FLASH_PROBE=1; shift ;;
    -h|--help)
      sed -n '2,18p' "$0"; exit 0 ;;
    *) echo "Unknown option: $1" >&2; exit 1 ;;
  esac
done

PYTHON="$HOME/.espressif/python_env/idf5.5_py3.12_env/bin/python"
ESPTOOL_SCRIPT="/opt/homebrew/bin/esptool.py"

if [ ! -x "$PYTHON" ]; then
  echo "ESP-IDF python venv not found at $PYTHON" >&2
  exit 1
fi
if [ ! -f "$ESPTOOL_SCRIPT" ]; then
  echo "esptool.py not found at $ESPTOOL_SCRIPT" >&2
  exit 1
fi

esptool_base() {
  "$PYTHON" "$ESPTOOL_SCRIPT" --chip esp32 --port "$PORT" --baud "$BAUD" "$@"
}

# Verify artifacts. Bootloader, partition table, and ota_data come from the
# app build because both builds produce identical copies (same partition CSV).
for f in \
  "$BUILD_APP_DIR/bootloader/bootloader.bin" \
  "$BUILD_APP_DIR/partition_table/partition-table.bin" \
  "$BUILD_APP_DIR/ota_data_initial.bin" \
  "$BUILD_APP_DIR/tr22_custom.bin" \
  "$BUILD_RECOVERY_DIR/tr22_custom.bin"
do
  if [ ! -f "$f" ]; then
    echo "Missing: $f" >&2
    echo "Run tools/build-firmware.sh and tools/build-recovery-firmware.sh first." >&2
    exit 1
  fi
done

echo "TR22 cold-flash (recovery -> ota_0, app -> ota_1)"
echo "  Port:     $PORT  Baud: $BAUD"
echo "  App:      $BUILD_APP_DIR/tr22_custom.bin"
echo "  Recovery: $BUILD_RECOVERY_DIR/tr22_custom.bin"
echo
echo "Physical sequence:"
echo "  1. Hold BOOT button."
echo "  2. Tap RESET (or power-cycle)."
echo "  3. Keep BOOT held until this script says ROM loader detected."
echo "  4. Release BOOT when prompted."
echo
echo "Waiting for ROM bootloader... (30 s timeout)"

FOUND=0
tmp_output="$(mktemp)"
trap 'rm -f "$tmp_output"' EXIT

for i in $(seq 1 60); do
  if esptool_base \
      --before no_reset --after no_reset \
      --connect-attempts 1 --no-stub \
      read_mac >"$tmp_output" 2>&1; then
    FOUND=1
    break
  fi
  sleep 0.5
done

if [ $FOUND -eq 0 ]; then
  echo "Timed out waiting for bootloader." >&2
  exit 1
fi

echo "ROM loader detected:"
grep -E "Chip is|Crystal is|MAC:" "$tmp_output" || cat "$tmp_output"
echo
echo "Release BOOT now. Keep USB connected; do not tap RESET."
sleep 2
echo

if [ $SKIP_FLASH_PROBE -eq 0 ]; then
  echo "Checking external flash chip..."
  FOUND=0
  for i in $(seq 1 20); do
    if esptool_base \
        --before no_reset --after no_reset \
        --connect-attempts 1 --no-stub \
        flash_id "${SPI_CONNECTION_ARGS[@]}" >"$tmp_output" 2>&1; then
      if grep -q "Detected flash size: $FLASH_SIZE" "$tmp_output"; then
        FOUND=1
        break
      fi
    fi
    sleep 0.5
  done
  if [ $FOUND -eq 0 ]; then
    echo "Flash probe failed. Re-run with --skip-flash-probe if write_flash is known to work." >&2
    exit 1
  fi
  grep -E "Chip is|Manufacturer:|Device:|Detected flash size" "$tmp_output" || true
  echo
fi

echo "Flashing 5 images (bootloader + partition + ota_data + recovery@ota_0 + app@ota_1)..."
echo

esptool_base \
  --before default_reset --after hard_reset \
  "${NO_STUB_ARGS[@]}" \
  write_flash \
  --flash_mode "$FLASH_MODE" \
  --flash_freq "$FLASH_FREQ" \
  --flash_size "$FLASH_SIZE" \
  "${SPI_CONNECTION_ARGS[@]}" \
  0x1000   "$BUILD_APP_DIR/bootloader/bootloader.bin" \
  0x8000   "$BUILD_APP_DIR/partition_table/partition-table.bin" \
  0xd000   "$BUILD_APP_DIR/ota_data_initial.bin" \
  0x10000  "$BUILD_RECOVERY_DIR/tr22_custom.bin" \
  0x190000 "$BUILD_APP_DIR/tr22_custom.bin"

echo
echo "Done."
echo "Make sure ota-server.py is running so recovery can hand off to ota_1:"
echo "  python3 tools/ota-server.py $BUILD_APP_DIR/tr22_custom.bin"
