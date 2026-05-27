#pragma once

#include <stddef.h>

#include "esp_err.h"

/**
 * Initialize NVS, TCP/IP stack, default event loop, and WiFi in STA mode.
 * Blocks until an IP address is obtained or OTA_WIFI_TIMEOUT_MS elapses.
 *
 * Returns ESP_OK on successful connection, ESP_FAIL otherwise.
 * On failure the caller should skip OTA and proceed with normal boot.
 */
esp_err_t wifi_connect_sta(void);

/**
 * Copy the last station IP address into buf.  If WiFi has not obtained an IP
 * yet, returns "NO WIFI".
 */
void wifi_get_ip(char *buf, size_t len);

/**
 * Disconnect from WiFi and free driver resources.
 * Normal firmware flow keeps WiFi connected after the OTA gate; this is for
 * explicit teardown paths.
 */
void wifi_disconnect(void);
