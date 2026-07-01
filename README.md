# ESP32 Badge Doorbell

Custom ESP-IDF firmware that turns a TROOPERS22 conference badge into a
battery-powered smart doorbell / door-sign. Touch the badge to ring, flip
between "please ring" and "do not disturb", flash a Home Assistant light on
a ring, and read the current status on a 2.9" e-paper display — all while
sipping power hard enough to run off a coin cell between charges.

## Hardware

- ESP32-WROVER-B, 16 MB flash
- 2.9" e-paper display (TR19-compatible, SSD1675-class controller)
- WS2812 addressable LEDs for status feedback
- Azoteq IQS550 capacitive touch keyboard (any touch rings the doorbell)
- DRV2605L haptic driver, reused here as a battery-voltage probe
- PCA9555 I/O expander for the badge's nav buttons
- All peripherals share a single I²C bus

## Features

- **E-paper door sign** — shows "please ring" / "do not disturb" / night-mode
  state, battery %, and IP address, with partial refreshes to save power.
- **Doorbell ring** — any touch on the capacitive keyboard triggers a ring,
  with cooldown/debounce so touch chatter can't spam it.
- **Home Assistant integration** — optional light-flash on ring via HA's
  REST API (off by default until you configure a token).
- **Night mode** — SNTP-synced Berlin time; between 23:00–08:00 the badge
  shows "ring disabled" and drops into deep sleep, which is the single
  biggest lever on battery life (µA-range draw vs. mA-range awake).
- **Two-image OTA** — a small recovery image handles validated firmware
  installs (from HTTP or SD card) so the normal app image can safely
  self-update without needing a second device.
- **WiFi on demand** — the radio only comes up for OTA checks, doorbell HA
  calls, and SNTP sync, then goes back down; it's not kept alive at idle.
- **Nav button support** — holding START for 3 s kicks off an OTA check
  from the running app.

## Quick start

Requires ESP-IDF 5.5, checked out in-tree at `.deps/esp-idf/` (all helper
scripts source `tools/esp-idf-env.sh`, which activates it and pins Python
3.12 — IDF 5.5 doesn't support Python 3.14).

```bash
# 1. Configure: copy the example config files and fill in your WiFi
#    credentials, OTA server URL, and (optional) Home Assistant token.
#    Both real files are gitignored.
cp firmware/native-esp32/main/ota_config.h.example firmware/native-esp32/main/ota_config.h
cp firmware/native-esp32/main/badge_config.h.example firmware/native-esp32/main/badge_config.h

# 2. Build
tools/build-firmware.sh

# 3. Flash (bootloader + partition table + otadata + app, one esptool session)
tools/flash-firmware.sh --port /dev/cu.usbserial-110
```

See `firmware/native-esp32/README.md` for the full build/flash/OTA/serial-log
workflow, including the no-power-cycle dev OTA loop and bootloader recovery
tips for this board's quirky SPI flash probe.

## Layout

- `firmware/native-esp32/`: the active ESP-IDF firmware.
- `tools/`: build, flash, OTA server, and serial-log helper scripts.
- `docs/`: hardware notes (pinouts, I²C addresses, button/keyboard mappings)
  and original-firmware recovery instructions.

## License

MIT — see `LICENSE`.
