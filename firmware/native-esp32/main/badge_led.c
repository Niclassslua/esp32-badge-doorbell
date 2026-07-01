#include "badge_led.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/rmt_common.h"
#include "driver/rmt_encoder.h"
#include "driver/rmt_tx.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "badge_config.h"

static const char *TAG = "badge.led";

#define LED_RMT_RESOLUTION_HZ 10000000u

static rmt_channel_handle_t s_channel;
static rmt_encoder_handle_t s_encoder;
static SemaphoreHandle_t s_mutex;
static bool s_ready;
static bool s_channel_enabled;
static bool s_custom_color_enabled;
static badge_led_color_t s_custom_color;
static bool s_please_ring = true;

static const rmt_symbol_word_t ws2812_zero = {
    .level0 = 1,
    .duration0 = 0.3 * LED_RMT_RESOLUTION_HZ / 1000000,
    .level1 = 0,
    .duration1 = 0.9 * LED_RMT_RESOLUTION_HZ / 1000000,
};

static const rmt_symbol_word_t ws2812_one = {
    .level0 = 1,
    .duration0 = 0.9 * LED_RMT_RESOLUTION_HZ / 1000000,
    .level1 = 0,
    .duration1 = 0.3 * LED_RMT_RESOLUTION_HZ / 1000000,
};

static const rmt_symbol_word_t ws2812_reset = {
    .level0 = 0,
    .duration0 = LED_RMT_RESOLUTION_HZ / 1000000 * 50 / 2,
    .level1 = 0,
    .duration1 = LED_RMT_RESOLUTION_HZ / 1000000 * 50 / 2,
};

static size_t ws2812_encoder_callback(const void *data,
                                      size_t data_size,
                                      size_t symbols_written,
                                      size_t symbols_free,
                                      rmt_symbol_word_t *symbols,
                                      bool *done,
                                      void *arg)
{
    (void)arg;

    if (symbols_free < 8) {
        return 0;
    }

    size_t byte_pos = symbols_written / 8;
    const uint8_t *bytes = (const uint8_t *)data;
    if (byte_pos < data_size) {
        size_t symbol_pos = 0;
        for (uint8_t bit = 0x80; bit != 0; bit >>= 1) {
            symbols[symbol_pos++] = (bytes[byte_pos] & bit)
                                    ? ws2812_one
                                    : ws2812_zero;
        }
        return symbol_pos;
    }

    symbols[0] = ws2812_reset;
    *done = true;
    return 1;
}

esp_err_t badge_led_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    s_mutex = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_mutex != NULL, ESP_ERR_NO_MEM, TAG,
                        "create LED mutex");

    rmt_tx_channel_config_t tx_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .gpio_num = BADGE_LED_GPIO,
        .mem_block_symbols = 64,
        .resolution_hz = LED_RMT_RESOLUTION_HZ,
        .trans_queue_depth = 1,
        .flags = {
            .io_od_mode = 1,
        },
    };
    ESP_RETURN_ON_ERROR(rmt_new_tx_channel(&tx_config, &s_channel), TAG,
                        "create RMT channel");

    rmt_simple_encoder_config_t encoder_config = {
        .callback = ws2812_encoder_callback,
    };
    ESP_RETURN_ON_ERROR(rmt_new_simple_encoder(&encoder_config, &s_encoder),
                        TAG, "create RMT encoder");

    s_ready = true;
    ESP_LOGI(TAG, "LEDs initialised on GPIO%d count=%d",
             BADGE_LED_GPIO, BADGE_LED_COUNT);
    return ESP_OK;
}

static esp_err_t badge_led_enable_channel(void)
{
    if (s_channel_enabled) {
        return ESP_OK;
    }
    esp_err_t err = rmt_enable(s_channel);
    if (err == ESP_OK) {
        s_channel_enabled = true;
    }
    return err;
}

static void badge_led_disable_channel(void)
{
    if (s_channel_enabled) {
        esp_err_t err = rmt_disable(s_channel);
        if (err == ESP_OK) {
            s_channel_enabled = false;
        } else {
            ESP_LOGD(TAG, "RMT disable failed: %s", esp_err_to_name(err));
        }
    }
}

static void badge_led_write_color(uint8_t red, uint8_t green, uint8_t blue)
{
    if (!s_ready || !s_mutex) {
        return;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    esp_err_t err = badge_led_enable_channel();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "LED enable failed: %s", esp_err_to_name(err));
        xSemaphoreGive(s_mutex);
        return;
    }

    uint8_t pixels[BADGE_LED_COUNT * 3];
    for (int i = 0; i < BADGE_LED_COUNT; i++) {
        size_t offset = (size_t)i * 3;
        /* The TR22 LED chain maps byte order as logical RGB. */
        pixels[offset + 0] = red;
        pixels[offset + 1] = green;
        pixels[offset + 2] = blue;
    }

    rmt_transmit_config_t tx_config = {
        .loop_count = 0,
    };
    err = rmt_transmit(s_channel, s_encoder, pixels, sizeof(pixels), &tx_config);
    if (err == ESP_OK) {
        err = rmt_tx_wait_all_done(s_channel, 100);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "LED update failed: %s", esp_err_to_name(err));
    }

    badge_led_disable_channel();
    xSemaphoreGive(s_mutex);
}

void badge_led_set_sign_state(bool please_ring)
{
    s_please_ring = please_ring;
    if (s_custom_color_enabled) {
        badge_led_write_color(s_custom_color.red,
                              s_custom_color.green,
                              s_custom_color.blue);
    } else if (please_ring) {
        badge_led_write_color(0, 0, 0);
    } else {
        badge_led_write_color(BADGE_LED_DO_NOT_DISTURB_STEADY_BRIGHTNESS, 0, 0);
    }
}

void badge_led_set_custom_color(bool enabled, badge_led_color_t color)
{
    s_custom_color_enabled = enabled;
    s_custom_color = color;
    badge_led_set_sign_state(s_please_ring);
}

void badge_led_off(void)
{
    badge_led_write_color(0, 0, 0);
}

void badge_led_pulse_button_feedback(bool please_ring)
{
    if (please_ring) {
        badge_led_write_color(BADGE_LED_FEEDBACK_BRIGHTNESS,
                              BADGE_LED_FEEDBACK_BRIGHTNESS,
                              BADGE_LED_FEEDBACK_BRIGHTNESS);
    } else {
        badge_led_write_color(BADGE_LED_FEEDBACK_BRIGHTNESS, 0, 0);
    }
    vTaskDelay(pdMS_TO_TICKS(BADGE_LED_FEEDBACK_MS));
    badge_led_set_sign_state(please_ring);
}
