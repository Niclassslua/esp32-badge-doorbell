#pragma once

#include "esp_err.h"

/**
 * Configure the badge buttons and start their handler tasks.
 *
 * TR22 touch controller:
 *   On press while the sign says "Please ring": when configured, flashes the
 *   Home Assistant light defined by BADGE_HOME_ASSISTANT_*.
 *   When the sign says "Do not disturb", presses are ignored.
 *   Repeated presses are rate-limited by BADGE_RING_COOLDOWN_MS and ignored
 *   while a previous ring sequence is still running.
 *
 * Optional BADGE_BTN_STATUS_GPIO (active-low, internal pull-up):
 *   On press: toggles sign state (PLEASE_RING <-> DO_NOT_DISTURB) and refreshes
 *             the display.
 *
 * Debounce: 200 ms.
 */
esp_err_t badge_button_init(void);
