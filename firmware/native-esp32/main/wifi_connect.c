#include "wifi_connect.h"
#include "ota_config.h"

#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "nvs_flash.h"

static const char *TAG = "tr22.wifi";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static EventGroupHandle_t             s_wifi_eg;
static esp_event_handler_instance_t  s_ev_any;
static esp_event_handler_instance_t  s_ev_ip;
static char                          s_ip_addr[16] = "NO WIFI";

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    static int retries = 0;

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        strlcpy(s_ip_addr, "NO WIFI", sizeof(s_ip_addr));
        if (retries < OTA_WIFI_RETRY_MAX) {
            retries++;
            ESP_LOGW(TAG, "retry %d/%d", retries, OTA_WIFI_RETRY_MAX);
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(s_wifi_eg, WIFI_FAIL_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        snprintf(s_ip_addr, sizeof(s_ip_addr), IPSTR, IP2STR(&ev->ip_info.ip));
        ESP_LOGI(TAG, "got IP: %s", s_ip_addr);
        retries = 0;
        xEventGroupSetBits(s_wifi_eg, WIFI_CONNECTED_BIT);
    }
}

void wifi_get_ip(char *buf, size_t len)
{
    if (!buf || len == 0) {
        return;
    }
    strlcpy(buf, s_ip_addr, len);
}

esp_err_t wifi_connect_sta(void)
{
    /* NVS is required by the WiFi driver for calibration data. */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS needs erase, re-initialising");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    s_wifi_eg = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, &s_ev_any));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, &s_ev_ip));

    wifi_config_t wifi_cfg = {
        .sta = {
            .ssid     = OTA_WIFI_SSID,
            .password = OTA_WIFI_PASSWORD,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MAX_MODEM));

    ESP_LOGI(TAG, "connecting to SSID: %s", OTA_WIFI_SSID);

    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_eg,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE, pdFALSE,
        pdMS_TO_TICKS(OTA_WIFI_TIMEOUT_MS));

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "WiFi connected");
        return ESP_OK;
    }

    ESP_LOGE(TAG, "WiFi connection failed");
    return ESP_FAIL;
}

void wifi_disconnect(void)
{
    strlcpy(s_ip_addr, "NO WIFI", sizeof(s_ip_addr));
    esp_wifi_disconnect();
    esp_wifi_stop();
    esp_wifi_deinit();
    esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, s_ev_ip);
    esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_ev_any);
    vEventGroupDelete(s_wifi_eg);
    ESP_LOGI(TAG, "WiFi disconnected");
}
