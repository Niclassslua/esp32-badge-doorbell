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

static bool                          s_one_time_init_done = false;
static bool                          s_wifi_running = false;
static esp_netif_t                  *s_sta_netif = NULL;
static EventGroupHandle_t            s_wifi_eg;
static esp_event_handler_instance_t  s_ev_any;
static esp_event_handler_instance_t  s_ev_ip;
static char                          s_ip_addr[16] = "NO WIFI";
static int                           s_retry_num = 0;

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        /* Keep s_ip_addr as the last-known IP. The display reads it for the
         * footer line; clearing it to "NO WIFI" on every release would make
         * the address disappear seconds after every doorbell ring. */
        if (s_retry_num < OTA_WIFI_RETRY_MAX) {
            s_retry_num++;
            ESP_LOGW(TAG, "retry %d/%d", s_retry_num, OTA_WIFI_RETRY_MAX);
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(s_wifi_eg, WIFI_FAIL_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        snprintf(s_ip_addr, sizeof(s_ip_addr), IPSTR, IP2STR(&ev->ip_info.ip));
        ESP_LOGI(TAG, "got IP: %s", s_ip_addr);
        s_retry_num = 0;
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

/*
 * One-time init of NVS + netif + event loop. Safe to call multiple times.
 * These resources are intentionally never torn down — only the WiFi driver
 * itself is started/stopped per on-demand session, which is what costs
 * current.
 */
static esp_err_t wifi_one_time_init(void)
{
    if (s_one_time_init_done) {
        return ESP_OK;
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS needs erase, re-initialising");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta_netif = esp_netif_create_default_wifi_sta();

#if OTA_WIFI_USE_STATIC_IP
    /* Stop the DHCP client and pin a static address so association is followed
     * by an immediate IP_EVENT_STA_GOT_IP instead of a ~1.5 s DHCP exchange.
     * The netif is created once and never destroyed, so configuring it here
     * survives the per-session esp_wifi_init()/deinit() cycles. */
    esp_err_t dhcp_err = esp_netif_dhcpc_stop(s_sta_netif);
    if (dhcp_err != ESP_OK && dhcp_err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        ESP_ERROR_CHECK(dhcp_err);
    }

    esp_netif_ip_info_t ip_info = { 0 };
    ip_info.ip.addr      = esp_ip4addr_aton(OTA_WIFI_STATIC_IP);
    ip_info.gw.addr      = esp_ip4addr_aton(OTA_WIFI_GATEWAY);
    ip_info.netmask.addr = esp_ip4addr_aton(OTA_WIFI_NETMASK);
    ESP_ERROR_CHECK(esp_netif_set_ip_info(s_sta_netif, &ip_info));

    esp_netif_dns_info_t dns_info = { 0 };
    dns_info.ip.type = ESP_IPADDR_TYPE_V4;
    dns_info.ip.u_addr.ip4.addr = esp_ip4addr_aton(OTA_WIFI_DNS);
    ESP_ERROR_CHECK(esp_netif_set_dns_info(s_sta_netif, ESP_NETIF_DNS_MAIN, &dns_info));

    ESP_LOGI(TAG, "static IP configured: %s gw %s dns %s",
             OTA_WIFI_STATIC_IP, OTA_WIFI_GATEWAY, OTA_WIFI_DNS);
#endif

    s_one_time_init_done = true;
    return ESP_OK;
}

esp_err_t wifi_begin(void)
{
    if (s_wifi_running) {
        return ESP_OK;
    }

    ESP_ERROR_CHECK(wifi_one_time_init());

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
    /* Reset the retry counter so each session starts clean regardless of how
     * the previous one ended (a leftover count would trip WIFI_FAIL_BIT early). */
    s_retry_num = 0;
    ESP_ERROR_CHECK(esp_wifi_start());
    /* Leave WiFi PS at the ESP-IDF default (MIN_MODEM with CONFIG_PM_ENABLE).
     * MAX_MODEM caused the Fritz.Box to drop the connection before DHCP
     * completed — the AP timed out the sleeping client at li:3 (300 ms). */

    /* esp_wifi_start() is non-blocking: the STA_START handler fires
     * esp_wifi_connect() and association/IP proceed asynchronously. The caller
     * does local feedback (LED/haptic) concurrently, then calls wifi_wait_up(). */
    ESP_LOGI(TAG, "connecting to SSID: %s", OTA_WIFI_SSID);
    return ESP_OK;
}

esp_err_t wifi_wait_up(uint32_t timeout_ms)
{
    if (s_wifi_running) {
        return ESP_OK;
    }
    if (!s_wifi_eg) {
        /* wifi_begin() was never called (or already torn down). */
        return ESP_FAIL;
    }

    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_eg,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE, pdFALSE,
        pdMS_TO_TICKS(timeout_ms));

    if (bits & WIFI_CONNECTED_BIT) {
        s_wifi_running = true;
        ESP_LOGI(TAG, "WiFi connected");
        return ESP_OK;
    }

    ESP_LOGE(TAG, "WiFi connection failed");
    /* Tear down what wifi_begin() started so the next attempt starts clean.
     * Unregister handlers first (see wifi_release() for why). */
    esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, s_ev_ip);
    esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_ev_any);
    esp_wifi_stop();
    esp_wifi_deinit();
    vEventGroupDelete(s_wifi_eg);
    s_wifi_eg = NULL;
    return ESP_FAIL;
}

static esp_err_t wifi_bring_up(uint32_t timeout_ms)
{
    esp_err_t err = wifi_begin();
    if (err != ESP_OK) {
        return err;
    }
    return wifi_wait_up(timeout_ms);
}

esp_err_t wifi_connect_sta(void)
{
    return wifi_bring_up(OTA_WIFI_TIMEOUT_MS);
}

esp_err_t wifi_ensure_up(uint32_t timeout_ms)
{
    return wifi_bring_up(timeout_ms);
}

void wifi_release(void)
{
    if (!s_wifi_running) {
        return;
    }
    /* Keep s_ip_addr untouched so wifi_get_ip() returns the last-known
     * address (the display shows it as the footer line). */
    /* Unregister the handlers FIRST: esp_wifi_disconnect() posts a
     * STA_DISCONNECTED event, and a still-live handler would fire a stray
     * esp_wifi_connect() mid-teardown (the "retry 1/5" after release) that
     * wedges the next session's DHCP. It would also race the s_wifi_eg delete
     * below. */
    esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, s_ev_ip);
    esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_ev_any);
    esp_wifi_disconnect();
    esp_wifi_stop();
    esp_wifi_deinit();
    vEventGroupDelete(s_wifi_eg);
    s_wifi_eg = NULL;
    s_wifi_running = false;
    ESP_LOGI(TAG, "WiFi released");
}

void wifi_disconnect(void)
{
    wifi_release();
}
