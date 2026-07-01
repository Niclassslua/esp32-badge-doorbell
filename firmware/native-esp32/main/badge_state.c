#include "badge_state.h"

#include <stdbool.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "badge_config.h"
#include "badge_led.h"
#include "tr19_epaper.h"

static const char *TAG = "badge.state";

/* NVS namespace and key names */
#define NVS_NAMESPACE    "badge"
#define NVS_KEY_TEXT     "custom_text"

/* Display task stack: e-paper refresh has enough SPI and driver call frames
 * that a little extra headroom is useful. */
#define DISPLAY_TASK_STACK  4096
#define DISPLAY_TASK_PRIO   3

/* Periodic refresh task: just sleeps and pokes the display task. Stack tiny. */
#define REFRESH_TASK_STACK  2048
#define REFRESH_TASK_PRIO   1

static SemaphoreHandle_t s_mutex;
static SemaphoreHandle_t s_display_idle; /* given while display task is between renders */
static TaskHandle_t      s_display_task;
static sign_state_t      s_state       = SIGN_PLEASE_RING;
static char              s_custom_text[BADGE_CUSTOM_TEXT_MAX];
static bool              s_led_color_enabled;
static badge_led_color_t s_led_color;

/* -------------------------------------------------------------------------- */
/* Background display task                                                     */
/* -------------------------------------------------------------------------- */

/*
 * The display task owns all e-paper access.  HTTP handlers and button tasks
 * NEVER call tr19_epaper_* directly; they just notify this task.
 *
 * ulTaskNotifyTake(pdTRUE, …) clears the notification count to 0 on return,
 * so rapid bursts of state changes collapse into a single refresh cycle.
 */
static void display_task(void *arg)
{
    while (true) {
        /* Block until at least one state change has been signalled. */
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        /* Snapshot state under the mutex — keeps the critical section short. */
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        sign_state_t state = s_state;
        char txt[BADGE_CUSTOM_TEXT_MAX];
        memcpy(txt, s_custom_text, sizeof(txt));
        xSemaphoreGive(s_mutex);

        /* Update the display — this blocks for up to ~30 s (e-paper refresh).
         * No mutex is held during the refresh so other tasks stay responsive.
         * SIGN_NIGHT_MODE is rendered directly by badge_state_enter_nightmode()
         * before deep sleep; the display task skips it to avoid SPI contention. */
        if (state == SIGN_NIGHT_MODE) {
            continue;
        }

        /* Hold s_display_idle=0 while the SPI bus is in use so that
         * badge_state_enter_nightmode() can wait for us to finish. */
        xSemaphoreTake(s_display_idle, portMAX_DELAY);
        esp_err_t err = tr19_epaper_show_sign(state == SIGN_PLEASE_RING, txt);
        xSemaphoreGive(s_display_idle);

        if (err != ESP_OK) {
            ESP_LOGE(TAG, "display refresh failed: %s", esp_err_to_name(err));
        }
    }
}

/*
 * Periodic refresh task: every BADGE_DISPLAY_REFRESH_MS, poke the display
 * task so battery % and IP shown in the footer don't sit frozen between
 * user-driven state changes. The wakeup itself is essentially free — the
 * task is in vTaskDelay almost the entire time and the CPU stays in light
 * sleep until the tick fires.
 */
static void refresh_task(void *arg)
{
    const TickType_t period = pdMS_TO_TICKS(BADGE_DISPLAY_REFRESH_MS);
    while (true) {
        vTaskDelay(period);
        if (s_display_task) {
            xTaskNotifyGive(s_display_task);
        }
    }
}

/* -------------------------------------------------------------------------- */

static void save_to_nvs(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return;
    }

    nvs_set_str(handle, NVS_KEY_TEXT, s_custom_text);
    nvs_commit(handle);
    nvs_close(handle);
}

/* -------------------------------------------------------------------------- */
/* Public API                                                                  */
/* -------------------------------------------------------------------------- */

esp_err_t badge_state_init(void)
{
    /* NVS init — safe if already initialised by OTA code */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition truncated, erasing...");
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "nvs_flash_erase");
        err = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(err, TAG, "nvs_flash_init");

    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) {
        ESP_LOGE(TAG, "failed to create mutex");
        return ESP_ERR_NO_MEM;
    }

    s_display_idle = xSemaphoreCreateBinary();
    if (!s_display_idle) {
        ESP_LOGE(TAG, "failed to create display semaphore");
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreGive(s_display_idle); /* starts as "idle" */

    /* Load persisted custom text only.  The sign intentionally boots into
     * the default ring/off state every time. */
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        size_t text_len = sizeof(s_custom_text);
        nvs_get_str(handle, NVS_KEY_TEXT, s_custom_text, &text_len);

        nvs_close(handle);
        ESP_LOGI(TAG, "loaded: state=please_ring text=\"%s\" led_color=off",
                 s_custom_text);
    } else {
        ESP_LOGI(TAG, "no saved state, using defaults");
    }

    /* Create the display task and immediately trigger an initial render. */
    if (xTaskCreate(display_task, "display", DISPLAY_TASK_STACK,
                    NULL, DISPLAY_TASK_PRIO, &s_display_task) != pdPASS) {
        ESP_LOGE(TAG, "failed to create display task");
        return ESP_ERR_NO_MEM;
    }

    badge_led_set_custom_color(s_led_color_enabled, s_led_color);
    badge_led_set_sign_state(s_state == SIGN_PLEASE_RING);

    /* Trigger the first render right away — before WiFi connects — so the
     * badge shows the persisted state as early as possible.               */
    xTaskNotifyGive(s_display_task);

#if BADGE_DISPLAY_REFRESH_MS > 0
    if (xTaskCreate(refresh_task, "display_refresh", REFRESH_TASK_STACK,
                    NULL, REFRESH_TASK_PRIO, NULL) != pdPASS) {
        ESP_LOGW(TAG, "failed to create periodic display refresh task");
        /* Non-fatal: display still refreshes on state changes. */
    } else {
        ESP_LOGI(TAG, "periodic display refresh every %u ms",
                 (unsigned)BADGE_DISPLAY_REFRESH_MS);
    }
#endif

    return ESP_OK;
}

/* -------------------------------------------------------------------------- */

sign_state_t badge_state_get_state(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    sign_state_t s = s_state;
    xSemaphoreGive(s_mutex);
    return s;
}

void badge_state_set_state(sign_state_t state)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_state == state) {
        xSemaphoreGive(s_mutex);
        return; /* no-op */
    }
    s_state = state;
    save_to_nvs();
    xSemaphoreGive(s_mutex);

    badge_led_set_sign_state(state == SIGN_PLEASE_RING);

    static const char *const state_names[] = {
        [SIGN_PLEASE_RING]    = "please_ring",
        [SIGN_DO_NOT_DISTURB] = "do_not_disturb",
        [SIGN_NIGHT_MODE]     = "night_mode",
    };
    ESP_LOGI(TAG, "state -> %s", state_names[state]);
    xTaskNotifyGive(s_display_task); /* returns immediately; display task wakes later */
}

void badge_state_get_custom_text(char *buf, size_t len)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    strncpy(buf, s_custom_text, len - 1);
    buf[len - 1] = '\0';
    xSemaphoreGive(s_mutex);
}

void badge_state_set_custom_text(const char *text)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    strncpy(s_custom_text, text, sizeof(s_custom_text) - 1);
    s_custom_text[sizeof(s_custom_text) - 1] = '\0';
    save_to_nvs();
    xSemaphoreGive(s_mutex);

    ESP_LOGI(TAG, "custom_text → \"%s\"", text);
    xTaskNotifyGive(s_display_task);
}

bool badge_state_get_led_color(badge_led_color_t *color)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool enabled = s_led_color_enabled;
    if (color) {
        *color = s_led_color;
    }
    xSemaphoreGive(s_mutex);
    return enabled;
}

void badge_state_set_led_color(bool enabled, badge_led_color_t color)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool changed = s_led_color_enabled != enabled ||
                   s_led_color.red != color.red ||
                   s_led_color.green != color.green ||
                   s_led_color.blue != color.blue;
    if (!changed) {
        xSemaphoreGive(s_mutex);
        return;
    }
    s_led_color_enabled = enabled;
    s_led_color = color;
    save_to_nvs();
    sign_state_t state = s_state;
    xSemaphoreGive(s_mutex);

    badge_led_set_custom_color(enabled, color);
    badge_led_set_sign_state(state == SIGN_PLEASE_RING);

    ESP_LOGI(TAG, "led_color -> %s#%02x%02x%02x",
             enabled ? "" : "off/",
             color.red, color.green, color.blue);
}

void badge_state_refresh_display(void)
{
    if (!s_display_task) {
        return;
    }
    xTaskNotifyGive(s_display_task);
}

esp_err_t badge_state_enter_nightmode(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_state = SIGN_NIGHT_MODE;
    xSemaphoreGive(s_mutex);

    /* Kill LEDs entirely — no current drain during deep sleep. */
    badge_led_off();

    /* Wait for any in-progress display refresh to finish before touching
     * the SPI bus. Once we hold s_display_idle the display task will see
     * SIGN_NIGHT_MODE and skip future renders, so the bus is ours. */
    if (s_display_idle) {
        xSemaphoreTake(s_display_idle, portMAX_DELAY);
    }

    esp_err_t err = tr19_epaper_show_nightmode();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "night mode display failed: %s", esp_err_to_name(err));
    }
    return err;
}
