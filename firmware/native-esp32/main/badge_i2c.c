#include "badge_i2c.h"

#include "esp_log.h"

#include "badge_config.h"

static const char *TAG = "badge.i2c";

static i2c_master_bus_handle_t s_bus;

esp_err_t badge_i2c_init(void)
{
    if (s_bus) {
        return ESP_OK;
    }

    i2c_master_bus_config_t cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = BADGE_I2C_SDA_GPIO,
        .scl_io_num = BADGE_I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags = { .enable_internal_pullup = 1 },
    };

    esp_err_t err = i2c_new_master_bus(&cfg, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus: %s", esp_err_to_name(err));
        s_bus = NULL;
        return err;
    }

    ESP_LOGI(TAG, "shared I2C bus ready on SDA=GPIO%d SCL=GPIO%d",
             BADGE_I2C_SDA_GPIO, BADGE_I2C_SCL_GPIO);
    return ESP_OK;
}

i2c_master_bus_handle_t badge_i2c_bus(void)
{
    return s_bus;
}
