#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
FIRMWARE_DIR="${FIRMWARE_DIR:-$REPO_DIR/firmware/native-esp32}"
APP_BIN="${APP_BIN:-$FIRMWARE_DIR/build/tr22_custom.bin}"

if [[ $# -ne 1 || "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
  cat <<EOF
Usage: $(basename "$0") /Volumes/SDCARD

Copy the normal app firmware to the SD-card recovery path:
  /TR22/app.bin

Environment:
  APP_BIN   App image to copy (default: $APP_BIN)
EOF
  exit 1
fi

SD_ROOT="$1"
if [[ ! -d "$SD_ROOT" ]]; then
  echo "SD root does not exist: $SD_ROOT" >&2
  exit 1
fi
if [[ ! -f "$APP_BIN" ]]; then
  echo "App image does not exist: $APP_BIN" >&2
  echo "Run ./tools/build-firmware.sh first." >&2
  exit 1
fi

mkdir -p "$SD_ROOT/TR22"
cp "$APP_BIN" "$SD_ROOT/TR22/app.bin"
shasum -a 256 "$SD_ROOT/TR22/app.bin" > "$SD_ROOT/TR22/app.sha256"

echo "Copied app image to $SD_ROOT/TR22/app.bin"
cat "$SD_ROOT/TR22/app.sha256"
