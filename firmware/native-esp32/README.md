# Native ESP32 Firmware

Minimal ESP-IDF bring-up firmware for the TR22 badge.

Current behavior:

- Logs chip and flash information over UART.
- Toggles GPIO2 once per second as a first LED probe.
- Attempts a TR19-compatible 2.9-inch ePaper update with `HELLO TR22 CUSTOM FW`.

GPIO2 is only a placeholder until the real TR22 LED pins are confirmed.
The display pins are copied from TR19: SCK 18, MOSI 23, CS 25, DC 27, RST 0, BUSY 13.
Display operations have timeouts so the firmware should continue blinking/logging if the panel is not responding.

## Configure

Copy the two example config files and fill in your own values (WiFi
credentials, OTA server URL, optional Home Assistant token). Both real files
are gitignored so your secrets never get committed.

```bash
cp main/ota_config.h.example main/ota_config.h
cp main/badge_config.h.example main/badge_config.h
```

## Build

```bash
../../tools/esp-idf-env.sh idf.py set-target esp32
../../tools/esp-idf-env.sh idf.py build
```

## Dev OTA

Run the OTA file server once from the repo root:

```bash
python3 tools/ota-server.py firmware/native-esp32/build/tr22_custom.bin
```

After the badge is running on WiFi, rebuild and trigger an update without
power-cycling:

```bash
tools/badge.py --host <badge-ip-or-hostname> ota --build
```

The HTTP request only reboots the badge into recovery. Recovery keeps the
actual install path: it fetches `OTA_FIRMWARE_URL`, validates the app image,
writes the normal app slot when needed, and then boots it.

## Flash

Only flash after recovery has been verified or you are prepared to use the original dump restore path.

```bash
../../tools/esp-idf-env.sh idf.py -p /dev/cu.usbserial-110 flash monitor
```

If auto-reset does not enter the bootloader, hold BOOT, tap RESET, then release BOOT after flashing starts.

To verify that the physical BOOT/RESET sequence worked before flashing, run this read-only detector from the repo root:

```bash
./tools/detect-bootloader.sh
```

It will print `SUCCESS` once the ESP32 ROM bootloader answers.

The generated flash layout is:

- `0x1000`: `build/bootloader/bootloader.bin`
- `0x8000`: `build/partition_table/partition-table.bin`
- `0x10000`: `build/tr22_custom.bin`

## Serial Log

For a scrollable macOS serial monitor equivalent to
`screen /dev/cu.usbserial-110 115200`, run:

```bash
./tools/serial-log.py
```

The tool waits until `/dev/cu.usbserial-110` appears, leaves output in your
normal terminal scrollback, and keeps all received bytes available for copy.
Press `Ctrl-Y` to copy all captured serial output to the macOS clipboard.
Press `Ctrl-]` to disconnect.

Use a different port or baud rate when needed:

```bash
./tools/serial-log.py /dev/cu.usbserial-210 --baud 460800
```

## Restore

If you have a backup of the original stock firmware (see
`tools/backup-original.sh` to create one, or `docs/recover-original-firmware.md`
for background), restore it with:

```bash
CONFIRM_RESTORE=yes ../../tools/restore-original.sh
```

Use `PORT=/dev/cu.usbserial-...` and `BAUD=460800` if the defaults do not connect reliably.
