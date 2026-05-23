#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

if [ -f "$REPO_DIR/.deps/esp-idf/export.sh" ]; then
  # shellcheck disable=SC1091
  source "$REPO_DIR/.deps/esp-idf/export.sh"
elif [ -f /opt/homebrew/share/esp-idf/export.sh ]; then
  # shellcheck disable=SC1091
  source /opt/homebrew/share/esp-idf/export.sh
elif [ -n "${IDF_PATH:-}" ] && [ -f "$IDF_PATH/export.sh" ]; then
  # shellcheck disable=SC1091
  source "$IDF_PATH/export.sh"
else
  echo "ESP-IDF export.sh not found. Install ESP-IDF or set IDF_PATH." >&2
  exit 1
fi

exec "$@"
