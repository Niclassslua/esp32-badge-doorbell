#pragma once

/**
 * log_ship — Automatic log shipping for TR22 badge firmware.
 *
 * Hooks into ESP-IDF's logging system via esp_log_set_vprintf() to capture
 * every log line into an 8 KB ring buffer.  Once WiFi is up the buffer is
 * flushed to a remote HTTP server and a background task keeps shipping new
 * batches every 5 seconds, including heap stats, device info, and reset
 * reason.
 *
 * Usage in app_main():
 *
 *   log_ship_init();                   // after the OTA gate
 *   ...
 *   if (wifi_ok) {
 *       log_ship_wifi_ready();         // flush buffer + start background task
 *   }
 *
 * Companion server:
 *   python3 tools/log-server.py
 */

/**
 * Install the vprintf hook and populate the device-info cache.
 *
 * Safe to call before the FreeRTOS scheduler starts.  In the badge firmware,
 * the OTA gate intentionally runs before log capture starts.
 */
void log_ship_init(void);

/**
 * Signal that WiFi has an IP address.
 *
 * Starts a FreeRTOS task that immediately flushes the pre-WiFi ring-buffer
 * contents and then ships a new batch every LOG_SHIP_TASK_PERIOD_MS ms.
 * Idempotent — safe to call more than once.
 */
void log_ship_wifi_ready(void);

/**
 * Tear down the shipping task and restore the original vprintf.
 * Intended for testing; not needed in normal firmware flow.
 */
void log_ship_deinit(void);
