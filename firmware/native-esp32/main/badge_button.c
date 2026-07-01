#include "badge_button.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "badge_config.h"
#include "badge_led.h"
#include "badge_power.h"
#include "badge_state.h"
#include "badge_touch.h"
#include "wifi_connect.h"

static const char *TAG = "badge.button";

#define DEBOUNCE_MS  200
#define TASK_STACK   4096
#define TASK_PRIO    5
#define RING_TASK_STACK 8192

static SemaphoreHandle_t s_sem_status;
static SemaphoreHandle_t s_ring_mutex;
static bool s_ring_task_active;
static int64_t s_next_ring_allowed_us;

static bool button_gpio_enabled(gpio_num_t gpio)
{
    return gpio >= 0 && gpio < GPIO_NUM_MAX;
}

static bool button_gpio_supports_internal_pullup(gpio_num_t gpio)
{
    return !(gpio >= GPIO_NUM_34 && gpio <= GPIO_NUM_39);
}

static uint64_t button_gpio_mask(gpio_num_t gpio)
{
    return 1ULL << (unsigned)gpio;
}

static void IRAM_ATTR btn_isr_handler(void *arg)
{
    SemaphoreHandle_t sem = (SemaphoreHandle_t)arg;
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(sem, &woken);
    portYIELD_FROM_ISR(woken);
}

/* ------------------------------------------------------ shared ring helper */

static esp_err_t post_json(const char *label, const char *url,
                           const char *auth_header, const char *body)
{
    if (!url || url[0] == '\0') {
        ESP_LOGD(TAG, "%s POST skipped: URL not configured", label);
        return ESP_OK;
    }

    esp_http_client_config_t cfg = {
        .url        = url,
        .method     = HTTP_METHOD_POST,
        .timeout_ms = BADGE_HOME_ASSISTANT_HTTP_TIMEOUT_MS,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "%s: esp_http_client_init failed", label);
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    if (auth_header && auth_header[0] != '\0') {
        esp_http_client_set_header(client, "Authorization", auth_header);
    }
    esp_http_client_set_post_field(client, body, (int)strlen(body));

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "%s POST -> HTTP %d",
                 label, esp_http_client_get_status_code(client));
    } else {
        ESP_LOGW(TAG, "%s POST failed: %s", label, esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
    return err;
}

static void flash_home_assistant_light(void)
{
    if (BADGE_HOME_ASSISTANT_AUTH_TOKEN[0] == '\0' ||
        BADGE_HOME_ASSISTANT_LIGHT_TURN_ON_URL[0] == '\0' ||
        BADGE_HOME_ASSISTANT_LIGHT_TURN_OFF_URL[0] == '\0' ||
        BADGE_HOME_ASSISTANT_ENTITY_ID[0] == '\0' ||
        BADGE_HOME_ASSISTANT_FLASH_COUNT <= 0) {
        ESP_LOGD(TAG, "Home Assistant light flash disabled");
        return;
    }

    char auth_header[sizeof("Bearer ") + sizeof(BADGE_HOME_ASSISTANT_AUTH_TOKEN)];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s",
             BADGE_HOME_ASSISTANT_AUTH_TOKEN);

    char turn_on_body[160];
    snprintf(turn_on_body, sizeof(turn_on_body),
             "{\"entity_id\":\"%s\",\"brightness_pct\":%d}",
             BADGE_HOME_ASSISTANT_ENTITY_ID,
             BADGE_HOME_ASSISTANT_BRIGHTNESS_PCT);

    char turn_off_body[128];
    snprintf(turn_off_body, sizeof(turn_off_body),
             "{\"entity_id\":\"%s\"}", BADGE_HOME_ASSISTANT_ENTITY_ID);

    for (int i = 0; i < BADGE_HOME_ASSISTANT_FLASH_COUNT; ++i) {
        post_json("home assistant light on",
                  BADGE_HOME_ASSISTANT_LIGHT_TURN_ON_URL,
                  auth_header, turn_on_body);
        vTaskDelay(pdMS_TO_TICKS(BADGE_HOME_ASSISTANT_FLASH_ON_MS));

        post_json("home assistant light off",
                  BADGE_HOME_ASSISTANT_LIGHT_TURN_OFF_URL,
                  auth_header, turn_off_body);
        vTaskDelay(pdMS_TO_TICKS(BADGE_HOME_ASSISTANT_FLASH_OFF_MS));
    }
}

static void finish_ring_task(void)
{
    if (s_ring_mutex) {
        xSemaphoreTake(s_ring_mutex, portMAX_DELAY);
        s_ring_task_active = false;
        xSemaphoreGive(s_ring_mutex);
    }
}

static bool reserve_ring_slot(const char *source)
{
    if (badge_state_get_state() != SIGN_PLEASE_RING) {
        ESP_LOGI(TAG, "ring (%s) ignored: do not disturb", source);
        return false;
    }

    if (!s_ring_mutex) {
        ESP_LOGW(TAG, "ring (%s) ignored: ring mutex not initialized", source);
        return false;
    }

    int64_t now_us = esp_timer_get_time();
    xSemaphoreTake(s_ring_mutex, portMAX_DELAY);

    if (s_ring_task_active) {
        xSemaphoreGive(s_ring_mutex);
        ESP_LOGI(TAG, "ring (%s) ignored: ring already active", source);
        return false;
    }

    if (now_us < s_next_ring_allowed_us) {
        int64_t remaining_ms = (s_next_ring_allowed_us - now_us + 999) / 1000;
        xSemaphoreGive(s_ring_mutex);
        ESP_LOGI(TAG, "ring (%s) ignored: cooldown %lld ms remaining",
                 source, (long long)remaining_ms);
        return false;
    }

    s_ring_task_active = true;
    s_next_ring_allowed_us = now_us + ((int64_t)BADGE_RING_COOLDOWN_MS * 1000);
    xSemaphoreGive(s_ring_mutex);

    return true;
}

static void release_ring_slot_after_create_failure(void)
{
    if (s_ring_mutex) {
        xSemaphoreTake(s_ring_mutex, portMAX_DELAY);
        s_ring_task_active = false;
        s_next_ring_allowed_us = 0;
        xSemaphoreGive(s_ring_mutex);
    }
}

/*
 * Fire one doorbell ring: gate on door-sign state, give the user LED + haptic
 * feedback, then optionally flash a Home Assistant light.
 * Runs in its own task so the IQS550 polling callback is never blocked by
 * network calls or the multi-second light sequence.
 */
static void task_ring(void *arg)
{
    const char *source = (const char *)arg;

    ESP_LOGI(TAG, "ring (%s): accepted", source);

    /* Kick the radio off first (non-blocking) so association + IP acquisition
     * overlap the ~870 ms of LED + haptic feedback below, instead of starting
     * only after it. Bring WiFi up only for the lamp flash, then release it —
     * keeps the radio off ~24/7. */
    wifi_begin();
    badge_led_pulse_button_feedback(true);
    badge_power_haptic_pulse();

    if (wifi_wait_up(8000) == ESP_OK) {
        flash_home_assistant_light();
        wifi_release();
    } else {
        ESP_LOGW(TAG, "ring (%s): WiFi unavailable, skipped lamp flash", source);
    }

    finish_ring_task();
    vTaskDelete(NULL);
}

static void ring_doorbell_once(const char *source)
{
    if (!reserve_ring_slot(source)) {
        return;
    }

    BaseType_t ok = xTaskCreate(task_ring, "btn_ring", RING_TASK_STACK,
                                (void *)source, TASK_PRIO, NULL);
    if (ok != pdPASS) {
        release_ring_slot_after_create_failure();
        ESP_LOGE(TAG, "ring (%s) ignored: failed to start ring task", source);
    }
}

static void touch_press_cb(void)
{
    ring_doorbell_once("touch");
}

/* ------------------------------------------------------ status button task  */

static void task_status(void *arg)
{
    (void)arg;

    while (true) {
        xSemaphoreTake(s_sem_status, portMAX_DELAY);

        vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_MS));
        if (gpio_get_level(BADGE_BTN_STATUS_GPIO) != 0) {
            continue;
        }

        sign_state_t current = badge_state_get_state();
        sign_state_t next    = (current == SIGN_PLEASE_RING)
                                ? SIGN_DO_NOT_DISTURB
                                : SIGN_PLEASE_RING;

        ESP_LOGI(TAG, "status button: %s -> %s",
                 current == SIGN_PLEASE_RING ? "please_ring" : "do_not_disturb",
                 next    == SIGN_PLEASE_RING ? "please_ring" : "do_not_disturb");

        badge_state_set_state(next);
    }
}

/* --------------------------------------------------------------- public API */

esp_err_t badge_button_init(void)
{
    s_ring_mutex = xSemaphoreCreateMutex();
    if (!s_ring_mutex) {
        ESP_LOGE(TAG, "failed to create ring mutex");
        return ESP_ERR_NO_MEM;
    }

    gpio_num_t status_gpio = BADGE_BTN_STATUS_GPIO;
    bool status_enabled = button_gpio_enabled(status_gpio);

    if (status_enabled) {
        esp_err_t err = gpio_install_isr_service(0);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "gpio_install_isr_service: %s", esp_err_to_name(err));
            return err;
        }

        s_sem_status = xSemaphoreCreateBinary();
        if (!s_sem_status) {
            ESP_LOGE(TAG, "failed to create status semaphore");
            return ESP_ERR_NO_MEM;
        }

        gpio_config_t cfg = {
            .pin_bit_mask = button_gpio_mask(status_gpio),
            .mode         = GPIO_MODE_INPUT,
            .pull_up_en   = button_gpio_supports_internal_pullup(status_gpio)
                            ? GPIO_PULLUP_ENABLE
                            : GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_NEGEDGE,
        };
        ESP_RETURN_ON_ERROR(gpio_config(&cfg), TAG, "gpio_config status");
        ESP_RETURN_ON_ERROR(
            gpio_isr_handler_add(status_gpio, btn_isr_handler,
                                 (void *)s_sem_status),
            TAG, "isr add status");
        xTaskCreate(task_status, "btn_status", TASK_STACK, NULL, TASK_PRIO, NULL);
        ESP_LOGI(TAG, "status button on GPIO%d", status_gpio);
    }

    /* Touch-keyboard doorbell: any finger-down on the IQS550 rings the bell.
     * Non-fatal — if the touch controller is absent, status flips can still
     * be driven via HTTP. */
    badge_touch_set_press_cb(touch_press_cb);
    esp_err_t touch_err = badge_touch_init();
    if (touch_err != ESP_OK) {
        ESP_LOGW(TAG, "touch keyboard doorbell disabled: %s",
                 esp_err_to_name(touch_err));
    }

    /* One-shot self-test buzz so we know the DRV2605 path is alive at boot. */
    ESP_LOGI(TAG, "haptic self-test pulse");
    badge_power_haptic_pulse();

    return ESP_OK;
}
