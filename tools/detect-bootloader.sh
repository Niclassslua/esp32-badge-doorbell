#!/usr/bin/env bash
# detect-bootloader.sh — read-only ESP32 ROM bootloader detector (gum TUI)
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

# ── locate gum ────────────────────────────────────────────────────────────────

GUM="${GUM:-}"
if [ -z "$GUM" ]; then
  if [ -x /opt/homebrew/bin/gum ]; then
    GUM=/opt/homebrew/bin/gum
  elif command -v gum >/dev/null 2>&1; then
    GUM="$(command -v gum)"
  else
    echo "gum not found. Install with: brew install gum" >&2
    exit 1
  fi
fi

# ── config ────────────────────────────────────────────────────────────────────

PORT="${PORT:-/dev/cu.usbserial-110}"
BAUD="${BAUD:-115200}"
FLASH_SIZE="${FLASH_SIZE:-16MB}"
SLEEP_SECONDS="${SLEEP_SECONDS:-0.5}"
ESPTOOL="${ESPTOOL:-}"

# ── locate esptool ────────────────────────────────────────────────────────────

if [ -z "$ESPTOOL" ]; then
  if [ -x /opt/homebrew/bin/esptool.py ]; then
    ESPTOOL=/opt/homebrew/bin/esptool.py
  elif command -v esptool.py >/dev/null 2>&1; then
    ESPTOOL="$(command -v esptool.py)"
  elif [ -x "$REPO_DIR/.deps/esp-idf/components/esptool_py/esptool/esptool.py" ]; then
    ESPTOOL="$REPO_DIR/.deps/esp-idf/components/esptool_py/esptool/esptool.py"
  else
    "$GUM" style \
      --border rounded --border-foreground 196 --padding "1 2" --foreground 196 \
      "ERROR — esptool.py not found

Set ESPTOOL=/path/to/esptool.py and retry."
    exit 1
  fi
fi


# ── tmpfile ───────────────────────────────────────────────────────────────────

tmp_output="$(mktemp)"
trap 'rm -f "$tmp_output"' EXIT

# ── helpers ───────────────────────────────────────────────────────────────────

on_status_line=false

clean_line() {
  if $on_status_line; then
    printf "\n"
    on_status_line=false
  fi
}

# ── header (printed once) ─────────────────────────────────────────────────────

"$GUM" style \
  --border rounded --border-foreground 212 --padding "0 2" --bold \
  "TR22 Badge — ROM Bootloader Detector"

available_ports="$(ls /dev/cu.* 2>/dev/null | grep -v '^/dev/cu\.Bluetooth\|^/dev/cu\.debug\|^/dev/cu\.wlan' | tr '\n' '  ' | sed 's/  $//')"
port_line="Port  $PORT   Baud  $BAUD"
[ -n "$available_ports" ] && port_line="$port_line
Ports  $available_ports"

"$GUM" style --foreground 240 "$port_line"

printf "\n"

"$GUM" style \
  --border normal --border-foreground 240 --padding "0 2" \
  "Physical sequence:
  1. Connect the badge via USB.
  2. Hold BOOT.
  3. Tap RESET.
  4. Keep BOOT held until SUCCESS appears."

printf "\n"

# ── main loop ─────────────────────────────────────────────────────────────────

device_seen=false
attempt=0

while true; do
  attempt=$(( attempt + 1 ))

  # ── port presence check ───────────────────────────────────────────────────

  if [ ! -e "$PORT" ]; then
    if $device_seen; then
      printf "\033[2K\r  \033[33m[%d]\033[0m device disconnected — waiting to reappear..." \
        "$attempt"
    else
      printf "\033[2K\r  \033[90m[%d]\033[0m waiting for device on %s..." \
        "$attempt" "$PORT"
    fi
    on_status_line=true
    sleep "$SLEEP_SECONDS"
    continue
  fi

  # ── device present — probe ────────────────────────────────────────────────

  device_seen=true
  printf "\033[2K\r  \033[90m[%d]\033[0m probing ROM loader..." "$attempt"
  on_status_line=true

  "$ESPTOOL" \
    --chip esp32 \
    --port "$PORT" \
    --baud "$BAUD" \
    --connect-attempts 1 \
    --before no_reset \
    --after no_reset \
    --no-stub \
    flash_id >"$tmp_output" 2>&1
  probe_rc=$?

  # ── esptool succeeded ─────────────────────────────────────────────────────

  if [ $probe_rc -eq 0 ]; then
    if grep -q "Detected flash size: $FLASH_SIZE" "$tmp_output"; then
      # ── SUCCESS ────────────────────────────────────────────────────────────
      details="$(grep -E \
        "Chip is|Crystal is|MAC:|Detected flash size|Manufacturer:|Device:|Flash voltage" \
        "$tmp_output")"
      clean_line
      "$GUM" style \
        --border rounded --border-foreground 76 --padding "1 2" --foreground 76 \
        "$(printf '%s\n\n%s\n\n%s' \
            "✓  SUCCESS — Bootloader mode confirmed" \
            "$details" \
            "Keep the badge in this state if you want to flash next.")"
      exit 0
    fi

    # ── flash not readable (wrong size or not responding) ─────────────────
    printf "\033[2K\r  \033[33m[%d] ⚠  Flash not readable — hold BOOT and tap RESET\033[0m" \
      "$attempt"
    on_status_line=true
    sleep "$SLEEP_SECONDS"
    continue
  fi

  # ── esptool failed — fatal vs transient ──────────────────────────────────

  # Only treat as "port busy" if the port node still exists — if it vanished
  # the badge was unplugged during the probe (normal RESET bounce), not blocked.
  if [ -e "$PORT" ] && grep -qi "port is busy\|Resource busy\|Operation not permitted" "$tmp_output"; then
    clean_line
    "$GUM" style \
      --border rounded --border-foreground 196 --padding "1 2" --foreground 196 \
      "✗  PORT BUSY

Another process has $PORT open.
Close your serial monitor or flash tool, then retry."
    exit 2
  fi

  # Transient (no response, RESET bounce) — loop silently
  sleep "$SLEEP_SECONDS"

done
