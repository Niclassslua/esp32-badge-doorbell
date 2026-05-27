# Tools

Small helper scripts for flashing, backup, hardware probing, and asset conversion.

## Firmware Build

From this directory:

```bash
./build-firmware.sh
./build-firmware.sh -DNAME=value
```

Extra arguments are passed to `idf.py build`.

## Dev OTA Loop

Start the OTA file server from the repo root and leave it running:

```bash
python3 tools/ota-server.py firmware/native-esp32/build/tr22_custom.bin
```

Then rebuild and ask the running badge to enter recovery:

```bash
tools/badge.py --host <badge-ip-or-hostname> ota --build
```

The badge's `POST /ota` handler only selects the recovery slot and restarts.
Recovery performs the same validated OTA install used at boot, so the normal
app is updated without unplugging the badge.

## Recovery Build

The current safe layout keeps the minimal recovery firmware in `ota_0` and
installs the normal app into `ota_1`.

```bash
./build-recovery-firmware.sh
./build-firmware.sh
./prepare-sd-update.sh /Volumes/SDCARD
```

`flash-firmware.sh` now defaults to `firmware/native-esp32/build-recovery`
because flashing an app image at `0x10000` would overwrite recovery.

## Flashing the Badge

```bash
./flash-firmware.sh --port /dev/cu.usbserial-110
```

Hold **BOOT**, tap **RESET**, release BOOT once the script reports
`ROM loader detected`. The script then probes the SPI flash and, if that
succeeds, writes bootloader, partition table, otadata, and app in one esptool
session.

### Known issue: `flash_id` probe is a false negative on this hardware

On the TR22 badge the ESP32-WROVER-B contains 8 MB of PSRAM that shares the
internal flash SPI bus. In download mode the PSRAM CS line (GPIO16) floats,
which corrupts the SPI status-register reads that `esptool flash_id` depends
on, so the probe loops with:

```
WARNING: Failed to communicate with the flash chip ...
Manufacturer: 00
Device: 0000
Detected flash size: Unknown
```

`write_flash` itself works fine on these badges — only the probe is unreliable.
Bypass it with:

```bash
./flash-firmware.sh --port /dev/cu.usbserial-110 --skip-flash-probe
```

Background and root-cause analysis is in
[`docs/hardware-assumptions.md`](../docs/hardware-assumptions.md).

### Recovering from "wrong-offset" boot loop

A common mistake when flashing manually is sending the app to the bootloader
offset:

```bash
# WRONG — overwrites bootloader, partition table, and otadata
esptool ... write_flash 0x1000 tr22_custom.bin
```

The symptom is an endless ROM-loader loop on the serial console:

```
ets Jun  8 2016 00:22:57
rst:0x10 (RTCWDT_RTC_RESET),boot:0x1f (SPI_FAST_FLASH_BOOT)
...
load:0x3f400020,len:184668
ets Jun  8 2016 00:22:57
...
```

`len:184668` (≈180 KB) loaded into the DROM cache region `0x3f400020` is the
giveaway: a real second-stage bootloader is ~25 KB and loads into IRAM/DRAM,
so this is an app image being interpreted as a bootloader. The RTC watchdog
fires before anything finishes.

Recovery is to re-flash all four images at their correct offsets via
`./flash-firmware.sh` (use `--skip-flash-probe` if the PSRAM probe fails).
The fixed offsets for this board are:

| Offset    | File                                  |
|-----------|---------------------------------------|
| `0x1000`  | `bootloader/bootloader.bin`           |
| `0x8000`  | `partition_table/partition-table.bin` |
| `0xd000`  | `ota_data_initial.bin`                |
| `0x10000` | `tr22_custom.bin` (app, ota_0 slot)   |

The app never goes to `0x1000` — that offset is fixed by the ESP32 mask ROM
for the second-stage bootloader.
