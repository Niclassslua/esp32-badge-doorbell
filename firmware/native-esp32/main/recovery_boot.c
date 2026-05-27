#include "recovery_boot.h"

#include <inttypes.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "recovery_config.h"

static const char *TAG = "tr22.recovery.boot";

static const esp_partition_t *find_app_partition(esp_partition_subtype_t subtype)
{
    return esp_partition_find_first(ESP_PARTITION_TYPE_APP, subtype, NULL);
}

const esp_partition_t *recovery_partition(void)
{
    return find_app_partition(RECOVERY_SLOT_SUBTYPE);
}

const esp_partition_t *recovery_app_partition(void)
{
    return find_app_partition(RECOVERY_APP_SLOT_SUBTYPE);
}

bool recovery_boot_button_held(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << RECOVERY_BOOT_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "BOOT GPIO config failed: %s", esp_err_to_name(err));
        return false;
    }

    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(RECOVERY_BOOT_HOLD_MS);
    while (xTaskGetTickCount() < deadline) {
        if (gpio_get_level(RECOVERY_BOOT_GPIO) != 0) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(25));
    }

    ESP_LOGW(TAG, "BOOT held for %d ms; recovery requested", RECOVERY_BOOT_HOLD_MS);
    return true;
}

esp_err_t recovery_boot_app_partition(const esp_partition_t *app_part)
{
    if (app_part == NULL) {
        ESP_LOGE(TAG, "no app partition available");
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "booting app partition %s at 0x%08" PRIx32,
             app_part->label, app_part->address);
    return esp_ota_set_boot_partition(app_part);
}

esp_err_t recovery_reboot_to_recovery(void)
{
    const esp_partition_t *part = recovery_partition();
    if (part == NULL) {
        ESP_LOGE(TAG, "no recovery partition available");
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGW(TAG, "switching next boot to recovery partition %s", part->label);
    return esp_ota_set_boot_partition(part);
}
