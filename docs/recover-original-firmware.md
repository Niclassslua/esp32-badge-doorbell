# Recover Original Firmware

The original TR22 dump lives at:

```text
../tr22-badge/flash-dump/
```

The tested badge is an ESP32 with 16 MB flash and no secure boot or flash encryption, so recovery is normal `esptool` flashing.

From `../tr22-badge/flash-dump/`, run:

```bash
python3 -m esptool --chip esp32 --port /dev/cu.usbserial-110 --baud 921600 \
  --before default-reset --after hard-reset write-flash \
  --flash_mode dio --flash_freq 80m --flash_size 16MB \
  0x1000 bootloader.bin \
  0x9000 nvs.bin \
  0xd000 otadata.bin \
  0xf000 phy_init.bin \
  0x10000 ota_0.bin \
  0x210000 ota_1.bin \
  0x404000 locfd_spiffs.bin
```

If a board has been fully erased, confirm the correct partition table is present at `0x8000` before expecting the original firmware to boot.

Before flashing experimental firmware, consider reading a fresh backup:

```bash
python3 -m esptool --chip esp32 --port /dev/cu.usbserial-110 --baud 921600 \
  read-flash 0x0 0x1000000 tr22-full-backup.bin
```
