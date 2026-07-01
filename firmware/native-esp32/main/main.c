#include <inttypes.h>
#include <stdio.h>

#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_pm.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "badge_button.h"
#include "badge_config.h"
#include "badge_i2c.h"
#include "badge_led.h"
#include "badge_nav.h"
#include "badge_nightmode.h"
#include "badge_power.h"
#include "badge_server.h"
#include "badge_state.h"
/* badge_state.h provides badge_state_get_state(); already pulled in above. */
#include "gpio_debug.h"
#include "log_ship.h"
#include "ota_config.h"
#include "ota_update.h"
#include "recovery_boot.h"
#include "tr19_epaper.h"
#include "wifi_connect.h"

/*
 * GPIO_DEBUG_MODE — set to 1 to run the pin scanner + live edge monitor
 * at boot instead of the normal badge firmware.  Flip back to 0 to restore
 * normal operation.  You can also pass -DGPIO_DEBUG_MODE=1 on the cmake
 * command line without touching this file.
 */
#ifndef GPIO_DEBUG_MODE
#define GPIO_DEBUG_MODE 0
#endif

static const char *TAG = "tr22";

static void configure_power_saving(void)
{
#if CONFIG_PM_ENABLE
    const int min_freq_mhz = 10;
    esp_pm_config_t pm_config = {
        .max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        .min_freq_mhz = min_freq_mhz,
        .light_sleep_enable = true,
    };
    esp_err_t err = esp_pm_configure(&pm_config);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "power management: DFS %d-%d MHz, light sleep enabled",
                 min_freq_mhz, CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
    } else {
        ESP_LOGW(TAG, "power management setup failed: %s", esp_err_to_name(err));
    }
#else
    ESP_LOGW(TAG, "power management disabled in sdkconfig; idle current will be high");
#endif
}

static void log_chip_info(void)
{
    esp_chip_info_t chip_info;
    uint32_t flash_size = 0;

    esp_chip_info(&chip_info);
    esp_flash_get_size(NULL, &flash_size);

    ESP_LOGI(TAG, "Hello from custom TR22 firmware");
    ESP_LOGI(TAG, "Cores: %d, revision: %d", chip_info.cores, chip_info.revision);
    ESP_LOGI(TAG, "Features: WiFi%s%s",
             (chip_info.features & CHIP_FEATURE_BT)  ? ", BT"  : "",
             (chip_info.features & CHIP_FEATURE_BLE) ? ", BLE" : "");
    ESP_LOGI(TAG, "Flash: %" PRIu32 " bytes", flash_size);
}

/*
 * OTA worker task — runs the actual WiFi + OTA flow.
 *
 * Spawned with 8 KB stack from the nav callback (the nav task itself only
 * has 3 KB, not enough headroom for esp_wifi_init + esp_http_client +
 * esp_image_verify). Same pattern as task_ring in badge_button.c.
 *
 * Notes:
 * - ota_update_reboot_to_recovery_if_server_newer() returns ESP_OK *both*
 *   for "no update needed" and "update applied & rebooted" (the reboot path
 *   never returns). Loud INFO logs make it obvious on the UART which branch
 *   ran without diving into the source.
 */
static void ota_worker_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "=== OTA worker started ===");

    if (wifi_ensure_up(15000) != ESP_OK) {
        ESP_LOGW(TAG, "OTA aborted: WiFi did not connect within 15s "
                      "(check OTA_WIFI_SSID/OTA_WIFI_PASSWORD in ota_config.h "
                      "and that the AP is reachable)");
        goto cleanup;
    }

    ESP_LOGI(TAG, "OTA: WiFi up, querying server at %s", OTA_FIRMWARE_URL);
    const esp_partition_t *recovery = recovery_partition();
    if (recovery == NULL) {
        ESP_LOGE(TAG, "OTA aborted: no recovery partition (ota_0) found — "
                      "is the partition table correct? did you flash recovery firmware?");
        wifi_release();
        goto cleanup;
    }

    esp_err_t err = ota_update_reboot_to_recovery_if_server_newer(recovery);
    if (err == ESP_OK) {
        /* If a newer build was found the helper rebooted into recovery and
         * we never reach this line. Reaching here means the server's app
         * desc matches what we're running. */
        ESP_LOGI(TAG, "OTA: server build matches running app — nothing to do "
                      "(rebuild + restart ota-server.py to push a new image)");
    } else {
        ESP_LOGW(TAG, "OTA handoff failed: %s "
                      "(is tools/ota-server.py running on the host in OTA_FIRMWARE_URL?)",
                 esp_err_to_name(err));
    }
    wifi_release();

cleanup:
    /* Re-sync the LED chain with the persisted sign state — the red
     * acknowledgement flash earlier silently set the LED state to DnD. */
    badge_led_set_sign_state(badge_state_get_state() == SIGN_PLEASE_RING);
    ESP_LOGI(TAG, "=== OTA worker finished ===");
    vTaskDelete(NULL);
}

/*
 * START-hold-3s callback (runs on the nav polling task, 3 KB stack — must
 * stay light). Acknowledge with an LED flash, then hand off the heavy WiFi
 * + OTA work to a dedicated 8 KB task and return immediately.
 */
static void on_start_hold_ota(void)
{
    ESP_LOGI(TAG, "=== START hold detected — spawning OTA worker ===");
    badge_led_pulse_button_feedback(false);

    BaseType_t ok = xTaskCreate(ota_worker_task, "ota_worker",
                                8192, NULL, 5, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "OTA worker task creation failed (out of memory?)");
        badge_led_set_sign_state(badge_state_get_state() == SIGN_PLEASE_RING);
    }
}

void app_main(void)
{
    if (recovery_boot_button_held()) {
        esp_err_t err = recovery_reboot_to_recovery();
        if (err == ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(100));
            esp_restart();
        }
        ESP_LOGE(TAG, "recovery request ignored: %s", esp_err_to_name(err));
    }

#if BADGE_ENABLE_LOG_SHIP
    /*
     * Install the log-ship vprintf hook before anything else so its decisions
     * are captured into the ring buffer and shipped once WiFi is up.
     */
    log_ship_init();
#endif

    configure_power_saving();

    /*
     * Reaching app_main means the bootloader handed us control. Mark the
     * running image valid now so a later OTA handoff can call
     * esp_ota_set_boot_partition(recovery) without ESP_ERR_OTA_ROLLBACK_INVALID_STATE.
     * The trade-off is no auto-rollback for crashes between here and the
     * end of init; recovery (SD or WiFi OTA) remains the fallback for bad apps.
     */
    esp_err_t valid_err = esp_ota_mark_app_valid_cancel_rollback();
    if (valid_err != ESP_OK && valid_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "mark app valid failed: %s", esp_err_to_name(valid_err));
    }

    log_chip_info();

#if GPIO_DEBUG_MODE
    gpio_debug_scan();   /* print pull-up/pull-down probe table */
    gpio_debug_watch();  /* block forever, logging every edge   */
    /* unreachable */
#endif

    /* ------------------------------------------------------------------ */
    /* 1. Shared I2C + LEDs + persistent state                            */
    /* ------------------------------------------------------------------ */
    esp_err_t i2c_err = badge_i2c_init();
    if (i2c_err != ESP_OK) {
        ESP_LOGW(TAG, "shared I2C init failed: %s", esp_err_to_name(i2c_err));
    }

    esp_err_t led_err = badge_led_init();
    if (led_err != ESP_OK) {
        ESP_LOGW(TAG, "LED init failed: %s", esp_err_to_name(led_err));
    }
    ESP_ERROR_CHECK(badge_state_init());

    /* ------------------------------------------------------------------ */
    /* 2. Draw the initial display state. WiFi stays off — see plan.       */
    /* ------------------------------------------------------------------ */
    badge_state_refresh_display();

    /* ------------------------------------------------------------------ */
    /* 2b. Night-mode watchdog (23:00–08:00 Berlin deep sleep).            */
    /*     Syncs time via SNTP; may enter deep sleep and never return if   */
    /*     the device booted inside the night window.                       */
    /* ------------------------------------------------------------------ */
    esp_err_t nm_err = badge_nightmode_init();
    if (nm_err != ESP_OK) {
        ESP_LOGW(TAG, "nightmode init failed: %s", esp_err_to_name(nm_err));
    }

#if BADGE_ENABLE_HTTP_SERVER
    /*
     * The HTTP doorbell server is disabled by default to allow WiFi to stay
     * off in idle. Flip BADGE_ENABLE_HTTP_SERVER (and remember to keep WiFi
     * up at boot) to revive it.
     */
    if (wifi_ensure_up(OTA_WIFI_TIMEOUT_MS) == ESP_OK) {
        esp_err_t srv_err = badge_server_start();
        if (srv_err != ESP_OK) {
            ESP_LOGE(TAG, "HTTP server failed: %s", esp_err_to_name(srv_err));
        }
    }
#endif

    /* ------------------------------------------------------------------ */
    /* 3. Buttons (doorbell touch + status GPIO)                          */
    /* ------------------------------------------------------------------ */
    esp_err_t btn_err = badge_button_init();
    if (btn_err != ESP_OK) {
        ESP_LOGE(TAG, "button init: %s (check GPIO pins in badge_config.h)",
                 esp_err_to_name(btn_err));
        /* Non-fatal — device still works via touch + nav hold. */
    }

    /* ------------------------------------------------------------------ */
    /* 4. Nav buttons (PCA9555). START-hold-3s triggers OTA.              */
    /* ------------------------------------------------------------------ */
    esp_err_t nav_err = badge_nav_init(on_start_hold_ota);
    if (nav_err != ESP_OK) {
        ESP_LOGW(TAG, "nav init failed: %s (OTA via START hold disabled)",
                 esp_err_to_name(nav_err));
    }

    /* ------------------------------------------------------------------ */
    /* 5. Idle — all work is event-driven:                                */
    /*    • display task  wakes on xTaskNotifyGive from state setters      */
    /*    • doorbell ring is triggered by touch / status button            */
    /*    • OTA flow      starts when START is held for 3 s               */
    /* ------------------------------------------------------------------ */
    ESP_LOGI(TAG, "boot complete — door sign active (WiFi on-demand)");
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
