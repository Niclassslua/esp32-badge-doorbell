#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
FIRMWARE_DIR="${FIRMWARE_DIR:-$REPO_DIR/firmware/native-esp32}"

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  cat <<EOF
Usage: $(basename "$0") [build args...]

Build the native ESP32 firmware from the tools directory.

Environment:
  FIRMWARE_DIR   Firmware project directory (default: $FIRMWARE_DIR)
  BADGE_BUILD_TIMESTAMP
                 Build timestamp shown on the badge. Defaults to local time.
  GPIO_DEBUG_MODE
                 Defaults to 0 for normal OTA-capable builds. Pass
                 GPIO_DEBUG_MODE=1 ALLOW_DANGEROUS_DEBUG=1 only for deliberate
                 GPIO bring-up builds that may block OTA/recovery.
  BUILD_DIR      Build output directory (default: $FIRMWARE_DIR/build)
  SDKCONFIG      SDK config path (default: $FIRMWARE_DIR/sdkconfig)

Examples:
  ./build-firmware.sh
  ./build-firmware.sh -DNAME=value
EOF
  exit 0
fi

BUILD_TIMESTAMP="${BADGE_BUILD_TIMESTAMP:-$(date '+%b %d %Y %H:%M:%S %Z')}"
BUILD_DIR="${BUILD_DIR:-$FIRMWARE_DIR/build}"
SDKCONFIG="${SDKCONFIG:-$FIRMWARE_DIR/sdkconfig}"

# ESP-IDF embeds __TIME__/__DATE__ into esp_app_desc.c. Ninja only recompiles
# that file when its source mtime changes, so incremental rebuilds keep the
# original build timestamp baked in -- which makes the OTA HEAD-check believe
# nothing changed and skip the download. Touch the file so __TIME__/__DATE__
# refresh on every build.
APP_DESC_SRC="$REPO_DIR/.deps/esp-idf/components/esp_app_format/esp_app_desc.c"
if [[ -f "$APP_DESC_SRC" ]]; then
  touch "$APP_DESC_SRC"
fi

if [[ "${GPIO_DEBUG_MODE:-0}" == "1" && "${ALLOW_DANGEROUS_DEBUG:-0}" != "1" ]]; then
  echo "Refusing GPIO_DEBUG_MODE=1 without ALLOW_DANGEROUS_DEBUG=1." >&2
  echo "GPIO debug blocks normal boot and can prevent OTA/recovery." >&2
  exit 1
fi

exec "$SCRIPT_DIR/esp-idf-env.sh" idf.py \
  -C "$FIRMWARE_DIR" \
  -B "$BUILD_DIR" \
  -DSDKCONFIG="$SDKCONFIG" \
  -DBADGE_BUILD_TIMESTAMP="$BUILD_TIMESTAMP" \
  -DRECOVERY_FIRMWARE=0 \
  -DGPIO_DEBUG_MODE="${GPIO_DEBUG_MODE:-0}" \
  -DALLOW_DANGEROUS_DEBUG="${ALLOW_DANGEROUS_DEBUG:-0}" \
  "$@" \
  build
