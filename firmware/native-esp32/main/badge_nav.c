#include "badge_nav.h"

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "badge_config.h"
#include "badge_i2c.h"

static const char *TAG = "badge.nav";

#define PCA9555_REG_INPUT_PORT0  0x00
#define NAV_I2C_TIMEOUT_MS       20
#define NAV_TASK_STACK           3072
#define NAV_TASK_PRIO            4

static i2c_master_dev_handle_t   s_nav_dev;
static badge_nav_start_hold_cb_t s_on_start_hold;

static esp_err_t nav_read_port0(uint8_t *out)
{
    uint8_t reg = PCA9555_REG_INPUT_PORT0;
    return i2c_master_transmit_receive(s_nav_dev, &reg, 1, out, 1,
                                       NAV_I2C_TIMEOUT_MS);
}

static esp_err_t nav_attach(uint8_t address)
{
    i2c_master_bus_handle_t bus = badge_i2c_bus();
    if (!bus) {
        return ESP_ERR_INVALID_STATE;
    }
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = address,
        .scl_speed_hz    = 400000,
    };
    i2c_master_dev_handle_t dev = NULL;
    esp_err_t err = i2c_master_bus_add_device(bus, &dev_cfg, &dev);
    if (err != ESP_OK) {
        return err;
    }
    /* Probe by reading port 0. NACK here means the address is unused. */
    uint8_t reg = PCA9555_REG_INPUT_PORT0;
    uint8_t value = 0;
    err = i2c_master_transmit_receive(dev, &reg, 1, &value, 1,
                                      NAV_I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        i2c_master_bus_rm_device(dev);
        return err;
    }
    s_nav_dev = dev;
    ESP_LOGI(TAG, "PCA9555 nav expander at 0x%02x (port0=0x%02x)", address, value);
    return ESP_OK;
}

static void task_nav(void *arg)
{
    (void)arg;
    const TickType_t poll_ticks = pdMS_TO_TICKS(BADGE_NAV_POLL_MS);
    int64_t start_press_us = -1;
    bool fired = false;

    while (true) {
        vTaskDelay(poll_ticks);

        uint8_t port0 = 0xff;
        if (nav_read_port0(&port0) != ESP_OK) {
            /* I2C glitch — re-arm and try again next tick. */
            start_press_us = -1;
            fired = false;
            continue;
        }

        bool start_down = ((port0 & BADGE_NAV_START_MASK) == 0); /* active-low */
        int64_t now_us = esp_timer_get_time();

        if (!start_down) {
            start_press_us = -1;
            fired = false;
            continue;
        }

        if (start_press_us < 0) {
            start_press_us = now_us;
            continue;
        }

        if (fired) {
            continue;
        }

        int64_t held_ms = (now_us - start_press_us) / 1000;
        if (held_ms >= BADGE_NAV_OTA_HOLD_MS) {
            ESP_LOGI(TAG, "START held %lld ms — firing OTA callback",
                     (long long)held_ms);
            fired = true;
            badge_nav_start_hold_cb_t cb = s_on_start_hold;
            if (cb) {
                cb();
            }
        }
    }
}

esp_err_t badge_nav_init(badge_nav_start_hold_cb_t on_start_hold)
{
    s_on_start_hold = on_start_hold;

    esp_err_t err = nav_attach(BADGE_NAV_I2C_ADDR_PRIMARY);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nav probe 0x%02x failed (%s); trying fallback 0x%02x",
                 BADGE_NAV_I2C_ADDR_PRIMARY, esp_err_to_name(err),
                 BADGE_NAV_I2C_ADDR_FALLBACK);
        err = nav_attach(BADGE_NAV_I2C_ADDR_FALLBACK);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "nav expander not found on 0x%02x either (%s)",
                     BADGE_NAV_I2C_ADDR_FALLBACK, esp_err_to_name(err));
            return err;
        }
    }

    if (xTaskCreate(task_nav, "badge_nav", NAV_TASK_STACK, NULL,
                    NAV_TASK_PRIO, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to create nav task");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
