#pragma once

#include <stdbool.h>

#include "esp_err.h"

/**
 * Initialise the SPI bus and e-paper panel.  Must be called once before any
 * other tr19_epaper_* function.  Safe to call multiple times (idempotent).
 */
esp_err_t tr19_epaper_init(void);

/**
 * Render the door-sign layout and push it to the display.
 *
 * @param please_ring true  -> shows "BITTE KLINGELN"
 *                    false -> shows "NICHT STÖREN"
 * @param custom_text Optional line(s) of text shown below the separator.
 *                    Pass NULL or "" to leave blank.
 *                    Only uppercase A-Z, Ö, 0-9, space, '!' and '.' are
 *                    rendered; other characters are silently shown as space.
 */
esp_err_t tr19_epaper_show_sign(bool please_ring, const char *custom_text);

/**
 * Show the built-in "HELLO TR22 CUSTOM FW" splash screen (calls
 * tr19_epaper_init() internally if needed).
 */
esp_err_t tr19_epaper_show_hello(void);

/**
 * Show a minimal recovery-mode status screen.
 *
 * This does not read WiFi, battery, NVS, or badge state. Recovery firmware
 * calls it from a background task so display timeouts never block recovery.
 */
esp_err_t tr19_epaper_show_recovery(const char *line1, const char *line2);
