#!/usr/bin/env bash
# flash-firmware.sh — Flash the TR22 badge firmware.
#
# The badge has unreliable/no auto-reset wiring on some setups, so this script
# waits for you to put the device into ROM bootloader mode.  It first confirms
# that the ROM loader answers, then asks you to release BOOT before verifying
# the external flash chip.  Finally it writes all images in one esptool session
# so the flasher stub is uploaded only once.
#
# Usage:
#   ./flash-firmware.sh [--port /dev/cu.xxx] [--baud 115200] [--no-stub]
#                       [--skip-flash-probe]
#
# Environment:
#   PORT        Serial port  (default: /dev/cu.usbserial-110)
#   BAUD        Flash baud   (default: 115200)
#   FLASH_FREQ  Boot flash frequency override (default: 80m)
#   FLASH_SIZE  Expected flash size (default: 16MB)
#   FLASH_SPI_CONNECTION
#               Optional esptool --spi-connection value (SPI, HSPI, or pins)
#
# On the TR22 the ESP32-WROVER-B PSRAM shares the flash SPI bus and its CS line
# (GPIO16) floats in download mode, which makes the `flash_id` probe report
# Manufacturer 00 / size Unknown even though `write_flash` works fine.  Pass
# `--skip-flash-probe` to bypass the probe on badges where this is a known
# false negative.  See docs/hardware-assumptions.md.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
FIRMWARE_DIR="${FIRMWARE_DIR:-$REPO_DIR/firmware/native-esp32}"
BUILD_DIR="${BUILD_DIR:-$FIRMWARE_DIR/build-recovery}"

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

# ── argument parsing ─────────────────────────────────────────────────────────
while [[ $# -gt 0 ]]; do
  case "$1" in
    --port)    PORT="$2"; shift 2 ;;
    --baud)    BAUD="$2"; shift 2 ;;
    --no-stub) NO_STUB_ARGS=(--no-stub); shift ;;
    --skip-flash-probe) SKIP_FLASH_PROBE=1; shift ;;
    -h|--help)
      cat <<EOF
Usage: $(basename "$0") [--port /dev/cu.xxx] [--baud 115200] [--no-stub]
                            [--skip-flash-probe]

Flash the TR22 badge firmware.

The script waits for ROM bootloader mode, prompts you to release BOOT, verifies
that the external flash chip is readable, then writes bootloader, app,
partition table, and OTA data in one esptool session.

Options:
  --skip-flash-probe
              Skip the flash_id sanity check.  The ESP32-WROVER-B on the TR22
              shares the SPI bus with PSRAM, and PSRAM CS floats in download
              mode, so flash_id often returns Manufacturer 00 even though
              write_flash works.  Use this on badges where the probe is a
              known false negative.

Environment:
  PORT        Serial port (default: $PORT)
  BAUD        Flash baud  (default: $BAUD)
  FLASH_FREQ  Boot flash frequency override (default: $FLASH_FREQ)
  FLASH_SIZE  Expected flash size (default: $FLASH_SIZE)
  FLASH_SPI_CONNECTION
               Optional esptool --spi-connection value (SPI, HSPI, or pins)
  BUILD_DIR   Recovery build directory (default: $BUILD_DIR)
EOF
      exit 0 ;;
    *) echo "Unknown option: $1" >&2; exit 1 ;;
  esac
done

# ── locate python + esptool ──────────────────────────────────────────────────
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
  "$PYTHON" "$ESPTOOL_SCRIPT" \
    --chip esp32 --port "$PORT" --baud "$BAUD" \
    "$@"
}

# ── verify build artifacts ───────────────────────────────────────────────────
for f in \
  "$BUILD_DIR/bootloader/bootloader.bin" \
  "$BUILD_DIR/partition_table/partition-table.bin" \
  "$BUILD_DIR/ota_data_initial.bin" \
  "$BUILD_DIR/tr22_custom.bin"
do
  if [ ! -f "$f" ]; then
    echo "Missing: $f  (run ./build-recovery-firmware.sh first)" >&2
    exit 1
  fi
done

# ── wait for bootloader mode ─────────────────────────────────────────────────
echo "TR22 badge flash tool"
echo "  Port: $PORT  Baud: $BAUD"
echo "  Build: $BUILD_DIR"
echo "  Mode:  flash recovery firmware to ota_0 at 0x10000"
echo
echo "Physical sequence:"
echo "  1. Hold BOOT button."
echo "  2. Tap RESET (or power-cycle)."
echo "  3. Keep BOOT held until this script says ROM loader detected."
echo "  4. Release BOOT when prompted; flash probing happens after that."
echo
echo "Waiting for ROM bootloader... (30 s timeout)"

FOUND=0
tmp_output="$(mktemp)"
trap 'rm -f "$tmp_output"' EXIT

for i in $(seq 1 60); do
  if esptool_base \
      --before no_reset \
      --after no_reset \
      --connect-attempts 1 \
      --no-stub \
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

if [ $SKIP_FLASH_PROBE -eq 1 ]; then
  echo "Skipping flash_id probe (--skip-flash-probe).  Writing directly."
  echo
else
  echo "Checking external flash chip..."

  FOUND=0
  for i in $(seq 1 20); do
    if esptool_base \
        --before no_reset \
        --after no_reset \
        --connect-attempts 1 \
        --no-stub \
        flash_id "${SPI_CONNECTION_ARGS[@]}" >"$tmp_output" 2>&1; then
      if grep -q "Detected flash size: $FLASH_SIZE" "$tmp_output"; then
        FOUND=1
        break
      fi
    fi

    echo
    echo "ESP32 ROM loader is active, but external flash is not readable as $FLASH_SIZE yet:"
    grep -E "WARNING: Failed to communicate|Manufacturer:|Device:|Detected flash size|Flash voltage" "$tmp_output" || cat "$tmp_output"
    echo "If you are still holding BOOT, release it. Otherwise hold BOOT, tap RESET, release BOOT when this script detects ROM again."
    echo "If this badge has the PSRAM/SPI quirk (see docs/hardware-assumptions.md), re-run with --skip-flash-probe."
    sleep 0.5
  done

  if [ $FOUND -eq 0 ]; then
    echo "Timed out waiting for readable flash chip." >&2
    echo "If write_flash is known to work on this badge, re-run with --skip-flash-probe." >&2
    exit 1
  fi

  echo "Bootloader and flash chip detected:"
  grep -E "Chip is|Crystal is|MAC:|Manufacturer:|Device:|Detected flash size|Flash voltage" "$tmp_output" || cat "$tmp_output"
  echo
fi

echo "Flashing..."
echo

# Use default_reset for the actual write, even though we manually put the chip
# into download mode for the probe steps above. After the probe, the chip's SPI
# bus state on the ESP32-WROVER-B (where PSRAM shares the bus and GPIO16 floats
# in download mode) tends to be in a weird mid-state, and the stub then fails
# with "Failed to communicate with the flash chip" once it tries to access flash.
# default_reset toggles DTR/RTS to drive EN and IO0 cleanly into a fresh
# download-mode entry — equivalent to the user's manual hold-BOOT, tap-RESET,
# release-BOOT sequence but without the bus-state drift introduced by the probe.
esptool_base \
  --before default_reset \
  --after hard_reset \
  "${NO_STUB_ARGS[@]}" \
  write_flash \
  --flash_mode "$FLASH_MODE" \
  --flash_freq "$FLASH_FREQ" \
  --flash_size "$FLASH_SIZE" \
  "${SPI_CONNECTION_ARGS[@]}" \
  0x1000  "$BUILD_DIR/bootloader/bootloader.bin" \
  0x10000 "$BUILD_DIR/tr22_custom.bin" \
  0x8000  "$BUILD_DIR/partition_table/partition-table.bin" \
  0xd000  "$BUILD_DIR/ota_data_initial.bin"

echo
echo "Done. Badge is rebooting into new firmware."
