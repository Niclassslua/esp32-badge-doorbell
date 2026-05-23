# Roadmap

## Milestone 0: Recovery Confidence

- Keep original partition images in `../tr22-badge/flash-dump/`.
- Verify the recovery command works before doing destructive experiments.
- Optionally create a full 16 MB backup from the exact physical badge being tested.

## Milestone 1: Display Bring-Up

- Compare TR19 `moddisplay` assumptions against the TR22 board.
- Build the smallest firmware that initializes the display.
- Draw a black/white test pattern.
- Draw a text string.

## Milestone 2: Input Bring-Up

- Identify buttons and any I/O expander mapping.
- Build a button test screen.
- Log button events over UART.

## Milestone 3: Badge Shell

- Add a simple menu.
- Add a filesystem-backed config file.
- Add one custom app.

## Milestone 4: Usable Custom Firmware

- Add sleep/wake handling.
- Add Wi-Fi setup if needed.
- Add an app loader or simple launcher.
- Decide whether to stay on MicroPython or move to native ESP-IDF for the long term.
