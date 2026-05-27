#include "badge_server.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "badge_config.h"
#include "badge_state.h"
#include "recovery_boot.h"

static const char *TAG = "badge.server";

static httpd_handle_t s_server = NULL;

/* ------------------------------------------------------------------ helpers */

/*
 * Extract the value string for a JSON key from a flat, single-level JSON body.
 * Looks for  "key" : "<value>"  and copies <value> into out (NUL-terminated).
 *
 * Returns true on success, false if the key is not found or the buffer is too
 * small.  No full JSON parser — keeps the binary small.
 */
static bool json_get_str(const char *body, const char *key,
                          char *out, size_t out_len)
{
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);

    const char *start = strstr(body, pattern);
    if (!start) {
        return false;
    }
    start += strlen(pattern);

    while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n') {
        start++;
    }
    if (*start != ':') {
        return false;
    }
    start++;
    while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n') {
        start++;
    }
    if (*start != '"') {
        return false;
    }
    start++;

    const char *end = strchr(start, '"');
    if (!end) {
        return false;
    }

    size_t value_len = (size_t)(end - start);
    if (value_len >= out_len) {
        value_len = out_len - 1;
    }
    memcpy(out, start, value_len);
    out[value_len] = '\0';
    return true;
}

static const char *state_api_value(sign_state_t state)
{
    return state == SIGN_PLEASE_RING ? "please_ring" : "do_not_disturb";
}

static const char *state_label(sign_state_t state)
{
    return state == SIGN_PLEASE_RING
           ? BADGE_LABEL_PLEASE_RING
           : BADGE_LABEL_DO_NOT_DISTURB;
}

static bool parse_sign_state(const char *value, sign_state_t *state)
{
    if (strcmp(value, "please_ring") == 0) {
        *state = SIGN_PLEASE_RING;
        return true;
    }
    if (strcmp(value, "do_not_disturb") == 0) {
        *state = SIGN_DO_NOT_DISTURB;
        return true;
    }
    return false;
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static bool parse_led_color(const char *value, bool *enabled,
                            badge_led_color_t *color)
{
    if (strcmp(value, "off") == 0 || strcmp(value, "none") == 0) {
        *enabled = false;
        *color = (badge_led_color_t) {0};
        return true;
    }

    if (*value == '#') {
        value++;
    }
    if (strlen(value) != 6) {
        return false;
    }

    int digits[6];
    for (int i = 0; i < 6; i++) {
        digits[i] = hex_digit(value[i]);
        if (digits[i] < 0) {
            return false;
        }
    }

    *enabled = true;
    *color = (badge_led_color_t) {
        .red = (uint8_t)((digits[0] << 4) | digits[1]),
        .green = (uint8_t)((digits[2] << 4) | digits[3]),
        .blue = (uint8_t)((digits[4] << 4) | digits[5]),
    };
    return true;
}

/*
 * Read the full request body into buf (at most buf_len-1 bytes).
 * Returns the number of bytes read, or -1 on error.
 */
static int read_body(httpd_req_t *req, char *buf, size_t buf_len)
{
    size_t remaining = req->content_len;
    if (remaining == 0) {
        buf[0] = '\0';
        return 0;
    }
    if (remaining >= buf_len) {
        remaining = buf_len - 1;
    }

    int received = httpd_req_recv(req, buf, remaining);
    if (received <= 0) {
        return -1;
    }
    buf[received] = '\0';
    return received;
}

/* ----------------------------------------------------------------- handlers */

static esp_err_t handler_get_status(httpd_req_t *req)
{
    sign_state_t state = badge_state_get_state();
    char text[BADGE_CUSTOM_TEXT_MAX];
    badge_state_get_custom_text(text, sizeof(text));
    badge_led_color_t led_color = {0};
    bool led_enabled = badge_state_get_led_color(&led_color);

    char led_json[16];
    if (led_enabled) {
        snprintf(led_json, sizeof(led_json), "\"#%02x%02x%02x\"",
                 led_color.red, led_color.green, led_color.blue);
    } else {
        snprintf(led_json, sizeof(led_json), "null");
    }

    char resp[320];
    snprintf(resp, sizeof(resp),
             "{\"state\":\"%s\",\"label\":\"%s\",\"custom_text\":\"%s\",\"led_color\":%s}",
             state_api_value(state),
             state_label(state),
             text,
             led_json);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t handler_post_status(httpd_req_t *req)
{
    char body[256];
    if (read_body(req, body, sizeof(body)) < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "read error");
        return ESP_FAIL;
    }

    char value[32];
    if (!json_get_str(body, "state", value, sizeof(value))) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"missing state field\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    sign_state_t state;
    if (!parse_sign_state(value, &state)) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"bad request\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    badge_state_set_state(state);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t handler_post_display(httpd_req_t *req)
{
    char body[256];
    if (read_body(req, body, sizeof(body)) < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "read error");
        return ESP_FAIL;
    }

    bool changed = false;

    char value[32];
    if (json_get_str(body, "state", value, sizeof(value))) {
        sign_state_t state;
        if (!parse_sign_state(value, &state)) {
            httpd_resp_set_type(req, "application/json");
            httpd_resp_send(req, "{\"error\":\"bad request\"}", HTTPD_RESP_USE_STRLEN);
            return ESP_OK;
        }
        badge_state_set_state(state);
        changed = true;
    }

    char text[BADGE_CUSTOM_TEXT_MAX];
    if (json_get_str(body, "text", text, sizeof(text))) {
        badge_state_set_custom_text(text);
        changed = true;
    }

    char led_color_value[16];
    if (json_get_str(body, "led_color", led_color_value,
                     sizeof(led_color_value))) {
        bool enabled = false;
        badge_led_color_t color = {0};
        if (!parse_led_color(led_color_value, &enabled, &color)) {
            httpd_resp_set_type(req, "application/json");
            httpd_resp_send(req, "{\"error\":\"bad led_color\"}",
                            HTTPD_RESP_USE_STRLEN);
            return ESP_OK;
        }
        badge_state_set_led_color(enabled, color);
        changed = true;
    }

    if (!changed) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"error\":\"missing state, text, or led_color field\"}",
                        HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static void reboot_to_recovery_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(250));
    ESP_LOGW(TAG, "restarting into recovery after HTTP OTA trigger");
    esp_restart();
}

static esp_err_t handler_post_ota(httpd_req_t *req)
{
    esp_err_t err = recovery_reboot_to_recovery();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA trigger failed: %s", esp_err_to_name(err));
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"recovery unavailable\"}",
                        HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true,\"reboot\":\"recovery\"}",
                    HTTPD_RESP_USE_STRLEN);

    BaseType_t task_ok = xTaskCreate(reboot_to_recovery_task, "ota_reboot",
                                     2048, NULL, 5, NULL);
    if (task_ok != pdPASS) {
        ESP_LOGW(TAG, "could not create reboot task; restarting immediately");
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_restart();
    }
    return ESP_OK;
}

/* --------------------------------------------------------------- URI table  */

static const httpd_uri_t uri_get_status = {
    .uri     = "/status",
    .method  = HTTP_GET,
    .handler = handler_get_status,
};

static const httpd_uri_t uri_post_status = {
    .uri     = "/status",
    .method  = HTTP_POST,
    .handler = handler_post_status,
};

static const httpd_uri_t uri_post_display = {
    .uri     = "/display",
    .method  = HTTP_POST,
    .handler = handler_post_display,
};

static const httpd_uri_t uri_post_custom = {
    .uri     = "/custom",
    .method  = HTTP_POST,
    .handler = handler_post_display,
};

static const httpd_uri_t uri_post_ota = {
    .uri     = "/ota",
    .method  = HTTP_POST,
    .handler = handler_post_ota,
};

/* ---------------------------------------------------------------- lifecycle */

esp_err_t badge_server_start(void)
{
    if (s_server) {
        ESP_LOGW(TAG, "server already running");
        return ESP_OK;
    }

    httpd_config_t config  = HTTPD_DEFAULT_CONFIG();
    config.server_port     = BADGE_HTTP_SERVER_PORT;
    config.stack_size      = 8192; /* extra headroom for display calls */
    config.max_uri_handlers = 5;

    ESP_RETURN_ON_ERROR(httpd_start(&s_server, &config), TAG, "httpd_start");

    httpd_register_uri_handler(s_server, &uri_get_status);
    httpd_register_uri_handler(s_server, &uri_post_status);
    httpd_register_uri_handler(s_server, &uri_post_display);
    httpd_register_uri_handler(s_server, &uri_post_custom);
    httpd_register_uri_handler(s_server, &uri_post_ota);

    ESP_LOGI(TAG, "HTTP server started on port %d", BADGE_HTTP_SERVER_PORT);
    return ESP_OK;
}

void badge_server_stop(void)
{
    if (!s_server) {
        return;
    }
    httpd_stop(s_server);
    s_server = NULL;
    ESP_LOGI(TAG, "HTTP server stopped");
}
