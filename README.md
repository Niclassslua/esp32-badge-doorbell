# TR22 Custom Firmware

Local workspace for building our own TROOPERS 22 badge firmware.

This repo starts from what we know:

- The badge is a classic ESP32 with 16 MB flash.
- Secure boot and flash encryption were not enabled on the tested device.
- The TR22 display is believed to be the same as the TR19 display; `firmware/native-esp32/main/tr19_epaper.c` is our own driver written against that assumption.

If you have a backup of the original stock firmware, `tools/backup-original.sh`
and `tools/restore-original.sh` can create/restore it; see
`docs/recover-original-firmware.md` for details.

## Layout

- `docs/`: hardware notes, recovery instructions, and roadmap.
- `firmware/`: our firmware experiments and eventual build targets.
- `tools/`: local helper scripts.

## Current Strategy

Work in layers:

1. Keep the original TR22 dump as a reliable recovery image.
2. Reuse or adapt the TR19 ePaper display driver first.
3. Build a minimal ESP32 firmware that can draw text or pixels.
4. Add button/input mapping.
5. Add storage, app loading, and nicer system behavior later.

The first serious milestone is simple: boot custom firmware and show a visible message on the badge display.
