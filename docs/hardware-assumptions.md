# Hardware Assumptions

Known or strongly suspected facts for the TR22 badge.

## Confirmed From Device Probe

- Chip: ESP32-D0WD, revision v1.0.
- Flash size: 16 MB.
- Flash mode/frequency in original images: DIO, 80 MHz.
- Secure boot: disabled.
- Flash encryption: disabled.
- UART flashing works via `/dev/cu.usbserial-110` on the tested machine.

## Confirmed By User

- Display hardware is exactly the same as the TR19 badge display.

## Inferred From Dump

- Original firmware is MicroPython-style and exposes modules such as `ugfx`, `badge`, `system`, `wifi`, `buttons`, `keyboard`, and `_pca9555mapping`.
- The top-level filesystem image is FAT, despite the old `locfd_spiffs.bin` name.
- `ota_1.bin` appears blank or erased in the dump.

## Still To Map

- Exact display pins if they differ from TR19 board wiring.
- Button GPIO or I/O expander mapping.
- LED/haptic peripherals.
- Power management behavior.
- Whether the TR22 badge expects any extra coprocessor or peripheral startup sequence.
