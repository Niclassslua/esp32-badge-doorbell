#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
FIRMWARE_DIR="${FIRMWARE_DIR:-$REPO_DIR/firmware/native-esp32}"
BUILD_DIR="${BUILD_DIR:-$FIRMWARE_DIR/build-recovery}"

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  cat <<EOF
Usage: $(basename "$0") [build args...]

Build the minimal recovery firmware into a separate build directory.

Environment:
  FIRMWARE_DIR   Firmware project directory (default: $FIRMWARE_DIR)
  BUILD_DIR      Recovery build output directory (default: $BUILD_DIR)
  SDKCONFIG      SDK config path (default: $BUILD_DIR/sdkconfig)
  BADGE_BUILD_TIMESTAMP
                 Build timestamp shown on recovery display.

Output:
  $BUILD_DIR/tr22_custom.bin
  $BUILD_DIR/tr22_recovery.bin
EOF
  exit 0
fi

BUILD_TIMESTAMP="${BADGE_BUILD_TIMESTAMP:-$(date '+%b %d %Y %H:%M:%S %Z')}"
SDKCONFIG="${SDKCONFIG:-$BUILD_DIR/sdkconfig}"

"$SCRIPT_DIR/esp-idf-env.sh" idf.py \
  -C "$FIRMWARE_DIR" \
  -B "$BUILD_DIR" \
  -DSDKCONFIG="$SDKCONFIG" \
  -DBADGE_BUILD_TIMESTAMP="$BUILD_TIMESTAMP" \
  -DRECOVERY_FIRMWARE=1 \
  -DGPIO_DEBUG_MODE=0 \
  "$@" \
  build

cp "$BUILD_DIR/tr22_custom.bin" "$BUILD_DIR/tr22_recovery.bin"
echo "Recovery image: $BUILD_DIR/tr22_recovery.bin"
