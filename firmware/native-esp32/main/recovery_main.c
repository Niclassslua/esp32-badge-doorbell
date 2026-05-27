#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "log_ship.h"
#include "ota_update.h"
#include "recovery_boot.h"
#include "recovery_config.h"
#include "sd_recovery.h"
#include "tr19_epaper.h"
#include "wifi_connect.h"

static const char *TAG = "tr22.recovery";

/*
 * OTA progress callback — called synchronously from the OTA task.
 *
 * Calling tr19_epaper_show_recovery() directly (rather than posting to an
 * async queue) is intentional: it serialises display updates with the
 * download so the partial-update optimisation in tr19_epaper_show_recovery
 * actually fires.
 *
 * With an async queue + xQueueOverwrite the OTA task fires CHECKING →
 * DOWNLOAD_START → DOWNLOADING(0%) in rapid succession before the display
 * task ever reads a message.  The queue ends up holding only the last entry,
 * s_recovery_line1 is still "RECOVERY" when the display task wakes, and
 * every OTA message triggers a full refresh instead of a partial one.
 *
 * Synchronous calls fix this:
 *   CHECKING  → full refresh (line1 changes "RECOVERY"→"OTA", ~2 s)
 *   DOWNLOAD_START → partial (line1 stays "OTA", ~0.3 s)
 *   DOWNLOADING N% → partial (~0.3 s each)
 *   INSTALLING / SUCCESS / FAILED → full or partial as appropriate
 *
 * The 0.3 s per partial update stalls the HTTP read briefly, but the remote
 * server's keepalive window is far longer than that, so the connection stays
 * open without needing a larger timeout.
 */
static void ota_progress_to_display(ota_progress_stage_t stage,
                                    int percent, const char *detail)
{
    char buf[32];
    switch (stage) {
    case OTA_PROGRESS_CHECKING:
        tr19_epaper_show_recovery("OTA", "CHECK SERVER");
        break;
    case OTA_PROGRESS_DOWNLOAD_START:
        if (detail && detail[0]) {
            snprintf(buf, sizeof(buf), "V %s", detail);
            tr19_epaper_show_recovery("OTA", buf);
        } else {
            tr19_epaper_show_recovery("OTA", "DOWNLOADING");
        }
        break;
    case OTA_PROGRESS_DOWNLOADING:
        if (percent >= 0) {
            snprintf(buf, sizeof(buf), "DL %d%%", percent);
            tr19_epaper_show_recovery("OTA", buf);
        } else {
            tr19_epaper_show_recovery("OTA", "DOWNLOADING");
        }
        break;
    case OTA_PROGRESS_INSTALLING:
        tr19_epaper_show_recovery("OTA", "INSTALLING");
        break;
    case OTA_PROGRESS_SUCCESS:
        tr19_epaper_show_recovery("OTA OK", "REBOOTING");
        break;
    case OTA_PROGRESS_FAILED:
        tr19_epaper_show_recovery("OTA FAIL", detail ? detail : "ERROR");
        break;
    }
}

static void log_partition_state(const esp_partition_t *running,
                                const esp_partition_t *app_part)
{
    const esp_app_desc_t *desc = esp_app_get_description();
    ESP_LOGW(TAG, "TR22 recovery firmware running: %s %s %s",
             desc->project_name, desc->date, desc->time);

    if (running != NULL) {
        ESP_LOGI(TAG, "running partition: %s subtype=0x%02x offset=0x%08" PRIx32,
                 running->label, running->subtype, running->address);
    }
    if (app_part != NULL) {
        ESP_LOGI(TAG, "app partition: %s subtype=0x%02x offset=0x%08" PRIx32,
                 app_part->label, app_part->subtype, app_part->address);
    }
}

void app_main(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *app_part = recovery_app_partition();
    bool wifi_attempted = false;
    bool wifi_ok = false;

    /* Install the vprintf hook early so SD/WiFi/OTA logs all get shipped. */
    log_ship_init();

    log_partition_state(running, app_part);

    /*
     * Recovery itself is the known-good image in the current OTA layout.
     * Mark it valid early so rollback can safely return here after a bad app.
     */
    esp_err_t valid_err = esp_ota_mark_app_valid_cancel_rollback();
    if (valid_err != ESP_OK && valid_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "mark recovery valid failed: %s", esp_err_to_name(valid_err));
    }

    ota_update_set_progress_cb(ota_progress_to_display);
    tr19_epaper_show_recovery("RECOVERY", "SD CHECK");

    if (app_part == NULL) {
        ESP_LOGE(TAG, "no ota_1 app partition; staying in recovery");
        tr19_epaper_show_recovery("RECOVERY", "NO APP SLOT");
    }

    while (true) {
        if (app_part != NULL) {
            tr19_epaper_show_recovery("RECOVERY", "CHECKING SD");
            esp_err_t sd_err = sd_recovery_install_app(app_part);
            if (sd_err == ESP_ERR_NOT_FOUND) {
                ESP_LOGI(TAG, "no SD update found");
                tr19_epaper_show_recovery("RECOVERY", "NO SD APP");
            } else if (sd_err != ESP_OK) {
                ESP_LOGW(TAG, "SD recovery failed: %s", esp_err_to_name(sd_err));
                tr19_epaper_show_recovery("RECOVERY", "SD FAILED");
            }
        }

        if (!wifi_attempted) {
            wifi_attempted = true;
            tr19_epaper_show_recovery("RECOVERY", "WIFI CHECK");
            wifi_ok = (wifi_connect_sta() == ESP_OK);
            if (wifi_ok) {
                log_ship_wifi_ready();
            } else {
                ESP_LOGW(TAG, "WiFi unavailable in recovery; SD retry loop remains active");
                tr19_epaper_show_recovery("RECOVERY", "WIFI FAILED");
            }
        }

        if (wifi_ok && app_part != NULL) {
            /* Per-stage messages are driven by ota_progress_to_display. */
            esp_err_t ota_err = ota_update_check_and_apply_to_partition(app_part);
            if (ota_err != ESP_OK) {
                ESP_LOGW(TAG, "WiFi OTA check/apply failed: %s", esp_err_to_name(ota_err));
            }
        }

        vTaskDelay(pdMS_TO_TICKS(RECOVERY_RETRY_DELAY_MS));
    }
}
