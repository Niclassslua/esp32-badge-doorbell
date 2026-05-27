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
#include "badge_power.h"
#include "badge_server.h"
#include "badge_state.h"
#include "gpio_debug.h"
#include "log_ship.h"
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
    const int min_freq_mhz = 40;
    esp_pm_config_t pm_config = {
        .max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        .min_freq_mhz = min_freq_mhz,
        .light_sleep_enable = false,
    };
    esp_err_t err = esp_pm_configure(&pm_config);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "power management: DFS %d-%d MHz, light sleep disabled",
                 min_freq_mhz, CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
    } else {
        ESP_LOGW(TAG, "power management setup failed: %s", esp_err_to_name(err));
    }
#endif
}

static bool run_ota_before_app_start(void)
{
    bool wifi_ok = (wifi_connect_sta() == ESP_OK);
    if (!wifi_ok) {
        return false;
    }

    /*
     * In the current two-slot recovery layout, ota_0 is reserved for recovery
     * and ota_1 is the app. The app must not use round-robin OTA because that
     * would eventually overwrite recovery. If the server has a new image, boot
     * recovery and let recovery write ota_1 while it is not running.
     */
    esp_err_t err = ota_update_reboot_to_recovery_if_server_newer(recovery_partition());
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "safe OTA handoff failed: %s", esp_err_to_name(err));
    }
    return true;
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

    /*
     * Install the log-ship vprintf hook before the OTA gate so its decisions
     * are captured into the ring buffer and shipped once WiFi is up.
     */
    log_ship_init();
    configure_power_saving();

    /*
     * Reaching app_main means the bootloader handed us control. Mark the
     * running image valid now so the OTA gate can call
     * esp_ota_set_boot_partition(recovery) — that call returns
     * ESP_ERR_OTA_ROLLBACK_INVALID_STATE while we sit in PENDING_VERIFY.
     * The trade-off is no auto-rollback for crashes between here and the
     * end of init; recovery (SD or WiFi OTA) remains the fallback for bad apps.
     */
    esp_err_t valid_err = esp_ota_mark_app_valid_cancel_rollback();
    if (valid_err != ESP_OK && valid_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "mark app valid failed: %s", esp_err_to_name(valid_err));
    }

    bool wifi_ok = run_ota_before_app_start();

    if (wifi_ok) {
        log_ship_wifi_ready();
    }

    log_chip_info();

#if GPIO_DEBUG_MODE
    if (!wifi_ok) {
        ESP_LOGW(TAG, "GPIO debug running without WiFi/log shipping");
    }

    gpio_debug_scan();   /* print pull-up/pull-down probe table */
    gpio_debug_watch();  /* block forever, logging every edge   */
    /* unreachable */
#endif

    /* ------------------------------------------------------------------ */
    /* 1. LEDs/state/display — only starts after the OTA gate has completed. */
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
    /* 2. Network services — WiFi is already connected if wifi_ok is true.  */
    /* ------------------------------------------------------------------ */
    if (wifi_ok) {
        badge_state_refresh_display();

        /* ---------------------------------------------------------------- */
        /* 3. HTTP server                                                    */
        /* ---------------------------------------------------------------- */
        esp_err_t srv_err = badge_server_start();
        if (srv_err != ESP_OK) {
            ESP_LOGE(TAG, "HTTP server failed: %s", esp_err_to_name(srv_err));
            /* Non-fatal — device still works locally via buttons. */
        }
    } else {
        ESP_LOGW(TAG, "WiFi unavailable — running offline "
                      "(no HTTP server, doorbell POST disabled)");
    }

    /* ------------------------------------------------------------------ */
    /* 4. Buttons                                                           */
    /* ------------------------------------------------------------------ */
    esp_err_t btn_err = badge_button_init();
    if (btn_err != ESP_OK) {
        ESP_LOGE(TAG, "button init: %s (check GPIO pins in badge_config.h)",
                 esp_err_to_name(btn_err));
        /* Non-fatal — device still works via HTTP. */
    }

    /* ------------------------------------------------------------------ */
    /* 5. Idle — all work is event-driven:                                 */
    /*    • display task  wakes on xTaskNotifyGive from state setters      */
    /*    • HTTP handlers call badge_state_set_* and return immediately     */
    /*    • button tasks  call badge_state_set_* on press                  */
    /* ------------------------------------------------------------------ */
    ESP_LOGI(TAG, "boot complete — door sign active");
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
