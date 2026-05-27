#pragma once

#include "esp_err.h"

/**
 * Start the on-badge HTTP server.
 *
 * Registers the following endpoints:
 *
 *   GET  /status
 *     Response: {"state":"please_ring|do_not_disturb","label":"...","custom_text":"...","led_color":"#rrggbb"|null}
 *
 *   POST /status
 *     Body:     {"state":"please_ring"} or {"state":"do_not_disturb"}
 *     Response: {"ok":true} or {"error":"bad request"}
 *     Effect:   updates door-sign state and refreshes the display.
 *
 *   POST /display or /custom
 *     Body:     {"text":"your message here","led_color":"#00ff80"}
 *               Use {"led_color":"off"} to clear the custom steady LED color.
 *     Response: {"ok":true}
 *     Effect:   updates the custom text / custom steady LED color.
 *
 *   POST /ota
 *     Body:     {}
 *     Response: {"ok":true,"reboot":"recovery"}
 *     Effect:   reboots into recovery, where the existing OTA path checks
 *               OTA_FIRMWARE_URL and installs the served app if needed.
 *
 * Requires WiFi to be connected before calling.
 */
esp_err_t badge_server_start(void);

/**
 * Stop the HTTP server and free its resources.
 */
void badge_server_stop(void);
