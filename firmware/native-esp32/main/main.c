#include <inttypes.h>
#include <stdio.h>

#include "driver/gpio.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "tr19_epaper.h"

static const char *TAG = "tr22";

/*
 * First-pass LED probe.
 *
 * GPIO2 is common on ESP32 dev boards. The TR22 badge may not have an LED here;
 * keeping this isolated makes it easy to replace once we confirm the badge LED
 * or backlight pins.
 */
#define PROBE_LED_GPIO GPIO_NUM_2

static void log_chip_info(void)
{
    esp_chip_info_t chip_info;
    uint32_t flash_size = 0;

    esp_chip_info(&chip_info);
    esp_flash_get_size(NULL, &flash_size);

    ESP_LOGI(TAG, "Hello from custom TR22 firmware");
    ESP_LOGI(TAG, "Cores: %d, revision: %d", chip_info.cores, chip_info.revision);
    ESP_LOGI(TAG, "Features: WiFi%s%s",
             (chip_info.features & CHIP_FEATURE_BT) ? ", BT" : "",
             (chip_info.features & CHIP_FEATURE_BLE) ? ", BLE" : "");
    ESP_LOGI(TAG, "Flash: %" PRIu32 " bytes", flash_size);
}

static void init_probe_led(void)
{
    gpio_config_t config = {
        .pin_bit_mask = 1ULL << PROBE_LED_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_ERROR_CHECK(gpio_config(&config));
}

void app_main(void)
{
    log_chip_info();
    init_probe_led();

    esp_err_t display_err = tr19_epaper_show_hello();
    if (display_err != ESP_OK) {
        ESP_LOGE(TAG, "ePaper hello failed: %s", esp_err_to_name(display_err));
    }

    bool on = false;
    uint32_t tick = 0;

    while (true) {
        on = !on;
        gpio_set_level(PROBE_LED_GPIO, on);
        ESP_LOGI(TAG, "tick=%" PRIu32 " probe_gpio=%d level=%d", tick++, PROBE_LED_GPIO, on);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
