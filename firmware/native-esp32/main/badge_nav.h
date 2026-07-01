#pragma once

#include "esp_err.h"

/**
 * Callback fired when START is held continuously for BADGE_NAV_OTA_HOLD_MS.
 * Runs in the nav polling task — keep it brief or hand off to another task
 * (it's safe to block on WiFi here; the nav poll loop just pauses).
 */
typedef void (*badge_nav_start_hold_cb_t)(void);

/**
 * Probe the PCA9555 nav expander on the shared I2C bus, then start a
 * background task that polls port 0 every BADGE_NAV_POLL_MS and fires
 * on_start_hold when START stays low for BADGE_NAV_OTA_HOLD_MS.
 *
 * Returns ESP_OK if the expander was found and the task was started.
 * If no expander responds, the function returns the I2C error and no
 * task is created (firmware continues to run without nav handling).
 */
esp_err_t badge_nav_init(badge_nav_start_hold_cb_t on_start_hold);
