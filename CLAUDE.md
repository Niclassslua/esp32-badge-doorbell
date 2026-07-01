# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Custom ESP-IDF firmware for the TROOPERS 22 (TR22) badge — ESP32-WROVER-B, 16 MB flash, 2.9" e-paper display (TR19-compatible), WS2812 LEDs, IQS550 capacitive touch keyboard, DRV2605L haptic driver, and a PCA9555 nav-button expander all sharing one I²C bus. The badge acts as a smart door-sign / doorbell with optional Home Assistant integration.

Active project lives at `firmware/native-esp32/`. The repo also contains helper scripts (`tools/`), hardware notes (`docs/`), and an in-tree ESP-IDF checkout (`.deps/esp-idf/`).

## Build & flash

ESP-IDF **5.5** is required and lives at `.deps/esp-idf/`. All build/flash helpers source `tools/esp-idf-env.sh`, which runs the in-tree `export.sh` and forces Python 3.12 onto PATH (Python 3.14 is not supported by IDF 5.5).

```bash
# Normal app build (writes build/tr22_custom.bin):
tools/build-firmware.sh

# Recovery image (separate build dir → build-recovery/tr22_recovery.bin):
tools/build-recovery-firmware.sh

# Flash both images + bootloader + partition table + otadata in one esptool session:
tools/flash-firmware.sh --port /dev/cu.usbserial-110
```

Hand-running idf.py without the wrappers:

```bash
.deps/esp-idf/tools/idf.py -C firmware/native-esp32 build
```

When auto-reset doesn't fire, hold **BOOT**, tap **RESET**, release BOOT once the script reports `ROM loader detected`. If the SPI flash probe fails (`Manufacturer 00`, size Unknown), pass `--skip-flash-probe` — see `docs/hardware-assumptions.md` for the root cause: the WROVER-B PSRAM CS line (GPIO16) floats in download mode and corrupts the SPI probe, but `write_flash` itself works fine.

`tools/detect-bootloader.sh` is a read-only sanity check that the BOOT/RESET dance landed in ROM mode.

### Dev OTA loop (no power-cycle needed)

```bash
# 1. Serve the current build — re-reads from disk on every GET:
python3 tools/ota-server.py firmware/native-esp32/build/tr22_custom.bin

# 2. Rebuild + ask the running badge to enter recovery so it picks it up:
tools/badge.py --host <badge-ip-or-hostname> ota --build
```

The badge's `POST /ota` handler does **not** install anything itself — it just selects the recovery slot and reboots. **All OTA installs actually happen in recovery** (validated download, partition write, swap, reboot). The same path is triggered by holding the **START** nav button for 3 s on the running app.

The build script touches `.deps/esp-idf/components/esp_app_format/esp_app_desc.c` before each build so the embedded `__DATE__`/`__TIME__` refresh — otherwise the OTA HEAD check sees an identical app_desc and skips the download.

## High-level architecture

### Boot flow (`main/main.c → app_main`)

1. **Recovery gate** — `recovery_boot_button_held()` polls GPIO0 for 1.5 s; if held, sets boot partition to `ota_0` (recovery) and restarts.
2. **Power management** — `esp_pm_configure()` enables 10 → CPU_FREQ MHz DFS with auto light-sleep. Requires tickless idle (set in `sdkconfig.defaults`).
3. **Cancel rollback** — `esp_ota_mark_app_valid_cancel_rollback()` so a later OTA handoff into recovery is allowed by the OTA state machine.
4. **I²C → LEDs → state** — shared `badge_i2c_init()`, then `badge_led_init()`, then `badge_state_init()` (loads custom text from NVS, spawns the **display task**, triggers first render).
5. **Night-mode watchdog** — `badge_nightmode_init()` runs SNTP; if the badge boots inside 23:00–08:00 Berlin it renders "KLINGEL DEAKTIVIERT" and enters deep sleep with RTC timer wakeup. **This call may never return.**
6. **Buttons / touch / nav** — `badge_button_init()` (touch press → doorbell ring) and `badge_nav_init(on_start_hold_ota)` (PCA9555 START-held-3s → OTA worker).
7. **Idle** — everything else is event-driven; `app_main` ends in `vTaskDelay(5000)` forever.

### Two-image OTA layout (counter-intuitive — read before touching partitions)

`partitions_ota.csv` defines `ota_0` (1.5 MB) and `ota_1` (1.5 MB) plus `nvs`/`otadata`/`phy_init`.

- `ota_0` = **recovery firmware** (`tools/build-recovery-firmware.sh`, builds with `-DRECOVERY_FIRMWARE=1`).
- `ota_1` = **normal badge app** (`tools/build-firmware.sh`).

The normal app cannot self-update without overwriting recovery, so `ota_update_reboot_to_recovery_if_server_newer()` just selects `ota_0` and reboots. `recovery_main.c` then does the validated OTA install into `ota_1`, marks it bootable, and restarts. SD-card installs (`/sdcard/TR22/app.bin`) go through the same code path via `sd_recovery_install_app()`.

Both apps call `esp_ota_mark_app_valid_cancel_rollback()` shortly after entry so the bootloader's rollback machinery doesn't fight the slot-swap.

### State + display ownership (`main/badge_state.c`)

`badge_state.c` is the single owner of door-sign state (`SIGN_PLEASE_RING` / `SIGN_DO_NOT_DISTURB` / `SIGN_NIGHT_MODE`). All other tasks go through `badge_state_set_*` / `badge_state_get_*`, which take a mutex and notify the **display task** via `xTaskNotifyGive`. The display task is the only caller of `tr19_epaper_*` in normal operation; rapid state-change bursts collapse into one refresh (`ulTaskNotifyTake(pdTRUE, …)` drains the count).

Persistence rules: custom text persists to NVS; door-sign state intentionally resets to `SIGN_PLEASE_RING` on every boot. A periodic refresh task pokes the display every `BADGE_DISPLAY_REFRESH_MS` (45 min) so battery % / IP don't sit frozen.

**`SIGN_NIGHT_MODE` is special**: `badge_state_enter_nightmode()` renders synchronously (display task is skipped) so it cannot race with the deep-sleep that immediately follows.

### WiFi is off-by-default

`wifi_connect.c` exposes an on-demand pattern: `wifi_ensure_up(timeout_ms)` / `wifi_release()`. NVS, netif, and the event loop are initialised once and never torn down — only the WiFi driver is started/stopped because that's what dominates current draw. Every caller (OTA worker, doorbell-ring → Home Assistant flash, SNTP sync) pairs an `ensure_up` with a `release`. Keep this pattern when adding new network features.

The radio runs at `WIFI_PS_MAX_MODEM` by default. SNTP explicitly switches to `WIFI_PS_NONE` for the sync window because aggressive power-save drops the AP connection before NTP replies arrive; the next `wifi_release()` resets it implicitly.

`s_ip_addr` is deliberately **not** cleared on disconnect — the display footer reads it, and clearing it would make the address vanish a few seconds after every doorbell ring.

### Night mode (`main/badge_nightmode.c`)

Berlin time (`CET-1CEST,M3.5.0,M10.5.0/3`). Window is `[23:00, 24:00) ∪ [0:00, 08:00)`. SNTP servers: `192.168.178.1` (Fritz.Box) primary, `pool.ntp.org` fallback. Outside the window, `nightmode_task` `vTaskDelay`s the exact seconds until 23:00 — tickless idle keeps the CPU in light-sleep across the delay, no polling loop needed. ESP32 deep-sleep is ~5–10 µA, so the night window is the dominant battery-saving feature.

Compile-time overrides for bench testing (default 0 = production):

```bash
tools/build-firmware.sh \
  -DBADGE_NIGHTMODE_TEST_TRIGGER_NOW=1 \
  -DBADGE_NIGHTMODE_TEST_SLEEP_S=60
```

### Buttons / touch / nav

- **Status button** (`BADGE_BTN_STATUS_GPIO`, currently `GPIO_NUM_NC` = disabled) — toggles please-ring / DnD; same effect as `POST /status`.
- **IQS550 touch keyboard** (`badge_touch.c`, I²C 0x74, polled @ 4 Hz) — any finger-down rings the doorbell.
- **Nav buttons** (`badge_nav.c`, PCA9555 @ 0x20 with 0x77 fallback) — only START is wired; holding it 3 s spawns the OTA worker.

Anywhere a small-stack polling task triggers heavy network work, the code **spawns an 8 KB worker task and returns immediately** — see `task_ring` in `badge_button.c` and `ota_worker_task` in `main.c`. The polling tasks themselves only have 3 KB, nowhere near enough for `esp_http_client` + `esp_wifi_init` + `esp_image_verify`. Follow this pattern.

`reserve_ring_slot()` gates every ring attempt by current state, a mutex, and `BADGE_RING_COOLDOWN_MS` (5 s) — touch chatter cannot stack rings.

### CMakeLists conditionals to know about

`main/CMakeLists.txt` branches on two cmake variables:

- `-DRECOVERY_FIRMWARE=1` builds the minimal recovery image (different source set, defines `RECOVERY_FIRMWARE` for the preprocessor).
- `-DGPIO_DEBUG_MODE=1` requires also passing `-DALLOW_DANGEROUS_DEBUG=1` (FATAL_ERROR otherwise). GPIO debug blocks normal boot and OTA — only use for deliberate bench bring-up.

Add new IDF components via the `PRIV_REQUIRES` block in `main/CMakeLists.txt`.

### Configuration files with secrets

These live in-tree (no env-var indirection — easier when flashing). Check before committing:

- `main/ota_config.h` — WiFi SSID/password, OTA + log-ship URLs.
- `main/badge_config.h` — Home Assistant long-lived token + entity ID, plus all per-feature pin / timer / brightness tunables.

## On-badge HTTP API (when `BADGE_ENABLE_HTTP_SERVER=1`)

**Default is off** so WiFi can stay down at idle. Enable in `badge_config.h` and keep WiFi up at boot. Endpoints documented in `main/badge_server.h`; `tools/badge.py` is the canonical client (`--host <ip-or-hostname>`, default `badge.local`). `BADGE_ENABLE_LOG_SHIP` is similarly off-by-default because it forces the radio on.

## Code conventions worth keeping

- All `tr19_epaper_*` calls go through the display task in normal operation. The one synchronous exception is the night-mode render.
- `badge_led_off()` must be called on the night-mode path or the WS2812s keep drawing through deep sleep.
- Boot flow lives in `app_main` and is sequenced for a reason — don't reorder I²C / LED / state / nightmode without understanding the deep-sleep handoff.
- Logging uses ESP-IDF `ESP_LOG*` macros. `LOG_DEFAULT_LEVEL=INFO` strips DEBUG strings from the binary; raise it back if you turn `BADGE_ENABLE_LOG_SHIP` on.
