#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/**
 * Initialize NVS, TCP/IP stack, default event loop, and WiFi in STA mode.
 * Blocks until an IP address is obtained or OTA_WIFI_TIMEOUT_MS elapses.
 * Returns ESP_OK on successful connection, ESP_FAIL otherwise.
 *
 * Kept for compatibility with recovery_main.c. New code should prefer
 * wifi_ensure_up() + wifi_release() so WiFi only consumes current while
 * a task actively needs it.
 */
esp_err_t wifi_connect_sta(void);

/**
 * Idempotent: bring WiFi up (or report it's already up) within timeout_ms.
 * Pair every successful call with wifi_release() once the task that needed
 * the radio is done. NVS / netif / event-loop init is performed once and
 * never torn down; only the WiFi driver itself is started/stopped, which
 * is what dominates the current draw.
 */
esp_err_t wifi_ensure_up(uint32_t timeout_ms);

/**
 * Non-blocking: start the WiFi driver and kick off association without waiting
 * for an IP. Lets the caller run local feedback (LED/haptic) concurrently with
 * the radio coming up. MUST be paired with wifi_wait_up() (which performs the
 * blocking wait and tears down on failure). No-op if WiFi is already running.
 */
esp_err_t wifi_begin(void);

/**
 * Block until the association started by wifi_begin() obtains an IP, or until
 * timeout_ms elapses. Returns ESP_OK on success; on failure (or timeout) it
 * tears down what wifi_begin() started and returns ESP_FAIL.
 */
esp_err_t wifi_wait_up(uint32_t timeout_ms);

/**
 * Stop the WiFi driver and free its resources. Safe to call when WiFi
 * isn't currently running (no-op).
 */
void wifi_release(void);

/**
 * Copy the last station IP address into buf.  If WiFi has not obtained an IP
 * yet, returns "NO WIFI".
 */
void wifi_get_ip(char *buf, size_t len);

/**
 * Deprecated alias for wifi_release(). Kept so old callers compile.
 */
void wifi_disconnect(void);
