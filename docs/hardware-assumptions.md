# Hardware Assumptions

Known or strongly suspected facts for the TR22 badge.

## Confirmed From Device Probe

- Chip: ESP32-D0WD, revision v1.0.
- ESP module marking on back: Espressif ESP32-WROVER-B.
  - This module contains the ESP32-D0WD, SPI flash, and PSRAM inside/under the module shield.
  - Given the probed/original flash size is 16 MB, the installed module is likely a 16 MB flash variant such as ESP32-WROVER-B-N16R8, but the exact ordering suffix has not been visually confirmed.
- Flash size: 16 MB.
- Flash mode/frequency in original images: DIO, 80 MHz.
- Secure boot: disabled.
- Flash encryption: disabled.
- UART ROM bootloader access works via `/dev/cu.usbserial-110` on the tested machine.
- UART flashing **does work** in practice with `esptool write_flash` (verified: full
  1 MB app image written and hash-verified). The unreliable parts are the
  read-side probes (`flash_id`, `read_flash_status`, `read_flash`), not the
  write path. Use `tools/flash-firmware.sh --skip-flash-probe` to bypass the
  probe on this badge. See investigation below and `tools/README.md`.

## esptool / Download-Mode Flash Investigation (both badges, reproducible)

### What works
- `read-mac` / chip identification always succeeds — the ROM UART protocol is alive.
- The badge boots normally and runs the original Troopers 2022 MicroPython firmware — flash is physically healthy in normal boot mode.

### What fails (confirmed on both tested badges, with and without stub, cold power-cycle into bootloader)
- `flash-id` → `Manufacturer: 00`, `Device: 0000`, size unknown. Every path tried: stub, `--no-stub`, explicit `--spi-connection 6,7,8,9,11`, `--spi-connection HSPI`.
- `read-flash` (stub) → returns data but it is garbage: address `0x1000` returns all zeros, `0x8000` / `0x10000` do not match expected magic bytes. Confirmed the badge is running firmware that requires a valid bootloader at `0x1000`, so these reads are unreliable artefacts, not real flash content.
- `read-flash-status` → always `0x0000`.
- `dump-mem 0x3F400000` (flash MMU cache) → all `0xBAD00BAD` — cache is disabled in download mode, as expected.
- `write-flash` with stub → fails deterministically after writing exactly **16 384 bytes** (one compressed FLASH_DEFL_BLOCK). Chip stops responding. Reproduced twice.
- `write-flash --no-stub` → ROM begins erase, then "Serial data stream stopped" before any data is written.

### Most likely root cause
The ESP32-WROVER-B has 8 MB PSRAM sharing the same SPI data bus (GPIO6–11) as the internal flash. **GPIO16 and GPIO17 are the PSRAM control signals** (confirmed by Espressif datasheet). In download mode the SPI controller releases these pins; PSRAM CS (GPIO16) floats to an indeterminate state and may be asserted, causing the PSRAM to drive MISO simultaneously with the flash during every SPI transaction. This corrupts all reads and likely corrupts the status-register polling that the flash write driver depends on (WIP / WEL bits), causing write operations to misbehave or the badge to reset mid-operation.

### What was ruled out
- GPIO12 / flash voltage strapping: confirmed 3.3 V, not the issue.
- Flash encryption / secure boot: both disabled (eFuse).
- Custom SPI pad mapping: `SPI_PAD_CONFIG_*` eFuses all zero.
- Single bad chip: same behaviour on two separate badges.
- Cable / baud rate: tested 115200, lower rates, multiple attempts.

### Open question
Why esptool ever worked on these badges previously is unknown. Possible explanations: an older esptool version with different stub behaviour; a different connection setup that incidentally held PSRAM CS high; or the badges were flashed at the factory before the Troopers firmware was loaded (when flash was in a different SPI mode).
- ESP32 eFuse `SPI_PAD_CONFIG_*` values are all `0`, so there is no custom flash pad mapping burned into eFuse. UART bootloader expects the standard ESP32 flash bus:
  - CLK GPIO6
  - Q / MISO GPIO7
  - D / MOSI GPIO8
  - HD GPIO9
  - CS0 GPIO11
- eFuse security state remains recovery-friendly: `FLASH_CRYPT_CNT=0`, `DISABLE_DL_ENCRYPT=false`, `DISABLE_DL_DECRYPT=false`, `DISABLE_DL_CACHE=false`.
- Flash voltage is determined by GPIO12/MTDI on reset. GPIO12 high selects 1.8 V; low/floating selects 3.3 V. Avoid pulling GPIO12 high during reset unless the flash is known to be 1.8 V.

## Confirmed By User

- Display hardware is exactly the same as the TR19 badge display.

## Visible Board Chips

User-read top markings:

- Chip 1: `GH12E` / `535MB8`
  - Likely AP2114H-3.3 3.3 V LDO regulator based on `GH12E` SMD marking references.
  - Not expected to be the ESP32 boot flash.
- Chip 2: `CP2102N` / `AD148C` / `1836 A`
  - Silicon Labs CP2102N USB-to-UART bridge.
  - USB serial path appears functional because ESP32 ROM `read_mac` works when the badge is in download mode.
- Chip 3: `PCM5 100A` / `88 58T` / `GLOFG4`
  - Likely TI PCM5100A-family audio DAC based on `PCM5 100A` style marking.
  - Not expected to be involved in boot/flash recovery.
- Chip 4: `NXP` / `PCA9555` / `CS3991 03` / `TnG1022A`
  - NXP PCA9555 I/O expander.
- Chip 5: `Azoteq` / `IQS550` / `GQ21E 16VG` / `CHN GQ 813` / `23 Y`
  - Azoteq IQS550 touch controller.

None of the visible chips above is the ESP32 boot SPI flash. With the Espressif ESP32-WROVER-B marking confirmed, the boot flash is inside/under the WROVER module shield rather than exposed as a separate board-level chip.

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

## Buttons:

I found useful TR19 evidence. The buttons are probably not direct ESP32 GPIO buttons. TR19 uses I2C expanders on:

  - SDA = GPIO4
  - SCL = GPIO5
  - 400 kHz

  Main nav buttons are on a PCA9539A at 0x77, interrupt on GPIO39 in TR19.
  TR22 has an observed PCA9555-class expander at 0x20 on the same bus.
  Live native-firmware polling on TR22 showed the nav buttons on port 0:

  | Expander pin | Native packed mask | Button |
  | ------------ | ------------------ | ------ |
  | P0_5         | 0x0020             | A      |
  | P0_6         | 0x0040             | B      |
  | P0_7         | 0x0080             | START  |
  | P0_1         | 0x0002             | joystick up |
  | P0_2         | 0x0004             | joystick down |
  | P0_0         | 0x0001             | joystick left |
  | P0_3         | 0x0008             | joystick right |
  | P0_4         | 0x0010             | joystick press |

  `BADGE_BTN_NAV_A_MASK` therefore uses `0x0020`.

  Earlier TR19 evidence used the mapping array order `P0_7..P0_0`, then
  `P1_7..P1_0`, and mapped A to P1_5:

  | Index | Expander pin | Native packed mask | Button |
  | ----- | ------------ | ------------------ | ------ |
  | 8     | P1_7         | 0x8000             | START  |
  | 9     | P1_6         | 0x4000             | B      |
  | 10    | P1_5         | 0x2000             | A      |
  | 11    | P1_4         | 0x1000             | SELECT |
  | 12    | P1_3         | 0x0800             | UP     |
  | 13    | P1_2         | 0x0400             | RIGHT  |
  | 14    | P1_1         | 0x0200             | LEFT   |
  | 15    | P1_0         | 0x0100             | DOWN   |

  Keyboard expanders are also present:

  - PCA9555 at 0x25, interrupt on GPIO35
  - PCA9555 at 0x24, interrupt on GPIO34

  Keyboard expander 0 (`0x25`) mapping:

  | Index | Key |
  | ----- | --- |
  | 0 | G |
  | 1 | B |
  | 2 | H |
  | 3 | RETURN |
  | 4 | M |
  | 5 | N |
  | 6 | SHIFT |
  | 7 | BACKSPACE |
  | 8 | J |
  | 9 | K |
  | 10 | L |
  | 11 | Y |
  | 12 | U |
  | 13 | I |
  | 14 | O |
  | 15 | P |

  Keyboard expander 1 (`0x24`) mapping:

  | Index | Key |
  | ----- | --- |
  | 0 | Q |
  | 1 | A |
  | 2 | Z |
  | 3 | SHIELD |
  | 4 | W |
  | 5 | S |
  | 6 | X |
  | 7 | FN |
  | 8 | T |
  | 9 | V |
  | 10 | F |
  | 11 | R |
  | 12 | SPACE |
  | 13 | C |
  | 14 | D |
  | 15 | E |

  The flash/boot button is direct GPIO0, active low.

  Relevant files:

  - /Users/niclasfrey/Desktop/badge/tr22-badge/esp-kit/firmware/python_modules/troopers2019/buttons.py:179
  - /Users/niclasfrey/Desktop/badge/tr19-badge/upstream/tr19-badge-firmware/ports/esp32/modules/system/input.py:224
  - /Users/niclasfrey/Desktop/badge/tr22-badge/esp-kit/firmware/python_modules/troopers2019/system.py:15

  For battery: TR19 has only disabled/commented-out battery sensing. It mentions:

  VBAT = ADC GPIO35, factor 3.1603
  VUSB = ADC GPIO34, factor 3.1436

  …but the actual functions return 0, and those same pins are used as keyboard expander interrupt lines. So TR19 likely did not have working battery sensing in firmware, or it was abandoned because it
  conflicted with the button hardware.

  Relevant file:

  - /Users/niclasfrey/Desktop/badge/tr22-badge/esp-kit/firmware/python_modules/troopers2019/badge.py:5

  Conclusion: our GPIO debugger should pivot from raw GPIO button scanning to an I2C expander debugger. Probe 0x24, 0x25, and 0x77, read their input registers live, decode the TR19 mappings, and ship
  those events to the log server. TR19 does not give a trustworthy battery pin.

## Battery Follow-Up

The GPIO35/GPIO39 battery ADC candidates did not produce a plausible value on the tested TR22 badge. The next places to look are I2C peripherals on the TR19/Troopers bus pins:

- Primary I2C candidate: SDA GPIO4, SCL GPIO5.
- Fallback I2C candidate from ESP32 defaults/probe notes: SDA GPIO21, SCL GPIO22.
- DRV2605-style haptic controller: address 0x5a, VBAT monitor register 0x21. TI documents the conversion as `raw * 5.6 V / 255`, but the value is only meaningful when the haptic output is active.
- Older Disobey SAMD status device: address 0x30, register 0, two-byte state. The high byte was exposed by old firmware as `read_battery()`, but its units are not confirmed for TR22.

Native firmware now tries those I2C probes first and then falls back to the ADC sweep.
