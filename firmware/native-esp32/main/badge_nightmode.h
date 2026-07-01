#pragma once

#include "esp_err.h"

/*
 * badge_nightmode — overnight doorbell disable with deep sleep.
 *
 * Syncs wall-clock time via SNTP at boot, then runs a background task that
 * watches for the 23:00 Berlin transition. At that point the e-paper is
 * updated to show "KLINGEL DEAKTIVIERT", all LEDs are turned off, and the
 * device enters deep sleep until 08:00 Berlin time the next morning.
 *
 * On wake the device boots normally and doorbell operation resumes.
 */

/**
 * Sync time via SNTP and start the night-mode watchdog task.
 *
 * Must be called after badge_state_init() and an initial display render so
 * the display task is already alive.  If the current time is already inside
 * the night window (23:00–08:00 Berlin) the device enters deep sleep
 * immediately inside this call and never returns.
 *
 * Non-fatal on SNTP failure: the watchdog task still runs and re-tries the
 * sync opportunistically.  Night mode will activate as soon as the clock is
 * known and the window is open.
 */
esp_err_t badge_nightmode_init(void);
